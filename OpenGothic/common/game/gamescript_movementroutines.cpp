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

bool GameScript::runFearOrSleepSpell(Npc& npc, const daedalus::DaedalusSymbol& symbol, int& result) {
  const std::string_view name = symbol.name();
  const auto self = std::static_pointer_cast<daedalus::INpc>(npc.handlePtr());
  result = LOOP_CONTINUE;
  if(name=="ZS_MAGICFLEE" || name=="ZS_MAGICSLEEP") {
    const bool fear = name=="ZS_MAGICFLEE";
    npc.setPerceptionEnable(PERC_ASSESSDAMAGE,findFunction(fear ? "B_STOPMAGICFLEE" : "B_STOPMAGICSLEEP").ptr);
    npc.setPerceptionEnable(PERC_ASSESSMAGIC,findFunction("B_ASSESSMAGIC").ptr);
    if(!fear)
      npc.setPerceptionEnable(PERC_ASSESSTALK,findFunction("B_ASSESSMAGICSLEEPTALK").ptr);

    npc.handle().aivar[12] = 0; // AIV_GUARDPOINT = GP_NONE
    npc_setrefusetalk(self,0);
    const auto hero = std::dynamic_pointer_cast<daedalus::INpc>(vm.global_hero()->get_instance());
    npc_settempattitude(self,npc_getpermattitude(self,hero));
    findSymbol("B_STOPLOOKAT.SLF")->set_instance(self);
    const auto lookTarget = npc_getlookattarget(self);
    findSymbol("B_STOPLOOKAT.TARGET")->set_instance(lookTarget);
    if(hlp_isvalidnpc(lookTarget))
      ai_stoplookat(self);
    ai_stoppointat(self);
    if(npc_hasbodyflag(self,int32_t(BS_FLAG_INTERRUPTABLE)))
      ai_standupquick(self);
    else
      ai_standup(self);

    if(!fear) {
      npc.aiPush(AiQueue::aiPlayAnimBs("T_STAND_2_VICTIM_SLE",BS_LIE));
      }
    else if(npc.handle().guild<int32_t(GIL_SEPERATOR_HUM)) {
      static constexpr std::string_view gestures[] = {
        "T_STAND_2_FEAR_VICTIM1", "T_STAND_2_FEAR_VICTIM2", "T_STAND_2_FEAR_VICTIM3",
        };
      const auto roll = rollRandom(3,&npc,true);
      findSymbol("ZS_MAGICFLEE.RANDY")->set_int(int32_t(roll));
      npc.aiPush(AiQueue::aiPlayAnimBs(gestures[roll],BS_STAND));
      }
    }
  else if(name=="ZS_MAGICFLEE_LOOP" && npc_getstatetime(self)>5) {
    npc.clearAiQueue();
    stopMagicFear(npc);
    }
  else if(name=="ZS_MAGICSLEEP_LOOP" && npc_getstatetime(self)>30) {
    npc.clearAiQueue();
    stopMagicSleep(npc);
    result = LOOP_END;
    }
  return true;
  }

void GameScript::stopMagicFear(Npc& npc) {
  const auto self = std::static_pointer_cast<daedalus::INpc>(npc.handlePtr());
  npc.setPerceptionDisable(PERC_ASSESSDAMAGE);
  const auto other = std::dynamic_pointer_cast<daedalus::INpc>(vm.global_other()->get_instance());
  npc_settarget(self,other);
  ai_startstate(self,int(findFunction("ZS_FLEE").ptr),0,"");
  }

void GameScript::stopMagicSleep(Npc& npc) {
  npc.setPerceptionDisable(PERC_ASSESSDAMAGE);
  sayMagicReaction(npc,"$WHATWASSTHAT",true);
  npc.aiPush(AiQueue::aiPlayAnim("T_VICTIM_SLE_2_STAND"));
  }

void GameScript::sayMagicReaction(Npc& npc, std::string_view line, bool overlay) {
  const auto self = std::static_pointer_cast<daedalus::INpc>(npc.handlePtr());
  const auto other = std::dynamic_pointer_cast<daedalus::INpc>(vm.global_other()->get_instance());
  if(overlay) {
    findSymbol("B_SAY_OVERLAY.TEXT")->set_string(line);
    findSymbol("B_SAY_OVERLAY.OTH")->set_instance(other);
    findSymbol("B_SAY_OVERLAY.SLF")->set_instance(self);
    ai_outputsvm_overlay(self,other,line);
    }
  else {
    findSymbol("B_SAY.TEXT")->set_string(line);
    findSymbol("B_SAY.OTH")->set_instance(other);
    findSymbol("B_SAY.SLF")->set_instance(self);
    ai_outputsvm(self,other,line);
    }
  }

bool GameScript::runFreezeOrWhirlwindSpell(Npc& npc, const daedalus::DaedalusSymbol& symbol, int& result) {
  const std::string_view name = symbol.name();
  const bool freeze = name.starts_with("ZS_MAGICFREEZE");
  const auto self = std::static_pointer_cast<daedalus::INpc>(npc.handlePtr());
  constexpr size_t freezeSecond=53, whirlwindSecond=76; // AIV_FREEZESTATETIME and AIV_WHIRLWINDSTATETIME
  auto& lastSecond = npc.handle().aivar[freeze ? freezeSecond : whirlwindSecond];
  result = LOOP_CONTINUE;
  if(name=="ZS_MAGICFREEZE" || name=="ZS_WHIRLWIND") {
    npc.setPerceptionEnable(PERC_ASSESSMAGIC,findFunction(freeze ? "B_RESTARTFREEZE" : "B_RESTARTWHIRLWIND").ptr);
    npc.stopAnim(freeze ? "S_FIRE_VICTIM" : "S_WHIRLWIND_VICTIM");
    if(!freeze) {
      npc.clearAiQueue();
      npc.aiPush(AiQueue::aiStandup());
      }
    findSymbol("C_BODYSTATECONTAINS.BODYSTATE")->set_int(int32_t(BS_UNCONSCIOUS));
    findSymbol("C_BODYSTATECONTAINS.SLF")->set_instance(self);
    if(!inBodyState(npc,BS_UNCONSCIOUS)) {
      const auto pose = freeze ? "T_STAND_2_FREEZE_VICTIM" : "T_STAND_2_WHIRLWIND_VICTIM";
      npc.aiPush(AiQueue::aiPlayAnimBs(pose,BS_UNCONSCIOUS));
      }
    lastSecond = 0;
    }
  else if(name.ends_with("_LOOP")) {
    const auto seconds = npc_getstatetime(self);
    if(seconds>(freeze ? 19 : 10)) {
      stopInterruptedSpell(npc);
      result = LOOP_END;
      return true;
      }
    if(seconds==lastSecond)
      return true;
    lastSecond = seconds;

    int32_t damage = 0;
    if(freeze) {
      // The stock guard compares HP to HP minus the positive base damage.
      constexpr int32_t freezeDamage=2, fireDragon=38, fireLizard=12, iceDragon=39;
      const auto species = npc.handle().aivar[43]; // AIV_MM_REAL_ID
      const auto guild = npc.handle().guild;
      if(species==fireDragon || species==fireLizard || guild==GIL_FIREGOLEM)
        damage = freezeDamage*2;
      else if(species==iceDragon || guild==GIL_ICEGOLEM)
        damage = freezeDamage/2;
      else
        damage = freezeDamage;
      }
    else if(const auto hp = npc.attribute(ATR_HITPOINTS); hp<=0) {
      damage = hp-1;
      }
    // Whirlwind still invokes the damage helper when its damage is zero.
    magicHurtNpc(std::dynamic_pointer_cast<daedalus::INpc>(vm.global_other()->get_instance()),self,damage);
    }
  return true;
  }

void GameScript::stopInterruptedSpell(Npc& npc, bool resumeRoutine) {
  npc.setPerceptionEnable(PERC_ASSESSMAGIC,findFunction("B_ASSESSMAGIC").ptr);
  npc.clearAiQueue();
  npc.aiPush(AiQueue::aiStandup());
  if(npc.handle().guild<int32_t(GIL_SEPERATOR_HUM))
    vm.unsafe_call_bl(findSymbol("B_ASSESSDAMAGE"));
  else
    npc_settempattitude(std::static_pointer_cast<daedalus::INpc>(npc.handlePtr()),int32_t(ATT_HOSTILE));
  if(resumeRoutine)
    ai_continueroutine(std::dynamic_pointer_cast<daedalus::INpc>(vm.global_self()->get_instance()));
  }
