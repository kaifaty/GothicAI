// Contract of the wind fade's distance intervals (graphics/foliagefadeinterval.h, M1 of
// reports/engine/2026-11-27-instance-management). The interval must hold the quantized fade code everywhere inside
// it, against the real fadeBound evaluated point by point; and a frame cache built on the intervals must produce
// the same code sequence as the full evaluation on still, moving and teleporting cameras. No game, base or GPU.
#include "graphics/foliagefadeinterval.h"
#include "graphics/foliagetree.h"

#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

using namespace FoliageTree;

namespace {

constexpr uint32_t treeAnchor = uint32_t(FoliageWind::AnchorTree) << 24;

uint16_t codeAt(float reach, float scale, float px, float d) {
  return fadeCode(fadeBound(treeAnchor, reach, scale, d, px));
  }

int failures = 0;

void check(bool ok, const char* what, float reach, float scale, float px, float d) {
  if(!ok) {
    ++failures;
    std::printf("FAIL %s reach=%.3f scale=%.4f px=%.4f d=%.3f\n", what, reach, scale, px, d);
    }
  }

// the inverse smoothstep round-trips
void testInvSmooth() {
  for(int i=0; i<=1000; ++i) {
    const float y = float(i)/1000.f;
    const float t = FoliageFadeInterval::invSmooth(y);
    const float s = t*t*(3.f-2.f*t);
    check(std::fabs(s-y)<1e-5f, "invSmooth", 0, 0, y, t);
    }
  }

// inside the interval the code holds; the interval contains its anchor
void testConstancy(std::mt19937& rng) {
  std::uniform_real_distribution<float> reachD(100.f, 3000.f);
  std::uniform_real_distribution<float> scaleD(0.3f, 3.f);
  std::uniform_real_distribution<float> pxD(0.2f, 40.f);
  std::uniform_real_distribution<float> distD(0.5f, 200000.f);
  for(int i=0; i<4000; ++i) {
    const float reach = reachD(rng), scale = scaleD(rng), px = pxD(rng), d0 = distD(rng);
    const auto iv = FoliageFadeInterval::of(treeAnchor, reach, scale, px, d0);
    const uint16_t c0 = codeAt(reach, scale, px, d0);
    check(iv.lo<=d0 && d0<iv.hi, "anchor inside", reach, scale, px, d0);
    // dense sweep inside, plus the edges
    for(int k=0; k<=64; ++k) {
      const float t = float(k)/64.f;
      float d;
      if(iv.hi>=1e30f)
        d = iv.lo + t*std::max(d0*4.f, 1000.f);
      else
        d = iv.lo + t*(iv.hi-iv.lo)*(1.f-1e-6f);
      if(d<iv.lo || d>=iv.hi)
        continue;
      check(codeAt(reach, scale, px, d)==c0, "code inside", reach, scale, px, d);
      }
    }
  }

// a frame cache on the intervals against the full evaluation, on three camera kinds
void testCachePaths(std::mt19937& rng) {
  std::uniform_real_distribution<float> reachD(100.f, 3000.f);
  std::uniform_real_distribution<float> scaleD(0.3f, 3.f);
  std::uniform_real_distribution<float> pxD(0.2f, 40.f);
  std::uniform_real_distribution<float> posD(-50000.f, 50000.f);
  for(int tree=0; tree<300; ++tree) {
    const float reach = reachD(rng), scale = scaleD(rng), px = pxD(rng);
    const float at = posD(rng);
    float fadeReach = -1, fadeScale = -1, fadePx = -1, lo = 0, hi = 0;
    uint16_t cachedCode = 0;
    bool have = false;
    long recomputes = 0;
    for(int frame=0; frame<600; ++frame) {
      float cam;
      const int kind = tree%3;
      if(kind==0)
        cam = 0.f;                                   // still
      else if(kind==1)
        cam = float(frame)*30.f;                     // walking, 30 cm/frame
      else
        cam = (frame%120==0) ? posD(rng) : float(frame%120)*30.f; // teleport every 120
      const float d = std::fabs(at-cam);
      const uint16_t full = codeAt(reach, scale, px, d);
      uint16_t got;
      if(have && fadeReach==reach && fadeScale==scale && fadePx==px && d>=lo && d<hi) {
        got = cachedCode;
        }
      else {
        got = full;
        const auto iv = FoliageFadeInterval::of(treeAnchor, reach, scale, px, d);
        fadeReach = reach; fadeScale = scale; fadePx = px;
        lo = iv.lo; hi = iv.hi;
        cachedCode = got;
        have = true;
        ++recomputes;
        }
      check(got==full, "cache path", reach, scale, px, d);
      }
    if(tree%3==0)
      check(recomputes==1, "still camera evaluates once", reach, scale, px, float(recomputes));
    }
  }

// no fade and no tree: the whole range holds
void testDegenerate() {
  const auto iv0 = FoliageFadeInterval::of(treeAnchor, 1000.f, 1.f, 0.f, 500.f);
  check(iv0.lo==0.f && iv0.hi>=1e30f, "px=0", 1000, 1, 0, 500);
  const auto iv1 = FoliageFadeInterval::of(0, 1000.f, 1.f, 10.f, 500.f);
  check(iv1.lo==0.f && iv1.hi>=1e30f, "not a tree", 1000, 1, 10, 500);
  // a far tree whose wind has faded to nothing keeps the code out to the horizon
  const float reach = 1500.f, scale = 1.f, px = 2.f;
  const float dFar = 150000.f;
  if(codeAt(reach, scale, px, dFar)==0) {
    const auto iv = FoliageFadeInterval::of(treeAnchor, reach, scale, px, dFar);
    check(iv.hi>=1e30f, "fade 0 to the horizon", reach, scale, px, dFar);
    }
  }

}

int main() {
  std::mt19937 rng(27112026);
  testInvSmooth();
  testConstancy(rng);
  testCachePaths(rng);
  testDegenerate();
  if(failures==0)
    std::printf("fade-interval: all checks passed\n");
  else
    std::printf("fade-interval: %d failures\n", failures);
  return failures==0 ? 0 : 1;
  }