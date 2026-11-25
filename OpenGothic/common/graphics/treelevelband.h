#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "graphics/meshlevels.h"
#include "graphics/treeboughasset.h"

// The level switches of a tree with a bough hierarchy (OPENGOTHIC_TREE_BOUGH_LOD; reports/engine/2026-11-16-modern-trees/
// tree-bough-lod.md, «Плавная смена древесины и тени»). A threshold of its own GLBs (the main view's `levels` px) and
// of its shadow scale (ShadowPx) is a band in the proportions of the impostor
// joint: from px*56/49 down to px*46/49 the coarser GLB takes the share TreeBough::impostorJointWeight(px, height) of
// the samples and the finer one the rest. The thresholds lie 56/46 times apart or more, so at most one band of a
// scale is active.
// The object's cluster draws the finer side, a second cluster the coarser one; the word in the static instance's
// deform slot (a static tree is never deformed) tells the masks the two shares and the side:
// tree_bough_dissolve.glsl. The bands are reversible and need no hysteresis.
namespace TreeLevelBand {

constexpr uint32_t Tag    = 0x80000000u;
constexpr uint32_t Coarse = 0x40000000u;
constexpr uint32_t Bits   = 15;
constexpr uint32_t Full   = (1u << Bits) - 1u;

// The GLBs at a height: inside a band `weight` is the coarser one's share of the samples, outside it fine==coarse
struct Pick {
  uint8_t fine   = 0;
  uint8_t coarse = 0;
  float   weight = 0;

  bool active() const { return fine!=coarse && weight>0.f && weight<1.f; }
  };

// The step `levels[0]` above px[0], ..., `levels[count]` below px[count-1] (px falling), its steps as bands
inline Pick pick(const float* px, const uint8_t* levels, size_t count, float height) {
  for(size_t i=0; i<count; ++i) {
    const float weight = TreeBough::impostorJointWeight(px[i], height);
    if(weight>=1.f)
      continue;
    if(weight>0.f && levels[i]!=levels[i+1])
      return {levels[i], levels[i+1], weight};
    return {levels[i], levels[i], 0.f};
    }
  return {levels[count], levels[count], 0.f};
  }

// The main view's GLB of `count` own levels (StaticMesh::levels), 0 the mesh itself
inline Pick main(const float* px, size_t count, float height) {
  uint8_t levels[MeshLevels::MaxLevels+1] = {};
  count = std::min(count, MeshLevels::MaxLevels);
  for(size_t i=0; i<=count; ++i)
    levels[i] = uint8_t(i);
  return pick(px, levels, count, height);
  }

// The shadow scale, a ladder in place of the step of MeshLevels::pickShadow from the visual GLB to the third level,
// which changes 22 % of a spruce's shadow at once («Лестница теней ближней ели»): the visual GLB above ShadowPx[0],
// then the first, the second and the fourth level (as many as there are, none finer than the visual GLB). The third
// level is left out: its cards are the fattest, the shadow would grow by a quarter and shrink again one step later.
// The last, largest step lies under 300 px, where a share of the shadow is as many pixels as at the steps above
constexpr float ShadowPx[] = {MeshLevels::ShadowNearDefaultPx, MeshLevels::ShadowFarPx, 240.f};
static_assert(ShadowPx[0]*46.f>=ShadowPx[1]*56.f && ShadowPx[1]*46.f>=ShadowPx[2]*56.f);

inline Pick shadow(size_t count, float heightAt1080, uint8_t visualGlb) {
  const auto level = [&](size_t i) { return std::max(visualGlb, uint8_t(std::min(i, count))); };
  const uint8_t levels[] = {visualGlb, level(1), level(2), level(4)};
  return pick(ShadowPx, levels, 3, heightAt1080);
  }

enum class MidSun : uint32_t { Native, Whole, Masked, Skip };

// On solar m0–m6 the middle of the crown's ladder uses L4. Where the full crown
// gives way to L1, keep the full side and its mask; only the coarse side takes L4.
inline MidSun midSun(const Pick& band, bool coarse) {
  if(band.fine==0 && band.coarse==1)
    return coarse ? MidSun::Masked : MidSun::Native;
  if(band.fine==1 || band.fine==2)
    return coarse ? MidSun::Skip : MidSun::Whole;
  return MidSun::Native;
  }

inline uint32_t share(const Pick& band) {
  if(!band.active())
    return 0;
  return std::clamp(uint32_t(std::lround(band.weight*float(Full))), 1u, Full-1u);
  }

// The instance word of one side: 0 outside both bands, so the masks draw the side whole
inline uint32_t word(const Pick& main, const Pick& shadow, bool coarse) {
  if(!main.active() && !shadow.active())
    return 0;
  return Tag | (coarse ? Coarse : 0u) | share(main) | (share(shadow) << Bits);
  }

}
