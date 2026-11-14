#pragma once

#include <cmath>
#include <cstdint>

// The meadow's patches, its spots of bare earth and what a tuft has of its own (reports/engine/2026-11-14-grass-
// system, «Круг правки вида»). Whole numbers decide where grass stands: the ground's shader repeats the patches and
// the spots from the same hashes (shader/grass/grass_common.glsl), so keep the two in step.
namespace Grass {

inline int32_t floorDiv(int32_t a, int32_t b) {
  const int32_t q = a/b;
  return (a%b!=0 && ((a<0)!=(b<0))) ? q-1 : q;
  }

// The same hash as grassHash of grass_common.glsl: 32 bits, so the ground's shader repeats the patches
inline uint32_t hash32(int32_t ix, int32_t iz, uint32_t salt) {
  uint32_t h = uint32_t(ix)*0x9E3779B1u ^ uint32_t(iz)*0x85EBCA77u ^ salt;
  h ^= h >> 15;
  h *= 0x2C1B3C6Du;
  h ^= h >> 12;
  h *= 0x297A2D39u;
  h ^= h >> 15;
  return h;
  }

inline uint64_t hash64(int32_t a, int32_t b, uint32_t salt) {
  uint64_t x = (uint64_t(uint32_t(a)) << 32 | uint64_t(uint32_t(b))) ^ (uint64_t(salt)*0x9E3779B97F4A7C15ull);
  x ^= x >> 33;
  x *= 0xff51afd7ed558ccdull;
  x ^= x >> 33;
  x *= 0xc4ceb9fe1a85ec53ull;
  x ^= x >> 33;
  return x;
  }

// Value noise on a lattice of `step` units: 0..65280
inline int32_t valueNoise(int32_t x, int32_t z, int32_t step, uint32_t salt) {
  const int32_t ix = floorDiv(x,step), iz = floorDiv(z,step);
  const int32_t tx = ((x-ix*step)*256)/step, tz = ((z-iz*step)*256)/step;
  const int32_t sx = (tx*tx*(768-2*tx)) >> 16, sz = (tz*tz*(768-2*tz)) >> 16;
  auto at = [&](int32_t i, int32_t j) { return int32_t((hash32(i,j,salt) >> 8) & 0xFFu); };
  const int32_t top = at(ix,iz)  *(256-sx) + at(ix+1,iz)  *sx;
  const int32_t bot = at(ix,iz+1)*(256-sx) + at(ix+1,iz+1)*sx;
  return (top*(256-sz) + bot*sz) >> 8;
  }

inline uint32_t isqrt(uint64_t v) {
  auto r = uint64_t(std::sqrt(double(v)));
  while(r*r>v)
    --r;
  while((r+1)*(r+1)<=v)
    ++r;
  return uint32_t(r);
  }

// The patches of the meadow: 0..65535 at a place (units), the same numbers as grassPatch of grass_common.glsl.
// Low: the short dense sward (three tenths of the meadow); then the main grass; the top eighth: the tall clumps
int32_t patchNoise(int32_t x, int32_t z);
// The share of BladesPerM2 a patch keeps, in 1/256, and the height against the record's
int32_t patchDensity(int32_t noise);
float   patchHeight(int32_t noise);
bool    patchSward(int32_t noise);
// A spot of bare earth, a metre or two across, in the short sward alone: how bare the place is, in 1/256.
// grassBare of grass_common.glsl
int32_t patchBare(int32_t x, int32_t z);

// A tuft: the blades of the lattice's nodes it owns. Each has a size, an outline, a count, a lean and a spread of
// heights of its own, so no two neighbours read as copies; the phase is one for the tuft, so it moves as one body
struct Tuft final {
  int32_t  x = 0, z = 0;       // its centre, units
  int32_t  densityQ8 = 0;      // the patch's, by the tuft's own count
  int32_t  reachQ8   = 256;    // of the squared distance to it: the smaller, the wider the tuft
  int32_t  axisX     = 1024;   // the long axis of its outline, of 1024
  int32_t  axisZ     = 0;
  int32_t  longQ8    = 256;    // the outline is this much longer than wide
  int32_t  pullQ8    = 256;    // how tightly the blades gather to the centre, of the record's `clumps`
  float    height    = 1.f;    // against the record's
  float    fall      = 0.4f;   // how much lower the blades at its rim
  float    spread    = 0.5f;   // of its blades' heights
  float    leanX     = 1.f;    // the side the whole tuft leans to
  float    leanZ     = 0.f;
  float    fan       = 1.f;    // the share of the lean out of the centre; the rest goes to that side
  float    lean      = 1.f;    // of the lean's angle
  uint8_t  phase     = 0;
  uint8_t  tint      = 0;
  bool     sward     = false;
  bool     clump     = false;
  };

// The tuft of the square (tx,tz) of TuftStep. patch: every tuft is of this patches' noise (a bench's measure)
Tuft    tuft(int32_t tx, int32_t tz, int32_t patch = -1);
// The measure of a point against a tuft, 65536 per squared unit for a plain one: the least owns the point
int64_t tuftMeasure(const Tuft& t, int32_t px, int32_t pz);

}
