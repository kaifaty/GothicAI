#include "bumpsignal.h"

#include <algorithm>
#include <cmath>

BumpSignal::Result BumpSignal::update(State& state, const Input& input) {
  Result result;
  if(!input.enabled) {
    state = {};
    return result;
    }
  const uint64_t moveStart = input.nowMs>=input.dtMs ? input.nowMs-input.dtMs : 0;
  const uint64_t windowStart = moveStart>=300 ? moveStart-300 : 0;
  while(state.count>0 && state.speeds[state.first].endMs<=windowStart) {
    state.first = (state.first+1)%state.speeds.size();
    --state.count;
    }
  double distance = 0;
  uint64_t duration = 0;
  for(size_t i=0; i<state.count; ++i) {
    const auto& sample = state.speeds[(state.first+i)%state.speeds.size()];
    const auto begin = std::max(sample.beginMs,windowStart);
    const auto end = std::min(sample.endMs,moveStart);
    if(end>begin) {
      distance += double(sample.speed)*double(end-begin);
      duration += end-begin;
      }
    }
  if(duration>0)
    result.avgSpeed = float(distance/double(duration));

  const float requestedLength = std::hypot(input.requested.x,input.requested.z);
  const float normalLength = std::hypot(input.normal.x,input.normal.z);
  float acceptedForward = 0;
  if(requestedLength>0) {
    acceptedForward = (input.accepted.x*input.requested.x+input.accepted.z*input.requested.z)/requestedLength;
    result.lost = 1.f-acceptedForward/requestedLength;
    }
  float cosine = 0;
  if(requestedLength>0 && normalLength>0) {
    cosine = std::clamp(-(input.requested.x*input.normal.x+input.requested.z*input.normal.z)/
                         (requestedLength*normalLength),-1.f,1.f);
    result.angleDeg = std::acos(cosine)*(180.f/3.14159265358979323846f);
    }
  result.speed = result.avgSpeed*cosine;
  result.strength = std::clamp((result.speed-200.f)/200.f,0.f,1.f);
  const bool eligible = input.human && input.grounded &&
      !input.airborne && !input.falling && !input.sliding && !input.swimming && !input.diving && !input.jumping &&
      !input.attacking && !input.blocking && !input.dodging && !input.rolling && !input.meleeStep &&
      !input.down && !input.ragdoll && !input.interactive;
  const bool contact = eligible && input.obstacle && normalLength>0 && requestedLength>0 && result.lost>=0.25f;
  result.eligible = eligible;
  result.contact = contact;
  result.contactStarted = contact && !state.contact;
  result.react = result.contactStarted && result.strength>0 &&
                 (!state.reacted || input.nowMs-state.lastReactionMs>=500);
  state.contact = contact;
  if(result.react) {
    state.lastReactionMs = input.nowMs;
    state.reacted = true;
    }

  // Append only after deciding: a stopped impact move must not lower its own speed estimate.
  if(input.dtMs>0) {
    if(state.count==state.speeds.size()) {
      state.first = (state.first+1)%state.speeds.size();
      --state.count;
      }
    state.speeds[(state.first+state.count)%state.speeds.size()] =
        {moveStart,input.nowMs,acceptedForward*1000.f/float(input.dtMs)};
    ++state.count;
    }
  return result;
  }
