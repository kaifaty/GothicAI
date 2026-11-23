#pragma once

#include <Tempest/AbstractGraphicsApi>

#include "vulkan_sdk.h"
#include "vbuffer.h"

namespace Tempest {
namespace Detail {

class VDevice;
class VBuffer;

struct VBlasBuildCtx : AbstractGraphicsApi::BlasBuildCtx {
  void pushGeometry(VDevice& dx,
                    const VBuffer& vbo, size_t vboSz, size_t stride,
                    const VBuffer& ibo, size_t iboSz, size_t ioffset, IndexClass icls);

  VkAccelerationStructureBuildSizesInfoKHR    buildSizes(VDevice& dx) const;
  VkAccelerationStructureBuildGeometryInfoKHR buildCmd  (VDevice& dx, VkAccelerationStructureKHR dest, VBuffer* scratch) const;

  std::vector<VkAccelerationStructureBuildRangeInfoKHR> ranges;
  std::vector<VkAccelerationStructureGeometryKHR>       geometry;
  std::vector<uint32_t>                                 maxPrimitiveCounts;
  };

class VAccelerationStructure : public AbstractGraphicsApi::AccelerationStructure {
  public:
    VAccelerationStructure(VDevice& owner);
    ~VAccelerationStructure();

    void                       build(const AbstractGraphicsApi::RtGeometry* geom, size_t size);
    VkDeviceAddress            toDeviceAddress(VDevice& owner) const;

    VDevice&                   owner;
    VkAccelerationStructureKHR impl = VK_NULL_HANDLE;
    VBuffer                    data;
    VkDeviceAddress            address = 0; // of impl, which keeps it for its lifetime
  };

class VTopAccelerationStructure : public AbstractGraphicsApi::AccelerationStructure {
  public:
    VTopAccelerationStructure(VDevice& owner);
    ~VTopAccelerationStructure();

    void            build(const RtInstance* inst, AccelerationStructure* const * as, size_t size);
    // Memory for `capacity` instances, the structure and the scratch of its builds (VCommandBuffer::buildTlas)
    void            alloc(size_t capacity);
    VkDeviceAddress toDeviceAddress(VDevice& owner) const;

    static void     pack(VkAccelerationStructureInstanceKHR& dst, const RtInstance& inst, const VAccelerationStructure& blas);

    VDevice&                   owner;
    VkAccelerationStructureKHR impl = VK_NULL_HANDLE;
    VBuffer                    data;
    VBuffer                    scratch;      // alloc(): kept between builds
    size_t                     capacity = 0; // alloc(): instances the memory holds
  };

}
}
