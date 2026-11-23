#pragma once

#include <Tempest/Vec>
#include <cmath>
#include <cstdint>

#include "graphics/foliagewind.h"

// The travelling front of the wind (reports/engine/2026-11-14-grass-system, "Ветер"): a gust does not come to the
// whole scene at once. The lean's pressure, the swing's share and the flutter's pressure of a place are those of
// the scene's wind at the tick `t - z/Speed`: the front runs along +Z, the mean direction of the world's wind, at
// Speed. The axis is fixed; the direction of the lean stays the scene's.
//
// The processor gives the two phases of WorldWind::sample's speed, folded into one turn, and the shader takes
// `z*k` off them (foliageFrontAt, shader/materials/foliage_wind.glsl). This file is that shader's mirror for the
// tests and the benches. worldwind.h is not changed: the speed's formula is repeated here and held to it by
// tests/grass_test.cpp.
namespace FoliageFront {

constexpr double Speed     = 500.0;  // cm/s
constexpr double RateSlow  = 0.113;  // WorldWind::sample: the slow term of the speed, turns per period
constexpr double RateGust  = 0.31;   // and the gust's

// SceneDesc::foliageFront and foliageFrontK of scene.glsl
struct Front final {
  Tempest::Vec4 phase; // x, y: 0..1 of the slow term's and the gust's turns at z=0; z, w: the previous frame's
  Tempest::Vec4 k;     // x, y: their turns per cm of z; z: the wind's scale; w: 1 when the front runs
  };

inline double fract(double v) { return v - std::floor(v); }

// Without the world's wind, or with the steady legacy one, there is no front: every place has the scene's numbers
inline Front make(uint64_t tick, uint64_t periodMs, bool windEnabled, float scale, bool legacy) {
  Front f;
  if(!windEnabled || periodMs==0 || legacy || scale<=0.f)
    return f;
  const double turns = double(tick)/double(periodMs);
  const double perCm = 1000.0/(Speed*double(periodMs)); // ticks per cm over the period
  f.phase = Tempest::Vec4(float(fract(turns*RateSlow)), float(fract(turns*RateGust)), 0.f, 0.f);
  f.phase.z = f.phase.x;
  f.phase.w = f.phase.y;
  f.k     = Tempest::Vec4(float(RateSlow*perCm), float(RateGust*perCm), scale, 1.f);
  return f;
  }

struct Local final {
  float pressure = 0.f; // FoliageWind::Scene::wind.z
  float swing    = 0.f; // swing.y
  float flutter  = 0.f; // swing.z
  };

// The numbers of the place at `z` (cm). `scene` is the frame's FoliageWind::scene: its numbers are returned where
// the front does not run
inline Local atPlace(const Front& f, const FoliageWind::Scene& scene, float z, bool previous = false) {
  if(f.k.w<=0.f)
    return Local{scene.wind.z, scene.swing.y, scene.swing.z};
  const float pa = previous ? f.phase.z : f.phase.x;
  const float pb = previous ? f.phase.w : f.phase.y;
  const float sa = std::sin(6.2831853f*FoliageWind::fract(pa - z*f.k.x));
  const float g  = 0.5f + 0.5f*std::sin(6.2831853f*FoliageWind::fract(pb - z*f.k.y));
  const float x  = (2.2f + 0.65f*sa + 2.8f*g*g*g*g)*f.k.z/FoliageWind::GustSpeed;
  return Local{FoliageWind::pressure(x), FoliageWind::swingAmp(x), FoliageWind::eddy(x)};
  }

// ms until the gust's peak next comes to the place at `z`, counted from `tick`
inline uint64_t untilGust(uint64_t tick, uint64_t periodMs, float z) {
  if(periodMs==0)
    return 0;
  const double cycle = double(periodMs)/RateGust;
  const double delay = double(z)*1000.0/Speed;
  const double at    = fract((double(tick) - delay)/cycle);
  const double left  = fract(0.25 - at);
  return uint64_t(left*cycle);
  }

}
