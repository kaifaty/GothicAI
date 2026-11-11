#include "navreward.h"

#include <algorithm>
#include <cmath>

void NavReward::Blocked::close(uint64_t elapsed) {
  if(!active)return;
  const auto duration=elapsed-since;
  totalMs+=duration;maxMs=std::max(maxMs,duration);
  until=elapsed;active=false;
}

void NavReward::Blocked::measure(uint64_t elapsed,const Tempest::Vec3& position,const Keys& keys) {
  motion.push_back({elapsed,position,keys.forward!=0 || keys.strafe!=0 || keys.jump || keys.climb});
  const auto cutoff=elapsed>3000?elapsed-3000:0;
  while(motion.size()>1 && motion[1].time<=cutoff)motion.pop_front();
  uint64_t commanded=0;
  for(size_t i=1;i<motion.size();++i)
    if(motion[i].commanded)commanded+=motion[i].time-motion[i-1].time;
  const auto displacement=motion.back().position-motion.front().position;
  const bool stalled=elapsed-motion.front().time>=3000 && commanded>=1500 &&
                     std::hypot(displacement.x,displacement.z)<50.f;
  if(stalled && !active) {
    active=true;since=std::max(motion.front().time,until);++spans;
  }
  else if(!stalled)close(elapsed);
}

double NavReward::pathLength(double previousCm,const Path& path) {
  return path.complete?path.lengthCm:previousCm;
}

NavReward::Components NavReward::transition(const Input& input) {
  Components out;
  out.pathAfterCm=pathLength(input.previousPathCm,input.path);
  out.progressCm=input.previousPathCm-out.pathAfterCm;
  out.arrived=input.arrived;out.blocked=input.blocked;out.death=input.dead;
  out.fallDamage=input.fell?std::max(0,input.previousHealth-input.health):0;
  out.jump=input.held.jump;out.climb=input.held.climb;
  out.timeMs=input.now-input.actionAt;
  return out;
}
