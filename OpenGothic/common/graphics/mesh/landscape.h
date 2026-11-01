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

    const Tempest::StorageBuffer& bvh()   const { return bvhNodes; }

  private:
    void init(const PackedMesh& packed, bool wholeWorld, const Tempest::Vec3* texturePosition = nullptr);
    using Item = VisualObjects::Item;

    struct Block {
      Item mesh;
      };

    VisualObjects&         visual;
    StaticMesh             mesh;
    Tempest::StorageBuffer meshletDesc;

    Tempest::StorageBuffer bvhNodes;
    std::vector<Block>     blocks;
  };
