#pragma once

// The cooked binary chunk (.wcb): one cell of the stitched world mesh, beside
// its GLB with the same stem (chunks/4_-2.glb -> chunks/4_-2.wcb). It holds the
// IDs readWorldChunks resolves, so a reader needs neither a glTF parse nor the
// stitching of the whole world. The GLB stays the authoring source and the
// fallback; world.json does not name the .wcb.
//
// Uncompressed and little-endian. A fixed 144-byte header, a directory of
// 32-byte section descriptors, then 16-byte aligned sections:
//
//   offset size  header field
//        0    4  magic "GWCB"
//        4    4  version (1)
//        8    4  0x01020304 (byte order check)
//       12    4  header size (144)
//       16    8  file size
//       24    8  cell x, z (i32 each)
//       32   16  vertex, feature, triangle and material counts (u32 each)
//       48   24  bounds min, max (f32 x3 each): the cell's ChunkTable entry
//       72    8  byte length of the GLB the chunk was cooked from
//       80   32  SHA-256 of that GLB
//      112    8  size of the source .ZEN (world.json "source")
//      120    4  its CRC32
//      124    4  section count
//      128    8  directory offset
//      136    8  zero
//
//   directory entry: type u32, stride u32, count u32, zero u32, offset u64, length u64
//
//   type stride section    record (see the structs below)
//      1     16 positions  x, y, z f32; id u32
//      2     28 features   u, v f32; light u32; normal x, y, z f32; id u32
//      3     44 triangles  vertex u32 x3; feature u32 x3; material, order, rank u32; lightmap i32;
//                          flags u16; sector i16
//      4     88 materials  id u32; name, texture, detail object (offset, size u32); twelve f32;
//                          twelve u8 (see Material)
//      5      1 strings    bytes the materials' names point into
//
// The structs only view these bytes: their layouts are checked at compile time
// against the table above, and the writer stores field by field.

#include "worldchunkmanifest.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wcb {

constexpr uint32_t Version    = 1;
constexpr uint32_t HeaderSize = 144;
constexpr uint32_t Added      = 0xFFFFFFFFu; // Triangle::order of geometry an edit added

struct Position {
  float    x, y, z;   // engine centimetres
  uint32_t id;        // the stitched mesh's vertex index
  };

struct Feature {
  float    u, v;
  uint32_t light;
  float    nx, ny, nz;
  uint32_t id;        // the stitched mesh's feature index
  };

// The array index is the triangle's ordinal in its GLB traversal (ChunkTable::triangleSources).
struct Triangle {
  uint32_t vertex[3];  // into this chunk's positions
  uint32_t feature[3]; // into this chunk's features
  uint32_t material;   // the stitched mesh's material index
  uint32_t order;      // the GLB's source triangle index, or Added
  uint32_t rank;       // index in readWorldChunks' stitched mesh
  int32_t  lightmap;
  uint16_t flags;      // PolygonFlagSet bits without the sector, as the GLB "flags" accessor packs them
  int16_t  sector;
  };

struct StringRef {
  uint32_t offset, size; // into the string section
  };

// Every field of asset::Material, enums as their numeric values.
struct Material {
  uint32_t  id;        // the stitched mesh's material index
  StringRef name, texture, detailObject;
  float     smoothAngle, textureScale[2], textureAnimFps, textureAnimMapDir[2], detailObjectScale,
            environmentMappingStrength, waveMaxAmplitude, waveGridSize, defaultMapping[2];
  uint8_t   group, color[4], textureAnimMapMode, waveMode, waveSpeed, alphaFunc;
  uint8_t   switches;  // MaterialSwitch bits
  uint8_t   zero[2];
  };

enum MaterialSwitch : uint8_t {
  DisableCollision   = 1,
  DisableLightmap    = 2,
  DontCollapse       = 4,
  ForceOccluder      = 8,
  EnvironmentMapping = 16,
  IgnoreSun          = 32,
  };

struct Header {
  ChunkId                cell;
  uint32_t               vertices = 0, features = 0, triangles = 0, materials = 0;
  asset::Vec3            min {}, max {};
  uint64_t               glbSize = 0;
  std::array<uint8_t,32> glbSha256 {};
  uint64_t               worldSize = 0;
  uint32_t               worldCrc32 = 0;
  };

uint16_t packFlags(const asset::PolygonFlagSet& flags);
asset::PolygonFlagSet unpackFlags(uint16_t flags, int16_t sector);

}

// What the writer stores; the counts of the header come from the arrays.
struct CookedChunkData {
  wcb::Header                                     header;
  std::vector<wcb::Position>                      positions;
  std::vector<wcb::Feature>                       features;
  std::vector<wcb::Triangle>                      triangles;
  std::vector<std::pair<uint32_t,asset::Material>> materials; // stitched index and record, ascending
  };

std::vector<uint8_t> encodeCookedChunk(const CookedChunkData& data);

// A validated view over the bytes of one .wcb. Opening checks the header, the
// directory and every section once: bounds, strides, counts, the local indices
// of the triangles and the strings of the materials. The spans then point into
// the bytes, which owner keeps alive (copies share it).
class CookedChunk {
  public:
    // Throws std::runtime_error "<name>: <section>: <reason>".
    static CookedChunk open(std::span<const uint8_t> bytes, const std::string& name,
                            std::shared_ptr<const void> owner = {});
    // One read of the file, then open.
    static CookedChunk read(const std::filesystem::path& path);

    const wcb::Header&             header()    const { return head; }
    std::span<const wcb::Position> positions() const { return positionSpan; }
    std::span<const wcb::Feature>  features()  const { return featureSpan; }
    std::span<const wcb::Triangle> triangles() const { return triangleSpan; }
    std::span<const wcb::Material> materials() const { return materialSpan; }
    std::string_view               string(const wcb::StringRef& ref) const;
    asset::Material                material(size_t i) const;
    size_t                         byteSize() const { return bytes.size(); }

  private:
    std::shared_ptr<const void>    owner;
    std::span<const uint8_t>       bytes;
    wcb::Header                    head;
    std::span<const wcb::Position> positionSpan;
    std::span<const wcb::Feature>  featureSpan;
    std::span<const wcb::Triangle> triangleSpan;
    std::span<const wcb::Material> materialSpan;
    std::span<const uint8_t>       stringSpan;
  };

// Empty when glb is the file the chunk was cooked from: the same byte length,
// and with hash also the same SHA-256. Otherwise the reason, naming the GLB.
std::string cookedChunkStale(const CookedChunk& chunk, const std::filesystem::path& glb, bool hash);

// SHA-256 of a file's bytes, raw.
std::array<uint8_t,32> sha256Bytes(std::span<const uint8_t> bytes);
