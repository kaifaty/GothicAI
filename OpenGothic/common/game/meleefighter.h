#pragma once

#include <cstdint>

namespace MeleeFighter {
  struct State final {
    uint64_t until = 0;
    };

  bool qualifies(bool player, bool meleeWeapon, bool attacking, bool hasNpcTarget, bool targetDown,
                 float horizontalDistanceSq);
  void refresh(State& attacker, State& target, uint64_t now);
  bool active(const State& state, uint64_t now, bool player);
  bool keepPhysics(bool enabled, bool eligible, bool fighter, bool wasInRange, float heroDistanceSq);
  bool physicsHits(bool enabled, bool player, bool characterVirtualAtStart);
  }
