// The VSM lamp-face cache without a GPU: the decisions of shader/virtual_shadow/vsm_cache.glsl (compiled here as
// C++) — a face's place, its size with hysteresis, the spheres that reset it — and the constant shadow range of a
// flickering light (LightGroup::shadowRange). CPU only: no device, no game data.
#include "graphics/lightgroup.h"
#include "graphics/lightsource.h"

#include <cstdint>
#include <cstdio>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace glsl {
using uint = uint32_t;
struct vec3 {
  float x, y, z;
  };
inline float dot(const vec3& a, const vec3& b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
inline float abs(float v) { return v<0 ? -v : v; }
inline uint  min(uint a, uint b) { return a<b ? a : b; }
inline void  atomicOr(uint& dst, uint bits) { dst |= bits; }
#define INOUT(T) T&
uint vsmCacheOcc[64] = {};
#include "../shader/virtual_shadow/vsm_cache.glsl"
}

using namespace glsl;

namespace {
size_t assertions = 0;

void require(bool condition, const std::string& message) {
  ++assertions;
  if(!condition)
    throw std::runtime_error(message);
  }

void clearOcc() {
  for(auto& w:vsmCacheOcc)
    w = 0;
  }

// a flickering torch: the light follows the animation, its faces keep the widest range
void shadowRangeIsConstant() {
  LightSource l;
  l.setRange(std::vector<float>{0.6f, 1.0f, 0.8f, 0.7f}, 1000.f, 10.f, true);
  float lo = 1e9f, hi = 0;
  for(uint64_t t=0; t<2000; t+=17) {
    l.update(t);
    lo = std::min(lo, l.currentRange());
    hi = std::max(hi, l.currentRange());
    require(LightGroup::shadowRange(l)==1000.f, "the shadow range does not follow the flicker");
    }
  require(lo<700.f && hi>950.f, "the light's own range flickers");

  l.setEnabled(false);
  require(LightGroup::shadowRange(l)==0.f, "a light switched off has no shadow range");

  LightSource wide;
  wide.setRange(5000.f);
  require(LightGroup::shadowRange(wide)==2000.f, "the shadow range is clamped as the light's range");
  }

// a face from the first frame on: placed once, then read from the cache, drawn only when dirty
void faceLife() {
  uint next = 0;
  require(vsmCacheDecide(0, 0, false, next)==VSM_CACHE_DROP && next==0, "no face, no request: nothing");
  require(vsmCacheDecide(0, 2, false, next)==VSM_CACHE_ALLOC && vsmCacheSize(next)==2 && !vsmCacheValid(next),
          "a new face asks for a place of its size");

  const uint placed = vsmCachePack(37*16+4, 2, 0, 0, true);
  require(vsmCacheSlot(placed)==37*16+4 && vsmCacheSize(placed)==2 && vsmCacheValid(placed), "entry packing");

  uint e = placed;
  for(int frame=0; frame<100; ++frame) {
    require(vsmCacheDecide(e, 2, false, next)==VSM_CACHE_HIT, "an unchanged face stays cached");
    require(vsmCacheSlot(next)==vsmCacheSlot(placed) && vsmCacheSize(next)==2, "a cached face keeps its place");
    e = next;
    }
  require(vsmCacheDecide(e, 2, true, next)==VSM_CACHE_DRAW && vsmCacheSlot(next)==vsmCacheSlot(placed),
          "a dirty face is drawn again in its place");

  // out of the frame: kept a while (not shown), dropped when dirty or after the idle frames
  e = placed;
  for(uint frame=1; frame<VSM_CACHE_IDLE_FRAMES; ++frame) {
    require(vsmCacheDecide(e, 0, false, next)==VSM_CACHE_IDLE, "an unrequested face keeps its place a while");
    e = next;
    }
  require(vsmCacheDecide(e, 0, false, next)==VSM_CACHE_DROP && next==0, "an unrequested face goes in the end");
  require(vsmCacheDecide(placed, 0, true, next)==VSM_CACHE_DROP, "an unrequested face that changed goes at once");

  // back in the frame after a few idle frames: still cached
  e = placed;
  for(int frame=0; frame<5; ++frame) {
    vsmCacheDecide(e, 0, false, next);
    e = next;
    }
  require(vsmCacheDecide(e, 2, false, next)==VSM_CACHE_HIT, "a face back in the frame is read from the cache");
  }

// the size: grows at once, shrinks only when the smaller request lasts; no flicker at a boundary
void sizeHysteresis() {
  uint next = 0;
  const uint face2 = vsmCachePack(16, 2, 0, 0, true);
  require(vsmCacheDecide(face2, 4, false, next)==VSM_CACHE_ALLOC && vsmCacheSize(next)==4, "a face grows at once");

  uint e = vsmCachePack(0, 4, 0, 0, true);
  for(uint frame=1; frame<VSM_CACHE_SHRINK_FRAMES; ++frame) {
    require(vsmCacheDecide(e, 2, false, next)==VSM_CACHE_HIT && vsmCacheSize(next)==4,
            "a smaller request keeps the size for a while");
    e = next;
    }
  require(vsmCacheDecide(e, 2, false, next)==VSM_CACHE_ALLOC && vsmCacheSize(next)==2,
          "a smaller request that lasts shrinks the face");

  // a request flickering between 2 and 4 at the boundary: one redraw for the growth, then none
  e = vsmCachePack(0, 2, 0, 0, true);
  int allocs = 0;
  for(int frame=0; frame<200; ++frame) {
    const uint req    = frame%2==0 ? 4 : 2;
    const uint action = vsmCacheDecide(e, req, false, next);
    if(action==VSM_CACHE_ALLOC) {
      ++allocs;
      next = vsmCachePack(0, vsmCacheSize(next), 0, 0, true);
      }
    e = next;
    }
  require(allocs==1 && vsmCacheSize(e)==4, "a flickering size is drawn once, not every frame");
  }

// slots: every page of the lamp region once, in the rows below the sun's; blocks are whole squares
void slotsAndBlocks() {
  std::set<uint> pages;
  for(uint slot=0; slot<VSM_CACHE_PAGES; ++slot) {
    const uint p = vsmCachePageId(slot);
    require(p/64>=VSM_CACHE_FIRST_ROW && p/64<64 && p%64<64, "a lamp slot lies in the lamp rows");
    pages.insert(p);
    }
  require(pages.size()==VSM_CACHE_PAGES, "slots map to distinct pages");

  for(uint size : {1u, 2u, 4u}) {
    for(uint slot=0; slot<VSM_CACHE_PAGES; slot+=size*size) {
      const uint p0 = vsmCachePageId(slot);
      std::set<uint> block;
      for(uint k=0; k<size*size; ++k)
        block.insert(vsmCachePageId(slot+k));
      std::set<uint> square;
      for(uint y=0; y<size; ++y)
        for(uint x=0; x<size; ++x)
          square.insert(p0 + x + y*64);
      require(block==square, "a block's slots are the square of pages from its first page");
      }
    }

  // placement: 4x4 faces take whole cells; small faces fill cells in use first
  clearOcc();
  for(uint i=0; i<VSM_CACHE_CELLS; ++i) {
    const int slot = vsmCacheFind(4);
    require(slot==int(i*16), "4x4 faces fill the cells in order");
    vsmCacheMark(uint(slot), 4);
    }
  require(vsmCacheFind(4)<0 && vsmCacheFind(1)<0, "a full region has no place");

  clearOcc();
  const int a = vsmCacheFind(2);
  vsmCacheMark(uint(a), 2);
  const int b = vsmCacheFind(1);
  vsmCacheMark(uint(b), 1);
  const int c = vsmCacheFind(2);
  vsmCacheMark(uint(c), 2);
  require(a==0 && b==4 && c==8, "small faces share a cell");
  const int d = vsmCacheFind(4);
  require(d==16, "a 4x4 face takes a free cell, not the shared one");
  }

// the spheres that reset a face: within the lamp's shadow range and the face's frustum
void sphereFaces() {
  const float range = 500;
  auto faces = [&](vec3 d, float r) {
    uint bits = 0;
    for(uint f=0; f<6; ++f)
      if(vsmCacheSphereTouchesFace(d, r, range, f))
        bits |= 1u<<f;
    return bits;
    };
  require(faces(vec3{200, 0, 0}, 20)==0x1, "a caster on +x reaches the +x face only");
  require(faces(vec3{0, -150, 0}, 10)==0x8, "a caster below reaches the -y face only");
  require(faces(vec3{200, 0, 200}, 30)==(0x1|0x10), "a caster on the diagonal reaches both faces");
  require(faces(vec3{600, 0, 0}, 50)==0, "a caster beyond the shadow range reaches nothing");
  require(faces(vec3{530, 0, 0}, 50)==0x1, "a caster's edge within the range counts");
  require(faces(vec3{0, 0, 0}, 40)==0x3F, "a caster at the lamp reaches every face");
  require(faces(vec3{-300, 10, -20}, 5)==0x2, "a caster on -x reaches the -x face");
  }
}

int main() {
  try {
    shadowRangeIsConstant();
    faceLife();
    sizeHysteresis();
    slotsAndBlocks();
    sphereFaces();
    }
  catch(const std::exception& e) {
    std::printf("{\"test\":\"vsm_cache\",\"passed\":false,\"assertions\":%zu,\"error\":\"%s\"}\n", assertions, e.what());
    return 1;
    }
  std::printf("{\"test\":\"vsm_cache\",\"passed\":true,\"assertions\":%zu}\n", assertions);
  return 0;
  }
