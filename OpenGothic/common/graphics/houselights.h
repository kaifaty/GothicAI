#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "graphics/windowglow.h"

// One state of a house (reports/render-features/2026-11-15-night-interiors, step 2): a record of a file of
// worlds/<WORLD>/houses.d/ in the content names a house by its box and the hour it goes to bed. The lamps inside
// the box go out one after another before that hour and come back in the morning (graphics/houselamps.h); the lit
// windows on its walls fade with them (shader/materials/window_glow.glsl), so a lit window means a lamp burns
// inside. A window or a lamp outside every box lives as before. Keep the numbers here and in the shader in step.
namespace HouseLights {

constexpr size_t MaxHouses   = 32;    // the scene block carries this many (scene.glsl HOUSE_MAX)
constexpr float  WindowWall  = 50.f;  // cm: a window this far outside the box is still the house's (a jettied floor)
constexpr float  LampWall    = 20.f;  // cm: a lamp is the house's this deep inside the box; a lantern on the facade is not
constexpr float  BedFrom     = 23.f;  // a house without `until` goes to bed between 23:00 and 00:30, by its key
constexpr float  BedSpread   = 1.5f;
constexpr float  SnuffSpread = 0.15f; // the lamps go out one after another over this long before the bedtime
constexpr float  WakeFrom    = 29.5f; // 05:30: the lamps come back in the morning, by the key within the hour
constexpr float  WakeSpread  = 1.f;
constexpr float  LampWake    = 0.1f;  // ... one after another over this long

struct House final {
  std::string          key;
  std::array<float,3>  centre = {};
  std::array<float,3>  half   = {};
  float                yaw    = 0;   // radians round Y: the box's X axis is (cos,0,sin) in the world
  float                until  = -1;  // the record's bedtime, 18..36 (24 is midnight); below 0: by the key
  float                bed    = 0;   // the hour the last lamp goes out, on the evening's scale; Dawn and later: never
  float                wake   = 0;
  };

inline uint32_t hash(std::string_view s) {
  uint32_t h = 2166136261u;
  for(const char c:s)
    h = (h^uint8_t(c))*16777619u;
  return h;
  }

inline uint32_t mix(uint32_t x) {
  x ^= x >> 15; x *= 0x2C1B3C6Du;
  x ^= x >> 12; x *= 0x297A2D39u;
  x ^= x >> 15;
  return x;
  }

inline float number(uint32_t h, uint32_t n) {
  return float(mix(h + n*0x9E3779B1u) & 0xFFFFu)/65536.f;
  }

// the house's evening from its record and its key
inline void settle(House& h) {
  const uint32_t k = hash(h.key);
  h.bed  = h.until>=0.f ? h.until : BedFrom + number(k,0)*BedSpread;
  h.wake = WakeFrom + number(k,1)*WakeSpread;
  }

inline bool allNight(const House& h) {
  return h.bed>=WindowGlow::Dawn;
  }

inline bool inside(const House& h, const float p[3], float margin) {
  const float dx = p[0]-h.centre[0], dy = p[1]-h.centre[1], dz = p[2]-h.centre[2];
  const float c  = std::cos(h.yaw), s = std::sin(h.yaw);
  const float lx = dx*c + dz*s, lz = dz*c - dx*s;
  return std::fabs(lx)<=h.half[0]+margin && std::fabs(dy)<=h.half[1]+margin && std::fabs(lz)<=h.half[2]+margin;
  }

// the first house a point belongs to; -1: none
inline int houseAt(const std::vector<House>& houses, const float p[3], float margin) {
  for(size_t i=0; i<houses.size(); ++i)
    if(inside(houses[i],p,margin))
      return int(i);
  return -1;
  }

// A lamp of the house: r is its turn in [0,1], the lamp with 0 goes out last, at the bedtime, and comes back first
inline bool lampOn(const House& h, float r, float dayHour) {
  if(allNight(h))
    return true;
  const float e = WindowGlow::eveningHour(dayHour);
  return !(e>=h.bed-r*SnuffSpread && e<h.wake+r*LampWake);
  }

// What is left of the light of the house's windows, 0..1: they fade as the lamps go out (windowHouse of
// window_glow.glsl)
inline float windowShare(const House& h, float dayHour) {
  if(allNight(h))
    return 1.f;
  return 1.f - WindowGlow::smoothstep(h.bed-SnuffSpread, h.bed, WindowGlow::eveningHour(dayHour));
  }

// The scene block's three vectors of a house: the centre and cos, the half size with the windows' margin and sin,
// the bedtime
inline void pack(const House& h, float out[12]) {
  out[0] = h.centre[0]; out[1] = h.centre[1]; out[2] = h.centre[2]; out[3] = std::cos(h.yaw);
  out[4] = h.half[0]+WindowWall; out[5] = h.half[1]+WindowWall; out[6] = h.half[2]+WindowWall; out[7] = std::sin(h.yaw);
  out[8] = allNight(h) ? 1.e6f : h.bed; out[9] = 0; out[10] = 0; out[11] = 0;
  }

// A file of houses.d: {"format":1,"houses":[{"key":"..","center":[x,y,z],"size":[x,y,z],"yaw_deg":0,"until":23.5}]}, cm.
// `yaw_deg` and `until` may be left out. False with the reason on anything else; out is then untouched.
// Value: rapidjson's
template<class Value>
bool parse(const Value& doc, std::vector<House>& out, std::string& error) {
  auto fail = [&](std::string text) { error = std::move(text); return false; };
  if(!doc.IsObject() || !doc.HasMember("format") || !doc["format"].IsInt() || doc["format"].GetInt()!=1)
    return fail("expected an object with format 1");
  if(!doc.HasMember("houses") || !doc["houses"].IsArray())
    return fail("expected the array `houses`");
  const auto& list = doc["houses"];
  if(list.Size()>MaxHouses)
    return fail("more houses than the scene block carries ("+std::to_string(MaxHouses)+")");
  std::vector<House> houses;
  for(const auto& e:list.GetArray()) {
    House h;
    if(!e.IsObject() || !e.HasMember("key") || !e["key"].IsString() || e["key"].GetStringLength()==0)
      return fail("a house needs a key");
    h.key = e["key"].GetString();
    const std::string at = "house "+h.key+": ";
    for(const auto& other:houses)
      if(other.key==h.key)
        return fail(at+"the key repeats");
    auto vec3 = [&](const char* field, std::array<float,3>& dst, bool positive) {
      if(!e.HasMember(field) || !e[field].IsArray() || e[field].Size()!=3)
        return false;
      for(unsigned i=0; i<3; ++i) {
        const auto& v = e[field][i];
        if(!v.IsNumber() || !std::isfinite(v.GetDouble()) || (positive && !(v.GetDouble()>0)))
          return false;
        dst[i] = float(v.GetDouble());
        }
      return true;
      };
    std::array<float,3> size = {};
    if(!vec3("center",h.centre,false))
      return fail(at+"center must be three numbers");
    if(!vec3("size",size,true))
      return fail(at+"size must be three numbers above 0");
    h.half = {size[0]*0.5f, size[1]*0.5f, size[2]*0.5f};
    if(e.HasMember("yaw_deg")) {
      if(!e["yaw_deg"].IsNumber() || !std::isfinite(e["yaw_deg"].GetDouble()))
        return fail(at+"yaw_deg must be a number");
      h.yaw = float(e["yaw_deg"].GetDouble()*3.14159265358979323846/180.0);
      }
    if(e.HasMember("until")) {
      const auto& u = e["until"];
      if(!u.IsNumber() || !(u.GetDouble()>=WindowGlow::MinUntil) || !(u.GetDouble()<=WindowGlow::MaxUntil))
        return fail(at+"until must be an hour in 18..36");
      h.until = float(u.GetDouble());
      }
    settle(h);
    houses.push_back(std::move(h));
    }
  out = std::move(houses);
  return true;
  }

// The houses of one more file after those of the files before it: the keys stay unique and the scene block's
// count holds. False with the reason; houses is then untouched
inline bool append(std::vector<House>& houses, const std::vector<House>& more, std::string& error) {
  if(houses.size()+more.size()>MaxHouses) {
    error = "more houses than the scene block carries ("+std::to_string(MaxHouses)+")";
    return false;
    }
  for(const auto& h:more)
    for(const auto& other:houses)
      if(other.key==h.key) {
        error = "house "+h.key+": the key is taken by a file before this one";
        return false;
        }
  houses.insert(houses.end(), more.begin(), more.end());
  return true;
  }

}
