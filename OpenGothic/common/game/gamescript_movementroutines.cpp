#include "gamescript.h"

#include "game/npcactivityhost.h"
#include "world/objects/npc.h"
#include "world/world.h"

namespace {
constexpr std::string_view bed = "BEDHIGH";

bool inBodyState(const Npc& npc, BodyState state) {
  return ActivityDefinitions::bodyStateContains(int32_t(npc.bodyState()),int32_t(state));
}
}

bool GameScript::runMovementRoutine(Npc& npc, const daedalus::DaedalusSymbol& symbol, int& result) {
  const std::string_view name = symbol.name();
  const auto self = std::static_pointer_cast<daedalus::INpc>(npc.handlePtr());
  auto& properties = npc.handle();

  auto enable = [&](PercType perception, std::string_view callback) {
    npc.setPerceptionEnable(perception,findFunction(callback).ptr);
    };
  auto reset = [&] {
    if(npc.lookAtTarget()!=nullptr)
      npc.aiPush(AiQueue::aiStopLookAt());
    npc.aiPush(AiQueue::aiRemoveWeapon());
    };
  auto standardInit = [&] {
    vm.call_function<void>("PERCEPTION_SET_NORMAL");
    reset();
    };
  auto goToWaypoint = [&] {
    NpcEngineCalls::gotoWaypoint(world(),npc,properties.wp);
    };

  if(name=="ZS_GOTOBED") {
    standardInit();
    npc.aiPush(AiQueue::aiSetWalkMode(WalkBit::WM_Walk));
    if(NpcEngineCalls::nearestWaypoint(world(),npc)!=properties.wp)
      goToWaypoint();
    // The transition also starts at an already reached bed and keeps the row's end time.
    ai_startstate(self,int(findFunction("ZS_SLEEP").ptr),0,"");
    result = 0;
    return true;
    }
  if(name=="ZS_SLEEP") {
    enable(PERC_ASSESSQUIETSOUND,"B_ASSESSQUIETSOUND");
    enable(PERC_ASSESSDAMAGE,"B_ASSESSDAMAGE");
    enable(PERC_ASSESSTALK,"B_ASSESSSLEEPTALK");
    npc.aiPush(AiQueue::aiSetWalkMode(WalkBit::WM_Walk));
    result = 0;
    return true;
    }
  if(name=="ZS_SLEEP_END") {
    if(!npc_isinplayersroom(self))
      ai_outputsvm(self,self,"$AWAKE");
    npc.aiPush(AiQueue::aiUseMob(bed,-1));
    result = 0;
    return true;
    }
  if(name=="ZS_SLEEP_LOOP") {
    result = LOOP_CONTINUE;
    if(inBodyState(npc,BS_LIE))
      return true;
    if(world().availableMob(npc,bed)!=nullptr) {
      npc.aiPush(AiQueue::aiUseMob(bed,1));
      return true;
      }
    auto* hero = findNpc(vm.global_hero());
    if(hero!=nullptr && npc_getdisttonpc(self,std::static_pointer_cast<daedalus::INpc>(hero->handlePtr()))<=500 && inBodyState(*hero,BS_LIE)) {
      auto& complaint = *findSymbol("PLAYER_GETOUTOFMYBEDCOMMENT");
      if(complaint.get_int()==0) {
        npc.aiPush(AiQueue::aiTurnToNpc(hero));
        if(auto* other = findNpc(vm.global_other()))
          ai_outputsvm(self,std::static_pointer_cast<daedalus::INpc>(other->handlePtr()),"$GETOUTOFMYBED");
        complaint.set_int(1);
        }
      }
    return true;
    }

  if(name=="ZS_FLEETOWP" || name=="ZS_RUNTOWP") {
    standardInit();
    if(name=="ZS_FLEETOWP") {
      if(hlp_getinstanceid(findSymbol("CORNELIUS")->get_instance())==hlp_getinstanceid(self))
        npc.setPerceptionDisable(PERC_ASSESSENTERROOM);
      }
    npc.aiPush(AiQueue::aiSetWalkMode(WalkBit::WM_Run));
    if(name=="ZS_FLEETOWP")
      mdl_applyoverlaymds(self,"HUMANS_FLEE.MDS");
    if(NpcEngineCalls::distanceToWaypoint(world(),npc,properties.wp)>500)
      goToWaypoint();
    npc.aiPush(AiQueue::aiAlignToWp());
    npc.aiPush(AiQueue::aiPlayAnim("T_STAND_2_HGUARD"));
    result = 0;
    return true;
    }
  if(name=="ZS_FLEETOWP_LOOP" || name=="ZS_RUNTOWP_LOOP") {
    if(npc.stateTime()/1000>5) {
      const auto roll = int32_t(rollRandom(2,&npc,true));
      findSymbol(symbol.name()+".RANDOM")->set_int(roll);
      if(roll==0)
        npc.aiPush(AiQueue::aiPlayAnim("T_HGUARD_LOOKAROUND"));
      npc.setStateTime(0);
      }
    result = LOOP_CONTINUE;
    return true;
    }

  if(name=="ZS_FOLLOW_PLAYER" || name=="ZS_GUIDE_PLAYER") {
    const bool following = name=="ZS_FOLLOW_PLAYER";
    if(!following) {
      vm.call_function<void>("PERCEPTION_SET_MINIMAL");
      reset();
      }
    properties.senses = int32_t(SensesBit::SENSE_SEE) | int32_t(SensesBit::SENSE_HEAR) | int32_t(SensesBit::SENSE_SMELL);
    properties.senses_range = 2000;
    npc.setPerceptionTime(following ? 300 : 1000);
    if(following)
      vm.call_function<void>("PERCEPTION_SET_MINIMAL");
    enable(PERC_ASSESSPLAYER,following ? "B_ASSESSFOLLOWPLAYER" : "B_ASSESSGUIDEPLAYER");
    enable(PERC_ASSESSENEMY,"B_ASSESSENEMY");
    enable(PERC_ASSESSTALK,"B_ASSESSTALK");
    enable(PERC_MOVEMOB,"B_MOVEMOB");
    enable(PERC_ASSESSFIGHTSOUND,following ? "B_ASSESSFOLLOWFIGHTSOUND" : "B_ASSESSGUIDEFIGHTSOUND");
    if(following) {
      reset();
      npc.aiPush(AiQueue::aiStandup());
      npc.aiPush(AiQueue::aiSetWalkMode(WalkBit::WM_Run));
      if(properties.aivar[80]==0)
        properties.aivar[80] = 500;
      }
    result = 0;
    return true;
    }
  if(name=="ZS_FOLLOW_PLAYER_LOOP") {
    auto* hero = findNpc(vm.global_hero());
    const auto heroRef = hero!=nullptr ? std::static_pointer_cast<daedalus::INpc>(hero->handlePtr()) : nullptr;
    const auto turnWhenDue = [&] {
      if(npc.stateTime()/1000>1) {
        npc.aiPush(AiQueue::aiTurnToNpc(hero));
        npc.setStateTime(0);
        }
      };
    if(npc_getdisttonpc(self,heroRef)<=properties.aivar[80]) {
      turnWhenDue();
      }
    else if(hlp_getinstanceid(self)==hlp_getinstanceid(findSymbol("DIEGOOW")->get_instance()) &&
            vm.call_function<int32_t>("C_DIEGOTOOFAR",int32_t(0))) {
      // The quest's allowed route remains shared with its dialogue and perceptions.
      turnWhenDue();
      }
    else {
      if(!inBodyState(npc,BS_SWIM))
        npc.aiPush(AiQueue::aiSetWalkMode(WalkBit::WM_Run));
      npc.aiPush(AiQueue::aiGoToNpc(hero));
      properties.aivar[19] = 0;
      }
    result = LOOP_CONTINUE;
    return true;
    }
  if(name=="ZS_GUIDE_PLAYER_LOOP") {
    auto* hero = findNpc(vm.global_hero());
    const auto heroRef = hero!=nullptr ? std::static_pointer_cast<daedalus::INpc>(hero->handlePtr()) : nullptr;
    const auto selfDistance = NpcEngineCalls::distanceToWaypoint(world(),npc,properties.wp);
    const auto heroDistance = npc_getdisttowp(heroRef,properties.wp);
    if(selfDistance<heroDistance && npc_getdisttonpc(self,heroRef)>800) {
      // This reaction is also installed as the guide's assess-player callback.
      vm.call_function<void>("B_ASSESSGUIDEPLAYER");
      }
    else if(selfDistance>300) {
      if(!inBodyState(npc,BS_SWIM))
        npc.aiPush(AiQueue::aiSetWalkMode(WalkBit::WM_Run));
      goToWaypoint();
      }
    else {
      npc.aiPush(AiQueue::aiTurnToNpc(hero));
      }
    result = LOOP_CONTINUE;
    return true;
    }
  if(name=="ZS_FLEETOWP_END" || name=="ZS_RUNTOWP_END" || name=="ZS_FOLLOW_PLAYER_END" || name=="ZS_GUIDE_PLAYER_END") {
    result = 0;
    return true;
    }
  return false;
  }
