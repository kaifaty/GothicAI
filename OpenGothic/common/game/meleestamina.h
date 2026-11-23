#pragma once

#include <cstdint>

namespace MeleeStamina {
  constexpr float JumpBackCost = 20.f;

  struct State final {
    float    current = 0;
    uint64_t lastSpend = 0;
    uint64_t lastUpdate = 0;
    bool     exhausted = false;
    bool     initialized = false;
    };

  float maximum(int strength);
  float attackCost(int strengthRequirement, int talent, bool fists = false);
  void  reset(State& state, float maximum, uint64_t now);
  void  recover(State& state, float maximum, uint64_t now, bool parade);
  bool  canAct(const State& state, bool enabled);
  bool  canAfford(const State& state, float cost, bool enabled);
  void  drain(State& state, float cost, uint64_t now);
  bool  spend(State& state, float cost, uint64_t now, bool enabled);
  }
