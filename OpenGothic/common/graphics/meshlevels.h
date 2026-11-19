#pragma once

#include <Tempest/Log>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <string_view>

#include "settings.h"

// Levels of detail of an own static mesh (reports/engine/2026-11-16-modern-trees/tree-lod.md). Its record of
// meshes/meshes.json names up to MaxLevels coarser GLBs with the materials of the mesh, each with the height of the
// object on the main view's screen under which it is drawn:
//   "levels": [{"glb": "<path>", "px": 400}, {"glb": "<path>", "px": 200},
//              {"glb": "<path>", "px": 100}, {"glb": "<path>", "px": 50}]
// px falling. The levels are packed after the mesh into its own buffers (PackedMesh::addLevel): a level is a slice of
// meshlets of each submesh. The main view and occluders use the cluster's visual range. Trees can choose a separate
// shadow range. The levels are picked on the processor once a frame by
// the main view (VisualObjects::preFrameUpdateLevels), with no blend: the switch is hard. An optional impostor
// follows the last GLB. Only the main view draws its card. The ray scene takes the coarsest GLB at any distance;
// collision, hit zones and fracture take the mesh itself.
// [INTERNAL] meshLevels=0|1; OPENGOTHIC_MESH_LEVELS=0|1 wins. Off: the levels are not loaded, the mesh is drawn and
// traced as it was without them. Read once
namespace MeshLevels {

constexpr size_t MaxLevels  = 4;
constexpr uint32_t ImpostorAtlasSize = 12*128;
constexpr uint32_t ImpostorMipCount  = 5;
// A level gives way to the finer one only at this many times its pixels: a height on a threshold does not switch the
// level back and forth
constexpr float  Hysteresis = 1.1f;
constexpr float  TreeL2Px   = 200.f;

struct Selection {
  uint8_t stage = 0;
  uint8_t glb   = 0;

  bool impostor() const { return stage!=glb; }
  bool operator==(const Selection&) const = default;
  };

inline bool parse(std::string_view env, bool ini) {
  return env.empty() ? ini : env!="0";
  }

inline bool enabled() {
  static const bool on = [] {
    const char* env = std::getenv("OPENGOTHIC_MESH_LEVELS");
    const bool  set = env!=nullptr && *env!='\0';
    // without the game's settings (a tool that loads meshes) the default of [INTERNAL] meshLevels
    const bool  ini = !::Settings::exists() || ::Settings::getI("INTERNAL","meshLevels")!=0;
    const bool  v   = parse(set ? std::string_view(env) : std::string_view(), ini);
    Tempest::Log::i("render: mesh levels of detail = ", v ? "1" : "0",
                    set ? " by OPENGOTHIC_MESH_LEVELS" : " by [INTERNAL] meshLevels");
    return v;
    }();
  return on;
  }

inline bool impostorsEnabled() {
  static const bool on = [] {
    const char* env = std::getenv("OPENGOTHIC_IMPOSTORS");
    const bool  set = env!=nullptr && *env!='\0';
    const bool  ini = !::Settings::exists() || ::Settings::getI("INTERNAL","impostors")!=0;
    const bool  v   = parse(set ? std::string_view(env) : std::string_view(), ini);
    Tempest::Log::i("render: impostors = ", v ? "1" : "0",
                    set ? " by OPENGOTHIC_IMPOSTORS" : " by [INTERNAL] impostors");
    return v;
    }();
  return on;
  }

inline bool shadowLevelsEnabled() {
  static const bool on = [] {
    const char* env = std::getenv("OPENGOTHIC_SHADOW_LEVELS");
    const bool  set = env!=nullptr && *env!='\0';
    const bool  ini = !::Settings::exists() || ::Settings::getI("INTERNAL","shadowLevels")!=0;
    const bool  v   = parse(set ? std::string_view(env) : std::string_view(), ini);
    Tempest::Log::i("render: tree shadow levels = ", v ? "1" : "0",
                    set ? " by OPENGOTHIC_SHADOW_LEVELS" : " by [INTERNAL] shadowLevels");
    return v;
    }();
  return on;
  }

inline bool smoothTreesEnabled() {
  if(!enabled())
    return false;
  static const bool on = [] {
    const char* env = std::getenv("OPENGOTHIC_TREE_SMOOTH_LOD");
    const bool set = env!=nullptr && *env!='\0';
    const bool ini = !::Settings::exists() || ::Settings::getI("INTERNAL","treeSmoothLod")!=0;
    const bool v = parse(set ? std::string_view(env) : std::string_view(), ini);
    Tempest::Log::i("render: smooth tree levels of detail = ", v ? "1" : "0",
                    set ? " by OPENGOTHIC_TREE_SMOOTH_LOD" : " by [INTERNAL] treeSmoothLod");
    return v;
    }();
  return on;
  }

// The pixels of the main view an object `height` high is `distance` from the camera, in the same units; projY: the
// projection's [1][1], screenH: the view's height in pixels. As the size of a motion in the fade of a tree's wind
// (FoliageTree::fadeScale)
inline float screenHeight(float height, float distance, float projY, int screenH) {
  return height*0.5f*float(std::max(screenH,0))*std::abs(projY)/std::max(distance,1.f);
  }

// The level to draw at `height` pixels: 0 the mesh itself, k the k-th of the `count` thresholds `px` (falling). Level
// k comes in under px[k-1] and goes back to k-1 at Hysteresis*px[k-1] and over; `current` is the level drawn now
inline uint8_t pick(const float* px, size_t count, uint8_t current, float height) {
  size_t level = std::min<size_t>(current, count);
  while(level<count && height<px[level])
    ++level;
  while(level>0 && height>=px[level-1]*Hysteresis)
    --level;
  return uint8_t(level);
  }

// The smooth prefix replaces the full GLB to L1 switch. Below 200 px only the usual L2 and farther ranges apply.
inline uint8_t pickVisual(const float* px, size_t count, uint8_t current, float height, bool smoothTree) {
  if(!smoothTree || count<2 || px[1]!=TreeL2Px)
    return pick(px, count, current, height);
  if(height>=TreeL2Px)
    return 0;
  const auto fromL2 = uint8_t(std::max(2, int(current))-2);
  return uint8_t(2 + pick(px+2, count-2, fromL2, height));
  }

inline float treeCardFraction(float height) {
  if(height<=400.f)
    return 0.8f;
  if(height>=560.f)
    return 1.f;
  return std::lerp(0.8f, 1.f, std::log2(height/400.f)/std::log2(560.f/400.f));
  }

// Same band and rank-centre convention as the raster shader. K==N has no fading band.
inline float treeCardBand(float keep, float total) {
  return std::min(std::clamp(0.04f*keep, 8.f, 256.f), 2.f*(total-keep));
  }

inline size_t treeMeshletPrefix(std::span<const int32_t> firstRanks, float keep, float total) {
  const float end = keep + 0.5f*treeCardBand(keep, total);
  const auto stop = std::lower_bound(firstRanks.begin(), firstRanks.end(), end,
                                    [](int32_t rank, float limit) { return float(rank)<limit; });
  return size_t(stop-firstRanks.begin());
  }

inline float treeCardCoverage(int32_t rank, float keep, float total) {
  const float width = treeCardBand(keep, total);
  if(width==0.f)
    return 1.f;
  const float t = std::clamp((float(rank)+0.5f - (keep-0.5f*width))/width, 0.f, 1.f);
  return 1.f - t*t*(3.f-2.f*t);
  }

// Leave the visual GLB below 800 pixels at 1080p and return to it at 880. Farther out, sun and lamps share
// the coarser scale based on the main view's field of view. Neither shadow path takes the impostor card.
inline uint8_t pickShadow(size_t count, uint8_t current, bool wasSeparate, float heightAt1080, uint8_t visualGlb, bool enabled) {
  constexpr float closeUpPx = 800.f;
  constexpr float px[MaxLevels] = {2400.f, 1400.f, closeUpPx, 400.f};
  const float visualThreshold = wasSeparate ? closeUpPx*Hysteresis : closeUpPx;
  if(!enabled || heightAt1080>=visualThreshold)
    return visualGlb;
  return pick(px, std::min(count, MaxLevels), current, heightAt1080);
  }

}
