#include "grasscover.h"

#include <algorithm>

#include "graphics/grass/grassplace.h"

namespace Grass {

namespace {

// The spots: the lattice of the first round's (grasspatch.cpp) and two thirds of its spots, so a spot that stays
// is where it was. The
// radius, 30 to 70 cm, is bent by a noise of 54 cm by up to 1.7 of itself and by one of 16 cm by up to 0.65: an
// outline half as long again as a circle's of the same area. The bend is held under 0.9 of the radius, so a spot
// with its thinning sward ends within 2 m of its centre: the four squares round a place are all that can reach it
constexpr int32_t  BareStep    = 400*Unit;
constexpr uint32_t BareChance  = 90;
constexpr int32_t  BareRadius  = 30*Unit;
constexpr int32_t  BareSpread  = 40*Unit;
constexpr int32_t  BareBend1   = 435; // of 256
constexpr int32_t  BareBend2   = 166;
constexpr int32_t  BareStep1   = 54*Unit;
constexpr int32_t  BareStep2   = 16*Unit;
static_assert((BareRadius+BareSpread)*19/10 + CoverSpotSoft*Unit/2 <= BareStep/2, "a spot is found in the four squares round a place");

// The border's line: the slow noise's values this far apart are the deepest bay and the longest tongue
constexpr int32_t  LineLow     = 15000;
constexpr int32_t  LineHigh    = 56000;

}

int32_t coverSpot(int32_t x, int32_t z, int32_t grow, int32_t soft) {
  // as far as a spot of the widest radius and bend is felt
  const int32_t reach = (BareRadius+BareSpread)*19/10 + grow + soft/2;
  const int32_t i0 = floorDiv(x-reach,BareStep), i1 = floorDiv(x+reach,BareStep);
  const int32_t j0 = floorDiv(z-reach,BareStep), j1 = floorDiv(z+reach,BareStep);
  int32_t bare = 0, bend = 0;
  bool    bent = false;
  for(int32_t j=j0; j<=j1; ++j)
    for(int32_t i=i0; i<=i1; ++i) {
      const uint32_t h = hash32(i,j,0xB3u);
      if((h & 0xFFu)>=BareChance)
        continue;
      const int32_t bx = i*BareStep + int32_t(((h >> 8) & 0xFFFu)*uint32_t(BareStep) >> 12);
      const int32_t bz = j*BareStep + int32_t(((h >> 20) & 0xFFFu)*uint32_t(BareStep) >> 12);
      const int32_t r  = BareRadius + int32_t((hash32(i,j,0xB4u) & 0xFFu)*uint32_t(BareSpread) >> 8);
      const int64_t dx = x-bx, dz = z-bz;
      // where the level comes to nothing at the furthest bend
      const int64_t end = r*19/10 + grow + soft/2;
      if(dx*dx + dz*dz>=end*end || !patchSward(patchNoise(bx,bz)))
        continue;
      if(!bent) {
        bend = (valueNoise(x,z,BareStep1,0xB6u)-32640)*BareBend1/32640 + (valueNoise(x,z,BareStep2,0xB7u)-32640)*BareBend2/32640;
        bent = true;
        }
      const int32_t warp = std::clamp(bend*r/256, -r*9/10, r*9/10);
      const int32_t d    = int32_t(isqrt(uint64_t(dx*dx + dz*dz)));
      bare = std::max(bare, std::clamp((r+grow-d-warp)*256/soft + 128, 0, 256));
      }
  return bare;
  }

int32_t coverBare(int32_t x, int32_t z) {
  return coverSpot(x,z,0,CoverSpotSoft*Unit);
  }

int32_t coverClear(int32_t x, int32_t z) {
  return 256 - coverSpot(x,z,(CoverSpotSoft/2 + CoverClearFrom + CoverClearSoft/2)*Unit,CoverClearSoft*Unit);
  }

int32_t coverBorder(int32_t x, int32_t z, int32_t s, int32_t reach) {
  if(s<=-CoverInside*Unit)
    return 256;
  if(s>=coverBeyond(reach)*Unit)
    return 0;
  const int32_t slow = (3*valueNoise(x,z,240*Unit,0x61u) + valueNoise(x+911,z+1777,60*Unit,0x62u)) >> 2;
  const int32_t t    = std::clamp((slow-LineLow)*256/(LineHigh-LineLow), 0, 256);
  // where the sward ends here, units from the border: over 0 on the ground beside the meadow
  int32_t line = -CoverNotch*Unit + ((CoverSpan*Unit*t) >> 8);
  if(line>0)
    line = line*reach/CoverReach;
  line += (valueNoise(x,z,24*Unit,0x63u)-32640)*(CoverRipple*Unit)/32640;
  return std::clamp((line-s)*256/(CoverSoft*Unit) + 128, 0, 256);
  }

int32_t coverTuft(int32_t x, int32_t z) {
  return (valueNoise(x,z,18*Unit,0x64u)-32640)*CoverTuft/32640;
  }

int32_t coverIsles(int32_t x, int32_t z) {
  return (valueNoise(x,z,45*Unit,0x67u)-32640)*CoverIsles/32640;
  }

int32_t coverBreaks(int32_t x, int32_t z) {
  return std::clamp((valueNoise(x,z,11*Unit,0x69u)-32640)*3, -32640, 32640)*CoverBreaks/32640;
  }

}
