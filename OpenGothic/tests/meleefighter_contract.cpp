#include "game/meleefighter.h"

#include <cstdio>
#include <cstdlib>

namespace {
  void require(bool condition, const char* message) {
    if(condition)
      return;
    std::fprintf(stderr,"FAIL: %s\n",message);
    std::exit(EXIT_FAILURE);
    }
  }

int main() {
  using namespace MeleeFighter;
  require(qualifies(false,true,true,true,false,200.f*200.f),"melee attack with a standing NPC target qualifies");
  require(!qualifies(true,true,true,true,false,200.f*200.f),"the hero is not a fighter");
  require(!qualifies(false,false,true,true,false,200.f*200.f),"ranged attacks do not qualify");
  require(!qualifies(false,true,false,true,false,200.f*200.f),"a spectator does not qualify");
  require(!qualifies(false,true,true,false,false,0.f),"an NPC target is required");
  require(!qualifies(false,true,true,true,true,200.f*200.f),"a downed target does not qualify");
  require(qualifies(false,true,true,true,false,1499.f*1499.f),"a target inside fifteen meters qualifies");
  require(!qualifies(false,true,true,true,false,1500.f*1500.f),"fifteen meters is outside fighter range");

  State attacker, target, spectator;
  require(!active(attacker,0,false),"a fresh actor has no fighter hold");
  refresh(attacker,target,1000);
  require(active(attacker,1000,false) && active(target,1000,false),"both attacker and target become fighters");
  require(!active(spectator,1000,false),"proximity alone does not promote a spectator");
  require(!active(target,1000,true),"target promotion does not change the hero");
  require(active(attacker,3999,false) && active(target,3999,false),"both remain fighters for three seconds");
  require(!active(attacker,4000,false) && !active(target,4000,false),"the hold expires after three seconds");
  refresh(attacker,target,3500);
  require(active(attacker,6499,false) && !active(attacker,6500,false),"a later attack tick renews the hold");

  const float distances[] = {1200.f,2500.f,4000.f};
  for(float distance:distances) {
    const float squared = distance*distance;
    require(keepPhysics(true,true,active(attacker,3500,false),false,squared),"a fighter enters physics at every distance");
    require(keepPhysics(true,true,active(target,3500,false),true,squared),"a target retains physics at every distance");
    require(keepPhysics(true,true,false,false,squared)==(distance<2000.f),"spectators retain the hero ring");
    require(!keepPhysics(false,true,true,true,squared),"disabled controller or living-body flags win over promotion");
    require(!keepPhysics(true,false,true,true,squared),"other eligibility gates win over promotion");
    require(keepPhysics(true,true,active(attacker,6500,false),true,squared)==(distance<=2200.f),
            "after combat the actor returns to the hero ring");
    }
  require(!keepPhysics(true,true,false,false,2000.f*2000.f),"ordinary entry is strictly inside twenty meters");
  require(keepPhysics(true,true,false,true,2200.f*2200.f),"ordinary retention includes twenty-two meters");
  require(!keepPhysics(true,true,false,true,2201.f*2201.f),"ordinary actors leave beyond twenty-two meters");

  require(physicsHits(true,false,true),"an NPC with CharacterVirtual uses physical hits");
  require(!physicsHits(true,false,false),"an NPC without the magnet falls back to legacy damage");
  require(physicsHits(true,true,false),"the hero retains physical hits without the NPC controller gate");
  require(!physicsHits(false,false,true) && !physicsHits(false,true,true),"disabled physical hits use legacy damage");
  std::puts("melee fighter contract passed");
  }
