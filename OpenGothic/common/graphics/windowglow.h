#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

// Windows that glow at night (reports/render-features/2026-11-15-night-interiors): the `window` field of a record in
// materials.json next to `emissive`. The ORM's alpha marks what glows (a pane, the slits of closed shutters, an
// opening); every repeat of the texture is one window with its own evening: whether anyone lights it at all, when
// the candle is lit as the rooms go dark and when it is put out. The shader's side is
// shader/materials/window_glow.glsl; keep the numbers here and there in step.
namespace WindowGlow {

struct Params final {
  bool  on    = false;
  float lit   = 0.6f;   // 0..1: the share of the windows that are lit at all on an evening
  float until = 23.5f;  // 18..36, the hour past which half of them are out; 24 is midnight, Dawn and later: all night
  bool  operator==(const Params&) const = default;
  };

// Hours of the evening: a day's hour before noon counts on past midnight (03:00 is 27)
constexpr float OnFrom    = 21.25f; // the first candles: the rooms go dark as the sun sets (21:33, sky.cpp)
constexpr float OnSpread  = 1.0f;   // the last ones are lit this much later, in the dark
constexpr float OffSpread = 2.0f;   // the windows go out within this many hours around `until`
constexpr float Fade      = 0.2f;   // a window comes up and goes out over this long
constexpr float Dawn      = 28.75f; // sunrise (04:45): nothing burns into the day
constexpr float MinUntil  = 18.f;
constexpr float MaxUntil  = 36.f;
constexpr float Dimmest   = 0.55f;  // the dimmest lit window of the brightest

// {} takes the defaults; false on a value of a wrong type or out of its range. Value: rapidjson's
template<class Value>
bool parse(const Value& v, Params& out) {
  if(!v.IsObject())
    return false;
  Params p;
  p.on = true;
  auto number = [&](const char* field, float& dst, double min, double max) {
    const auto it = v.FindMember(field);
    if(it==v.MemberEnd())
      return true;
    if(!it->value.IsNumber())
      return false;
    const double x = it->value.GetDouble();
    if(!(x>=min) || !(x<=max))
      return false;
    dst = float(x);
    return true;
    };
  if(!number("lit",p.lit,0,1) || !number("until",p.until,MinUntil,MaxUntil))
    return false;
  out = p;
  return true;
  }

// Bucket::foliage under BK_WINDOW (windowPacked* of window_glow.glsl): 8 bits of `lit`, 8 of `until`
inline uint32_t pack(const Params& p) {
  if(!p.on)
    return 0;
  auto u8 = [](float v) { return uint32_t(std::lround(std::clamp(v, 0.f, 1.f)*255.f)); };
  return u8(p.lit) | (u8((p.until-MinUntil)/(MaxUntil-MinUntil)) << 8);
  }

inline float packedLit  (uint32_t bits) { return float( bits     & 0xFFu)/255.f; }
inline float packedUntil(uint32_t bits) { return MinUntil + float((bits>>8) & 0xFFu)*((MaxUntil-MinUntil)/255.f); }

inline float eveningHour(float dayHour) {
  return dayHour<12.f ? dayHour+24.f : dayHour;
  }

inline float smoothstep(float a, float b, float x) {
  const float t = std::clamp((x-a)/(b-a), 0.f, 1.f);
  return t*t*(3.f-2.f*t);
  }

// The light of one window, 0..1: r is its three numbers in [0,1) (who lights it, when, when it goes out), dayHour
// the game's hour 0..24. The mirror of windowLight in window_glow.glsl without its slow flicker
inline float light(uint32_t bits, const float r[3], float dayHour) {
  const float lit = packedLit(bits), until = packedUntil(bits);
  if(r[0]>=lit)
    return 0.f;
  const float e   = eveningHour(dayHour);
  const float on  = OnFrom + r[1]*OnSpread;
  float       off = until + (r[2]-0.5f)*OffSpread;
  if(until>=Dawn)
    off = Dawn - r[2]*0.5f;
  off = std::clamp(off, on+0.5f, Dawn);
  const float s = smoothstep(on, on+Fade, e)*(1.f - smoothstep(off, off+Fade, e));
  const float b = r[1]*7.31f + r[2]*3.17f;
  return s*(Dimmest + (1.f-Dimmest)*(b - std::floor(b)));
  }

}
