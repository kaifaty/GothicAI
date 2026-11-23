#if defined(TEMPEST_BUILD_VULKAN)

#include "vpipelinecache.h"

#include <Tempest/Log>

#include "gapi/pipelinecache.h"

#include <chrono>
#include <cstring>
#include <vector>

using namespace Tempest;
using namespace Tempest::Detail;

static constexpr char   VkCacheMagic[8] = {'T','P','C','V','K','0','0','1'};
static constexpr size_t MaxCacheSize    = 512u*1024u*1024u;

static double msSince(std::chrono::steady_clock::time_point t) {
  return std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-t).count();
  }

VPipelineCache::~VPipelineCache() {
  destroy();
  }

void VPipelineCache::init(VkDevice device, VkPhysicalDevice pdev) {
  dev = device;

  VkPhysicalDeviceProperties prop = {};
  vkGetPhysicalDeviceProperties(pdev,&prop);
  std::memcpy(ident.magic,VkCacheMagic,sizeof(ident.magic));
  ident.vendorID      = prop.vendorID;
  ident.deviceID      = prop.deviceID;
  ident.driverVersion = prop.driverVersion;
  ident.apiVersion    = prop.apiVersion;
  std::memcpy(ident.uuid,prop.pipelineCacheUUID,VK_UUID_SIZE);

  const auto dir = PipelineCacheFile::directory();
  if(!dir.empty())
    file = dir/("vulkan-"+PipelineCacheFile::hex((uint64_t(prop.vendorID)<<32u) | prop.deviceID)+".bin");

  std::vector<uint8_t> bytes;
  const uint8_t*       initial     = nullptr;
  size_t               initialSize = 0;
  if(!file.empty() && PipelineCacheFile::read(file,bytes,MaxCacheSize)) {
    const char* reason = nullptr;
    FileHeader  head   = {};
    if(bytes.size()<sizeof(FileHeader)) {
      reason = "truncated header";
      } else {
      std::memcpy(&head,bytes.data(),sizeof(head));
      const uint8_t* payload = bytes.data()+sizeof(FileHeader);
      const size_t   size    = bytes.size()-sizeof(FileHeader);
      VkPipelineCacheHeaderVersionOne vkHead = {};
      if(std::memcmp(head.magic,VkCacheMagic,sizeof(head.magic))!=0)
        reason = "unknown format";
      else if(head.vendorID!=ident.vendorID || head.deviceID!=ident.deviceID)
        reason = "other device";
      else if(head.driverVersion!=ident.driverVersion || head.apiVersion!=ident.apiVersion)
        reason = "other driver version";
      else if(std::memcmp(head.uuid,ident.uuid,VK_UUID_SIZE)!=0)
        reason = "other pipeline cache UUID";
      else if(head.dataSize!=size || size<sizeof(vkHead))
        reason = "truncated data";
      else if(PipelineCacheFile::hash(payload,size)!=head.checksum)
        reason = "checksum mismatch";
      else {
        std::memcpy(&vkHead,payload,sizeof(vkHead));
        if(vkHead.headerVersion!=VK_PIPELINE_CACHE_HEADER_VERSION_ONE || vkHead.vendorID!=ident.vendorID ||
           vkHead.deviceID!=ident.deviceID || std::memcmp(vkHead.pipelineCacheUUID,ident.uuid,VK_UUID_SIZE)!=0)
          reason = "driver header mismatch";
        }
      if(reason==nullptr) {
        initial     = payload;
        initialSize = size;
        }
      }
    if(reason!=nullptr)
      Log::i("pipeline cache: ignored ",file.string(),": ",reason);
    }

  VkPipelineCacheCreateInfo info = {};
  info.sType           = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
  info.initialDataSize = initialSize;
  info.pInitialData    = initial;
  if(file.empty()) {
    // Off: keep the original behaviour, no cache object at all.
    Log::i("pipeline cache: off");
    return;
    }
  VkResult err = vkCreatePipelineCache(dev,&info,nullptr,&impl);
  if(err!=VK_SUCCESS && initial!=nullptr) {
    Log::i("pipeline cache: ignored ",file.string(),": rejected by driver");
    info.initialDataSize = 0;
    info.pInitialData    = nullptr;
    initial              = nullptr;
    initialSize          = 0;
    err = vkCreatePipelineCache(dev,&info,nullptr,&impl);
    }
  if(err!=VK_SUCCESS) {
    impl = VK_NULL_HANDLE;
    Log::i("pipeline cache: unavailable (VkResult=",int(err),")");
    return;
    }
  if(initial!=nullptr) {
    lastHash = PipelineCacheFile::hash(initial,initialSize);
    lastSize = initialSize;
    Log::i("pipeline cache: loaded ",initialSize," bytes from ",file.string());
    } else {
    Log::i("pipeline cache: empty, will write ",file.string());
    }
  }

void VPipelineCache::save(bool async) {
  if(impl==VK_NULL_HANDLE)
    return;
  joinSaver();
  if(async)
    saver = std::thread([this](){ saveNow("play"); });
  else
    saveNow("request");
  }

void VPipelineCache::joinSaver() {
  if(saver.joinable())
    saver.join();
  }

void VPipelineCache::saveNow(const char* reason) {
  std::lock_guard<std::mutex> guard(sync);
  const auto start = std::chrono::steady_clock::now();
  // The cache is internally synchronized: reading it does not block pipeline creation on other threads.
  size_t size = 0;
  if(vkGetPipelineCacheData(dev,impl,&size,nullptr)!=VK_SUCCESS || size==0)
    return;
  std::vector<uint8_t> bytes(sizeof(FileHeader)+size);
  if(vkGetPipelineCacheData(dev,impl,&size,bytes.data()+sizeof(FileHeader))!=VK_SUCCESS)
    return;
  bytes.resize(sizeof(FileHeader)+size);

  const uint8_t* payload = bytes.data()+sizeof(FileHeader);
  const uint64_t h       = PipelineCacheFile::hash(payload,size);
  if(h==lastHash && size==lastSize) {
    Log::i("pipeline cache: unchanged (",reason,")");
    return;
    }

  FileHeader head = ident;
  head.dataSize = size;
  head.checksum = h;
  std::memcpy(bytes.data(),&head,sizeof(head));
  if(PipelineCacheFile::writeAtomic(file,bytes.data(),bytes.size())) {
    lastHash = h;
    lastSize = size;
    Log::i("pipeline cache: saved ",size," bytes in ",int(msSince(start))," ms (",reason,")");
    }
  }

void VPipelineCache::destroy() {
  joinSaver();
  if(impl==VK_NULL_HANDLE)
    return;
  saveNow("exit");
  vkDestroyPipelineCache(dev,impl,nullptr);
  impl = VK_NULL_HANDLE;
  }

#endif
