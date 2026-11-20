#pragma once
// NavigationPassage::check over the current body-frame memory view. Memory
// updates stay in memory_kernels.h; this file only reads their persistent state.
#include "memory_kernels.h"

namespace gm {
namespace passage {
enum : uint8_t { Unknown = 0, Free = 1, Closed = 2 };

GM_HD bool byteNonzero(float value) {
  return lroundf(sclamp(value, 0.f, 1.f) * 255.f) != 0;
}

// Same z-major splat as NavigationMemory::read and gm::readCell, retaining
// familiar (the policy's ten input planes do not carry that channel).
GM_HD Sample cell(const Ctx& c, const GridView& v, const Body& b, bool familiar, int vx, int vz) {
  const float rx = -b.fz, rz = b.fx;
  const float half = float(v.size / 2) * v.cell;
  const float ux = (float(vx) + .5f) * v.cell - half, uz = (float(vz) + .5f) * v.cell - half;
  const int ix = cellCoordinate(b.x + ux * rx + uz * b.fx, v.cell);
  const int iz = cellCoordinate(b.z + ux * rz + uz * b.fz, v.cell);
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
      o.familiar = smax(o.familiar, s.familiar);
    }
  return o;
}

GM_HD bool hazard(const Sample& c) {
  return !byteNonzero(c.familiar) &&
      (byteNonzero(c.wall) || byteNonzero(c.ledge) || byteNonzero(c.drop) || byteNonzero(c.water));
}
GM_HD bool seenFree(const Sample& c) {
  return !byteNonzero(c.familiar) && byteNonzero(c.confidence) && byteNonzero(c.free) && !hazard(c);
}

struct NearSample { bool hazard = false, free = false; };
struct NearMemo {
  int key = -1;
  NearSample sample;
};

template <int L>
GM_HD NearSample near(const Ctx& c, const GridView& v, const Body& b, float r, float f, float corridor,
                      int lane, unsigned mask, NearMemo& memo) {
  const int x0 = imax(0, int(floorf((r - corridor) / NearCell + float(NearSize) * .5f)));
  const int x1 = imin(NearSize - 1, int(floorf((r + corridor) / NearCell + float(NearSize) * .5f)));
  const int z0 = imax(0, int(floorf((f - corridor) / NearCell + float(NearSize) * .5f)));
  const int z1 = imin(NearSize - 1, int(floorf((f + corridor) / NearCell + float(NearSize) * .5f)));
  // The same (x,z) belongs to the same lane as the corridor moves. Adjacent
  // samples usually reuse most of the previous 3x3 box. Wide corridors visit
  // additional cells at the same lane's stride.
  constexpr int stride = L == 1 ? 1 : 4;
  const int startX = x0 + ((lane % stride - x0) & (stride - 1));
  const int startZ = z0 + ((lane / stride - z0) & (stride - 1));
  NearSample out;
  for (int z = startZ; z <= z1 && !out.hazard; z += stride)
    for (int x = startX; x <= x1; x += stride) {
      const float cr = (float(x) + .5f - float(NearSize) * .5f) * NearCell;
      const float cf = (float(z) + .5f - float(NearSize) * .5f) * NearCell;
      const float dx = smax(0.f, fabsf(r - cr) - NearCell * .5f);
      const float dz = smax(0.f, fabsf(f - cf) - NearCell * .5f);
      if (dx * dx + dz * dz > corridor * corridor) continue;
      const int key = z * NearSize + x;
      if (memo.key != key) {
        const auto sample = cell(c, v, b, false, x, z);
        const bool closed = hazard(sample);
        memo = {key, {closed, !closed && seenFree(sample)}};
      }
      out.hazard = out.hazard || memo.sample.hazard;
      out.free = out.free || memo.sample.free;
      if (out.hazard) break;
    }
#if defined(__CUDA_ARCH__)
  if constexpr (L > 1) {
    out.hazard = __any_sync(mask, out.hazard);
    out.free = __any_sync(mask, out.free);
  }
#endif
  return out;
}

// q: world-space from.xyz, to.xyz, corridorCm, footprintCm.
template <int L = 1>
GM_HD uint8_t check(const Ctx& c, int64_t agent, const float* q, int lane = 0, unsigned mask = 0) {
  static_assert(L == 1 || L == 16);
  if (!c.posed[agent]) return Unknown;
  const Body b = body(c, agent);
  // Frame normalises the view's already normalised forward once again.
  const float norm = sqrtf(b.fx * b.fx + b.fz * b.fz);
  if (!(norm > 1.e-3f)) return Unknown;
  const float inverse = 1.f / norm;
  const float fx = b.fx * inverse, fz = b.fz * inverse;
  const float ax = q[0] - b.x, az = q[2] - b.z;
  const float bx = q[3] - b.x, bz = q[5] - b.z;
  const float ar = ax * -fz + az * fx, af = ax * fx + az * fz;
  const float br = bx * -fz + bz * fx, bf = bx * fx + bz * fz;
  const float dr = br - ar, df = bf - af;
  const float length = float(hypot(double(dr), double(df)));
  const float corridor = q[6], footprint = q[7];
  const float nearHalf = NearCell * float(NearSize) * .5f - corridor;
  const float farHalf = FarCell * float(FarSize) * .5f;
  const auto nearGrid = gridView(c, agent, 0);
  const auto farGrid = gridView(c, agent, 1);
  const bool familiar = c.mapId[agent] >= 0 && c.famValid[agent];
  NearMemo memo;
  bool free = true;
  float along = 0.f;
  while (true) {
    const float t = length > 0.f ? smin(along, length) / length : 0.f;
    const float r = ar + dr * t, f = af + df * t;
    float step = FarCell * .5f;
    if (along >= footprint) {
      if (fabsf(r) <= nearHalf && fabsf(f) <= nearHalf) {
        const auto sample = near<L>(c, nearGrid, b, r, f, corridor, lane, mask, memo);
        if (sample.hazard) return Closed;
        free = free && sample.free;
        step = NearCell * .5f;
      } else if (fabsf(r) < farHalf && fabsf(f) < farHalf) {
        const int x = int(floorf(r / FarCell + float(FarSize) * .5f));
        const int z = int(floorf(f / FarCell + float(FarSize) * .5f));
        if (x >= 0 && x < FarSize && z >= 0 && z < FarSize) {
          int verdict = Unknown;
          if (lane == 0) {
            const auto sample = cell(c, farGrid, b, familiar, x, z);
            if (hazard(sample) && !byteNonzero(sample.free)) verdict = Closed;
            else if (seenFree(sample)) verdict = Free;
          }
#if defined(__CUDA_ARCH__)
          if constexpr (L > 1) verdict = __shfl_sync(mask, verdict, 0, L);
#endif
          if (verdict == Closed) return Closed;
          free = free && verdict == Free;
        } else free = false;
      } else free = false;
    } else if (fabsf(r) <= nearHalf && fabsf(f) <= nearHalf) step = NearCell * .5f;
    if (along >= length) break;
    along = smin(along + step, length);
  }
  return free ? Free : Unknown;
}
}  // namespace passage
}  // namespace gm
