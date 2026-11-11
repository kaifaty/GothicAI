#include "navreward.h"

#include <iostream>
#include <stdexcept>

namespace {
void check(bool condition,const char* message) {
  if(!condition)throw std::runtime_error(message);
}
const NavReward::Keys forward{1,0,false,false};
}

int main() {
  try {
    NavReward::Blocked window;
    window.measure(0,{},forward);window.measure(2999,{},forward);
    check(!window.active,"blocked window must reach 3000 ms");
    window.measure(3000,{},forward);
    check(window.active && window.since==0 && window.spans==1,
          "blocked span starts at the window's first sample, not its detection time");
    window.measure(3100,{50,0,0},forward);
    check(!window.active && window.totalMs==3100 && window.maxMs==3100 && window.until==3100,
          "50 cm of XZ displacement closes the blocked span at the current time");
    window.measure(6100,{50,1000,0},forward);
    check(window.active && window.since==3100 && window.spans==2,
          "height is ignored and a new blocked span must not overlap a closed one");
    window.close(6200);window.close(6300);
    check(!window.active && window.totalMs==6200 && window.maxMs==3100 && window.until==6200,
          "closing accounts for a final blocked span exactly once");

    for(uint64_t commanded:{1499u,1500u}) {
      NavReward::Blocked commands;
      commands.measure(0,{},{});
      commands.measure(commanded,{},forward);
      commands.measure(3000,{},{});
      check(commands.active==(commanded==1500),"commands must cover at least 1500 ms of the window");
    }
    for(float displacement:{49.999f,50.f}) {
      NavReward::Blocked distance;
      distance.measure(0,{},forward);distance.measure(3000,{0,0,displacement},forward);
      check(distance.active==(displacement<50.f),"XZ displacement must be strictly below 50 cm");
    }
    for(const auto keys:{NavReward::Keys{0,1,false,false},NavReward::Keys{0,0,true,false},
                         NavReward::Keys{0,0,false,true}}) {
      NavReward::Blocked commands;
      commands.measure(0,{},keys);commands.measure(3000,{},keys);
      check(commands.active,"strafe, held jump and held climb all count as movement commands");
    }
    NavReward::Blocked idle;
    idle.measure(0,{},{});idle.measure(3000,{},{});
    check(!idle.active,"standing without movement commands is not blocked");

    NavReward::Input input;
    input.previousPathCm=900;input.path={650,true};input.actionAt=4000;input.now=4100;
    input.previousHealth=100;input.health=75;
    input.arrived=true;input.blocked=true;input.dead=true;
    input.held={0,0,true,true};
    auto result=NavReward::transition(input);
    check(result.pathAfterCm==650 && result.progressCm==250 && result.timeMs==100,
          "progress and time must use the transition endpoints");
    check(result.arrived && result.blocked && result.death && result.jump && result.climb,
          "event and held-key reward components must be preserved");
    check(result.fallDamage==0,"damage without a flight or fall must not count as fall damage");
    input.fell=true;result=NavReward::transition(input);
    check(result.fallDamage==25,"damage after a flight or fall must count as fall damage");
    input.health=120;
    check(NavReward::transition(input).fallDamage==0,"healing must not produce negative fall damage");
    input.path={50,false};result=NavReward::transition(input);
    check(result.pathAfterCm==900 && result.progressCm==0 && NavReward::pathLength(900,input.path)==900,
          "an incomplete path must retain the previous complete length");
    input.path={1000,true};
    check(NavReward::transition(input).progressCm==-100,"moving away must retain negative progress");
    std::cout<<"navreward contract passed\n";
  }
  catch(const std::exception& error) {
    std::cerr<<"navreward contract failed: "<<error.what()<<'\n';return 1;
  }
}
