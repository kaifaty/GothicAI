#pragma once

// The empty island as files (reports/engine/2026-11-13-empty-island/island-content.md): everything the engine
// needs to draw it, made by code alone. No input but the code: two runs write the same bytes.
//
//   <dir>/world/               world.json and the chunks        (readWorldChunks)
//   <dir>/content/materials/   materials.json and the DDS maps  (OPENGOTHIC_CONTENT = <dir>/content)
//   <dir>/content/meshes/      meshes.json and the GLB props
//   <dir>/textures/            the sky, the sun and the moon    (OPENGOTHIC_TEXTURES = <dir>/textures)

#include "islandprops.h"
#include "islandtextures.h"

#include <cstdint>
#include <filesystem>
#include <vector>

namespace island {

// The world's name: WorldView and the sky ask for it.
constexpr char WorldName[] = "ISLAND.ZEN";

struct Written {
  uint64_t                  triangles = 0; // of the ground and the water
  uint64_t                  chunks    = 0;
  std::vector<MaterialNote> materials;
  std::vector<PropNote>     props;
  double                    seconds[4] = {}; // the world, the materials, the props, the sky
  };

// Replaces world/, content/ and textures/ under dir; nothing else there is touched.
Written writeIsland(const std::filesystem::path& dir);

}
