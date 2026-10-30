#pragma once

#include "worldchunks.h"

#include <cmath>
#include <span>

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
  // mesh, filtered alongside polygons by retainWorldTriangles. Ownership comes
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

// Keep an increasing, unique list of source triangle IDs in source order.
// Only polygon arrays and their corresponding triangleChunks are filtered;
// material, vertex and feature arrays and their source IDs remain unchanged.
// If chunks is supplied it must cover every input triangle; bounds/counts are
// rebuilt from the retained triangles. Reusable by visual and collision passes.
void retainWorldTriangles(asset::WorldMesh& mesh, std::span<const uint32_t> triangles,
                          ChunkTable* chunks = nullptr);

// Lists source polygon IDs per table entry without compacting the source mesh.
std::vector<std::vector<uint32_t>> chunkTriangleLists(const asset::WorldMesh& mesh, const ChunkTable& chunks);

// The fields readWorldChunks fills, copied from a source mesh: materials,
// vertices, features and polygons. readWorldChunks of unedited chunks equals
// canonicalWorldMesh of the source mesh bit for bit.
asset::WorldMesh canonicalWorldMesh(const asset::WorldMesh& mesh);

// Empty when equal; otherwise the first difference, for logs and tests.
std::string compareWorldMeshes(const asset::WorldMesh& expected, const asset::WorldMesh& actual);
