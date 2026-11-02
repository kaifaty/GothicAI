#include "meleeriposte.h"

#include <algorithm>
#include <cmath>

bool MeleeRiposte::available(const Situation& s) {
  return s.enabled && s.player && s.standingOneHand && s.heroClip && s.victimHuman && s.victimAlive &&
         !s.victimDown && s.victimStunned && s.victimClip &&
         s.distanceCm>=80.f && s.distanceCm<=250.f && std::abs(s.heightCm)<=50.f &&
         std::abs(s.heroAngleDeg)<=45.f && std::abs(s.victimAngleDeg)<120.f;
  }

MeleeRiposte::Point MeleeRiposte::pairPosition(Point hero, float forwardX, float forwardZ) {
  return {hero.x+forwardX*PairDistanceCm,hero.y,hero.z+forwardZ*PairDistanceCm};
  }

MeleeRiposte::Point MeleeRiposte::alignment(Point from, Point pair, uint64_t elapsedMs) {
  const float t = float(std::min(elapsedMs,AlignmentMs))/float(AlignmentMs);
  const float amount = t*t*(3.f-2.f*t);
  return {from.x+(pair.x-from.x)*amount,from.y+(pair.y-from.y)*amount,from.z+(pair.z-from.z)*amount};
  }

bool MeleeRiposte::bladeReady(uint64_t elapsedMs, uint64_t optFrameMs) {
  return elapsedMs>=optFrameMs;
  }

bool MeleeRiposte::handoverReady(uint64_t elapsedMs) {
  return elapsedMs>=HandoverMs;
  }

float MeleeRiposte::multiplier() {
  return 3.f;
  }

float MeleeRiposte::cost(float attackCost, bool staminaEnabled) {
  return staminaEnabled ? attackCost : 0.f;
  }

bool MeleeRiposte::superArmor(bool active, uint64_t elapsedMs, uint64_t hitEndMs) {
  return active && elapsedMs<=hitEndMs;
  }
