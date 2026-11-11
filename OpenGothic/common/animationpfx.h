#pragma once

#include "graphics/mesh/animation.h"

class AnimationPfx {
  public:
    virtual ~AnimationPfx() = default;
    virtual void animationPfx(const Animation::MdsParticleEffect& event, MdlVisual& visual) = 0;
  };
