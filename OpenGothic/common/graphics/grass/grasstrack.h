#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

// The cart track's own material (reports/engine/2026-11-15-meadow-road): the `track` field of a road texture's
// record - two layers by world position, the dry crust and the loose earth, and three small maps in the mesh's
// own uv, the ribbon's: where the ruts, the crown and the shoulders are. What has a direction comes from the
// ribbon, what has none lies by the world (shader/grass/grass_track.glsl). The optional `border` of a meadow's
// `ground` is the earth of its notches by such a road: the road's loose layer, one earth on both sides of the
// mesh's edge. Bindless textures only, and only with the grass on: without them the road keeps its paint.
namespace Grass {

struct Track final {
  bool  on        = false;
  float crustTile = 4.f;   // metres of the world per repeat
  float looseTile = 3.f;
  float wander    = 9.f;   // cm: the ribbon's drawing wanders to either side by the world's noise
  float fade      = 0.6f;  // 0..1: how much of their depth and tone the ruts lose where they fade
  float ragged    = 1.f;   // 0..2: the world's noise on the wear - the ruts' edges widen, narrow and stain
  bool  operator==(const Track&) const = default;
  };

constexpr float MaxWander = 30.f;
constexpr float MaxRagged = 2.f;

// What the shader takes in one float (Bucket::foliageReach of a bucket with BK_TRACK): whole numbers under 2^24
inline float pack(const Track& t) {
  const auto step = [](float v, float max, int steps) {
    return uint32_t(std::lround(std::clamp(v/max, 0.f, 1.f)*float(steps)));
    };
  return float(step(t.wander,MaxWander,255) | (step(t.fade,1.f,255) << 8) | (step(t.ragged,MaxRagged,255) << 16));
  }

// The tile of one layer of maps, `tile_m` of its object; the maps are the record's reader's
template<class Value>
bool parseTile(const Value& layer, float& dst) {
  if(!layer.IsObject())
    return false;
  const auto it = layer.FindMember("tile_m");
  if(it==layer.MemberEnd())
    return true;
  if(!it->value.IsNumber())
    return false;
  const double x = it->value.GetDouble();
  if(!(x>=0.25) || !(x<=64))
    return false;
  dst = float(x);
  return true;
  }

// The numbers of `track`; its nine maps are the record's reader's. False on a value of a wrong type or range.
// Value: rapidjson's
template<class Value>
bool parse(const Value& v, Track& out) {
  if(!v.IsObject())
    return false;
  Track t;
  t.on = true;
  const auto crust = v.FindMember("crust"), loose = v.FindMember("loose"), ribbon = v.FindMember("ribbon");
  if(crust==v.MemberEnd() || loose==v.MemberEnd() || ribbon==v.MemberEnd() || !ribbon->value.IsObject() ||
     !parseTile(crust->value,t.crustTile) || !parseTile(loose->value,t.looseTile))
    return false;
  auto number = [&](const char* field, float& dst, double max) {
    const auto it = v.FindMember(field);
    if(it==v.MemberEnd())
      return true;
    if(!it->value.IsNumber())
      return false;
    const double x = it->value.GetDouble();
    if(!(x>=0) || !(x<=max))
      return false;
    dst = float(x);
    return true;
    };
  if(!number("wander",t.wander,MaxWander) || !number("fade",t.fade,1.0) || !number("ragged",t.ragged,MaxRagged))
    return false;
  out = t;
  return true;
  }

}
