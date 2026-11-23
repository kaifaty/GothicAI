#pragma once

#include "asset/worldmesh.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

struct ChunkTable;

// Authored cloth polygons are split before either landscape packing pass.
// This keeps the original triangles available for the cloth=0 fallback without
// retaining a second copy of the entire landscape or changing BSP sectors.
namespace WorldCloth {

// The number of searchIslands' algorithm. Raise it whenever its result changes for the same landscape, chunk
// table and rules: a cooked bundle (worldclothbundle.h) of another number is not used.
// gothic-worldcloth-cooked-contract holds a digest of the result on its fixture and fails until it is raised.
constexpr uint32_t Algorithm = 1;

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

// Selects the cloth triangles by the rules, joins them into islands, builds each surface and takes the islands
// out of the landscape and the chunk table. With a chunk table whose file-backed source uses a cooked index
// (worldchunkcookedsource.h), and every cell's GLB of its index entry's length at load, the result comes from the
// set's cooked bundle when it is there and valid (worldclothbundle.h); otherwise, and on any failure of the
// bundle, from searchIslands. Both give the same result.
Extracted extract(asset::WorldMesh& landscape, ChunkTable* chunks = nullptr);
// The island search alone: what extract computes without a bundle. The cook runs it.
Extracted searchIslands(asset::WorldMesh& landscape, ChunkTable* chunks = nullptr);

enum class Path : uint8_t { None, Search, Bundle };
// The path the latest extract took, on any thread: for tests and measurements.
Path lastPath();

// SHA-256 of the rules text this build compiled in (data/cloth-world.csv).
const std::array<uint8_t,32>& rulesSha256();

}
