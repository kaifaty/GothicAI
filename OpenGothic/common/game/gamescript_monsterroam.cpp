#include "gamescript.h"

#include <array>

#include "world/objects/npc.h"
#include "world/world.h"

bool GameScript::runRoamingRoutine(Npc& npc, const daedalus::DaedalusSymbol& symbol, int& result) {
  constexpr std::string_view freePoint = "FP_ROAM";
  constexpr size_t positionStage = 19;
  constexpr int32_t inPosition = 0, outOfPosition = 1;

  const std::string_view name = symbol.name();
  auto& stage = npc.handle().aivar[positionStage];
  const auto goToFreePoint = [&] {
    if(const auto* point = world().findFreePoint(npc,freePoint))
      npc.aiPush(AiQueue::aiGoToPoint(*point));
    };

  result = LOOP_CONTINUE;
  if(name=="ZS_MM_RTN_ROAM") {
    enableMonsterRoutinePerceptions(npc);
    beginMonsterRoutine(npc);
    stage = outOfPosition;
    return true;
    }
  if(name=="ZS_MM_RTN_WUSEL") {
    enableMonsterRoutinePerceptions(npc);
    beginMonsterRoutine(npc,false,WalkBit::WM_Run);
    goToFreePoint();
    return true;
    }
  if(name=="ZS_MM_RTN_ROAM_END" || name=="ZS_MM_RTN_WUSEL_END")
    return true;

  if(name=="ZS_MM_RTN_ROAM_LOOP") {
    if(endMonsterRoutine(npc,MonsterRoam)) {
      result = LOOP_END;
      return true;
      }
    // The stock routine shares this timer value between NPCs and saves it with the script variables.
    auto* wanderTime = findSymbol("ZS_MM_RTN_ROAM_LOOP.WANDERTIME");
    if(stage==outOfPosition) {
      wanderTime->set_int(int32_t(rollRandom(5,&npc,true)));
      npc.setStateTime(0);
      stage = inPosition;
      }
    if(int32_t(npc.stateTime()/1000)>wanderTime->get_int()) {
      if(world().findNextFreePoint(npc,freePoint)!=nullptr)
        npc.aiPush(AiQueue::aiGoToNextFp(freePoint));
      stage = outOfPosition;
      }
    else if(rollRandom(1000,&npc,true)<=5) {
      constexpr std::array animations = {"R_ROAM1","R_ROAM2","R_ROAM3"};
      const auto roll = rollRandom(3,&npc,true);
      findSymbol("ZS_MM_RTN_ROAM_LOOP.RANDOMMOVE")->set_int(int32_t(roll));
      npc.aiPush(AiQueue::aiPlayAnim(animations[roll]));
      }
    return true;
    }

  if(name=="ZS_MM_RTN_WUSEL_LOOP") {
    if(endMonsterRoutine(npc,MonsterWusel)) {
      result = LOOP_END;
      return true;
      }
    if(int32_t(npc.stateTime()/1000)>=1) {
      if(npc.isOnFreePoint(freePoint)) {
        if(world().findNextFreePoint(npc,freePoint)!=nullptr) {
          npc.clearAiQueue();
          npc.aiPush(AiQueue::aiGoToNextFp(freePoint));
          }
        }
      else {
        const auto body = int32_t(npc.bodyState());
        if(!ActivityDefinitions::bodyStateContains(body,BS_RUN) &&
           !ActivityDefinitions::bodyStateContains(body,BS_WALK))
          goToFreePoint();
        }
      npc.setStateTime(0);
      stage = outOfPosition;
      }
    return true;
    }
  return false;
  }
