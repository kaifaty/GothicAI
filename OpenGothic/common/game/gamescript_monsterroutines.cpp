#include "gamescript.h"

#include "world/objects/npc.h"

namespace {
constexpr std::string_view roam = "FP_ROAM";
constexpr std::string_view campfire = "FP_CAMPFIRE";
constexpr size_t routinePosition = 19;

// Stock NotR AIVAR slots used by ZS_MM_ATTACK in the verified program.
enum MonsterAttackVar : size_t {
  invincible = 4,
  lastTarget = 7,
  pursuitEnd = 8,
  selectSpell = 20,
  hitByOtherNpc = 22,
  waitBeforeAttack = 23,
  followTime = 27,
  followInWater = 28,
  monsterPriority = 29,
  maxDistanceToWaypoint = 59,
  originalFightTactic = 60,
  summonTime = 62,
  loadGame = 65,
  stateTime = 68,
  pursuitDistance = 69,
  };
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

bool GameScript::runMonsterAttack(Npc& npc, const daedalus::DaedalusSymbol& symbol, int& result) {
  const auto self = std::static_pointer_cast<daedalus::INpc>(npc.handlePtr());
  auto& ai = self->aivar;
  auto& otherSymbol = *vm.global_other();
  const auto other = [&] { return std::dynamic_pointer_cast<daedalus::INpc>(otherSymbol.get_instance()); };
  const auto needsWeapon = [&] {
    // These script parameters are serialized as static instance locals.
    findSymbol("C_NPCISMONSTERMAGE.SLF")->set_instance(self);
    const auto guild = self->guild;
    return guild>GIL_SEPERATOR_ORC || guild==GIL_SUMMONED_SKELETON || guild==GIL_SKELETON ||
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

    // B_VALIDATEOTHER restores the last target when this state starts without OTHER.
    if(!hlp_isvalidnpc(other())) {
      const auto restored = hlp_getnpc(ai[lastTarget]);
      otherSymbol.set_instance(restored);
      npc_settarget(self,restored);
      ai[loadGame] = 1;
      } else {
      npc_gettarget(self);
      ai[lastTarget] = hlp_getinstanceid(other());
      ai[loadGame] = 0;
      }
    if(self->guild==GIL_SHEEP) {
      npc_clearaiqueue(self);
      vm.call_function<void>("B_CLEARPERCEPTIONS",self);
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
    if(vm.call_function<int32_t>("C_WANTTOEAT",self,other())!=0 && npc_isdead(other())) {
      npc_clearaiqueue(self);
      ai_startstate(self,int(findFunction("ZS_MM_EATBODY").ptr),0,"");
      }
    result = 0;
    return true;
    }

  if(phase!="ZS_MM_ATTACK_LOOP")
    return false;

  const auto targetHasBodyState = [&](int32_t state) {
    const auto target = other();
    findSymbol("C_BODYSTATECONTAINS.BODYSTATE")->set_int(state);
    findSymbol("C_BODYSTATECONTAINS.SLF")->set_instance(target);
    return ActivityDefinitions::bodyStateContains(npc_getbodystate(target),state);
    };
  const auto targetIsRunningOrJumping = [&] {
    const bool jumping = targetHasBodyState(BS_JUMP);
    const bool running = targetHasBodyState(BS_RUN);
    return running || jumping;
    };
  const auto targetIsDown = [&](const std::shared_ptr<daedalus::INpc>& target) {
    findSymbol("C_NPCISDOWN.SLF")->set_instance(target);
    return npc_isdead(target) || npc_isinstate(target,int(findFunction("ZS_MAGICSLEEP").ptr)) ||
           npc_isinstate(target,int(ZS_Unconscious));
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
  if(self->guild==GIL_DRAGON) {
    ++ai[routinePosition];
    if(ai[routinePosition]>=2 && self->attribute[ATR_HITPOINTS]<self->attribute[ATR_HITPOINTSMAX]) {
      ++self->attribute[ATR_HITPOINTS];
      ai[routinePosition] = 0;
      }
    }
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

  const bool diving = targetHasBodyState(BS_DIVE);
  const bool swimming = targetHasBodyState(BS_SWIM);
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
  if(!targetIsDown(target) && hlp_isvalidnpc(target)) {
    if(target->aivar[invincible]==0)
      ai_attack(self);
    else
      npc_clearaiqueue(self);
    ai[lastTarget] = hlp_getinstanceid(target);
    result = LOOP_CONTINUE;
    return true;
    }

  if(vm.call_function<int32_t>("C_WANTTOEAT",self,other())!=0 && ai[monsterPriority]==findSymbol("PRIO_EAT")->get_int()) {
    stop();
    result = LOOP_END;
    return true;
    }
  npc_perceiveall(self);
  npc_getnexttarget(self);
  const auto next = other();
  const bool closeOrPlayer = npc_isplayer(next) || distanceToTarget()<findSymbol("PERC_DIST_INTERMEDIAT")->get_int();
  const bool down = targetIsDown(next);
  if(next!=nullptr && next->aivar[invincible]==0 && closeOrPlayer && !down && hlp_isvalidnpc(next)) {
    ai[lastTarget] = hlp_getinstanceid(next);
    result = LOOP_CONTINUE;
    return true;
    }
  stop();
  result = LOOP_END;
  return true;
  }
