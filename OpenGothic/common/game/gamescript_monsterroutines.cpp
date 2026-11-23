#include "gamescript.h"

#include "world/objects/npc.h"

namespace {
constexpr std::string_view roam = "FP_ROAM";
constexpr std::string_view campfire = "FP_CAMPFIRE";
constexpr size_t routinePosition = 19;
}

bool GameScript::runMonsterRoutine(Npc& npc, const daedalus::DaedalusSymbol& symbol, int& result) {
  const std::string_view name = symbol.name();
  const auto self = std::static_pointer_cast<daedalus::INpc>(npc.handlePtr());
  auto& position = npc.handle().aivar[routinePosition];

  if(name=="ZS_MM_RTN_SLEEP") {
    auto enable = [&](PercType perception, std::string_view callback) {
      npc.setPerceptionEnable(perception,findFunction(callback).ptr);
      };
    enable(PERC_ASSESSMAGIC,"B_ASSESSMAGIC");
    enable(PERC_ASSESSDAMAGE,"B_MM_ASSESSDAMAGE");
    enable(PERC_ASSESSOTHERSDAMAGE,"B_MM_ASSESSOTHERSDAMAGE");
    enable(PERC_ASSESSMURDER,"B_MM_ASSESSOTHERSDAMAGE");
    enable(PERC_ASSESSWARN,"B_MM_ASSESSWARN");
    enable(PERC_ASSESSQUIETSOUND,"B_MM_ASSESSQUIETSOUND_SLEEP");
    beginMonsterRoutine(npc);
    ai_gotofp(self,roam);
    npc.aiPush(AiQueue::aiPlayAnim("T_PERCEPTION"));
    npc.aiPush(AiQueue::aiPlayAnimBs("T_STAND_2_SLEEP",BS_LIE));
    result = 0;
    return true;
    }

  if(name=="ZS_MM_RTN_EATGROUND" || name=="ZS_MM_RTN_REST" || name=="ZS_MM_RTN_ORCSIT") {
    enableMonsterRoutinePerceptions(npc);
    beginMonsterRoutine(npc);
    if(name=="ZS_MM_RTN_EATGROUND") {
      ai_gotofp(self,roam);
      npc.aiPush(AiQueue::aiPlayAnim("T_STAND_2_EAT"));
      mdl_applyrandomani(self,"S_EAT","R_ROAM1");
      mdl_applyrandomani(self,"S_EAT","R_ROAM2");
      mdl_applyrandomani(self,"S_EAT","R_ROAM3");
      mdl_applyrandomanifreq(self,"S_EAT",8.f);
      }
    else {
      position = 1;
      }
    result = 0;
    return true;
    }

  if(name=="ZS_MM_RTN_SLEEP_LOOP" || name=="ZS_MM_RTN_EATGROUND_LOOP") {
    const auto routine = name=="ZS_MM_RTN_SLEEP_LOOP" ? MonsterSleep : MonsterEatGround;
    result = endMonsterRoutine(npc,routine) ? LOOP_END : LOOP_CONTINUE;
    return true;
    }

  if(name=="ZS_MM_RTN_REST_LOOP" || name=="ZS_MM_RTN_ORCSIT_LOOP") {
    const bool rest = name=="ZS_MM_RTN_REST_LOOP";
    if(rest)
      removeRavensStoneGuardians(npc);
    if(endMonsterRoutine(npc,rest ? MonsterRest : MonsterOrcSit)) {
      result = LOOP_END;
      return true;
      }
    if(position==1) {
      const auto point = rest ? roam : campfire;
      ai_gotofp(self,point);
      if(npc.isOnFreePoint(point)) {
        if(!rest)
          npc.aiPush(AiQueue::aiPlayAnimBs("T_STAND_2_GUARDSLEEP",BS_SIT));
        position = 0;
        }
      }
    else if(rest && rollRandom(1000,&npc,true)<=5) {
      static constexpr std::string_view variations[] = {"R_ROAM1","R_ROAM2","R_ROAM3"};
      const auto choice = rollRandom(3,&npc,true);
      findSymbol("ZS_MM_RTN_REST_LOOP.RANDOMMOVE")->set_int(int32_t(choice));
      npc.aiPush(AiQueue::aiPlayAnim(variations[choice]));
      }
    result = LOOP_CONTINUE;
    return true;
    }

  if(name=="ZS_MM_RTN_SLEEP_END")
    npc.aiPush(AiQueue::aiPlayAnimBs("T_SLEEP_2_STAND",BS_STAND));
  else if(name=="ZS_MM_RTN_EATGROUND_END")
    npc.aiPush(AiQueue::aiPlayAnim("T_EAT_2_STAND"));
  else if(name=="ZS_MM_RTN_ORCSIT_END")
    npc.aiPush(AiQueue::aiPlayAnimBs("T_GUARDSLEEP_2_STAND",BS_STAND));
  else if(name!="ZS_MM_RTN_REST_END")
    return false;
  result = 0;
  return true;
  }

bool GameScript::runShrinkState(Npc& npc, const daedalus::DaedalusSymbol& symbol, int& result) {
  const std::string_view name=symbol.name();
  const auto self=std::static_pointer_cast<daedalus::INpc>(npc.handlePtr());
  result=LOOP_CONTINUE;

  if(name=="ZS_MAGICSHRUNK") {
    npc_settarget(self,std::dynamic_pointer_cast<daedalus::INpc>(vm.global_hero()->get_instance()));
    return true;
    }
  if(name=="ZS_MAGICSHRUNK_LOOP") {
    ai_flee(self);
    return true;
    }
  // Both End phases leave the scale and reduced attributes in place.
  if(name.ends_with("_END"))
    return true;

  auto& record=npc.handle();
  auto& stage=record.aivar[42]; // AIV_MM_SHRINKSTATE, also used by the spell and saved NPCs.
  if(name=="ZS_MAGICSHRINK") {
    if(npc_hasbodyflag(self,BS_FLAG_INTERRUPTABLE))
      ai_standupquick(self);
    else
      ai_standup(self);
    stage=0;
    return true;
    }

  static constexpr float scale[]={
    0.90f,0.85f,0.80f,0.75f,0.70f,0.65f,0.60f,0.55f,0.50f,0.45f,0.40f,0.35f,0.30f
    };
  if(stage>=0 && stage<int(std::size(scale))) {
    const auto next=stage+1;
    const auto value=scale[stage];
    mdl_setmodelscale(self,value,value,value);
    stage=next;
    }
  else if(stage==13) {
    for(const auto attribute:{ATR_HITPOINTS,ATR_HITPOINTSMAX,ATR_STRENGTH})
      npc_changeattribute(self,attribute,-(record.attribute[attribute]*9/10));
    record.protection[PROT_BLUNT]/=10;
    record.protection[PROT_EDGE]/=10;
    record.protection[PROT_FIRE]/=10;
    record.protection[PROT_FLY]=0;
    record.protection[PROT_MAGIC]/=10;
    record.protection[PROT_POINT]/=10;
    stage=14;
    }
  else if(stage==14) {
    const auto shrunk=findFunction("ZS_MAGICSHRUNK");
    npc_clearaiqueue(self);
    record.start_aistate=int32_t(shrunk.ptr);
    ai_startstate(self,int(shrunk.ptr),1,"");
    result=LOOP_END;
    return true;
    }
  ai_wait(self,0.38f);
  return true;
  }
