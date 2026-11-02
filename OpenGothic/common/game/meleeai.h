#pragma once

#include "game/meleedodge.h"

#include <cstdint>

// Decisions layered over native FAI moves and the two equivalent P30 actions.
// Execution still uses Npc's ordinary dodge and riposte rules.
namespace MeleeAi {
  float reactionMs(int talent);
  uint64_t dodgeStartMs(uint64_t swingStart, uint64_t contact, float aimMs, int talent, float errorMs);
  MeleeDodge::Direction dodgeDirection(float uniform);

  struct DodgeStreak final {
    unsigned count = 0;
    uint64_t lastDodgeMs = 0;

    bool allows(uint64_t now) const;
    void dodged(uint64_t now);
    void reset(); // own attack or parade
    };

  float riposteChance(int talent);
  bool riposteRoll(int talent, float uniform);

  enum class Action : uint8_t { Other, JumpBack, Attack };
  enum class BodyReaction : uint8_t { Original, DodgeBack, RiposteRoll };
  BodyReaction bodyReaction(Action action, bool eligibleOneHand, bool dodgeEnabled, bool riposteEnabled,
                            bool targetStunned);
  }
