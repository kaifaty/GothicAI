#pragma once

#include <Tempest/AbstractGraphicsApi>

#include <mutex>

#include "gapi/shader.h"
#include "vulkan_sdk.h"

namespace Tempest {
namespace Detail {

class VDevice;

class VShader : public Tempest::Detail::Shader {
  public:
    VShader(VDevice& device, const void* source, size_t src_size);
    explicit VShader(VDevice& device);
    ~VShader();

    VkShaderModule splitDescriptorsModule() const;

    VkShaderModule impl = VK_NULL_HANDLE;

  protected:
    VkDevice       device;

  private:
    VkShaderModule createModule(const uint32_t* words, size_t wordCount) const;

    mutable std::mutex             splitSync;
    mutable std::vector<uint32_t> splitSource;
    mutable VkShaderModule         splitModule = VK_NULL_HANDLE;
  };

}}
