#pragma once

#include <Tempest/Log>

#include <cstdlib>
#include <string_view>

#include "settings.h"

// The screen ray of the lamps (shader/lighting/lamp_contact.glsl, VF_LAMP_CONTACT of scene.glsl): a lamp's shadow map
// is too coarse for what is a few centimetres deep, a short ray through the depth buffer darkens it.
// [INTERNAL] lampContact=0|1 (1 where the lamps' shadows are soft, 0 on Metal: the Mac's frame has no room for it
// unmeasured); OPENGOTHIC_LAMP_CONTACT=0|1 wins. Read once
namespace LampContact {

inline bool enabled() {
  static const bool on = [] {
    const char* env = std::getenv("OPENGOTHIC_LAMP_CONTACT");
    const bool  set = env!=nullptr && *env!='\0';
    const bool  v   = set ? std::string_view(env)!="0" : ::Settings::getI("INTERNAL","lampContact")!=0;
    Tempest::Log::i("render: lamp contact ray = ", v ? 1 : 0, set ? " by OPENGOTHIC_LAMP_CONTACT" : " by [INTERNAL] lampContact");
    return v;
    }();
  return on;
  }

}
