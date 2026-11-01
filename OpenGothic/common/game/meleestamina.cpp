#include "meleestamina.h"

#include <algorithm>

float MeleeStamina::maximum(int strength) {
  return 80.f+.2f*float(strength);
  }

float MeleeStamina::attackCost(int strengthRequirement, int talent, bool fists) {
  if(fists)
    return 8.f;
  return (8.f+float(strengthRequirement)/5.f)*(1.3f-.006f*float(std::clamp(talent,0,100)));
  }

void MeleeStamina::reset(State& state, float maximum, uint64_t now) {
  state = State{maximum,now,now,false,true};
  }

void MeleeStamina::recover(State& state, float maximum, uint64_t now, bool parade) {
  if(!state.initialized) {
    reset(state,maximum,now);
    return;
    }
  const uint64_t begin = std::max(state.lastUpdate,state.lastSpend+800);
  state.lastUpdate = now;
  state.current = std::min(state.current,maximum);
  if(!parade && now>begin)
    state.current = std::min(maximum,state.current+float(now-begin)*.035f);
  if(state.current>=.25f*maximum)
    state.exhausted = false;
  }

bool MeleeStamina::canAct(const State& state, bool enabled) {
  return !enabled || (!state.exhausted && state.current>0.f);
  }

bool MeleeStamina::canAfford(const State& state, float cost, bool enabled) {
  return !enabled || (canAct(state,true) && state.current>=cost);
  }

void MeleeStamina::drain(State& state, float cost, uint64_t now) {
  state.current = std::max(0.f,state.current-cost);
  state.lastSpend = now;
  state.lastUpdate = now;
  if(state.current==0.f)
    state.exhausted = true;
  }

bool MeleeStamina::spend(State& state, float cost, uint64_t now, bool enabled) {
  if(!canAct(state,enabled))
    return false;
  if(!enabled)
    return true;
  drain(state,cost,now);
  return true;
  }
