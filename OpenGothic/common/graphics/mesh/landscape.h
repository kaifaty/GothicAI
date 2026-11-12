#pragma once

#include <Tempest/Device>
#include <Tempest/Matrix4x4>
#include <Tempest/UniformBuffer>

#include "graphics/visualobjects.h"
#include "graphics/mesh/submesh/staticmesh.h"

class PackedMesh;

class Landscape final {
  public:
    Landscape(VisualObjects& visual, const PackedMesh& wmesh);
    Landscape(VisualObjects& visual, const PackedMesh& wmesh, LandscapePages& pages, const Tempest::Vec3& texturePosition);
    ~Landscape();

    void setVisible(bool visible);

  private:
    void init(const PackedMesh& packed, bool wholeWorld, const Tempest::Vec3* texturePosition = nullptr);
    using Item = VisualObjects::Item;

    struct Block {
      Item mesh;
      };

    // The ray scene: the solid (0) and the alpha-tested (1) submeshes as a BLAS each, a geometry per submesh, built
    // with the Landscape (a chunk: at its publication) and recycled with it; water keeps a BLAS of its submesh
    struct RtPart {
      Tempest::AccelerationStructure blas;
      size_t                         id = size_t(-1);  // VisualObjects::addRtLandscape
      };

    VisualObjects&         visual;
    StaticMesh             mesh;
    Tempest::StorageBuffer meshletDesc;
    std::vector<Block>     blocks;
    RtPart                 rt[2];
  };
