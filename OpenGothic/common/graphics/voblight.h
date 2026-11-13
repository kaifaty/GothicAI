#pragma once

#include "asset/worldtypes.h"
#include "graphics/pfx/pfxmotion.h"

namespace VobLayer {

// A layer light's flame size in cm ("source_cm"): the soft edge of its shadow (VSM, light.frag); 6, a torch, else
inline constexpr float DefaultLightSourceCm = 6.f;

struct Added {
  virtual ~Added() = default;
  std::string key;
  };
struct Emitter final : asset::VirtualObject, Added {
  PfxMotion::Flicker flicker; // the flame's signal ("flicker"), off by default
  };
struct Light final : asset::VLight, Added {
  float sourceCm = VobLayer::DefaultLightSourceCm; // the flame's size ("source_cm")
  std::string        flickerFrom; // the emitter's key whose signal drives the light ("flicker_from")
  PfxMotion::Flicker flicker;
  float              jitterCm = 0; // the light's trembling ("jitter_cm")
  };

float lightSourceCm(const asset::VirtualObject& vob);
// The flame's signal of a layer emitter ("flicker") or of the emitter a layer light names ("flicker_from"), with the
// light's trembling in cm ("jitter_cm"); nullptr for other vobs (pfxmotion.h)
const PfxMotion::Flicker* flicker(const asset::VirtualObject& vob, float* jitterCm = nullptr);

} // namespace VobLayer
