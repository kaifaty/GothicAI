#include "reachsignal.h"

#include <algorithm>

bool ReachSignal::wantsProbe(const Input& input) {
  return input.enabled && input.eligible && input.moving && !input.contact && input.avgSpeed>=ProbeSpeed;
  }

float ReachSignal::probeLength(float avgSpeed, float radius) {
  return radius + avgSpeed*KeepTime;
  }

ReachSignal::Result ReachSignal::update(State& state, const Input& input) {
  Result result;
  if(!input.enabled) {
    state = {};
    return result;
    }

  // pressed: the move asked is not gone, forward or along the obstacle. A threshold with room and an eased weight:
  // a body held by something that gives way loses a little, then all, then a little again
  float goal = 0;
  if(input.eligible && input.moving && input.obstacle) {
    const float k = std::clamp((1.f-input.moved-PressFrom)/(PressFull-PressFrom),0.f,1.f);
    goal = k*k*(3.f-2.f*k);
    }
  const float step = float(input.dtMs)/PressTime;
  state.press += std::clamp(goal-state.press,-step,step);
  state.press  = std::clamp(state.press,0.f,1.f);
  result.press = state.press;

  // about to hit: only before the contact - a body that slides on along the wall does not trail its hands on it
  bool warn = false;
  if(wantsProbe(input) && input.ahead && input.cosine>0) {
    result.closing   = input.avgSpeed*input.cosine;
    // the capsule meets a slanted wall before its axis' ray does
    result.timeToHit = std::max(0.f,(input.distance*input.cosine-input.radius)/std::max(result.closing,1.f));
    warn = state.warn ? (result.closing>=KeepSpeed && result.timeToHit<=KeepTime)
                      : (result.closing>=WarnSpeed && result.timeToHit<=WarnTime);
    }
  state.warn   = warn;
  result.warn  = warn;
  if(warn)
    state.grace = GraceTime;
  else
    state.grace = state.grace>input.dtMs ? state.grace-input.dtMs : 0;
  const bool held = state.grace>0 && input.eligible && input.moving;
  result.hands = warn || held || goal>=HandsFrom;
  result.plant = warn || held || goal>=0.9f;
  return result;
  }
