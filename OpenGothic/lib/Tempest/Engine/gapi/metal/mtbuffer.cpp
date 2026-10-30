#if defined(TEMPEST_BUILD_METAL)

#include "mtbuffer.h"
#include "mtdevice.h"

#include "utility/compiller_hints.h"

#if defined(OPENGOTHIC_ENABLE_FRAME_PROFILE) && OPENGOTHIC_ENABLE_FRAME_PROFILE
#include "gapi/metalapi.h"
#endif

using namespace Tempest::Detail;

MtBuffer::MtBuffer() {
  }

MtBuffer::MtBuffer(MtDevice& dev, const void* data, size_t size, MTL::ResourceOptions f)
  :dev(&dev), size(size) {
  const size_t roundSize = ((size+64-1)/64)*64; // for uniforms/ssbo structures in msl

  const MTL::ResourceOptions flg = f | MTL::HazardTrackingModeDefault;
  if(data==nullptr) {
    impl = NsPtr<MTL::Buffer>(dev.impl->newBuffer(roundSize,flg));
    if(impl==nullptr)
      throw std::system_error(GraphicsErrc::OutOfVideoMemory);
    probeMetalUploadBuffer(impl.get(),false);
    return;
    }

  if(roundSize==size && 0==(flg & MTL::ResourceStorageModePrivate)) {
    impl = NsPtr<MTL::Buffer>(dev.impl->newBuffer(data,roundSize,flg));
    if(impl==nullptr)
      throw std::system_error(GraphicsErrc::OutOfVideoMemory);
    probeMetalUploadBuffer(impl.get(),false);
    return;
    }

  impl = NsPtr<MTL::Buffer>(dev.impl->newBuffer(roundSize,flg));
  if(impl==nullptr)
    throw std::system_error(GraphicsErrc::OutOfVideoMemory);
  probeMetalUploadBuffer(impl.get(),false);
  if(impl->storageMode()==MTL::StorageModePrivate)
    upload(data,0,size,true); else
    implUpdate(data,0,size);
  }

MtBuffer::~MtBuffer() {
  }

MtBuffer& MtBuffer::operator =(MtBuffer&& other) {
  assert(other.counter.load()==0); // internal use only
  assert(this->counter.load()==0);
  std::swap(dev,  other.dev);
  std::swap(size, other.size);
  std::swap(impl, other.impl);
  return *this;
  }

void MtBuffer::update(const void *data, size_t off, size_t size) {
  if(T_LIKELY(impl->storageMode()!=MTL::StorageModePrivate)) {
    implUpdate(data,off,size);
    return;
    }
  upload(data,off,size,true);
  }

void MtBuffer::updateAsync(const void* data, size_t off, size_t size) {
  if(T_LIKELY(impl->storageMode()!=MTL::StorageModePrivate)) {
    implUpdate(data,off,size);
    return;
    }
  upload(data,off,size,false);
  }

void MtBuffer::upload(const void* data, size_t off, size_t size, bool wait) {
  auto pool = NsPtr<NS::AutoreleasePool>::init();
  MTL::ResourceOptions opt   = MTL::ResourceStorageModeShared | MTL::ResourceCPUCacheModeWriteCombined;
  auto                 stage = NsPtr<MTL::Buffer>(dev->impl->newBuffer(size,opt));
  if(stage==nullptr)
    throw std::system_error(GraphicsErrc::OutOfVideoMemory);
  probeMetalUploadBuffer(stage.get(),true);
  std::memcpy(stage->contents(), data, size);

  // Retained references keep both buffers alive after the staging owner exits.
  // Consuming draws commit later on this same queue and see the completed copy.
  auto cmd = dev->queue->commandBuffer();
  auto enc = cmd->blitCommandEncoder();
  enc->copyFromBuffer(stage.get(),0, impl.get(),off, size);
  enc->endEncoding();
#if defined(OPENGOTHIC_ENABLE_FRAME_PROFILE) && OPENGOTHIC_ENABLE_FRAME_PROFILE
  profileGpu(cmd);
#endif
  if(!wait) {
    auto* device = dev;
    device->onSubmit();
    cmd->addCompletedHandler(^(MTL::CommandBuffer*) { device->onFinish(); });
    }
  cmd->commit();
  if(wait)
    cmd->waitUntilCompleted();
  }

void MtBuffer::read(void *data, size_t off, size_t size) {
  auto pool = NsPtr<NS::AutoreleasePool>::init();

  MTL::ResourceOptions opt   = MTL::ResourceStorageModeShared | MTL::ResourceCPUCacheModeDefaultCache |
                               MTL::ResourceHazardTrackingModeTracked;
  auto                 stage = NsPtr<MTL::Buffer>(dev->impl->newBuffer(size,opt));
  probeMetalUploadBuffer(stage.get(),true);
  auto cmd = dev->queue->commandBuffer();
  auto enc = cmd->blitCommandEncoder();
  enc->copyFromBuffer(impl.get(),off,stage.get(),0, size);
  enc->endEncoding();
#if defined(OPENGOTHIC_ENABLE_FRAME_PROFILE) && OPENGOTHIC_ENABLE_FRAME_PROFILE
  profileGpu(cmd);
#endif
  cmd->commit();

  cmd->waitUntilCompleted();
  std::memcpy(data,stage->contents(),size);
  }

void MtBuffer::implUpdate(const void *data, size_t off, size_t size) {
  auto ptr = reinterpret_cast<uint8_t*>(impl->contents());

  std::memcpy(ptr+off, data, size);
  if(impl->storageMode()!=MTL::StorageModeManaged)
    return;

  impl->didModifyRange(NS::Range(off,size));
  }

#endif
