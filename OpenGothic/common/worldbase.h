#pragma once

#include <cstddef>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "asset/worldmesh.h"
#include "worlddata.h"

struct ChunkTable;

struct WorldBaseResult {
  bool        replaced = false;
  size_t      triangles = 0;
  size_t      chunks = 0;
  double      readMs = 0;
  std::string error;
  };

// Select a complete world against the selected base identity, without opening its ZEN.
// Failure leaves outputs untouched.
WorldBaseResult tryWorldData(WorldData::Data& world, asset::WorldMesh& worldMesh,
                            std::string_view name, const std::filesystem::path& dir,
                            const std::filesystem::path& overrideDir,
                            const std::vector<std::pair<std::string,uintmax_t>>& baseArchives,
                            const std::map<std::string,std::filesystem::path>* ownMeshes = nullptr,
                            ChunkTable* chunks = nullptr);
