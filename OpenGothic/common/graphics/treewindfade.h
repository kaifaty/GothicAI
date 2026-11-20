#pragma once

#include <Tempest/Log>

#include <cstdlib>
#include <string_view>

#include "settings.h"

// The switch of the fade of a tree's wind with the distance (reports/engine/2026-11-16-modern-trees/tree-wind.md,
// section 4; FoliageTree::fade of graphics/foliagetree.h, foliageTreeFade of shader/materials/foliage_tree.glsl):
// far off the needles stop first, then the branches, the trunk last, and past that a tree is drawn with no wind.
// [INTERNAL] treeWindFade=0|1; OPENGOTHIC_TREE_WIND_FADE=0|1 wins. Off: the wind at any distance. Read once
namespace TreeWindFade {

inline bool parse(std::string_view env, bool ini) {
  return env.empty() ? ini : env!="0";
  }

inline bool enabled() {
  static const bool on = [] {
    const char* env = std::getenv("OPENGOTHIC_TREE_WIND_FADE");
    const bool  set = env!=nullptr && *env!='\0';
    const bool  v   = parse(set ? std::string_view(env) : std::string_view(), ::Settings::getI("INTERNAL","treeWindFade")!=0);
    Tempest::Log::i("render: tree wind fade = ", v ? "1" : "0",
                    set ? " by OPENGOTHIC_TREE_WIND_FADE" : " by [INTERNAL] treeWindFade");
    return v;
    }();
  return on;
  }

}
