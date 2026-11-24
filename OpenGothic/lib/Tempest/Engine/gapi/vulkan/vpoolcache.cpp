#if defined(TEMPEST_BUILD_VULKAN)

#include "vpoolcache.h"
#include "vdevice.h"
#include "vdescriptorarray.h"

#include <algorithm>
#include <bit>
#include <stdexcept>

using namespace Tempest;
using namespace Tempest::Detail;

static void addPoolSize(VkDescriptorPoolSize *p, size_t &sz, uint32_t cnt, VkDescriptorType elt) {
  for(size_t i=0; i<sz; ++i){
    if(p[i].type==elt) {
      p[i].descriptorCount += cnt;
      return;
      }
    }
  p[sz].type            = elt;
  p[sz].descriptorCount = cnt;
  sz++;
  }

static ShaderReflection::LayoutDesc descriptorPart(ShaderReflection::LayoutDesc layout, uint32_t active) {
  const auto removed = layout.active & ~active;
  layout.active    &= active;
  layout.array     &= active;
  layout.runtime   &= active;
  layout.resources &= active;
  layout.samplers  &= active;
  for(auto mask=removed; mask!=0; mask &= mask-1) {
    const auto i       = std::countr_zero(mask);
    layout.bindings[i] = ShaderReflection::Count;
    layout.stage[i]    = ShaderReflection::None;
    layout.count[i]    = 0;
    layout.bufferSz[i] = 0;
    layout.bufferEl[i] = 0;
    }
  return layout;
  }

static bool sameActiveBindings(const Bindings& a, const Bindings& b, uint32_t active) {
  if(((a.array ^ b.array) & active)!=0)
    return false;
  for(auto mask=active; mask!=0; mask &= mask-1) {
    const auto i = std::countr_zero(mask);
    if(a.data[i]!=b.data[i])
      return false;
    // An array copies its own descriptors, including their samplers and image views.
    if((a.array & (1u << i))==0 && (a.offset[i]!=b.offset[i] || a.smp[i]!=b.smp[i] || a.map[i]!=b.map[i]))
      return false;
    }
  return true;
  }

VPoolCache::VPoolCache(VDevice& dev)
  : dev(dev) {
  cache.reserve(MaxCache);
  }

VPoolCache::~VPoolCache() {
  for(auto* entries : {&descriptors, &splitDescriptors})
    for(const auto& entry : *entries)
      vkDestroyDescriptorPool(dev.device.impl, entry.pool, nullptr);
  for(auto& i:cache)
    vkDestroyDescriptorPool(dev.device.impl, i, nullptr);
  }

void VPoolCache::setupLimits() {
  if(dev.hasDeviceFeatures2) {
    VkPhysicalDeviceAccelerationStructurePropertiesKHR rtas = {};
    rtas.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR;

    VkPhysicalDeviceProperties2 properties = {};
    properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2_KHR;
    properties.pNext = &rtas;

    auto vkGetPhysicalDeviceProperties2 = PFN_vkGetPhysicalDeviceProperties2(vkGetInstanceProcAddr(dev.instance,"vkGetPhysicalDeviceProperties2KHR"));
    vkGetPhysicalDeviceProperties2(dev.physicalDevice, &properties);

    limits   = properties.properties.limits;
    rtLimits = rtas;
    } else {
    VkPhysicalDeviceProperties prop = {};
    vkGetPhysicalDeviceProperties(dev.physicalDevice, &prop);
    limits = prop.limits;
    }
  }

void VPoolCache::notifyDestroy(const AbstractGraphicsApi::NoCopy* res) {
  std::lock_guard<std::mutex> guard(sync);

  for(auto* entries : {&descriptors, &splitDescriptors}) {
    for(size_t i=0; i<entries->size();) {
      auto& d = (*entries)[i];
      if(!d.bindings.contains(res)) {
        ++i;
        continue;
        }
      vkDestroyDescriptorPool(dev.device.impl, d.pool, nullptr);
      d = std::move(entries->back());
      entries->pop_back();
      }
    }
  }

void VPoolCache::notifyUpdate(const VDescriptorArray& arr, uint32_t element) {
  const Range range{element, 1};
  notifyUpdate(arr, std::span(&range,1));
  }

void VPoolCache::notifyUpdate(const VDescriptorArray& arr, std::span<const Range> ranges) {
  // Both forms of cached set hold copies of the array. The caller waits for
  // GPU use of the array and its cached copies before this update.
  std::lock_guard<std::mutex> guard(sync);

  // The source set has already been updated, so copying to cached sets must not allocate.
  VkCopyDescriptorSet cpy[MaxBindings] = {};
  uint32_t cnt = 0;
  for(auto* entries : {&descriptors, &splitDescriptors}) {
    for(const auto& d : *entries) {
      for(uint32_t mask=d.arrays; mask!=0; mask &= mask-1) {
        const auto i = std::countr_zero(mask);
        if(d.bindings.data[i]!=&arr)
          continue;
        for(const auto& range : ranges) {
          VkCopyDescriptorSet& cx = cpy[cnt++];
          cx.sType           = VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET;
          cx.srcSet          = arr.set();
          cx.srcArrayElement = range.first;
          cx.dstSet          = d.set;
          cx.dstBinding      = uint32_t(i);
          cx.dstArrayElement = range.first;
          cx.descriptorCount = range.count;
          if(cnt==MaxBindings) {
            vkUpdateDescriptorSets(dev.device.impl, 0, nullptr, cnt, cpy);
            cnt = 0;
            }
          }
        }
      }
    }

  if(cnt>0)
    vkUpdateDescriptorSets(dev.device.impl, 0, nullptr, cnt, cpy);
  }

VkDescriptorPool VPoolCache::allocPool() {
  {
    std::lock_guard<std::mutex> guard(sync);
    if(cache.size()>0) {
      auto ret = cache.back();
      cache.pop_back();
      return ret;
      }
  }

  VkDescriptorPoolSize poolSize[int(ShaderReflection::Class::Count)] = {};
  size_t               pSize   = 0;

  const uint32_t maxResources = 8096;
  const uint32_t maxSamplers  = 1024;

  for(size_t i=0; i<ShaderReflection::Class::Count; ++i) {
    if(i==ShaderReflection::Push)
      continue;
    if(i==ShaderReflection::Tlas && !dev.props.raytracing.rayQuery)
      continue;
    auto& sz = poolSize[pSize];
    ++pSize;

    switch(ShaderReflection::Class(i)) {
      case ShaderReflection::Ubo: {
        sz.type            = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        sz.descriptorCount = std::min(limits.maxDescriptorSetUniformBuffers, maxResources);
        break;
        }
      case ShaderReflection::Texture: {
        sz.type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        sz.descriptorCount = std::min(limits.maxDescriptorSetSampledImages, maxResources);
        sz.descriptorCount = std::min(limits.maxDescriptorSetSamplers, sz.descriptorCount);
        sz.descriptorCount = std::min(maxSamplers, sz.descriptorCount);
        break;
        }
      case ShaderReflection::Image: {
        sz.type            = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        sz.descriptorCount = std::min(limits.maxDescriptorSetSampledImages, maxResources);
        break;
        }
      case ShaderReflection::Sampler: {
        sz.type            = VK_DESCRIPTOR_TYPE_SAMPLER;
        sz.descriptorCount = std::min(limits.maxDescriptorSetSamplers, maxSamplers);
        break;
        }
      case ShaderReflection::SsboR:
      case ShaderReflection::SsboRW: {
        sz.type            = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        sz.descriptorCount = std::min(limits.maxDescriptorSetStorageBuffers, maxResources);
        break;
        }
      case ShaderReflection::ImgR:
      case ShaderReflection::ImgRW: {
        sz.type            = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        sz.descriptorCount = std::min(limits.maxDescriptorSetStorageImages, maxResources);
        break;
        }
      case ShaderReflection::Tlas: {
        if(dev.props.raytracing.rayQuery) {
          sz.type            = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
          sz.descriptorCount = std::min(rtLimits.maxDescriptorSetAccelerationStructures, maxSamplers); // note: can be less than 32
          }
        break;
        }
      case ShaderReflection::Push:    break;
      case ShaderReflection::Count:   break;
      }
    }

  VkDescriptorPoolCreateInfo poolInfo = {};
  poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  poolInfo.maxSets       = std::max(maxResources, maxSamplers);
  poolInfo.flags         = 0;
  poolInfo.poolSizeCount = uint32_t(pSize);
  poolInfo.pPoolSizes    = poolSize;

  VkDescriptorPool ret = VK_NULL_HANDLE;
  vkAssert(vkCreateDescriptorPool(dev.device.impl, &poolInfo, nullptr, &ret));
  return ret;
  }

void VPoolCache::freePool(VkDescriptorPool p) {
  std::lock_guard<std::mutex> guard(sync);
  if(cache.size()>=MaxCache) {
    vkDestroyDescriptorPool(dev.device.impl, p, nullptr);
    return;
    }

  try {
    cache.push_back(p);
    vkResetDescriptorPool(dev.device.impl, p, 0);
    }
  catch(...) {
    vkDestroyDescriptorPool(dev.device.impl, p, nullptr);
    }
  }

VkDescriptorPool VPoolCache::allocPool(const LayoutDesc &l) {
  VkDescriptorPoolSize poolSize[int(ShaderReflection::Class::Count)] = {};
  size_t               pSize   = 0;

  for(size_t i=0; i<MaxBindings; ++i) {
    if(l.stage[i]==Tempest::Detail::ShaderReflection::None)
      continue;
    auto     cls = l.bindings[i];
    uint32_t cnt = l.count[i];
    if((l.runtime & (1u << i))!=0)
      cnt = std::max<uint32_t>(1, l.count[i]);
    switch(cls) {
      case ShaderReflection::Ubo:     addPoolSize(poolSize,pSize,cnt,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);             break;
      case ShaderReflection::Texture: addPoolSize(poolSize,pSize,cnt,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);     break;
      case ShaderReflection::Image:   addPoolSize(poolSize,pSize,cnt,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);              break;
      case ShaderReflection::Sampler: addPoolSize(poolSize,pSize,cnt,VK_DESCRIPTOR_TYPE_SAMPLER);                    break;
      case ShaderReflection::SsboR:
      case ShaderReflection::SsboRW:  addPoolSize(poolSize,pSize,cnt,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);             break;
      case ShaderReflection::ImgR:
      case ShaderReflection::ImgRW:   addPoolSize(poolSize,pSize,cnt,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);              break;
      case ShaderReflection::Tlas:    addPoolSize(poolSize,pSize,cnt,VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR); break;
      case ShaderReflection::Push:    break;
      case ShaderReflection::Count:   break;
      }
    }

  VkDescriptorPoolCreateInfo poolInfo = {};
  poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  poolInfo.maxSets       = 1;
  poolInfo.flags         = 0;
  poolInfo.poolSizeCount = uint32_t(pSize);
  poolInfo.pPoolSizes    = poolSize;

  if(l.isUpdateAfterBind())
    poolInfo.flags |= VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;

  VkDevice dev = this->dev.device.impl;
  VkDescriptorPool ret = VK_NULL_HANDLE;
  vkAssert(vkCreateDescriptorPool(dev,&poolInfo,nullptr,&ret));
  return ret;
  }

VkDescriptorSet VPoolCache::allocDescSet(VkDescriptorPool pool, VkDescriptorSetLayout lay) {
  VkDescriptorSetAllocateInfo allocInfo = {};
  allocInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  allocInfo.descriptorPool     = pool;
  allocInfo.descriptorSetCount = 1;
  allocInfo.pSetLayouts        = &lay;

  VkDescriptorSet desc = VK_NULL_HANDLE;
  vkAssert(vkAllocateDescriptorSets(dev.device.impl, &allocInfo, &desc));
  return desc;
  }

uint32_t VPoolCache::bindlessCapacity(ShaderReflection::Class cls, size_t size) {
  // The set layout, and with it the pipeline layout of every PSO instance, follows these counts.
  // Large arrays grow in power-of-two steps from a high floor, so a streamed world adding buckets
  // keeps one layout instead of recompiling every pipeline whenever the array size changes.
  // Only the array's own descriptors are copied; the tail stays unwritten (PARTIALLY_BOUND).
  constexpr size_t LargeArray    = 1024;
  constexpr size_t LargeCapacity = 8192;
  if(size<LargeArray)
    return uint32_t(size);
  const auto capacity = dev.roundUpDescriptorCount(cls, std::bit_ceil(std::max(size, LargeCapacity)));
  return uint32_t(std::max<size_t>(size, capacity));
  }

VPoolCache::LayoutDesc VPoolCache::bindlessLayout(const LayoutDesc& layout, const Bindings& binding) {
  auto lx = layout;
  for(uint32_t mask=lx.runtime; mask!=0; mask &= mask-1) {
    const auto i = std::countr_zero(mask);
    if((binding.array & (1u << i))==0 || binding.data[i]==nullptr)
      throw std::logic_error("Missing runtime descriptor array");
    const auto* a = static_cast<const VDescriptorArray*>(binding.data[i]);
    // A different stage can declare a fixed size for the same binding.
    lx.count[i] = std::max(lx.count[i], bindlessCapacity(lx.bindings[i], a->size()));
    }
  return lx;
  }

VPoolCache::Inst VPoolCache::allocBindless(const PushBlock& pb, const LayoutDesc& layout, const Bindings& binding) {
  const auto lx = bindlessLayout(layout, binding);

  Inst ret;
  ret.dLay = dev.setLayouts.findLayout(lx);
  ret.pLay = dev.psoLayouts.findLayout(pb, ret.dLay);

  std::lock_guard<std::mutex> guard(sync);
  for(auto& i:descriptors) {
    if(i.dLay!=ret.dLay || i.bindings!=binding)
      continue;
    ret.set = i.set;
    return ret;
    }

  ret.set = cacheSet(descriptors, ret.dLay, lx, binding);
  return ret;
  }

VPoolCache::SplitInst VPoolCache::allocSplit(const PushBlock& pb, const LayoutDesc& layout, const Bindings& binding) {
  if(((binding.array ^ layout.array) & layout.active)!=0)
    throw std::logic_error("Descriptor binding does not match shader array declaration");
  for(uint32_t mask=layout.array & ~layout.runtime; mask!=0; mask &= mask-1) {
    const auto i = std::countr_zero(mask);
    const auto* a = static_cast<const VDescriptorArray*>(binding.data[i]);
    if(a==nullptr || a->size()!=layout.count[i])
      throw std::logic_error("Incorrect size for fixed descriptor array");
    }

  const auto lx = bindlessLayout(layout, binding);
  const auto scalars = descriptorPart(lx, ~lx.array);
  const auto arrays  = descriptorPart(lx, lx.array);
  const auto scalarLayout = dev.setLayouts.findLayout(scalars);
  const auto arrayLayout  = dev.setLayouts.findLayout(arrays);

  SplitInst ret;
  ret.pLay = dev.psoLayouts.findLayout(pb, scalarLayout, arrayLayout);

  std::lock_guard<std::mutex> guard(sync);
  const auto getSet = [&](const LayoutDesc& part, VkDescriptorSetLayout setLayout) {
    for(const auto& entry : splitDescriptors)
      if(entry.dLay==setLayout && sameActiveBindings(entry.bindings, binding, part.active))
        return entry.set;

    // Hold only the bindings used by this set so unrelated resources cannot evict it.
    Bindings key;
    key.array = binding.array & part.active;
    for(auto mask=part.active; mask!=0; mask &= mask-1) {
      const auto i  = std::countr_zero(mask);
      key.data[i]   = binding.data[i];
      key.offset[i] = binding.offset[i];
      key.smp[i]    = binding.smp[i];
      key.map[i]    = binding.map[i];
      }
    return cacheSet(splitDescriptors, setLayout, part, key);
    };

  ret.sets[0] = getSet(scalars, scalarLayout);
  ret.sets[1] = getSet(arrays, arrayLayout);
  return ret;
  }

VkDescriptorSet VPoolCache::cacheSet(std::vector<DSet>& entries, VkDescriptorSetLayout setLayout,
                                    const LayoutDesc& layout, const Bindings& binding) {
  auto& desc = entries.emplace_back();
  try {
    desc.dLay     = setLayout;
    desc.bindings = binding;
    desc.arrays   = layout.active & binding.array;
    desc.pool     = allocPool(layout);
    desc.set      = allocDescSet(desc.pool, setLayout);
    initDescriptorSet(desc.set, binding, layout);
    }
  catch(...) {
    if(desc.pool!=VK_NULL_HANDLE)
      vkDestroyDescriptorPool(dev.device.impl, desc.pool, nullptr);
    entries.pop_back();
    throw;
    }

  return desc.set;
  }

void VPoolCache::initDescriptorSet(VkDescriptorSet dset, const Bindings &binding, const LayoutDesc& l) {
  VkCopyDescriptorSet    cpy[MaxBindings] = {};
  uint32_t               cntCpy = 0;

  VPushDescriptor::WriteInfo winfo[MaxBindings] = {};
  VkWriteDescriptorSet       wr   [MaxBindings] = {};
  uint32_t                   cntWr = 0;

  for(size_t i=0; i<MaxBindings; ++i) {
    if((l.active & (1u << i))==0)
      continue;

    VkCopyDescriptorSet& cx = cpy[cntCpy];
    if((binding.array & (1u << i))!=0) {
      // assert((l.runtime & (1u << i))!=0);
      auto arr = reinterpret_cast<const VDescriptorArray*>(binding.data[i]);
      cx.sType           = VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET;
      cx.pNext           = nullptr;
      cx.srcSet          = arr->set();
      cx.srcBinding      = 0;
      cx.srcArrayElement = 0;
      cx.dstSet          = dset;
      cx.dstBinding      = uint32_t(i);
      cx.dstArrayElement = 0;
      cx.descriptorCount = uint32_t(arr->size());
      if(cx.descriptorCount>0)
        ++cntCpy;
      continue;
      }

    VPushDescriptor::write(dev, wr[cntWr], winfo[cntWr], uint32_t(i), l.bindings[i],
                           binding.data[i], binding.offset[i], binding.map[i], binding.smp[i]);

    VkWriteDescriptorSet& wx = wr[cntWr];
    wx.dstSet = dset;
    if(wx.descriptorCount>0)
      ++cntWr;
    }

  vkUpdateDescriptorSets(dev.device.impl, cntWr, wr, cntCpy, cpy);
  }

#endif
