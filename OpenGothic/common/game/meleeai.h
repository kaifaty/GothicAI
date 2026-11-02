#pragma once

#include "game/meleedodge.h"

#include <cstdint>
#include <optional>
#include <string_view>

// Decisions layered over native FAI moves and the two equivalent P30 actions.
// Execution uses Npc's shared dodge, charge and riposte rules.
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

  float chargeChance(int talent);
  bool chargeRoll(int talent, float uniform);
  uint64_t chargeHoldMs(float uniform);
  bool chargeRelease(uint64_t elapsedMs, uint64_t holdMs, bool inRange);

  enum class BargeTrigger : uint8_t { None, Parade, Guarded };
  float bargeChance(int talent);
  bool bargeRoll(int talent, float uniform);
  BargeTrigger bargeTrigger(bool parade, uint64_t now, std::optional<uint64_t> lastGuardedMs);
  bool bargeGeometry(float distanceCm, float heightCm, float angleDeg);
  bool bargeArmor(std::optional<MeleeImpact::ArmorClass> armor);
  std::string_view name(BargeTrigger trigger);

  bool specialCooldownReady(uint64_t now, std::optional<uint64_t> endedMs);

  enum class Special : uint8_t { None, Charge, Barge, Riposte };
  // One slot belongs to each target. Only its owner may return it.
  struct SpecialSlot final {
    const void* owner = nullptr;
    Special kind = Special::None;

    bool busy() const;
    bool acquire(const void* actor, Special move);
    void release(const void* actor);
    };

  }
