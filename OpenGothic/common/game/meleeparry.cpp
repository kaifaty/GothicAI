#include "meleeparry.h"

#include <algorithm>

float MeleeParry::windowMs(int talent) {
  return 120.f+1.3f*float(std::clamp(talent,0,100));
  }

MeleeParry::Result MeleeParry::classify(uint64_t paradeMs, int talent, uint64_t defenceEndMs) {
  if(paradeMs>=defenceEndMs)
    return Result::None;
  return float(paradeMs)<windowMs(talent) ? Result::Parry : Result::Guard;
  }

float MeleeParry::guardCost(float postureIncrease) {
  return std::clamp(postureIncrease,5.f,50.f);
  }

bool MeleeParry::guardBroken(float stamina, float cost, bool staminaEnabled) {
  return staminaEnabled && stamina<cost;
  }
