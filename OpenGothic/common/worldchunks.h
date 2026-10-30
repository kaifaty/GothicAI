#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "asset/worldmesh.h"

// The converted world mesh: one .glb per 64 x 64 m chunk plus world.json.
// Format: docs/asset-migration.md#мир. Only the triangle set the engine uses
// is carried (asset::WorldMesh::polygons after the BSP filter): materials,
// vertices, features and polygons. BSP, sectors, portals and raw polygons
// still come from the .ZEN.

struct WorldChunkSource {
  std::string world;       // archive name, e.g. NEWWORLD.ZEN
  uint64_t    size  = 0;   // bytes of the .ZEN the chunks were cut from
  uint32_t    crc32 = 0;
  };

struct WorldChunkManifest {
  WorldChunkSource         source;
  int32_t                  chunkSize = 6400; // centimetres
  std::vector<std::string> chunks;           // file names, relative to the world directory
  uint64_t                 triangles = 0;
  uint64_t                 vertices  = 0;    // size of the source arrays
  uint64_t                 features  = 0;
  uint64_t                 unreferencedVertices = 0;
  uint64_t                 unreferencedFeatures = 0;
  std::vector<asset::Material> materials;  // the full source table
  };

// Writes world.json and chunks/<x>_<z>.glb into dir (created if needed).
// Throws std::runtime_error on invalid input or I/O failure.
WorldChunkManifest writeWorldChunks(const asset::WorldMesh& mesh, const WorldChunkSource& source,
                                    const std::filesystem::path& dir, int32_t chunkSize = 6400);

uint32_t worldChunkCrc32(const void* data, size_t size, uint32_t crc = 0);
