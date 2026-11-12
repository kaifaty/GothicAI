#pragma once

#include "worldchunks.h"

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// The runtime view of world.json "chunks". worldchunks.h is a base format
// input (cmake/BaseFormat.cmake), so the writer and WorldChunkManifest stay as
// they are; the cells live here, outside the base fingerprint.

struct ChunkId {
  // The converter uses a 64 m grid rooted at (0,0), including negative cells.
  static constexpr int32_t Size = 6400;
  int32_t x = 0;
  int32_t z = 0;

  static ChunkId fromPosition(float x, float z) {
    return {int32_t(std::floor(double(x)/Size)),int32_t(std::floor(double(z)/Size))};
    }
  bool operator==(const ChunkId&) const = default;
  };

// Only the draw level 0 mesh of a cell is read so far. The others are parsed,
// validated and carried for levels of detail and collision; their files are
// not opened.
enum class ChunkMeshRole : uint8_t {
  Draw,
  Collision,
  };

struct ChunkMeshRef {
  std::string   file;  // relative to the world directory
  ChunkMeshRole role  = ChunkMeshRole::Draw;
  int32_t       level = 0; // draw level of detail; always 0 for collision
  bool operator==(const ChunkMeshRef&) const = default;
  };

// One entry of world.json "chunks". A string is one draw level 0 mesh in the
// cell of its file stem <x>_<z>; an object is
//   {"cell": [x, z], "meshes": [{"file": ..., "role": "draw", "level": 0},
//                               {"file": ..., "role": "collision"}]}
// with exactly one draw level 0 mesh and at most one mesh per role and level.
struct ChunkCell {
  ChunkId                   id;
  std::vector<ChunkMeshRef> meshes; // in manifest order
  const std::string& drawFile() const; // the draw level 0 mesh, the one the reader loads
  bool operator==(const ChunkCell&) const = default;
  };

// world.json as the engine reads it: one cell per entry of "chunks", in order,
// and chunks[i] is the draw level 0 file of cells[i].
struct WorldCellManifest : WorldChunkManifest {
  std::vector<ChunkCell> cells;
  };

// Reads and validates dir/world.json without opening any chunk. Throws
// std::runtime_error naming the entry, cell or file at fault.
WorldCellManifest readWorldChunkManifest(const std::filesystem::path& dir);
