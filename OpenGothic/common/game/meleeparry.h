#pragma once

#include <cstdint>

namespace MeleeParry {
  constexpr float ParryCost = 5.f;
  constexpr float WhiffCost = 10.f;
  constexpr uint64_t StunDuration = 1000;

  enum class Result : uint8_t { None, Parry, Guard };
  enum class StunnedAction : uint8_t { None, Move, Turn, Attack };
  enum class SwingAction : uint8_t { Wait, TimeParade, GiveUp };

  float  windowMs(int talent);
  float  timingSigmaMs(int talent);
  SwingAction awaitSwing(uint64_t heldMs, bool hasSwing);
  uint64_t paradeStartMs(uint64_t now, uint64_t contact, int talent, float errorMs, float leadMs);
  StunnedAction resolveStunnedTactics(bool enabled, bool stunned, bool inRange, bool focus);
  Result classify(uint64_t paradeMs, int talent, uint64_t defenceEndMs);
  float  guardCost(float postureIncrease);
  bool   guardBroken(float stamina, float cost, bool staminaEnabled);
  }
