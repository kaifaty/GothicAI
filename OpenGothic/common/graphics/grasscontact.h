#pragma once

#include <Tempest/Log>

#include <cstdint>
#include <cstdlib>
#include <string_view>

#include "settings.h"

// The sun's screen ray over the grass (shader/grass/grass_contact.glsl, VF_GRASS_CONTACT of scene.glsl): a blade is in
// no shadow map, a short ray through the depth buffer lets it shade the ground at its root and the blade behind it.
// [INTERNAL] grassContact=0|1 (0 on Metal: the Mac's frame has no room for it unmeasured);
// OPENGOTHIC_GRASS_CONTACT=0|1 wins. Read once
namespace GrassContact {

enum Study : uint32_t {
  None,
  Count,
  Compare,
  };

// One mask word covers a workgroup's square in grass/grass_contact_mask.comp.
inline constexpr uint32_t MaskTilePixels = 64;

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

inline bool maskEnabled() {
  static const bool on = [] {
    const char* env = std::getenv("OPENGOTHIC_GRASS_CONTACT_MASK");
    const bool enabled = env==nullptr || std::string_view(env)!="0";
    Tempest::Log::i("render: grass contact mask = ", enabled ? 1 : 0);
    return enabled;
    }();
  return on;
  }

inline Study study() {
  static const Study mode = [] {
    const char* env = std::getenv("OPENGOTHIC_GRASS_CONTACT_STUDY");
    const std::string_view value = env!=nullptr ? env : "";
    if(value=="count")
      return Count;
    if(value=="compare")
      return Compare;
    return None;
    }();
  return mode;
  }

}
