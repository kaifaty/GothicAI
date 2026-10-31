#pragma once

#include "asset/worldmesh.h"

#include <cstdint>
#include <string>
#include <vector>

// Authored cloth polygons are split before either landscape packing pass.
// This keeps the original triangles available for the cloth=0 fallback without
// retaining a second copy of the entire landscape or changing BSP sectors.
namespace WorldCloth {

struct Surface {
  asset::WorldMesh mesh;
  std::vector<uint32_t> pins;
  std::string visual;
  uint32_t id = 0;
  float stiffness = 0.85f;
  float linearDamping = 1.5f;
  };

struct Extracted {
  asset::WorldMesh original;
  std::vector<Surface> surfaces;
  };

Extracted extract(asset::WorldMesh& landscape);

}
