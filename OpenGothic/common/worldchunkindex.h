#pragma once

// The index of a cooked world (chunks.wci): what readWorldChunks' stitched mesh
// and chunk table need beyond the cells' own .wcb files (worldchunkbinary.h),
// that is everything that depends on the whole world and override set. One
// file per world and override set: in the world directory without an override
// directory, in the override directory with one, in --out when that is given
// (cookedIndexPath, worldchunkcook.h).
//
// With the .wcb files, world.json and unreferenced.bin it rebuilds the stitched
// mesh:
//   - a source triangle with GLB order o has the rank o - (missing orders below
//     o); the missing orders are those below the highest held that no cell holds;
//   - the k-th Added triangle of cell i, by GLB ordinal, has the rank
//     addedStart(i) + k, so Added triangles follow the source ones in cell order;
//   - a position, feature or material of a cell has its source key as stitched
//     id unless the cell's exceptions give another one; geometry and materials
//     without a source key always have an exception;
//   - the materials past world.json's are the appended ones, in stitched order.
// A world without edits has no missing orders, exceptions or appended materials.
//
// Little-endian, in the style of .wcb: an 80-byte header, a directory of
// 32-byte entries (worldchunkbinary.h), 16-byte aligned sections.
//
//   offset size  header field
//        0    4  magic "GWCI"
//        4    4  version (1)
//        8    4  0x01020304 (byte order check)
//       12    4  header size (80)
//       16    8  file size
//       24    8  size of the source .ZEN (world.json "source")
//       32    4  its CRC32
//       36    4  cell count, as world.json lists them
//       40   16  the stitched mesh's vertices, features, materials and triangles (u32 each)
//       56    4  source triangles: the rank of the first Added triangle
//       60    4  section count
//       64    8  directory offset
//       72    8  zero
//
//   type stride section
//      1     80 cells                x, z i32; GLB length u64; GLB SHA-256; addedStart u32;
//                                    vertex, feature, material exceptions (first, count u32 each); zero u32
//      2      4 missing orders       u32, ascending
//      3      8 vertex exceptions    local, id u32; grouped by cell in cell order, local ascending
//      4      8 feature exceptions   the same
//      5      8 material exceptions  the same
//      6     88 materials            the appended materials: the .wcb material record, source None
//      7      1 strings              bytes their names point into

#include "worldchunkbinary.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace wci {

constexpr uint32_t    Version    = 1;
constexpr uint32_t    HeaderSize = 80;
constexpr const char* FileName   = "chunks.wci";

struct Range {
  uint32_t first, count; // into an exception section
  };

struct Cell {
  int32_t  x, z;
  uint64_t glbSize;       // the GLB the cell's numbers were computed from: its .wcb header has the same
  uint8_t  glbSha256[32];
  uint32_t addedStart;    // the stitched rank of the cell's first Added triangle
  Range    vertices, features, materials;
  uint32_t zero;
  };

// A local index of a cell whose stitched id is not its source key.
struct Exception {
  uint32_t local, id;
  };

struct Header {
  uint64_t worldSize = 0;
  uint32_t worldCrc32 = 0;
  uint32_t cells = 0;
  uint32_t vertices = 0, features = 0, materials = 0, triangles = 0;
  uint32_t sourceTriangles = 0;
  };

}

// What the writer stores; the cell count comes from the array.
struct CookedIndexData {
  wci::Header                  header;
  std::vector<wci::Cell>       cells;
  std::vector<uint32_t>        missing;
  std::vector<wci::Exception>  vertices, features, materials; // ranges as the cells give them
  std::vector<asset::Material> appended;
  };

std::vector<uint8_t> encodeCookedIndex(const CookedIndexData& data);

// A validated view over the bytes of one index. Opening checks the header, the
// directory, every section and the cells' ranges once: exceptions grouped by
// cell and ascending, every stitched id below the totals, Added starts in cell
// order. The spans then point into the bytes, which owner keeps alive.
class CookedIndex {
  public:
    // Throws std::runtime_error "chunk index <name>: <part>: <reason>".
    static CookedIndex open(std::span<const uint8_t> bytes, const std::string& name,
                            std::shared_ptr<const void> owner = {});
    static CookedIndex read(const std::filesystem::path& path);

    const wci::Header&               header() const { return head; }
    std::span<const wci::Cell>       cells()  const { return cellSpan; }
    std::span<const uint32_t>        missing() const { return missingSpan; }
    std::span<const wci::Exception>  vertexExceptions(size_t cell) const;
    std::span<const wci::Exception>  featureExceptions(size_t cell) const;
    std::span<const wci::Exception>  materialExceptions(size_t cell) const;
    size_t                           appendedMaterials() const { return materialSpan.size(); }
    asset::Material                  appended(size_t i) const;
    // The stitched rank of a source triangle of this GLB order, or wcb::None
    // when the index places no held triangle there.
    uint32_t                         sourceRank(uint32_t order) const;
    // The rank after cell's last Added triangle: the next cell's addedStart, or the triangle total.
    uint32_t                         addedEnd(size_t cell) const;
    const std::string&               name() const { return label; }
    size_t                           byteSize() const { return bytes.size(); }

    // The stitched id of a local index: its exception's id, else its source key (wcb::None if it has none).
    static uint32_t resolve(std::span<const wci::Exception> exceptions, uint32_t local, uint32_t source);

  private:
    std::shared_ptr<const void>     owner;
    std::span<const uint8_t>        bytes;
    std::string                     label;
    wci::Header                     head;
    std::span<const wci::Cell>      cellSpan;
    std::span<const uint32_t>       missingSpan;
    std::span<const wci::Exception> exceptionSpan[3];
    std::span<const wcb::Material>  materialSpan;
    std::span<const uint8_t>        stringSpan;
  };
