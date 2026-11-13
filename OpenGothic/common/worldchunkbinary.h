#pragma once

// The cooked binary chunk (.wcb): one cell of the world mesh, beside its GLB
// with the same stem (chunks/4_-2.glb -> chunks/4_-2.wcb). It holds only what
// follows from that GLB and the world's world.json: the source keys the GLB
// carries (_VERTEX, _FEATURE, a material's world.json index), indices into the
// file's own arrays and each triangle's GLB order. What depends on the whole
// world and override set — the stitched ids of moved and added geometry, the
// appended materials, a triangle's place in the stitched mesh — lies in the
// set's index (worldchunkindex.h). So a cell cooked once stays valid when
// other cells or the override set change. The GLB stays the authoring source
// and the fallback; world.json does not name the .wcb.
//
// Uncompressed and little-endian. A fixed 144-byte header, a directory of
// 32-byte section descriptors, then 16-byte aligned sections:
//
//   offset size  header field
//        0    4  magic "GWCB"
//        4    4  version (2)
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
//      1     16 positions  x, y, z f32; source u32
//      2     28 features   u, v f32; light u32; normal x, y, z f32; source u32
//      3     40 triangles  vertex u32 x3; feature u32 x3; material, order u32; lightmap i32; flags u16; sector i16
//      4     88 materials  source u32; name, texture, detail object (offset, size u32); twelve f32;
//                          twelve u8 (see Material)
//      5      1 strings    bytes the materials' names point into
//
// Positions, features and materials are in first use: the order in which the
// GLB's triangles (by ordinal, then corner) first name them. A position or
// feature is one per distinct source key and value, as readWorldChunks keys them.
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
#include <utility>
#include <vector>

namespace wcb {

constexpr uint32_t Version    = 2;
constexpr uint32_t HeaderSize = 144;
constexpr uint32_t None       = 0xFFFFFFFFu; // no source key: geometry an edit added, or a material world.json lacks
constexpr uint32_t Added      = 0xFFFFFFFFu; // Triangle::order of geometry an edit added

struct Position {
  float    x, y, z;   // engine centimetres
  uint32_t source;    // the GLB's _VERTEX, or None
  };

struct Feature {
  float    u, v;
  uint32_t light;
  float    nx, ny, nz;
  uint32_t source;    // the GLB's _FEATURE, or None
  };

// The array index is the triangle's ordinal in its GLB traversal (ChunkTable::triangleSources).
struct Triangle {
  uint32_t vertex[3];  // into this chunk's positions
  uint32_t feature[3]; // into this chunk's features
  uint32_t material;   // into this chunk's materials
  uint32_t order;      // the GLB's source triangle index, or Added
  int32_t  lightmap;
  uint16_t flags;      // PolygonFlagSet bits without the sector, as the GLB "flags" accessor packs them
  int16_t  sector;
  };

struct StringRef {
  uint32_t offset, size; // into the string section
  };

// Every field of asset::Material, enums as their numeric values. The index
// (worldchunkindex.h) stores its appended materials in the same record.
struct Material {
  uint32_t  source;    // the world.json index when world.json holds this record there, else None
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

// ---- what the cooked files share (this chunk and the index, worldchunkindex.h) ----

constexpr uint32_t ByteOrder    = 0x01020304u;
constexpr uint32_t EntrySize    = 32;  // a directory entry
constexpr uint32_t SectionAlign = 16;

inline uint32_t le32(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1])<<8) | (uint32_t(p[2])<<16) | (uint32_t(p[3])<<24);
  }
inline uint64_t le64(const uint8_t* p) { return uint64_t(le32(p)) | (uint64_t(le32(p+4))<<32); }

// "<what> <name>: <part>: <reason>", the error of every rejected cooked file.
[[noreturn]] void reject(const std::string& what, const std::string& name, const std::string& part,
                         const std::string& reason);

// Little-endian writer: a header, the directory, then sections in type order.
class Writer {
  public:
    std::vector<uint8_t> out;

    void u8(uint8_t v) { out.push_back(v); }
    void u16(uint16_t v) { u8(uint8_t(v)); u8(uint8_t(v>>8)); }
    void u32(uint32_t v) { for(unsigned i=0;i<4;++i) u8(uint8_t(v>>(8*i))); }
    void u64(uint64_t v) { for(unsigned i=0;i<8;++i) u8(uint8_t(v>>(8*i))); }
    void i32(int32_t v)  { u32(uint32_t(v)); }
    void f32(float v);
    void put32(size_t at, uint32_t v) { for(unsigned i=0;i<4;++i) out[at+i]=uint8_t(v>>(8*i)); }
    void put64(size_t at, uint64_t v) { for(unsigned i=0;i<8;++i) out[at+i]=uint8_t(v>>(8*i)); }
    // Room for count directory entries at the current end; then section() fills them in type order.
    void directory(uint32_t count);
    // Aligns, writes the body and its directory entry (type is 1-based).
    template<class Body>
    void section(uint32_t type, uint32_t stride, size_t count, const char* name, Body&& body) {
      align();
      const size_t begin = out.size();
      body();
      entry(type,stride,count,name,begin);
      }
    // A material record (88 bytes); its strings go to strings.
    void material(uint32_t source, const asset::Material& m, std::string& strings);
    // The string bytes as a section.
    void strings(uint32_t type, const std::string& bytes);

  private:
    size_t directoryAt = 0;
    void align() { while(out.size()%SectionAlign!=0) u8(0); }
    void entry(uint32_t type, uint32_t stride, size_t count, const char* name, size_t begin);
  };

uint32_t count32(size_t n, const char* what);

struct SectionSpec {
  const char* name;
  uint32_t    stride;
  };
struct SectionView {
  uint64_t offset = 0;
  uint32_t count  = 0;
  };

// Checks what every cooked file opens with — magic, version, byte order mark,
// header size, the file size at offset 16, the alignment in memory its records
// need — and returns the file size.
uint64_t checkPrologue(std::span<const uint8_t> bytes, const char* magic, uint32_t version, uint32_t headerSize,
                       size_t alignment, const std::string& what, const std::string& name);
// Checks the directory and every section once: known and unique types, each
// stride, alignment, bounds and count x stride = length, every spec present.
// Returns one view per spec, spec i being type i+1.
std::vector<SectionView> readDirectory(std::span<const uint8_t> bytes, uint32_t headerSize, uint32_t sections,
                                       uint64_t directory, std::span<const SectionSpec> specs,
                                       const std::string& what, const std::string& name);

template<class T>
std::span<const T> view(std::span<const uint8_t> bytes, const SectionView& section) {
  return {reinterpret_cast<const T*>(bytes.data()+section.offset),section.count};
  }

// A material record back; its strings must lie inside strings (materialStringsFit).
asset::Material readMaterial(const Material& record, std::span<const uint8_t> strings);
bool materialStringsFit(const Material& record, size_t stringBytes);

}

// What the writer stores; the counts of the header come from the arrays.
struct CookedChunkData {
  wcb::Header                                     header;
  std::vector<wcb::Position>                      positions;
  std::vector<wcb::Feature>                       features;
  std::vector<wcb::Triangle>                      triangles;
  std::vector<std::pair<uint32_t,asset::Material>> materials; // source index or None, and record, in first use
  };

std::vector<uint8_t> encodeCookedChunk(const CookedChunkData& data);

// A validated view over the bytes of one .wcb. Opening checks the header, the
// directory and every section once: bounds, strides, counts, the local indices
// of the triangles and the strings of the materials. The spans then point into
// the bytes, which owner keeps alive (copies share it).
class CookedChunk {
  public:
    // Throws std::runtime_error "cooked chunk <name>: <section>: <reason>".
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
    // Triangles an edit added (order Added).
    uint32_t                       added() const { return addedCount; }

  private:
    std::shared_ptr<const void>    owner;
    std::span<const uint8_t>       bytes;
    wcb::Header                    head;
    std::span<const wcb::Position> positionSpan;
    std::span<const wcb::Feature>  featureSpan;
    std::span<const wcb::Triangle> triangleSpan;
    std::span<const wcb::Material> materialSpan;
    std::span<const uint8_t>       stringSpan;
    uint32_t                       addedCount = 0;
  };

// Empty when glb is the file the chunk was cooked from: the same byte length,
// and with hash also the same SHA-256. Otherwise the reason, naming the GLB.
std::string cookedChunkStale(const CookedChunk& chunk, const std::filesystem::path& glb, bool hash);

// SHA-256 of a file's bytes, raw.
std::array<uint8_t,32> sha256Bytes(std::span<const uint8_t> bytes);
