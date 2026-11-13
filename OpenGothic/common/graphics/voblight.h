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
// What the layer knows of a lamp beyond the base's fields: of a lamp it adds and of a base lamp it edits in place
struct LampExtra {
  virtual ~LampExtra() = default;
  float sourceCm = VobLayer::DefaultLightSourceCm; // the flame's size ("source_cm")
  std::string        flickerFrom; // the emitter's key whose signal drives the light ("flicker_from")
  PfxMotion::Flicker flicker;
  float              jitterCm  = 0; // the light's trembling ("jitter_cm")
  float              nearLimit = 0; // the lamp's own limit of the light near it ("near"); 0: the common one
  };
struct Light final : asset::VLight, Added, LampExtra {};
// A base lamp edited in place ("light"): the same id, place and tree, so the saved state of the base still fits it
struct BaseLight final : asset::VLight, LampExtra {
  explicit BaseLight(const asset::VLight& base) : asset::VLight(base) {}
  };

float lightSourceCm(const asset::VirtualObject& vob);
// A lamp's own limit of the light near it ("near"), in units of its light at the range (light.frag); 0: the
// common limit (Renderer lampNearKnee). Of a lamp the layer adds or of a base lamp it edits in place ("light")
float lightNear(const asset::VirtualObject& vob);
// The flame's signal of a layer emitter ("flicker") or of the emitter a lamp names ("flicker_from": a layer lamp
// or a base lamp edited in place), with the light's trembling in cm ("jitter_cm"); nullptr for other vobs (pfxmotion.h)
const PfxMotion::Flicker* flicker(const asset::VirtualObject& vob, float* jitterCm = nullptr);

} // namespace VobLayer
