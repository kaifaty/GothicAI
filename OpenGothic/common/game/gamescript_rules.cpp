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
    startNpcAttack(self,other,reason,0);
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
  const bool wantsAttack = wantsToAttackRoomIntruder(self);
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

bool GameScript::nativeHumanPerceptionsEnabled() const {
  return activityEvents && !floorDefer.active() && ActivityDefinitionRuntime::mode()!=ActivityDefinitionRuntime::Mode::Off;
  }

void GameScript::loadNativeHumanPerceptions() {
  if(!nativeRoutineProgram)
    return;
  drawWeaponCallback = findFunction("B_ASSESSDRAWWEAPON");
  threatCallback = findFunction("B_ASSESSTHREAT");
  warnCallback = findFunction("B_ASSESSWARN");
  fightSoundCallback = findFunction("B_ASSESSFIGHTSOUND");
  damageCallback = findFunction("B_ASSESSDAMAGE");
  enemyCallback = findFunction("B_ASSESSENEMY");
  playerCallback = findFunction("B_ASSESSPLAYER");
  theftCallback = findFunction("B_ASSESSTHEFT");
  useMobCallback = findFunction("B_ASSESSUSEMOB");
  stopShortZapCallback = findFunction("B_STOPSHORTZAPPED");
  const auto enabled = [this] { return nativeHumanPerceptionsEnabled(); };
  vm.override_function("B_ASSESSDRAWWEAPON",std::function<int32_t()>{[this] { return runDrawWeaponReaction(); }},enabled);
  vm.override_function("B_ASSESSDAMAGE",std::function<void()>{[this] { runDamageReaction(); }},enabled);
  vm.override_function("B_ASSESSENEMY",std::function<int32_t()>{[this] { return runEnemyReaction(); }},enabled);
  vm.override_function("B_ASSESSPLAYER",std::function<void()>{[this] { runPlayerAssessment(); }},enabled);
  vm.override_function("B_ASSESSFIGHTSOUND",std::function<void()>{[this] { runFightSoundReaction(); }},enabled);
  vm.override_function("B_VALIDATEOTHER",std::function<void()>{[this] {
    validateCombatTarget(std::dynamic_pointer_cast<daedalus::INpc>(vm.global_self()->get_instance()));
    }},enabled);
  using Actor = std::shared_ptr<daedalus::INpc>;
  vm.override_function("C_NPCISBOTHEREDBYWEAPON",std::function<int32_t(Actor,Actor)>{[this](const Actor& self, const Actor& other) {
    return botheredByWeapon(self,other);
    }},enabled);
  }

bool GameScript::invokeNativeHumanPerception(ScriptFn fn, NpcActivityProbe::Phase& phase) {
  const bool stopShortZap = fn==stopShortZapCallback;
  const bool propertyCrime = fn==theftCallback || fn==useMobCallback;
  if((fn!=drawWeaponCallback && fn!=threatCallback && fn!=warnCallback && fn!=fightSoundCallback &&
      fn!=damageCallback && fn!=enemyCallback && fn!=playerCallback && !propertyCrime && !stopShortZap) ||
     !nativeHumanPerceptionsEnabled())
    return false;
  phase.fromRecord();
  if(fn==drawWeaponCallback)
    phase.result = runDrawWeaponReaction();
  else if(fn==threatCallback)
    runThreatReaction();
  else if(fn==enemyCallback)
    phase.result = runEnemyReaction();
  else if(fn==playerCallback)
    runPlayerAssessment();
  else if(propertyCrime)
    runPropertyCrime(fn==useMobCallback);
  else if(fn==warnCallback)
    runWarnReaction();
  else if(fn==fightSoundCallback)
    runFightSoundReaction();
  else if(stopShortZap)
    stopControlSpell(std::dynamic_pointer_cast<daedalus::INpc>(vm.global_self()->get_instance()),true);
  else
    runDamageReaction();
  return true;
  }

int32_t GameScript::botheredByWeapon(const std::shared_ptr<daedalus::INpc>& self, const std::shared_ptr<daedalus::INpc>& other) {
  findSymbol("C_NPCISBOTHEREDBYWEAPON.OTH")->set_instance(other);
  findSymbol("C_NPCISBOTHEREDBYWEAPON.SLF")->set_instance(self);
  constexpr size_t partyMember = 15; // AIV_PARTYMEMBER
  if(self==nullptr || self->aivar[partyMember]==1)
    return 0;
  // The friend exemption reads global OTHER. The weapon, attitude and spell checks use the argument.
  if(npc_isplayer(std::dynamic_pointer_cast<daedalus::INpc>(vm.global_other()->get_instance())) && self->type==NpcType::G2_FRIEND)
    return 0;
  const bool melee = npc_isinfightmode(other,int(FightMode::FMODE_MELEE));
  const bool toughGuy = isToughGuy(self);
  if(melee && toughGuy)
    return 0;
  if(npc_getattitude(self,other)==ATT_FRIENDLY || isGateGuard(self) ||
     npc_isinfightmode(other,int(FightMode::FMODE_FIST)))
    return 0;
  const auto spellCategory = npc_getactivespellcat(other);
  if(npc_isinfightmode(other,int(FightMode::FMODE_MAGIC)) && spellCategory!=SPELL_BAD)
    return 0;
  return self->guild!=GIL_ORC && self->guild!=GIL_DMT;
  }

int32_t GameScript::runDrawWeaponReaction() {
  const auto self = std::dynamic_pointer_cast<daedalus::INpc>(vm.global_self()->get_instance());
  const auto other = [&] { return std::dynamic_pointer_cast<daedalus::INpc>(vm.global_other()->get_instance()); };
  if(npc_isinfightmode(other(),int(FightMode::FMODE_NONE)) ||
     npc_getdisttonpc(self,other())>findSymbol("PERC_DIST_DIALOG")->get_int() || botheredByWeapon(self,other())==0)
    return 0;
  const auto react = findFunction("ZS_REACTTOWEAPON");
  if(npc_isinstate(self,int(react.ptr)))
    return 0;
  const bool watchedFight = npc_wasinstate(self,int(findFunction("ZS_WATCHFIGHT").ptr));
  const bool observing = npc_isinstate(self,int(findFunction("ZS_OBSERVEPLAYER").ptr));
  if((watchedFight && observing) || !npc_canseenpc(self,other()))
    return 0;

  startHumanWarning(self,react);
  return 1;
  }

void GameScript::startHumanWarning(const std::shared_ptr<daedalus::INpc>& self, ScriptFn state) {
  npc_clearaiqueue(self);
  clearPerceptions(self);
  ai_startstate(self,int(state.ptr),0,"");
  }

void GameScript::runThreatReaction() {
  const auto self = std::dynamic_pointer_cast<daedalus::INpc>(vm.global_self()->get_instance());
  const auto other = std::dynamic_pointer_cast<daedalus::INpc>(vm.global_other()->get_instance());
  if(npc_getdisttonpc(self,other)>findSymbol("PERC_DIST_INTERMEDIAT")->get_int() ||
     !npc_canseenpc(self,other) || botheredByWeapon(self,other)==0)
    return;
  startHumanWarning(self,findFunction("ZS_REACTTOWEAPON"));
  }

void GameScript::runWarnReaction() {
  const auto self = std::dynamic_pointer_cast<daedalus::INpc>(vm.global_self()->get_instance());
  const auto source = std::dynamic_pointer_cast<daedalus::INpc>(vm.global_other()->get_instance());
  const auto victim = vm.global_victim()->get_instance();
  if(hlp_getinstanceid(self)==hlp_getinstanceid(victim))
    return;
  const auto observe = findFunction("ZS_OBSERVEPLAYER");
  if(npc_isinstate(self,int(observe.ptr)) || npc_isinstate(self,int(findFunction("ZS_REACTTOWEAPON").ptr)))
    return;
  if((source!=nullptr && source->guild>int32_t(GIL_SEPERATOR_HUM)) || isGateGuard(self))
    return;
  startHumanWarning(self,observe);
  }

bool GameScript::hasAttackReasonToKill(const std::shared_ptr<daedalus::INpc>& npc) {
  findSymbol("C_NPCHASATTACKREASONTOKILL.SLF")->set_instance(npc);
  const auto reason = npc!=nullptr ? npc->aivar[9] : 0; // AIV_ATTACKREASON
  return reason==findSymbol("AR_GUARDCALLEDTOKILL")->get_int() || reason==findSymbol("AR_GUARDSTOPSINTRUDER")->get_int() ||
         reason==findSymbol("AR_HUMANMURDEREDHUMAN")->get_int() || reason==findSymbol("AR_GUILDENEMY")->get_int() ||
         reason==findSymbol("AR_KILL")->get_int();
  }

bool GameScript::wantsToAttackSheepKiller(const std::shared_ptr<daedalus::INpc>& self,
                                        const std::shared_ptr<daedalus::INpc>& other) {
  findSymbol("C_WANTTOATTACKSHEEPKILLER.OTH")->set_instance(other);
  findSymbol("C_WANTTOATTACKSHEEPKILLER.SLF")->set_instance(self);
  const auto guild = self->guild;
  constexpr size_t dontAttackSheepKiller = 56;
  if(guild==GIL_DJG || guild==GIL_BDT || guild==GIL_STRF || guild==GIL_DMT ||
     self->aivar[dontAttackSheepKiller]==1 || (self->type==NpcType::G2_FRIEND && npc_isplayer(other)))
    return false;
  if(hasAttackReasonToKill(other))
    return false;
  const auto source = std::dynamic_pointer_cast<daedalus::INpc>(vm.global_other()->get_instance());
  if(source!=nullptr && source->aivar[9]!=0 && source->guild<int32_t(GIL_SEPERATOR_HUM))
    return false;
  return !isGateGuard(self);
  }

bool GameScript::wantsToAttackThief(const std::shared_ptr<daedalus::INpc>& self,
                                   const std::shared_ptr<daedalus::INpc>& other) {
  findSymbol("C_WANTTOATTACKTHIEF.OTH")->set_instance(other);
  findSymbol("C_WANTTOATTACKTHIEF.SLF")->set_instance(self);
  const auto guild = self->guild;
  constexpr size_t dontAttackThief = 55;
  if(guild==GIL_DJG || guild==GIL_STRF || guild==GIL_DMT ||
     self->aivar[dontAttackThief]==1 || self->aivar[0]==findSymbol("FIGHT_LOST")->get_int() ||
     (self->type==NpcType::G2_FRIEND && npc_isplayer(other)))
    return false;
  return !isGateGuard(self);
  }

void GameScript::runFightSoundReaction() {
  using Actor = std::shared_ptr<daedalus::INpc>;
  const auto self = std::dynamic_pointer_cast<daedalus::INpc>(vm.global_self()->get_instance());
  const auto other = [&] { return std::dynamic_pointer_cast<daedalus::INpc>(vm.global_other()->get_instance()); };
  const auto source = other();
  const auto victim = std::dynamic_pointer_cast<daedalus::INpc>(vm.global_victim()->get_instance());
  if(self==nullptr || !hlp_isvalidnpc(victim))
    return;

  const auto value = [this](std::string_view name) { return findSymbol(name)->get_int(); };
  const auto guild = [](const Actor& actor) { return actor!=nullptr ? actor->guild : int32_t(GIL_NONE); };
  const auto aivar = [](const Actor& actor, size_t index) { return actor!=nullptr ? actor->aivar[index] : 0; };
  constexpr size_t followsInWater = 28, tough = 57, enemyOverride = 61, attackReason = 9;
  constexpr size_t storyBandit = 74, supportsEsteban = 75;
  const auto witnessGuild = guild(self);
  const auto sourceGuild = guild(source);
  const auto victimGuild = guild(victim);
  const auto range = value("PERC_DIST_INTERMEDIAT");
  const bool outOfRange = npc_getdisttonpc(self,source)>range && npc_getdisttonpc(self,victim)>range;
  if(outOfRange && !((witnessGuild==GIL_BDT || witnessGuild==GIL_OUT) &&
                    (witnessGuild==sourceGuild || witnessGuild==victimGuild)))
    return;
  if(aivar(self,followsInWater)==0) {
    const auto body = [&](const Actor& actor, BodyState state) {
      findSymbol("C_BODYSTATECONTAINS.BODYSTATE")->set_int(int32_t(state));
      findSymbol("C_BODYSTATECONTAINS.SLF")->set_instance(actor);
      return ActivityDefinitions::bodyStateContains(npc_getbodystate(actor),int32_t(state));
      };
    const bool victimDives = body(victim,BS_DIVE);
    const bool victimSwims = body(victim,BS_SWIM);
    const bool sourceDives = body(source,BS_DIVE);
    const bool sourceSwims = body(source,BS_SWIM);
    if(victimDives || victimSwims || sourceDives || sourceSwims)
      return;
    }
  const auto nailed = value("FAI_NAILED");
  if(victim->fight_tactic==nailed || (source!=nullptr && source->fight_tactic==nailed))
    return;
  const auto sourceHeight = npc_getheighttonpc(self,source);
  const auto victimHeight = npc_getheighttonpc(self,victim);
  const auto heightLimit = value("PERC_DIST_HEIGHT");
  if((sourceHeight>heightLimit && victimHeight>heightLimit) ||
     (npc_getdisttowp(self,"NW_MONASTERY_PLACE_04")<=3000 && sourceHeight>500))
    return;
  const auto ownId = hlp_getinstanceid(self);
  if(ownId==hlp_getinstanceid(source) || ownId==hlp_getinstanceid(victim))
    return;

  const auto attack = [&](const Actor& target, std::string_view reason) {
    startNpcAttack(self,target,value(reason),0);
    };
  if(victimGuild==GIL_SHEEP && aivar(victim,tough)==0) {
    if(wantsToAttackSheepKiller(self,source))
      attack(source,"AR_SHEEPKILLER");
    else if(isGateGuard(self))
      memorizePlayerCrime(self,source,value("CRIME_SHEEPKILLER"));
    return;
    }
  if(isGateGuard(self))
    return;

  const bool monsterSource = sourceGuild>int32_t(GIL_SEPERATOR_HUM);
  const bool monsterVictim = victimGuild>int32_t(GIL_SEPERATOR_HUM);
  if(monsterSource && monsterVictim)
    return;
  if(npc_getattitude(self,victim)!=ATT_HOSTILE && monsterSource) {
    attack(source,"AR_MONSTERVSHUMAN");
    return;
    }
  if(!npc_isdead(victim) && npc_getattitude(self,source)!=ATT_HOSTILE && monsterVictim) {
    attack(victim,"AR_MONSTERVSHUMAN");
    return;
    }

  if(aivar(self,enemyOverride)==1) {
    self->aivar[enemyOverride] = 0;
    npc_perceiveall(self);
    npc_getnexttarget(self); // Updates OTHER before the validation and queued target use.
    const auto target = other();
    findSymbol("C_NPCISDOWN.SLF")->set_instance(target);
    const bool down = npc_isdead(target) || npc_isinstate(target,int(findFunction("ZS_MAGICSLEEP").ptr)) ||
                      npc_isinstate(target,int(findFunction("ZS_UNCONSCIOUS").ptr));
    if(!down && hlp_isvalidnpc(target))
      attack(target,"AR_GUILDENEMY");
    return;
    }

  const auto watch = [&] { startHumanWarning(self,findFunction("ZS_WATCHFIGHT")); };
  const bool disguisedVictim = isPlayerFakeBandit(self,victim);
  const bool disguisedSource = isPlayerFakeBandit(self,source);
  if(witnessGuild==GIL_BDT && (sourceGuild==GIL_BDT || victimGuild==GIL_BDT) && (disguisedVictim || disguisedSource)) {
    if(aivar(self,supportsEsteban)==1 && aivar(source,supportsEsteban)==1) {
      attack(victim,"AR_NONE");
      return;
      }
    if(aivar(self,supportsEsteban)==1 && aivar(victim,supportsEsteban)==1) {
      attack(source,"AR_NONE");
      return;
      }
    if((aivar(victim,attackReason)==0 && aivar(source,attackReason)==0) ||
       aivar(victim,storyBandit)==1 || aivar(source,storyBandit)==1) {
      watch();
      return;
      }
    }

  const auto hasReason = [&](const Actor& actor, std::initializer_list<std::string_view> reasons) {
    return std::ranges::any_of(reasons,[&](std::string_view reason) { return aivar(actor,attackReason)==value(reason); });
    };
  const auto cooperativeGuild = [](int32_t guild) { return guild==GIL_NONE || guild==GIL_DJG || guild==GIL_SLD; };
  const auto directions = std::array{std::pair{source,victim},std::pair{victim,source}};
  const auto friendly = [&](const Actor& actor) { return npc_getattitude(self,actor)==ATT_FRIENDLY; };
  for(const auto& [fighter,target] : directions) {
    if(friendly(fighter) && hasReason(fighter,{"AR_GUARDCALLEDTOKILL","AR_GUILDENEMY","AR_HUMANMURDEREDHUMAN",
                                             "AR_MONSTERCLOSETOGATE","AR_GUARDSTOPSINTRUDER"})) {
      attack(target,"AR_GUARDCALLEDTOKILL");
      return;
      }
    }
  if(!(cooperativeGuild(sourceGuild) && cooperativeGuild(victimGuild)) &&
     (npc_isplayer(source) || npc_isplayer(victim))) {
    for(const auto& [fighter,target] : directions) {
      if(friendly(fighter) && hasReason(fighter,{"AR_REACTTOWEAPON","AR_REACTTODAMAGE","AR_GUARDSTOPSFIGHT"})) {
        attack(target,"AR_GUARDSTOPSFIGHT");
        return;
        }
      }
    }
  if(witnessGuild==GIL_MIL && hasReason(victim,{"AR_KILL","AR_NONE"}) && hasReason(source,{"AR_KILL","AR_NONE"})) {
    const bool sourceFriendly = friendly(source);
    const bool victimFriendly = friendly(victim);
    const bool helpSource = sourceFriendly!=victimFriendly ? sourceFriendly : npc_isplayer(source);
    attack(helpSource ? victim : source,"AR_GUARDSTOPSFIGHT");
    return;
    }
  for(const auto& [fighter,target] : directions) {
    if(friendly(fighter) && hasReason(fighter,{"AR_LEFTPORTALROOM","AR_THEFT","AR_USEMOB"}) && wantsToAttackThief(self,target)) {
      attack(target,"AR_GUARDCALLEDTOTHIEF");
      return;
      }
    }
  for(const auto& [fighter,target] : directions) {
    if(friendly(fighter) && hasReason(fighter,{"AR_CLEARROOM"})) {
      attack(target,"AR_GUARDCALLEDTOROOM");
      return;
      }
    }
  if(outOfRange || (witnessGuild!=GIL_BDT &&
                    (npc_getattitude(self,victim)==ATT_HOSTILE || npc_getattitude(self,source)==ATT_HOSTILE)) ||
     !npc_canseenpcfreelos(self,victim))
    return;
  watch();
  }

void GameScript::runDamageReaction() {
  constexpr size_t lastTarget = 7; // AIV_LASTTARGET
  constexpr size_t attackReason = 9; // AIV_ATTACKREASON
  constexpr size_t partyMember = 15; // AIV_PARTYMEMBER
  constexpr size_t hitByOther = 22; // AIV_HITBYOTHERNPC
  constexpr size_t arenaFight = 45; // AIV_ARENAFIGHT
  constexpr size_t enemyOverride = 61; // AIV_ENEMYOVERRIDE
  const auto self = std::dynamic_pointer_cast<daedalus::INpc>(vm.global_self()->get_instance());
  const auto other = [&] { return std::dynamic_pointer_cast<daedalus::INpc>(vm.global_other()->get_instance()); };

  const auto quarhodron = hlp_getnpc(int(findSymbolIndex("NONE_ADDON_111_QUARHODRON")));
  findSymbol("B_ASSESSDAMAGE.QUARHO")->set_instance(quarhodron);
  const auto rhademes = hlp_getnpc(int(findSymbolIndex("NONE_ADDON_112_RHADEMES")));
  findSymbol("B_ASSESSDAMAGE.RHADEM")->set_instance(rhademes);
  if(self==nullptr)
    return;
  const auto selfId = hlp_getinstanceid(self);
  if(selfId==hlp_getinstanceid(rhademes) || selfId==hlp_getinstanceid(quarhodron)) {
    vm.call_function<int32_t>("B_GHOSTSPECIALDAMAGE",other(),self);
    return;
    }
  vm.call_function<void>("B_BELIARSWEAPONSPECIALDAMAGE",other(),self);

  auto& aivar = self->aivar;
  if(aivar[arenaFight]==findSymbol("AF_AFTER")->get_int())
    aivar[arenaFight] = findSymbol("AF_AFTER_PLUS_DAMAGE")->get_int();
  if(aivar[enemyOverride]==1) {
    const auto raven = hlp_getnpc(int(findSymbolIndex("BDT_1090_ADDON_RAVEN")));
    findSymbol("B_ASSESSDAMAGE.RAV")->set_instance(raven);
    if(hlp_getinstanceid(raven)==selfId)
      aivar[enemyOverride] = 0;
    }

  if(npc_isinstate(self,int(ZS_Attack))) {
    const bool player = npc_isplayer(other());
    if(player && (self->type==NpcType::G2_FRIEND || aivar[partyMember]==1))
      return;
    const auto sourceId = hlp_getinstanceid(other());
    if(aivar[lastTarget]!=sourceId) {
      if(sourceId!=hlp_getinstanceid(vm.global_hero()->get_instance()) || aivar[hitByOther]==sourceId)
        npc_settarget(self,other());
      else
        aivar[hitByOther] = sourceId;
      }
    return;
    }

  if(runEnemyReaction()!=0)
    return;
  const auto none = findSymbol("AR_NONE")->get_int();
  const auto attacker = other();
  if((attacker==nullptr || attacker->aivar[attackReason]==none) && !npc_isplayer(attacker)) {
    startNpcAttack(self,attacker,none,0);
    return;
    }

  const auto closeCombat = npc_isinfightmode(attacker,int(FightMode::FMODE_NONE)) ||
                           npc_isinfightmode(attacker,int(FightMode::FMODE_FIST)) ||
                           npc_isinfightmode(attacker,int(FightMode::FMODE_MELEE));
  if(closeCombat) {
    const bool playerFriend = npc_isplayer(attacker) && self->type==NpcType::G2_FRIEND;
    const bool friendly = npc_getattitude(self,attacker)==ATT_FRIENDLY;
    const auto warn = findFunction("ZS_REACTTODAMAGE");
    if((playerFriend || friendly) && !npc_isinstate(self,int(warn.ptr))) {
      startHumanWarning(self,warn);
      return;
      }
    }
  startNpcAttack(self,attacker,findSymbol("AR_REACTTODAMAGE")->get_int(),0);
  }

int32_t GameScript::runEnemyReaction() {
  constexpr size_t partyMember = 15; // AIV_PARTYMEMBER
  constexpr size_t followInWater = 28; // AIV_MM_FOLLOWINWATER
  constexpr size_t lastPlayerAttackReason = 47; // AIV_LASTPLAYERAR
  constexpr size_t enemyOverride = 61; // AIV_ENEMYOVERRIDE
  constexpr size_t noFightParker = 71; // AIV_NOFIGHTPARKER
  const auto self = std::dynamic_pointer_cast<daedalus::INpc>(vm.global_self()->get_instance());
  const auto other = std::dynamic_pointer_cast<daedalus::INpc>(vm.global_other()->get_instance());
  const auto actorId = [&](std::string_view instance, std::string_view scratch) {
    const auto actor = hlp_getnpc(int(findSymbolIndex(instance)));
    findSymbol(scratch)->set_instance(actor);
    return hlp_getinstanceid(actor);
    };
  const auto inspector = actorId("PC_LEVELINSPEKTOR","B_ASSESSENEMY.PCL");
  const auto otherId = hlp_getinstanceid(other);
  if(otherId==inspector || self==nullptr)
    return 0;
  const auto otherGuild = other ? int32_t(other->guild) : 0;
  const auto humanSeparator = findSymbol("GIL_SEPERATOR_HUM")->get_int();
  const bool otherParker = other && other->aivar[noFightParker]==1;
  if((otherParker && otherGuild>humanSeparator) ||
     ((otherParker || self->aivar[noFightParker]==1) && otherGuild<humanSeparator &&
      otherId!=hlp_getinstanceid(vm.global_hero()->get_instance())))
    return 0;

  const bool diving = vm.call_function<int32_t>("C_BODYSTATECONTAINS",other,int32_t(BS_DIVE))!=0;
  const bool swimming = vm.call_function<int32_t>("C_BODYSTATECONTAINS",other,int32_t(BS_SWIM))!=0;
  if((self->aivar[followInWater]==0 && (diving || swimming)) ||
     npc_getheighttonpc(self,other)>findSymbol("PERC_DIST_HEIGHT")->get_int())
    return 0;
  if(self->aivar[partyMember]==1 && (npc_getdisttonpc(self,other)>1500 || !npc_canseenpc(self,other)))
    return 0;
  const bool fakeBandit = isPlayerFakeBandit(self,other);
  if(self->guild==GIL_BDT && fakeBandit)
    return 0;

  const auto golem = actorId("MAGICGOLEM","B_ASSESSENEMY.MGO");
  const auto lares = actorId("VLK_449_LARES","B_ASSESSENEMY.LAR");
  if(otherId==golem && hlp_getinstanceid(self)==lares)
    return 0;
  const bool otherOverridesEnemy = other && other->aivar[enemyOverride]==1;
  if(otherGuild<humanSeparator && (otherOverridesEnemy || self->aivar[enemyOverride]==1))
    return 0;
  if(npc_getattitude(self,other)!=ATT_HOSTILE || isGateGuard(self) ||
     (self->type==NpcType::G2_FRIEND && npc_isplayer(other)))
    return 0;

  if(wld_getguildattitude(int32_t(self->guild),otherGuild)==ATT_HOSTILE) {
    startNpcAttack(self,other,findSymbol("AR_GUILDENEMY")->get_int(),0);
    return 1;
    }
  const bool near = npc_getdisttonpc(self,other)<=findSymbol("PERC_DIST_INTERMEDIAT")->get_int();
  const bool observing = npc_isinstate(self,int(findFunction("ZS_OBSERVEPLAYER").ptr));
  const bool waited = npc_getstatetime(self)>2;
  if(near && (observing || waited) && npc_getattitude(self,other)==ATT_HOSTILE) {
    startNpcAttack(self,other,self->aivar[lastPlayerAttackReason],0);
    return 1;
    }
  return 0;
  }

void GameScript::runPropertyCrime(bool usingMob) {
  const auto self = std::dynamic_pointer_cast<daedalus::INpc>(vm.global_self()->get_instance());
  const auto other = [&] { return std::dynamic_pointer_cast<daedalus::INpc>(vm.global_other()->get_instance()); };
  const auto item = [&] { return std::dynamic_pointer_cast<daedalus::IItem>(vm.global_item()->get_instance()); };
  const auto culprit = other();
  if(!npc_isplayer(culprit) || self==nullptr)
    return;
  const bool indoors = wld_getplayerportalguild()>=int32_t(GIL_NONE);
  if(indoors && npc_getheighttonpc(self,culprit)>findSymbol("PERC_DIST_INDOOR_HEIGHT")->get_int())
    return;
  if(wld_getguildattitude(int32_t(self->guild),int32_t(culprit->guild))==ATT_FRIENDLY) {
    const bool personallyOwned = usingMob ? npc_isdetectedmobownedbynpc(culprit,self)
                                          : npc_ownedbynpc(item(),self) && hlp_isvaliditem(item());
    if(!personallyOwned)
      return;
    }
  const bool possession = usingMob ? vm.call_function<int32_t>("C_ISUSEDMOBMYPOSSESSION",self,other())!=0
                                   : vm.call_function<int32_t>("C_ISTAKENITEMMYPOSSESSION",self,other(),item())!=0;
  if(!possession)
    return;
  if(!npc_canseenpc(self,other())) {
    const bool supervising = npc_isinstate(self,int(findFunction("ZS_CLEARROOM").ptr)) ||
                             npc_isinstate(self,int(findFunction("ZS_OBSERVEPLAYER").ptr));
    if(!supervising || !npc_isinplayersroom(self))
      return;
    }
  if(usingMob) {
    const auto scheme = npc_getdetectedmob(other());
    findSymbol("B_ASSESSUSEMOB.DETMOB")->set_string(scheme);
    if(scheme!="CHESTSMALL" && scheme!="CHESTBIG")
      return;
    }
  if(vm.call_function<int32_t>("C_WANTTOATTACKTHIEF",self,other())==0) {
    if(isGateGuard(self))
      memorizePlayerCrime(self,other(),findSymbol("CRIME_THEFT")->get_int());
    return;
    }
  const auto reason = findSymbol(usingMob ? "AR_USEMOB" : "AR_THEFT")->get_int();
  startNpcAttack(self,other(),reason,0);
  }

void GameScript::runPlayerAssessment() {
  constexpr size_t startedTalk = 3; // AIV_NPCSTARTEDTALK
  constexpr size_t invincible = 4; // AIV_INVINCIBLE
  constexpr size_t guardPassageStatus = 12; // AIV_GUARDPASSAGE_STATUS
  constexpr size_t partyMember = 15; // AIV_PARTYMEMBER
  const auto self = std::dynamic_pointer_cast<daedalus::INpc>(vm.global_self()->get_instance());
  const auto other = [&] { return std::dynamic_pointer_cast<daedalus::INpc>(vm.global_other()->get_instance()); };
  const auto constant = [&](std::string_view name) { return findSymbol(name)->get_int(); };
  const auto distance = [&] { return npc_getdisttonpc(self,other()); };
  const auto hasBodyState = [&](const std::shared_ptr<daedalus::INpc>& actor, int32_t state) {
    return vm.call_function<int32_t>("C_BODYSTATECONTAINS",actor,state)!=0;
    };
  const auto fakeGuild = [&] { return vm.call_function<int32_t>("C_PLAYERHASFAKEGUILD",self,other())!=0; };
  const auto assessTalk = [&] {
    self->aivar[startedTalk] = 1;
    vm.call_function<void>("B_ASSESSTALK");
    };

  const auto inspector = hlp_getnpc(int(findSymbolIndex("PC_LEVELINSPEKTOR")));
  findSymbol("B_ASSESSPLAYER.PCL")->set_instance(inspector);
  const auto source = other();
  if(hlp_getinstanceid(source)==hlp_getinstanceid(inspector) || self==nullptr || (source && source->aivar[invincible]==1) ||
     vm.call_function<int32_t>("C_NPCISDOWN",source)!=0)
    return;

  const auto otherGuild = source ? int32_t(source->guild) : 0;
  if(otherGuild>constant("GIL_SEPERATOR_HUM")) {
    if(isGateGuard(self)) {
      ai_standupquick(self);
      startNpcAttack(self,other(),constant("AR_MONSTERCLOSETOGATE"),0);
      return;
      }
    if(wld_getguildattitude(int32_t(self->guild),otherGuild)==ATT_HOSTILE &&
       self->type!=NpcType::G2_FRIEND && self->aivar[partyMember]==0) {
      startNpcAttack(self,other(),constant("AR_GUILDENEMY"),0);
      return;
      }
    }
  if(runEnemyReaction()!=0)
    return;
  const bool fakeBandit = isPlayerFakeBandit(self,other());
  if(self->guild!=GIL_BDT && fakeBandit)
    startNpcAttack(self,other(),constant("AR_GUILDENEMY"),0);

  const bool near = distance()<=constant("PERC_DIST_INTERMEDIAT");
  const bool pursueMurderer = vm.call_function<int32_t>("C_WANTTOATTACKMURDER",self,other())!=0;
  const bool murdered = playerCrime(self)==constant("CRIME_MURDER");
  if(near && pursueMurderer && murdered) {
    startNpcAttack(self,other(),constant("AR_HUMANMURDEREDHUMAN"),0);
    return;
    }
  if(runEnterRoomReaction()!=0 || runDrawWeaponReaction()!=0)
    return;
  findSymbol("PLAYER_DRAWWEAPONCOMMENT")->set_int(0);

  if(hasBodyState(other(),BS_SNEAK)) {
    const bool react = vm.call_function<int32_t>("C_WANTTOREACTTOSNEAKER",self,other())!=0;
    const auto observe = findFunction("ZS_OBSERVEPLAYER");
    if(react && !npc_isinstate(self,int(observe.ptr))) {
      npc_clearaiqueue(self);
      clearPerceptions(self);
      ai_startstate(self,int(observe.ptr),1,"");
      return;
      }
    }
  else if(hasBodyState(other(),BS_STAND)) {
    findSymbol("PLAYER_SNEAKERCOMMENT")->set_int(0);
    }
  if(!hasBodyState(other(),BS_LIE))
    findSymbol("PLAYER_GETOUTOFMYBEDCOMMENT")->set_int(0);

  vm.call_function<void>("B_ASSIGNDEMENTORTALK",self);
  const auto talkDistance = constant("PERC_DIST_DIALOG");
  if(npc_checkinfo(self,1) && distance()<=talkDistance) {
    if(isGateGuard(self)) {
      assessTalk();
      return;
      }
    const bool disguise = fakeGuild();
    const bool refuse = vm.call_function<int32_t>("C_REFUSETALK",self,other())!=0;
    const bool hasCrime = playerCrime(self)!=constant("CRIME_NONE");
    const bool diving = hasBodyState(other(),BS_DIVE);
    const bool swimming = hasBodyState(other(),BS_SWIM);
    const bool falling = hasBodyState(other(),BS_FALL);
    if(!disguise && !refuse && !hasCrime && !diving && !swimming && !falling) {
      assessTalk();
      return;
      }
    }
  const bool disguise = fakeGuild();
  const bool gateGuard = isGateGuard(self);
  const bool refuse = npc_refusetalk(other());
  const bool withinGreetingRange = distance()<=talkDistance;
  const bool walking = hasBodyState(self,BS_WALK);
  if(!disguise && !gateGuard && !refuse && withinGreetingRange && walking) {
    lookAtNpc(self,other());
    vm.call_function<void>("B_SAY_GUILDGREETINGS",self,other());
    stopLookAtNpc(self);
    npc_setrefusetalk(other(),20);
    }
  const bool outsideTalkRange = distance()>talkDistance;
  if(isGateGuard(self) && outsideTalkRange)
    self->aivar[guardPassageStatus] = constant("GP_NONE");
  }

void GameScript::stopControlSpell(const std::shared_ptr<daedalus::INpc>& self, bool continueRoutine) {
  npc_percenable(self,PERC_ASSESSMAGIC,int(findFunction("B_ASSESSMAGIC").ptr));
  npc_clearaiqueue(self);
  ai_standup(self);
  if(int32_t(self->guild)<findSymbol("GIL_SEPERATOR_HUM")->get_int())
    runDamageReaction();
  else
    npc_settempattitude(self,ATT_HOSTILE);
  if(continueRoutine)
    ai_continueroutine(self);
  }

bool GameScript::runControlSpell(Npc& npc, const daedalus::DaedalusSymbol& symbol, int& result) {
  const auto self = std::static_pointer_cast<daedalus::INpc>(npc.handlePtr());
  const std::string_view name = symbol.name();
  result = 0;
  if(name=="ZS_GREENTENTACLE") {
    npc_percenable(self,PERC_ASSESSMAGIC,int(findFunction("B_RESTARTGREENTENTACLE").ptr));
    npc_percenable(self,PERC_ASSESSDAMAGE,int(damageCallback.ptr));
    npc_stopani(self,"s_GreententacleA_Victim");
    npc_stopani(self,"s_GreententacleB_Victim");
    npc_stopani(self,"s_GreententacleC_Victim");
    npc_clearaiqueue(self);
    ai_standup(self);
    if(vm.call_function<int32_t>("C_BODYSTATECONTAINS",self,int32_t(BS_UNCONSCIOUS))!=0)
      return true;
    if(int32_t(self->guild)<findSymbol("GIL_SEPERATOR_HUM")->get_int()) {
      static constexpr std::string_view humanPoses[] = {
        "T_STAND_2_GREENTENTACLEA_VICTIM", "T_STAND_2_GREENTENTACLEB_VICTIM", "T_STAND_2_GREENTENTACLEC_VICTIM",
        };
      const auto pose = findSymbol("ZS_GREENTENTACLE.RANDY")->get_int();
      if(size_t(pose)<std::size(humanPoses))
        ai_playanibs(self,humanPoses[size_t(pose)],BS_UNCONSCIOUS);
      } else {
      ai_playanibs(self,"T_STAND_2_FREEZE_VICTIM",BS_UNCONSCIOUS);
      }
    return true;
    }
  if(name=="ZS_GREENTENTACLE_LOOP") {
    result = LOOP_CONTINUE;
    if(npc_getstatetime(self)>findSymbol("SPL_TIME_GREENTENTACLE")->get_int()) {
      stopControlSpell(self,false);
      result = LOOP_END;
      }
    return true;
    }
  if(name=="ZS_SHORTZAPPED") {
    npc_percenable(self,PERC_ASSESSSTOPMAGIC,int(stopShortZapCallback.ptr));
    if(npc_hasbodyflag(self,findSymbol("BS_FLAG_INTERRUPTABLE")->get_int()))
      ai_standupquick(self);
    else
      ai_standup(self);
    if(int32_t(self->guild)<findSymbol("GIL_SEPERATOR_HUM")->get_int())
      ai_playani(self,"T_STAND_2_LIGHTNING_VICTIM");
    return true;
    }
  if(name=="ZS_SHORTZAPPED_LOOP") {
    if(npc_getstatetime(self)>findSymbol("SPL_TIME_SHORTZAPPED")->get_int())
      stopControlSpell(self,true);
    return true;
    }
  return name=="ZS_GREENTENTACLE_END" || name=="ZS_SHORTZAPPED_END";
  }

void GameScript::validateCombatTarget(const std::shared_ptr<daedalus::INpc>& self) {
  constexpr size_t lastTarget = 7; // AIV_LASTTARGET
  constexpr size_t loadedGame = 65; // AIV_LOADGAME
  if(self==nullptr)
    return;
  auto& other = *vm.global_other();
  if(!hlp_isvalidnpc(std::dynamic_pointer_cast<daedalus::INpc>(other.get_instance()))) {
    const auto savedTarget = hlp_getnpc(self->aivar[lastTarget]);
    other.set_instance(savedTarget);
    npc_settarget(self,savedTarget);
    self->aivar[loadedGame] = 1;
    } else {
    npc_gettarget(self);
    self->aivar[lastTarget] = hlp_getinstanceid(other.get_instance());
    self->aivar[loadedGame] = 0;
    }
  }

bool GameScript::runHumanFlee(Npc& npc, const daedalus::DaedalusSymbol& symbol, int& result) {
  constexpr size_t loadedGame = 65; // AIV_LOADGAME
  constexpr std::string_view fleeOverlay = "HUMANS_FLEE.MDS";
  const auto self = std::static_pointer_cast<daedalus::INpc>(npc.handlePtr());
  const std::string_view name = symbol.name();
  if(name=="ZS_FLEE") {
    npc_percenable(self,PERC_ASSESSMAGIC,int(findFunction("B_ASSESSMAGIC").ptr));
    validateCombatTarget(self);
    if(self->aivar[loadedGame]==0)
      vm.call_function<void>("B_SAY_OVERLAY",self,vm.global_other()->get_instance(),std::string_view("$RUNAWAY"));
    ai_removeweapon(self);
    ai_setwalkmode(self,findSymbol("NPC_RUN")->get_int());
    mdl_applyoverlaymds(self,fleeOverlay);
    result = 0;
    return true;
    }
  if(name=="ZS_FLEE_LOOP") {
    npc_gettarget(self);
    const auto target = std::dynamic_pointer_cast<daedalus::INpc>(vm.global_other()->get_instance());
    if(npc_getdisttonpc(self,target)>findSymbol("FIGHT_DIST_CANCEL")->get_int() ||
       vm.call_function<int32_t>("C_NPCISDOWN",target)!=0) {
      npc_clearaiqueue(self);
      result = LOOP_END;
      } else {
      ai_flee(self);
      result = LOOP_CONTINUE;
      }
    return true;
    }
  if(name=="ZS_FLEE_END") {
    mdl_removeoverlaymds(self,fleeOverlay);
    ai_standup(self);
    ai_startstate(self,int(findFunction("ZS_HEALSELF").ptr),1,"");
    result = 0;
    return true;
    }
  return false;
  }

bool GameScript::runWatchFight(Npc& npc, const daedalus::DaedalusSymbol& symbol, int& result) {
  constexpr size_t inPosition = 19; // AIV_TAPOSITION
  constexpr size_t lastTurn = 68; // AIV_STATETIME
  const auto self = std::static_pointer_cast<daedalus::INpc>(npc.handlePtr());
  const auto other = [&] { return std::dynamic_pointer_cast<daedalus::INpc>(vm.global_other()->get_instance()); };
  const auto victim = [&] { return std::dynamic_pointer_cast<daedalus::INpc>(vm.global_victim()->get_instance()); };
  const auto constant = [&](std::string_view name) { return findSymbol(name)->get_int(); };
  const auto guild = [](const std::shared_ptr<daedalus::INpc>& actor) { return actor ? int32_t(actor->guild) : 0; };
  const auto turn = [&](const std::shared_ptr<daedalus::INpc>& actor) {
    vm.call_function<void>("B_TURNTONPC",self,actor);
    };
  auto& aivar = self->aivar;
  const std::string_view name = symbol.name();
  if(name=="ZS_WATCHFIGHT") {
    npc_percenable(self,PERC_ASSESSDEFEAT,int(findFunction("B_ASSESSDEFEAT").ptr));
    npc_percenable(self,PERC_ASSESSOTHERSDAMAGE,int(findFunction("B_CHEERFIGHT").ptr));
    setPerceptionPreset(false);
    ai_standup(self);
    turn(victim());
    ai_removeweapon(self);
    const auto separator = constant("GIL_SEPERATOR_HUM");
    const auto distance = constant("PERC_DIST_INTERMEDIAT");
    if(guild(victim())<separator && guild(other())<separator &&
       !npc_isinstate(victim(),int(ZS_Unconscious)) && !npc_isinstate(other(),int(ZS_Unconscious)) &&
       (npc_getdisttonpc(self,victim())<distance || npc_getdisttonpc(self,other())<distance)) {
      const bool tough = vm.call_function<int32_t>("C_NPCISTOUGHGUY",self)!=0;
      const std::string_view line = tough ? "$THERESAFIGHT" : "$OHMYGODITSAFIGHT";
      vm.call_function<void>("B_SAY",self,other(),line);
      }
    aivar[inPosition] = constant("NOTINPOS");
    aivar[lastTurn] = 0;
    result = 0;
    return true;
    }
  if(name=="ZS_WATCHFIGHT_LOOP") {
    const auto outerRange = constant("WATCHFIGHT_DIST_MAX");
    if(npc_getdisttonpc(self,victim())>outerRange && npc_getdisttonpc(self,other())>outerRange) {
      npc_clearaiqueue(self);
      result = LOOP_END;
      return true;
      }
    const auto react = int(findFunction("ZS_REACTTODAMAGE").ptr);
    const auto stillFighting = [&](const std::shared_ptr<daedalus::INpc>& actor) {
      return npc_isinstate(actor,react) || npc_isinstate(actor,int(ZS_Attack));
      };
    if(npc_getstatetime(self)>0 && !stillFighting(victim()) && !stillFighting(other())) {
      const auto down = [&](const std::shared_ptr<daedalus::INpc>& actor) {
        return npc_isinstate(actor,int(ZS_Dead)) || npc_isinstate(actor,int(ZS_Unconscious));
        };
      if((down(victim()) || down(other())) && aivar[inPosition]==constant("NOTINPOS")) {
        aivar[inPosition] = constant("ISINPOS");
        result = LOOP_CONTINUE;
        } else {
        npc_clearaiqueue(self);
        result = LOOP_END;
        }
      return true;
      }
    // The stock loop still evaluates this policy on the continuing path.
    vm.call_function<int32_t>("C_NPCISTOUGHGUY",self);
    const auto innerRange = constant("WATCHFIGHT_DIST_MIN");
    if(npc_getdisttonpc(self,victim())<=innerRange || npc_getdisttonpc(self,other())<=innerRange) {
      npc_clearaiqueue(self);
      const bool otherIsCloser = npc_getdisttonpc(self,other())<=npc_getdisttonpc(self,victim());
      turn(otherIsCloser ? victim() : other());
      ai_dodge(self);
      }
    else if(npc_getstatetime(self)!=aivar[lastTurn]) {
      const bool otherIsCloser = npc_getdisttonpc(self,other())<=npc_getdisttonpc(self,victim());
      turn(otherIsCloser ? other() : victim());
      aivar[lastTurn] = npc_getstatetime(self);
      }
    result = LOOP_CONTINUE;
    return true;
    }
  if(name=="ZS_WATCHFIGHT_END") {
    result = 0;
    return true;
    }
  return false;
  }

bool GameScript::runFakeGuildComment(Npc& npc, const daedalus::DaedalusSymbol& symbol, int& result) {
  if(symbol.name()!="ZS_COMMENTFAKEGUILD")
    return false;
  const auto self = std::static_pointer_cast<daedalus::INpc>(npc.handlePtr());
  const auto other = [&] { return std::dynamic_pointer_cast<daedalus::INpc>(vm.global_other()->get_instance()); };
  const auto seated = [&](const std::shared_ptr<daedalus::INpc>& actor) {
    return vm.call_function<int32_t>("C_BODYSTATECONTAINS",actor,int32_t(BS_SIT))!=0;
    };
  setPerceptionPreset(false);
  ai_standup(self);
  if(!seated(self))
    vm.call_function<void>("B_TURNTONPC",self,other());
  if(!seated(other())) {
    vm.call_function<void>("B_TURNTONPC",other(),self);
    if(npc_getdisttonpc(other(),self)<80)
      ai_dodge(other());
    }

  std::string_view line;
  if(npc_hasequippedarmor(other())) {
    findSymbol("ZS_COMMENTFAKEGUILD.ITM")->set_instance(npc_getequippedarmor(other()));
    switch(self->guild) {
      case GIL_PAL: case GIL_MIL: line = "$ADDON_WRONGARMOR_MIL"; break;
      case GIL_KDF: case GIL_NOV: line = "$ADDON_WRONGARMOR_KDF"; break;
      case GIL_DJG: case GIL_SLD: line = "$ADDON_WRONGARMOR_SLD"; break;
      default: line = "$ADDON_WRONGARMOR"; break;
      }
    } else if(self->guild==GIL_BDT) {
    line = "$ADDON_NOARMOR_BDT";
    }
  if(!line.empty())
    vm.call_function<void>("B_SAY",self,other(),line);
  result = 0;
  return true;
  }

bool GameScript::runHumanPlunder(Npc& npc, const daedalus::DaedalusSymbol& symbol, int& result) {
  const auto self = std::static_pointer_cast<daedalus::INpc>(npc.handlePtr());
  const auto other = [&] { return std::dynamic_pointer_cast<daedalus::INpc>(vm.global_other()->get_instance()); };
  const auto transfer = [&](int item, int amount) {
    createinvitems(self,item,amount);
    npc_removeinvitems(other(),item,amount);
    };
  const auto heal = [&] {
    if(self->attribute[NpcAttribute::HITPOINTS]<self->attribute[NpcAttribute::HITPOINTS_MAX]/2)
      ai_startstate(self,int(findFunction("ZS_HEALSELF").ptr),0,"");
    };
  const std::string_view name = symbol.name();
  if(name=="ZS_GETMEAT") {
    setPerceptionPreset(false);
    ai_standup(self);
    ai_gotonpc(self,other());
    ai_turntonpc(self,other());
    ai_playani(self,"T_PLUNDER");
    const auto meat = int(findSymbolIndex("ITFOMUTTONRAW"));
    const auto amount = npc_hasitems(other(),meat);
    findSymbol("ZS_GETMEAT.X")->set_int(amount);
    transfer(meat,amount);
    heal();
    result = 0;
    return true;
    }
  if(name=="ZS_RANSACKBODY") {
    setPerceptionPreset(true);
    ai_standup(self);
    ai_gotonpc(self,other());
    result = 0;
    return true;
    }
  if(name=="ZS_RANSACKBODY_LOOP") {
    result = LOOP_END;
    return true;
    }
  if(name!="ZS_RANSACKBODY_END")
    return false;

  vm.call_function<void>("B_TURNTONPC",self,other());
  ai_playani(self,"T_PLUNDER");
  const auto questStock = [&](std::string_view owner, int item) {
    const auto ownerId = hlp_getinstanceid(findSymbol(owner)->get_instance());
    const auto amount = npc_hasitems(other(),item);
    return ownerId==hlp_getinstanceid(self) ? amount : 0;
    };
  const auto hammer = int(findSymbolIndex("HOLY_HAMMER_MIS"));
  const auto sword = int(findSymbolIndex("ITMW_2H_ROD"));
  const auto key = int(findSymbolIndex("ITKE_GREG_ADDON_MIS"));
  if(questStock("GARWIG",hammer)>0)
    transfer(hammer,1);
  if(questStock("ROD",sword)>0) {
    transfer(sword,1);
    ai_equipbestmeleeweapon(self);
    }
  if(questStock("FRANCIS",key)!=0)
    transfer(key,1);

  const auto gold = int(findSymbolIndex("ITMI_GOLD"));
  const auto amount = npc_hasitems(other(),gold);
  if(amount>0) {
    findSymbol("ZS_RANSACKBODY_END.X")->set_int(amount);
    transfer(gold,amount);
    }
  const std::string_view line = amount>0 ? "$ITOOKYOURGOLD" : "$SHITNOGOLD";
  vm.call_function<void>("B_SAY",self,other(),line);

  npc_perceiveall(self);
  // Both scans can update ITEM. Melee weapons take precedence when both find a candidate.
  const bool foundRanged = wld_detectitem(self,findSymbol("ITEM_KAT_FF")->get_int());
  const bool foundMelee = wld_detectitem(self,findSymbol("ITEM_KAT_NF")->get_int());
  if(foundRanged || foundMelee) {
    const auto item = std::dynamic_pointer_cast<daedalus::IItem>(vm.global_item()->get_instance());
    if(hlp_isvaliditem(item) && npc_getdisttoitem(self,item)<500) {
      ai_takeitem(self,item);
      vm.call_function<void>("B_SAY",self,self,std::string_view("$ITAKEYOURWEAPON"));
      ai_equipbestmeleeweapon(self);
      ai_equipbestrangedweapon(self);
      }
    }
  heal();
  result = 0;
  return true;
  }

bool GameScript::runHumanAttack(Npc& npc, const daedalus::DaedalusSymbol& symbol, int& result) {
  constexpr size_t lastFightAgainstPlayer = 0; // AIV_LASTFIGHTAGAINSTPLAYER
  constexpr size_t invincible = 4; // AIV_INVINCIBLE
  constexpr size_t lastTarget = 7; // AIV_LASTTARGET
  constexpr size_t pursuitEnded = 8; // AIV_PURSUITEND
  constexpr size_t attackReason = 9; // AIV_ATTACKREASON
  constexpr size_t ransacked = 10; // AIV_RANSACKED
  constexpr size_t guardPassageStatus = 12; // AIV_GUARDPASSAGE_STATUS
  constexpr size_t guardCalls = 19; // AIV_TAPOSITION
  constexpr size_t selectedSpell = 20; // AIV_SELECTSPELL
  constexpr size_t hitByOther = 22; // AIV_HITBYOTHERNPC
  constexpr size_t waitBeforeAttack = 23; // AIV_WAITBEFOREATTACK
  constexpr size_t lastAbsolution = 24; // AIV_LASTABSOLUTIONLEVEL
  constexpr size_t followTime = 27; // AIV_MM_FOLLOWTIME
  constexpr size_t followInWater = 28; // AIV_MM_FOLLOWINWATER
  constexpr size_t arenaFight = 45; // AIV_ARENAFIGHT
  constexpr size_t lastPlayerAttackReason = 47; // AIV_LASTPLAYERAR
  constexpr size_t maxDistanceToWayPoint = 59; // AIV_MAXDISTTOWP
  constexpr size_t originalTactic = 60; // AIV_ORIGINALFIGHTTACTIC
  constexpr size_t fightDistanceCancel = 63; // AIV_FIGHTDISTCANCEL
  constexpr size_t lastFightComment = 64; // AIV_LASTFIGHTCOMMENT
  constexpr size_t loadedGame = 65; // AIV_LOADGAME
  constexpr size_t killedByPlayer = 67; // AIV_KILLEDBYPLAYER
  constexpr size_t stateTime = 68; // AIV_STATETIME
  constexpr size_t pursuitDistance = 69; // AIV_DIST

  const auto self = std::static_pointer_cast<daedalus::INpc>(npc.handlePtr());
  const auto other = [&] { return std::dynamic_pointer_cast<daedalus::INpc>(vm.global_other()->get_instance()); };
  const auto hero = [&] { return std::dynamic_pointer_cast<daedalus::INpc>(vm.global_hero()->get_instance()); };
  const auto constant = [&](std::string_view name) { return findSymbol(name)->get_int(); };
  const auto absolution = [&] { return currentAbsolutionLevel(self); };
  const auto bodyState = [&](int32_t body) { return vm.call_function<int32_t>("C_BODYSTATECONTAINS",other(),body)!=0; };
  const auto targetDown = [&] { return vm.call_function<int32_t>("C_NPCISDOWN",other())!=0; };
  const auto stopPursuit = [&] {
    npc_clearaiqueue(self);
    ai_standup(self);
    };
  const auto turnToTarget = [&] { turnToNpc(self,other()); };
  const auto say = [&](std::string_view line) { sayToNpc(self,other(),line); };
  const auto start = [&](std::string_view state) { ai_startstate(self,int(findFunction(state).ptr),0,""); };
  const auto targetValue = [&](size_t slot) { const auto target = other(); return target ? target->aivar[slot] : 0; };
  const auto guild = [](const std::shared_ptr<daedalus::INpc>& actor) { return actor ? int32_t(actor->guild) : 0; };
  auto& aivar = self->aivar;
  const std::string_view name = symbol.name();
  if(name=="ZS_ATTACK") {
    result = 0;
    setPerceptionPreset(false);
    npc_percenable(self,PERC_ASSESSSURPRISE,int(findFunction("B_ASSESSSURPRISE").ptr));
    validateCombatTarget(self);
    aivar[lastTarget] = hlp_getinstanceid(other());
    if(vm.call_function<int32_t>("C_WANTTOFLEE",self,other())!=0) {
      npc_clearaiqueue(self);
      clearPerceptions(self);
      npc_settarget(self,other());
      start("ZS_FLEE");
      return true;
      }
    if(aivar[loadedGame]==0)
      vm.call_function<void>("B_SAY_ATTACKREASON");
    if(npc_isinfightmode(self,int(FightMode::FMODE_NONE))) {
      ai_equipbestrangedweapon(self);
      ai_equipbestmeleeweapon(self);
      }
    ai_standup(self);
    stopLookAtNpc(self);
    turnToTarget();
    ai_setwalkmode(self,constant("NPC_RUN"));
    aivar[guardPassageStatus] = constant("GP_NONE");
    aivar[lastAbsolution] = absolution();
    aivar[pursuitEnded] = 0;
    aivar[stateTime] = 0;
    aivar[guardCalls] = 0;
    aivar[hitByOther] = 0;
    aivar[selectedSpell] = 0;
    return true;
    }

  if(name=="ZS_ATTACK_LOOP") {
    result = LOOP_CONTINUE;
    vm.call_function<void>("B_GREG_COMESTODEXTER");
    npc_gettarget(self);
    if(npc_getdisttonpc(self,other())>aivar[fightDistanceCancel]) {
      stopPursuit();
      aivar[pursuitEnded] = 1;
      result = LOOP_END;
      return true;
      }
    if(aivar[pursuitEnded]==0 && npc_getstatetime(self)>aivar[followTime]) {
      stopPursuit();
      aivar[pursuitEnded] = 1;
      aivar[pursuitDistance] = npc_getdisttonpc(self,other());
      aivar[stateTime] = npc_getstatetime(self);
      if(guild(other())<constant("GIL_SEPERATOR_HUM"))
        say("$RUNCOWARD");
      }
    if(aivar[pursuitEnded]==1) {
      if(npc_getdisttonpc(self,other())>self->senses_range) {
        result = LOOP_END;
        return true;
        }
      if(npc_getstatetime(self)>aivar[stateTime]) {
        const bool jumping = bodyState(BS_JUMP);
        const bool running = bodyState(BS_RUN);
        if((!jumping && !running) || npc_getdisttonpc(self,other())<aivar[pursuitDistance]) {
          aivar[pursuitEnded] = 0;
          npc_setstatetime(self,0);
          aivar[stateTime] = 0;
          } else {
          turnToTarget();
          aivar[pursuitDistance] = npc_getdisttonpc(self,other());
          aivar[stateTime] = npc_getstatetime(self);
          }
        }
      return true;
      }
    if(absolution()>aivar[lastAbsolution]) {
      stopPursuit();
      result = LOOP_END;
      return true;
      }
    const bool diving = bodyState(BS_DIVE);
    const bool swimming = bodyState(BS_SWIM);
    if(aivar[followInWater]==0 && (diving || swimming)) {
      stopPursuit();
      aivar[pursuitEnded] = 1;
      result = LOOP_END;
      return true;
      }
    if(aivar[waitBeforeAttack]>=1) {
      ai_wait(self,0.8f);
      aivar[waitBeforeAttack] = 0;
      }
    if(aivar[maxDistanceToWayPoint]>0) {
      const bool targetOutside = npc_getdisttowp(other(),self->wp)>aivar[maxDistanceToWayPoint];
      const bool selfOutside = npc_getdisttowp(self,self->wp)>aivar[maxDistanceToWayPoint];
      self->fight_tactic = targetOutside && selfOutside ? constant("FAI_NAILED") : aivar[originalTactic];
      }
    const bool jumping = bodyState(BS_JUMP);
    const bool running = bodyState(BS_RUN);
    if(!jumping && !running)
      npc_setstatetime(self,0);
    if(aivar[guardCalls]==0 && npc_getstatetime(self)>2) {
      vm.call_function<void>("B_CALLGUARDS");
      aivar[guardCalls] = 1;
      }
    if(aivar[guardCalls]==1 && npc_getstatetime(self)>8) {
      vm.call_function<void>("B_CALLGUARDS");
      aivar[guardCalls] = 2;
      }
    vm.call_function<void>("B_CREATEAMMO",self);
    vm.call_function<void>("B_SELECTWEAPON",self,other());
    if(!targetDown() && hlp_isvalidnpc(other())) {
      if(targetValue(invincible)==0)
        ai_attack(self);
      else
        npc_clearaiqueue(self);
      aivar[lastTarget] = hlp_getinstanceid(other());
      return true;
      }
    npc_clearaiqueue(self);
    if(targetDown() && npc_isplayer(other()) && hlp_isvalidnpc(other()))
      npc_settempattitude(self,npc_getpermattitude(self,hero()));
    if(aivar[attackReason]!=constant("AR_KILL")) {
      npc_perceiveall(self);
      npc_getnexttarget(self);
      }
    const bool fakeBandit = isPlayerFakeBandit(self,other());
    const bool vulnerable = targetValue(invincible)==0;
    const bool nearHeight = npc_getheighttonpc(self,other())<constant("PERC_DIST_HEIGHT");
    const bool nearDistance = npc_getdisttonpc(self,other())<constant("PERC_DIST_INTERMEDIAT");
    const bool player = npc_isplayer(other());
    const bool down = targetDown();
    if(!(self->guild==GIL_BDT && fakeBandit) && vulnerable && nearHeight && (player || nearDistance) &&
       !down && hlp_isvalidnpc(other())) {
      if(wld_getguildattitude(int(self->guild),guild(other()))==ATT_HOSTILE) {
        const auto reason = constant("AR_GUILDENEMY");
        aivar[attackReason] = reason;
        if(player) {
          aivar[lastPlayerAttackReason] = reason;
          aivar[lastFightAgainstPlayer] = constant("FIGHT_CANCEL");
          aivar[lastFightComment] = 0;
          }
        } else if(npc_getattitude(self,other())==ATT_HOSTILE) {
        aivar[attackReason] = aivar[lastPlayerAttackReason];
        }
      return true;
      }
    npc_clearaiqueue(self);
    if(aivar[lastTarget]!=hlp_getinstanceid(hero()) && aivar[lastFightAgainstPlayer]==constant("FIGHT_CANCEL"))
      aivar[lastFightComment] = 1;
    result = LOOP_END;
    return true;
    }

  if(name!="ZS_ATTACK_END")
    return false;
  result = 0;
  vm.global_other()->set_instance(hlp_getnpc(aivar[lastTarget]));
  if(aivar[pursuitEnded]==1) {
    if(self->type!=NpcType::G2_FRIEND && npc_isplayer(other()) && hlp_isvalidnpc(other()))
      npc_settempattitude(self,ATT_HOSTILE);
    if(aivar[arenaFight]==constant("AF_RUNNING"))
      aivar[arenaFight] = constant("AF_AFTER");
    }
  if(aivar[pursuitEnded]==0) {
    if(absolution()>aivar[lastAbsolution])
      say("$WISEMOVE");
    else
      vm.call_function<void>("B_SAY_ATTACKEND");
    }
  if(wld_getguildattitude(int(self->guild),guild(hero()))!=ATT_HOSTILE && targetValue(killedByPlayer)==1)
    vm.call_function<void>("B_SETATTITUDE",self,int(ATT_FRIENDLY));
  if(vm.call_function<int32_t>("C_NPCHASATTACKREASONTOKILL",self)!=0 && npc_isinstate(other(),int(ZS_Unconscious)))
    vm.call_function<void>("B_FINISHINGMOVE",self,other());
  ai_removeweapon(self);
  const bool near = npc_getdisttonpc(self,other())<constant("PERC_DIST_INTERMEDIAT");
  const bool alwaysRansack = vm.call_function<int32_t>("C_NPCRANSACKSALWAYS",self)!=0;
  const bool robbed = targetValue(ransacked)!=0;
  const bool wantToRansack = vm.call_function<int32_t>("C_WANTTORANSACK",self)!=0;
  const bool down = targetDown();
  if(near && (alwaysRansack || !robbed) && wantToRansack && down) {
    if(const auto target = other())
      target->aivar[ransacked] = 1;
    if(guild(other())<constant("GIL_SEPERATOR_HUM")) {
      start("ZS_RANSACKBODY");
      return true;
      }
    if(npc_hasitems(other(),int(findSymbolIndex("ITFOMUTTONRAW")))>0 &&
       hlp_getinstanceid(findSymbol("ALLIGATORJACK")->get_instance())==hlp_getinstanceid(self)) {
      start("ZS_GETMEAT");
      return true;
      }
    }
  if(self->attribute[NpcAttribute::HITPOINTS]<self->attribute[NpcAttribute::HITPOINTS_MAX]/2)
    start("ZS_HEALSELF");
  return true;
  }

bool GameScript::runHumanReaction(Npc& npc, const daedalus::DaedalusSymbol& symbol, int& result) {
  constexpr size_t lastFightAgainstPlayer = 0; // AIV_LASTFIGHTAGAINSTPLAYER
  constexpr size_t repeatedWarning = 19; // AIV_TAPOSITION
  constexpr size_t lastPlayerAttackReason = 47; // AIV_LASTPLAYERAR
  constexpr size_t turnTime = 68; // AIV_STATETIME
  const auto self = std::static_pointer_cast<daedalus::INpc>(npc.handlePtr());
  const auto other = [&] { return vm.global_other()->get_instance(); };
  const auto otherNpc = [&] { return std::dynamic_pointer_cast<daedalus::INpc>(other()); };
  const auto say = [&](std::string_view line) {
    sayToNpc(self,otherNpc(),line);
    };
  const auto turnToOther = [&] {
    turnToNpc(self,otherNpc());
    };
  const auto stopLook = [&] {
    stopLookAtNpc(self);
    };
  const auto inFightMode = [&](FightMode mode) {
    return npc_isinfightmode(otherNpc(),int(mode));
    };

  const std::string_view name = symbol.name();
  const bool weapon = name.starts_with("ZS_REACTTOWEAPON");
  auto& aivar = self->aivar;
  if(name=="ZS_REACTTODAMAGE" || name=="ZS_REACTTOWEAPON") {
    result = 0;
    if(weapon) {
      setPerceptionPreset(false);
      npc_percenable(self,PERC_ASSESSFIGHTSOUND,int(findFunction("B_ASSESSFIGHTSOUND").ptr));
      if(runEnemyReaction()!=0)
        return true;
      ai_standup(self);
      } else {
      setPerceptionPreset(true);
      }
    lookAtNpc(self,otherNpc());
    vm.call_function<void>("B_SELECTWEAPON",self,other());
    turnToOther();
    if(weapon) {
      auto& comment = *findSymbol("PLAYER_DRAWWEAPONCOMMENT");
      if(npc_isplayer(otherNpc()) && aivar[lastPlayerAttackReason]!=findSymbol("AR_NONE")->get_int() &&
         aivar[lastFightAgainstPlayer]==findSymbol("FIGHT_WON")->get_int()) {
        say("$LOOKINGFORTROUBLEAGAIN");
        }
      else if(comment.get_int()==0) {
        say(inFightMode(FightMode::FMODE_MAGIC) ? "$STOPMAGIC" : "$WEAPONDOWN");
        comment.set_int(1);
        }
      npc_sendpassiveperc(self,PERC_ASSESSWARN,self,otherNpc());
      aivar[repeatedWarning] = 0;
      } else {
      say("$WHATAREYOUDOING");
      }
    aivar[turnTime] = 0;
    return true;
    }

  if(name=="ZS_REACTTODAMAGE_LOOP" || name=="ZS_REACTTOWEAPON_LOOP") {
    if(weapon) {
      const bool farAway = npc_getdisttonpc(self,otherNpc())>findSymbol("PERC_DIST_INTERMEDIAT")->get_int();
      if(farAway || inFightMode(FightMode::FMODE_NONE)) {
        npc_clearaiqueue(self);
        if(!farAway)
          say("$WISEMOVE");
        ai_removeweapon(self);
        stopLook();
        result = LOOP_END;
        return true;
        }
      }
    if(npc_getstatetime(self)>aivar[turnTime]) {
      if(!npc_canseenpc(self,otherNpc())) {
        if(weapon)
          turnToOther();
        else
          ai_turntonpc(self,otherNpc());
        }
      ++aivar[turnTime];
      }
    if(weapon) {
      if(npc_getstatetime(self)>5 && aivar[repeatedWarning]==0) {
        say(inFightMode(FightMode::FMODE_MAGIC) ? "$ISAIDSTOPMAGIC" : "$ISAIDWEAPONDOWN");
        aivar[repeatedWarning] = 1;
        }
      if(npc_getstatetime(self)>10)
        startNpcAttack(self,otherNpc(),findSymbol("AR_REACTTOWEAPON")->get_int(),0);
      result = LOOP_CONTINUE;
      } else {
      result = npc_getstatetime(self)>10 ? LOOP_END : LOOP_CONTINUE;
      }
    return true;
    }

  if(name=="ZS_REACTTODAMAGE_END" || name=="ZS_REACTTOWEAPON_END") {
    if(!weapon)
      ai_removeweapon(self);
    stopLook();
    if(weapon)
      ai_startstate(self,int(findFunction("ZS_OBSERVEPLAYER").ptr),1,"");
    result = 0;
    return true;
    }
  return false;
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
  const bool wantsAttack = wantsToAttackRoomIntruder(self);
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
  const auto wantsAttack = [&] { return wantsToAttackRoomIntruder(self); };

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
        startNpcAttack(self,otherNpc(),reason,0);
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

void GameScript::loadNativeEquipment() {
  if(!nativeRoutineProgram)
    return;
  using NpcRef = std::shared_ptr<daedalus::INpc>;
  const auto enabled = [this] {
    return activityEvents && !floorDefer.active() && ActivityDefinitionRuntime::mode()!=ActivityDefinitionRuntime::Mode::Off;
    };
  vm.override_function("B_CREATEAMMO",std::function<void(NpcRef)>{[this](NpcRef self) {
    createAmmo(self);
    }},enabled);
  vm.override_function("B_SELECTWEAPON",std::function<void(NpcRef,NpcRef)>{[this](NpcRef self,NpcRef other) {
    selectWeapon(self,other);
    }},enabled);
  }

void GameScript::createAmmo(const std::shared_ptr<daedalus::INpc>& self) {
  findSymbol("B_CREATEAMMO.SLF")->set_instance(self);
  std::shared_ptr<daedalus::IItem> weapon;
  if(npc_isinfightmode(self,int32_t(FightMode::FMODE_FAR)))
    weapon = npc_getreadiedweapon(self);
  else if(npc_hasequippedrangedweapon(self))
    weapon = npc_getequippedrangedweapon(self);
  else
    return;
  findSymbol("B_CREATEAMMO.RANGEDWEAPON")->set_instance(weapon);
  if(!weapon)
    return;

  const auto ammo = weapon->munition;
  if((ammo==int32_t(findSymbolIndex("ITRW_ARROW")) || ammo==int32_t(findSymbolIndex("ITRW_BOLT"))) && npc_hasitems(self,ammo)<10)
    createinvitems(self,ammo,10);
  }

void GameScript::selectWeapon(const std::shared_ptr<daedalus::INpc>& self, const std::shared_ptr<daedalus::INpc>& other) {
  findSymbol("B_SELECTWEAPON.OTH")->set_instance(other);
  findSymbol("B_SELECTWEAPON.SLF")->set_instance(self);
  const auto body = npc_getbodystate(self);
  // All three stock checks run before branching, leaving the last helper arguments in its symbols.
  findSymbol("C_BODYSTATECONTAINS.SLF")->set_instance(self);
  findSymbol("C_BODYSTATECONTAINS.BODYSTATE")->set_int(BS_FALL);
  if(ActivityDefinitions::bodyStateContains(body,BS_DIVE) || ActivityDefinitions::bodyStateContains(body,BS_SWIM) ||
     ActivityDefinitions::bodyStateContains(body,BS_FALL))
    return;
  if(vm.call_function<int32_t>("B_SELECTSPELL",self,other)!=0)
    return;

  const auto mode = [&](FightMode value) { return npc_isinfightmode(self,int32_t(value)); };
  const auto ready = [&](bool ranged) {
    if(!mode(FightMode::FMODE_NONE))
      ai_removeweapon(self);
    if(ranged)
      ai_readyrangedweapon(self);
    else
      ai_readymeleeweapon(self);
    };
  if(mode(FightMode::FMODE_MAGIC)) {
    ready(false);
    return;
    }
  const auto distance = npc_getdisttonpc(self,other);
  const auto inner = findSymbol("FIGHT_DIST_RANGED_INNER")->get_int();
  const auto outer = findSymbol("FIGHT_DIST_RANGED_OUTER")->get_int();
  const bool melee = npc_hasequippedmeleeweapon(self);
  const bool ranged = npc_hasequippedrangedweapon(self);
  if(mode(FightMode::FMODE_FAR) && (!melee || distance>inner))
    return;
  if(mode(FightMode::FMODE_MELEE) && (!ranged || distance<=outer))
    return;
  if(distance<=outer && melee) {
    ready(false);
    return;
    }
  if(vm.call_function<int32_t>("C_NPCHASATTACKREASONTOKILL",self)!=0 && distance>inner && ranged) {
    ready(true);
    return;
    }
  if(mode(FightMode::FMODE_NONE))
    ready(!melee && ranged);
  }

bool GameScript::runDeadState(Npc& npc, const daedalus::DaedalusSymbol& symbol, int& result) {
  // These AIVAR slots belong to the verified NotR program.
  constexpr size_t sawCrime = 1;
  constexpr size_t ransacked = 10;
  constexpr size_t partyMember = 15;
  constexpr size_t victoryXpGiven = 16;
  constexpr size_t deathHandled = 19; // AIV_TAPOSITION
  constexpr size_t monsterId = 43;
  constexpr size_t killedByPlayer = 67;
  const auto self = std::static_pointer_cast<daedalus::INpc>(npc.handlePtr());

  if(symbol.name()=="ZS_DEAD_LOOP") {
    if(self->aivar[deathHandled]==0) {
      vm.push_reference(vm.global_self());
      vm.unsafe_call_bl(findSymbol("B_DRAGONKILLCOUNTER"));
      (void)vm.pop_int(); // The stock Loop ignores the helper's result.
      self->aivar[deathHandled] = 1;
      }
    result = LOOP_CONTINUE;
    return true;
    }
  if(symbol.name()!="ZS_DEAD")
    return false;

  const auto other = [&] { return std::dynamic_pointer_cast<daedalus::INpc>(vm.global_other()->get_instance()); };
  const auto namedNpc = [&](std::string_view name) {
    return std::dynamic_pointer_cast<daedalus::INpc>(findSymbol(name)->get_instance());
    };
  const auto matches = [&](std::string_view name) {
    return hlp_getinstanceid(self)==hlp_getinstanceid(namedNpc(name));
    };
  const auto number = [&](std::string_view name) { return findSymbol(name)->get_int(); };
  const auto setNumber = [&](std::string_view name, int32_t value) { findSymbol(name)->set_int(value); };
  const auto increment = [&](std::string_view name) { setNumber(name,number(name)+1); };

  self->aivar[ransacked] = 0;
  self->aivar[partyMember] = 0;
  vm.call_function<void>("B_STOPLOOKAT",self);
  ai_stoppointat(self);

  const auto winner = other();
  if(self->aivar[victoryXpGiven]==0 && (npc_isplayer(winner) || (winner && winner->aivar[partyMember]==1))) {
    vm.call_function<void>("B_GIVEPLAYERXP",self->level*number("XP_PER_VICTORY"));
    self->aivar[victoryXpGiven] = 1;
    }

  if(vm.call_function<int32_t>("C_IAMCANYONRAZOR",self)==1) {
    increment("CANYONRAZORBODYCOUNT");
    if(number("MIS_ADDON_GREG_CLEARCANYON")==number("LOG_RUNNING"))
      vm.call_function<void>("B_COUNTCANYONRAZOR");
    }

  if(self->aivar[monsterId]==number("ID_SWAMPDRONE")) {
    const auto target = other();
    if(npc_getdisttonpc(self,target)<300)
      target->attribute[NpcAttribute::hitpoints] -= 50;
    }
  if(matches("DIEGOOW"))
    setNumber("DIEGO_ISDEAD",1);

  if(npc_isplayer(other())) {
    self->aivar[killedByPlayer] = 1;
    if(vm.call_function<int32_t>("C_DROPUNCONSCIOUS")!=0)
      increment("MADKILLERCOUNT");
    const auto running = number("LOG_RUNNING");
    if(number("MIS_FESTER_KILLBUGS")==running && self->guild==number("GIL_GIANT_BUG"))
      increment("FESTERS_GIANT_BUG_KILLED");
    if(number("MIS_KROKOJAGD")==running && matches("SWAMPRAT"))
      increment("ALLIGATORJACK_KROKOSKILLED");
    if(matches("RAMON"))
      setNumber("PLAYER_HASTALKEDTOBANDITCAMP",1);
    if(matches("FRANCO")) {
      if(number("MIS_HLPLOGAN")==running)
        setNumber("MIS_HLPLOGAN",number("LOG_OBSOLETE"));
      if(number("MIS_HLPEDGOR")==running)
        setNumber("MIS_HLPEDGOR",number("LOG_OBSOLETE"));
      }
    if(matches("FORTUNO"))
      log_settopicstatus(findSymbol("TOPIC_ADDON_FORTUNO")->get_string(),number("LOG_OBSOLETE"));
    }

  if(matches("GOLDMINECRAWLER")) {
    if(number("BLOODWYN_SPAWN")==0 && number("MINECRAWLER_KILLED")>=9) {
      ai_teleport(namedNpc("BLOODWYN"),"ADW_MINE_TO_MC_03");
      vm.call_function<void>("B_STARTOTHERROUTINE",namedNpc("BLOODWYN"),std::string_view("MINE"));
      vm.call_function<void>("B_GIVEPLAYERXP",number("XP_ADDON_BLOODYWYN"));
      setNumber("BLOODWYN_SPAWN",1);
      } else {
      increment("MINECRAWLER_KILLED");
      }
    }

  vm.call_function<void>("B_GIVETRADEINV",self);
  vm.call_function<void>("B_GIVEDEATHINV",self);
  vm.call_function<void>("B_CLEARRUNEINV",self);
  vm.call_function<void>("B_DELETEPETZCRIME",self);
  self->aivar[sawCrime] = number("CRIME_NONE");
  ai_unequipweapons(self);
  self->aivar[deathHandled] = 0;
  result = 0;
  return true;
  }

bool GameScript::runUnconsciousState(Npc& npc, const daedalus::DaedalusSymbol& symbol, int& result) {
  // These AIVAR slots belong to the verified NotR program.
  constexpr size_t fightResult = 0;
  constexpr size_t ransacked = 10;
  constexpr size_t guardPassage = 12;
  constexpr size_t partyMember = 15;
  constexpr size_t victoryXpGiven = 16;
  constexpr size_t arenaFight = 45;
  constexpr size_t attackReason = 47;
  constexpr size_t duelCounted = 48;
  constexpr size_t magic = 51;
  constexpr size_t defeatedByPlayer = 66;
  const auto self = std::static_pointer_cast<daedalus::INpc>(npc.handlePtr());
  const auto other = [&] { return std::dynamic_pointer_cast<daedalus::INpc>(vm.global_other()->get_instance()); };
  const auto hero = [&] { return std::dynamic_pointer_cast<daedalus::INpc>(vm.global_hero()->get_instance()); };
  const auto number = [&](std::string_view name) { return findSymbol(name)->get_int(); };
  const auto namedId = [&](std::string_view name) { return hlp_getinstanceid(findSymbol(name)->get_instance()); };
  const auto startState = [&](std::string_view name) { ai_startstate(self,int(findFunction(name).ptr),0,""); };

  result = 0;
  if(symbol.name()=="ZS_UNCONSCIOUS_LOOP") {
    result = npc_getstatetime(self)<number("HAI_TIME_UNCONSCIOUS") ? LOOP_CONTINUE : LOOP_END;
    return true;
    }

  if(symbol.name()=="ZS_UNCONSCIOUS") {
    npc_percenable(self,PERC_ASSESSMAGIC,int(findFunction("B_ASSESSMAGIC").ptr));
    const auto body = npc_getbodystate(self);
    const bool diving = ActivityDefinitions::bodyStateContains(body,BS_DIVE);
    const bool swimming = ActivityDefinitions::bodyStateContains(body,BS_SWIM);
    // The second stock check leaves its arguments in the saved VM symbols.
    findSymbol("C_BODYSTATECONTAINS.BODYSTATE")->set_int(BS_SWIM);
    findSymbol("C_BODYSTATECONTAINS.SLF")->set_instance(self);
    if(diving || swimming) {
      npc_clearaiqueue(self);
      vm.call_function<void>("B_CLEARPERCEPTIONS",self);
      startState("ZS_DEAD");
      return true;
      }

    self->aivar[guardPassage] = number("GP_NONE");
    npc_setrefusetalk(self,0);
    npc_settempattitude(self,npc_getpermattitude(self,hero()));
    vm.call_function<void>("B_STOPLOOKAT",self);
    ai_stoppointat(self);
    if(npc_isplayer(other()) && self->guild<int32_t(GIL_SEPERATOR_HUM)) {
      self->aivar[defeatedByPlayer] = 1;
      self->aivar[fightResult] = number("FIGHT_LOST");
      if(self->guild==GIL_SLD && self->aivar[duelCounted]==0 && self->aivar[attackReason]==number("AR_NONE")) {
        auto& duels = *findSymbol("SLD_DUELLE_GEWONNEN");
        duels.set_int(duels.get_int()+1);
        self->aivar[duelCounted] = 1;
        }
      if(self->aivar[arenaFight]==number("AF_RUNNING"))
        self->aivar[arenaFight] = number("AF_AFTER");
      }
    if(const auto winner = other(); npc_isplayer(self) && winner) {
      winner->aivar[fightResult] = number("FIGHT_WON");
      if(winner->aivar[arenaFight]==number("AF_RUNNING"))
        winner->aivar[arenaFight] = number("AF_AFTER");
      }

    vm.call_function<void>("B_GIVETRADEINV",self);
    vm.call_function<void>("B_CLEARRUNEINV",self);
    const auto winner = other();
    if(self->aivar[victoryXpGiven]==0 && ((winner && winner->aivar[partyMember]==1) || npc_isplayer(winner))) {
      vm.call_function<void>("B_GIVEPLAYERXP",self->level*number("XP_PER_VICTORY"));
      self->aivar[victoryXpGiven] = 1;
      }
    ai_unequipweapons(self);
    if(namedId("CIPHER")==hlp_getinstanceid(other()) && namedId("DAR")==hlp_getinstanceid(self))
      findSymbol("DAR_LOSTAGAINSTCIPHER")->set_int(1);
    return true;
    }

  if(symbol.name()!="ZS_UNCONSCIOUS_END")
    return false;
  self->aivar[ransacked] = 0;
  ai_standup(self);
  if(npc_isplayer(self))
    return true;
  if(namedId("RAVEN")==hlp_getinstanceid(self)) {
    self->aivar[magic] = number("MAGIC_ALWAYS");
    vm.call_function<void>("B_ATTACK",self,hero(),number("AR_GUILDENEMY"),int32_t(0));
    return true;
    }

  const auto distance = npc_getdisttonpc(self,other());
  const auto canSee = npc_canseenpcfreelos(self,other());
  if(distance<number("PERC_DIST_INTERMEDIAT") && canSee) {
    vm.call_function<void>("B_TURNTONPC",self,other());
    const auto attitude = npc_getpermattitude(self,other());
    const auto toughGuy = vm.call_function<int32_t>("C_NPCISTOUGHGUY",self);
    const bool threatens = int32_t(self->type)!=number("NPCTYPE_FRIEND") && attitude!=ATT_HOSTILE && toughGuy!=0;
    vm.call_function<void>("B_SAY",self,other(),std::string_view(threatens ? "$NEXTTIMEYOUREINFORIT" : "$OHMYHEAD"));
    }
  npc_perceiveall(self);
  // Both detections run. A melee item overrides ITEM when both searches succeed.
  const bool ranged = wld_detectitem(self,ITM_CAT_FF);
  const bool melee = wld_detectitem(self,ITM_CAT_NF);
  if(ranged || melee) {
    const auto item = std::dynamic_pointer_cast<daedalus::IItem>(vm.global_item()->get_instance());
    if(hlp_isvaliditem(item) && npc_getdisttoitem(self,item)<=500)
      ai_takeitem(self,item);
    }
  ai_equipbestmeleeweapon(self);
  ai_equipbestrangedweapon(self);
  startState("ZS_HEALSELF");
  return true;
  }

bool GameScript::runHealSelf(Npc& npc, const daedalus::DaedalusSymbol& symbol, int& result) {
  result = 0;
  if(symbol.name()=="ZS_HEALSELF") {
    // The stock minimal perception setup only enables these entries.
    npc.handle().senses = int32_t(SensesBit::SENSE_SEE|SensesBit::SENSE_HEAR);
    npc.handle().senses_range = findSymbol("PERC_DIST_ACTIVE_MAX")->get_int();
    npc.setPerceptionEnable(PERC_ASSESSMAGIC,findFunction("B_ASSESSMAGIC").ptr);
    npc.setPerceptionEnable(PERC_ASSESSDAMAGE,findFunction("B_ASSESSDAMAGE").ptr);
    npc.setPerceptionEnable(PERC_ASSESSMURDER,findFunction("B_ASSESSMURDER").ptr);
    npc.setPerceptionEnable(PERC_ASSESSTHEFT,findFunction("B_ASSESSTHEFT").ptr);
    npc.setPerceptionEnable(PERC_ASSESSUSEMOB,findFunction("B_ASSESSUSEMOB").ptr);
    npc.setPerceptionEnable(PERC_ASSESSENTERROOM,findFunction("B_ASSESSPORTALCOLLISION").ptr);
    return true;
    }
  if(symbol.name()=="ZS_HEALSELF_END")
    return true;
  if(symbol.name()!="ZS_HEALSELF_LOOP")
    return false;

  if(npc.attribute(ATR_HITPOINTS)!=npc.attribute(ATR_HITPOINTSMAX)) {
    static constexpr std::string_view potions[] = {"ITPO_HEALTH_03", "ITPO_HEALTH_02", "ITPO_HEALTH_01"};
    for(const auto potion : potions) {
      const auto item = findSymbol(potion)->index();
      if(int(npc.itemCount(item))>0) {
        npc.aiPush(AiQueue::aiUseItem(int32_t(item)));
        result = LOOP_CONTINUE;
        return true;
        }
      }
    }
  npc.clearAiQueue();
  result = LOOP_END;
  return true;
  }

bool GameScript::wantsToAttackRoomIntruder(const std::shared_ptr<daedalus::INpc>& self) {
  findSymbol("C_WANTTOATTACKROOMINTRUDER.SLF")->set_instance(self);
  const auto owner = std::dynamic_pointer_cast<daedalus::INpc>(vm.global_self()->get_instance());
  const auto crime = playerCrime(owner);
  const auto guild = owner ? owner->guild : GIL_NONE;
  return crime!=findSymbol("CRIME_NONE")->get_int() || guild==GIL_DJG || guild==GIL_SLD || guild==GIL_PAL || guild==GIL_MIL;
  }

int32_t GameScript::playerCrime(const std::shared_ptr<daedalus::INpc>& self) {
  findSymbol("B_GETPLAYERCRIME.SLF")->set_instance(self);
  constexpr size_t crimeType = 1, crimeDay = 2, crimeAbsolution = 46;
  const auto none = findSymbol("CRIME_NONE")->get_int();
  const auto crime = self ? self->aivar[crimeType] : 0;
  if(crime<=findSymbol("CRIME_ATTACK")->get_int() && (self ? self->aivar[crimeDay] : 0)<wld_getday()-2)
    return none;
  const auto recordedLevel = self ? self->aivar[crimeAbsolution] : 0;
  return recordedLevel<currentAbsolutionLevel(self) ? none : crime;
  }

void GameScript::memorizePlayerCrime(const std::shared_ptr<daedalus::INpc>& witness,
                                     const std::shared_ptr<daedalus::INpc>& offender, int32_t crime) {
  findSymbol("B_MEMORIZEPLAYERCRIME.CRIME")->set_int(crime);
  findSymbol("B_MEMORIZEPLAYERCRIME.TAETER")->set_instance(offender);
  findSymbol("B_MEMORIZEPLAYERCRIME.SLF")->set_instance(witness);
  if(!npc_isplayer(offender) || witness==nullptr || witness->guild==GIL_DMT)
    return;

  if(crime>playerCrime(witness)) {
    vm.call_function<void>("B_DELETEPETZCRIME",witness);
    vm.call_function<void>("B_ADDPETZCRIME",witness,crime);
    }
  if(crime>=playerCrime(witness)) {
    constexpr size_t crimeType = 1, crimeDay = 2, crimeAbsolution = 46;
    witness->aivar[crimeType] = crime;
    witness->aivar[crimeDay] = wld_getday();
    witness->aivar[crimeAbsolution] = currentAbsolutionLevel(witness);
    }
  }

int32_t GameScript::currentAbsolutionLevel(const std::shared_ptr<daedalus::INpc>& self) {
  findSymbol("B_GETCURRENTABSOLUTIONLEVEL.SLF")->set_instance(self);
  findSymbol("C_NPCBELONGSTOOLDCAMP.SLF")->set_instance(self);
  const auto guild = self ? self->guild : GIL_NONE;
  const auto type = self ? self->type : NpcType::G2_AMBIENT;
  const bool city = guild==GIL_PAL || guild==GIL_MIL || guild==GIL_VLK;
  if(city && (type==NpcType::G2_OC_AMBIENT || type==NpcType::G2_OC_MAIN))
    return findSymbol("ABSOLUTIONLEVEL_OLDCAMP")->get_int();
  findSymbol("C_NPCBELONGSTOCITY.SLF")->set_instance(self);
  if(city)
    return findSymbol("ABSOLUTIONLEVEL_CITY")->get_int();
  findSymbol("C_NPCBELONGSTOMONASTERY.SLF")->set_instance(self);
  if(guild==GIL_NOV || guild==GIL_KDF)
    return findSymbol("ABSOLUTIONLEVEL_MONASTERY")->get_int();
  findSymbol("C_NPCBELONGSTOFARM.SLF")->set_instance(self);
  if(guild==GIL_SLD || guild==GIL_BAU)
    return findSymbol("ABSOLUTIONLEVEL_FARM")->get_int();
  findSymbol("C_NPCBELONGSTOBL.SLF")->set_instance(self);
  if(type==NpcType::G2_BL_MAIN || type==NpcType::G2_BL_AMBIENT)
    return findSymbol("ABSOLUTIONLEVEL_BL")->get_int();
  return 0;
  }

bool GameScript::isToughGuy(const std::shared_ptr<daedalus::INpc>& self) {
  findSymbol("C_NPCISTOUGHGUY.SLF")->set_instance(self);
  constexpr size_t tough = 57;
  const auto guild = self ? self->guild : GIL_NONE;
  return (self && self->aivar[tough]==1) || guild==GIL_PIR || guild==GIL_BDT || guild==GIL_DJG || guild==GIL_SLD;
  }

void GameScript::startNpcAttack(const std::shared_ptr<daedalus::INpc>& attacker,
                                const std::shared_ptr<daedalus::INpc>& target, int32_t reason, int32_t wait) {
  auto& selfSlot = *findSymbol("B_ATTACK.SLF");
  auto& otherSlot = *findSymbol("B_ATTACK.OTH");
  auto& reasonSlot = *findSymbol("B_ATTACK.ATTACK_REASON");
  auto& waitSlot = *findSymbol("B_ATTACK.WAIT");
  waitSlot.set_int(wait);
  reasonSlot.set_int(reason);
  otherSlot.set_instance(target);
  selfSlot.set_instance(attacker);
  const auto self = [&] { return std::dynamic_pointer_cast<daedalus::INpc>(selfSlot.get_instance()); };
  const auto other = [&] { return std::dynamic_pointer_cast<daedalus::INpc>(otherSlot.get_instance()); };
  const auto value = [this](std::string_view name) { return findSymbol(name)->get_int(); };

  constexpr size_t invincible = 4, lastTarget = 7, attackReason = 9, waitBeforeAttack = 23;
  constexpr size_t lastPlayerReason = 47, enemyOverride = 61, lastFightComment = 64;
  self()->aivar[waitBeforeAttack] = waitSlot.get_int();
  if(reasonSlot.get_int()==value("AR_SUDDENENEMYINFERNO")) {
    self()->aivar[enemyOverride] = 0;
    npc_sendpassiveperc(self(),PERC_ASSESSFIGHTSOUND,self(),other());
    }

  if(npc_isinstate(self(),int(ZS_Talk))) {
    self()->aivar[invincible] = 0;
    if(auto actor = other())
      actor->aivar[invincible] = 0;
    }

  const bool sameTarget = self()->aivar[lastTarget]==hlp_getinstanceid(other());
  const bool attacking = npc_isinstate(self(),int(ZS_Attack));
  bool changeReason = true;
  if(sameTarget && attacking) {
    const auto current = self()->aivar[attackReason];
    const bool wantsToKill = hasAttackReasonToKill(self());
    changeReason = reasonSlot.get_int()>current && !wantsToKill;
    }
  if(changeReason) {
    self()->aivar[attackReason] = reasonSlot.get_int();
    if(npc_isplayer(other()))
      self()->aivar[lastPlayerReason] = reasonSlot.get_int();
    }

  const auto hasReason = [&](std::string_view name) { return self()->aivar[attackReason]==value(name); };
  const auto memorize = [&](std::string_view name) {
    memorizePlayerCrime(self(),other(),value(name));
    };
  if(hasReason("AR_SHEEPKILLER"))
    memorize("CRIME_SHEEPKILLER");
  if(hasReason("AR_REACTTOWEAPON") || hasReason("AR_REACTTODAMAGE")) {
    const bool friendNpc = int32_t(self()->type)==value("NPCTYPE_FRIEND");
    const bool targetPlayer = npc_isplayer(other());
    const bool toughGuy = isToughGuy(self());
    if(!(friendNpc && targetPlayer) && !toughGuy)
      memorize("CRIME_ATTACK");
    }
  if(hasReason("AR_USEMOB") || hasReason("AR_THEFT"))
    memorize("CRIME_THEFT");
  if(hasReason("AR_HUMANMURDEREDHUMAN"))
    memorize("CRIME_MURDER");

  if(npc_isinstate(self(),int(ZS_Attack)))
    return;
  if(hasReason("AR_KILL")) {
    findSymbol("B_SETATTITUDE.ATT")->set_int(int32_t(ATT_HOSTILE));
    findSymbol("B_SETATTITUDE.SLF")->set_instance(self());
    npc_setattitude(self(),ATT_HOSTILE);
    npc_settempattitude(self(),ATT_HOSTILE);
    }
  if(npc_isplayer(other()))
    self()->aivar[0] = value("FIGHT_CANCEL");
  self()->aivar[lastFightComment] = 0;
  if(!npc_isinstate(self(),int(ZS_Talk))) {
    npc_clearaiqueue(self());
    clearPerceptions(self());
    }
  npc_settarget(self(),other());
  findSymbol("C_BODYSTATECONTAINS.BODYSTATE")->set_int(int32_t(BS_LIE));
  findSymbol("C_BODYSTATECONTAINS.SLF")->set_instance(self());
  const auto mustStand = ActivityDefinitions::bodyStateContains(npc_getbodystate(self()),BS_LIE);
  ai_startstate(self(),int(ZS_Attack),mustStand ? 1 : 0,"");
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
