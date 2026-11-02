#pragma once

#include <cstdint>

// Pure rules for the hero's charged forward strike, [GAME] melee_charge.
namespace MeleeCharge {
  constexpr uint64_t HoldThresholdMs = 200;
  constexpr uint64_t WindupMs        = 400;
  constexpr uint64_t AutoReleaseMs   = 3000;

  struct Situation final {
    bool enabled          = true;
    bool player           = false;
    bool oneHand          = false;
    bool windupAnimation  = false;
    bool holdAnimation    = false;
    bool releaseAnimation = false;
    bool standing         = false;
    bool attacking        = false;
    bool parading         = false;
    bool stunned          = false;
    bool knockedDown      = false;
    bool inAir            = false;
    bool inWater          = false;
    };

  bool  available(const Situation& situation);
  bool  startsWindup(uint64_t heldMs);
  bool  autoRelease(uint64_t windupElapsedMs);
  bool  releaseReady(uint64_t windupElapsedMs, bool released);
  float level(uint64_t windupElapsedMs);
  float multiplier(uint64_t windupElapsedMs);
  float cost(float attackCost, bool staminaEnabled);
  // Like the two-handed swing, the release covers DEF_HIT_END itself.
  bool  superArmor(bool charging, bool releasing, uint64_t releaseElapsedMs, uint64_t hitEndMs);
  }
