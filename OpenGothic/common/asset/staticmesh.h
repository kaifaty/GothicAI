#pragma once

#include "material.h"
#include "math.h"
#include <utility>
#include <vector>

namespace asset {

struct MeshTriangle { uint16_t wedges[3]; };
struct MeshWedge {
  Vec3 normal;
  Vec2 texture;
  uint16_t index;
  };
struct SubMesh {
  Material mat;
  std::vector<MeshTriangle> triangles;
  std::vector<MeshWedge> wedges;
  };

struct StaticMesh {
  std::vector<Vec3> positions;
  std::vector<Vec3> normals;
  std::vector<SubMesh> sub_meshes;
  std::vector<Material> materials;
  uint8_t alpha_test = true;
  Aabb bbox {};
  Obb obbox;
  };

struct SkinWeight {
  float weight;
  Vec3 position;
  uint8_t node_index;
  };
struct SoftSkinMesh {
  StaticMesh mesh;
  std::vector<Obb> bboxes;
  std::vector<std::vector<SkinWeight>> weights;
  };
struct ModelMesh {
  std::vector<SoftSkinMesh> meshes;
  // Preserve the archive map's iteration order: it determines attachment IDs.
  std::vector<std::pair<std::string,StaticMesh>> attachments;
  };

}
