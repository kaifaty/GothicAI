#include "gamescript.h"

#include "world/objects/npc.h"

namespace {
constexpr std::string_view roam = "FP_ROAM";
constexpr std::string_view campfire = "FP_CAMPFIRE";
constexpr size_t routinePosition = 19;
constexpr std::string_view roamGestures[] = {"R_ROAM1","R_ROAM2","R_ROAM3"};
constexpr int32_t humanGuildSeparator = int32_t(GIL_SEPERATOR_HUM);
constexpr int32_t orcGuildSeparator = int32_t(GIL_SEPERATOR_ORC);

// Stock NotR AIVAR slots used by the combat states in the verified program.
enum MonsterCombatVar : size_t {
  invincible = 4,
  lastTarget = 7,
  pursuitEnd = 8,
  partyMember = 15,
  selectSpell = 20,
  hitByOtherNpc = 22,
  waitBeforeAttack = 23,
  threatenBeforeAttack = 26,
  followTime = 27,
  followInWater = 28,
  monsterPriority = 29,
  realId = 43,
  lastBody = 44,
  maxDistanceToWaypoint = 59,
  originalFightTactic = 60,
  enemyOverride = 61,
  summonTime = 62,
  loadGame = 65,
  stateTime = 68,
  pursuitDistance = 69,
  noFightParker = 71,
  };

void regenerateDragon(daedalus::INpc& actor) {
  if(actor.guild!=GIL_DRAGON)
    return;
  auto& position = actor.aivar[routinePosition];
  ++position;
  if(position>=2 && actor.attribute[ATR_HITPOINTS]<actor.attribute[ATR_HITPOINTSMAX]) {
    ++actor.attribute[ATR_HITPOINTS];
    position = 0;
    }
  }
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
      const auto choice = rollRandom(3,&npc,true);
      findSymbol("ZS_MM_RTN_REST_LOOP.RANDOMMOVE")->set_int(int32_t(choice));
      npc.aiPush(AiQueue::aiPlayAnim(roamGestures[choice]));
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

bool GameScript::monsterTargetHasBodyState(const std::shared_ptr<daedalus::INpc>& target, int32_t state) {
  findSymbol("C_BODYSTATECONTAINS.BODYSTATE")->set_int(state);
  findSymbol("C_BODYSTATECONTAINS.SLF")->set_instance(target);
  return ActivityDefinitions::bodyStateContains(npc_getbodystate(target),state);
  }

bool GameScript::runMonsterAttack(Npc& npc, const daedalus::DaedalusSymbol& symbol, int& result) {
  const auto self = std::static_pointer_cast<daedalus::INpc>(npc.handlePtr());
  auto& ai = self->aivar;
  auto& otherSymbol = *vm.global_other();
  const auto other = [&] { return std::dynamic_pointer_cast<daedalus::INpc>(otherSymbol.get_instance()); };
  const auto needsWeapon = [&] {
    // These script parameters are serialized as static instance locals.
    findSymbol("C_NPCISMONSTERMAGE.SLF")->set_instance(self);
    const auto guild = self->guild;
    return guild>orcGuildSeparator || guild==GIL_SUMMONED_SKELETON || guild==GIL_SKELETON ||
           guild==GIL_DRAGON || guild==GIL_ICEGOLEM || guild==GIL_FIREGOLEM || guild==GIL_SKELETON_MAGE;
    };

  const std::string_view phase = symbol.name();
  if(phase=="ZS_MM_ATTACK") {
    auto enable = [&](PercType perception, std::string_view callback) {
      npc.setPerceptionEnable(perception,findFunction(callback).ptr);
      };
    npc.setPerceptionTime(1000);
    enable(PERC_ASSESSBODY,"B_MM_ASSESSBODY");
    enable(PERC_ASSESSMAGIC,"B_ASSESSMAGIC");
    enable(PERC_ASSESSDAMAGE,"B_MM_ASSESSDAMAGE");
    enable(PERC_ASSESSWARN,"B_MM_ASSESSWARN");
    enable(PERC_ASSESSSURPRISE,"B_MM_ASSESSSURPRISE");

    validateCombatTarget(self);
    if(self->guild==GIL_SHEEP) {
      npc_clearaiqueue(self);
      clearPerceptions(self);
      npc_settarget(self,other());
      ai_startstate(self,int(findFunction("ZS_MM_FLEE").ptr),0,"");
      } else {
      ai_standup(self);
      ai_setwalkmode(self,int(WalkBit::WM_Run));
      npc_sendpassiveperc(self,PERC_ASSESSWARN,other(),self);
      ai[pursuitEnd] = 0;
      ai[stateTime] = 0;
      ai[hitByOtherNpc] = 0;
      ai[selectSpell] = 0;
      ai[routinePosition] = 0;
      }
    result = 0;
    return true;
    }

  if(phase=="ZS_MM_ATTACK_END") {
    otherSymbol.set_instance(hlp_getnpc(ai[lastTarget]));
    if(needsWeapon())
      ai_removeweapon(self);
    if(monsterWantsToEat(self,other()) && npc_isdead(other())) {
      npc_clearaiqueue(self);
      ai_startstate(self,int(findFunction("ZS_MM_EATBODY").ptr),0,"");
      }
    result = 0;
    return true;
    }

  if(phase!="ZS_MM_ATTACK_LOOP")
    return false;

  const auto targetIsRunningOrJumping = [&] {
    const auto target = other();
    const bool jumping = monsterTargetHasBodyState(target,BS_JUMP);
    const bool running = monsterTargetHasBodyState(target,BS_RUN);
    return running || jumping;
    };
  const auto distanceToTarget = [&] { return npc_getdisttonpc(self,other()); };
  const auto stop = [&] {
    npc_clearaiqueue(self);
    ai_standup(self);
    };
  const auto resetPursuitTime = [&] {
    npc_setstatetime(self,0);
    ai[stateTime] = 0;
    };

  npc_gettarget(self);
  regenerateDragon(*self);
  removeRavensStoneGuardians(npc);

  const bool atOldWorldRamp = findSymbol("CURRENTLEVEL")->get_int()==findSymbol("OLDWORLD_ZEN")->get_int() &&
                              npc_getdisttowp(self,"OC_RAMP_07")<=500;
  if(atOldWorldRamp || distanceToTarget()>findSymbol("FIGHT_DIST_CANCEL")->get_int()) {
    stop();
    if(atOldWorldRamp)
      ai_playani(self,"T_WARN");
    ai[pursuitEnd] = 1;
    result = LOOP_END;
    return true;
    }
  if(ai[pursuitEnd]==0 && npc_getstatetime(self)>ai[followTime]) {
    stop();
    ai[pursuitEnd] = 1;
    ai[pursuitDistance] = distanceToTarget();
    ai[stateTime] = npc_getstatetime(self);
    ai_playani(self,"T_WARN");
    }
  if(ai[pursuitEnd]==1) {
    if(distanceToTarget()>self->senses_range) {
      result = LOOP_END;
      return true;
      }
    if(npc_getstatetime(self)>ai[stateTime]) {
      if(!targetIsRunningOrJumping() || distanceToTarget()<ai[pursuitDistance]) {
        ai[pursuitEnd] = 0;
        resetPursuitTime();
        } else {
        ai_turntonpc(self,other());
        ai[pursuitDistance] = distanceToTarget();
        ai[stateTime] = npc_getstatetime(self);
        }
      }
    result = LOOP_CONTINUE;
    return true;
    }

  const bool diving = monsterTargetHasBodyState(other(),BS_DIVE);
  const bool swimming = monsterTargetHasBodyState(other(),BS_SWIM);
  if(ai[followInWater]==0 && (diving || swimming)) {
    stop();
    result = LOOP_END;
    return true;
    }
  if(ai[waitBeforeAttack]==1) {
    ai_wait(self,0.8f);
    ai[waitBeforeAttack] = 0;
    }
  if(self->level==0) {
    const auto time = npc_getstatetime(self);
    if(time>ai[stateTime]) {
      ++ai[summonTime];
      ai[stateTime] = time;
      }
    if(ai[summonTime]>=findSymbol("MONSTER_SUMMON_TIME")->get_int())
      npc_changeattribute(self,ATR_HITPOINTS,-self->attribute[ATR_HITPOINTSMAX]);
    }
  const auto seconds = npc_getstatetime(self);
  const bool targetMoving = targetIsRunningOrJumping();
  if(seconds>0 && !targetMoving)
    resetPursuitTime();

  if(ai[maxDistanceToWaypoint]>0) {
    const auto limit = ai[maxDistanceToWaypoint];
    const bool targetOutside = npc_getdisttowp(other(),self->wp)>limit;
    const bool attackerOutside = npc_getdisttowp(self,self->wp)>limit;
    self->fight_tactic = targetOutside && attackerOutside ? findSymbol("FAI_NAILED")->get_int() : ai[originalFightTactic];
    }
  if(needsWeapon()) {
    vm.call_function<void>("B_CREATEAMMO",self);
    npc_changeattribute(self,ATR_MANA,ATR_MANAMAX);
    vm.call_function<void>("B_SELECTWEAPON",self,other());
    }
  const auto target = other();
  if(!isNpcDownInScript(target) && hlp_isvalidnpc(target)) {
    if(target->aivar[invincible]==0)
      ai_attack(self);
    else
      npc_clearaiqueue(self);
    ai[lastTarget] = hlp_getinstanceid(target);
    result = LOOP_CONTINUE;
    return true;
    }

  if(monsterWantsToEat(self,other()) && ai[monsterPriority]==findSymbol("PRIO_EAT")->get_int()) {
    stop();
    result = LOOP_END;
    return true;
    }
  npc_perceiveall(self);
  npc_getnexttarget(self);
  const auto next = other();
  const bool closeOrPlayer = npc_isplayer(next) || distanceToTarget()<findSymbol("PERC_DIST_INTERMEDIAT")->get_int();
  const bool down = isNpcDownInScript(next);
  if(next!=nullptr && next->aivar[invincible]==0 && closeOrPlayer && !down && hlp_isvalidnpc(next)) {
    ai[lastTarget] = hlp_getinstanceid(next);
    result = LOOP_CONTINUE;
    return true;
    }
  stop();
  result = LOOP_END;
  return true;
  }

bool GameScript::runMonsterThreat(Npc& npc, const daedalus::DaedalusSymbol& symbol, int& result) {
  const auto phase = symbol.name();
  const auto self = std::static_pointer_cast<daedalus::INpc>(npc.handlePtr());
  const auto other = std::dynamic_pointer_cast<daedalus::INpc>(vm.global_other()->get_instance());
  if(phase=="ZS_MM_THREATENENEMY") {
    auto enable = [&](PercType perception, std::string_view callback) {
      npc.setPerceptionEnable(perception,findFunction(callback).ptr);
      };
    npc.setPerceptionTime(2000);
    enable(PERC_ASSESSBODY,"B_MM_ASSESSBODY");
    enable(PERC_ASSESSMAGIC,"B_ASSESSMAGIC");
    enable(PERC_ASSESSDAMAGE,"B_MM_ASSESSDAMAGE");
    enable(PERC_ASSESSOTHERSDAMAGE,"B_MM_ASSESSOTHERSDAMAGE");
    enable(PERC_ASSESSMURDER,"B_MM_ASSESSOTHERSDAMAGE");
    ai_standup(self);
    npc_sendpassiveperc(self,PERC_ASSESSWARN,other,self);
    result = 0;
    return true;
    }
  if(phase=="ZS_MM_THREATENENEMY_END") {
    result = 0;
    return true;
    }
  if(phase!="ZS_MM_THREATENENEMY_LOOP")
    return false;

  const auto distance = npc_getdisttonpc(self,other);
  if(distance>findSymbol("PERC_DIST_MONSTER_ACTIVE_MAX")->get_int()) {
    npc_clearaiqueue(self);
    result = LOOP_END;
    return true;
    }
  if(distance<=findSymbol("FIGHT_DIST_MONSTER_ATTACKRANGE")->get_int() ||
     npc_getstatetime(self)>=findSymbol("MONSTER_THREATEN_TIME")->get_int()) {
    npc_clearaiqueue(self);
    npc_settarget(self,other);
    ai_startstate(self,int(findFunction("ZS_MM_ATTACK").ptr),0,"");
    result = LOOP_END;
    return true;
    }
  ai_turntonpc(self,other);
  ai_playani(self,"T_WARN");
  result = LOOP_CONTINUE;
  return true;
  }

bool GameScript::runMonsterFlee(Npc& npc, const daedalus::DaedalusSymbol& symbol, int& result) {
  const auto phase = symbol.name();
  const auto self = std::static_pointer_cast<daedalus::INpc>(npc.handlePtr());
  const auto other = [&] { return std::dynamic_pointer_cast<daedalus::INpc>(vm.global_other()->get_instance()); };
  if(phase=="ZS_MM_FLEE") {
    npc.setPerceptionEnable(PERC_ASSESSMAGIC,findFunction("B_ASSESSMAGIC").ptr);
    validateCombatTarget(self);
    ai_standup(self);
    ai_setwalkmode(self,int(WalkBit::WM_Run));
    npc_sendpassiveperc(self,PERC_ASSESSWARN,other(),self);
    desynchronizeNpc();
    result = 0;
    return true;
    }
  if(phase=="ZS_MM_FLEE_END") {
    result = 0;
    return true;
    }
  if(phase!="ZS_MM_FLEE_LOOP")
    return false;

  npc_gettarget(self);
  if(npc_getdisttonpc(self,other())>=2000) {
    npc_clearaiqueue(self);
    result = LOOP_END;
    return true;
    }
  if(npc_getstatetime(self)>0) {
    npc_sendpassiveperc(self,PERC_ASSESSWARN,other(),self);
    npc_setstatetime(self,0);
    }
  ai_flee(self);
  result = LOOP_CONTINUE;
  return true;
  }

bool GameScript::runMonsterHunt(Npc& npc, const daedalus::DaedalusSymbol& symbol, int& result) {
  const auto phase = symbol.name();
  if(phase=="ZS_MM_HUNT") {
    const auto self = std::static_pointer_cast<daedalus::INpc>(npc.handlePtr());
    const auto other = std::dynamic_pointer_cast<daedalus::INpc>(vm.global_other()->get_instance());
    enableMonsterRoutinePerceptions(npc);
    ai_standup(self);
    ai_turntonpc(self,other);
    ai_setwalkmode(self,int(WalkBit::WM_Walk));
    ai_gotonpc(self,other);
    result = 0;
    return true;
    }
  if(phase=="ZS_MM_HUNT_LOOP") {
    result = LOOP_END;
    return true;
    }
  if(phase!="ZS_MM_HUNT_END")
    return false;
  result = 0;
  return true;
  }

bool GameScript::runMonsterEatBody(Npc& npc, const daedalus::DaedalusSymbol& symbol, int& result) {
  const auto phase = symbol.name();
  const auto self = std::static_pointer_cast<daedalus::INpc>(npc.handlePtr());
  const auto other = std::dynamic_pointer_cast<daedalus::INpc>(vm.global_other()->get_instance());
  auto& ai = self->aivar;
  if(phase=="ZS_MM_EATBODY") {
    npc.setPerceptionTime(1000);
    npc.setPerceptionEnable(PERC_ASSESSMAGIC,findFunction("B_ASSESSMAGIC").ptr);
    npc.setPerceptionEnable(PERC_ASSESSDAMAGE,findFunction("B_MM_ASSESSDAMAGE").ptr);
    const auto othersDamage = findFunction("B_MM_ASSESSOTHERSDAMAGE").ptr;
    npc.setPerceptionEnable(PERC_ASSESSOTHERSDAMAGE,othersDamage);
    npc.setPerceptionEnable(PERC_ASSESSMURDER,othersDamage);
    ai_gotonpc(self,other);
    ai_turntonpc(self,other);
    ai_playani(self,"T_STAND_2_EAT");
    ai[monsterPriority] = findSymbol("PRIO_EAT")->get_int();
    ai[lastBody] = hlp_getinstanceid(other);
    ai[routinePosition] = findSymbol("NOTINPOS")->get_int();
    result = 0;
    return true;
    }
  if(phase=="ZS_MM_EATBODY_LOOP") {
    if(ai[routinePosition]==findSymbol("NOTINPOS")->get_int()) {
      npc.setPerceptionEnable(PERC_ASSESSENEMY,findFunction("B_MM_ASSESSENEMY").ptr);
      ai[routinePosition] = findSymbol("ISINPOS")->get_int();
      }
    if(!hlp_isvalidnpc(other)) {
      npc_clearaiqueue(self);
      result = LOOP_END;
      } else {
      result = LOOP_CONTINUE;
      }
    return true;
    }
  if(phase!="ZS_MM_EATBODY_END")
    return false;
  ai_playani(self,"T_EAT_2_STAND");
  result = 0;
  return true;
  }

bool GameScript::runMonsterDragonRest(Npc& npc, const daedalus::DaedalusSymbol& symbol, int& result) {
  const auto phase = symbol.name();
  const auto self = std::static_pointer_cast<daedalus::INpc>(npc.handlePtr());
  if(phase=="ZS_MM_RTN_DRAGONREST") {
    npc.setPerceptionTime(1000);
    self->aivar[monsterPriority] = findSymbol("PRIO_EAT")->get_int();
    if(hlp_getinstanceid(self)!=hlp_getinstanceid(findSymbol("DRAGON_TESTMODELL")->get_instance())) {
      enableMonsterRoutinePerceptions(npc);
      npc.setPerceptionEnable(PERC_ASSESSPLAYER,findFunction("B_MM_ASSESSPLAYER").ptr);
      }
    npc.setPerceptionEnable(PERC_ASSESSTALK,findFunction("B_ASSESSTALK").ptr);
    beginMonsterRoutine(npc);
    if(wld_isfpavailable(self,roam))
      ai_gotofp(self,roam);
    else
      ai_aligntowp(self);
    self->aivar[routinePosition] = 0;
    result = 0;
    return true;
    }
  if(phase=="ZS_MM_RTN_DRAGONREST_LOOP") {
    if(endMonsterRoutine(npc,MonsterRest)) {
      result = LOOP_END;
      return true;
      }
    regenerateDragon(*self);
    if(rollRandom(1000,&npc,true)<=5) {
      const auto gesture = rollRandom(3,&npc,true);
      findSymbol("ZS_MM_RTN_DRAGONREST_LOOP.RANDOMMOVE")->set_int(int32_t(gesture));
      ai_standup(self);
      ai_playani(self,roamGestures[gesture]);
      }
    result = LOOP_CONTINUE;
    return true;
    }
  if(phase!="ZS_MM_RTN_DRAGONREST_END")
    return false;
  ai_playani(self,"T_REST_2_STAND");
  result = 0;
  return true;
  }

bool GameScript::runMonsterSheepFollow(Npc& npc, const daedalus::DaedalusSymbol& symbol, int& result) {
  const auto phase = symbol.name();
  if(phase=="ZS_MM_RTN_FOLLOW_SHEEP" || phase=="ZS_MM_RTN_FOLLOW_SHEEP_BALTHASAR") {
    npc.setPerceptionTime(1000);
    npc.setPerceptionEnable(PERC_ASSESSPLAYER,findFunction("B_MM_ASSESSPLAYER").ptr);
    if(phase=="ZS_MM_RTN_FOLLOW_SHEEP") {
      npc.setPerceptionEnable(PERC_ASSESSTALK,findFunction("B_ASSESSTALK").ptr);
      npc.setPerceptionEnable(PERC_ASSESSMAGIC,findFunction("B_ASSESSMAGIC").ptr);
      }
    result = 0;
    return true;
    }
  if(phase=="ZS_MM_RTN_FOLLOW_SHEEP_END" || phase=="ZS_MM_RTN_FOLLOW_SHEEP_BALTHASAR_END") {
    result = 0;
    return true;
    }

  constexpr std::string_view destination = "NW_BIGMILL_FARM3_BALTHASAR";
  const auto self = std::static_pointer_cast<daedalus::INpc>(npc.handlePtr());
  std::shared_ptr<daedalus::INpc> leader;
  if(phase=="ZS_MM_RTN_FOLLOW_SHEEP_LOOP") {
    if(self->aivar[partyMember]!=1) {
      const auto gesture = rollRandom(3,&npc,true);
      findSymbol("ZS_MM_RTN_FOLLOW_SHEEP_LOOP.RANDOMMOVE")->set_int(int32_t(gesture));
      ai_playani(self,roamGestures[gesture]);
      result = LOOP_CONTINUE;
      return true;
      }
    leader = std::dynamic_pointer_cast<daedalus::INpc>(vm.global_hero()->get_instance());
    }
  else if(phase=="ZS_MM_RTN_FOLLOW_SHEEP_BALTHASAR_LOOP") {
    leader = std::dynamic_pointer_cast<daedalus::INpc>(findSymbol("BALTHASAR")->get_instance());
    if(npc_getdisttowp(leader,destination)<=500) {
      ai_startstate(self,int(findFunction("ZS_MM_RTN_ROAM").ptr),1,destination);
      result = LOOP_CONTINUE;
      return true;
      }
    }
  else {
    return false;
    }
  if(npc_getdisttonpc(self,leader)>500) {
    if(!monsterTargetHasBodyState(self,BS_SWIM))
      ai_setwalkmode(self,int(WalkBit::WM_Run));
    ai_gotonpc(self,leader);
    }
  else {
    ai_turntonpc(self,leader);
    self->wp = npc_getnearestwp(self);
    }
  result = LOOP_CONTINUE;
  return true;
  }

bool GameScript::runMonsterSummoned(Npc& npc, const daedalus::DaedalusSymbol& symbol, int& result) {
  const auto phase = symbol.name();
  const auto self = std::static_pointer_cast<daedalus::INpc>(npc.handlePtr());
  const bool wisp = hlp_getinstanceid(self)==hlp_getinstanceid(findSymbol("WISP_DETECTOR")->get_instance());
  if(phase=="ZS_MM_RTN_SUMMONED") {
    npc.setPerceptionTime(1000);
    npc.setPerceptionEnable(PERC_ASSESSPLAYER,findFunction("B_MM_ASSESSPLAYER").ptr);
    npc.setPerceptionEnable(PERC_ASSESSENEMY,findFunction("B_MM_ASSESSENEMY").ptr);
    npc.setPerceptionEnable(PERC_ASSESSMAGIC,findFunction("B_ASSESSMAGIC").ptr);
    npc.setPerceptionEnable(PERC_ASSESSDAMAGE,findFunction("B_MM_ASSESSDAMAGE").ptr);
    npc.setPerceptionEnable(PERC_ASSESSFIGHTSOUND,findFunction("B_MM_ASSESSOTHERSDAMAGE").ptr);
    npc.setPerceptionEnable(PERC_ASSESSTALK,findFunction(wisp ? "B_ASSESSTALK" : "B_SUMMONEDASSESSTALK").ptr);
    findSymbol("B_SETATTITUDE.ATT")->set_int(ATT_FRIENDLY);
    findSymbol("B_SETATTITUDE.SLF")->set_instance(self);
    npc_setattitude(self,ATT_FRIENDLY);
    npc_settempattitude(self,ATT_FRIENDLY);
    self->aivar[partyMember] = 1;
    ai_standup(self);
    ai_setwalkmode(self,int(WalkBit::WM_Run));
    result = 0;
    return true;
    }
  if(phase=="ZS_MM_RTN_SUMMONED_END") {
    result = 0;
    return true;
    }
  if(phase!="ZS_MM_RTN_SUMMONED_LOOP")
    return false;

  // The summoned Wisp still uses its item detector and quest logic.
  if(wisp)
    vm.call_function("B_MM_WISPDETECT");
  const auto actor = std::static_pointer_cast<daedalus::INpc>(vm.global_self()->get_instance());
  const auto hero = std::dynamic_pointer_cast<daedalus::INpc>(vm.global_hero()->get_instance());
  if(npc_getdisttonpc(actor,hero)>500) {
    ai_gotonpc(actor,hero);
    }
  else if(const auto time = npc_getstatetime(actor); time>=1) {
    if(!npc_canseenpc(actor,hero))
      ai_turntonpc(actor,hero);
    auto& elapsed = actor->aivar[summonTime];
    elapsed += time;
    if(elapsed>=findSymbol("MONSTER_SUMMON_TIME")->get_int())
      npc_changeattribute(actor,ATR_HITPOINTS,-actor->attribute[ATR_HITPOINTSMAX]);
    npc_setstatetime(actor,0);
    }
  actor->wp = npc_getnearestwp(actor);
  result = LOOP_CONTINUE;
  return true;
  }

bool GameScript::nativeMonsterPerceptionsEnabled() {
  return nativeRoutineProgram && activityEvents && !floorDefer.active() &&
         ActivityDefinitionRuntime::mode()!=ActivityDefinitionRuntime::Mode::Off &&
         vm.global_self()->is_instance_of<daedalus::INpc>() && vm.global_other()->is_instance_of<daedalus::INpc>();
  }

bool GameScript::invokeNativeMonsterPerception(const daedalus::DaedalusSymbol& symbol, NpcActivityProbe::Phase& probe) {
  const auto name = symbol.name();
  if((name!="B_MM_ASSESSENEMY" && name!="B_MM_ASSESSBODY") || !nativeMonsterPerceptionsEnabled())
    return false;
  probe.fromRecord();
  if(name=="B_MM_ASSESSENEMY")
    runMonsterAssessEnemy();
  else
    runMonsterAssessBody();
  return true;
  }

bool GameScript::monsterWantsToEat(const std::shared_ptr<daedalus::INpc>& monster, const std::shared_ptr<daedalus::INpc>& body) {
  // An old save can restore a last target that is absent from this world.
  if(body==nullptr)
    return vm.call_function<int32_t>("C_WANTTOEAT",monster,body)!=0;

  findSymbol("C_WANTTOEAT.SLF")->set_instance(monster);
  findSymbol("C_WANTTOEAT.OTH")->set_instance(body);
  if(monster->aivar[partyMember]==1)
    return false;

  const auto guild = body->guild;
  const bool humanoid = guild<humanGuildSeparator || guild>orcGuildSeparator;
  const bool smallAnimal = guild==GIL_SHEEP || guild==GIL_GIANT_RAT || guild==GIL_MOLERAT;
  const bool commonPrey = smallAnimal || guild==GIL_HARPY || guild==GIL_WOLF || guild==GIL_SUMMONED_WOLF ||
                         guild==GIL_GOBBO || guild==GIL_WARAN || guild==GIL_LURKER;
  const bool insect = guild==GIL_MEATBUG || guild==GIL_GIANT_BUG || guild==GIL_BLOODFLY;
  switch(monster->guild) {
    case GIL_SCAVENGER:
    case GIL_GIANT_RAT:
      return humanoid || commonPrey || insect || guild==GIL_MINECRAWLER || guild==GIL_SHADOWBEAST || guild==GIL_SNAPPER ||
             (monster->guild==GIL_GIANT_RAT && guild==GIL_SCAVENGER);
    case GIL_GIANT_BUG:
      return guild==GIL_MEATBUG || guild==GIL_BLOODFLY || guild==GIL_MINECRAWLER;
    case GIL_WARAN:
      return guild==GIL_SCAVENGER || guild==GIL_HARPY || guild==GIL_GOBBO;
    case GIL_WOLF:
      return monster->aivar[realId]!=findSymbol("ID_KEILER")->get_int() && (guild<humanGuildSeparator || smallAnimal);
    case GIL_SUMMONED_WOLF:
      return smallAnimal;
    case GIL_MINECRAWLER:
      return humanoid || insect;
    case GIL_LURKER:
      return humanoid || guild==GIL_GOBBO;
    case GIL_ZOMBIE:
      return humanoid;
    case GIL_SNAPPER:
      return humanoid || commonPrey || guild==GIL_SCAVENGER || guild==GIL_SHADOWBEAST;
    case GIL_SHADOWBEAST:
      return humanoid || commonPrey || guild==GIL_SCAVENGER || guild==GIL_SNAPPER;
    default:
      return false;
    }
  }

void GameScript::runMonsterAssessBody() {
  const auto self = std::static_pointer_cast<daedalus::INpc>(vm.global_self()->get_instance());
  const auto other = std::static_pointer_cast<daedalus::INpc>(vm.global_other()->get_instance());
  if(self->guild>orcGuildSeparator || npc_getheighttonpc(self,other)>findSymbol("PERC_DIST_HEIGHT")->get_int() ||
     self->aivar[monsterPriority]==findSymbol("PRIO_ATTACK")->get_int() || !monsterWantsToEat(self,other))
    return;
  if(npc_isinstate(self,int(findFunction("ZS_MM_ATTACK").ptr))) {
    const auto attacker = hlp_getnpc(self->aivar[lastTarget]);
    findSymbol("B_MM_ASSESSBODY.STOERENFRIED")->set_instance(attacker);
    if(npc_getdisttonpc(attacker,other)<=findSymbol("FIGHT_DIST_MONSTER_ATTACKRANGE")->get_int())
      return;
    }
  npc_clearaiqueue(self);
  clearPerceptions(self);
  ai_startstate(self,int(findFunction("ZS_MM_EATBODY").ptr),0,"");
  }

bool GameScript::monsterPredatorFoundPrey(const std::shared_ptr<daedalus::INpc>& predator, const std::shared_ptr<daedalus::INpc>& prey) {
  // Keep the serialized script locals in the same state as the original call.
  findSymbol("C_PREDATORFOUNDPREY.SLF")->set_instance(predator);
  findSymbol("C_PREDATORFOUNDPREY.OTH")->set_instance(prey);
  const auto guild = prey->guild;
  const bool small = guild==GIL_SHEEP || guild==GIL_GIANT_RAT || guild==GIL_MOLERAT ||
                     (guild==GIL_SCAVENGER && prey->aivar[realId]!=findSymbol("ID_ORCBITER")->get_int());
  switch(predator->guild) {
    case GIL_WOLF:
      return predator->aivar[realId]!=findSymbol("ID_KEILER")->get_int() && small;
    case GIL_LURKER:
      return guild==GIL_GOBBO;
    case GIL_SNAPPER:
      return guild<humanGuildSeparator || guild==GIL_GOBBO || small;
    case GIL_SHADOWBEAST:
      return guild<humanGuildSeparator || guild>orcGuildSeparator || small;
    default:
      return false;
    }
  }

void GameScript::runMonsterAssessEnemy() {
  const auto self = std::static_pointer_cast<daedalus::INpc>(vm.global_self()->get_instance());
  const auto other = std::static_pointer_cast<daedalus::INpc>(vm.global_other()->get_instance());
  const auto hero = std::dynamic_pointer_cast<daedalus::INpc>(vm.global_hero()->get_instance());
  const auto item = int(findSymbolIndex("ITMI_INNOSEYE_MIS"));
  if(npc_hasitems(hero,item)>=1 && self->guild==GIL_DRAGON)
    return;
  if(self->aivar[noFightParker]==1 || other->aivar[noFightParker]==1)
    return;
  if(self->aivar[enemyOverride]==1 && other->guild<humanGuildSeparator)
    return;

  const auto levelInspector = hlp_getnpc(int(findSymbolIndex("PC_LEVELINSPEKTOR")));
  findSymbol("B_MM_ASSESSENEMY.PCL")->set_instance(levelInspector);
  if(hlp_getinstanceid(other)==hlp_getinstanceid(levelInspector))
    return;
  const auto magicGolem = hlp_getnpc(int(findSymbolIndex("MAGICGOLEM")));
  const auto lares = hlp_getnpc(int(findSymbolIndex("VLK_449_LARES")));
  findSymbol("B_MM_ASSESSENEMY.MGO")->set_instance(magicGolem);
  findSymbol("B_MM_ASSESSENEMY.LAR")->set_instance(lares);
  if(hlp_getinstanceid(self)==hlp_getinstanceid(magicGolem) && hlp_getinstanceid(other)==hlp_getinstanceid(lares))
    return;
  if(npc_getdisttowp(self,"OC_RAMP_07")<=500 || other->aivar[invincible]==1)
    return;

  const bool diving = monsterTargetHasBodyState(other,BS_DIVE);
  const bool swimming = monsterTargetHasBodyState(other,BS_SWIM);
  if(self->aivar[followInWater]==0 && (diving || swimming))
    return;
  if(npc_getheighttonpc(self,other)>findSymbol("PERC_DIST_HEIGHT")->get_int())
    return;
  if(other->guild>humanGuildSeparator && npc_isplayer(other) && wld_getguildattitude(self->guild,other->guild)!=ATT_HOSTILE)
    return;

  const auto distance = npc_getdisttonpc(self,other);
  const auto activeRange = findSymbol("PERC_DIST_MONSTER_ACTIVE_MAX")->get_int();
  if(self->senses_range>activeRange && distance>activeRange && !npc_canseenpc(self,other))
    return;
  if(self->guild==GIL_ORC || self->guild==GIL_FRIENDLY_ORC) {
    const bool standing = monsterTargetHasBodyState(other,BS_STAND);
    const bool sneaking = monsterTargetHasBodyState(other,BS_SNEAK);
    if((standing || sneaking) && !npc_canseenpc(self,other))
      return;
    }
  if(!npc_canseenpcfreelos(self,other))
    return;

  const auto start = [&](std::string_view state, bool setTarget = true) {
    npc_clearaiqueue(self);
    if(setTarget)
      npc_settarget(self,other);
    clearPerceptions(self);
    ai_startstate(self,int(findFunction(state).ptr),0,"");
    };
  if(self->aivar[partyMember]==1) {
    if(int32_t(other->type)!=findSymbol("NPCTYPE_FRIEND")->get_int())
      start("ZS_MM_ATTACK");
    return;
    }
  if(self->aivar[threatenBeforeAttack]==0) {
    start("ZS_MM_ATTACK");
    return;
    }
  const auto hunt = findFunction("ZS_MM_HUNT");
  if(npc_isinstate(self,int(findFunction("ZS_MM_EATBODY").ptr)) || npc_isinstate(self,int(hunt.ptr))) {
    if(distance<=findSymbol("FIGHT_DIST_MONSTER_ATTACKRANGE")->get_int())
      start("ZS_MM_ATTACK");
    return;
    }
  if(monsterPredatorFoundPrey(self,other)) {
    start("ZS_MM_HUNT");
    return;
    }
  if(monsterPredatorFoundPrey(other,self)) {
    if(distance<=findSymbol("FIGHT_DIST_MONSTER_FLEE")->get_int() && !npc_isinstate(other,int(hunt.ptr)))
      start("ZS_MM_FLEE");
    return;
    }
  start("ZS_MM_THREATENENEMY",false);
  }
