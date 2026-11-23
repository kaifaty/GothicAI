#include "gamescript.h"

#include "damagecalculator.h"
#include "definitions/fightaidefinitions.h"
#include "npcactivityhost.h"
#include "gothic.h"

#include "world/bodytraces.h"
#include "world/objects/npc.h"

#include <algorithm>

std::string_view GameScript::symbolName(size_t id) {
  if(auto* symbol = findSymbol(id))
    return symbol->name();
  return "";
  }

ScriptFn GameScript::findFunction(std::string_view name) {
  auto* symbol = findSymbol(name);
  if(symbol==nullptr || symbol->type()!=daedalus::DaedalusDataType::FUNCTION)
    return {};
  return ScriptFn(symbol->index());
  }

bool GameScript::usesNativeQuietSound(ScriptFn callback) const {
  return callback.isValid() && callback==quietSound.callback && activityEvents && !floorDefer.active() &&
         ActivityDefinitionRuntime::mode()!=ActivityDefinitionRuntime::Mode::Off;
  }

bool GameScript::nativeRoomReactionsEnabled() const {
  return nativeRoutineProgram && activityEvents && !floorDefer.active() &&
         ActivityDefinitionRuntime::mode()!=ActivityDefinitionRuntime::Mode::Off;
  }

bool GameScript::invokeNativeRoomPerception(const daedalus::DaedalusSymbol& symbol, NpcActivityProbe::Phase& phase) {
  if(symbol.name()!="B_EXITIFROOMLEFT" || !nativeRoomReactionsEnabled())
    return false;
  phase.fromRecord();
  phase.result = runExitIfRoomLeft();
  return true;
  }

void GameScript::loadNativeRoomReaction() {
  if(!nativeRoutineProgram)
    return;
  vm.override_function("B_ASSESSENTERROOM",std::function<int32_t()>{[this] { return runEnterRoomReaction(); }},[this] {
    return nativeRoomReactionsEnabled();
    });
  }

int32_t GameScript::runEnterRoomReaction() {
  const auto self = std::dynamic_pointer_cast<daedalus::INpc>(vm.global_self()->get_instance());
  const auto other = [&] { return std::dynamic_pointer_cast<daedalus::INpc>(vm.global_other()->get_instance()); };
  const auto inState = [&](std::string_view state) { return npc_isinstate(self,int(findFunction(state).ptr)); };
  const auto hasBodyState = [&](const std::shared_ptr<daedalus::INpc>& actor, int32_t state) {
    return ActivityDefinitions::bodyStateContains(npc_getbodystate(actor),state);
    };

  auto& portalGuild = *findSymbol("B_ASSESSENTERROOM.PORTALGUILD");
  const auto guild = wld_getplayerportalguild();
  portalGuild.set_int(guild);
  auto& comment = *findSymbol("PLAYER_LEFTROOMCOMMENT");
  if(guild!=int32_t(GIL_PUBLIC) && guild>int32_t(GIL_NONE) && comment.get_int()==1 && npc_isplayer(other()))
    comment.set_int(0);

  if(npc_getdisttonpc(self,other())>1000)
    return 0;
  const bool listenerInRoom = npc_getportalguild(self)>=int32_t(GIL_NONE);
  if(listenerInRoom && !npc_isinplayersroom(self))
    return 0;
  if(npc_isinstate(self,int(ZS_Attack)) || vm.call_function<int32_t>("C_NPCISGATEGUARD",self)!=0 || !npc_isplayer(other()))
    return 0;
  if(int32_t(self->type)==findSymbol("NPCTYPE_FRIEND")->get_int() || npc_getattitude(self,other())==ATT_FRIENDLY)
    return 0;

  const auto source = other();
  if(hasBodyState(source,BS_STAND) || hasBodyState(source,BS_SNEAK)) {
    const bool observing = inState("ZS_OBSERVEPLAYER");
    const bool canSee = npc_canseenpc(self,other());
    if(!observing && !canSee)
      return 0;
    }
  if(self->guild==GIL_NONE)
    return 0;
  const bool sleeping = inState("ZS_SLEEP");
  const bool sameRoom = npc_isinplayersroom(self);
  if(sleeping && !sameRoom)
    return 0;

  const bool publicRoom = sameRoom && portalGuild.get_int()==int32_t(GIL_PUBLIC);
  if(publicRoom) {
    if(inState("ZS_OBSERVEPLAYER"))
      return 0;
    if(hasBodyState(self,BS_LIE))
      vm.call_function<void>("B_MM_DESYNCHRONIZE");
    static constexpr std::string_view interruptible[] = {
      "ZS_STAND_WP", "ZS_STAND_GUARDING", "ZS_STAND_EATING", "ZS_STAND_DRINKING", "ZS_STAND_ARMSCROSSED",
      "ZS_SMOKE_JOINT", "ZS_SMALLTALK", "ZS_SLEEP", "ZS_SIT_THRONE", "ZS_SIT_CHAIR", "ZS_SIT_CAMPFIRE",
      "ZS_SIT_BENCH", "ZS_READ_BOOKSTAND", "ZS_POTION_ALCHEMY",
      };
    if(!std::ranges::any_of(interruptible,inState))
      return 0;
    }
  else if(vm.call_function<int32_t>("C_NPCISBOTHEREDBYPLAYERROOMGUILD",self)==0) {
    return 0;
    }

  npc_clearaiqueue(self);
  vm.call_function<void>("B_CLEARPERCEPTIONS",self);
  if(publicRoom)
    ai_startstate(self,int(findFunction("ZS_OBSERVEPLAYER").ptr),hasBodyState(self,BS_SIT) ? 0 : 1,"");
  else
    ai_startstate(self,int(findFunction("ZS_CLEARROOM").ptr),1,"");
  return 1;
  }

void GameScript::runQuietSoundReaction(Npc& listener) {
  const auto* source = findNpc(vm.global_other());
  if(source==nullptr)
    return;
  const auto height = NpcEngineCalls::heightDifference(listener,*source);
  if(height>quietSound.heightLimit ||
     (height>quietSound.indoorHeightLimit && wld_getplayerportalguild()>=int32_t(GIL_NONE)))
    return;

  if(runEnterRoomReaction()!=0 || listener.isInState(findFunction("ZS_GUARD_PASSAGE")))
    return;

  const auto self = std::static_pointer_cast<daedalus::INpc>(listener.handlePtr());
  const auto hostileToSource = [&] {
    const auto* other = findNpc(vm.global_other());
    return other!=nullptr && personAttitude(listener,*other)==ATT_HOSTILE;
    };
  const bool importantInfo = npc_checkinfo(self,1);
  const bool hostile = hostileToSource();
  if(!importantInfo && !hostile)
    return;

  constexpr size_t enemyOverride = 61; // AIV_ENEMYOVERRIDE in the verified program
  const bool bandit = listener.handle().guild==GIL_BDT;
  const auto fakeBandit = vm.call_function<int32_t>("C_PLAYERISFAKEBANDIT",vm.global_self()->get_instance(),vm.global_other()->get_instance())!=0;
  const bool overrideEnemy = listener.handle().aivar[enemyOverride]==1;
  const bool stillHostile = hostileToSource();
  if(((bandit && fakeBandit) || overrideEnemy) && stillHostile)
    return;
  if(listener.canSeeSource())
    return;

  listener.clearAiQueue();
  vm.call_function<void>("B_CLEARPERCEPTIONS",vm.global_self()->get_instance());
  ai_startstate(self,int(findFunction("ZS_OBSERVEPLAYER").ptr),1,"");
  }

bool GameScript::runObservePlayer(Npc& npc, const daedalus::DaedalusSymbol& symbol, int& result) {
  constexpr size_t seenLeftRoom = 21; // AIV_SEENLEFTROOM in the verified program
  constexpr size_t turnInterval = 68; // AIV_STATETIME in the verified program
  const auto self = std::static_pointer_cast<daedalus::INpc>(npc.handlePtr());
  // Read OTHER from the live script context after each shared helper.
  const auto other = [&] { return vm.global_other()->get_instance(); };
  const auto otherNpc = [&] { return std::dynamic_pointer_cast<daedalus::INpc>(other()); };
  const auto hasBodyState = [&](const std::shared_ptr<daedalus::INpc>& actor, int32_t state) {
    return ActivityDefinitions::bodyStateContains(npc_getbodystate(actor),state);
    };
  const auto say = [&](std::string_view line) {
    vm.call_function<void>("B_SAY",self,other(),line);
    };
  const auto turnToOther = [&] {
    vm.call_function<void>("B_TURNTONPC",self,other());
    };

  const std::string_view name = symbol.name();
  if(name=="ZS_OBSERVEPLAYER") {
    vm.call_function<void>("PERCEPTION_SET_NORMAL");
    const bool seated = hasBodyState(self,BS_SIT);
    if(!seated)
      ai_standup(self);
    vm.call_function<void>("B_LOOKATNPC",self,other());
    if(!seated)
      turnToOther();
    if(npc_wasinstate(self,int(findFunction("ZS_SLEEP").ptr)))
      say("$YOUDISTURBEDMYSLUMBER");
    const bool watchingPlayer = npc_isplayer(otherNpc());
    const bool sawExit = self->aivar[seenLeftRoom]==1;
    if(watchingPlayer && sawExit) {
      self->aivar[seenLeftRoom] = 0;
      auto& comment = *findSymbol("PLAYER_LEFTROOMCOMMENT");
      if(comment.get_int()==0) {
        say("$WHATDIDYOUDOINTHERE");
        comment.set_int(1);
        }
      }
    self->aivar[turnInterval] = 1+hlp_random(2);
    result = 0;
    return true;
    }

  if(name=="ZS_OBSERVEPLAYER_LOOP") {
    auto& comment = *findSymbol("PLAYER_SNEAKERCOMMENT");
    const bool commentPending = comment.get_int()==0;
    const bool sneaking = hasBodyState(otherNpc(),BS_SNEAK);
    if(commentPending && sneaking) {
      comment.set_int(1);
      ai_pointatnpc(self,otherNpc());
      say("$WHATSTHISSUPPOSEDTOBE");
      ai_stoppointat(self);
      npc_sendpassiveperc(self,PERC_ASSESSWARN,self,otherNpc());
      }
    if(npc_getstatetime(self)>self->aivar[turnInterval]) {
      if(!hasBodyState(self,BS_SIT))
        turnToOther();
      self->aivar[turnInterval] = 1+hlp_random(2);
      npc_setstatetime(self,0);
      }
    const auto distance = npc_getdisttonpc(self,otherNpc());
    if(distance>findSymbol("PERC_DIST_INTERMEDIAT")->get_int()) {
      npc_clearaiqueue(self);
      result = LOOP_END;
      } else {
      result = LOOP_CONTINUE;
      }
    return true;
    }

  if(name=="ZS_OBSERVEPLAYER_END") {
    vm.call_function<void>("B_STOPLOOKAT",self);
    result = 0;
    return true;
    }
  return false;
  }

int32_t GameScript::runExitIfRoomLeft() {
  const auto self = std::dynamic_pointer_cast<daedalus::INpc>(vm.global_self()->get_instance());
  const auto guild = wld_getplayerportalguild();
  findSymbol("B_EXITIFROOMLEFT.PORTALGUILD")->set_int(guild);
  const bool bothered = vm.call_function<int32_t>("C_NPCISBOTHEREDBYPLAYERROOMGUILD",self)!=0;
  if(guild!=int32_t(GIL_PUBLIC) && bothered)
    return 0;

  npc_clearaiqueue(self);
  ai_standup(self);
  vm.call_function<void>("B_STOPLOOKAT",self);
  const bool wantsAttack = vm.call_function<int32_t>("C_WANTTOATTACKROOMINTRUDER",self)!=0;
  vm.call_function<void>("B_SAY",self,vm.global_other()->get_instance(),
                         wantsAttack ? std::string_view{"$YESGOOUTOFHERE"} : std::string_view{"$WISEMOVE"});
  ai_continueroutine(self);
  return 1;
  }

bool GameScript::runClearRoom(Npc& npc, const daedalus::DaedalusSymbol& symbol, int& result) {
  if(!nativeRoomReactionsEnabled())
    return false;
  constexpr size_t routinePosition = 19; // AIV_TAPOSITION in the verified program
  constexpr int32_t notInPosition = 1;
  constexpr int32_t inPosition = 0;
  const auto self = std::static_pointer_cast<daedalus::INpc>(npc.handlePtr());
  const auto other = [&] { return vm.global_other()->get_instance(); };
  const auto otherNpc = [&] { return std::dynamic_pointer_cast<daedalus::INpc>(other()); };
  const auto gotoOther = [&] { ai_gotowp(self,npc_getnearestwp(otherNpc())); };
  const auto turnToOther = [&] { vm.call_function<void>("B_TURNTONPC",self,other()); };
  const auto say = [&](std::string_view line) { vm.call_function<void>("B_SAY",self,other(),line); };
  const auto wantsAttack = [&] { return vm.call_function<int32_t>("C_WANTTOATTACKROOMINTRUDER",self)!=0; };

  const std::string_view name = symbol.name();
  if(name=="ZS_CLEARROOM") {
    vm.call_function<void>("PERCEPTION_SET_MINIMAL");
    npc_percenable(self,PERC_ASSESSENTERROOM,int(findFunction("B_EXITIFROOMLEFT").ptr));
    npc_percenable(self,PERC_MOVEMOB,int(findFunction("B_MOVEMOB").ptr));
    npc_percenable(self,PERC_ASSESSTALK,int(findFunction("B_CLEARROOMTALK").ptr));
    ai_standup(self);
    vm.call_function<void>("B_LOOKATNPC",self,other());
    ai_setwalkmode(self,int(WalkBit::WM_Run));
    if(!npc_isinplayersroom(self))
      gotoOther();
    self->aivar[routinePosition] = notInPosition;
    result = 0;
    return true;
    }

  if(name=="ZS_CLEARROOM_LOOP") {
    if(self->aivar[routinePosition]==notInPosition) {
      turnToOther();
      if(wantsAttack()) {
        vm.call_function<void>("B_SELECTWEAPON",self,other());
        say("$GETOUTOFHERE");
        } else if(npc_getattitude(otherNpc(),self)!=ATT_FRIENDLY) {
        say("$WHYAREYOUINHERE");
        }
      npc_setstatetime(self,0);
      self->aivar[routinePosition] = inPosition;
      }
    if(runExitIfRoomLeft()!=0) {
      result = LOOP_END;
      return true;
      }
    if(wantsAttack()) {
      if(npc_getstatetime(self)>5) {
        const auto reason = findSymbol("AR_CLEARROOM")->get_int();
        vm.call_function<void>("B_ATTACK",self,other(),reason,0);
        result = LOOP_END;
        return true;
        }
      } else if(npc_getstatetime(self)>=2) {
      if(!npc_canseenpcfreelos(self,otherNpc())) {
        gotoOther();
        turnToOther();
        } else if(!npc_canseenpc(self,otherNpc())) {
        turnToOther();
        }
      npc_setstatetime(self,0);
      }
    result = LOOP_CONTINUE;
    return true;
    }

  if(name=="ZS_CLEARROOM_END") {
    vm.call_function<void>("B_STOPLOOKAT",self);
    result = 0;
    return true;
    }
  return false;
  }

bool GameScript::runSpellVictimState(Npc& npc, const daedalus::DaedalusSymbol& symbol, int& result) {
  enum class Kind { Inflate, Drain, Swarm };
  struct TimedSpell {
    Kind kind;
    std::string_view state, restart, spellId, duration, damage, victimLoop, victimTransition;
    size_t lastSecond;
    };
  static constexpr TimedSpell spells[] = {
    {Kind::Inflate,"ZS_INFLATE","B_RESTARTINFLATE","SPL_INFLATE","SPL_TIME_INFLATE","SPL_INFLATE_DAMAGE",
     "S_INFLATE_VICTIM","T_STAND_2_INFLATE_VICTIM",77},
    {Kind::Drain,"ZS_SUCKENERGY","B_RESTARTSUCKENERGY","SPL_SUCKENERGY","SPL_TIME_SUCKENERGY","SPL_SUCKENERGY_DAMAGE",
     "S_SUCKENERGY_VICTIM","T_STAND_2_SUCKENERGY_VICTIM",79},
    {Kind::Swarm,"ZS_SWARM","B_RESTARTSWARM","SPL_SWARM","SPL_TIME_SWARM","SPL_SWARM_DAMAGE",
     "S_SWARM_VICTIM","T_STAND_2_SWARM_VICTIM",78},
    };
  const std::string_view name = symbol.name();
  const auto* spell = std::find_if(std::begin(spells),std::end(spells),[&](const auto& s) {
    return name.starts_with(s.state) || name==s.restart;
    });
  if(spell==std::end(spells))
    return false;

  const auto self = std::static_pointer_cast<daedalus::INpc>(npc.handlePtr());
  const auto other = [&] { return vm.global_other()->get_instance(); };
  const auto number = [&](std::string_view key) { return findSymbol(key)->get_int(); };
  const auto enableMagic = [&](std::string_view callback) {
    npc_percenable(self,PERC_ASSESSMAGIC,int(findFunction(callback).ptr));
    };
  const auto playTransition = [&] {
    findSymbol("C_BODYSTATECONTAINS.SLF")->set_instance(self);
    findSymbol("C_BODYSTATECONTAINS.BODYSTATE")->set_int(BS_UNCONSCIOUS);
    if(!ActivityDefinitions::bodyStateContains(npc_getbodystate(self),BS_UNCONSCIOUS))
      ai_playanibs(self,spell->victimTransition,BS_UNCONSCIOUS);
    };
  const bool drain = spell->kind==Kind::Drain;
  result = 0;
  if(name==spell->restart) {
    if(npc_getlasthitspellid(self)==number(spell->spellId)) {
      npc_setstatetime(self,0);
      if(drain)
        ai_playani(self,spell->victimTransition);
      }
    return true;
    }
  auto& lastSecond = self->aivar[spell->lastSecond];
  const auto phase = name.substr(spell->state.size());
  if(phase.empty()) {
    enableMagic(spell->restart);
    npc_stopani(self,spell->victimLoop);
    if(spell->kind!=Kind::Inflate) {
      npc_clearaiqueue(self);
      ai_standup(self);
      }
    if(!drain)
      playTransition();
    lastSecond = drain ? 1 : 0;
    if(drain)
      findSymbol("TEMP_SUCKENERGY_DISTTOPLAYER")->set_int(npc_getdisttoplayer(self));
    return true;
    }
  if(phase=="_END")
    return true;
  if(phase!="_LOOP")
    return false;

  const auto seconds = npc_getstatetime(self);
  // Drain remembers the original distance to HERO, shared by victims in the script.
  const bool approachedHero = drain && number("TEMP_SUCKENERGY_DISTTOPLAYER")>=int32_t(uint32_t(npc_getdisttoplayer(self))+100u);
  if(approachedHero || seconds>number(spell->duration)) {
    enableMagic("B_ASSESSMAGIC");
    npc_clearaiqueue(self);
    ai_standup(self);
    if(self->guild<GIL_SEPERATOR_HUM)
      vm.call_function<void>("B_ASSESSDAMAGE");
    else
      npc_settempattitude(self,ATT_HOSTILE);
    result = LOOP_END;
    return true;
    }
  result = LOOP_CONTINUE;
  if(seconds==lastSecond)
    return true;

  std::string_view line;
  if(spell->kind==Kind::Inflate) {
    static constexpr float fatness[] = {1,3,8,12,9,15,18,11,15,13,12,10,8,5,6,4,2,1,1};
    if(seconds>0 && seconds<=int(std::size(fatness)))
      mdl_setmodelfatness(self,fatness[seconds-1]);
    switch(seconds) {
      case 5: line="$Aargh_1"; break;
      case 8: line="$Aargh_2"; break;
      case 14: line="$Aargh_3"; break;
      }
    }
  else if(spell->kind==Kind::Swarm) {
    switch(seconds) {
      case 2: line="$RunAway"; break;
      case 6: line="$Aargh_2"; break;
      case 7: line="$Aargh_3"; break;
      case 8: line="$Aargh_1"; break;
      }
    }
  else if(seconds==0) {
    playTransition();
    wld_playeffect("spellFX_SuckEnergy_BloodFly",self,vm.global_hero()->get_instance(),0,0,0,0);
    }
  if(!line.empty())
    vm.call_function<void>("B_SAY",self,other(),line);

  lastSecond = npc_getstatetime(self);
  const auto hp = self->attribute[NpcAttribute::hitpoints];
  const auto fullDamage = number(spell->damage);
  vm.call_function<void>("B_MAGICHURTNPC",other(),self,hp>fullDamage ? fullDamage : hp-1);
  if(drain && hp>fullDamage)
    npc_changeattribute(std::dynamic_pointer_cast<daedalus::INpc>(other()),ATR_HITPOINTS,fullDamage);
  return true;
  }

int32_t GameScript::intSymbol(std::string_view name, int32_t fallback) {
  auto* symbol = findSymbol(name);
  if(symbol==nullptr || symbol->type()!=daedalus::DaedalusDataType::INT)
    return fallback;
  return symbol->get_int(0);
  }

std::string_view GameScript::stringSymbol(std::string_view name) {
  if(auto* symbol = findSymbol(name))
    return symbol->get_string();
  return "";
  }

bool GameScript::isNpcInstance(size_t id) {
  auto* symbol = findSymbol(id);
  for(size_t i=0; i<16 && symbol!=nullptr && symbol->type()!=daedalus::DaedalusDataType::CLASS; ++i)
    symbol = findSymbol(size_t(symbol->parent()));
  return symbol!=nullptr && symbol->name()=="C_NPC";
  }

auto GameScript::npcRecord(size_t id) -> std::shared_ptr<NpcRecord> {
  if(auto* symbol = findSymbol(id))
    return npcRecord(symbol);
  return nullptr;
  }

size_t GameScript::goldItemId() const {
  return itMi_Gold->index();
  }

const AiState& GameScript::aiState(ScriptFn id) {
  auto it = aiStates.find(id.ptr);
  if(it!=aiStates.end())
    return it->second;
  AiState state;
  state.funcIni = id.ptr;
  if(auto* symbol = findSymbol(id.ptr)) {
    state.mname = symbol->name().c_str();
    state.funcLoop = findSymbolIndex(symbol->name()+"_Loop");
    state.funcEnd = findSymbolIndex(symbol->name()+"_End");
    } else {
    state.funcLoop = uint32_t(-1);
    state.funcEnd = uint32_t(-1);
    }
  return aiStates.emplace(id.ptr,state).first->second;
  }

int GameScript::whirlwindDuration() {
  if(auto* duration = findSymbol("SPL_TIME_WHIRLWIND"))
    return std::clamp(duration->get_int(),1,60);
  return 10;
  }

void GameScript::startupFloorForget(Npc& npc) {
  floorDefer.forget(npc);
  }

void GameScript::startupFloorBeforeResetMove(Npc& npc, const Tempest::Vec3& destination) {
  floorDefer.beforeResetMove(npc,destination);
  }

void GameScript::startupFloorBeforeKeep(Npc& npc) {
  floorDefer.beforeKeep(npc);
  }

std::array<int32_t,3> GameScript::storyNpcTypes() {
  const char* names[] = {"NPCTYPE_MAIN","NPCTYPE_OCMAIN","NPCTYPE_BL_MAIN"};
  std::array<std::optional<int32_t>,3> symbols;
  for(size_t i=0;i<symbols.size();++i) {
    auto* symbol = vm.find_symbol_by_name(names[i]);
    if(symbol!=nullptr && symbol->type()==daedalus::DaedalusDataType::INT)
      symbols[i] = symbol->get_int(0);
    }
  return BodyTraces::storyTypes(symbols);
  }

size_t GameScript::scriptVmMemorySize() const {
  return sizeof(vm)+vm.symbols().capacity()*sizeof(vm.symbols()[0])+vm.size();
  }

DamageValue GameScript::damageValue(Npc& src, Npc& other, const Bullet* bullet, bool isSpell, const Damage& spellDamage, CollideMask mask) {
  return DamageCalculator::damageValue(src,other,bullet,isSpell,spellDamage,mask);
  }

float GameScript::meleeMultiplier(float speedCmS, float typicalCmS, float bladeFraction, float surfaceAngleDeg, size_t targetIndex) {
  return DamageCalculator::meleeMultiplier(speedCmS,typicalCmS,bladeFraction,surfaceAngleDeg,targetIndex);
  }

DamageValue GameScript::damageFall(Npc& npc, float speed) {
  return DamageCalculator::damageFall(npc,speed);
  }

int32_t GameScript::vanillaChance(int32_t hitChance, float distance) {
  return DamageCalculator::vanillaChance(*this,hitChance,distance);
  }

Damage GameScript::rangeDamageValue(Npc& npc) {
  return DamageCalculator::rangeDamageValue(npc);
  }

int32_t GameScript::damageTypeMask(Npc& npc) {
  return DamageCalculator::damageTypeMask(npc);
  }

const FightTactic& GameScript::fightAi(size_t tactic) {
  return Gothic::fai()[tactic];
  }

float GameScript::attackFinishDistance() {
  float NPC_ATTACK_FINISH_DISTANCE = 180;
  if(auto var = findSymbol("NPC_ATTACK_FINISH_DISTANCE")) {
    if(var->type()==daedalus::DaedalusDataType::INT)
      NPC_ATTACK_FINISH_DISTANCE = float(var->get_int());
    else if(var->type()==daedalus::DaedalusDataType::FLOAT)
      NPC_ATTACK_FINISH_DISTANCE = var->get_float();
    }
  return NPC_ATTACK_FINISH_DISTANCE;
  }
