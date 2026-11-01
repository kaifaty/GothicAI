#if defined(TEMPEST_BUILD_METAL)

#include "mtdescriptorarray.h"

#include "mtdevice.h"
#include "mttexture.h"
#include "mtbuffer.h"
#include "mtpipelinelay.h"
#include "mtaccelerationstructure.h"
#include "mtshader.h"

#include <algorithm>
#include <functional>
#include <stdexcept>

using namespace Tempest;
using namespace Tempest::Detail;

MtDescriptorArray::MtDescriptorArray(MtDevice& dev, AbstractGraphicsApi::Texture** tex, size_t cnt,
                                     uint32_t mipLevel, const Sampler& smp)
  : cnt(cnt), mipLevel(mipLevel), textureArray(true) {
  fillBuffer(dev, tex, cnt, mipLevel, &smp);
  }

MtDescriptorArray::MtDescriptorArray(MtDevice& dev, AbstractGraphicsApi::Texture** tex, size_t cnt, uint32_t mipLevel)
  : cnt(cnt), mipLevel(mipLevel), textureArray(true) {
  fillBuffer(dev, tex, cnt, mipLevel, nullptr);
  }

MtDescriptorArray::MtDescriptorArray(MtDevice& dev, AbstractGraphicsApi::Buffer** buf, size_t cnt)
  : cnt(cnt) {
  fillBuffer(dev, buf, cnt);
  }

MtDescriptorArray::~MtDescriptorArray() {
  }

size_t MtDescriptorArray::size() const {
  return cnt;
  }

void MtDescriptorArray::fillBuffer(MtDevice& dev, AbstractGraphicsApi::Texture** tex, size_t cnt,
                                   uint32_t mipLevel, const Sampler* smp) {
  const bool combined = false;

  size_t shift = 0;
  size_t bufSz = cnt*sizeof(MTL::ResourceID);
  if(combined) {
    bufSz  = ((bufSz+16-1)/16)*16;
    shift  = bufSz;
    bufSz += cnt*sizeof(MTL::ResourceID);
    }
  argsBuf = MtBuffer(dev, nullptr, bufSz, MTL::ResourceStorageModeShared);

  std::unique_ptr<uint8_t[]> addr(new uint8_t[bufSz]);
  args.reserve(cnt);
  args.clear();
  indexedArgs.resize(cnt);
  references.reserve(cnt);
  for(size_t i=0; i<cnt; ++i) {
    auto* ptr = reinterpret_cast<MTL::ResourceID*>(addr.get());
    if(tex[i]==nullptr) {
      ptr[i] = MTL::ResourceID{0};
      continue;
      }
    const auto mapping = ComponentMapping();
    auto& t    = *reinterpret_cast<MtTexture*>(tex[i]);
    auto& view = t.view(mapping,mipLevel);
    ptr[i]     = view.gpuResourceID();
    auto* resource=t.impl.get();
    indexedArgs[i]=resource;
    const auto entry=references.emplace(resource,ResourceReference{0,args.size()});
    ++entry.first->second.count;
    if(entry.second)
      args.push_back(resource);
    }

  if(combined) {
    auto& sampler = dev.samplers.get(*smp,true);
    auto* ptr     = reinterpret_cast<MTL::ResourceID*>(addr.get() + shift);
    for(size_t i=0; i<cnt; ++i) {
      ptr[i] = sampler.gpuResourceID();
      }
    }

  argsBuf.update(addr.get(), 0, bufSz);
  }

void MtDescriptorArray::setTexture(size_t index, AbstractGraphicsApi::Texture* texture) {
  if(!textureArray)
    throw std::system_error(GraphicsErrc::UnsupportedExtension,"texture update of a buffer descriptor array");
  if(index>=cnt)
    throw std::out_of_range("descriptor array index");
  auto* native=reinterpret_cast<MtTexture*>(texture);
  const auto id=native!=nullptr ? native->view(ComponentMapping(),mipLevel).gpuResourceID() : MTL::ResourceID{0};
  MTL::Resource* resource=native!=nullptr ? native->impl.get() : nullptr;
  auto* previous=indexedArgs[index];
  if(previous!=resource) {
    if(previous!=nullptr) {
      auto found=references.find(previous);
      if(--found->second.count==0) {
        const auto at=found->second.index;
        args[at]=args.back();
        references.find(args[at])->second.index=at;
        args.pop_back();
        references.erase(found);
        }
      }
    if(resource!=nullptr) {
      const auto entry=references.emplace(resource,ResourceReference{0,args.size()});
      ++entry.first->second.count;
      if(entry.second)
        args.push_back(resource);
      }
    indexedArgs[index]=resource;
    }
  argsBuf.update(&id,index*sizeof(id),sizeof(id));
  }

void MtDescriptorArray::fillBuffer(MtDevice& dev, AbstractGraphicsApi::Buffer** buf, size_t cnt) {
  struct spvBufferDescriptor {
    uint64_t ptr;
    uint32_t len;
    // 4 bytes of padding
    };
  argsBuf = MtBuffer(dev, nullptr, cnt*sizeof(spvBufferDescriptor), MTL::ResourceStorageModePrivate);

  std::unique_ptr<spvBufferDescriptor[]> addr(new spvBufferDescriptor[cnt]);
  args.reserve(cnt);
  args.clear();
  for(size_t i=0; i<cnt; ++i) {
    if(buf[i]==nullptr) {
      addr[i].ptr = 0;
      addr[i].len = 0;
      continue;
      }
    addr[i].ptr = reinterpret_cast<MtBuffer*>(buf[i])->impl->gpuAddress();
    addr[i].len = uint32_t(reinterpret_cast<MtBuffer*>(buf[i])->size);
    args.push_back(reinterpret_cast<MtBuffer*>(buf[i])->impl.get());
    }
  std::sort(args.begin(),args.end(),std::less<MTL::Resource*>());
  args.erase(std::unique(args.begin(),args.end()),args.end());
  argsBuf.update(addr.get(), 0, cnt*sizeof(spvBufferDescriptor));
  }

void MtDescriptorArray::useResource(MTL::ComputeCommandEncoder& cmd, ShaderReflection::Stage st) const {
  implUseResource(cmd, st);
  }

void MtDescriptorArray::useResource(MTL::RenderCommandEncoder& cmd, ShaderReflection::Stage st) const {
  implUseResource(cmd, st);
  }

template<class Enc>
void MtDescriptorArray::implUseResource(Enc& cmd, ShaderReflection::Stage st) const {
  auto stages = nativeFormat(st);
  //TODO: UsageWrite
  implUseResource(cmd, args.data(), args.size(), MTL::ResourceUsageRead | MTL::ResourceUsageSample, stages);
  }

void MtDescriptorArray::implUseResource(MTL::ComputeCommandEncoder& cmd,
                                        const MTL::Resource* const resources[], NS::UInteger count,
                                        MTL::ResourceUsage usage, MTL::RenderStages stages) const {
  (void)stages;
  cmd.useResources(resources, count, usage);
  }

void MtDescriptorArray::implUseResource(MTL::RenderCommandEncoder& cmd,
                                        const MTL::Resource* const resources[], NS::UInteger count,
                                        MTL::ResourceUsage usage, MTL::RenderStages stages) const {
  cmd.useResources(resources, count, usage, stages);
  }

#endif

