#pragma once

#include <filesystem>
#include <mutex>
#include <thread>

#include "vulkan_sdk.h"

namespace Tempest {
namespace Detail {

// One VkPipelineCache per device: loaded from disk at device creation, saved on request and at destruction.
// The file is keyed by vendor, device, driver version and pipelineCacheUUID; anything else is ignored.
class VPipelineCache {
  public:
    VPipelineCache() = default;
    ~VPipelineCache();

    void        init(VkDevice dev, VkPhysicalDevice pdev);
    // Writes the current contents; async=true leaves the file I/O to a background thread.
    void        save(bool async);
    void        destroy();

    VkPipelineCache impl = VK_NULL_HANDLE;

  private:
    struct FileHeader {
      char     magic[8];
      uint32_t vendorID;
      uint32_t deviceID;
      uint32_t driverVersion;
      uint32_t apiVersion;
      uint8_t  uuid[VK_UUID_SIZE];
      uint64_t dataSize;
      uint64_t checksum;
      };

    void        saveNow(const char* reason);
    void        joinSaver();

    VkDevice              dev = VK_NULL_HANDLE;
    std::filesystem::path file;
    FileHeader            ident = {};
    uint64_t              lastHash = 0;
    size_t                lastSize = 0;
    std::mutex            sync;
    std::thread           saver;
  };

}
}
