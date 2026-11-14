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

  requireNear(chargeChance(0),0.f,"zero talent has zero charge chance");
  requireNear(chargeChance(50),.15f,"talent 50 has 0.15 charge chance");
  requireNear(chargeChance(100),.3f,"master has 0.3 charge chance");
  require(!chargeRoll(0,0.f) && chargeRoll(50,.149f) && !chargeRoll(50,.15f),
          "charge roll succeeds strictly below its chance");
  require(chargeHoldMs(0.f)==400 && chargeHoldMs(1.f)==1200,"uniform endpoints hold for 400 and 1200 ms");
  require(!chargeRelease(799,800,true) && chargeRelease(800,800,true),"charge releases in range at its hold time");
  require(!chargeRelease(800,800,false) && !chargeRelease(1100,800,false) && chargeRelease(1100,800,true),
          "a target outside range delays release until it returns");
  require(!chargeRelease(2999,1200,false) && chargeRelease(3000,1200,false),"auto release ignores range at 3000 ms");

  require(specialCooldownReady(0,std::nullopt),"no previous special has no cooldown");
  require(!specialCooldownReady(6999,1000) && specialCooldownReady(7000,1000),
          "charge cooldown ends at 6000 ms");

  const int actor = 0, other = 1;
  {
    SpecialSlot slot;
    require(slot.acquire(&actor,Special::Charge) && slot.busy() && slot.kind==Special::Charge,
            "charge acquires a free target");
    require(!slot.acquire(&other,Special::Charge),"a charge excludes another charge on this target");
    slot.release(&other);
    require(slot.busy(),"another actor cannot return the target slot");
    slot.release(&actor);
    require(!slot.busy() && slot.kind==Special::None,"the owner returns its target slot");
    require(slot.acquire(&other,Special::Charge),"a returned slot can start another charge");
    }

  std::puts("melee AI contract passed");
  }
