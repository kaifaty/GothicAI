#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string_view>

// The grass system (reports/engine/2026-11-14-grass-system): the `grass` field of a ground texture's record in
// materials.json, the switch and the numbers of the ring round the camera. The blades are placed by
// grassplace.h, kept and drawn by grassfield.h, shaped by shader/grass/grass.vert; the ground under them is
// shader/grass/grass_ground.glsl. Keep the numbers here and in shader/grass/grass_common.glsl in step.
namespace Grass {

struct Params final {
  bool  on           = false;
  float density      = 1.f;   // 0..2 of BladesPerM2
  float height       = 20.f;  // 5..60 cm: the main grass; the short one and the clumps are counted from it
  float clumps       = 1.f;   // 0..1: how much of the tall clumps and of the gathering into tufts
  float slope        = 35.f;  // 0..60 degrees: none over it, thinner on the last 10
  float edge         = 150.f; // 0..400 cm: the band at an open edge where the grass is lower and thinner
  float wind         = 1.f;   // 0..4: the lean and the swing, as `foliage`'s
  float translucency = 0.6f;  // 0..1: the share of the sun through a blade
  bool  operator==(const Params&) const = default;
  };

constexpr float BladesPerM2  = 320.f;
constexpr float MaxDensity   = 2.f;
constexpr float MaxHeight    = 60.f;
constexpr float MaxWind      = 4.f;

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
  if(!number("density",p.density,0,MaxDensity) || !number("height",p.height,5,MaxHeight) ||
     !number("clumps",p.clumps,0,1) || !number("slope",p.slope,0,60) || !number("edge",p.edge,0,400) ||
     !number("wind",p.wind,0,MaxWind) || !number("translucency",p.translucency,0,1))
    return false;
  out = p;
  return true;
  }

// Bucket::foliage of the ground under BK_GRASS (grassPacked* of grass_common.glsl): 6 bits each of the density
// (of MaxDensity), the height (cm), the slope (degrees) and the wind (of MaxWind), 8 of the translucency
inline uint32_t pack(const Params& p) {
  if(!p.on)
    return 0;
  auto bits = [](float v, float range, float top) { return uint32_t(std::lround(std::clamp(v/range, 0.f, 1.f)*top)); };
  return bits(p.density,MaxDensity,63.f) | (bits(p.height,63.f,63.f) << 6) | (bits(p.slope,63.f,63.f) << 12) |
         (bits(p.wind,MaxWind,63.f) << 18) | (bits(p.translucency,1.f,255.f) << 24);
  }

// The switch. [INTERNAL] grass=0|1|2 of Gothic.ini is given by the renderer before the first bucket is built:
// 0 off, 1 a smaller and thinner ring, 2 the whole (default). OPENGOTHIC_GRASS overrides it for the benches:
// 0 off, 1 or 2 the quality, `ground` the ground's look without the blades (the pair of the near ground's mark).
enum Mode : uint8_t { Off = 0, Low = 1, Full = 2 };

struct Switch final {
  uint8_t mode   = Full;
  bool    blades = true;
  };

inline Switch& switchState() {
  static Switch s;
  return s;
  }

inline bool envSwitch(Switch& s) {
  const char* e = std::getenv("OPENGOTHIC_GRASS");
  if(e==nullptr)
    return false;
  const std::string_view v = e;
  if(v=="0")
    s = Switch{Off,false};
  else if(v=="1")
    s = Switch{Low,true};
  else if(v=="ground")
    s = Switch{Full,false};
  else
    s = Switch{Full,true};
  return true;
  }

inline void setSwitch(int ini) {
  Switch s;
  s.mode   = ini<=0 ? uint8_t(Off) : (ini==1 ? uint8_t(Low) : uint8_t(Full));
  s.blades = s.mode!=Off;
  envSwitch(s);
  switchState() = s;
  }

inline Switch enabled() {
  static const bool once = [] { Switch s; if(envSwitch(s)) switchState() = s; return true; }();
  (void)once;
  return switchState();
  }

// The ring, cm. Full density to `full`; from there the share of blades left is full/d and each is d/full wider;
// from `fadeFrom` to `radius` the height and the share go to nothing. The ground takes the far look of the grass
// from fadeFrom/2 to fadeFrom
struct Ring final {
  float full     = 1000.f;
  float fadeFrom = 4800.f;
  float radius   = 6400.f;
  float density  = 1.f; // of the record's
  };

inline Ring ring(uint8_t mode) {
  if(mode==Low)
    return Ring{800.f, 3000.f, 4000.f, 0.625f};
  return Ring{};
  }

}
