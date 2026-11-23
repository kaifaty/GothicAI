#pragma once

#include <string_view>

class AnimationSound {
  public:
    virtual ~AnimationSound() = default;
    virtual void emitSoundEffect(std::string_view sound, float range, bool freeSlot) = 0;
  };

class AnimationGroundSound : public AnimationSound {
  public:
    virtual bool isInAir() const = 0;
    virtual void emitSoundGround(std::string_view sound, float range, bool freeSlot) = 0;
  };
