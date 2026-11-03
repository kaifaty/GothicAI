#include "game/meleecharge.h"
#include "game/meleestamina.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>

namespace {
  void require(bool condition, const char* message) {
    if(condition)
      return;
    std::fprintf(stderr,"FAIL: %s\n",message);
    std::exit(EXIT_FAILURE);
    }

  void requireNear(float actual, float expected, const char* message) {
    if(std::isfinite(actual) && std::abs(actual-expected)<.0001f)
      return;
    std::fprintf(stderr,"FAIL: %s: actual %.9g, expected %.9g\n",message,double(actual),double(expected));
    std::exit(EXIT_FAILURE);
    }
  }

int main() {
  using namespace MeleeCharge;

  require(!startsWindup(199),"199 ms still waits for a tap");
  require( startsWindup(200),"200 ms begins the windup");

  Situation stance;
  stance.player = stance.oneHand = stance.windupAnimation = stance.holdAnimation = stance.releaseAnimation =
      stance.standing = true;
  require(available(stance),"the hero's complete 1H stance allows charging");
  for(auto member:{&Situation::enabled,&Situation::player,&Situation::oneHand,&Situation::windupAnimation,
                   &Situation::holdAnimation,&Situation::releaseAnimation,&Situation::standing}) {
    auto s = stance;
    s.*member = false;
    require(!available(s),"disabled, NPC, 2H/fist, missing clip or moving: immediate ordinary attack");
    }
  for(auto member:{&Situation::attacking,&Situation::parading,&Situation::stunned,&Situation::knockedDown,
                   &Situation::inAir,&Situation::inWater}) {
    auto s = stance;
    s.*member = true;
    require(!available(s),"a busy, stunned, downed, airborne or wet stance cannot begin charging");
    }
  auto ai = stance;
  ai.player = false;
  require(!available(ai),"an NPC without melee_ai_charge cannot charge");
  ai.aiEnabled = true;
  require(available(ai),"melee_ai_charge admits the same complete 1H stance");
  for(auto member:{&Situation::enabled,&Situation::oneHand,&Situation::windupAnimation,&Situation::holdAnimation,
                   &Situation::releaseAnimation,&Situation::standing}) {
    auto s = ai;
    s.*member = false;
    require(!available(s),"flagged AI still needs every hero charge prerequisite");
    }
  for(auto member:{&Situation::attacking,&Situation::parading,&Situation::stunned,&Situation::knockedDown,
                   &Situation::inAir,&Situation::inWater}) {
    auto s = ai;
    s.*member = true;
    require(!available(s),"flagged AI has the same busy-state refusals as the hero");
    }

  require(!releaseReady(0,true) && !releaseReady(399,true),"release during windup waits for its end");
  require(releaseReady(400,true),"the earliest release starts after 400 ms of windup");
  require(!releaseReady(400,false) && !releaseReady(2999,false),"a held key loops until the limit");
  require(!autoRelease(2999) && autoRelease(3000),"auto release begins at 3000 ms");
  require(releaseReady(3000,false),"the limit releases without a key release");

  const uint64_t times[] = {400,600,1000,1500};
  const float levels[] = {.4f,.6f,1.f,1.f};
  for(unsigned i=0; i<4; ++i) {
    requireNear(level(times[i]),levels[i],"charge grows for 1000 ms then clamps");
    requireNear(multiplier(times[i]),1.f+levels[i],"damage and posture use 1+c");
    }
  requireNear(level(0),0.f,"charge starts at zero");

  require(superArmor(true,false,0,520),"windup and hold have super armor from their start");
  require(superArmor(false,true,0,520) && superArmor(false,true,519,520) &&
          superArmor(false,true,520,520),"release has super armor through DEF_HIT_END");
  require(!superArmor(false,true,521,520),"release recovery after DEF_HIT_END is open");
  require(!superArmor(false,false,0,520),"outside a charge there is no charge super armor");

  const auto ordinaryCost = MeleeStamina::attackCost(40,60);
  requireNear(cost(ordinaryCost,true),1.5f*ordinaryCost,"charging costs 1.5 ordinary attacks");
  requireNear(cost(ordinaryCost,false),0.f,"without melee_stamina the charge is free");
  MeleeStamina::State stamina;
  MeleeStamina::reset(stamina,cost(ordinaryCost,true),0);
  require(MeleeStamina::canAfford(stamina,cost(ordinaryCost,true),true),"AI can afford exactly the full charge price");
  MeleeStamina::drain(stamina,1.f,0);
  require(!MeleeStamina::canAfford(stamina,cost(ordinaryCost,true),true),"AI needs the full charge price");

  std::puts("melee charge contract passed");
  }
