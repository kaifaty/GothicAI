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
    setPerceptionPreset(true);
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
      setPerceptionPreset(false);
      reset();
      }
    properties.senses = int32_t(SensesBit::SENSE_SEE) | int32_t(SensesBit::SENSE_HEAR) | int32_t(SensesBit::SENSE_SMELL);
    properties.senses_range = 2000;
    npc.setPerceptionTime(following ? 300 : 1000);
    if(following)
      setPerceptionPreset(false);
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
            diegoRouteBoundary(0)!=0) {
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
      runGuidePlayerAssessment();
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

void GameScript::runFollowerPlayerAssessment() {
  const auto self = std::dynamic_pointer_cast<daedalus::INpc>(vm.global_self()->get_instance());
  if(self==nullptr)
    return;
  const auto hero = std::dynamic_pointer_cast<daedalus::INpc>(vm.global_hero()->get_instance());
  constexpr size_t positionStage = 19, followDistance = 80;
  auto& stage = self->aivar[positionStage];
  const auto stand = [&] {
    npc_clearaiqueue(self);
    ai_standup(self);
    stage = 1;
    };
  if(stage==0 && npc_getdisttonpc(self,hero)<self->aivar[followDistance]) {
    stand();
    return;
    }
  if(hlp_getinstanceid(self)==hlp_getinstanceid(findSymbol("DIEGOOW")->get_instance())) {
    const auto boundary = diegoRouteBoundary(0);
    if(stage==0 && boundary!=0) {
      stand();
      if(npc_getdisttonpc(self,hero)>findSymbol("PERC_DIST_DIALOG")->get_int()) {
        const auto other = std::dynamic_pointer_cast<daedalus::INpc>(vm.global_other()->get_instance());
        sayToNpc(self,other,"$SC_HEYWAITASECOND");
        }
      return;
      }
    }
  runPlayerAssessment();
  }

void GameScript::runGuidePlayerAssessment() {
  const auto self = std::dynamic_pointer_cast<daedalus::INpc>(vm.global_self()->get_instance());
  if(self==nullptr)
    return;
  const auto hero = std::dynamic_pointer_cast<daedalus::INpc>(vm.global_hero()->get_instance());
  const auto distance = npc_getdisttonpc(self,hero);
  if(distance<=800) {
    runPlayerAssessment();
    return;
    }
  if(npc_getdisttowp(self,self->wp)<npc_getdisttowp(hero,self->wp)) {
    npc_clearaiqueue(self);
    ai_standup(self);
    turnToNpc(self,hero);
    }
  constexpr size_t enemyOverride = 61;
  if(npc_getattitude(self,hero)==ATT_HOSTILE && self->aivar[enemyOverride]==1 && distance>1200)
    self->aivar[enemyOverride] = 0;
  }

void GameScript::runCompanionFightSound(bool follower) {
  using Actor = std::shared_ptr<daedalus::INpc>;
  const auto source = std::dynamic_pointer_cast<daedalus::INpc>(vm.global_other()->get_instance());
  const auto victim = std::dynamic_pointer_cast<daedalus::INpc>(vm.global_victim()->get_instance());
  constexpr size_t partyMember = 15; // AIV_PARTYMEMBER
  const auto inParty = [](const Actor& actor) { return actor!=nullptr && actor->aivar[partyMember]==1; };
  if(npc_isplayer(victim) || npc_isplayer(source) || (follower && (inParty(victim) || inParty(source))))
    runFightSoundReaction();
  }

int32_t GameScript::diegoRouteBoundary(int32_t tolerance) {
  findSymbol("C_DIEGOTOOFAR.TOLERANCE")->set_int(tolerance);
  const auto hero = std::dynamic_pointer_cast<daedalus::INpc>(vm.global_hero()->get_instance());
  const auto near = [&](std::string_view point, int32_t radius) {
    return npc_getdisttowp(hero,point)<int64_t(radius)+tolerance;
    };
  struct Boundary {
    std::string_view point;
    int32_t radius;
    int32_t region;
    };
  static constexpr Boundary north[] = {
    {"SPAWN_OW_SHADOWBEAST_10_01",4000,1}, // Angar
    {"OW_ICEREGION_ENTRANCE_01",  3000,2},
    {"OW_PATH_046",              1800,4}, // Swamp
    {"CASTLE_5",                 1500,3}, // Fire dragon
    {"MT13",                     5500,5}, // Lake
    {"MT16",                     6500,5},
    };
  for(const auto& boundary : north)
    if(near(boundary.point,boundary.radius))
      return boundary.region;

  const auto self = std::dynamic_pointer_cast<daedalus::INpc>(vm.global_self()->get_instance());
  if((npc_getheighttonpc(self,hero)>1000 && npc_getdisttowp(hero,"DT_E3_03")<1000) ||
     wld_getplayerportalguild()==int32_t(GIL_DMT) || near("DT_MAINGATE",500))
    return 6;

  static constexpr Boundary minesAndCastle[] = {
    {"OW_NEWMINE_03_B",           2500,7},
    {"OW_NEWMINE_03",             2500,7},
    {"OW_MINE3_OUT",              1200,8},
    {"OW_PATH_266",               3000,9},
    {"OC_CENTER_05",             12000,10},
    {"OC_CENTER_02",             12000,10},
    {"PATH_OC_PSI_01",            5000,11},
    {"OC_ORCBARRIER_08",          5000,11},
    {"OW_PATH_BLOODFLY01_SPAWN01", 5000,12},
    };
  for(const auto& boundary : minesAndCastle)
    if(near(boundary.point,boundary.radius))
      return boundary.region;
  return 0;
  }
