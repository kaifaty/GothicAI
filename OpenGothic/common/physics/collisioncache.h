#pragma once

#include "physics.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

class StaticCollision;

// The static collision of one world in one derived file, <cache root>/derived/<base>/<WORLD>.collision: the Jolt
// shapes of the landscape and water bodies and the CPU data their queries read (staticcollision.h), from one build,
// written whole to a temporary name and renamed. Jolt's sub-shape ids name triangles of these index lists, so the
// two parts are never mixed from two builds. On a hit nothing is built from a mesh.
//
// Byte table (native endianness; the identity names it, with the Jolt version and word sizes):
//   0    19  magic "GOTHIC-COLLISION-2\n" (version 1, "GOTHIC-COLLISION-1\n", held the shapes alone)
//   19   64  identity: SHA-256 hex of the key and the build
//   83    5  zero
//   88    8  checksum: 64-bit XXH64 of every byte after the header (the payload)
//   96   12 × 8 section sizes, as counts: sector name bytes, sector names, vertices; for the land and then the
//            water mesh: indices, segments, duplicate groups, duplicate members; last, the shapes' bytes
//   192      the payload, in that order:
//            sector names   per name: u32 length, then its bytes
//            vertices       x, y, z float metres (PhysicVbo's vertices, shared by both meshes)
//            indices        u32, three per triangle, in PhysicVbo's order (winding as addIndex leaves it)
//            segments       u64 index offset, u64 triangle count, u32 material group, u32 sector name (~0: none)
//            groups         u32 member count of each ray duplicate group (PhysicVbo::prepareTriangleData)
//            members        u32 triangle ids of the groups, in their order
//            shapes         Shape::SaveWithChildren of the land shape, then the water shape (~0u: none)
// A file is used only when the magic, the identity, the file size against the sizes and the checksum hold, every
// index is below the vertex count, the segments tile their index list, every sector reference names a sector,
// every duplicate member names a triangle of its mesh once, and Jolt restores the shapes. Anything else is a miss
// with one log line "[collision-cache] <file>: <reason>; rebuilding from the collision input", never a failed load.
class CollisionCache final {
  public:
    static constexpr uint32_t BakeVersion = 2;
    static constexpr size_t   HeaderSize  = 192;
    static constexpr std::string_view Magic = "GOTHIC-COLLISION-2\n";
    struct Key {
      std::string base, world, content;
      uint32_t    collisionFlags = 0;
      std::string clothRules; // WorldCloth::rulesSha256 and Algorithm: they decide which triangles leave the collision
      uint32_t    version = BakeVersion;
      };

    // No path (nothing is read or written) with OPENGOTHIC_COLLISION_CACHE=0, no root, or an incomplete key.
    CollisionCache(const std::filesystem::path& cacheRoot, const Key& key);
    // Fills out's sectors, vertices, land and water meshes (their triangle data prepared) and shapes and returns
    // true. A missing file is a miss without a log line, unless leftover temporary files of an interrupted write
    // lie beside it; out is left as it was on any miss.
    bool read(StaticCollision& out) const;
    void save(const StaticCollision& in) const;
    const std::filesystem::path& path() const { return file; }

    // The payload checksum (for contracts that edit a file and keep it consistent).
    static uint64_t checksum(const void* data, size_t size);

  private:
    std::filesystem::path file;
    std::string identity;
  };
