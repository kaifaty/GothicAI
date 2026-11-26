#pragma once

#include "graphics/foliagetree.h"

#include <algorithm>
#include <cfloat>
#include <cmath>

// Distance intervals of the quantized wind fade (reports/engine/2026-11-27-instance-management/README.md, M1).
// FoliageTree::fadeCode is a step function of the distance: each of the three smoothstep components of the fade is
// quantized by ceil to FadeBoundSteps, and the quantized max is the max of the quantized parts, so the packed code
// changes only where one component crosses a quantization boundary. A component's argument is K/(d*FadeBoundNear),
// so its boundaries invert in closed form through the inverse smoothstep. Between the nearest boundaries around a
// distance the code holds; preFrameUpdateTrees re-evaluates fadeBound only when the distance leaves that interval
// or the inputs (reach, scale, px) change. The interval is shrunk by a relative margin: float rounding at a boundary
// then costs an extra evaluation, never a stale code.
namespace FoliageFadeInterval {

// smoothstep(t) = t*t*(3-2t) inverted on [0,1]: t = 1/2 - sin(asin(1-2y)/3)
inline float invSmooth(float y) {
  return 0.5f - std::sin(std::asin(1.f - 2.f*y)/3.f);
  }

struct Interval {
  float lo = 0.f;
  float hi = 0.f;
  };

// The distance range [lo,hi) on which fadeBound(bits,reach,scale,d,px) keeps the code it has at d0.
// Conservative: an boundary the fade's distance clamp (max(d,1) inside fade) makes moot still splits the interval.
inline Interval of(uint32_t bits, float reach, float scale, float px, float d0) {
  if(!FoliageTree::isTree(bits) || px<=0.f)
    return {0.f, FLT_MAX};

  const float height = std::max(reach, 1e-3f);
  const float arm    = height*FoliageTree::BranchReach;
  const float bough  = arm*FoliageTree::BranchGive*
                       std::min(std::sqrt(scale*arm/FoliageTree::BranchLong), FoliageTree::BranchTopK);
  // the K of each component: its smoothstep argument is K/(d*FadeBoundNear)
  const float K[3] = {
    FoliageTree::LeafMax*scale*px,
    bough*scale*px,
    FoliageTree::TreeLean*height*scale*px,
    };
  const float from[3] = {FoliageTree::FadeLeafNone, FoliageTree::FadeBranchNone, FoliageTree::FadeTrunkNone};
  const float full[3] = {FoliageTree::FadeLeafFull, FoliageTree::FadeBranchFull, FoliageTree::FadeTrunkFull};

  float bounds[3*9];
  size_t n = 0;
  for(int c=0; c<3; ++c) {
    if(K[c]<=0.f)
      continue;
    // the clamp edges of the smoothstep: argument >= full (code 8) and <= from (code 0)
    bounds[n++] = K[c]/(full[c]*FoliageTree::FadeBoundNear);
    bounds[n++] = K[c]/(from[c]*FoliageTree::FadeBoundNear);
    for(int q=1; q<int(FoliageTree::FadeBoundSteps); ++q) {
      const float t = invSmooth(float(q)/FoliageTree::FadeBoundSteps);
      const float v = from[c] + (full[c]-from[c])*t;
      bounds[n++] = K[c]/(v*FoliageTree::FadeBoundNear);
      }
    }
  std::sort(bounds, bounds+n);

  // below 1/Near the fade's argument is clamped and the code holds; boundaries under 1 are moot
  float lo = d0<1.f ? 0.f : 1.f;
  float hi = FLT_MAX;
  for(size_t i=0; i<n; ++i) {
    const float b = bounds[i];
    if(b<1.f)
      continue;
    if(b<=d0)
      lo = b;
    else {
      hi = b;
      break;
      }
    }

  constexpr float margin = 1e-4f;
  lo = std::min(lo*(1.f+margin), d0);
  hi = std::max(hi*(1.f-margin), d0*(1.f+margin));
  if(d0<1.f)
    lo = 0.f;
  return {lo, hi};
  }

}