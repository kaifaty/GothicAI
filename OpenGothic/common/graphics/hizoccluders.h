#pragma once

#include <Tempest/Log>

#include <cstdlib>
#include <string_view>

#include "graphics/foliagewind.h"
#include "settings.h"

// The occluder pass without the crowns of trees (reports/engine/2026-11-16-modern-trees/hiz-occluders.md): the
// leaves of a material with `"foliage": {"anchor": "tree"}` and the alpha test are drawn neither into the main view's
// depth ahead of the G-buffer (Renderer::drawHiZ) nor, through it, into the HiZ pyramid; the bark of the same tree,
// solid, stays an occluder. A crown of cards is a poor occluder - full of holes, the depth behind them stays far - and
// a dear one: its depth pass with the alpha test cost 1-4.4 ms in a forest of 1 024 spruces. The G-buffer draws it
// as before. BK_CANOPY of scene.glsl marks such a bucket whatever this says (the counters of the main view read it);
// VF_HIZ_NO_CANOPY of the scene's flags leaves it out of the occluder view (visibility_pass.comp).
// [INTERNAL] hizSkipCrowns=0|1; OPENGOTHIC_HIZ_SKIP_CROWNS=0|1 wins. Read once
namespace HiZOccluders {

inline bool parse(std::string_view env, bool ini) {
  if(env.empty())
    return ini;
  return env!="0";
  }

inline bool skipCrowns() {
  static const bool v = [] {
    const char* env = std::getenv("OPENGOTHIC_HIZ_SKIP_CROWNS");
    const bool  set = env!=nullptr && *env!='\0';
    const bool  on  = parse(set ? std::string_view(env) : std::string_view(), ::Settings::getI("INTERNAL","hizSkipCrowns")!=0);
    Tempest::Log::i("render: occluders without the crowns of trees = ", on ? "1" : "0",
                    set ? " by OPENGOTHIC_HIZ_SKIP_CROWNS" : " by [INTERNAL] hizSkipCrowns");
    return on;
    }();
  return v;
  }

// BK_CANOPY of a material: the alpha-tested leaves of a tree. Its bark (solid), a tuft (the anchor of a line) and the
// original's alpha-tested leaves (no `foliage`) are none
inline bool canopy(const FoliageWind::Params& p, bool alphaTest) {
  return alphaTest && p.on && p.anchor==FoliageWind::AnchorTree;
  }

}
