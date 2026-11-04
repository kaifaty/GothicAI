#ifndef VSM_CACHE_GLSL
#define VSM_CACHE_GLSL

// The lamp-face cache of the virtual shadow map ([INTERNAL] vsmCache): a lamp's cube face keeps its place in the
// atlas from frame to frame and is drawn again only when something in it changed
// (reports/render-features/2026-11-04-vsm-cache/step-2b.md). These are the decisions of the passes
// vsm_cache_lights.comp, vsm_cache_invalidate.comp and vsm_cache_alloc.comp. The file is plain enough to compile as
// C++ as well: tests/vsm_cache_test.cpp includes it with its own uint, vec3, atomicOr and INOUT.

#ifndef INOUT
#define INOUT(T) inout T
#endif

// The atlas holds 64x64 pages; the sun takes rows [0..32), lamp faces rows [32..64) in 4x4-page cells
const uint VSM_CACHE_FIRST_ROW     = 32u;
const uint VSM_CACHE_CELLS         = 128u;  // 16 cells a row, 8 rows of cells
const uint VSM_CACHE_PAGES         = VSM_CACHE_CELLS*16u;
// an unrequested face keeps its place (not shown, not drawn) this many frames, for a camera that turns back
const uint VSM_CACHE_IDLE_FRAMES   = 60u;
// a face shrinks only once its smaller request has lasted this many frames: no flicker of the size at a boundary
const uint VSM_CACHE_SHRINK_FRAMES = 30u;

// What a face does in a frame
const uint VSM_CACHE_DROP  = 0u; // no place: not requested for long, or changed while not requested
const uint VSM_CACHE_IDLE  = 1u; // keeps its place, not requested this frame
const uint VSM_CACHE_HIT   = 2u; // requested and unchanged: read from the cache, not drawn
const uint VSM_CACHE_DRAW  = 3u; // requested, keeps its place, drawn again
const uint VSM_CACHE_ALLOC = 4u; // requested, needs a new place (new, or its size changed), drawn

// A face's entry: valid 31 | shrink frames 21..27 | idle frames 14..20 | size code 11..13 | slot 0..10.
// The slot is a page of the lamp region: cell*16 + the page's Morton index in the cell; a block starts at a slot
// aligned to its page count. Size code 0: none, 1: 1x1, 2: 2x2, 3: 4x4 pages.
uint vsmCacheSlot  (uint e) { return e & 0x7FFu; }
uint vsmCacheIdle  (uint e) { return (e >> 14u) & 0x7Fu; }
uint vsmCacheShrink(uint e) { return (e >> 21u) & 0x7Fu; }
bool vsmCacheValid (uint e) { return (e >> 31u)!=0u; }

uint vsmCacheSize(uint e) {
  const uint code = (e >> 11u) & 0x7u;
  return code==0u ? 0u : (1u << (code-1u));
  }

uint vsmCachePack(uint slot, uint size, uint idle, uint shrink, bool valid) {
  const uint code = size>=4u ? 3u : (size>=2u ? 2u : (size>=1u ? 1u : 0u));
  return (slot & 0x7FFu) | (code << 11u) | (min(idle, 0x7Fu) << 14u) | (min(shrink, 0x7Fu) << 21u) |
         (valid ? 0x80000000u : 0u);
  }

// e: the face's entry from the last frame; req: pages a side the frame asks for (0: none, 1, 2, 4);
// dirty: something that casts into the face moved, or the lamp moved. The new entry goes to next
// (for VSM_CACHE_ALLOC: not valid yet, with the size to place)
uint vsmCacheDecide(uint e, uint req, bool dirty, INOUT(uint) next) {
  if(!vsmCacheValid(e)) {
    next = req==0u ? 0u : vsmCachePack(0u, req, 0u, 0u, false);
    return req==0u ? VSM_CACHE_DROP : VSM_CACHE_ALLOC;
    }
  const uint size = vsmCacheSize(e);
  if(req==0u) {
    const uint idle = vsmCacheIdle(e) + 1u;
    if(dirty || idle>=VSM_CACHE_IDLE_FRAMES) {
      next = 0u;
      return VSM_CACHE_DROP;
      }
    next = vsmCachePack(vsmCacheSlot(e), size, idle, 0u, true);
    return VSM_CACHE_IDLE;
    }

  // a larger request grows the face at once; a smaller one has to last
  uint target = size;
  uint shrink = 0u;
  if(req>size) {
    target = req;
    }
  else if(req<size) {
    shrink = vsmCacheShrink(e) + 1u;
    if(shrink>=VSM_CACHE_SHRINK_FRAMES) {
      target = req;
      shrink = 0u;
      }
    }
  if(target!=size) {
    next = vsmCachePack(0u, target, 0u, 0u, false);
    return VSM_CACHE_ALLOC;
    }
  next = vsmCachePack(vsmCacheSlot(e), size, 0u, shrink, true);
  return dirty ? VSM_CACHE_DRAW : VSM_CACHE_HIT;
  }

// The atlas page (VSM_PAGE_PER_ROW = 64 a row) of a block's first page
uint vsmCachePageId(uint slot) {
  const uint cell = slot >> 4u;
  const uint m    = slot & 15u;
  const uint x    = (m & 1u) | (((m >> 2u) & 1u) << 1u);
  const uint y    = ((m >> 1u) & 1u) | (((m >> 3u) & 1u) << 1u);
  const uint col  = (cell%16u)*4u + x;
  const uint row  = VSM_CACHE_FIRST_ROW + (cell/16u)*4u + y;
  return col + row*64u;
  }

// A sphere of shadow casters (dvec: its centre from the lamp, r: its radius) can reach the lamp's cube face:
// within the lamp's shadow range and inside the face's 90-degree frustum widened by the radius (as
// vsm_visibility_pass.comp culls clusters)
bool vsmCacheSphereTouchesFace(vec3 dvec, float r, float range, uint face) {
  const float rr = r + range;
  if(dot(dvec,dvec) > rr*rr)
    return false;
  const uint  axis = face/2u;
  const float s    = (face%2u)==0u ? 1.0f : -1.0f;
  const float a    = s*(axis==0u ? dvec.x : (axis==1u ? dvec.y : dvec.z));
  const float b    = axis==0u ? dvec.y : dvec.x;
  const float c    = axis==2u ? dvec.y : dvec.z;
  const float w    = r*1.41421356237f;
  return abs(b)-a <= w && abs(c)-a <= w;
  }

#if !defined(VSM_CACHE_NO_PLACEMENT)
// Placement in the lamp region's occupancy (a bit per page, a cell's 16 pages in one half of a word): the includer
// declares uint vsmCacheOcc[VSM_CACHE_CELLS/2]
uint vsmCacheBlockBits(uint size) {
  const uint n = size*size;
  return n>=16u ? 0xFFFFu : ((1u << n) - 1u);
  }

uint vsmCacheCellBits(uint cell) {
  return (vsmCacheOcc[cell >> 1u] >> ((cell & 1u)*16u)) & 0xFFFFu;
  }

void vsmCacheMark(uint slot, uint size) {
  const uint cell = slot >> 4u;
  atomicOr(vsmCacheOcc[cell >> 1u], vsmCacheBlockBits(size) << ((slot & 15u) + (cell & 1u)*16u));
  }

// A free slot for a block of size x size pages, or -1. Small blocks go into cells already in use first, so that
// whole cells stay free for 4x4 faces
int vsmCacheFind(uint size) {
  const uint n    = size*size;
  const uint bits = vsmCacheBlockBits(size);
  for(uint pass=0u; pass<2u; ++pass) {
    for(uint cell=0u; cell<VSM_CACHE_CELLS; ++cell) {
      const uint used = vsmCacheCellBits(cell);
      if(used==0xFFFFu)
        continue;
      if(pass==0u && used==0u && n<16u)
        continue;
      if(pass==1u && used!=0u)
        continue;
      for(uint at=0u; at<16u; at+=n) {
        if(((used >> at) & bits)==0u)
          return int(cell*16u + at);
        }
      }
    }
  return -1;
  }
#endif

#endif
