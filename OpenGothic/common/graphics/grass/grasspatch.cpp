#include "grasspatch.h"

#include <algorithm>

#include "graphics/grass/grassplace.h"

namespace Grass {

namespace {

// The patches by the noise's value: its measured quantiles 30, 38 and 88 % and the top of the clumps
constexpr int32_t PatchMain0 = 26192;
constexpr int32_t PatchMain1 = 28857;
constexpr int32_t PatchClump = 46436;
constexpr int32_t PatchTop   = 56000;
constexpr int32_t PatchLow   = 18361; // the sward is at its lowest under it

// The short sward: more blades than the main grass has, since a blade of half the height hides half the ground
constexpr int32_t SwardDensity = 320;

// The spots of bare earth: one in a square of 4 m with this chance of 256, 55 to 110 cm in radius, the last 30 cm
// of it thinning; the rim wanders by 20 cm
constexpr int32_t  BareStep   = 400*Unit;
constexpr uint32_t BareChance = 140;
constexpr int32_t  BareRadius = 55*Unit;
constexpr int32_t  BareSoft   = 30*Unit;
constexpr int32_t  BareRim    = 20*Unit;

int32_t ramp(int32_t n, int32_t a, int32_t b, int32_t va, int32_t vb) {
  return va + int32_t(int64_t(vb-va)*int64_t(n-a)/int64_t(b-a));
  }

float rampF(int32_t n, int32_t a, int32_t b, float va, float vb) {
  return va + (vb-va)*float(n-a)/float(b-a);
  }

// eight sides of half a turn, of 1024
constexpr int32_t AxisX[8] = {1024, 946, 724, 392, 0, -392, -724, -946};
constexpr int32_t AxisZ[8] = {0, 392, 724, 946, 1024, 946, 724, 392};

}

int32_t patchNoise(int32_t x, int32_t z) {
  return (3*valueNoise(x,z,600*Unit,0x51u) + valueNoise(x+1777,z+911,220*Unit,0xA7u)) >> 2;
  }

int32_t patchDensity(int32_t n) {
  if(n<PatchMain0)
    return SwardDensity;
  if(n<PatchMain1)
    return ramp(n,PatchMain0,PatchMain1,SwardDensity,256);
  if(n<PatchClump)
    return 256;
  if(n<PatchTop)
    return ramp(n,PatchClump,PatchTop,256,512);
  return 512;
  }

float patchHeight(int32_t n) {
  if(n<PatchLow)
    return 0.55f;
  if(n<PatchMain0)
    return rampF(n,PatchLow,PatchMain0,0.55f,0.65f);
  if(n<PatchMain1)
    return rampF(n,PatchMain0,PatchMain1,0.65f,0.9f);
  if(n<PatchClump)
    return rampF(n,PatchMain1,PatchClump,0.9f,1.15f);
  if(n<PatchTop)
    return rampF(n,PatchClump,PatchTop,1.5f,2.4f);
  return 2.4f;
  }

bool patchSward(int32_t n) {
  return n<PatchMain0;
  }

int32_t patchBare(int32_t x, int32_t z) {
  const int32_t cx = floorDiv(x-BareStep/2,BareStep), cz = floorDiv(z-BareStep/2,BareStep);
  int32_t bare = 0;
  for(int32_t j=cz; j<=cz+1; ++j)
    for(int32_t i=cx; i<=cx+1; ++i) {
      const uint32_t h = hash32(i,j,0xB3u);
      if((h & 0xFFu)>=BareChance)
        continue;
      const int32_t bx = i*BareStep + int32_t(((h >> 8) & 0xFFFu)*uint32_t(BareStep) >> 12);
      const int32_t bz = j*BareStep + int32_t(((h >> 20) & 0xFFFu)*uint32_t(BareStep) >> 12);
      const int32_t r  = BareRadius + int32_t((hash32(i,j,0xB4u) & 0xFFu)*uint32_t(BareRadius) >> 8);
      const int64_t dx = x-bx, dz = z-bz;
      if(dx*dx + dz*dz>=int64_t(r+BareRim)*int64_t(r+BareRim) || !patchSward(patchNoise(bx,bz)))
        continue;
      const int32_t d = int32_t(isqrt(uint64_t(dx*dx + dz*dz))) + (valueNoise(x,z,60*Unit,0xB5u)-32640)*BareRim/32640;
      bare = std::max(bare, std::clamp((r-d)*256/BareSoft, 0, 256));
      }
  return bare;
  }

Tuft tuft(int32_t tx, int32_t tz, int32_t patch) {
  const uint64_t h = hash64(tx,tz,0x7Fu), own = hash64(tx,tz,0xC3u);
  auto of = [](uint64_t bits, int shift) { return float((bits >> shift) & 0xFFu)/255.f; };

  Tuft t;
  t.x = tx*TuftStep + int32_t(h % uint64_t(TuftStep));
  t.z = tz*TuftStep + int32_t((h >> 16) % uint64_t(TuftStep));
  const int32_t n = patch>=0 ? patch : patchNoise(t.x,t.z);
  t.sward = patchSward(n);
  t.clump = !t.sward && patchDensity(n)>256;
  // the sward is even: half the difference between its tufts, and its blades are hardly gathered
  const float wild = t.sward ? 0.5f : 1.f;

  // a count of 0.62 to 1.38 of the patch's, a height of 0.7 to 1.35
  const int32_t count = 256 + int32_t(float(int32_t((own >> 27) & 0xFFu) - 128)*0.76f*wild);
  t.densityQ8 = patchDensity(n)*count/256;
  t.height    = patchHeight(n)*(1.025f + (of(h,32)-0.5f)*0.65f*wild);
  // the outline: a reach of 0.8 to 1.26 of a plain tuft's, up to twice as long as wide, turned by a side of eight
  t.reachQ8   = 160 + int32_t(own & 0xFFu);
  t.axisX     = AxisX[(own >> 8) & 7u];
  t.axisZ     = AxisZ[(own >> 8) & 7u];
  t.longQ8    = 256 + int32_t((own >> 11) & 0xFFu);
  // one tuft in five is loose: its blades stand where the lattice put them
  const auto tight = int32_t((own >> 19) & 0xFFu);
  t.pullQ8    = tight<56 ? tight*2/3 : 90 + (tight-56)*210/199;
  if(t.sward)
    t.pullQ8 = t.pullQ8*115/256;
  t.fall      = (0.15f + 0.40f*of(own,35))*wild;
  t.spread    = 0.25f + 0.55f*of(own,43)*wild;
  const float side = of(own,51)*6.2831853f;
  t.leanX     = std::cos(side);
  t.leanZ     = std::sin(side);
  t.fan       = float((own >> 59) & 0x1Fu)/31.f;
  t.lean      = 0.6f + 0.8f*float((h >> 52) & 0x3Fu)/63.f;
  t.phase     = uint8_t((h >> 40) & 0xFFu);
  t.tint      = uint8_t((h >> 48) & 15u);
  return t;
  }

int64_t tuftMeasure(const Tuft& t, int32_t px, int32_t pz) {
  const int64_t dx = px-t.x, dz = pz-t.z;
  const int64_t u  = dx*t.axisX + dz*t.axisZ, v = dz*t.axisX - dx*t.axisZ;
  const int64_t u2 = (u*u) >> 20, v2 = (v*v) >> 20;
  return (u2*65536/t.longQ8 + v2*t.longQ8)*t.reachQ8;
  }

}
