#pragma once

#include <Tempest/AbstractGraphicsApi>
#include "vulkan_sdk.h"

namespace Tempest {
namespace Detail {

class VDevice;
#if defined(OPENGOTHIC_ENABLE_FRAME_PROFILE) && OPENGOTHIC_ENABLE_FRAME_PROFILE
struct VFrameProfileQueries;
#endif

struct VFence : public AbstractGraphicsApi::Fence {
  using ResPtr  = Detail::DSharedPtr<const AbstractGraphicsApi::Shared*>;

  VFence(VDevice* device, VkFence f, uint32_t id):device(device), fence(f), id(id) {}

  void wait() override;
  bool wait(uint64_t time) override;

  void setStatus(VkResult status);
  void setPayload(std::vector<ResPtr>&&) override;
  void clearPayload();

  VDevice*            device = nullptr;
  VkFence             fence  = VK_NULL_HANDLE;
  uint32_t            id     = 0;
  VkResult            status = VK_SUCCESS;

  std::vector<ResPtr> holdRes;
#if defined(OPENGOTHIC_ENABLE_FRAME_PROFILE) && OPENGOTHIC_ENABLE_FRAME_PROFILE
  std::shared_ptr<VFrameProfileQueries> profileQueries;
#endif
  };

}}
