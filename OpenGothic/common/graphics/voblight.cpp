#include "voblight.h"

namespace VobLayer {

float lightSourceCm(const asset::VirtualObject& vob) {
  if(const auto* lamp=dynamic_cast<const LampExtra*>(&vob)) return lamp->sourceCm;
  return DefaultLightSourceCm;
  }

float lightNear(const asset::VirtualObject& vob) {
  if(const auto* lamp=dynamic_cast<const LampExtra*>(&vob)) return lamp->nearLimit;
  return 0;
  }

std::array<float,3> lightOffset(const asset::VirtualObject& vob) {
  if(const auto* lamp=dynamic_cast<const LampExtra*>(&vob)) return lamp->offsetCm;
  return {};
  }

const PfxMotion::Flicker* flicker(const asset::VirtualObject& vob, float* jitterCm, float* swayHz) {
  if(swayHz!=nullptr) *swayHz=0;
  if(const auto* e=dynamic_cast<const Emitter*>(&vob); e!=nullptr && e->flicker.active()) {
    if(jitterCm!=nullptr) *jitterCm=0;
    return &e->flicker;
    }
  if(const auto* l=dynamic_cast<const LampExtra*>(&vob); l!=nullptr && l->flicker.active()) {
    if(jitterCm!=nullptr) *jitterCm=l->jitterCm;
    if(swayHz!=nullptr) *swayHz=l->swayHz;
    return &l->flicker;
    }
  return nullptr;
  }

} // namespace VobLayer
