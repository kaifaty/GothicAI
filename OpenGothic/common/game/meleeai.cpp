#include "meleeai.h"

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

float MeleeAi::riposteChance(int talent) {
  return float(std::clamp(talent,0,100))/100.f;
  }

bool MeleeAi::riposteRoll(int talent, float uniform) {
  return uniform<riposteChance(talent);
  }

MeleeAi::BodyReaction MeleeAi::bodyReaction(Action action, bool eligibleOneHand, bool dodgeEnabled,
                                         bool riposteEnabled, bool targetStunned) {
  if(!eligibleOneHand)
    return BodyReaction::Original;
  if(action==Action::JumpBack && dodgeEnabled)
    return BodyReaction::DodgeBack;
  if(action==Action::Attack && riposteEnabled && targetStunned)
    return BodyReaction::RiposteRoll;
  return BodyReaction::Original;
  }
