#pragma once

#include "worldchunks.h"

#include <array>
#include "asset/worldtypes.h"

// Non-mesh .ZEN data, shared by the converter and the engine base loader.
// Coordinates are Gothic centimetres. See docs/asset-migration.md.
namespace WorldData {

struct Triangle { std::array<zenkit::Vec3,3> point; };
struct Room {
  zenkit::Vec3 min, max;
  std::vector<Triangle> triangles;
  };

// One volume per BSP sector, including empty rooms, in source order.
bool isIndoor(asset::BspTreeType mode, const std::vector<Room>& rooms, const zenkit::Vec3& point);

struct Data {
  WorldChunkSource source;
  std::string name;
  zenkit::AxisAlignedBoundingBox bbox = zenkit::AxisAlignedBoundingBox::zero();
  std::vector<std::shared_ptr<asset::VirtualObject>> vobs;
  std::shared_ptr<asset::WayNet> waynet;
  // Raw mesh polygon references are omitted, as specified for bsp.bin.
  asset::BspTree bsp {};
  std::vector<Room> rooms;
  };

void write(const Data& data, const std::filesystem::path& dir);
Data read(const std::filesystem::path& dir, const std::filesystem::path& overrideDir = {});
// Complete own object records, references and ordered waypoint/edge comparison.
// Individual retained BSP fields and room floats.
// Also samples isIndoor on a grid of at most 400 cm spacing at three heights.
// Empty on equality, otherwise includes the first differing field and values.
std::string compare(const Data& expected, const Data& actual);
size_t vobCount(const std::vector<std::shared_ptr<asset::VirtualObject>>& vobs);

struct ZenFile {
  WorldChunkSource source;
  // "world" for meshless bundles, "cameras", or "light_presets".
  std::string kind;
  std::vector<std::shared_ptr<asset::VirtualObject>> vobs;
  std::shared_ptr<asset::WayNet> waynet;
  std::vector<asset::LightPreset> light_presets;
  // Only for kind "cutscene_library" (OU.BIN/OU.DAT): the library's roots in archive order, before
  // CutsceneLibrary::load sorts them. Other kinds never write or read this member.
  std::vector<std::shared_ptr<asset::CutsceneBlock>> blocks;
  };

void writeZen(const ZenFile& data, const std::filesystem::path& file);
ZenFile readZen(const std::filesystem::path& file);
// readZen from bytes the caller already holds; `what` names them in errors.
ZenFile parseZen(const std::vector<uint8_t>& bytes, const std::string& what);
std::string compareZen(const ZenFile& expected, const ZenFile& actual);


}
