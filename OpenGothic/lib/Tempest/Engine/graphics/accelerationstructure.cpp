#include "accelerationstructure.h"

#if defined(OPENGOTHIC_ENABLE_FRAME_PROFILE) && OPENGOTHIC_ENABLE_FRAME_PROFILE && defined(TEMPEST_BUILD_VULKAN)
#include "gapi/vulkan/vaccelerationstructure.h"
#endif

using namespace Tempest;

AccelerationStructure::AccelerationStructure(Device& dev, AbstractGraphicsApi::PRtAs&& impl)
  :impl(std::move(impl)) {
  }

bool Tempest::AccelerationStructure::isEmpty() const {
  return impl.handler==nullptr;
  }

#if defined(OPENGOTHIC_ENABLE_FRAME_PROFILE) && OPENGOTHIC_ENABLE_FRAME_PROFILE
int64_t Tempest::profileByteSize(const AccelerationStructure& as) {
  const auto* impl = as.impl.handler;
  if(impl==nullptr)
    return 0;
#if defined(TEMPEST_BUILD_VULKAN)
  if(auto* blas = dynamic_cast<const Detail::VAccelerationStructure*>(impl))
    return int64_t(blas->data.size());
  if(auto* tlas = dynamic_cast<const Detail::VTopAccelerationStructure*>(impl))
    return int64_t(tlas->data.size());
#endif
  return -1; // Metal: no size is kept
  }
#endif
