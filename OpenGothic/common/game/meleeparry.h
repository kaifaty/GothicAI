#pragma once

#include <cstdint>

namespace MeleeParry {
  constexpr float ParryCost = 5.f;
  constexpr float WhiffCost = 10.f;
  constexpr uint64_t StunDuration = 1000;

  enum class Result : uint8_t { None, Parry, Guard };

  float  windowMs(int talent);
  Result classify(uint64_t paradeMs, int talent, uint64_t defenceEndMs);
  float  guardCost(float postureIncrease);
  bool   guardBroken(float stamina, float cost, bool staminaEnabled);
  }
