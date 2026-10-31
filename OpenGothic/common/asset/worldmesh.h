#pragma once

#include "material.h"
#include <zenkit/Boxes.hh>
#include <vector>

namespace asset {

struct VertexFeature {
  zenkit::Vec2 texture;
  uint32_t light;
  zenkit::Vec3 normal;
  };

struct PolygonFlagSet {
  uint8_t is_portal : 2;
  uint8_t is_occluder : 1;
  uint8_t is_sector : 1;
  uint8_t should_relight : 1;
  uint8_t is_outdoor : 1;
  uint8_t is_ghost_occluder : 1;
  uint8_t is_dynamically_lit : 1;
  int16_t sector_index : 16;
  uint8_t is_lod : 1;
  uint8_t normal_axis : 2;
  bool operator==(const PolygonFlagSet&) const = default;
  };

struct PolygonList {
  std::vector<uint32_t> material_indices;
  std::vector<int32_t> lightmap_indices;
  std::vector<uint32_t> feature_indices;
  std::vector<uint32_t> vertex_indices;
  std::vector<PolygonFlagSet> flags;
  };

// Render/collision triangles, after the archive's BSP filter. Raw BSP polygons
// and lightmap images are consumed at the archive boundary, not by this mesh.
struct WorldMesh {
  std::string name;
  zenkit::AxisAlignedBoundingBox bbox {};
  std::vector<Material> materials;
  std::vector<zenkit::Vec3> vertices;
  std::vector<VertexFeature> features;
  PolygonList polygons;
  };

}
