#include "accelerationstructure.h"

#if defined(OPENGOTHIC_ENABLE_FRAME_PROFILE) && OPENGOTHIC_ENABLE_FRAME_PROFILE && defined(TEMPEST_BUILD_VULKAN)
#include "gapi/vulkan/vaccelerationstructure.h"
#endif

#include <atomic>

using namespace Tempest;

static std::atomic<size_t> liveStructures{0};

// Structures alive now, the ones Resources::recycle still keeps included (a test's count of what a world leaves)
size_t tempestProbeAccelerationStructureCount() {
  return liveStructures.load(std::memory_order_relaxed);
  }

AccelerationStructure::AccelerationStructure(Device& dev, AbstractGraphicsApi::PRtAs&& impl, size_t capacity)
  :impl(std::move(impl)), cap(capacity) {
  if(this->impl.handler!=nullptr)
    liveStructures.fetch_add(1,std::memory_order_relaxed);
  }

AccelerationStructure::~AccelerationStructure() {
  if(impl.handler!=nullptr)
    liveStructures.fetch_sub(1,std::memory_order_relaxed);
  }

AccelerationStructure::AccelerationStructure(AccelerationStructure&& other)
  :impl(std::move(other.impl)), cap(other.cap) {
  other.cap = 0;
  }

AccelerationStructure& AccelerationStructure::operator=(AccelerationStructure&& other) {
  std::swap(impl, other.impl);
  std::swap(cap,  other.cap);
  return *this;
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
