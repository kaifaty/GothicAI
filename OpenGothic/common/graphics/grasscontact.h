#pragma once

#include <Tempest/Log>

#include <cstdlib>
#include <string_view>

#include "settings.h"

// The sun's screen ray over the grass (shader/grass/grass_contact.glsl, VF_GRASS_CONTACT of scene.glsl): a blade is in
// no shadow map, a short ray through the depth buffer lets it shade the ground at its root and the blade behind it.
// [INTERNAL] grassContact=0|1 (0 on Metal: the Mac's frame has no room for it unmeasured);
// OPENGOTHIC_GRASS_CONTACT=0|1 wins. Read once
namespace GrassContact {

inline bool enabled() {
  static const bool on = [] {
    const char* env = std::getenv("OPENGOTHIC_GRASS_CONTACT");
    const bool  set = env!=nullptr && *env!='\0';
    const bool  v   = set ? std::string_view(env)!="0" : ::Settings::getI("INTERNAL","grassContact")!=0;
    Tempest::Log::i("render: grass contact ray = ", v ? 1 : 0, set ? " by OPENGOTHIC_GRASS_CONTACT" : " by [INTERNAL] grassContact");
    return v;
    }();
  return on;
  }

}
