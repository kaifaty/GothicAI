#if defined(TEMPEST_BUILD_METAL)

#include "mtaccelerationstructure.h"

#include "mtdevice.h"
#include "mtbuffer.h"

#include "utility/compiller_hints.h"

#include <Tempest/Log>
#include <algorithm>
#include <cassert>
#include <cstring>

#if defined(OPENGOTHIC_ENABLE_FRAME_PROFILE) && OPENGOTHIC_ENABLE_FRAME_PROFILE
#include "gapi/metalapi.h"
#endif

using namespace Tempest::Detail;

// A build of its own command buffer on the frame queue, without waiting: the frames committed after it see the
// structure (Metal keeps the order of a queue for tracked resources), and the command buffer retains what it reads.
static void submitBuild(MtDevice& dx, MTL::CommandBuffer& cmd, const char* what) {
  cmd.addCompletedHandler(^(MTL::CommandBuffer* completed) {
    if(completed->status()!=MTL::CommandBufferStatusCompleted)
      Tempest::Log::e("metal: ", what, " build failed, status ", int(completed->status()));
    });
#if defined(OPENGOTHIC_ENABLE_FRAME_PROFILE) && OPENGOTHIC_ENABLE_FRAME_PROFILE
  profileGpu(&cmd);
#endif
  auto guard = dx.lockUpload(0);
  dx.submitUpload(cmd, 0);
  }

void MtBlasBuildCtx::pushGeometry(MtDevice& dx,
                                  const MtBuffer& vbo, size_t vboSz, size_t stride,
                                  const MtBuffer& ibo, size_t iboSz, size_t ioffset, IndexClass icls) {
  auto geo = NsPtr<MTL::AccelerationStructureTriangleGeometryDescriptor>::init();
  if(geo==nullptr)
    throw std::system_error(GraphicsErrc::OutOfHostMemory);

  geo->setVertexBuffer(vbo.impl.get());
  geo->setVertexBufferOffset(0);
  geo->setVertexStride(stride);
  geo->setVertexFormat(MTL::AttributeFormatFloat3);
  geo->setIndexBuffer(ibo.impl.get());
  geo->setIndexBufferOffset(ioffset*sizeofIndex(icls));
  geo->setIndexType(nativeFormat(icls));
  geo->setTriangleCount(iboSz/3);
  geo->setOpaque(true);
  geo->setAllowDuplicateIntersectionFunctionInvocation(true);

  if(geo->indexBufferOffset()%32!=0) {
    Log::d("FIXME: index buffer offset alignment on metal(",geo->indexBufferOffset()%32,")");
    geo->setIndexBufferOffset(0);
    geo->setTriangleCount(0);
    }
  ranges.emplace_back(std::move(geo));
  this->geo.push_back(ranges.back().get());
  }

void MtBlasBuildCtx::bake() {
  auto geoArr = NsPtr<NS::Array>(NS::Array::array(geo.data(), geo.size()));
  desc = NsPtr<MTL::PrimitiveAccelerationStructureDescriptor>::init();
  desc->retain();
  desc->setGeometryDescriptors(geoArr.get());
  desc->setUsage(MTL::AccelerationStructureUsageNone);
  }

MTL::AccelerationStructureSizes MtBlasBuildCtx::buildSizes(MtDevice& dx) const {
  return dx.impl->accelerationStructureSizes(desc.get());
  }


MtAccelerationStructure::MtAccelerationStructure(MtDevice& dx, const Tempest::AbstractGraphicsApi::RtGeometry* geom, size_t size)
  :owner(dx) {
  auto pool = NsPtr<NS::AutoreleasePool>::init();
  MtBlasBuildCtx ctx;
  for(size_t i=0; i<size; ++i) {
    auto& vbo     = *reinterpret_cast<const MtBuffer*>(geom[i].vbo);
    auto  vboSz   = geom[i].vboSz;
    auto  stride  = geom[i].stride;
    auto& ibo     = *reinterpret_cast<const MtBuffer*>(geom[i].ibo);
    auto  iboSz   = geom[i].iboSz;
    auto  ioffset = geom[i].ioffset;
    auto  icls    = geom[i].icls;
    ctx.pushGeometry(dx, vbo, vboSz, stride, ibo, iboSz, ioffset, icls);
    }
  ctx.bake();

  auto sz = ctx.buildSizes(dx);

  impl = NsPtr<MTL::AccelerationStructure>(dx.impl->newAccelerationStructure(sz.accelerationStructureSize));
  if(impl==nullptr)
    throw std::system_error(GraphicsErrc::OutOfVideoMemory);

  NsPtr<MTL::Buffer> scratch = NsPtr<MTL::Buffer>(dx.impl->newBuffer(sz.buildScratchBufferSize, MTL::ResourceStorageModePrivate));
  if(scratch==nil && sz.buildScratchBufferSize>0)
    throw std::system_error(GraphicsErrc::OutOfVideoMemory);

  auto cmd = dx.queue->commandBuffer();
  auto enc = cmd->accelerationStructureCommandEncoder();
  enc->buildAccelerationStructure(impl.get(), ctx.desc.get(), scratch.get(), 0);
  enc->endEncoding();
  submitBuild(dx, *cmd, "blas");
  }

MtAccelerationStructure::~MtAccelerationStructure() {
  }


static void writeInstance(MTL::AccelerationStructureUserIDInstanceDescriptor& obj, const Tempest::RtInstance& inst, uint32_t index) {
  for(int x=0; x<4; ++x)
    for(int y=0; y<3; ++y)
      obj.transformationMatrix[x][y]  = inst.mat.at(x,y);
  obj.options                         = nativeFormat(inst.flags);
  obj.mask                            = inst.mask;
  obj.intersectionFunctionTableOffset = 0;
  obj.accelerationStructureIndex      = index;
  obj.userID                          = inst.id;
  }

MtTopAccelerationStructure::MtTopAccelerationStructure(MtDevice& dx, const RtInstance* inst,
                                                       AccelerationStructure*const* as, size_t asSize)
  :owner(dx) {
  auto pool = NsPtr<NS::AutoreleasePool>::init();
  instances = NsPtr<MTL::Buffer>(dx.impl->newBuffer(sizeof(MTL::AccelerationStructureUserIDInstanceDescriptor)*std::max<size_t>(asSize,1),
                                                    hostVisibleResourceOptions(*dx.impl)));
  if(instances==nullptr)
    throw std::system_error(GraphicsErrc::OutOfVideoMemory);

  auto*  dst   = reinterpret_cast<MTL::AccelerationStructureUserIDInstanceDescriptor*>(instances->contents());
  size_t count = 0;
  for(size_t i=0; i<asSize; ++i) {
    if(as[i]==nullptr)
      continue;
    writeInstance(dst[count], inst[i], acquire(reinterpret_cast<MtAccelerationStructure*>(as[i])->impl.get()));
    ++count;
    }
  if(instances->storageMode()==MTL::StorageModeManaged)
    instances->didModifyRange(NS::Range(0,instances->length()));

  auto desc = buildDesc(instances.get(), count);
  auto sz   = dx.impl->accelerationStructureSizes(desc.get());
  impl = NsPtr<MTL::AccelerationStructure>(dx.impl->newAccelerationStructure(sz.accelerationStructureSize));
  if(impl==nullptr)
    throw std::system_error(GraphicsErrc::OutOfVideoMemory);

  auto scratch = NsPtr<MTL::Buffer>(dx.impl->newBuffer(std::max<size_t>(sz.buildScratchBufferSize,1),MTL::ResourceStorageModePrivate));
  if(scratch==nullptr)
    throw std::system_error(GraphicsErrc::OutOfVideoMemory);

  auto cmd = dx.queue->commandBuffer();
  auto enc = cmd->accelerationStructureCommandEncoder();
  enc->buildAccelerationStructure(impl.get(), desc.get(), scratch.get(), 0);
  enc->endEncoding();
  submitBuild(dx, *cmd, "tlas");
  }

MtTopAccelerationStructure::MtTopAccelerationStructure(MtDevice& dx, size_t cap)
  :owner(dx), capacity(cap) {
  auto pool = NsPtr<NS::AutoreleasePool>::init();
  {
  // The placeholder: a BLAS of one degenerate triangle, index 0 of the list. The builds that read it come later on
  // this queue (tracked resources). Its vertex and index buffers are members: the build reads them through the
  // geometry descriptor, which the command buffer is not documented to retain. The scratch is an argument of the
  // encoder, which the command buffer retains.
  static const float    vert[9] = {};
  static const uint32_t ind [3] = {};
  placeholderVbo = NsPtr<MTL::Buffer>(dx.impl->newBuffer(vert, sizeof(vert), hostVisibleResourceOptions(*dx.impl)));
  placeholderIbo = NsPtr<MTL::Buffer>(dx.impl->newBuffer(ind,  sizeof(ind),  hostVisibleResourceOptions(*dx.impl)));
  auto geo = NsPtr<MTL::AccelerationStructureTriangleGeometryDescriptor>::init();
  auto pd  = NsPtr<MTL::PrimitiveAccelerationStructureDescriptor>::init();
  if(placeholderVbo==nullptr || placeholderIbo==nullptr || geo==nullptr || pd==nullptr)
    throw std::system_error(GraphicsErrc::OutOfVideoMemory);
  geo->setVertexBuffer(placeholderVbo.get());
  geo->setVertexStride(sizeof(float)*3);
  geo->setVertexFormat(MTL::AttributeFormatFloat3);
  geo->setIndexBuffer(placeholderIbo.get());
  geo->setIndexType(MTL::IndexTypeUInt32);
  geo->setTriangleCount(1);
  geo->setOpaque(true);
  const NS::Object* geoArr[] = {geo.get()};
  pd->setGeometryDescriptors(NS::Array::array(geoArr, 1));
  auto psz = dx.impl->accelerationStructureSizes(pd.get());
  placeholder = NsPtr<MTL::AccelerationStructure>(dx.impl->newAccelerationStructure(psz.accelerationStructureSize));
  auto pscratch = NsPtr<MTL::Buffer>(dx.impl->newBuffer(std::max<size_t>(psz.buildScratchBufferSize,1),MTL::ResourceStorageModePrivate));
  if(placeholder==nullptr || pscratch==nullptr)
    throw std::system_error(GraphicsErrc::OutOfVideoMemory);
  auto cmd = dx.queue->commandBuffer();
  auto enc = cmd->accelerationStructureCommandEncoder();
  enc->buildAccelerationStructure(placeholder.get(), pd.get(), pscratch.get(), 0);
  enc->endEncoding();
  submitBuild(dx, *cmd, "placeholder blas");
  blas.push_back(placeholder.get());
  blasRefs.push_back(0);
  }

  auto desc = buildDesc(nullptr, cap);
  auto sz   = dx.impl->accelerationStructureSizes(desc.get());
  impl = NsPtr<MTL::AccelerationStructure>(dx.impl->newAccelerationStructure(sz.accelerationStructureSize));
  if(impl==nullptr)
    throw std::system_error(GraphicsErrc::OutOfVideoMemory);
  scratch = NsPtr<MTL::Buffer>(dx.impl->newBuffer(std::max<size_t>(sz.buildScratchBufferSize,1),MTL::ResourceStorageModePrivate));
  if(scratch==nullptr)
    throw std::system_error(GraphicsErrc::OutOfVideoMemory);
  }

void MtTopAccelerationStructure::pack(const RtInstance& inst, AccelerationStructure* as, void* slot) {
  assert(placeholder!=nullptr); // second constructor only
  MTL::AccelerationStructureUserIDInstanceDescriptor obj = {};
  std::memcpy(&obj, slot, sizeof(obj));
  const uint32_t prev = obj.accelerationStructureIndex;
  // acquire first: an instance that keeps its BLAS keeps the index
  const uint32_t next = as!=nullptr ? acquire(reinterpret_cast<MtAccelerationStructure*>(as)->impl.get()) : 0;
  release(prev);
  obj = {};
  if(as!=nullptr)
    writeInstance(obj, inst, next); // else zeros: mask 0, the placeholder
  std::memcpy(slot, &obj, sizeof(obj));
  }

uint32_t MtTopAccelerationStructure::acquire(MTL::Resource* ax) {
  auto it = blasIndex.find(ax);
  if(it!=blasIndex.end()) {
    ++blasRefs[it->second];
    return it->second;
    }
  uint32_t index = 0;
  if(!blasFree.empty()) {
    index = blasFree.back();
    blasFree.pop_back();
    blas    [index] = ax;
    blasRefs[index] = 1;
    } else {
    index = uint32_t(blas.size());
    blas    .push_back(ax);
    blasRefs.push_back(1);
    }
  ax->retain();
  blasIndex[ax] = index;
  return index;
  }

void MtTopAccelerationStructure::release(uint32_t index) {
  if(index>=blas.size() || blasRefs[index]==0)
    return; // the placeholder
  if(--blasRefs[index]>0)
    return;
  blasIndex.erase(blas[index]);
  blas[index]->release();
  blas[index] = placeholder.get();
  blasFree.push_back(index);
  }

NsPtr<MTL::InstanceAccelerationStructureDescriptor> MtTopAccelerationStructure::buildDesc(MTL::Buffer* ibo, size_t count) const {
  // autoreleased: the descriptor retains it
  auto* asArray = NS::Array::array(reinterpret_cast<NS::Object* const*>(blas.data()), blas.size());
  auto  desc    = NsPtr<MTL::InstanceAccelerationStructureDescriptor>::init();
  if(desc==nullptr || asArray==nullptr)
    throw std::system_error(GraphicsErrc::OutOfHostMemory);
  desc->setInstanceDescriptorBuffer(ibo);
  desc->setInstanceDescriptorBufferOffset(0);
  desc->setInstanceDescriptorStride(sizeof(MTL::AccelerationStructureUserIDInstanceDescriptor));
  desc->setInstanceCount(count);
  desc->setInstancedAccelerationStructures(asArray);
  desc->setInstanceDescriptorType(MTL::AccelerationStructureInstanceDescriptorTypeUserID);

  if(owner.prop.transposedRtMatrix && false) {
    //TODO: vulkan/dx-like matrices
    desc->setInstanceTransformationMatrixLayout(MTL::MatrixLayoutRowMajor);
    }
  return desc;
  }

MtTopAccelerationStructure::~MtTopAccelerationStructure() {
  for(size_t i=0; i<blas.size(); ++i)
    if(blasRefs[i]>0)
      blas[i]->release();
  }

void MtTopAccelerationStructure::useResource(MTL::RenderCommandEncoder& cmd, ShaderReflection::Stage st) const {
  implUseResource(cmd, st);
  }

void MtTopAccelerationStructure::useResource(MTL::ComputeCommandEncoder& cmd, ShaderReflection::Stage st) const {
  implUseResource(cmd, st);
  }

template<class Enc>
void MtTopAccelerationStructure::implUseResource(Enc& cmd, ShaderReflection::Stage st) const {
  auto  stages = nativeFormat(st);
  implUseResource(cmd, blas.data(), blas.size(), MTL::ResourceUsageRead, stages);
  }

void MtTopAccelerationStructure::implUseResource(MTL::ComputeCommandEncoder& cmd,
                                                 const MTL::Resource* const resources[], NS::UInteger count,
                                                 MTL::ResourceUsage usage, MTL::RenderStages stages) const {
  (void)stages;
  cmd.useResources(resources, count, usage);
  }

void MtTopAccelerationStructure::implUseResource(MTL::RenderCommandEncoder& cmd,
                                                 const MTL::Resource* const resources[], NS::UInteger count,
                                                 MTL::ResourceUsage usage, MTL::RenderStages stages) const {
  cmd.useResources(resources, count, usage, stages);
  }

#endif
