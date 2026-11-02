#include "meleefighter.h"

bool MeleeFighter::qualifies(bool player, bool meleeWeapon, bool attacking, bool hasNpcTarget, bool targetDown,
                           float horizontalDistanceSq) {
  return !player && meleeWeapon && attacking && hasNpcTarget && !targetDown &&
         horizontalDistanceSq<1500.f*1500.f;
  }

void MeleeFighter::refresh(State& attacker, State& target, uint64_t now) {
  attacker.until = now+3000;
  target.until = attacker.until;
  }

bool MeleeFighter::active(const State& state, uint64_t now, bool player) {
  return !player && state.until!=0 && now<state.until;
  }

bool MeleeFighter::keepPhysics(bool enabled, bool eligible, bool fighter, bool wasInRange, float heroDistanceSq) {
  const bool inRange = wasInRange ? heroDistanceSq<=2200.f*2200.f : heroDistanceSq<2000.f*2000.f;
  return enabled && eligible && (fighter || inRange);
  }

bool MeleeFighter::physicsHits(bool enabled, bool player, bool characterVirtualAtStart) {
  return enabled && (player || characterVirtualAtStart);
  }
