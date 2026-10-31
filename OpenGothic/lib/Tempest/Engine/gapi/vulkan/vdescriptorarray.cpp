#if defined(TEMPEST_BUILD_VULKAN)
#include "vbuffer.h"

#include "vdevice.h"
#include "vdescriptorarray.h"
#include "vtexture.h"
#include "vaccelerationstructure.h"

#include <stdexcept>

using namespace Tempest;
using namespace Tempest::Detail;

VDescriptorArray::VDescriptorArray(VDevice &dev, AbstractGraphicsApi::Texture **tex, size_t cnt, uint32_t mipLevel, const Sampler &smp)
  : dev(dev), cnt(cnt), sampler(dev.samplers.get(smp)), mipLevel(mipLevel), textureArray(true) {
  // Roundup a little, to avoid spamming of layouts
  const uint32_t cntRound = dev.roundUpDescriptorCount(ShaderReflection::Texture, cnt);

  const auto lay = dev.bindlessArrayLayout(ShaderReflection::Texture, cntRound);
  alloc(lay, dev, ShaderReflection::Texture, cntRound);
  populate(dev, tex, cnt, mipLevel, &smp);
  }

VDescriptorArray::VDescriptorArray(VDevice &dev, AbstractGraphicsApi::Texture **tex, size_t cnt, uint32_t mipLevel)
  : dev(dev), cnt(cnt), mipLevel(mipLevel), textureArray(true) {
  // Roundup a little, to avoid spamming of layouts
  const uint32_t cntRound = dev.roundUpDescriptorCount(ShaderReflection::Image, cnt);

  const auto lay = dev.bindlessArrayLayout(ShaderReflection::Image, cntRound);
  alloc(lay, dev, ShaderReflection::Image, cntRound);
  populate(dev, tex, cnt, mipLevel, nullptr);
  }

VDescriptorArray::VDescriptorArray(VDevice &dev, AbstractGraphicsApi::Buffer **buf, size_t cnt)
  : dev(dev), cnt(cnt) {
  // Roundup a little, to avoid spamming of layouts
  const uint32_t cntRound = dev.roundUpDescriptorCount(ShaderReflection::SsboRW, cnt);

  const auto lay = dev.bindlessArrayLayout(ShaderReflection::SsboRW, cntRound);
  alloc(lay, dev, ShaderReflection::SsboRW, cntRound);
  populate(dev, buf, cnt);
  }

VDescriptorArray::~VDescriptorArray() {
  dev.descPool.notifyDestroy(this);
  if(pool!=VK_NULL_HANDLE)
    vkDestroyDescriptorPool(dev.device.impl, pool, nullptr);
  }

size_t VDescriptorArray::size() const {
  return cnt;
  }

void VDescriptorArray::setTexture(size_t index, AbstractGraphicsApi::Texture* texture) {
  if(!textureArray)
    throw std::system_error(GraphicsErrc::UnsupportedExtension,"texture update of a buffer descriptor array");
  if(index>=cnt)
    throw std::out_of_range("descriptor array index");
  auto* native=reinterpret_cast<VTexture*>(texture);
  VkDescriptorImageInfo image{};
  image.imageLayout=native!=nullptr ? native->defaultLayout() : VK_IMAGE_LAYOUT_UNDEFINED;
  image.imageView=native!=nullptr ? native->view(ComponentMapping(),mipLevel,false,false) : VK_NULL_HANDLE;
  image.sampler=sampler;
  assert(native==nullptr || native->nonUniqId==0);
  VkWriteDescriptorSet write{};
  write.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  write.dstSet=dset;
  write.dstBinding=0;
  write.dstArrayElement=uint32_t(index);
  write.descriptorType=sampler!=VK_NULL_HANDLE ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  write.descriptorCount=1;
  write.pImageInfo=&image;
  vkUpdateDescriptorSets(dev.device.impl,1,&write,0,nullptr);
  }

void VDescriptorArray::alloc(VkDescriptorSetLayout lay, VDevice &dev, ShaderReflection::Class cls, size_t cnt) {
  VkDescriptorPoolSize       poolSize = {};
  poolSize.type            = nativeFormat(cls);
  poolSize.descriptorCount = uint32_t(cnt);

  VkDescriptorPoolCreateInfo poolInfo = {};
  poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  poolInfo.maxSets       = 1;
  poolInfo.flags         = 0; // VK_DESCRIPTOR_POOL_CREATE_HOST_ONLY_BIT_EXT;
  poolInfo.poolSizeCount = 1;
  poolInfo.pPoolSizes    = &poolSize;
  poolInfo.flags         = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
  vkAssert(vkCreateDescriptorPool(dev.device.impl, &poolInfo, nullptr, &pool));

  VkDescriptorSetAllocateInfo allocInfo = {};
  allocInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  //allocInfo.pNext              = &vinfo;
  allocInfo.descriptorPool     = pool;
  allocInfo.descriptorSetCount = 1;
  allocInfo.pSetLayouts        = &lay;
  vkAssert(vkAllocateDescriptorSets(dev.device.impl,&allocInfo,&dset));
  }

void VDescriptorArray::populate(VDevice &dev, AbstractGraphicsApi::Texture **t, size_t cnt, uint32_t mipLevel, const Sampler* smp) {
  const bool is3DImage = false; //TODO
  nonUniqId = NonUniqResId::I_None;

  // 16 is non-bindless limit
  SmallArray<VkDescriptorImageInfo,16> imageInfo(cnt);
  for(size_t i=0; i<cnt; ++i) {
    VTexture* tex = reinterpret_cast<VTexture*>(t[i]);
    imageInfo[i].imageLayout = tex!=nullptr ? tex->defaultLayout() : VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo[i].imageView   = tex!=nullptr ? tex->view(ComponentMapping(), mipLevel, is3DImage, false) : VK_NULL_HANDLE;
    imageInfo[i].sampler     = smp!=nullptr ? dev.samplers.get(*smp) : VK_NULL_HANDLE;
    // TODO: support mutable textures in bindless
    assert(tex==nullptr || tex->nonUniqId==0);
    if(tex!=nullptr)
      nonUniqId |= tex->nonUniqId;
    }

  VkWriteDescriptorSet descriptorWrite = {};
  descriptorWrite.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  descriptorWrite.dstSet          = dset;
  descriptorWrite.dstBinding      = 0;
  descriptorWrite.dstArrayElement = 0;
  descriptorWrite.descriptorType  = smp!=nullptr ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  descriptorWrite.descriptorCount = uint32_t(cnt);
  descriptorWrite.pImageInfo      = imageInfo.get();

  vkUpdateDescriptorSets(dev.device.impl, 1, &descriptorWrite, 0, nullptr);
  }

void VDescriptorArray::populate(VDevice &dev, AbstractGraphicsApi::Buffer **b, size_t cnt) {
  nonUniqId = NonUniqResId::I_None;
  // 16 is non-bindless limit
  SmallArray<VkDescriptorBufferInfo,16> bufInfo(cnt);
  for(size_t i=0; i<cnt; ++i) {
    VBuffer* buf = reinterpret_cast<VBuffer*>(b[i]);
    bufInfo[i].buffer = buf!=nullptr ? buf->impl : VK_NULL_HANDLE;
    bufInfo[i].offset = 0;
    bufInfo[i].range  = VK_WHOLE_SIZE;
    if(!dev.props.hasRobustness2 && buf==nullptr) {
      bufInfo[i].buffer = dev.dummySsbo().impl;
      bufInfo[i].offset = 0;
      bufInfo[i].range  = 0;
      }
    // assert(buf->nonUniqId==0);
    if(buf!=nullptr)
      nonUniqId |= buf->nonUniqId;
    }

  VkWriteDescriptorSet descriptorWrite = {};
  descriptorWrite.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  descriptorWrite.dstSet          = dset;
  descriptorWrite.dstBinding      = uint32_t(0);
  descriptorWrite.dstArrayElement = 0;
  descriptorWrite.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  descriptorWrite.descriptorCount = uint32_t(cnt);
  descriptorWrite.pBufferInfo     = bufInfo.get();

  vkUpdateDescriptorSets(dev.device.impl, 1, &descriptorWrite, 0, nullptr);
  }


VDescriptorHeapArray::VDescriptorHeapArray(VDevice& dev, AbstractGraphicsApi::Texture** tex, size_t cnt, uint32_t mipLevel)
  : VDescriptorHeapArray(dev, tex, cnt, mipLevel, nullptr){
  }

VDescriptorHeapArray::VDescriptorHeapArray(VDevice& dev, AbstractGraphicsApi::Texture** tex, size_t cnt, uint32_t mipLevel, const Sampler* sampler)
  : dev(dev), cnt(uint32_t(cnt)), mipLevel(mipLevel), textureArray(true) {
  //NOTE: no bindless storage image
  try {
    auto alloc = dev.descAlloc.alloc(tex, cnt, mipLevel);
    dPtrR = alloc.ptr;

    if(sampler!=nullptr) {
      dPtrS = dev.samplers.getH(*sampler);
      }

    nonUniqId = NonUniqResId::I_None;
    for(size_t i=0; i<cnt; ++i) {
      auto* bx = reinterpret_cast<VTexture*>(tex[i]);
      if(bx!=nullptr)
        nonUniqId |= bx->nonUniqId;
      }
    }
  catch(...) {
    clear();
    throw;
    }
  }

VDescriptorHeapArray::VDescriptorHeapArray(VDevice& dev, AbstractGraphicsApi::Buffer** buf, size_t cnt)
  : dev(dev), cnt(uint32_t(cnt)) {
  try {
    auto alloc = dev.descAlloc.alloc(buf, cnt);
    dPtrR = alloc.ptr;

    nonUniqId = NonUniqResId::I_None;
    for(size_t i=0; i<cnt; ++i) {
      auto* bx = reinterpret_cast<VBuffer*>(buf[i]);
      if(bx!=nullptr)
        nonUniqId |= bx->nonUniqId;
      }
    }
  catch(...) {
    clear();
    throw;
    }
  }

VDescriptorHeapArray::~VDescriptorHeapArray() {
  clear();
  }

void VDescriptorHeapArray::setTexture(size_t index, AbstractGraphicsApi::Texture* texture) {
  if(!textureArray)
    throw std::system_error(GraphicsErrc::UnsupportedExtension,"texture update of a buffer descriptor array");
  if(index>=cnt)
    throw std::out_of_range("descriptor array index");
  if(texture==nullptr)
    throw std::system_error(GraphicsErrc::UnsupportedExtension,"null texture update of a descriptor heap array");
  dev.descAlloc.set(dPtrR+uint32_t(index),texture,mipLevel);
  }

void VDescriptorHeapArray::clear() {
  dev.descAlloc.free(dPtrR, uint32_t(cnt));
  }

#endif
