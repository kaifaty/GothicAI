#include "gamescript.h"

#include "damagecalculator.h"
#include "definitions/fightaidefinitions.h"
#include "npcactivityhost.h"
#include "gothic.h"

#include "world/bodytraces.h"
#include "world/objects/npc.h"

#include <algorithm>
#include <bit>
#include <stdexcept>
#include <variant>

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
  const std::string_view name = symbol.name();
  if((name!="B_EXITIFROOMLEFT" && name!="B_ASSESSPORTALCOLLISION" && name!="B_CLEARROOMTALK") ||
     !nativeRoomReactionsEnabled())
    return false;
  phase.fromRecord();
  if(name=="B_EXITIFROOMLEFT")
    phase.result = runExitIfRoomLeft();
  else if(name=="B_ASSESSPORTALCOLLISION")
    runPortalCollisionReaction();
  else
    runClearRoomTalk();
  return true;
  }

void GameScript::loadNativeRoomReaction() {
  if(!nativeRoutineProgram)
    return;
  vm.override_function("B_ASSESSENTERROOM",std::function<int32_t()>{[this] { return runEnterRoomReaction(); }},[this] {
    return nativeRoomReactionsEnabled();
    });
  }

void GameScript::setPerceptionPreset(bool normal) {
  const auto self = std::dynamic_pointer_cast<daedalus::INpc>(vm.global_self()->get_instance());
  if(self==nullptr)
    return;
  self->senses = int32_t(SensesBit::SENSE_SEE | SensesBit::SENSE_HEAR);
  self->senses_range = findSymbol("PERC_DIST_ACTIVE_MAX")->get_int();
  if(normal) {
    const bool gateGuard = isGateGuard(self);
    const bool knowsInfo = npc_knowsinfo(self,1);
    npc_setperctime(self,gateGuard || knowsInfo ? 0.3f : 1.f);
    }

  static constexpr struct {
    PercType perception;
    std::string_view callback;
    bool normalOnly;
    } callbacks[] = {
    {PERC_ASSESSPLAYER,    "B_ASSESSPLAYER",          true},
    {PERC_ASSESSENEMY,     "B_ASSESSENEMY",           true},
    {PERC_ASSESSMAGIC,     "B_ASSESSMAGIC",           false},
    {PERC_ASSESSDAMAGE,    "B_ASSESSDAMAGE",          false},
    {PERC_ASSESSMURDER,    "B_ASSESSMURDER",          false},
    {PERC_ASSESSTHEFT,     "B_ASSESSTHEFT",           false},
    {PERC_ASSESSUSEMOB,    "B_ASSESSUSEMOB",          false},
    {PERC_ASSESSENTERROOM, "B_ASSESSPORTALCOLLISION", false},
    {PERC_ASSESSTHREAT,    "B_ASSESSTHREAT",          true},
    {PERC_DRAWWEAPON,      "B_ASSESSDRAWWEAPON",      true},
    {PERC_ASSESSFIGHTSOUND,"B_ASSESSFIGHTSOUND",      true},
    {PERC_ASSESSQUIETSOUND,"B_ASSESSQUIETSOUND",      true},
    {PERC_ASSESSWARN,      "B_ASSESSWARN",            true},
    {PERC_ASSESSTALK,      "B_ASSESSTALK",            true},
    {PERC_MOVEMOB,         "B_MOVEMOB",               true},
    };
  for(const auto& entry : callbacks) {
    if(normal || !entry.normalOnly)
      npc_percenable(self,entry.perception,int(findFunction(entry.callback).ptr));
    }
  }

void GameScript::clearPerceptions(const std::shared_ptr<daedalus::INpc>& self) {
  findSymbol("B_CLEARPERCEPTIONS.SLF")->set_instance(self);
  static constexpr PercType clear[] = {
    PERC_ASSESSPLAYER, PERC_ASSESSENEMY, PERC_ASSESSBODY, PERC_ASSESSMAGIC, PERC_ASSESSDAMAGE, PERC_ASSESSMURDER,
    PERC_ASSESSTHEFT, PERC_ASSESSUSEMOB, PERC_ASSESSENTERROOM, PERC_ASSESSTHREAT, PERC_DRAWWEAPON,
    PERC_ASSESSFIGHTSOUND, PERC_ASSESSQUIETSOUND, PERC_ASSESSWARN, PERC_ASSESSTALK, PERC_MOVEMOB,
    PERC_ASSESSOTHERSDAMAGE, PERC_ASSESSSTOPMAGIC, PERC_ASSESSSURPRISE,
    };
  for(const auto perception : clear)
    npc_percdisable(self,perception);
  }

bool GameScript::isGateGuard(const std::shared_ptr<daedalus::INpc>& self) {
  findSymbol("C_NPCISGATEGUARD.SLF")->set_instance(self);
  return npc_isinstate(self,int(findFunction("ZS_GUARD_PASSAGE").ptr));
  }

bool GameScript::isBotheredByPlayerRoomGuild(const std::shared_ptr<daedalus::INpc>& self) {
  findSymbol("C_NPCISBOTHEREDBYPLAYERROOMGUILD.SLF")->set_instance(self);
  const auto portalGuild = wld_getplayerportalguild();
  findSymbol("C_NPCISBOTHEREDBYPLAYERROOMGUILD.PORTALGUILD")->set_int(portalGuild);
  const auto npcGuild = self ? int32_t(self->guild) : int32_t(GIL_NONE);
  const bool allied = wld_getguildattitude(npcGuild,portalGuild)==ATT_FRIENDLY;
  return (allied || npcGuild==portalGuild) && portalGuild>int32_t(GIL_NONE);
  }

bool GameScript::isPlayerFakeBandit(const std::shared_ptr<daedalus::INpc>& self, const std::shared_ptr<daedalus::INpc>& other) {
  findSymbol("C_PLAYERISFAKEBANDIT.OTH")->set_instance(other);
  findSymbol("C_PLAYERISFAKEBANDIT.SLF")->set_instance(self);
  constexpr size_t ignoresArmor = 73; // AIV_IGNORESARMOR in the verified program
  constexpr size_t ignoresFakeGuild = 70; // AIV_IGNORESFAKEGUILD
  const auto guild = self ? self->guild : GIL_NONE;
  if(self && (self->aivar[ignoresArmor]==1 || self->aivar[ignoresFakeGuild]==1 || guild==GIL_PIR))
    return false;

  if(npc_hasequippedarmor(other)) {
    const auto armor = npc_getequippedarmor(other);
    findSymbol("C_PLAYERISFAKEBANDIT.ITM")->set_instance(armor);
    const bool heavyBandit = hlp_isitem(armor,int(findSymbolIndex("ITAR_BDT_H")));
    const bool thorus = hlp_isitem(armor,int(findSymbolIndex("ITAR_THORUS_ADDON")));
    const bool mediumBandit = hlp_isitem(armor,int(findSymbolIndex("ITAR_BDT_M")));
    return heavyBandit || thorus || mediumBandit;
    }

  return findSymbol("PLAYER_HASTALKEDTOBANDITCAMP")->get_int()==1 &&
         findSymbol("CURRENTLEVEL")->get_int()==findSymbol("ADDONWORLD_ZEN")->get_int() && guild==GIL_BDT;
  }

void GameScript::desynchronizeNpc() {
  const auto delay = hlp_random(1000);
  findSymbol("B_MM_DESYNCHRONIZE.MSEC")->set_int(delay);
  ai_waitms(std::dynamic_pointer_cast<daedalus::INpc>(vm.global_self()->get_instance()),delay);
  }

void GameScript::sayToNpc(const std::shared_ptr<daedalus::INpc>& self, const std::shared_ptr<daedalus::INpc>& other,
                         std::string_view line) {
  auto& text = *findSymbol("B_SAY.TEXT");
  text.set_string(line);
  findSymbol("B_SAY.OTH")->set_instance(other);
  findSymbol("B_SAY.SLF")->set_instance(self);
  ai_outputsvm(self,other,text.get_string());
  }

void GameScript::turnToNpc(const std::shared_ptr<daedalus::INpc>& self, const std::shared_ptr<daedalus::INpc>& other) {
  findSymbol("B_TURNTONPC.OTH")->set_instance(other);
  findSymbol("B_TURNTONPC.SLF")->set_instance(self);
  ai_turntonpc(self,other);
  }

void GameScript::lookAtNpc(const std::shared_ptr<daedalus::INpc>& self, const std::shared_ptr<daedalus::INpc>& other) {
  findSymbol("B_LOOKATNPC.OTH")->set_instance(other);
  findSymbol("B_LOOKATNPC.SLF")->set_instance(self);
  stopLookAtNpc(self);
  ai_lookatnpc(self,other);
  }

void GameScript::stopLookAtNpc(const std::shared_ptr<daedalus::INpc>& self) {
  findSymbol("B_STOPLOOKAT.SLF")->set_instance(self);
  const auto target = npc_getlookattarget(self);
  findSymbol("B_STOPLOOKAT.TARGET")->set_instance(target);
  if(hlp_isvalidnpc(target))
    ai_stoplookat(self);
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
  if(npc_isinstate(self,int(ZS_Attack)) || isGateGuard(self) || !npc_isplayer(other()))
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
      desynchronizeNpc();
    static constexpr std::string_view interruptible[] = {
      "ZS_STAND_WP", "ZS_STAND_GUARDING", "ZS_STAND_EATING", "ZS_STAND_DRINKING", "ZS_STAND_ARMSCROSSED",
      "ZS_SMOKE_JOINT", "ZS_SMALLTALK", "ZS_SLEEP", "ZS_SIT_THRONE", "ZS_SIT_CHAIR", "ZS_SIT_CAMPFIRE",
      "ZS_SIT_BENCH", "ZS_READ_BOOKSTAND", "ZS_POTION_ALCHEMY",
      };
    if(!std::ranges::any_of(interruptible,inState))
      return 0;
    }
  else if(!isBotheredByPlayerRoomGuild(self)) {
    return 0;
    }

  npc_clearaiqueue(self);
  clearPerceptions(self);
  if(publicRoom)
    ai_startstate(self,int(findFunction("ZS_OBSERVEPLAYER").ptr),hasBodyState(self,BS_SIT) ? 0 : 1,"");
  else
    ai_startstate(self,int(findFunction("ZS_CLEARROOM").ptr),1,"");
  return 1;
  }

void GameScript::runPortalCollisionReaction() {
  auto& former = *findSymbol("B_ASSESSPORTALCOLLISION.FORMERPORTALGUILD");
  former.set_int(wld_getformerplayerportalguild());
  if(runEnterRoomReaction()!=0)
    return;

  const auto self = std::dynamic_pointer_cast<daedalus::INpc>(vm.global_self()->get_instance());
  const auto other = std::dynamic_pointer_cast<daedalus::INpc>(vm.global_other()->get_instance());
  const auto body = npc_getbodystate(other);
  const bool onFeet = ActivityDefinitions::bodyStateContains(body,BS_STAND) ||
                      ActivityDefinitions::bodyStateContains(body,BS_SNEAK);
  const bool visible = npc_canseenpc(self,other);
  if(onFeet && !visible)
    return;

  npc_perceiveall(self);
  // On success the original query replaces OTHER with the NPC already clearing the room.
  if(wld_detectnpcex(self,-1,int(findFunction("ZS_CLEARROOM").ptr),-1,0))
    return;

  const int32_t guild = int32_t(self->guild);
  const int32_t formerGuild = former.get_int();
  const bool alliedToFormerRoom = wld_getguildattitude(guild,formerGuild)==ATT_FRIENDLY;
  if(!alliedToFormerRoom && guild!=formerGuild)
    return;
  const bool friendOfPlayer = self->type==NpcType::G2_FRIEND && npc_isplayer(other);
  const int32_t otherGuild = other ? int32_t(other->guild) : int32_t(GIL_NONE);
  if(friendOfPlayer || wld_getguildattitude(guild,otherGuild)==ATT_FRIENDLY || guild==GIL_NONE)
    return;

  if(alliedToFormerRoom && (formerGuild==GIL_SLD || formerGuild==GIL_MIL)) {
    const auto reason = findSymbol("AR_LEFTPORTALROOM")->get_int();
    vm.call_function<void>("B_ATTACK",self,other,reason,0);
    return;
    }

  constexpr size_t seenLeftRoom = 21; // AIV_SEENLEFTROOM in the verified program
  self->aivar[seenLeftRoom] = 1;
  npc_clearaiqueue(self);
  clearPerceptions(self);
  ai_startstate(self,int(findFunction("ZS_OBSERVEPLAYER").ptr),0,"");
  }

void GameScript::runClearRoomTalk() {
  const auto self = std::dynamic_pointer_cast<daedalus::INpc>(vm.global_self()->get_instance());
  const bool wantsAttack = vm.call_function<int32_t>("C_WANTTOATTACKROOMINTRUDER",self)!=0;
  sayToNpc(self,std::dynamic_pointer_cast<daedalus::INpc>(vm.global_other()->get_instance()),
           wantsAttack ? "$GETOUTOFHERE" : "$WHYAREYOUINHERE");
  }

void GameScript::runQuietSoundReaction(Npc& listener) {
  const auto* source = findNpc(vm.global_other());
  if(source==nullptr)
    return;
  const auto height = NpcEngineCalls::heightDifference(listener,*source);
  if(height>quietSound.heightLimit ||
     (height>quietSound.indoorHeightLimit && wld_getplayerportalguild()>=int32_t(GIL_NONE)))
    return;

  const bool roomReaction = runEnterRoomReaction()!=0;
  const auto self = std::static_pointer_cast<daedalus::INpc>(listener.handlePtr());
  const bool gateGuard = isGateGuard(self);
  if(roomReaction || gateGuard)
    return;

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
  const auto currentSelf = std::dynamic_pointer_cast<daedalus::INpc>(vm.global_self()->get_instance());
  const bool fakeBandit = isPlayerFakeBandit(currentSelf,std::dynamic_pointer_cast<daedalus::INpc>(vm.global_other()->get_instance()));
  const bool overrideEnemy = listener.handle().aivar[enemyOverride]==1;
  const bool stillHostile = hostileToSource();
  if(((bandit && fakeBandit) || overrideEnemy) && stillHostile)
    return;
  if(listener.canSeeSource())
    return;

  listener.clearAiQueue();
  clearPerceptions(std::dynamic_pointer_cast<daedalus::INpc>(vm.global_self()->get_instance()));
  ai_startstate(self,int(findFunction("ZS_OBSERVEPLAYER").ptr),1,"");
  }

bool GameScript::runObservePlayer(Npc& npc, const daedalus::DaedalusSymbol& symbol, int& result) {
  constexpr size_t seenLeftRoom = 21; // AIV_SEENLEFTROOM in the verified program
  constexpr size_t turnInterval = 68; // AIV_STATETIME in the verified program
  const auto self = std::static_pointer_cast<daedalus::INpc>(npc.handlePtr());
  // Read OTHER from the live script context after each shared helper.
  const auto otherNpc = [&] { return std::dynamic_pointer_cast<daedalus::INpc>(vm.global_other()->get_instance()); };
  const auto hasBodyState = [&](const std::shared_ptr<daedalus::INpc>& actor, int32_t state) {
    return ActivityDefinitions::bodyStateContains(npc_getbodystate(actor),state);
    };
  const auto say = [&](std::string_view line) {
    sayToNpc(self,otherNpc(),line);
    };
  const auto turnToOther = [&] {
    turnToNpc(self,otherNpc());
    };

  const std::string_view name = symbol.name();
  if(name=="ZS_OBSERVEPLAYER") {
    setPerceptionPreset(true);
    const bool seated = hasBodyState(self,BS_SIT);
    if(!seated)
      ai_standup(self);
    lookAtNpc(self,otherNpc());
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
    stopLookAtNpc(self);
    result = 0;
    return true;
    }
  return false;
  }

int32_t GameScript::runExitIfRoomLeft() {
  const auto self = std::dynamic_pointer_cast<daedalus::INpc>(vm.global_self()->get_instance());
  const auto guild = wld_getplayerportalguild();
  findSymbol("B_EXITIFROOMLEFT.PORTALGUILD")->set_int(guild);
  const bool bothered = isBotheredByPlayerRoomGuild(self);
  if(guild!=int32_t(GIL_PUBLIC) && bothered)
    return 0;

  npc_clearaiqueue(self);
  ai_standup(self);
  stopLookAtNpc(self);
  const bool wantsAttack = vm.call_function<int32_t>("C_WANTTOATTACKROOMINTRUDER",self)!=0;
  sayToNpc(self,std::dynamic_pointer_cast<daedalus::INpc>(vm.global_other()->get_instance()),
           wantsAttack ? "$YESGOOUTOFHERE" : "$WISEMOVE");
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
  const auto turnToOther = [&] { turnToNpc(self,otherNpc()); };
  const auto say = [&](std::string_view line) { sayToNpc(self,otherNpc(),line); };
  const auto wantsAttack = [&] { return vm.call_function<int32_t>("C_WANTTOATTACKROOMINTRUDER",self)!=0; };

  const std::string_view name = symbol.name();
  if(name=="ZS_CLEARROOM") {
    setPerceptionPreset(false);
    npc_percenable(self,PERC_ASSESSENTERROOM,int(findFunction("B_EXITIFROOMLEFT").ptr));
    npc_percenable(self,PERC_MOVEMOB,int(findFunction("B_MOVEMOB").ptr));
    npc_percenable(self,PERC_ASSESSTALK,int(findFunction("B_CLEARROOMTALK").ptr));
    ai_standup(self);
    lookAtNpc(self,otherNpc());
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
    stopLookAtNpc(self);
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
    if(self->guild<int32_t(GIL_SEPERATOR_HUM))
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
  magicHurtNpc(std::dynamic_pointer_cast<daedalus::INpc>(other()),self,hp>fullDamage ? fullDamage : hp-1);
  if(drain && hp>fullDamage)
    npc_changeattribute(std::dynamic_pointer_cast<daedalus::INpc>(other()),ATR_HITPOINTS,fullDamage);
  return true;
  }

void GameScript::givePlayerXp(int32_t amount) {
  const auto add=[](int32_t a,int32_t b) {
    return std::bit_cast<int32_t>(uint32_t(a)+uint32_t(b));
    };
  const auto hero=[&] {
    return std::dynamic_pointer_cast<daedalus::INpc>(vm.global_hero()->get_instance());
    };
  const auto print=[&](daedalus::DaedalusSymbol* text,const char* y,const char* font) {
    vm.push_reference(text);
    vm.push_int(-1);
    vm.push_reference(findSymbol(y));
    vm.push_reference(findSymbol(font));
    vm.push_int(2);
    vm.unsafe_call_external(findSymbol("PRINTSCREEN"));
    };

  auto* savedAmount=findSymbol("B_GIVEPLAYERXP.ADD_XP");
  auto* text=findSymbol("B_GIVEPLAYERXP.CONCATTEXT");
  savedAmount->set_int(amount);
  const auto current=hero();
  vm.unsafe_set_gi(current);
  if(current) {
    if(current->level==0)
      current->exp_next=500;
    current->exp=add(current->exp,amount);
    }

  // The original formatting calls also update the VM's temporary strings.
  text->set_string(findSymbol("PRINT_XPGAINED")->get_string());
  vm.push_reference(text);
  vm.push_reference(savedAmount);
  vm.unsafe_call_external(findSymbol("INTTOSTRING"));
  vm.unsafe_call_external(findSymbol("CONCATSTRINGS"));
  text->set_string(vm.pop_string());
  print(text,"YPOS_XPGAINED","FONT_SCREENSMALL");

  const auto levelUp=hero();
  vm.unsafe_set_gi(levelUp);
  if(!levelUp || levelUp->exp>=levelUp->exp_next) {
    if(levelUp) {
      levelUp->level=add(levelUp->level,1);
      const auto threshold=std::bit_cast<int32_t>(500u*(uint32_t(levelUp->level)+1u));
      levelUp->exp_next=add(levelUp->exp_next,threshold);
      const auto health=findSymbol("HP_PER_LEVEL")->get_int();
      levelUp->attribute[ATR_HITPOINTSMAX]=add(levelUp->attribute[ATR_HITPOINTSMAX],health);
      levelUp->attribute[ATR_HITPOINTS]=add(levelUp->attribute[ATR_HITPOINTS],health);
      levelUp->lp=add(levelUp->lp,findSymbol("LP_PER_LEVEL")->get_int());
      }
    print(findSymbol("PRINT_LEVELUP"),"YPOS_LEVELUP","FONT_SCREEN");
    // Reference the verified program's LevelUp literal to preserve the temporary string slots.
    vm.push_reference(findSymbol("$12941"));
    vm.unsafe_call_external(findSymbol("SND_PLAY"));
    }

  vm.unsafe_call_bl(findSymbol("B_CHECKLOG"));
  }

void GameScript::closeQuestTopic(std::string_view topic, int32_t mission, int32_t common, int32_t endChapter) {
  auto* savedTopic=findSymbol("B_CLOSETOPIC.TOPIC");
  findSymbol("B_CLOSETOPIC.ENDCHAPTER")->set_int(endChapter);
  findSymbol("B_CLOSETOPIC.COMMONVAR")->set_int(common);
  findSymbol("B_CLOSETOPIC.MISSIONVAR")->set_int(mission);
  savedTopic->set_string(topic);

  using Status=QuestLog::Status;
  auto status=Status::Running;
  if(common==1 || mission==int32_t(Status::Success))
    status=Status::Success;
  else if(mission==int32_t(Status::Obsolete) || mission==int32_t(Status::Failed))
    status=Status(mission);
  else if((common==0 || mission==int32_t(Status::Running)) && findSymbol("KAPITEL")->get_int()>=endChapter)
    status=Status::Obsolete;
  if(status!=Status::Running) {
    vm.push_reference(savedTopic);
    vm.push_int(int32_t(status));
    vm.unsafe_call_external(findSymbol("LOG_SETTOPICSTATUS"));
    }
  }

void GameScript::loadNativeQuestLog(const std::function<bool()>& enabled) {
  using Op=daedalus::DaedalusOpcode;
  using Type=daedalus::DaedalusDataType;
  using Number=std::variant<int32_t,uint32_t>; // literal or live integer symbol
  struct Row {
    uint32_t topic;
    std::array<Number,3> values;
    };
  struct GuildRule {
    size_t before;
    uint32_t topic, status;
    std::array<uint32_t,3> excluded;
    };
  static constexpr std::array groups{
    std::pair{"B_CLOSETOPICS_MATTES",101u},
    std::pair{"B_CLOSETOPICS_HOSHI",18u},
    std::pair{"B_CLOSETOPICS_YOLY",87u}
    };
  const auto* closeFunction=findSymbol("B_CLOSETOPIC");
  const auto close=closeFunction->index();
  const auto setStatus=findSymbol("LOG_SETTOPICSTATUS")->index();
  auto pc=findSymbol("B_CHECKLOG")->address();
  const auto invalid=[&] {
    return std::runtime_error("Invalid stock quest log record at "+std::to_string(pc));
    };
  const auto take=[&](Op expected) {
    const auto instruction=vm.instruction_at(pc);
    if(instruction.op!=expected)
      throw invalid();
    pc+=instruction.size;
    return instruction;
    };
  const auto reference=[&](Type type) {
    const auto id=take(Op::PUSHV).symbol;
    const auto* symbol=findSymbol(id);
    if(symbol==nullptr || symbol->type()!=type || symbol->is_member())
      throw invalid();
    return id;
    };
  const auto number=[&]() -> Number {
    if(vm.instruction_at(pc).op==Op::PUSHI)
      return take(Op::PUSHI).immediate;
    return reference(Type::INT);
    };

  for(const auto& [name,count]:groups)
    if(take(Op::BL).address!=findSymbol(name)->address())
      throw invalid();
  take(Op::RSR);

  vm.override_function("B_CLOSETOPIC",std::function<void(std::string_view,int32_t,int32_t,int32_t)>{
    [this](std::string_view topic,int32_t mission,int32_t common,int32_t chapter) {
      closeQuestTopic(topic,mission,common,chapter);
      }},enabled);
  for(const auto& [name,count]:groups) {
    pc=findSymbol(name)->address();
    std::vector<Row> rows;
    rows.reserve(count);
    std::optional<GuildRule> guildRule;
    while(vm.instruction_at(pc).op!=Op::RSR) {
      const auto* first=findSymbol(vm.instruction_at(pc).symbol);
      if(first!=nullptr && first->type()==Type::STRING) {
        Row row{reference(Type::STRING),{number(),number(),number()}};
        if(take(Op::BL).address!=closeFunction->address())
          throw invalid();
        rows.push_back(row);
        continue;
        }
      // The only branch in the three groups is Yoly's guild exception.
      if(std::string_view(name)!="B_CLOSETOPICS_YOLY" || guildRule)
        throw invalid();
      GuildRule rule{rows.size(),0,0,{}};
      for(auto& guild:rule.excluded) {
        guild=reference(Type::INT);
        if(take(Op::GMOVI).symbol!=vm.global_hero()->index() ||
           take(Op::PUSHV).symbol!=findSymbol("C_NPC.GUILD")->index())
          throw invalid();
        take(Op::NEQ);
        }
      take(Op::AND);
      take(Op::AND);
      const auto skip=take(Op::BZ).address;
      rule.topic=reference(Type::STRING);
      rule.status=reference(Type::INT);
      if(take(Op::BE).symbol!=setStatus || pc!=skip)
        throw invalid();
      guildRule=rule;
      }
    const bool hasGuildRule=std::string_view(name)=="B_CLOSETOPICS_YOLY";
    if(rows.size()!=count || guildRule.has_value()!=hasGuildRule || (guildRule && guildRule->before>=rows.size()))
      throw invalid();

    vm.override_function(name,std::function<void()>{[this, rows=std::move(rows), guildRule, close, setStatus] {
      for(size_t i=0;i<rows.size();++i) {
        if(guildRule && i==guildRule->before) {
          const auto context=vm.global_hero()->get_instance();
          vm.unsafe_set_gi(context);
          const auto hero=std::dynamic_pointer_cast<daedalus::INpc>(context);
          const auto guild=hero ? hero->guild : 0;
          if(std::ranges::none_of(guildRule->excluded,[&](uint32_t id) { return findSymbol(id)->get_int()==guild; })) {
            vm.push_reference(findSymbol(guildRule->topic));
            vm.push_reference(findSymbol(guildRule->status));
            vm.unsafe_call_external(findSymbol(setStatus));
            }
          }
        const auto& row=rows[i];
        vm.push_reference(findSymbol(row.topic));
        for(const auto& value:row.values) {
          if(const auto* literal=std::get_if<int32_t>(&value))
            vm.push_int(*literal);
          else
            vm.push_reference(findSymbol(std::get<uint32_t>(value)));
          }
        vm.unsafe_call_bl(findSymbol(close));
        }
      }},enabled);
    }
  vm.override_function("B_CHECKLOG",std::function<void()>{[this] {
    for(const auto& [name,count]:groups)
      vm.unsafe_call_bl(findSymbol(name));
    }},enabled);
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
