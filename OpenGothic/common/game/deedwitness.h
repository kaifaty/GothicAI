#pragma once

#include "constants.h"

// A marked NPC deed must still have native simulation at its transfer/capture point.
// Shared with the queued ground pickup, which can run before the bridge's next tick.
struct NpcDeedSimulation final {
  static constexpr bool admitted(NpcProcessPolicy policy) {
    return policy==NpcProcessPolicy::AiNormal;
    }

  static constexpr const char* refusal(NpcProcessPolicy policy) {
    return admitted(policy) ? nullptr : "left_simulation";
    }

  static constexpr const char* refusal(NpcProcessPolicy policy, bool& lost) {
    lost = lost || !admitted(policy);
    return lost ? "left_simulation" : nullptr;
    }
  };

// Shared, side-effect-free admission for the bridge's witness snapshot. Native
// perception callbacks are never invoked to discover an NPC deed's witnesses.
struct DeedWitness final {
  static constexpr bool foreignCombat(bool fighting, bool targetIsCulprit) {
    return fighting && !targetIsCulprit;
    }

  bool culprit = false;
  bool victim = false;
  bool player = false;
  bool down = false;
  bool lying = false;
  bool sleeping = false;
  bool simulated = true;
  bool stateStarted = true;
  bool foreignFight = false;
  uint32_t guild = GIL_NONE;
  bool perceptionEnabled = true;
  float distanceSquared = 0;
  float range = 0;

  constexpr bool admitted() const {
    return !culprit && !victim && !player && !down && !lying && !sleeping &&
           simulated && stateStarted && !foreignFight && guild<GIL_SEPERATOR_HUM &&
           perceptionEnabled && distanceSquared<=range*range;
    }
  };
