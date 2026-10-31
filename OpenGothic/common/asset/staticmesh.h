#pragma once

#include "material.h"
#include <zenkit/Boxes.hh>
#include <utility>
#include <vector>

namespace asset {

struct MeshTriangle { uint16_t wedges[3]; };
struct MeshWedge {
  zenkit::Vec3 normal;
  zenkit::Vec2 texture;
  uint16_t index;
  };
struct SubMesh {
  Material mat;
  std::vector<MeshTriangle> triangles;
  std::vector<MeshWedge> wedges;
  };

struct StaticMesh {
  std::vector<zenkit::Vec3> positions;
  std::vector<zenkit::Vec3> normals;
  std::vector<SubMesh> sub_meshes;
  std::vector<Material> materials;
  uint8_t alpha_test = true;
  zenkit::AxisAlignedBoundingBox bbox {};
  zenkit::OrientedBoundingBox obbox;
  };

struct SkinWeight {
  float weight;
  zenkit::Vec3 position;
  uint8_t node_index;
  };
struct SoftSkinMesh {
  StaticMesh mesh;
  std::vector<zenkit::OrientedBoundingBox> bboxes;
  std::vector<std::vector<SkinWeight>> weights;
  };
struct ModelMesh {
  std::vector<SoftSkinMesh> meshes;
  // Preserve the archive map's iteration order: it determines attachment IDs.
  std::vector<std::pair<std::string,StaticMesh>> attachments;
  };

}
