#include "meleeparry.h"

#include <algorithm>
#include <cmath>

float MeleeParry::windowMs(int talent) {
  return 120.f+1.3f*float(std::clamp(talent,0,100));
  }

float MeleeParry::timingSigmaMs(int talent) {
  return 150.f-1.1f*float(std::clamp(talent,0,100));
  }

uint64_t MeleeParry::paradeStartMs(uint64_t now, uint64_t contact, int talent, float errorMs, float leadMs) {
  const double start = double(contact)-double(windowMs(talent))*.5-double(leadMs)+double(errorMs);
  return uint64_t(std::max(double(now),std::round(start)));
  }

MeleeParry::StunnedAction MeleeParry::resolveStunnedTactics(bool enabled, bool stunned, bool inRange, bool focus) {
  if(!enabled || !stunned)
    return StunnedAction::None;
  if(!inRange)
    return StunnedAction::Move;
  if(!focus)
    return StunnedAction::Turn;
  return StunnedAction::Attack;
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
