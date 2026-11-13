#pragma once

// Noise of the island generator: an integer hash of the lattice point and the seed, no rand() and no state, so the
// same call gives the same number in any order and on any thread (reports/engine/2026-11-13-empty-island/
// island-content.md, "Детерминизм"). A period > 0 wraps the lattice: the noise then tiles with that many cells.

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace island {

inline uint32_t hash32(uint32_t x) {
  x ^= x >> 16;
  x *= 0x7feb352dU;
  x ^= x >> 15;
  x *= 0x846ca68bU;
  x ^= x >> 16;
  return x;
  }

inline uint32_t hashCell(int32_t x, int32_t y, uint32_t seed) {
  return hash32(uint32_t(x)*0x9e3779b1U ^ hash32(uint32_t(y)*0x85ebca77U ^ hash32(seed)));
  }

// [0,1)
inline float hashUnit(uint32_t h) {
  return float(h >> 8)*(1.f/16777216.f);
  }

inline int32_t wrapCell(int32_t v, int32_t period) {
  if(period<=0)
    return v;
  v %= period;
  return v<0 ? v+period : v;
  }

inline float saturate(float v) {
  return std::min(1.f, std::max(0.f, v));
  }

inline float smoothstep(float a, float b, float v) {
  const float t = saturate((v-a)/(b-a));
  return t*t*(3.f-2.f*t);
  }

inline float mix(float a, float b, float t) {
  return a + (b-a)*t;
  }

// Value noise in [-1,1], twice differentiable (quintic weights); the lattice wraps by its own period on each axis.
inline float valueNoise(float x, float y, int32_t periodX, int32_t periodY, uint32_t seed) {
  const float fx = std::floor(x), fy = std::floor(y);
  const int32_t ix = int32_t(fx), iy = int32_t(fy);
  const float tx = x-fx, ty = y-fy;
  const float wx = tx*tx*tx*(tx*(tx*6.f-15.f)+10.f);
  const float wy = ty*ty*ty*(ty*(ty*6.f-15.f)+10.f);
  const int32_t x0 = wrapCell(ix, periodX), x1 = wrapCell(ix+1, periodX);
  const int32_t y0 = wrapCell(iy, periodY), y1 = wrapCell(iy+1, periodY);
  const float a = hashUnit(hashCell(x0, y0, seed)), b = hashUnit(hashCell(x1, y0, seed));
  const float c = hashUnit(hashCell(x0, y1, seed)), d = hashUnit(hashCell(x1, y1, seed));
  return mix(mix(a, b, wx), mix(c, d, wx), wy)*2.f-1.f;
  }

inline float valueNoise(float x, float y, int32_t period, uint32_t seed) {
  return valueNoise(x, y, period, period, seed);
  }

// Octaves of value noise, each twice the frequency and `gain` of the amplitude; normalised to [-1,1].
inline float fbm(float x, float y, int32_t period, uint32_t seed, int octaves, float gain = 0.5f) {
  float sum = 0, amp = 1, norm = 0;
  for(int o=0; o<octaves; ++o) {
    sum  += amp*valueNoise(x, y, period, seed+uint32_t(o)*101U);
    norm += amp;
    amp  *= gain;
    x *= 2.f;
    y *= 2.f;
    period *= 2;
    }
  return sum/norm;
  }

// The same noise in space, without a period: the shape of a loose stone.
inline float valueNoise3(float x, float y, float z, uint32_t seed) {
  const float fx = std::floor(x), fy = std::floor(y), fz = std::floor(z);
  const int32_t ix = int32_t(fx), iy = int32_t(fy), iz = int32_t(fz);
  const float tx = x-fx, ty = y-fy, tz = z-fz;
  const float wx = tx*tx*tx*(tx*(tx*6.f-15.f)+10.f);
  const float wy = ty*ty*ty*(ty*(ty*6.f-15.f)+10.f);
  const float wz = tz*tz*tz*(tz*(tz*6.f-15.f)+10.f);
  float layer[2];
  for(int32_t k=0; k<2; ++k) {
    const uint32_t s = hash32(seed ^ uint32_t(iz+k)*0xc2b2ae35U);
    const float a = hashUnit(hashCell(ix, iy, s)),   b = hashUnit(hashCell(ix+1, iy, s));
    const float c = hashUnit(hashCell(ix, iy+1, s)), d = hashUnit(hashCell(ix+1, iy+1, s));
    layer[k] = mix(mix(a, b, wx), mix(c, d, wx), wy);
    }
  return mix(layer[0], layer[1], wz)*2.f-1.f;
  }

inline float fbm3(float x, float y, float z, uint32_t seed, int octaves, float gain = 0.5f) {
  float sum = 0, amp = 1, norm = 0;
  for(int o=0; o<octaves; ++o) {
    sum  += amp*valueNoise3(x, y, z, seed+uint32_t(o)*101U);
    norm += amp;
    amp  *= gain;
    x *= 2.f;
    y *= 2.f;
    z *= 2.f;
    }
  return sum/norm;
  }

struct Cells {
  float    f1 = 0, f2 = 0; // distances to the nearest and the second nearest point, in cells
  uint32_t id = 0;         // hash of the nearest point's cell
  float    dx = 0, dy = 0; // from the nearest point to the sample, in cells
  };

// Scattered points, one per lattice cell (Worley).
inline Cells cellNoise(float x, float y, int32_t period, uint32_t seed) {
  const float fx = std::floor(x), fy = std::floor(y);
  const int32_t ix = int32_t(fx), iy = int32_t(fy);
  Cells r;
  r.f1 = r.f2 = 1e9f;
  for(int32_t oy=-1; oy<=1; ++oy)
    for(int32_t ox=-1; ox<=1; ++ox) {
      const uint32_t h = hashCell(wrapCell(ix+ox, period), wrapCell(iy+oy, period), seed);
      const float px = float(ix+ox) + hashUnit(h);
      const float py = float(iy+oy) + hashUnit(hash32(h));
      const float dx = x-px, dy = y-py;
      const float d  = std::sqrt(dx*dx+dy*dy);
      if(d<r.f1) {
        r.f2 = r.f1;
        r.f1 = d;
        r.id = h;
        r.dx = dx;
        r.dy = dy;
        } else if(d<r.f2) {
        r.f2 = d;
        }
      }
  return r;
  }

}
