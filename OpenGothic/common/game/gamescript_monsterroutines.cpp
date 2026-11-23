#include "gamescript.h"

#include <algorithm>

#include "world/objects/item.h"
#include "world/objects/npc.h"

namespace {
constexpr std::string_view roam = "FP_ROAM";
constexpr std::string_view campfire = "FP_CAMPFIRE";
constexpr size_t routinePosition = 19;
constexpr size_t shrinkStage = 42; // AIV_MM_SHRINKSTATE
constexpr int32_t shrinkSpell = 46; // SPL_SHRINK
constexpr int32_t shrinkScrollCost = 5;
constexpr int32_t shrinkRuneCost = 300;
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
      findSymbol("B_CLEARPERCEPTIONS.SLF")->set_instance(victim);
      for(const auto type:{PERC_ASSESSPLAYER, PERC_ASSESSENEMY, PERC_ASSESSBODY, PERC_ASSESSMAGIC,
                           PERC_ASSESSDAMAGE, PERC_ASSESSMURDER, PERC_ASSESSTHEFT, PERC_ASSESSUSEMOB,
                           PERC_ASSESSENTERROOM, PERC_ASSESSTHREAT, PERC_DRAWWEAPON, PERC_ASSESSFIGHTSOUND,
                           PERC_ASSESSQUIETSOUND, PERC_ASSESSWARN, PERC_ASSESSTALK, PERC_MOVEMOB,
                           PERC_ASSESSOTHERSDAMAGE, PERC_ASSESSSTOPMAGIC, PERC_ASSESSSURPRISE})
        npc_percdisable(victim,type);
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
