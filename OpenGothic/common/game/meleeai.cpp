#include "meleeai.h"
#include "meleecharge.h"

#include <algorithm>
#include <cmath>

float MeleeAi::reactionMs(int talent) {
  return 400.f-1.5f*float(std::clamp(talent,0,100));
  }

uint64_t MeleeAi::dodgeStartMs(uint64_t swingStart, uint64_t contact, float aimMs, int talent, float errorMs) {
  const double desired = double(contact)-double(aimMs)+double(errorMs);
  const double earliest = double(swingStart)+double(std::max(250.f,reactionMs(talent)));
  return uint64_t(std::ceil(std::max(desired,earliest)));
  }

MeleeDodge::Direction MeleeAi::dodgeDirection(float uniform) {
  if(uniform<.5f)
    return MeleeDodge::Direction::Back;
  if(uniform<.75f)
    return MeleeDodge::Direction::Left;
  return MeleeDodge::Direction::Right;
  }

bool MeleeAi::DodgeStreak::allows(uint64_t now) const {
  return count<2 || now-lastDodgeMs>=3000;
  }

void MeleeAi::DodgeStreak::dodged(uint64_t now) {
  if(now-lastDodgeMs>=3000)
    count = 0;
  ++count;
  lastDodgeMs = now;
  }

void MeleeAi::DodgeStreak::reset() {
  count = 0;
  }

float MeleeAi::chargeChance(int talent) {
  return .3f*float(std::clamp(talent,0,100))/100.f;
  }

bool MeleeAi::chargeRoll(int talent, float uniform) {
  return uniform<chargeChance(talent);
  }

uint64_t MeleeAi::chargeHoldMs(float uniform) {
  return 400+uint64_t(800.f*uniform);
  }

bool MeleeAi::chargeRelease(uint64_t elapsedMs, uint64_t holdMs, bool inRange) {
  return elapsedMs>=MeleeCharge::AutoReleaseMs || (elapsedMs>=holdMs && inRange);
  }

bool MeleeAi::specialCooldownReady(uint64_t now, std::optional<uint64_t> endedMs) {
  return !endedMs || now-*endedMs>=6000;
  }

bool MeleeAi::SpecialSlot::busy() const {
  return owner!=nullptr;
  }

bool MeleeAi::SpecialSlot::acquire(const void* actor, Special move) {
  if(busy())
    return false;
  owner = actor;
  kind = move;
  return true;
  }

void MeleeAi::SpecialSlot::release(const void* actor) {
  if(owner!=actor)
    return;
  owner = nullptr;
  kind = Special::None;
  }
