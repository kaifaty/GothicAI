#include "gamescript.h"

#include <algorithm>

#include "world/objects/item.h"
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
  startedTalk = 3,
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
  packHunter = 50,
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
constexpr size_t shrinkStage = 42; // AIV_MM_SHRINKSTATE
constexpr int32_t shrinkSpell = 46; // SPL_SHRINK
constexpr int32_t shrinkScrollCost = 5;
constexpr int32_t shrinkRuneCost = 300;
}

bool GameScript::runMonsterRoutine(Npc& npc, const daedalus::DaedalusSymbol& symbol, int& result) {
  const std::string_view name = symbol.name();
  const auto self = std::static_pointer_cast<daedalus::INpc>(npc.handlePtr());
  auto& position = npc.handle().aivar[routinePosition];

  if(name=="ZS_PAL_ZOMBIE") {
    npc.handle().senses = int32_t(SensesBit::SENSE_SMELL);
    npc.handle().senses_range = 2000;
    npc.setPerceptionTime(1000);
    npc.setPerceptionEnable(PERC_ASSESSPLAYER,findFunction("B_PAL_ZOMBIE_RISE").ptr);
    position = 1;
    result = 0;
    return true;
    }
  if(name=="ZS_PAL_ZOMBIE_LOOP") {
    if(position==1) {
      npc.aiPush(AiQueue::aiPlayAnim("T_DOWN"));
      position = 0;
      }
    result = LOOP_CONTINUE;
    return true;
    }
  if(name=="ZS_PAL_ZOMBIE_END") {
    result = 0;
    return true;
    }

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
    const bool jumping = npcBodyStateContains(target,BS_JUMP);
    const bool running = npcBodyStateContains(target,BS_RUN);
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

  const bool diving = npcBodyStateContains(other(),BS_DIVE);
  const bool swimming = npcBodyStateContains(other(),BS_SWIM);
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
    if(!npcBodyStateContains(self,BS_SWIM))
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
  static constexpr std::pair<std::string_view,void (GameScript::*)()> callbacks[] = {
    {"B_MM_ASSESSENEMY",        &GameScript::runMonsterAssessEnemy},
    {"B_MM_ASSESSBODY",         &GameScript::runMonsterAssessBody},
    {"B_MM_ASSESSDAMAGE",       &GameScript::runMonsterAssessDamage},
    {"B_MM_ASSESSOTHERSDAMAGE", &GameScript::runMonsterAssessOthersDamage},
    {"B_MM_ASSESSPLAYER",       &GameScript::runMonsterAssessPlayer},
    {"B_MM_ASSESSWARN",         &GameScript::runMonsterAssessWarn},
    {"B_MM_ASSESSSURPRISE",     &GameScript::runMonsterAssessSurprise},
    {"B_MM_ASSESSQUIETSOUND_SLEEP", &GameScript::runMonsterAssessQuietSoundSleep},
    {"B_SUMMONEDASSESSTALK",        &GameScript::runMonsterSummonedAssessTalk},
    };
  for(const auto& [name,callback] : callbacks) {
    if(symbol.name()!=name)
      continue;
    if(!nativeMonsterPerceptionsEnabled())
      return false;
    probe.fromRecord();
    (this->*callback)();
    return true;
    }
  return false;
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

void GameScript::runMonsterAssessDamage() {
  const auto self = std::static_pointer_cast<daedalus::INpc>(vm.global_self()->get_instance());
  const auto other = [&] { return std::static_pointer_cast<daedalus::INpc>(vm.global_other()->get_instance()); };
  auto& ai = self->aivar;
  ai[monsterPriority] = findSymbol("PRIO_ATTACK")->get_int();
  vm.call_function<void>("B_BELIARSWEAPONSPECIALDAMAGE",other(),self);

  const auto hammer = int(findSymbolIndex("HOLY_HAMMER_MIS"));
  if(npc_hasitems(other(),hammer)>0) {
    const auto golem = hlp_getnpc(int(findSymbolIndex("MAGICGOLEM")));
    findSymbol("B_MM_ASSESSDAMAGE.MAGGOL")->set_instance(golem);
    npc_getinvitem(other(),hammer);
    const auto weapon = npc_getreadiedweapon(other());
    findSymbol("B_MM_ASSESSDAMAGE.OTHWEAP")->set_instance(weapon);
    if(hlp_getinstanceid(vm.global_item()->get_instance())==hlp_getinstanceid(weapon) &&
       hlp_getinstanceid(self)==hlp_getinstanceid(golem)) {
      npc_changeattribute(self,ATR_HITPOINTS,-1000);
      return;
      }
    }
  if(ai[enemyOverride]==1 && self->guild==GIL_Stoneguardian)
    vm.call_function<void>("B_AWAKE_STONEGUARDIAN",self);

  const auto source = other();
  const auto attack = int(findFunction("ZS_MM_ATTACK").ptr);
  const bool predator = monsterPredatorFoundPrey(source,self);
  if(!predator && npc_isinstate(self,attack)) {
    if((ai[partyMember]==1 && npc_isplayer(source)) ||
       (source->aivar[realId]==findSymbol("ID_SKELETON_MAGE")->get_int() && ai[realId]==findSymbol("ID_SKELETON")->get_int()))
      return;
    const auto hit = hlp_getinstanceid(source);
    if(hit!=ai[lastTarget]) {
      if(hit==ai[hitByOtherNpc])
        npc_settarget(self,source);
      else
        ai[hitByOtherNpc] = hit;
      }
    return;
    }
  npc_clearaiqueue(self);
  npc_settarget(self,source);
  clearPerceptions(self);
  ai_startstate(self,predator ? int(findFunction("ZS_MM_FLEE").ptr) : attack,0,"");
  }

void GameScript::runMonsterAssessOthersDamage() {
  const auto self = std::static_pointer_cast<daedalus::INpc>(vm.global_self()->get_instance());
  const auto other = std::static_pointer_cast<daedalus::INpc>(vm.global_other()->get_instance());
  const auto victim = std::dynamic_pointer_cast<daedalus::INpc>(vm.global_victim()->get_instance());
  const auto range = findSymbol("PERC_DIST_INTERMEDIAT")->get_int();
  if((npc_getdisttonpc(self,other)>range && npc_getdisttonpc(self,victim)>range) || !npc_canseenpcfreelos(self,victim))
    return;

  const auto attack = [&](const std::shared_ptr<daedalus::INpc>& target) {
    npc_clearaiqueue(self);
    clearPerceptions(self);
    npc_settarget(self,target);
    ai_startstate(self,int(findFunction("ZS_MM_ATTACK").ptr),0,"");
    };
  if(self->aivar[partyMember]==1) {
    if(npc_isplayer(victim)) {
      attack(other);
      return;
      }
    if(!npc_isdead(victim) && npc_isplayer(other)) {
      attack(victim);
      return;
      }
    }
  if(self->guild==GIL_WOLF && npc_isdead(victim) && npc_isplayer(other) && other->guild==GIL_WOLF && victim->guild==GIL_WOLF) {
    npc_clearaiqueue(self);
    clearPerceptions(self);
    const auto summoned = int(findFunction("ZS_MM_RTN_SUMMONED").ptr);
    self->start_aistate = summoned;
    ai_startstate(self,summoned,0,"");
    return;
    }
  if(self->aivar[enemyOverride]==1 && self->guild==GIL_Stoneguardian && victim->guild==GIL_Stoneguardian)
    vm.call_function<void>("B_AWAKE_STONEGUARDIAN",self);

  const bool friendlyToSource = wld_getguildattitude(self->guild,other->guild)==ATT_FRIENDLY;
  const bool friendlyToVictim = wld_getguildattitude(self->guild,victim->guild)==ATT_FRIENDLY;
  if(!friendlyToSource && friendlyToVictim)
    attack(other);
  else if(!npc_isdead(victim) && !friendlyToVictim && friendlyToSource)
    attack(victim);
  }

void GameScript::runMonsterAssessPlayer() {
  const auto self = std::static_pointer_cast<daedalus::INpc>(vm.global_self()->get_instance());
  const auto other = [&] { return std::static_pointer_cast<daedalus::INpc>(vm.global_other()->get_instance()); };
  if(other()->aivar[invincible]==1 || isNpcDownInScript(other()))
    return;

  vm.push_instance(self);
  vm.unsafe_call_bl(findSymbol("B_ASSIGNDRAGONTALK"));
  const bool importantInfo = npc_checkinfo(self,1);
  const auto distance = npc_getdisttonpc(self,other());
  if(importantInfo && distance<=700 && (self->guild==GIL_DRAGON || distance<=findSymbol("PERC_DIST_DIALOG")->get_int())) {
    const bool diving = npcBodyStateContains(other(),BS_DIVE);
    const bool swimming = npcBodyStateContains(other(),BS_SWIM);
    const bool falling = npcBodyStateContains(other(),BS_FALL);
    if(!diving && !swimming && !falling) {
      self->aivar[startedTalk] = 1;
      vm.unsafe_call_bl(findSymbol("B_ASSESSTALK"));
      return;
      }
    }
  if(self->aivar[partyMember]==1) {
    const bool standing = npcBodyStateContains(self,BS_STAND);
    const auto hero = std::dynamic_pointer_cast<daedalus::INpc>(vm.global_hero()->get_instance());
    if(!standing && npc_getdisttonpc(self,hero)<500) {
      npc_clearaiqueue(self);
      ai_standup(self);
      ai_turntonpc(self,hero);
      }
    }
  }

void GameScript::runMonsterAssessWarn() {
  const auto self = std::static_pointer_cast<daedalus::INpc>(vm.global_self()->get_instance());
  auto& otherSymbol = *vm.global_other();
  const auto other = std::static_pointer_cast<daedalus::INpc>(otherSymbol.get_instance());
  const auto& ai = self->aivar;
  const auto victim = std::dynamic_pointer_cast<daedalus::INpc>(vm.global_victim()->get_instance());
  const auto attackState = int(findFunction("ZS_MM_ATTACK").ptr);
  const auto attackVictim = [&] {
    npc_clearaiqueue(self);
    npc_settarget(self,victim);
    clearPerceptions(self);
    ai_startstate(self,attackState,0,"");
    };
  if(self->guild>orcGuildSeparator) {
    if(!npc_isinstate(self,attackState) && wld_getguildattitude(self->guild,other->guild)==ATT_FRIENDLY)
      attackVictim();
    return;
    }
  if(other->guild<humanGuildSeparator || npc_getheighttonpc(self,other)>findSymbol("PERC_DIST_HEIGHT")->get_int())
    return;
  if(npc_isinstate(self,attackState)) {
    const bool predator = monsterPredatorFoundPrey(self,other);
    if(ai[monsterPriority]==findSymbol("PRIO_EAT")->get_int() && predator) {
      npc_clearaiqueue(self);
      npc_settarget(self,other);
      }
    else {
      otherSymbol.set_instance(hlp_getnpc(ai[lastTarget]));
      }
    return;
    }
  if(ai[packHunter]!=1 || self->guild!=other->guild || (npc_isplayer(victim) && victim->guild==GIL_WOLF && self->guild==GIL_WOLF))
    return;
  if(npc_isinstate(other,attackState)) {
    attackVictim();
    }
  else if(npc_isinstate(other,int(findFunction("ZS_MM_THREATENENEMY").ptr))) {
    ai_setwalkmode(self,int(WalkBit::WM_Run));
    ai_gotonpc(self,victim);
    }
  }

void GameScript::runMonsterAssessSurprise() {
  npc_settarget(std::static_pointer_cast<daedalus::INpc>(vm.global_self()->get_instance()),
                std::static_pointer_cast<daedalus::INpc>(vm.global_other()->get_instance()));
  }

void GameScript::runMonsterAssessQuietSoundSleep() {
  const auto self = std::static_pointer_cast<daedalus::INpc>(vm.global_self()->get_instance());
  const auto other = std::static_pointer_cast<daedalus::INpc>(vm.global_other()->get_instance());
  if(wld_getguildattitude(self->guild,other->guild)==ATT_HOSTILE)
    runMonsterAssessEnemy();
  }

void GameScript::runMonsterSummonedAssessTalk() {
  const auto self = std::static_pointer_cast<daedalus::INpc>(vm.global_self()->get_instance());
  npc_changeattribute(self,ATR_HITPOINTS,-self->attribute[ATR_HITPOINTSMAX]);
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

  const bool diving = npcBodyStateContains(other,BS_DIVE);
  const bool swimming = npcBodyStateContains(other,BS_SWIM);
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
    const bool standing = npcBodyStateContains(other,BS_STAND);
    const bool sneaking = npcBodyStateContains(other,BS_SNEAK);
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

bool GameScript::runDormantGolem(Npc& npc, const daedalus::DaedalusSymbol& symbol, int& result) {
  const std::string_view name = symbol.name();
  auto& body = npc.handle();
  auto& position = body.aivar[routinePosition];
  result = LOOP_CONTINUE;

  if(name=="ZS_GOLEMDOWN") {
    body.senses = int32_t(SensesBit::SENSE_SMELL);
    body.senses_range = 2000;
    npc.setPerceptionTime(1000);
    npc.setPerceptionEnable(PERC_ASSESSPLAYER,findFunction("B_GOLEMRISE").ptr);
    position = 1;
    }
  else if(name=="ZS_GOLEMDOWN_LOOP" && position==1) {
    npc.aiPush(AiQueue::aiPlayAnim("T_DEAD"));
    position = 0;
    }
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
  auto& stage=record.aivar[shrinkStage];
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

bool GameScript::usesNativeShrinkSpell(const Item* item) const {
  return item!=nullptr && item->isSpellOrRune() && item->spellId()==shrinkSpell &&
         nativeRoutineProgram && activityEvents && !floorDefer.active() &&
         ActivityDefinitionRuntime::mode()!=ActivityDefinitionRuntime::Mode::Off;
  }

int GameScript::runShrinkMana(Npc& caster, int mana, bool release) {
  const auto self=std::static_pointer_cast<daedalus::INpc>(caster.handlePtr());
  if(release) {
    findSymbol("SPELL_PROCESSMANA_RELEASE.MANAINVESTED")->set_int(mana);
    findSymbol("SPELL_PROCESSMANA_RELEASE.ACTIVESPELL")->set_int(npc_getactivespell(self));
    return SPL_SENDSTOP;
    }
  findSymbol("SPELL_PROCESSMANA.MANAINVESTED")->set_int(mana);
  findSymbol("SPELL_PROCESSMANA.ACTIVESPELL")->set_int(npc_getactivespell(self));
  findSymbol("SPELL_LOGIC_SHRINK.MANAINVESTED")->set_int(mana);
  const auto available=self->attribute[ATR_MANA];
  const auto scroll=npc_getactivespellisscroll(self);
  return (scroll && available>=shrinkScrollCost) || available>=shrinkRuneCost ? SPL_SENDCAST : SPL_SENDSTOP;
  }

void GameScript::castShrink(Npc& caster, Npc* target) {
  const auto self=std::static_pointer_cast<daedalus::INpc>(caster.handlePtr());
  self->attribute[ATR_MANA]-=npc_getactivespellisscroll(self) ? shrinkScrollCost : shrinkRuneCost;

  const auto victim=target ? std::static_pointer_cast<daedalus::INpc>(target->handlePtr()) : nullptr;
  // The stock predicate evaluates the undead check even for ineligible targets.
  findSymbol("C_NPCISUNDEAD.SLF")->set_instance(victim);
  if(victim) {
    bool undead=victim->aivar[43]==42; // AIV_MM_REAL_ID == ID_DRAGON_UNDEAD
    switch(victim->guild) {
      case GIL_UNDEADORC:
      case GIL_SHADOWBEAST_SKELETON:
      case GIL_ZOMBIE:
      case GIL_SKELETON_MAGE:
      case GIL_SUMMONED_SKELETON:
      case GIL_SKELETON:
      case GIL_SUMMONED_GOBBO_SKELETON:
      case GIL_GOBBO_SKELETON:
        undead=true;
        break;
      }
    if(victim->aivar[shrinkStage]==0 && victim->guild>int32_t(GIL_SEPERATOR_HUM) &&
       !undead && victim->flags!=NpcFlag::IMMORTAL) {
      npc_clearaiqueue(victim);
      clearPerceptions(victim);
      ai_startstate(victim,int(findFunction("ZS_MAGICSHRINK").ptr),0,"");
      }
    }
  ++self->aivar[20]; // AIV_SELECTSPELL
  }

bool GameScript::runMagicHurtState(Npc& npc, const daedalus::DaedalusSymbol& symbol, int& result) {
  static constexpr struct State {
    std::string_view name, stop, restart;
    int32_t damage;
    } states[]={
    {"ZS_MAGICBURN","B_STOPMAGICBURN","B_RESTARTBURN",1},
    {"ZS_MAGICBURNSHORT","B_STOPMAGICBURNSHORT","B_RESTARTBURNSHORT",1},
    {"ZS_PYRO","B_STOPPYRO","",20},
    {"ZS_ZAPPED","B_STOPZAPPED","",2}
    };
  const std::string_view name=symbol.name();
  const bool loop=name.ends_with("_LOOP"), end=name.ends_with("_END");
  const auto stateName=name.substr(0,name.size()-(loop ? 5 : end ? 4 : 0));
  const auto state=std::ranges::find(states,stateName,&State::name);
  if(state==std::end(states))
    return false;

  const auto self=std::static_pointer_cast<daedalus::INpc>(npc.handlePtr());
  const bool shortBurn=stateName=="ZS_MAGICBURNSHORT";
  const bool burns=!state->restart.empty();
  const auto stand=[&] {
    npc_clearaiqueue(self);
    ai_standup(self);
    };
  result=LOOP_CONTINUE;
  if(end) {
    if(shortBurn)
      stand();
    return true;
    }

  const bool human=self->guild<int32_t(GIL_SEPERATOR_HUM);
  if(!loop) {
    npc_percenable(self,PERC_ASSESSSTOPMAGIC,int(findFunction(state->stop).ptr));
    if(npc_hasbodyflag(self,BS_FLAG_INTERRUPTABLE))
      ai_standupquick(self);
    else
      ai_standup(self);
    if(shortBurn && human)
      npc_playani(self,"S_FIRE_VICTIM");
    else if(!burns && human)
      ai_playani(self,"T_STAND_2_LIGHTNING_VICTIM");
    return true;
    }

  if(burns)
    npc_percenable(self,PERC_ASSESSMAGIC,int(findFunction(state->restart).ptr));
  const auto seconds=npc_getstatetime(self);
  const bool damageDue=burns ? seconds==1 : seconds>=1;
  if(damageDue) {
    npc_setstatetime(self,0);
    vm.push_instance(vm.global_other()->get_instance());
    vm.push_instance(self);
    vm.push_int(state->damage);
    vm.unsafe_call_bl(findSymbol("B_MAGICHURTNPC"));
    } else if(shortBurn && human) {
    npc_playani(self,"S_FIRE_VICTIM");
    }

  if((shortBurn && damageDue) || ((burns || damageDue) && self->attribute[ATR_HITPOINTS]<=0)) {
    stand();
    result=LOOP_END;
    }
  return true;
  }

void GameScript::wakeDormantGolem(Npc& npc) {
  const auto self = std::static_pointer_cast<daedalus::INpc>(npc.handlePtr());
  const auto hero = std::dynamic_pointer_cast<daedalus::INpc>(vm.global_hero()->get_instance());
  const auto remains = mob_hasitems("NW_GOLEMCHEST",int(findSymbolIndex("ITSE_GOLEMCHEST_MIS")));
  const auto distance = npc_getdisttonpc(self,hero);
  if(remains!=0 || distance>700)
    return;

  npc.aiPush(AiQueue::aiPlayAnim("T_RISE"));
  auto& body = npc.handle();
  body.no_focus = 0;
  body.name[0] = findSymbol("$27152")->get_string(); // Localized name from the stock program.
  body.flags = NpcFlag::NONE;
  ai_startstate(self,int(findFunction("ZS_MM_ATTACK").ptr),0,"");
  body.bodystate_interruptable_override = 0;
  body.start_aistate = int32_t(findFunction(monsterScheduler).ptr);
  body.aivar[32] = -1; // AIV_MM_RESTSTART = ONLYROUTINE
  }

void GameScript::magicHurtNpc(const std::shared_ptr<daedalus::INpc>& attacker,
                             const std::shared_ptr<daedalus::INpc>& target,int32_t damage) {
  auto* oth=findSymbol("B_MAGICHURTNPC.OTH");
  auto* slf=findSymbol("B_MAGICHURTNPC.SLF");
  findSymbol("B_MAGICHURTNPC.DAMAGE")->set_int(damage);
  oth->set_instance(target);
  slf->set_instance(attacker);

  // Use the original engine damage callback and its startup placement barrier.
  vm.push_instance(target);
  vm.push_int(ATR_HITPOINTS);
  vm.push_int(-damage);
  vm.unsafe_call_external(findSymbol("NPC_CHANGEATTRIBUTE"));
  const auto victim=std::static_pointer_cast<daedalus::INpc>(oth->get_instance());
  vm.push_instance(victim);
  vm.unsafe_call_external(findSymbol("NPC_ISDEAD"));
  if(vm.pop_int()==0)
    return;

  const bool uncredited=!victim || victim->aivar[16]==0; // AIV_VictoryXPGiven
  const auto source=std::static_pointer_cast<daedalus::INpc>(slf->get_instance());
  const bool partyMember=source && source->aivar[15]==1; // AIV_PARTYMEMBER
  vm.unsafe_set_gi(source);
  vm.push_instance(source);
  vm.unsafe_call_external(findSymbol("NPC_ISPLAYER"));
  const bool player=vm.pop_int()!=0;
  if(uncredited && (player || partyMember)) {
    // The stock XP rule reads SELF rather than the damage helper's target parameter.
    const auto self=std::dynamic_pointer_cast<daedalus::INpc>(vm.global_self()->get_instance());
    vm.unsafe_set_gi(self);
    vm.push_int(self ? self->level*10 : 0);
    vm.unsafe_call_bl(findSymbol("B_GIVEPLAYERXP"));
    const auto credited=std::static_pointer_cast<daedalus::INpc>(oth->get_instance());
    if(credited)
      credited->aivar[16]=1;
    }
  }

void GameScript::raisePaladinZombie(Npc& npc) {
  const auto self = std::static_pointer_cast<daedalus::INpc>(npc.handlePtr());
  const auto hero = std::dynamic_pointer_cast<daedalus::INpc>(vm.global_hero()->get_instance());
  if(npc_getdisttonpc(self,hero)>1400)
    return;
  npc.aiPush(AiQueue::aiPlayAnim("T_RISE"));
  ai_startstate(self,int(findFunction("ZS_MM_ATTACK").ptr),0,"");
  auto& body = npc.handle();
  body.bodystate_interruptable_override = 0;
  body.start_aistate = int32_t(findFunction(monsterScheduler).ptr);
  body.aivar[32] = -1; // AIV_MM_RESTSTART = ONLYROUTINE
  }

void GameScript::assessMagic(Npc& npc) {
  const auto self=std::static_pointer_cast<daedalus::INpc>(npc.handlePtr());
  if(npc_getlasthitspellcat(self)==SPELL_BAD) {
    vm.push_instance(self);
    vm.push_int(PERC_ASSESSFIGHTSOUND);
    vm.push_instance(self);
    vm.push_instance(vm.global_other()->get_instance());
    vm.unsafe_call_external(findSymbol("NPC_SENDPASSIVEPERC"));
    }

  constexpr int32_t fear=32, fireRain=42; // SPL_FEAR and SPL_FIRERAIN.
  const auto spell=npc_getlasthitspellid(self);
  std::string_view next;
  switch(spell) {
    case 71: next="ZS_WHIRLWIND"; break;
    case 86: next="ZS_SUCKENERGY"; break;
    case 82: next="ZS_GREENTENTACLE"; break;
    case 81: next="ZS_SWARM"; break;
    case 39:
    case 33: next="ZS_MAGICFREEZE"; break;
    case 34: next="ZS_SHORTZAPPED"; break;
    case fear: break;
    case fireRain: next="ZS_MAGICBURNSHORT"; break;
    default: return;
    }

  npc_clearaiqueue(self);
  if(spell!=fireRain)
    clearPerceptions(self);
  if(spell==fear) {
    npc_settarget(self,std::dynamic_pointer_cast<daedalus::INpc>(vm.global_other()->get_instance()));
    const auto guild=self->guild;
    if(guild<int32_t(GIL_SEPERATOR_HUM) && guild!=int32_t(GIL_PAL) && guild!=int32_t(GIL_KDF))
      next="ZS_MAGICFLEE";
    else if(guild>int32_t(GIL_SEPERATOR_HUM) && guild!=int32_t(GIL_FIREGOLEM) && guild!=int32_t(GIL_ICEGOLEM) &&
            guild!=int32_t(GIL_STONEGOLEM) && guild!=int32_t(GIL_TROLL) && guild!=int32_t(GIL_DRAGON))
      next="ZS_MM_FLEE";
    }
  if(!next.empty())
    ai_startstate(self,int(findFunction(next).ptr),0,"");
  }
