#include "game/meleestamina.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

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
  using namespace MeleeStamina;
  requireNear(maximum(10),82.f,"reserve at strength 10");
  requireNear(maximum(100),100.f,"reserve at strength 100");
  requireNear(attackCost(30,10),17.36f,"sword cost at talent 10 percent");
  requireNear(attackCost(30,100),9.8f,"sword cost at talent 100 percent");
  requireNear(attackCost(0,0,true),8.f,"fist cost");

  State state;
  recover(state,100.f,1000,false);
  requireNear(state.current,100.f,"new state starts full");
  require(spend(state,50.f,1000,true),"accepted attack spends stamina");
  recover(state,100.f,1799,false);
  requireNear(state.current,50.f,"no recovery before 800 ms");
  recover(state,100.f,1800,false);
  requireNear(state.current,50.f,"no recovery at 800 ms");
  recover(state,100.f,2800,false);
  requireNear(state.current,85.f,"recovery is 35 per second after delay");
  recover(state,100.f,3800,true);
  requireNear(state.current,85.f,"parade holds recovery");
  recover(state,100.f,3900,false);
  requireNear(state.current,88.5f,"parade time does not accumulate recovery");
  recover(state,100.f,4900,false);
  requireNear(state.current,100.f,"recovery is capped at reserve");

  reset(state,140.f,1000);
  require(spend(state,139.f,1000,true),"partial spend accepted");
  require(canAct(state,true),"one stamina allows player action");
  require(!canAfford(state,JumpBackCost,true),"native AI requires the full cost");
  require(spend(state,JumpBackCost,1000,true),"action can cost more than the remainder");
  requireNear(state.current,0.f,"spending clamps to zero");
  require(state.exhausted && !canAct(state,true),"empty state is exhausted and refuses actions");
  require(!spend(state,8.f,1100,true),"exhausted action is refused");
  requireNear(float(state.lastSpend),1000.f,"refusal does not restart recovery delay");
  recover(state,140.f,2760,false);
  requireNear(state.current,33.6f,"recovery reaches 24 percent");
  require(state.exhausted && !canAct(state,true),"24 percent remains exhausted");
  require(!canAfford(state,8.f,true),"native AI respects exhaustion even when cost is affordable");
  recover(state,140.f,2800,false);
  requireNear(state.current,35.f,"recovery reaches 25 percent");
  require(!state.exhausted && canAct(state,true),"25 percent clears exhaustion");

  require(spend(state,100.f,2800,true),"second depletion accepted");
  require(canAct(state,false),"disabled flag bypasses exhaustion gate");
  require(canAfford(state,100.f,false),"disabled flag bypasses native cost gate");
  require(spend(state,100.f,3000,false),"disabled flag accepts action at zero");
  requireNear(state.current,0.f,"disabled flag does not spend");
  require(state.lastSpend==2800,"disabled flag leaves spend time unchanged");
  drain(state,10.f,3200);
  require(state.current==0.f && state.exhausted && state.lastSpend==3200 && state.lastUpdate==3200,
          "accepted defence at exhaustion restarts recovery delay without leaving zero");
  std::puts("melee stamina contract passed");
  }
