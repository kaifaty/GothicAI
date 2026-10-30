#pragma once

#include "worldchunks.h"

#include <cmath>

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

struct ChunkTable {
  struct Entry {
    ChunkId      id;
    std::string  file; // relative to the world directory, even with an override
    zenkit::Vec3 min {};
    zenkit::Vec3 max {};
    uint32_t     triangleCount = 0;
    };

  std::vector<Entry> entries;
  // One table index per triangle in readWorldChunks' stitched source-order
  // mesh, before cloth extraction or GPU/physics packing. Ownership comes
  // from the chunk file, so edited triangles keep their file's identity.
  std::vector<uint32_t> triangleChunks;
  };

WorldChunkManifest readWorldChunkManifest(const std::filesystem::path& dir);

// Reads the chunks listed in dir/world.json. A chunk with the same file name
// under overrideDir (if not empty) replaces the base one. Fills materials,
// vertices, features and polygons of a fresh mesh. Unedited chunks restore
// the source arrays exactly, with every source index (the engine's meshlet
// packing keys on them); geometry an edit added or moved goes after them.
asset::WorldMesh readWorldChunks(const std::filesystem::path& dir, const std::filesystem::path& overrideDir = {},
                                 ChunkTable* chunks = nullptr);

// The fields readWorldChunks fills, copied from a source mesh: materials,
// vertices, features and polygons. readWorldChunks of unedited chunks equals
// canonicalWorldMesh of the source mesh bit for bit.
asset::WorldMesh canonicalWorldMesh(const asset::WorldMesh& mesh);

// Empty when equal; otherwise the first difference, for logs and tests.
std::string compareWorldMeshes(const asset::WorldMesh& expected, const asset::WorldMesh& actual);
