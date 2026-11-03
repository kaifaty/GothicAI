#include "meleecharge.h"

#include <algorithm>

bool MeleeCharge::available(const Situation& s) {
  return s.enabled && (s.player || s.aiEnabled) && s.oneHand && s.windupAnimation && s.holdAnimation && s.releaseAnimation &&
         s.standing && !s.attacking && !s.parading && !s.stunned && !s.knockedDown && !s.inAir && !s.inWater;
  }

bool MeleeCharge::startsWindup(uint64_t heldMs) {
  return heldMs>=HoldThresholdMs;
  }

bool MeleeCharge::autoRelease(uint64_t windupElapsedMs) {
  return windupElapsedMs>=AutoReleaseMs;
  }

bool MeleeCharge::releaseReady(uint64_t windupElapsedMs, bool released) {
  return windupElapsedMs>=WindupMs && (released || autoRelease(windupElapsedMs));
  }

float MeleeCharge::level(uint64_t windupElapsedMs) {
  return float(std::min(windupElapsedMs,uint64_t(1000)))/1000.f;
  }

float MeleeCharge::multiplier(uint64_t windupElapsedMs) {
  return 1.f+level(windupElapsedMs);
  }

float MeleeCharge::cost(float attackCost, bool staminaEnabled) {
  return staminaEnabled ? 1.5f*attackCost : 0.f;
  }

bool MeleeCharge::superArmor(bool charging, bool releasing, uint64_t releaseElapsedMs, uint64_t hitEndMs) {
  return charging || (releasing && releaseElapsedMs<=hitEndMs);
  }
