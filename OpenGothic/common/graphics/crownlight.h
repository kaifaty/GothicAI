#pragma once

#include <Tempest/Log>

#include <cstdint>
#include <cstdlib>
#include <string_view>

#include "graphics/foliagewind.h"
#include "settings.h"

// The light of a tree's crown (reports/engine/2026-11-16-modern-trees/crown-light.md; BK_CROWN of scene.glsl,
// shader/lighting/crown_light.glsl): a material with `"foliage": {"anchor": "tree"}` darkens into the crown by the
// openness its vertices carry (COLOR_0 A of the own GLB), and the sky and the lamps shine through its leaves as the
// sun does. [INTERNAL] crownLight=0|1; OPENGOTHIC_CROWN_LIGHT=0|1 wins, `mask` paints the crown by its openness in
// place of the light (the measure of the note). Read once
namespace CrownLight {

enum Mode : uint8_t {
  Off  = 0,
  On   = 1,
  Mask = 2,
  };

inline Mode parse(std::string_view env, bool ini) {
  if(env.empty())
    return ini ? On : Off;
  if(env=="mask")
    return Mask;
  return env=="0" ? Off : On;
  }

inline Mode mode() {
  static const Mode m = [] {
    const char* env = std::getenv("OPENGOTHIC_CROWN_LIGHT");
    const bool  set = env!=nullptr && *env!='\0';
    const Mode  v   = parse(set ? std::string_view(env) : std::string_view(), ::Settings::getI("INTERNAL","crownLight")!=0);
    Tempest::Log::i("render: crown light = ", v==Mask ? "mask" : (v==On ? "1" : "0"),
                    set ? " by OPENGOTHIC_CROWN_LIGHT" : " by [INTERNAL] crownLight");
    return v;
    }();
  return m;
  }

inline bool enabled() {
  return mode()!=Off;
  }

// BK_CROWN of a material: a tree's, while the light of the foliage is on (FoliageWind::enabled)
inline bool lit(const FoliageWind::Params& p, uint8_t halves, bool on) {
  return on && p.on && p.anchor==FoliageWind::AnchorTree && (halves & FoliageWind::Light)!=0;
  }

}
