#pragma once
// Batched port of NavigationMemory (OpenGothic/common/game/navigationmemory.cpp)
// and its fused HD readout (OpenGothic/tests/navsim_memory_bench.cpp).
//
// One source, two builds. CUDA runs every agent on one warp (L=32); the host
// build (L=1) runs the same code sequentially and checks logic on a CPU. Rays
// are applied in slot order: inside one ray the DDA cells are distinct, so the
// warp's lanes split them; between rays the warp synchronises. min/max/clamp
// copy libstdc++'s comparisons (not fminf/fmaxf) to keep NaN and signed zero.
// Build with --fmad=false and without fast math.
#include <math.h>
#include <stdint.h>

#if defined(__CUDACC__)
#define GM_HD __host__ __device__ inline
#else
#define GM_HD inline
#endif

namespace gm {

constexpr int NearSize = 64, FarSize = 100;
constexpr float NearCell = 25.f, FarCell = 200.f;
constexpr int ChannelCount = 7, HeightSlots = 5, PointSlots = 4;
enum : int { Free = 0, Clearance = 1, Ground = 2, Wall = 3, Water = 4, Movable = 5, Surface = 6 };
enum : uint32_t { ErrHistory = 1, ErrWaterSort = 2 };
constexpr int NearOutputs = 64 * 64, FarOutputs = 50 * 100, Outputs = NearOutputs + FarOutputs;

// Only fields that a later decision reads are stored: Free and Water keep
// time and path, Clearance adds its height, the four hit channels add the hit
// height and XZ. Cleared cells zero known and times; other fields stay stale
// because every read of them is gated by a known bit.
GM_HD int heightSlot(int ch) {
  return ch == Clearance ? 0 : ch == Ground ? 1 : ch == Wall ? 2 : ch == Movable ? 3 : ch == Surface ? 4 : -1;
}
GM_HD int pointSlot(int ch) {
  return ch == Ground ? 0 : ch == Wall ? 1 : ch == Movable ? 2 : ch == Surface ? 3 : -1;
}

struct GridPtrs {
  uint8_t* known;    // [N*cells]
  uint32_t* time;    // [7][N*cells]
  float* path;       // [7][N*cells]
  float* height;     // [5][N*cells]
  float* hitX;       // [4][N*cells]
  float* hitZ;       // [4][N*cells]
  int32_t* minX;     // [N]
  int32_t* minZ;     // [N]
  uint8_t* positioned;  // [N]
};

// Plain pointers and 64-bit integers only: Python fills the same layout.
struct Ctx {
  int64_t N, R, histCap, reserved;
  // Per-step input: body pose and the eye's 2*238 output slots.
  float* posePos; float* poseFwd; uint32_t* poseMs; float* poseLimits; uint8_t* active;
  float* rayOrigin; float* rayPoint; float* rayDistance; float* rayNormalY;
  uint8_t* rayKind; uint8_t* rayHit; uint32_t* rayTime;
  // Persistent state.
  float* bodyPos; float* bodyFwd; uint32_t* bodyMs; float* bodyLimits; double* pathCm;
  uint8_t* posed; uint8_t* journey;
  uint32_t* stamps;  // [N*R], sampled time + 1; zero is "not seen"
  uint32_t* histT; double* histP; int32_t* histHead; int32_t* histCount;  // ring [N*histCap]
  // Scratch between the two update kernels.
  uint16_t* fresh; int32_t* freshCount; float* freshPath; uint16_t* water; int32_t* waterCount;
  uint8_t* updated; uint8_t* resetFlag; uint32_t* errors;
  // Familiar atlas (shared by agents with the same map) and per-agent window.
  int32_t* mapId; int32_t* famMinX; int32_t* famMinZ; uint8_t* famValid; int32_t* famWindow;
  int32_t* tileX; int32_t* tileZ; float* tileGround; uint8_t* tileChannels; uint64_t* mask;
  int64_t* mapTileBegin; int64_t* mapMaskBegin;
  GridPtrs grid[2];
};

// Recorded eye output and poses, replayed into Ctx by scatter().
struct Table {
  int64_t updates, changed;
  int64_t* begin;  // [updates+1] into the changed-ray arrays
  uint16_t* slot; float* origin; float* point; float* distance; float* normalY;
  uint8_t* kind; uint8_t* hit; uint32_t* time;
  float* pos; float* fwd; uint32_t* ms; float* limits;
};

GM_HD float smin(float a, float b) { return b < a ? b : a; }
GM_HD float smax(float a, float b) { return a < b ? b : a; }
GM_HD double dmax(double a, double b) { return a < b ? b : a; }
GM_HD float sclamp(float v, float lo, float hi) { return v < lo ? lo : hi < v ? hi : v; }
GM_HD int iclamp(int v, int lo, int hi) { return v < lo ? lo : hi < v ? hi : v; }
GM_HD int imax(int a, int b) { return a < b ? b : a; }
GM_HD int imin(int a, int b) { return b < a ? b : a; }
GM_HD int iabs(int v) { return v < 0 ? -v : v; }
GM_HD int cellCoordinate(float value, float cell) { return int(floorf(value / cell)); }
GM_HD int ringCoordinate(int value, int size) {
  const int r = value % size;
  return r < 0 ? r + size : r;
}
GM_HD bool finite3(const float* p) { return isfinite(p[0]) && isfinite(p[1]) && isfinite(p[2]); }
GM_HD float slopeTan(float radians) {
#if defined(__CUDA_ARCH__)
  // libm's tanf is correctly rounded (CORE-MATH); double then round matches.
  return float(tan(double(radians)));
#else
  return tanf(radians);
#endif
}
GM_HD int popcount(uint32_t v) {
#if defined(__CUDA_ARCH__)
  return __popc(v);
#else
  return __builtin_popcount(v);
#endif
}
template <int L> GM_HD void laneSync() {
#if defined(__CUDA_ARCH__)
  if (L > 1) __syncwarp();
#endif
}
template <int L> GM_HD uint32_t laneBallot(bool p) {
#if defined(__CUDA_ARCH__)
  if (L > 1) return __ballot_sync(0xffffffffu, p);
#endif
  return p ? 1u : 0u;
}
template <int L> GM_HD uint32_t laneMin(uint32_t v) {
#if defined(__CUDA_ARCH__)
  if (L > 1)
    for (int offset = 16; offset > 0; offset >>= 1) {
      const uint32_t other = __shfl_xor_sync(0xffffffffu, v, offset);
      v = other < v ? other : v;
    }
#endif
  return v;
}

struct GridView {
  const GridPtrs* g;
  int size;
  float cell;
  int64_t base, stride;  // agent's first cell, distance between channel planes
  int minX, minZ;
  GM_HD bool contains(int x, int z) const { return x >= minX && z >= minZ && x < minX + size && z < minZ + size; }
  GM_HD int64_t at(int x, int z) const {
    return base + int64_t(ringCoordinate(z, size)) * size + ringCoordinate(x, size);
  }
  GM_HD uint32_t& time(int ch, int64_t i) const { return g->time[ch * stride + i]; }
  GM_HD float& path(int ch, int64_t i) const { return g->path[ch * stride + i]; }
  GM_HD float& height(int ch, int64_t i) const { return g->height[heightSlot(ch) * stride + i]; }
  GM_HD float& hitX(int ch, int64_t i) const { return g->hitX[pointSlot(ch) * stride + i]; }
  GM_HD float& hitZ(int ch, int64_t i) const { return g->hitZ[pointSlot(ch) * stride + i]; }
  GM_HD void clear(int64_t i) const {
    g->known[i] = 0;
    for (int ch = 0; ch < ChannelCount; ++ch) time(ch, i) = 0;
  }
};

GM_HD GridView gridView(const Ctx& c, int64_t a, int index) {
  GridView v;
  v.g = &c.grid[index];
  v.size = index ? FarSize : NearSize;
  v.cell = index ? FarCell : NearCell;
  const int64_t cells = int64_t(v.size) * v.size;
  v.base = a * cells;
  v.stride = c.N * cells;
  v.minX = c.grid[index].minX[a];
  v.minZ = c.grid[index].minZ[a];
  return v;
}

// ---- path history (NavigationMemory::pathHistory / pathAt) ----
GM_HD int64_t historySlot(const Ctx& c, int64_t a, int i) {
  int h = c.histHead[a] + i;
  if (h >= c.histCap) h -= int(c.histCap);
  return a * c.histCap + h;
}
GM_HD int historyLower(const Ctx& c, int64_t a, uint32_t t) {
  int lo = 0, hi = c.histCount[a];
  while (lo < hi) {
    const int mid = (lo + hi) / 2;
    if (c.histT[historySlot(c, a, mid)] < t) lo = mid + 1;
    else hi = mid;
  }
  return lo;
}
GM_HD float pathAt(const Ctx& c, int64_t a, uint32_t t) {
  const int i = historyLower(c, a, t);
  if (i == c.histCount[a]) return float(c.pathCm[a]);
  const int64_t next = historySlot(c, a, i);
  if (i == 0 || c.histT[next] == t) return float(c.histP[next]);
  const int64_t previous = historySlot(c, a, i - 1);
  const double fraction = double(t - c.histT[previous]) / double(c.histT[next] - c.histT[previous]);
  return float(c.histP[previous] + (c.histP[next] - c.histP[previous]) * fraction);
}

// ---- update, part 1: pose, history, fresh rays (warp per agent) ----
template <int L> GM_HD void prepareAgent(const Ctx& c, int64_t a, int lane) {
  const float* pos = c.posePos + 3 * a;
  const float* fwd = c.poseFwd + 3 * a;
  if (!c.active[a] || !finite3(pos) || !finite3(fwd)) {
    if (lane == 0) c.updated[a] = 0;
    return;
  }
  const uint32_t ms = c.poseMs[a];
  bool posed = c.posed[a] != 0;
  const bool reset = posed && ms < c.bodyMs[a];
  double path = c.pathCm[a];
  if (reset) {
    // NavigationMemory::reset(); the grids are cleared by ingest().
    for (int64_t i = lane; i < c.R; i += L) c.stamps[a * c.R + i] = 0;
    posed = false;
    path = 0;
  }
  if (posed) {
    const float* body = c.bodyPos + 3 * a;
    const float dx = pos[0] - body[0], dy = pos[1] - body[1], dz = pos[2] - body[2];
    path += double(float(sqrtf(dx * dx + dy * dy + dz * dz)));
  }
  laneSync<L>();
  if (lane == 0) {
    if (reset) {
      c.histHead[a] = 0;
      c.histCount[a] = 0;
    }
    for (int k = 0; k < 3; ++k) {
      c.bodyPos[3 * a + k] = pos[k];
      c.bodyLimits[3 * a + k] = c.poseLimits[3 * a + k];
    }
    const float length = sqrtf(fwd[0] * fwd[0] + fwd[2] * fwd[2]);
    c.bodyFwd[3 * a + 0] = length > 1.e-6f ? fwd[0] / length : 1.f;
    c.bodyFwd[3 * a + 1] = 0.f;
    c.bodyFwd[3 * a + 2] = length > 1.e-6f ? fwd[2] / length : 0.f;
    c.bodyMs[a] = ms;
    c.pathCm[a] = path;
    c.posed[a] = 1;
    const int count = c.histCount[a];
    if (count > 0 && c.histT[historySlot(c, a, count - 1)] == ms) {
      c.histP[historySlot(c, a, count - 1)] = path;
    } else {
      if (count == c.histCap) {
        // Capacity is a stand limit, not engine behaviour; flagged, never silent.
        c.errors[a] |= ErrHistory;
        c.histHead[a] = int((c.histHead[a] + 1) % c.histCap);
        c.histCount[a] = count - 1;
      }
      const int n = c.histCount[a];
      const int64_t slot = historySlot(c, a, n);
      c.histT[slot] = ms;
      c.histP[slot] = path;
      c.histCount[a] = n + 1;
    }
    c.resetFlag[a] = reset ? 1 : 0;
    c.updated[a] = 1;
  }
  laneSync<L>();

  // Fresh rays in slot order; stamps are per slot, so lanes are independent.
  uint32_t oldest = ms;
  int fresh = 0;
  for (int64_t first = 0; first < c.R; first += L) {
    const int64_t i = first + lane;
    bool take = false;
    uint32_t t = 0;
    if (i < c.R) {
      const int64_t k = a * c.R + i;
      t = c.rayTime[k];
      oldest = t < oldest ? t : oldest;
      const uint32_t stamp = c.stamps[k];
      const float d = c.rayDistance[k];
      const bool skip = (stamp != 0 && t <= stamp - 1) || t > ms || !finite3(c.rayOrigin + 3 * k) ||
                        !finite3(c.rayPoint + 3 * k) || !isfinite(d) || d <= 0.f;
      if (!skip) {
        c.stamps[k] = t + 1;
        take = true;
      }
    }
    const uint32_t mask = laneBallot<L>(take);
    if (take) {
      const int at = fresh + popcount(mask & ((1u << lane) - 1u));
      c.fresh[a * c.R + at] = uint16_t(i);
      c.freshPath[a * c.R + at] = pathAt(c, a, t);
    }
    fresh += popcount(mask);
  }
  oldest = laneMin<L>(oldest);
  laneSync<L>();
  if (lane == 0) {
    c.freshCount[a] = fresh;
    // Water observations, deduplicated in order like update().
    int count = 0;
    for (int f = 0; f < fresh; ++f) {
      const int64_t k = a * c.R + c.fresh[a * c.R + f];
      if (!c.rayHit[k] || c.rayKind[k] != 5) continue;
      bool duplicate = false;
      for (int w = 0; w < count && !duplicate; ++w) {
        const int64_t o = a * c.R + c.fresh[a * c.R + c.water[a * c.R + w]];
        const float dx = c.rayPoint[3 * o] - c.rayPoint[3 * k];
        const float dy = c.rayPoint[3 * o + 1] - c.rayPoint[3 * k + 1];
        const float dz = c.rayPoint[3 * o + 2] - c.rayPoint[3 * k + 2];
        duplicate = c.rayTime[o] == c.rayTime[k] && dx * dx + dy * dy + dz * dz < 1.e-4f;
      }
      if (!duplicate) c.water[a * c.R + count++] = uint16_t(f);
    }
    c.waterCount[a] = count;
    // Paths are taken; keep the predecessor of the oldest slot timestamp.
    if (c.R > 0) {
      const int end = historyLower(c, a, oldest);
      if (end > 0) {
        c.histHead[a] = int((c.histHead[a] + end - 1) % c.histCap);
        c.histCount[a] -= end - 1;
      }
    }
  }
}

GM_HD bool clip(float origin, float direction, float low, float high, float& enter, float& exit) {
  if (fabsf(direction) < 1.e-7f) return origin >= low && origin < high;
  float a = (low - origin) / direction, b = (high - origin) / direction;
  if (a > b) {
    const float s = a;
    a = b;
    b = s;
  }
  enter = smax(enter, a);
  exit = smin(exit, b);
  return enter <= exit;
}

struct RayView {
  float ox, oy, oz, px, py, pz, dx, dy, dz, inverse, normalY, observedPath;
  uint32_t t;
  uint8_t kind;
  bool hit;
};

// NavigationMemory::ingest, the free-segment part for one DDA cell.
GM_HD void freeCell(const GridView& v, const RayView& r, int x, int z, float t, float end) {
  const int64_t i = v.at(x, z);
  uint8_t known = v.g->known[i];
  const float a = r.oy + r.dy * t, b = r.oy + r.dy * end;
  const float h = smin(a, b);
  if (!(r.t < v.time(Free, i))) {
    v.time(Free, i) = r.t;
    v.path(Free, i) = r.observedPath;
    known |= 1u << Free;
  }
  const uint32_t clearanceTime = v.time(Clearance, i);
  if (!(r.t < clearanceTime)) {
    const bool knownClearance = (known & (1u << Clearance)) != 0;
    float height = h;
    if (!(knownClearance && height > v.height(Clearance, i))) {
      if (r.t == clearanceTime && knownClearance) height = smin(height, v.height(Clearance, i));
      v.time(Clearance, i) = r.t;
      v.path(Clearance, i) = r.observedPath;
      v.height(Clearance, i) = height;
      known |= 1u << Clearance;
    }
  }
  const float low = smin(a, b), high = smax(a, b);
  const int channels[4] = {Ground, Wall, Movable, Surface};
  for (int n = 0; n < 4; ++n) {
    const int ch = channels[n];
    if ((known & (1u << ch)) == 0 || !(r.t > v.time(ch, i))) continue;
    const float eh = v.height(ch, i);
    if (!(eh >= low - 25.f && eh <= high + 25.f)) continue;
    const float dx = v.hitX(ch, i) - r.ox, dy = eh - r.oy, dz = v.hitZ(ch, i) - r.oz;
    const float along = sclamp((dx * r.dx + dy * r.dy + dz * r.dz) * r.inverse, t, smax(t, end));
    const float ex = dx - r.dx * along, ey = dy - r.dy * along, ez = dz - r.dz * along;
    if (ex * ex + ey * ey + ez * ez <= 625.f) known &= uint8_t(~(1u << ch));
  }
  v.g->known[i] = known;
}

GM_HD void hitCell(const GridView& v, const RayView& r, int x, int z) {
  const int64_t i = v.at(x, z);
  const int ch = r.kind == 3 || r.kind == 4 ? Movable : r.kind == 5 ? Water
               : r.normalY >= .7f ? Ground : fabsf(r.normalY) <= .3f ? Wall : Surface;
  const uint32_t previous = v.time(ch, i);
  if (r.t < previous) return;
  uint8_t known = v.g->known[i];
  const bool knownChannel = (known & (1u << ch)) != 0;
  float height = r.py;
  if (ch == Wall && knownChannel && height > v.height(Wall, i)) return;
  if (ch == Wall && r.t == previous && knownChannel) height = smin(height, v.height(Wall, i));
  v.time(ch, i) = r.t;
  v.path(ch, i) = r.observedPath;
  if (heightSlot(ch) >= 0) v.height(ch, i) = height;
  if (pointSlot(ch) >= 0) {
    v.hitX(ch, i) = r.px;
    v.hitZ(ch, i) = r.pz;
  }
  known |= 1u << ch;
  v.g->known[i] = known;
}

template <int L> GM_HD void ingestRay(const GridView& v, const RayView& r, int lane) {
  const int hitX = cellCoordinate(r.px, v.cell), hitZ = cellCoordinate(r.pz, v.cell);
  float enter = 0, exit = 1;
  const float minX = float(v.minX) * v.cell, minZ = float(v.minZ) * v.cell;
  if (clip(r.ox, r.dx, minX, minX + float(v.size) * v.cell, enter, exit) &&
      clip(r.oz, r.dz, minZ, minZ + float(v.size) * v.cell, enter, exit)) {
    const float start = smin(exit, enter + 1.e-6f);
    int x = cellCoordinate(r.ox + r.dx * start, v.cell);
    int z = cellCoordinate(r.oz + r.dz * start, v.cell);
    x = iclamp(x, v.minX, v.minX + v.size - 1);
    z = iclamp(z, v.minZ, v.minZ + v.size - 1);
    const int stepX = r.dx > 0.f ? 1 : r.dx < 0.f ? -1 : 0;
    const int stepZ = r.dz > 0.f ? 1 : r.dz < 0.f ? -1 : 0;
    const float infinity = INFINITY;
    const float dtx = stepX == 0 ? infinity : v.cell / fabsf(r.dx);
    const float dtz = stepZ == 0 ? infinity : v.cell / fabsf(r.dz);
    float tx = stepX == 0 ? infinity : (float(x + (stepX > 0 ? 1 : 0)) * v.cell - r.ox) / r.dx;
    float tz = stepZ == 0 ? infinity : (float(z + (stepZ > 0 ? 1 : 0)) * v.cell - r.oz) / r.dz;
    float t = enter;
    // Every lane walks the same cells; lane s%L keeps step s and the warp
    // applies one batch of L distinct cells at a time.
    bool pending = false;
    int px = 0, pz = 0;
    float pt = 0, pend = 0;
    for (int step = 0; v.contains(x, z) && t <= exit; ++step) {
      float end = tx;
      if (tz < end) end = tz;
      if (exit < end) end = exit;
      if (!(r.hit && x == hitX && z == hitZ) && step % L == lane) {
        pending = true;
        px = x;
        pz = z;
        pt = t;
        pend = end;
      }
      const bool last = end >= exit;
      if (step % L == L - 1 || last) {
        if (pending) freeCell(v, r, px, pz, pt, pend);
        pending = false;
      }
      if (last) break;
      const bool advanceX = tx <= tz, advanceZ = tz <= tx;
      t = end;
      if (advanceX) {
        x += stepX;
        tx += dtx;
      }
      if (advanceZ) {
        z += stepZ;
        tz += dtz;
      }
    }
    if (pending) freeCell(v, r, px, pz, pt, pend);
  }
  if (!r.hit || !v.contains(hitX, hitZ)) return;
  if (lane == 0) hitCell(v, r, hitX, hitZ);
}

GM_HD RayView rayView(const Ctx& c, int64_t k, float observedPath) {
  RayView r;
  r.ox = c.rayOrigin[3 * k];
  r.oy = c.rayOrigin[3 * k + 1];
  r.oz = c.rayOrigin[3 * k + 2];
  r.px = c.rayPoint[3 * k];
  r.py = c.rayPoint[3 * k + 1];
  r.pz = c.rayPoint[3 * k + 2];
  r.dx = r.px - r.ox;
  r.dy = r.py - r.oy;
  r.dz = r.pz - r.oz;
  const float length = r.dx * r.dx + r.dy * r.dy + r.dz * r.dz;
  r.inverse = length > 1.e-12f ? 1.f / length : 0.f;
  r.normalY = c.rayNormalY[k];
  r.observedPath = observedPath;
  r.t = c.rayTime[k];
  r.kind = c.rayKind[k];
  r.hit = c.rayHit[k] != 0;
  return r;
}

// NavigationMemory::reconstructWater for one grid.
template <int L> GM_HD void reconstructWater(const Ctx& c, int64_t a, const GridView& v, int lane) {
  const int count = c.waterCount[a];
  const int64_t base = a * c.R;
  for (int i = 0; i < count; ++i) {
    const int fa = c.water[base + i];
    const int64_t ka = base + c.fresh[base + fa];
    const float ax = c.rayPoint[3 * ka], ay = c.rayPoint[3 * ka + 1], az = c.rayPoint[3 * ka + 2];
    const uint32_t at = c.rayTime[ka];
    const float path = c.freshPath[base + fa];
    // The six nearest same-sample neighbours, stable by observation order:
    // std::sort is insertion sort up to 16 elements; more is flagged.
    int kept[6];
    float keptDistance[6];
    int keptCount = 0, neighbours = 0;
    for (int j = 0; j < count; ++j) {
      if (i == j) continue;
      const int64_t kb = base + c.fresh[base + c.water[base + j]];
      const float dx = c.rayPoint[3 * kb] - ax, dz = c.rayPoint[3 * kb + 2] - az;
      const float distance = dx * dx + dz * dz;
      if (!(at == c.rayTime[kb] && fabsf(ay - c.rayPoint[3 * kb + 1]) <= 10.f && distance <= 100.f * 100.f)) continue;
      ++neighbours;
      int position = keptCount;
      while (position > 0 && distance < keptDistance[position - 1]) --position;
      if (position >= 6) continue;
      const int last = keptCount < 6 ? keptCount : 5;
      for (int m = last; m > position; --m) {
        kept[m] = kept[m - 1];
        keptDistance[m] = keptDistance[m - 1];
      }
      kept[position] = j;
      keptDistance[position] = distance;
      if (keptCount < 6) ++keptCount;
    }
    if (neighbours > 16 && lane == 0) c.errors[a] |= ErrWaterSort;
    for (int j = 0; j < keptCount; ++j)
      for (int k = j + 1; k < keptCount; ++k) {
        if (kept[j] < i || kept[k] < i) continue;
        const int64_t kb = base + c.fresh[base + c.water[base + kept[j]]];
        const int64_t kc = base + c.fresh[base + c.water[base + kept[k]]];
        const float bxw = c.rayPoint[3 * kb], byw = c.rayPoint[3 * kb + 1], bzw = c.rayPoint[3 * kb + 2];
        const float cxw = c.rayPoint[3 * kc], cyw = c.rayPoint[3 * kc + 1], czw = c.rayPoint[3 * kc + 2];
        const float bx = bxw - ax, bz = bzw - az, cx = cxw - ax, cz = czw - az;
        const float edgeX = cxw - bxw, edgeZ = czw - bzw;
        if (edgeX * edgeX + edgeZ * edgeZ > 100.f * 100.f || fabsf(byw - cyw) > 10.f) continue;
        const float area = bx * cz - bz * cx;
        if (fabsf(area) < 1.f) continue;
        float lowX = ax, highX = ax, lowZ = az, highZ = az;
        if (bxw < lowX) lowX = bxw;
        if (cxw < lowX) lowX = cxw;
        if (highX < bxw) highX = bxw;
        if (highX < cxw) highX = cxw;
        if (bzw < lowZ) lowZ = bzw;
        if (czw < lowZ) lowZ = czw;
        if (highZ < bzw) highZ = bzw;
        if (highZ < czw) highZ = czw;
        const int x0 = imax(v.minX, cellCoordinate(lowX, v.cell));
        const int x1 = imin(v.minX + v.size - 1, cellCoordinate(highX, v.cell));
        const int z0 = imax(v.minZ, cellCoordinate(lowZ, v.cell));
        const int z1 = imin(v.minZ + v.size - 1, cellCoordinate(highZ, v.cell));
        const int width = x1 - x0 + 1, cells = width > 0 && z1 >= z0 ? width * (z1 - z0 + 1) : 0;
        for (int q = lane; q < cells; q += L) {
          const int x = x0 + q % width, z = z0 + q / width;
          const float px = (float(x) + .5f) * v.cell, pz = (float(z) + .5f) * v.cell;
          const float dx = px - ax, dz = pz - az;
          const float u = (dx * cz - dz * cx) / area, w = (bx * dz - bz * dx) / area;
          if (u < 0.f || w < 0.f || u + w > 1.f) continue;
          const float height = ay + u * (byw - ay) + w * (cyw - ay);
          const int64_t cell = v.at(x, z);
          const uint8_t known = v.g->known[cell];
          if ((known & (1u << Ground)) != 0 && v.height(Ground, cell) > height + 2.f) continue;
          if ((known & (1u << Water)) != 0 && v.time(Water, cell) > at) continue;
          v.time(Water, cell) = at;
          v.path(Water, cell) = path;
          v.g->known[cell] = uint8_t(known | (1u << Water));
        }
        laneSync<L>();
      }
  }
}

// ---- update, part 2: one warp per (agent, grid) ----
template <int L> GM_HD void ingestAgent(const Ctx& c, int64_t a, int index, int lane) {
  if (!c.updated[a]) return;
  GridView v = gridView(c, a, index);
  const GridPtrs& g = c.grid[index];
  const int64_t cells = int64_t(v.size) * v.size;
  // Grid::center(); a reset leaves the grid unpositioned.
  const bool positioned = !c.resetFlag[a] && g.positioned[a] != 0;
  const int nextX = cellCoordinate(c.bodyPos[3 * a], v.cell) - v.size / 2;
  const int nextZ = cellCoordinate(c.bodyPos[3 * a + 2], v.cell) - v.size / 2;
  if (!(positioned && nextX == v.minX && nextZ == v.minZ)) {
    if (!positioned || iabs(nextX - v.minX) >= v.size || iabs(nextZ - v.minZ) >= v.size) {
      for (int64_t i = lane; i < cells; i += L) v.clear(v.base + i);
    } else {
      for (int x = nextX; x < nextX + v.size; ++x)
        if (x < v.minX || x >= v.minX + v.size)
          for (int z = nextZ + lane; z < nextZ + v.size; z += L) v.clear(v.at(x, z));
      for (int z = nextZ; z < nextZ + v.size; ++z)
        if (z < v.minZ || z >= v.minZ + v.size)
          for (int x = nextX + lane; x < nextX + v.size; x += L) v.clear(v.at(x, z));
    }
    v.minX = nextX;
    v.minZ = nextZ;
    laneSync<L>();
    if (lane == 0) {
      g.minX[a] = nextX;
      g.minZ[a] = nextZ;
      g.positioned[a] = 1;
    }
  }
  laneSync<L>();
  // refreshFamiliarWindow(): ring slots of the far grid point to atlas tiles.
  const int map = c.mapId[a];
  if (index == 1 && map >= 0 && !(c.famValid[a] && c.famMinX[a] == v.minX && c.famMinZ[a] == v.minZ)) {
    for (int64_t i = lane; i < cells; i += L) c.famWindow[v.base + i] = -1;
    laneSync<L>();
    const int64_t first = c.mapTileBegin[map], last = c.mapTileBegin[map + 1];
    const uint64_t* mask = c.mask + c.mapMaskBegin[map];
    for (int x = v.minX + lane; x < v.minX + v.size; x += L) {
      int64_t lo = first, hi = last;
      while (lo < hi) {
        const int64_t mid = lo + (hi - lo) / 2;
        if (c.tileX[mid] < x || (c.tileX[mid] == x && c.tileZ[mid] < v.minZ)) lo = mid + 1;
        else hi = mid;
      }
      for (int64_t t = lo; t < last && c.tileX[t] == x && c.tileZ[t] < v.minZ + v.size; ++t) {
        const int64_t local = t - first;
        if ((mask[local / 64] & (uint64_t(1) << (local % 64))) != 0)
          c.famWindow[v.base + int64_t(ringCoordinate(c.tileZ[t], v.size)) * v.size + ringCoordinate(x, v.size)] = int32_t(t);
      }
    }
    laneSync<L>();
    if (lane == 0) {
      c.famValid[a] = 1;
      c.famMinX[a] = v.minX;
      c.famMinZ[a] = v.minZ;
    }
  }
  const int count = c.freshCount[a];
  for (int f = 0; f < count; ++f) {
    const int64_t k = a * c.R + c.fresh[a * c.R + f];
    ingestRay<L>(v, rayView(c, k, c.freshPath[a * c.R + f]), lane);
    laneSync<L>();
  }
  reconstructWater<L>(c, a, v, lane);
}

// ---- readout: classify + fused HD planes, one thread per output cell ----
struct Sample {
  float confidence, free, wall, ledge, drop, water, movable, familiar, ground;
};

GM_HD Sample emptySample() {
  Sample s;
  s.confidence = s.free = s.wall = s.ledge = s.drop = s.water = s.movable = s.familiar = 0.f;
  s.ground = NAN;
  return s;
}

struct Body {
  float x, y, z, fx, fz;
  uint32_t ms;
  double path;
  bool journey;
  float step, slope, bodyHeight, rawBodyHeight;
};

GM_HD Body body(const Ctx& c, int64_t a) {
  Body b;
  b.x = c.bodyPos[3 * a];
  b.y = c.bodyPos[3 * a + 1];
  b.z = c.bodyPos[3 * a + 2];
  b.fx = c.bodyFwd[3 * a];
  b.fz = c.bodyFwd[3 * a + 2];
  b.ms = c.bodyMs[a];
  b.path = c.pathCm[a];
  b.journey = c.journey[a] != 0;
  const float* limits = c.bodyLimits + 3 * a;
  b.step = isfinite(limits[0]) && limits[0] > 0.f ? limits[0] : 50.f;
  const float slope = isfinite(limits[1]) ? sclamp(limits[1], 0.f, 89.f) : 45.f;
  b.slope = slopeTan(slope * 3.14159265358979323846f / 180.f);
  b.bodyHeight = isfinite(limits[2]) && limits[2] > 0.f ? limits[2] : 180.f;
  return b;
}

GM_HD float confidence(const Body& b, uint32_t t, float path, bool movable) {
#if defined(GM_FLOAT_CONFIDENCE)
  // Measurement variant only: what the readout costs without FP64 (GA102
  // runs it at 1/64 of FP32). Not bitwise; the bench reports its parity.
  const float age = float(b.ms - t) * .001f;
  const float distance = smax(0.f, float(b.path) - path);
  const float tau = movable ? 5.f : b.journey ? 300.f : 30.f;
  const float lambda = b.journey ? 30000.f : 3000.f;
  return expf(-age / tau - distance / lambda);
#else
  const double age = double(b.ms - t) * .001;
  const double distance = dmax(0., b.path - double(path));
  const double tau = movable ? 5. : b.journey ? 300. : 30.;
  const double lambda = b.journey ? 30000. : 3000.;
  return float(exp(-age / tau - distance / lambda));
#endif
}

GM_HD Sample classify(const GridView& v, const Body& b, int x, int z) {
  Sample out = emptySample();
  const int64_t i = v.at(x, z);
  const uint8_t known = v.g->known[i];
  if (known == 0) return out;
  float c[ChannelCount];
  for (int ch = 0; ch < ChannelCount; ++ch) {
    c[ch] = 0.f;
    if ((known & (1u << ch)) != 0) {
      c[ch] = confidence(b, v.time(ch, i), v.path(ch, i), ch == Movable);
      out.confidence = smax(out.confidence, c[ch]);
    }
  }
  out.free = smax(out.free, c[Free]);
  bool overheadWall = false;
  if (c[Wall] > 0.f) {
    const float wallX = v.hitX(Wall, i), wallZ = v.hitZ(Wall, i), wallHeight = v.height(Wall, i);
    float nearest = INFINITY;
    for (int nz = z - 1; nz <= z + 1; ++nz)
      for (int nx = x - 1; nx <= x + 1; ++nx) {
        if (!v.contains(nx, nz)) continue;
        const int64_t n = v.at(nx, nz);
        if ((v.g->known[n] & (1u << Ground)) == 0) continue;
        const float gx = v.hitX(Ground, n) - wallX, gz = v.hitZ(Ground, n) - wallZ;
        const float distance = gx * gx + gz * gz;
        if (distance < nearest) {
          nearest = distance;
          overheadWall = wallHeight > v.height(Ground, n) + b.bodyHeight;
        }
      }
  }
  if (!overheadWall) out.wall = smax(out.wall, c[Wall]);
  out.water = smax(out.water, c[Water]);
  out.movable = smax(out.movable, c[Movable]);
  if (c[Ground] > 0.f) {
    const float height = v.height(Ground, i), gx = v.hitX(Ground, i), gz = v.hitZ(Ground, i);
    out.ground = height;
    out.free = smax(out.free, c[Ground]);
    for (int nz = z - 1; nz <= z + 1; ++nz)
      for (int nx = x - 1; nx <= x + 1; ++nx) {
        if ((nx == x && nz == z) || !v.contains(nx, nz)) continue;
        const int64_t n = v.at(nx, nz);
        if ((v.g->known[n] & (1u << Ground)) == 0) continue;
        const float difference = height - v.height(Ground, n);
        const float dx = gx - v.hitX(Ground, n), dz = gz - v.hitZ(Ground, n);
        const float distance = sqrtf(dx * dx + dz * dz);
        if (fabsf(difference) <= b.step || fabsf(difference) <= distance * b.slope) continue;
        const float certainty = smin(c[Ground], confidence(b, v.time(Ground, n), v.path(Ground, n), false));
        if (difference > 0.f) out.ledge = smax(out.ledge, certainty);
        else out.drop = smax(out.drop, certainty);
      }
  }
  return out;
}

GM_HD Sample familiarCell(const Ctx& c, const Body& b, int64_t tile, const Sample& live) {
  if (live.confidence >= .35f) return live;
  const float bodyHeight = b.bodyHeight;
  const float tileGround = c.tileGround[tile];
  if (isfinite(live.ground) && isfinite(tileGround) && fabsf(live.ground - tileGround) > bodyHeight) return live;
  const uint8_t channels = c.tileChannels[tile];
  Sample out = emptySample();
  out.confidence = .35f;
  out.familiar = 1.f;
  out.ground = tileGround;
  out.free = (channels & 1) ? .35f : 0.f;
  out.wall = (channels & 2) ? .35f : 0.f;
  out.ledge = (channels & 4) ? .35f : 0.f;
  out.drop = (channels & 8) ? .35f : 0.f;
  out.water = (channels & 16) ? .35f : 0.f;
  out.movable = live.movable;
  return out;
}

GM_HD float quantize(float value) { return float(lroundf(value * 255.f)) / 255.f; }

// Output cell (vx, vz) of one grid's body-frame view. A rotated equal-pitch
// lattice puts at most a few world centres in one output cell, all within the
// 3x3 world cells around its inverse-mapped centre; they are merged in the
// readout's z-major world order, like NavigationMemory::read.
GM_HD void readCell(const Ctx& c, int64_t a, int index, int vx, int vz, float* out, int64_t plane) {
  const GridView v = gridView(c, a, index);
  const Body b = body(c, a);
  const float rx = -b.fz, rz = b.fx;
  const float half = float(v.size / 2) * v.cell;
  const float ux = (float(vx) + .5f) * v.cell - half, uz = (float(vz) + .5f) * v.cell - half;
  const int ix = cellCoordinate(b.x + ux * rx + uz * b.fx, v.cell);
  const int iz = cellCoordinate(b.z + ux * rz + uz * b.fz, v.cell);
  const bool familiar = index == 1 && c.mapId[a] >= 0 && c.famValid[a];
  Sample o = emptySample();
  for (int z = iz - 1; z <= iz + 1; ++z)
    for (int x = ix - 1; x <= ix + 1; ++x) {
      if (!v.contains(x, z)) continue;
      const int64_t slot = v.at(x, z);
      const int64_t tile = familiar ? c.famWindow[slot] : -1;
      if (v.g->known[slot] == 0 && tile < 0) continue;
      const float dx = (float(x) + .5f) * v.cell - b.x;
      const float dz = (float(z) + .5f) * v.cell - b.z;
      if (cellCoordinate(dx * rx + dz * rz + half, v.cell) != vx ||
          cellCoordinate(dx * b.fx + dz * b.fz + half, v.cell) != vz) continue;
      Sample s = classify(v, b, x, z);
      if (tile >= 0) s = familiarCell(c, b, tile, s);
      if (s.confidence <= 0.f) continue;
      if (o.confidence > 0.f && o.familiar != s.familiar) {
        if (o.familiar == 0.f && o.confidence >= .35f) continue;
        if (s.familiar == 0.f && s.confidence >= .35f) o = emptySample();
      }
      o.confidence = smax(o.confidence, s.confidence);
      o.free = smax(o.free, s.free);
      o.wall = smax(o.wall, s.wall);
      o.ledge = smax(o.ledge, s.ledge);
      o.drop = smax(o.drop, s.drop);
      o.water = smax(o.water, s.water);
      o.movable = smax(o.movable, s.movable);
      o.familiar = smax(o.familiar, s.familiar);
      if (isfinite(s.ground)) o.ground = s.ground;
    }
  // Quantisation is monotone, so Q(max) equals the fused path's max(Q).
  out[0 * plane] = quantize(o.confidence);
  out[1 * plane] = quantize(o.free);
  out[2 * plane] = quantize(o.wall);
  out[3 * plane] = quantize(o.ledge);
  out[4 * plane] = quantize(o.drop);
  out[5 * plane] = quantize(o.water);
  out[6 * plane] = quantize(o.movable);
  const bool valid = isfinite(o.ground);
  const float relative = valid ? sclamp((o.ground - b.y) / 200.f, -10.f, 10.f) : 0.f;
  out[7 * plane] = relative;
  out[8 * plane] = relative;
  out[9 * plane] = valid ? 1.f : 0.f;
}

// Output k of a readout batch: near CHW [k][10][64][64], far CHW [k][10][50][100].
GM_HD void readOutput(const Ctx& c, int64_t a, int64_t k, int r, float* near, float* far) {
  if (r < NearOutputs) {
    readCell(c, a, 0, r % 64, r / 64, near + k * 10 * NearOutputs + r, NearOutputs);
  } else {
    r -= NearOutputs;
    readCell(c, a, 1, r % 100, 50 + r / 100, far + k * 10 * FarOutputs + r, FarOutputs);
  }
}

// Bench-only stand-in for the eye: copy one recorded update into the slots.
template <int L> GM_HD void scatterAgent(const Ctx& c, const Table& t, int64_t a, int64_t u, int lane) {
  if (u < 0) {
    if (lane == 0) c.active[a] = 0;
    return;
  }
  for (int64_t e = t.begin[u] + lane; e < t.begin[u + 1]; e += L) {
    const int64_t k = a * c.R + t.slot[e];
    for (int n = 0; n < 3; ++n) {
      c.rayOrigin[3 * k + n] = t.origin[3 * e + n];
      c.rayPoint[3 * k + n] = t.point[3 * e + n];
    }
    c.rayDistance[k] = t.distance[e];
    c.rayNormalY[k] = t.normalY[e];
    c.rayKind[k] = t.kind[e];
    c.rayHit[k] = t.hit[e];
    c.rayTime[k] = t.time[e];
  }
  if (lane == 0) {
    for (int n = 0; n < 3; ++n) {
      c.posePos[3 * a + n] = t.pos[3 * u + n];
      c.poseFwd[3 * a + n] = t.fwd[3 * u + n];
      c.poseLimits[3 * a + n] = t.limits[3 * u + n];
    }
    c.poseMs[a] = t.ms[u];
    c.active[a] = 1;
  }
}

}  // namespace gm
