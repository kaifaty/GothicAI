#pragma once

#include "game/meleedodge.h"

#include <cstdint>
#include <optional>

// Decisions layered over native FAI moves and the two equivalent P30 actions.
// Execution uses Npc's shared dodge and charge rules.
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

  float chargeChance(int talent);
  bool chargeRoll(int talent, float uniform);
  uint64_t chargeHoldMs(float uniform);
  bool chargeRelease(uint64_t elapsedMs, uint64_t holdMs, bool inRange);

  bool specialCooldownReady(uint64_t now, std::optional<uint64_t> endedMs);

  enum class Special : uint8_t { None, Charge };
  // One slot belongs to each target. Only its owner may return it.
  struct SpecialSlot final {
    const void* owner = nullptr;
    Special kind = Special::None;

    bool busy() const;
    bool acquire(const void* actor, Special move);
    void release(const void* actor);
    };

  }
