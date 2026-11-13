#pragma once

#include "animation.h"

class AnimationSolver;

class AnimationInteractive {
  public:
    enum Anim : int8_t {
      In        =  1,
      Active    =  0,
      Out       = -1,

      ToStand   = 10,
      FromStand = 11,
      };

    virtual ~AnimationInteractive() = default;
    virtual const Animation::Sequence* animNpc(const AnimationSolver& solver, Anim t) const = 0;
  };
