#pragma once

#include <Tempest/AbstractGraphicsApi>
#include <Metal/Metal.hpp>

#include "gapi/shaderreflection.h"
#include "nsptr.h"

#include <unordered_map>
#include <vector>

namespace Tempest {
namespace Detail {

class MtDevice;
class MtBuffer;

struct MtBlasBuildCtx : AbstractGraphicsApi::BlasBuildCtx {
  void pushGeometry(MtDevice& dx,
                    const MtBuffer& vbo, size_t vboSz, size_t stride,
                    const MtBuffer& ibo, size_t iboSz, size_t ioffset, IndexClass icls);
  void bake();

  MTL::AccelerationStructureSizes buildSizes(MtDevice& dx) const;

  std::vector<NsPtr<MTL::AccelerationStructureTriangleGeometryDescriptor>> ranges;
  std::vector<const NS::Object*>                                           geo;
  NsPtr<MTL::PrimitiveAccelerationStructureDescriptor>                     desc;
  };

class MtAccelerationStructure : public Tempest::AbstractGraphicsApi::AccelerationStructure {
  public:
    MtAccelerationStructure(MtDevice& owner,
                            MtBuffer& vbo, size_t vboSz, size_t stride,
                            MtBuffer& ibo, size_t iboSz, size_t ioffset, Detail::IndexClass icls);
    MtAccelerationStructure(MtDevice& owner, const AbstractGraphicsApi::RtGeometry* geom, size_t size);
    ~MtAccelerationStructure();

    MtDevice&                         owner;
    NsPtr<MTL::AccelerationStructure> impl;
  };

class MtTopAccelerationStructure : public AbstractGraphicsApi::AccelerationStructure {
  public:
    MtTopAccelerationStructure(MtDevice& owner, const RtInstance* inst, AccelerationStructure* const * as, size_t asSize);
    // Memory for `capacity` instances, the structure and the scratch of its builds (MtCommandBuffer::buildTlas)
    MtTopAccelerationStructure(MtDevice& owner, size_t capacity);
    ~MtTopAccelerationStructure();

    void useResource(MTL::RenderCommandEncoder&  cmd, ShaderReflection::Stage st) const;
    void useResource(MTL::ComputeCommandEncoder& cmd, ShaderReflection::Stage st) const;

    // One instance into its slot (Device::packRtInstance): `slot` holds zeros or what pack() wrote there before.
    // The instance holds its BLAS `as` by an index in `blas` and releases the one it held; null `as`: inactive.
    void pack(const RtInstance& inst, AccelerationStructure* as, void* slot);
    // Descriptor of a build from `count` instances in `instances`
    NsPtr<MTL::InstanceAccelerationStructureDescriptor> buildDesc(MTL::Buffer* instances, size_t count) const;

    MtDevice&                         owner;
    NsPtr<MTL::AccelerationStructure> impl;
    NsPtr<MTL::Buffer>                instances;
    NsPtr<MTL::Buffer>                scratch;  // second constructor: kept between builds
    size_t                            capacity = 0;

    // BLAS the instances index: the builds read them, ray queries make them resident (useResources). A BLAS keeps
    // its index while any instance holds it, and the list retains it meanwhile. With the second constructor index 0
    // is a placeholder (an inactive instance, all zeros, indexes it), and so is every free index: the list never
    // holds a BLAS no instance holds.
    std::vector<MTL::Resource*>       blas;

  private:
    uint32_t acquire(MTL::Resource* ax);
    void     release(uint32_t index);

    template<class Enc>
    void implUseResource(Enc& cmd, ShaderReflection::Stage st) const;

    void implUseResource(MTL::ComputeCommandEncoder& cmd,
                         const MTL::Resource* const resources[], NS::UInteger count, MTL::ResourceUsage usage, MTL::RenderStages stages) const;
    void implUseResource(MTL::RenderCommandEncoder& cmd,
                         const MTL::Resource* const resources[], NS::UInteger count, MTL::ResourceUsage usage, MTL::RenderStages stages) const;

    NsPtr<MTL::AccelerationStructure>           placeholder;
    // Vertices and indices of the placeholder: its build reads them through the geometry descriptor, and Metal does
    // not document that the command buffer retains those, so the structure keeps them
    NsPtr<MTL::Buffer>                          placeholderVbo;
    NsPtr<MTL::Buffer>                          placeholderIbo;
    std::vector<uint32_t>                       blasRefs;  // instances holding blas[i]; 0 for the placeholder
    std::unordered_map<MTL::Resource*,uint32_t> blasIndex;
    std::vector<uint32_t>                       blasFree;
  };
}
}
