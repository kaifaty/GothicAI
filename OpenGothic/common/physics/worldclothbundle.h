#pragma once

// The world cloth bundle (cloth.wcl): WorldCloth::searchIslands' result for one world and override set, cooked
// once beside the set's index (docs/landscape-plan.md, Д3, task 4). Cloth islands cross cell borders, so the
// search needs the stitched mesh of the whole world; the bundle lets the game take the result without one. It is
// written for every cooked world, also with no cloth (zero surfaces), and holds:
//   - the surfaces in searchIslands' order and the original mesh (the removed triangles, the cloth=0 fallback);
//   - per cell in world.json order, the ascending cell-local triangle ordinals taken out as cloth: the ordinals of
//     ChunkTable::triangleSources and the triangle index of the cell's .wcb;
//   - since version 2, what the search leaves of the landscape, as the chunk table and the landscape hold it after
//     WorldCloth::extract: per cell its triangle count and bounds, the landscape's bounds (PackedMesh::landscapeBbox)
//     and the stitched ids of the materials its triangles use, in first use. The catalogue start
//     (worldchunkcatalogue.h) builds the chunk table and the landscape view's start from these, with no geometry.
//     They depend on the rules as the exclusions do, so they live here and not in the index.
// It is a function of the set, the rules and the algorithm: its header names the source world, the SHA-256 of
// the index file's bytes it was cooked with, the SHA-256 of the rules text and WorldCloth::Algorithm. The one
// exception is the mesh name: searchIslands copies the landscape's (worldbase.cpp sets it from sectors.json, which
// neither the cook's readWorldChunks nor the index sees), so the bundle stores none and the reader gives every
// non-empty mesh the landscape's name, as searchIslands does.
//
// After a catalogue start (worldchunkcatalogue.h) the start has checked the bundle and built the chunk table from
// what it leaves, and the landscape holds no triangles: takeClothBundle returns the bundle's surfaces and original
// mesh and changes nothing. Otherwise WorldCloth::extract takes the bundle (takeClothBundle) when the chunk table's
// source uses a cooked index (worldchunkcookedsource.h) and every cell's GLB had at load its index entry's length,
// and the bundle lies beside that index, opens, names the same source world, carries the SHA-256 of the index bytes
// the source read and this build's rules hash and algorithm, and its exclusions name triangles the loaded table
// holds, as many as its original mesh has. Anything else runs the island search, with one log line naming the bundle
// and the reason; a missing bundle is not logged. OPENGOTHIC_WCB=0 uses no index, so no bundle. As for the cells, a
// GLB changed at the same length after the cook is not noticed (the game hashes no GLB); content-convert
// verify-cooked-world compares the bundle with a fresh island search and finds it.
//
// Little-endian, in the style of .wcb and the index (worldchunkbinary.h): a 160-byte header, a directory of
// 32-byte entries, 16-byte aligned sections.
//
//   offset size  header field
//        0    4  magic "GWCL"
//        4    4  version (2)
//        8    4  0x01020304 (byte order check)
//       12    4  header size (160)
//       16    8  file size
//       24    8  size of the source .ZEN (world.json "source")
//       32    4  its CRC32
//       36    4  cell count, as world.json lists them
//       40    4  surface count
//       44    4  WorldCloth::Algorithm
//       48   32  SHA-256 of the index file's bytes the bundle was cooked with
//       80   32  SHA-256 of the rules text (data/cloth-world.csv)
//      112    4  section count
//      116    4  zero
//      120    8  directory offset
//      128    8  zero
//      136   24  the landscape's bounds after the search: min, max f32x3 (zero when it has no triangle)
//
//   type stride section
//      1     32 surfaces    id u32; stiffness, linear damping f32; visual (offset, size u32); pins (first, count u32);
//                           zero u32
//      2     56 meshes      bounds min, max f32x3; vertices, features, triangles, materials (first, count u32 each);
//                           one per surface in order, then the original mesh; ranges back to back
//      3      4 pins        u32, into the surface mesh's vertices
//      4     12 vertices    x, y, z f32
//      5     24 features    u, v f32; light u32; normal x, y, z f32
//      6     36 triangles   vertex u32 x3, feature u32 x3, material u32 (into the mesh's own arrays); lightmap i32;
//                           flags u16, sector i16 (as .wcb packs them)
//      7     88 materials   the .wcb material record, source None
//      8     40 cells       per cell in world.json order: exclusions (first, count u32), back to back; the triangles
//                           the search leaves u32; zero u32; their bounds min, max f32x3 (the chunk table entry's)
//      9      4 exclusions  u32 cell-local triangle ordinals, ascending within a cell
//     10      1 strings     bytes the visuals and the materials' names point into
//     11      4 landscape materials  u32 stitched material ids of the triangles the search leaves, in first use

#include "physics/worldcloth.h"
#include "worldchunkbinary.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

struct ChunkTable;
struct CookedSet;

namespace wcl {

constexpr uint32_t    Version    = 2;
constexpr uint32_t    HeaderSize = 160;
constexpr const char* FileName   = "cloth.wcl";

struct Range {
  uint32_t first, count;
  };

// A cell: its exclusions and what the search leaves of it.
struct Cell {
  Range    exclusions;
  uint32_t triangles, zero;
  float    min[3], max[3];
  };

struct Surface {
  uint32_t       id;
  float          stiffness, linearDamping;
  wcb::StringRef visual;
  Range          pins;
  uint32_t       zero;
  };

struct Mesh {
  float min[3], max[3];
  Range vertices, features, triangles, materials;
  };

struct Vertex {
  float x, y, z;
  };

struct Feature {
  float    u, v;
  uint32_t light;
  float    nx, ny, nz;
  };

struct Triangle {
  uint32_t vertex[3], feature[3], material;
  int32_t  lightmap;
  uint16_t flags;
  int16_t  sector;
  };

struct Header {
  uint64_t               worldSize = 0;
  uint32_t               worldCrc32 = 0;
  uint32_t               cells = 0, surfaces = 0;
  uint32_t               algorithm = 0;
  std::array<uint8_t,32> indexSha256 {}, rulesSha256 {};
  asset::Vec3            landscapeMin {}, landscapeMax {};
  };

}

// What the search leaves of a cell: its chunk table entry's count and bounds after WorldCloth::extract.
struct ClothBundleCell {
  uint32_t    triangles = 0;
  asset::Vec3 min {}, max {};
  };

// What the writer stores; the counts of the header come from the arrays. Mesh names are not stored.
struct ClothBundleData {
  wcl::Header                        header;
  WorldCloth::Extracted              cloth;
  std::vector<std::vector<uint32_t>> exclusions; // per cell in world.json order, ascending
  std::vector<ClothBundleCell>       cells;      // per cell in world.json order
  std::vector<uint32_t>              landscapeMaterials; // stitched ids, in first use
  };

std::vector<uint8_t> encodeClothBundle(const ClothBundleData& data);

// A validated view over the bytes of one bundle. Opening checks the header, the directory and every section once:
// mesh ranges back to back, every local index of a triangle and every pin inside its mesh, the strings, the cells'
// exclusion ranges back to back and ascending, each ordinal below the cell's triangles with its exclusions, the
// landscape materials unique. The spans then point into the bytes, which owner keeps alive.
class ClothBundle {
  public:
    // Throws std::runtime_error "cloth bundle <name>: <part>: <reason>".
    static ClothBundle open(std::span<const uint8_t> bytes, const std::string& name,
                            std::shared_ptr<const void> owner = {});
    static ClothBundle read(const std::filesystem::path& path);

    const wcl::Header&        header() const { return head; }
    size_t                    surfaces() const { return surfaceSpan.size(); }
    // Surface i and the original mesh as searchIslands built them, with name as every mesh's name.
    WorldCloth::Surface       surface(size_t i, const std::string& name = {}) const;
    asset::WorldMesh          original(const std::string& name = {}) const;
    size_t                    originalTriangles() const;
    size_t                    cells() const { return cellSpan.size(); }
    std::span<const uint32_t> exclusions(size_t cell) const;
    // What the search leaves of a cell, and of the landscape (its materials as stitched ids, in first use).
    ClothBundleCell           cell(size_t cell) const;
    std::span<const uint32_t> landscapeMaterials() const { return landscapeMaterialSpan; }
    ClothBundleData           data() const;
    const std::string&        name() const { return label; }
    size_t                    byteSize() const { return bytes.size(); }

  private:
    std::shared_ptr<const void>     owner;
    std::span<const uint8_t>        bytes;
    std::string                     label;
    wcl::Header                     head;
    std::span<const wcl::Surface>   surfaceSpan;
    std::span<const wcl::Mesh>      meshSpan;
    std::span<const uint32_t>       pinSpan;
    std::span<const wcl::Vertex>    vertexSpan;
    std::span<const wcl::Feature>   featureSpan;
    std::span<const wcl::Triangle>  triangleSpan;
    std::span<const wcb::Material>  materialSpan;
    std::span<const wcl::Cell>      cellSpan;
    std::span<const uint32_t>       exclusionSpan;
    std::span<const uint8_t>        stringSpan;
    std::span<const uint32_t>       landscapeMaterialSpan;

    asset::WorldMesh mesh(size_t i, const std::string& name) const;
  };

// The bundle of a set: beside its index (cookedIndexPath, worldchunkcook.h).
std::filesystem::path cookedClothPath(const std::filesystem::path& dir, const std::filesystem::path& overrideDir,
                                      const std::filesystem::path& outDir = {});

// The cook's part: runs WorldCloth::searchIslands on readWorldChunks' stitched mesh and chunk table (both are
// changed as by extract) and returns what the bundle stores for the set of that source world and index bytes.
ClothBundleData cookClothBundle(asset::WorldMesh& mesh, ChunkTable& table, const WorldChunkSource& source,
                                const std::array<uint8_t,32>& indexSha256);

// Throws, naming the reason, unless the bundle names set's source world and index bytes, has as many cells, and
// carries this build's rules hash and algorithm.
void checkClothBundle(const ClothBundle& bundle, const CookedSet& set);
// The result of the bundle on the loaded landscape and table: throws, before changing anything, unless every
// exclusion names a triangle the table holds and they remove as many as the original mesh has; then takes them
// out with retainWorldTriangles, as searchIslands does.
WorldCloth::Extracted applyClothBundle(const ClothBundle& bundle, asset::WorldMesh& landscape, ChunkTable& table);
// extract's bundle path: the result, or nothing when the island search must run (see above).
std::optional<WorldCloth::Extracted> takeClothBundle(asset::WorldMesh& landscape, ChunkTable& table);

// Empty when equal: every surface (id, visual, stiffness and damping bits, pins, the mesh with its name and
// bounds bit for bit) and the original mesh; otherwise the first difference, naming the surface.
std::string compareWorldCloth(const WorldCloth::Extracted& expected, const WorldCloth::Extracted& actual);
// The same for two bundles' contents, header and exclusions included.
std::string compareClothBundles(const ClothBundleData& expected, const ClothBundleData& actual);
