#pragma once

#include <Tempest/Log>

#include <cstdlib>
#include <string_view>

#include "settings.h"

// The short occlusion over the light of the GI probes (reports/render-features/2026-11-16-contact-ambient): the probes
// stand 25 cm apart and more and see the world's mesh alone, so nothing darkens the ground at a foot, a table under a
// cup, a neck under a chin where no direct light comes. With the probes ssao.comp traces a sphere of a quarter of a
// metre instead of a metre (its push flag), probe_ambient.frag takes it (VF_CONTACT_AO of scene.glsl).
// [INTERNAL] giContactAo=0|1; OPENGOTHIC_GI_CONTACT_AO=0|1 wins. Read once
namespace ContactAo {

inline bool enabled() {
  static const bool on = [] {
    const char* env = std::getenv("OPENGOTHIC_GI_CONTACT_AO");
    const bool  set = env!=nullptr && *env!='\0';
    const bool  v   = set ? std::string_view(env)!="0" : ::Settings::getI("INTERNAL","giContactAo")!=0;
    Tempest::Log::i("render: gi contact ao = ", v ? 1 : 0, set ? " by OPENGOTHIC_GI_CONTACT_AO" : " by [INTERNAL] giContactAo");
    return v;
    }();
  return on;
  }

}
