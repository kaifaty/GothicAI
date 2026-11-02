#include "game/meleeai.h"

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
  using namespace MeleeAi;
  requireNear(reactionMs(0),400.f,"novice reacts after 400 ms");
  requireNear(reactionMs(60),310.f,"talent 60 reacts after 310 ms");
  requireNear(reactionMs(100),250.f,"master reacts after 250 ms");
  require(dodgeStartMs(1000,1500,200.f,100,-900.f)==1250,"early desired start waits for the reaction");
  require(dodgeStartMs(1000,1700,200.f,100,0.f)==1500,"late desired start keeps its timing");
  require(dodgeStartMs(1000,1400,200.f,0,0.f)==1400,"novice cannot dodge before its 400 ms reaction");
  for(int talent:{0,60,100})
    require(dodgeStartMs(1000,1100,200.f,talent,-1000.f)>=1250,"no error starts a dodge before swing start plus 250 ms");

  using Direction = MeleeDodge::Direction;
  require(dodgeDirection(.49f)==Direction::Back,"u below 0.5 chooses back");
  require(dodgeDirection(.5f)==Direction::Left,"u at 0.5 chooses left");
  require(dodgeDirection(.74f)==Direction::Left,"u below 0.75 chooses left");
  require(dodgeDirection(.75f)==Direction::Right,"u at 0.75 chooses right");

  DodgeStreak streak;
  require(streak.allows(100),"first dodge is allowed");
  streak.dodged(100);
  require(streak.allows(200),"second dodge is allowed");
  streak.dodged(200);
  require(!streak.allows(300),"third consecutive dodge is refused");
  streak.reset();
  require(streak.allows(300),"own attack resets the streak");
  streak.dodged(300);
  streak.dodged(400);
  streak.reset();
  require(streak.allows(500),"own parade resets the streak");
  streak.dodged(500);
  streak.dodged(600);
  require(!streak.allows(3599) && streak.allows(3600),"three seconds without a dodge reset the streak");
  streak.dodged(3600);
  require(streak.count==1 && streak.allows(3601),"the first dodge after the idle interval begins a new streak");

  requireNear(riposteChance(0),0.f,"zero talent has zero riposte chance");
  requireNear(riposteChance(55),.55f,"talent 55 has 0.55 riposte chance");
  requireNear(riposteChance(100),1.f,"master has certain riposte chance");
  require(!riposteRoll(0,0.f),"zero chance fails even at zero roll");
  require(riposteRoll(55,.549f) && !riposteRoll(55,.55f),"a roll succeeds strictly below the probability");
  require(riposteRoll(100,.999f),"master succeeds throughout the half-open unit interval");

  require(bodyReaction(Action::JumpBack,true,true,true,false)==BodyReaction::DodgeBack,
          "P30 jump back substitutes an immediate backward dodge");
  require(bodyReaction(Action::Attack,true,true,true,true)==BodyReaction::RiposteRoll,
          "P30 direct attack on a stunned target substitutes a riposte roll");
  require(bodyReaction(Action::JumpBack,true,false,true,false)==BodyReaction::Original,
          "P30 dodge disabled preserves the original jump back");
  require(bodyReaction(Action::Attack,true,true,false,true)==BodyReaction::Original,
          "P30 riposte disabled preserves the original attack");
  require(bodyReaction(Action::Attack,true,true,true,false)==BodyReaction::Original,
          "P30 attack on an unstunned target is unchanged");
  for(auto action:{Action::JumpBack,Action::Attack,Action::Other}) {
    require(bodyReaction(action,false,true,true,true)==BodyReaction::Original,
            "P30 2H, fists, nonhumans and missing clips retain their actions");
    }
  require(bodyReaction(Action::Other,true,true,true,true)==BodyReaction::Original,
          "all other P30 actions are unchanged");
  std::puts("melee AI contract passed");
  }
