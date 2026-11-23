#pragma once

// Static props of the empty island (reports/engine/2026-11-13-empty-island/island-content.md): four loose stones
// and a shed of posts, beams and boards, written as GLB the engine reads through meshes.json. Where they stand is
// decided by the world that places them; each has its origin on the ground under its middle.

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace island {

// Keys of meshes.json; a visual is asked for as the key with ".3DS".
inline const char* const PropNames[] = {"ISLAND_ROCK_1", "ISLAND_ROCK_2", "ISLAND_ROCK_3", "ISLAND_ROCK_4",
                                        "ISLAND_SHED"};

struct PropNote {
  std::string name;
  size_t      triangles = 0;
  float       min[3] = {}, max[3] = {}; // metres, y up
  };

// meshes.json and the GLB files under dir (<island>/content/meshes). Returns what was built.
std::vector<PropNote> writeProps(const std::filesystem::path& dir);

}
