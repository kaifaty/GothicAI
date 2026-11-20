#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "graphics/meshlevels.h"
#include "graphics/treeboughasset.h"

// The level switches of a tree with a bough hierarchy (OPENGOTHIC_TREE_BOUGH_LOD; reports/engine/2026-11-16-modern-trees/
// tree-bough-lod.md, «Плавная смена древесины и тени»). A threshold of its own GLBs (the main view's `levels` px) and
// of the shadow scale (MeshLevels::pickShadow: 800 and 400 px at 1080p) is a band in the proportions of the impostor
// joint: from px*56/49 down to px*46/49 the coarser GLB takes the share TreeBough::impostorJointWeight(px, height) of
// the samples and the finer one the rest. The thresholds lie an octave apart, so at most one band of a scale is active.
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

// The shadow scale of MeshLevels::pickShadow with bands at 800 and 400 px: the visual GLB above, then the third and
// the fourth level (as many as there are)
inline Pick shadow(size_t count, float heightAt1080, uint8_t visualGlb) {
  constexpr float px[] = {800.f, 400.f};
  const uint8_t levels[] = {visualGlb, uint8_t(std::min<size_t>(3, count)), uint8_t(std::min<size_t>(4, count))};
  return pick(px, levels, 2, heightAt1080);
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
