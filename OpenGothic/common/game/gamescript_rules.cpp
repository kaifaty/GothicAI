#include "gamescript.h"

#include "basestore.h"
#include "game/definitions/basescripts.h"
#include "programdata.h"

#include "damagecalculator.h"
#include "definitions/fightaidefinitions.h"
#include "npcactivityhost.h"
#include "gothic.h"

#include "world/objects/item.h"
#include "world/objects/npc.h"

#include <algorithm>
#include <bit>
#include <stdexcept>
#include <unordered_map>
#include <variant>
#include <array>

namespace {
constexpr std::string_view bartokOrcGreeting = "DIA_Bartok_Angekommen_04_02";
constexpr std::string_view cookCleaverGreeting = "Hackebeil_01_00";

// AR_* values used by the announcement helpers in the verified stock program.
enum class AttackAnnouncementReason : int32_t {
  LeftPortalRoom = 1,
  ClearRoom = 2,
  GuardCalledToRoom = 3,
  MonsterVsHuman = 4,
  MonsterMurderedHuman = 5,
  SheepKiller = 6,
  Theft = 7,
  UseMob = 8,
  GuardCalledToThief = 9,
  ReactToWeapon = 10,
  ReactToDamage = 11,
  GuardStopsFight = 12,
  GuildEnemy = 14,
  HumanMurderedHuman = 15,
  MonsterCloseToGate = 16,
  GuardStopsIntruder = 17,
  Kill = 19,
  };
}

std::string_view GameScript::symbolName(size_t id) {
  if(stockPrograms && id<=UINT32_MAX)
    if(const auto name = storySymbolName(uint32_t(id)); !name.empty())
      return name;
  if(id<=UINT32_MAX)
    if(const auto* entry = own.find(uint32_t(id)))
      return entry->name;
  if(auto* symbol = findSymbol(id))
    return symbol->name();
  return "";
  }

ScriptFn GameScript::findFunction(std::string_view name) {
  if(baseNativeCatalog()!=nullptr) {
    const auto id = storyScriptFunctionId(name);
    return id ? ScriptFn(*id) : ScriptFn{};
    }
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

bool GameScript::invokeNativeRoomPerception(std::string_view name, NpcActivityProbe::Phase& phase) {
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
  using Actor=std::shared_ptr<daedalus::INpc>;
  nativeExternals.insert_or_assign("B_ASSESSENTERROOM",storygraph::makeNativeInternal(std::function<int32_t()>{
    [this] { return runEnterRoomReaction(); }}));
  nativeExternals.insert_or_assign("C_NPCISBOTHEREDBYPLAYERROOMGUILD",
    storygraph::makeNativeInternal(std::function<int32_t(Actor)>{
      [this](Actor npc) { return int32_t(isBotheredByPlayerRoomGuild(npc)); }}));
  }

void GameScript::setPerceptionPreset(bool normal) {
  setPerceptionPreset(normal,nativeNpc("SELF"));
  }

void GameScript::setPerceptionPreset(bool normal,const std::shared_ptr<daedalus::INpc>& self) {
  if(self==nullptr)
    return;
  self->senses = int32_t(SensesBit::SENSE_SEE | SensesBit::SENSE_HEAR);
  self->senses_range = intConstant("PERC_DIST_ACTIVE_MAX").value();
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
  storyActorSlot("B_CLEARPERCEPTIONS.SLF") = self;
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
  storyActorSlot("C_NPCISGATEGUARD.SLF") = self;
  return npc_isinstate(self,int(findFunction("ZS_GUARD_PASSAGE").ptr));
  }

bool GameScript::isBotheredByPlayerRoomGuild(const std::shared_ptr<daedalus::INpc>& self) {
  storyActorSlot("C_NPCISBOTHEREDBYPLAYERROOMGUILD.SLF") = self;
  const auto portalGuild = wld_getplayerportalguild();
  setNpcRuleInt("C_NPCISBOTHEREDBYPLAYERROOMGUILD.PORTALGUILD",portalGuild);
  const auto npcGuild = self ? int32_t(self->guild) : int32_t(GIL_NONE);
  const bool allied = wld_getguildattitude(npcGuild,portalGuild)==ATT_FRIENDLY;
  return (allied || npcGuild==portalGuild) && portalGuild>int32_t(GIL_NONE);
  }

bool GameScript::isPlayerFakeBandit(const std::shared_ptr<daedalus::INpc>& self, const std::shared_ptr<daedalus::INpc>& other) {
  storyActorSlot("C_PLAYERISFAKEBANDIT.OTH") = other;
  storyActorSlot("C_PLAYERISFAKEBANDIT.SLF") = self;
  constexpr size_t ignoresArmor = 73; // AIV_IGNORESARMOR in the verified program
  constexpr size_t ignoresFakeGuild = 70; // AIV_IGNORESFAKEGUILD
  const auto guild = self ? uint32_t(self->guild) : GIL_NONE;
  if(self && (self->aivar[ignoresArmor]==1 || self->aivar[ignoresFakeGuild]==1 || guild==GIL_PIR))
    return false;

  if(npc_hasequippedarmor(other)) {
    const auto armor = npc_getequippedarmor(other);
    storyActorSlot("C_PLAYERISFAKEBANDIT.ITM") = armor;
    const bool heavyBandit = hlp_isitem(armor,int(findSymbolIndex("ITAR_BDT_H")));
    const bool thorus = hlp_isitem(armor,int(findSymbolIndex("ITAR_THORUS_ADDON")));
    const bool mediumBandit = hlp_isitem(armor,int(findSymbolIndex("ITAR_BDT_M")));
    return heavyBandit || thorus || mediumBandit;
    }

  return npcRuleInt("PLAYER_HASTALKEDTOBANDITCAMP")==1 &&
         currentStoryWorld==npcRuleInt("ADDONWORLD_ZEN") && guild==GIL_BDT;
  }

bool GameScript::isPlayerFakePirate(const std::shared_ptr<daedalus::INpc>& self, const std::shared_ptr<daedalus::INpc>& other) {
  constexpr size_t ignoresArmor = 73; // AIV_IGNORESARMOR
  constexpr size_t ignoresFakeGuild = 70; // AIV_IGNORESFAKEGUILD
  storyActorSlot("C_PLAYERISFAKEPIRATE.OTH") = other;
  storyActorSlot("C_PLAYERISFAKEPIRATE.SLF") = self;
  if(self && (self->aivar[ignoresArmor]==1 || self->aivar[ignoresFakeGuild]==1))
    return false;
  const auto currentOther = nativeNpc("OTHER");
  if(!npc_hasequippedarmor(currentOther))
    return false;
  const auto armor = npc_getequippedarmor(other);
  storyActorSlot("C_PLAYERISFAKEPIRATE.ITM") = armor;
  return hlp_isitem(armor,int(findSymbolIndex("ITAR_PIR_H_ADDON"))) ||
         hlp_isitem(armor,int(findSymbolIndex("ITAR_PIR_M_ADDON"))) ||
         hlp_isitem(armor,int(findSymbolIndex("ITAR_PIR_L_ADDON")));
  }

bool GameScript::hasFakeGuild(const std::shared_ptr<daedalus::INpc>& self, const std::shared_ptr<daedalus::INpc>& other) {
  storyActorSlot("C_PLAYERHASFAKEGUILD.OTH") = other;
  storyActorSlot("C_PLAYERHASFAKEGUILD.SLF") = self;
  const auto armor = npc_getequippedarmor(other);
  storyActorSlot("C_PLAYERHASFAKEGUILD.ITM") = armor;
  auto& rangerComment = *findSymbol("C_PLAYERHASFAKEGUILD.NSC_COMMENTRANGERARMOR");
  rangerComment.set_int(1);
  constexpr size_t ignoresArmor = 73; // AIV_IGNORESARMOR
  constexpr size_t ignoresFakeGuild = 70; // AIV_IGNORESFAKEGUILD
  if(self && (self->aivar[ignoresArmor]==1 || self->aivar[ignoresFakeGuild]==1))
    return false;
  // This guard check reads global SELF. The rest of the rule uses SLF and OTH.
  if(isGateGuard(nativeNpc("SELF")))
    return false;
  const bool fakeBandit = isPlayerFakeBandit(self,other);
  const auto listenerGuild = self ? self->guild : int32_t(GIL_NONE);
  const bool hasArmor = npc_hasequippedarmor(other);
  if(fakeBandit && listenerGuild==GIL_BDT)
    return !hasArmor;
  if(!hasArmor || other->guild!=listenerGuild)
    return false;
  if(hlp_isitem(armor,int(findSymbolIndex("ITAR_RANGER_ADDON"))))
    return rangerComment.get_int()==1;

  const auto wears = [&](std::initializer_list<std::string_view> items) {
    return std::ranges::any_of(items,[&](std::string_view item) {
      return hlp_isitem(armor,int(findSymbolIndex(item)));
      });
    };
  const auto mercenaryArmor = [&] { return wears({"ITAR_SLD_L","ITAR_SLD_M","ITAR_SLD_H"}); };
  switch(listenerGuild) {
    case GIL_NONE:
      return !wears({"ITAR_LEATHER_L","ITAR_BAU_M","ITAR_BAU_L","ITAR_VLK_H","ITAR_VLK_M","ITAR_VLK_L"});
    case GIL_MIL:
      return !wears({"ITAR_MIL_M","ITAR_MIL_L"});
    case GIL_PAL:
      return !wears({"ITAR_PAL_H","ITAR_PAL_M"});
    case GIL_SLD:
      return !mercenaryArmor();
    case GIL_DJG:
      return !mercenaryArmor() && !wears({"ITAR_DJG_CRAWLER","ITAR_DJG_H","ITAR_DJG_M","ITAR_DJG_L"});
    case GIL_NOV:
      return !wears({"ITAR_NOV_L"});
    case GIL_KDF:
      return !wears({"ITAR_KDF_H","ITAR_KDF_L"});
    default:
      return true;
    }
  }

void GameScript::desynchronizeNpc() {
  const auto delay = hlp_random(1000);
  setNpcRuleInt("B_MM_DESYNCHRONIZE.MSEC",delay);
  ai_waitms(nativeNpc("SELF"),delay);
  }

void GameScript::sayToNpc(const std::shared_ptr<daedalus::INpc>& self, const std::shared_ptr<daedalus::INpc>& other,
                         std::string_view line, bool overlay) {
  const auto text = overlay ? "B_SAY_OVERLAY.TEXT" : "B_SAY.TEXT";
  setNpcRuleString(text,line);
  storyActorSlot(overlay ? "B_SAY_OVERLAY.OTH" : "B_SAY.OTH") = other;
  storyActorSlot(overlay ? "B_SAY_OVERLAY.SLF" : "B_SAY.SLF") = self;
  if(overlay)
    ai_outputsvm_overlay(self,other,npcRuleString(text));
  else
    ai_outputsvm(self,other,npcRuleString(text));
  }

void GameScript::turnToNpc(const std::shared_ptr<daedalus::INpc>& self, const std::shared_ptr<daedalus::INpc>& other) {
  storyActorSlot("B_TURNTONPC.OTH") = other;
  storyActorSlot("B_TURNTONPC.SLF") = self;
  ai_turntonpc(self,other);
  }

void GameScript::lookAtNpc(const std::shared_ptr<daedalus::INpc>& self, const std::shared_ptr<daedalus::INpc>& other) {
  storyActorSlot("B_LOOKATNPC.OTH") = other;
  storyActorSlot("B_LOOKATNPC.SLF") = self;
  stopLookAtNpc(self);
  ai_lookatnpc(self,other);
  }

void GameScript::stopLookAtNpc(const std::shared_ptr<daedalus::INpc>& self) {
  storyActorSlot("B_STOPLOOKAT.SLF") = self;
  const auto target = npc_getlookattarget(self);
  storyActorSlot("B_STOPLOOKAT.TARGET") = target;
  if(hlp_isvalidnpc(target))
    ai_stoplookat(self);
  }

void GameScript::queueItemUse(const std::shared_ptr<daedalus::INpc>& self, int32_t item) {
  setNpcRuleInt("B_USEITEM.ITMINSTANCE",item);
  storyActorSlot("B_USEITEM.SLF") = self;
  if(npcBodyStateContains(self,BS_SIT))
    return;
  const auto target = npc_getlookattarget(self);
  storyActorSlot("B_USEITEM.TARGET") = target;
  const bool looking = hlp_isvalidnpc(target);
  if(looking)
    stopLookAtNpc(self);
  ai_useitem(self,item);
  if(looking)
    lookAtNpc(self,target);
  }

void GameScript::resetRoutineNpc(const std::shared_ptr<daedalus::INpc>& self) {
  storyActorSlot("B_RESETALL.SLF") = self;
  stopLookAtNpc(self);
  ai_removeweapon(self);
  }

bool GameScript::npcBodyStateContains(const std::shared_ptr<daedalus::INpc>& self, int32_t state) {
  setNpcRuleInt("C_BODYSTATECONTAINS.BODYSTATE",state);
  storyActorSlot("C_BODYSTATECONTAINS.SLF") = self;
  return ActivityDefinitions::bodyStateContains(npc_getbodystate(self),state);
  }

int32_t GameScript::runEnterRoomReaction() {
  const auto self = nativeNpc("SELF");
  const auto other = [&] { return nativeNpc("OTHER"); };
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
  if(int32_t(self->type)==npcRuleInt("NPCTYPE_FRIEND") || npc_getattitude(self,other())==ATT_FRIENDLY)
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

  const auto self = nativeNpc("SELF");
  const auto other = nativeNpc("OTHER");
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
    const auto reason = npcRuleInt("AR_LEFTPORTALROOM");
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
  const auto self = nativeNpc("SELF");
  const bool wantsAttack = wantsToAttackRoomIntruder(self);
  sayToNpc(self,nativeNpc("OTHER"),
           wantsAttack ? "$GETOUTOFHERE" : "$WHYAREYOUINHERE");
  }

void GameScript::runQuietSoundReaction(Npc& listener) {
  const auto* source = findNpc(nativeNpc("OTHER"));
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
    const auto* other = findNpc(nativeNpc("OTHER"));
    return other!=nullptr && personAttitude(listener,*other)==ATT_HOSTILE;
    };
  const bool importantInfo = npc_checkinfo(self,1);
  const bool hostile = hostileToSource();
  if(!importantInfo && !hostile)
    return;

  constexpr size_t enemyOverride = 61; // AIV_ENEMYOVERRIDE in the verified program
  const bool bandit = listener.handle().guild==GIL_BDT;
  const auto currentSelf = nativeNpc("SELF");
  const bool fakeBandit = isPlayerFakeBandit(currentSelf,nativeNpc("OTHER"));
  const bool overrideEnemy = listener.handle().aivar[enemyOverride]==1;
  const bool stillHostile = hostileToSource();
  if(((bandit && fakeBandit) || overrideEnemy) && stillHostile)
    return;
  if(listener.canSeeSource())
    return;

  listener.clearAiQueue();
  clearPerceptions(nativeNpc("SELF"));
  ai_startstate(self,int(findFunction("ZS_OBSERVEPLAYER").ptr),1,"");
  }

bool GameScript::nativeHumanPerceptionsEnabled() const {
  return activityEvents && !floorDefer.active() && ActivityDefinitionRuntime::mode()!=ActivityDefinitionRuntime::Mode::Off;
  }

void GameScript::setCombatAttitude(const std::shared_ptr<daedalus::INpc>& npc,int32_t attitude) {
  setNpcRuleInt("B_SETATTITUDE.ATT",attitude);
  storyActorSlot("B_SETATTITUDE.SLF") = npc;
  npc_setattitude(npc,attitude);
  npc_settempattitude(npc,attitude);
  }

void GameScript::loadNativeHumanPerceptions() {
  if(!nativeRoutineProgram)
    return;
  bindCrimeGlobals();
  drawWeaponCallback = findFunction("B_ASSESSDRAWWEAPON");
  threatCallback = findFunction("B_ASSESSTHREAT");
  warnCallback = findFunction("B_ASSESSWARN");
  fightSoundCallback = findFunction("B_ASSESSFIGHTSOUND");
  fightDefeatCallback = findFunction("B_ASSESSDEFEAT");
  cheerFightCallback = findFunction("B_CHEERFIGHT");
  damageCallback = findFunction("B_ASSESSDAMAGE");
  enemyCallback = findFunction("B_ASSESSENEMY");
  playerCallback = findFunction("B_ASSESSPLAYER");
  followPlayerCallback = findFunction("B_ASSESSFOLLOWPLAYER");
  guidePlayerCallback = findFunction("B_ASSESSGUIDEPLAYER");
  followFightSoundCallback = findFunction("B_ASSESSFOLLOWFIGHTSOUND");
  guideFightSoundCallback = findFunction("B_ASSESSGUIDEFIGHTSOUND");
  theftCallback = findFunction("B_ASSESSTHEFT");
  useMobCallback = findFunction("B_ASSESSUSEMOB");
  moveMobCallback = findFunction("B_MOVEMOB");
  surpriseCallback = findFunction("B_ASSESSSURPRISE");
  murderCallback = findFunction("B_ASSESSMURDER");
  stopShortZapCallback = findFunction("B_STOPSHORTZAPPED");
  restartTentacleCallback = findFunction("B_RESTARTGREENTENTACLE");
  const auto bind = [this]<class R,class... P>(std::string_view name,const std::function<R(P...)>& callback) {
    nativeExternals.insert_or_assign(std::string(name),storygraph::makeNativeInternal(callback));
    };
  bind("PERCEPTION_SET_NORMAL",std::function<void()>{[this] { setPerceptionPreset(true); }});
  bind("PERCEPTION_SET_MINIMAL",std::function<void()>{[this] { setPerceptionPreset(false); }});
  bind("B_ASSESSDRAWWEAPON",std::function<int32_t()>{[this] { return runDrawWeaponReaction(); }});
  bind("B_ASSESSDAMAGE",std::function<void()>{[this] { runDamageReaction(); }});
  bind("B_ASSESSENEMY",std::function<int32_t()>{[this] { return runEnemyReaction(); }});
  bind("B_ASSESSPLAYER",std::function<void()>{[this] { runPlayerAssessment(); }});
  bind("B_ASSESSFIGHTSOUND",std::function<void()>{[this] { runFightSoundReaction(); }});
  bind("B_ASSESSGUIDEPLAYER",std::function<void()>{[this] { runGuidePlayerAssessment(); }});
  bind("C_DIEGOTOOFAR",std::function<int32_t(int32_t)>{[this](int32_t tolerance) {
    return diegoRouteBoundary(tolerance);
    }});
  bind("B_VALIDATEOTHER",std::function<void()>{[this] {
    validateCombatTarget(nativeNpc("SELF"));
    }});
  using Actor = std::shared_ptr<daedalus::INpc>;
  bind("B_ATTACK",std::function<void(Actor,Actor,int32_t,int32_t)>{
    [this](const Actor& self,const Actor& other,int32_t reason,int32_t wait) {
      startNpcAttack(self,other,reason,wait);
      }});
  bind("C_ISUSEDMOBMYPOSSESSION",std::function<int32_t(Actor,Actor)>{
    [this](const Actor& self,const Actor& other) { return usedMobIsPossession(self,other); }});
  bind("C_ISTAKENITEMMYPOSSESSION",std::function<int32_t(Actor,Actor,std::shared_ptr<daedalus::IItem>)>{
    [this](const Actor& self,const Actor& other,const auto& item) { return takenItemIsPossession(self,other,item); }});
  bind("C_WANTTOATTACKTHIEF",std::function<int32_t(Actor,Actor)>{
    [this](const Actor& self,const Actor& other) { return wantsToAttackThief(self,other); }});
  bind("B_GHOSTSPECIALDAMAGE",std::function<int32_t(Actor,Actor)>{[this](const Actor& attacker,const Actor& defender) {
    ghostSpecialDamage(attacker,defender);
    return 0;
    }});
  bind("B_BELIARSWEAPONSPECIALDAMAGE",std::function<void(Actor,Actor)>{[this](const Actor& attacker,const Actor& defender) {
    beliarWeaponSpecialDamage(attacker,defender);
    }});
  bind("C_SCHASREADIEDBELIARSWEAPON",std::function<int32_t()>{[this] { return hasReadiedBeliarWeapon(); }});
  bind("C_ISITEMBELIARSWEAPON",std::function<int32_t(std::shared_ptr<daedalus::IItem>)>{
    [this](const auto& weapon) { return isBeliarWeaponItem(weapon); }});
  bind("C_NPCISTOUGHGUY",std::function<int32_t(Actor)>{[this](const Actor& npc) {
    return isToughGuy(npc);
    }});
  bind("C_REFUSETALK",std::function<int32_t(Actor,Actor)>{[this](const Actor& npc,const Actor& other) {
    return refusesTalk(npc,other);
    }});
  bind("C_IAMCANYONRAZOR",std::function<int32_t(Actor)>{[this](const Actor& npc) {
    return isCanyonRazor(npc);
    }});
  bind("C_DROPUNCONSCIOUS",std::function<int32_t()>{[this] { return dropsUnconsciousNpc(); }});
  bind("B_COUNTCANYONRAZOR",std::function<void()>{[this] { showRemainingCanyonRazors(); }});
  bind("C_WANTTOCALLGUARDS",std::function<int32_t(Actor)>{[this](const Actor& npc) {
    return wantsToCallGuards(npc);
    }});
  bind("B_CALLGUARDS",std::function<void()>{[this] { callGuards(); }});
  bind("C_BEKLAUEN",std::function<int32_t(int32_t,int32_t)>{[this](int32_t dexterity, int32_t gold) {
    return canPickpocket(dexterity,gold);
    }});
  bind("B_BEKLAUEN",std::function<void()>{[this] { attemptPickpocket(); }});
  bind("B_GIVETHIEFXP",std::function<void()>{[this] { givePickpocketXp(); }});
  bind("B_RESETTHIEFLEVEL",std::function<void()>{[this] { decrementPickpocketVictimCount(); }});
  bind("C_WANTTOFLEE",std::function<int32_t(Actor,Actor)>{[this](const Actor& self, const Actor& other) {
    rememberFleeCheck(self,other);
    return 0;
    }});
  bind("C_NPCRANSACKSALWAYS",std::function<int32_t(Actor)>{[this](const Actor& npc) {
    return alwaysRansacks(npc);
    }});
  bind("C_WANTTORANSACK",std::function<int32_t(Actor)>{[this](const Actor& npc) {
    return wantsToRansack(npc);
    }});
  bind("B_FINISHINGMOVE",std::function<void(Actor,Actor)>{[this](const Actor& self, const Actor& other) {
    queueFinishingMove(self,other);
    }});
  bind("B_ENDPRODUCTIONDIALOG",std::function<void()>{[this] { endProductionDialog(); }});
  bind("B_USEITEM",std::function<void(Actor,int32_t)>{[this](const Actor& self, int32_t item) {
    queueItemUse(self,item);
    }});
  bind("B_TURNTONPC",std::function<void(Actor,Actor)>{[this](const Actor& self,const Actor& other) {
    turnToNpc(self,other);
    }});
  bind("B_LOOKATNPC",std::function<void(Actor,Actor)>{[this](const Actor& self, const Actor& other) {
    lookAtNpc(self,other);
    }});
  bind("B_STOPLOOKAT",std::function<void(Actor)>{[this](const Actor& self) { stopLookAtNpc(self); }});
  bind("C_NPCISGATEGUARD",std::function<int32_t(Actor)>{[this](const Actor& npc) {
    return isGateGuard(npc);
    }});
  bind("C_BODYSTATECONTAINS",std::function<int32_t(Actor,int32_t)>{[this](const Actor& npc, int32_t state) {
    return npcBodyStateContains(npc,state);
    }});
  bind("B_CLEARPERCEPTIONS",std::function<void(Actor)>{[this](const Actor& npc) {
    clearPerceptions(npc);
    }});
  bind("B_SETATTITUDE",std::function<void(Actor,int32_t)>{[this](const Actor& npc,int32_t attitude) {
    setCombatAttitude(npc,attitude);
    }});
  bind("B_SAY",std::function<void(Actor,Actor,std::string_view)>{[this](const Actor& self, const Actor& other, std::string_view line) {
    sayToNpc(self,other,line);
    }});
  bind("B_SAY_OVERLAY",std::function<void(Actor,Actor,std::string_view)>{[this](const Actor& self, const Actor& other, std::string_view line) {
    sayToNpc(self,other,line,true);
    }});
  bind("B_SAY_GUILDGREETINGS",std::function<void(Actor,Actor)>{[this](const Actor& self, const Actor& other) {
    sayGuildGreeting(self,other);
    }});
  bind("B_BARTOK_SHITANORC",std::function<void()>{[this] { sayAmbientGreeting(bartokOrcGreeting); }});
  bind("B_KOCH_HACKEBEIL",std::function<void()>{[this] { sayAmbientGreeting(cookCleaverGreeting); }});
  bind("B_SAY_ATTACKREASON",std::function<void()>{[this] { sayAttackReason(); }});
  bind("B_SAY_ATTACKEND",std::function<void()>{[this] { sayAttackEnd(); }});
  bind("C_PLAYERISFAKEPIRATE",std::function<int32_t(Actor,Actor)>{[this](const Actor& self, const Actor& other) {
    return isPlayerFakePirate(self,other);
    }});
  bind("B_ADDPETZCRIME",std::function<void(Actor,int32_t)>{[this](const Actor& witness, int32_t crime) {
    adjustPetzCrime(witness,crime);
    }});
  bind("B_DELETEPETZCRIME",std::function<void(Actor)>{[this](const Actor& witness) {
    adjustPetzCrime(witness,std::nullopt);
    }});
  loadNativeCrimeReports();
  bind("C_NPCISBOTHEREDBYWEAPON",std::function<int32_t(Actor,Actor)>{[this](const Actor& self, const Actor& other) {
    return botheredByWeapon(self,other);
    }});
  bind("C_WANTTOATTACKMURDER",std::function<int32_t(Actor,Actor)>{[this](const Actor& self, const Actor& other) {
    return wantsToAttackMurderer(self,other);
    }});
  bind("C_WANTTOREACTTOSNEAKER",std::function<int32_t(Actor,Actor)>{[this](const Actor& self, const Actor& other) {
    return wantsToReactToSneaker(self,other);
    }});
  bind("B_ASSIGNDEMENTORTALK",std::function<void(Actor)>{[this](const Actor& npc) {
    assignDementorTalk(npc);
    }});
  bind("C_NPCBELONGSTOBL",std::function<int32_t(Actor)>{[this](const Actor& npc) {
    return belongsToBanditCamp(npc);
    }});
  bind("C_NPCISDOWN",std::function<int32_t(Actor)>{[this](const Actor& npc) {
    return isNpcDownInScript(npc);
    }});
  bind("C_PLAYERHASFAKEGUILD",std::function<int32_t(Actor,Actor)>{[this](const Actor& self, const Actor& other) {
    return hasFakeGuild(self,other);
    }});
  }

bool GameScript::invokeNativeHumanPerception(ScriptFn fn, NpcActivityProbe::Phase& phase) {
  const bool stopShortZap = fn==stopShortZapCallback;
  const bool restartTentacle = fn==restartTentacleCallback;
  const bool propertyCrime = fn==theftCallback || fn==useMobCallback;
  const bool fightWitness = fn==fightDefeatCallback || fn==cheerFightCallback;
  const bool companionFight = fn==followFightSoundCallback || fn==guideFightSoundCallback;
  if((fn!=drawWeaponCallback && fn!=threatCallback && fn!=warnCallback && fn!=fightSoundCallback &&
      fn!=damageCallback && fn!=enemyCallback && fn!=playerCallback && fn!=murderCallback && fn!=followPlayerCallback &&
      fn!=guidePlayerCallback && !companionFight && !propertyCrime && !stopShortZap && !restartTentacle && !fightWitness &&
      fn!=moveMobCallback && fn!=surpriseCallback) ||
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
  else if(fn==followPlayerCallback)
    runFollowerPlayerAssessment();
  else if(fn==guidePlayerCallback)
    runGuidePlayerAssessment();
  else if(companionFight)
    runCompanionFightSound(fn==followFightSoundCallback);
  else if(propertyCrime)
    runPropertyCrime(fn==useMobCallback);
  else if(fn==moveMobCallback)
    runMoveMobReaction();
  else if(fn==surpriseCallback)
    runSurpriseReaction();
  else if(fn==murderCallback)
    runMurderReaction();
  else if(fn==warnCallback)
    runWarnReaction();
  else if(fn==fightSoundCallback)
    runFightSoundReaction();
  else if(fn==fightDefeatCallback)
    runFightDefeat();
  else if(fn==cheerFightCallback)
    runCheerFight();
  else if(stopShortZap)
    stopControlSpell(nativeNpc("SELF"),true);
  else if(restartTentacle) {
    const auto self = nativeNpc("SELF");
    if(npc_getlasthitspellid(self)==npcRuleInt("SPL_GREENTENTACLE"))
      npc_setstatetime(self,0);
    }
  else
    runDamageReaction();
  return true;
  }

int32_t GameScript::botheredByWeapon(const std::shared_ptr<daedalus::INpc>& self, const std::shared_ptr<daedalus::INpc>& other) {
  storyActorSlot("C_NPCISBOTHEREDBYWEAPON.OTH") = other;
  storyActorSlot("C_NPCISBOTHEREDBYWEAPON.SLF") = self;
  constexpr size_t partyMember = 15; // AIV_PARTYMEMBER
  if(self==nullptr || self->aivar[partyMember]==1)
    return 0;
  // The friend exemption reads global OTHER. The weapon, attitude and spell checks use the argument.
  if(npc_isplayer(nativeNpc("OTHER")) && self->type==NpcType::G2_FRIEND)
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
  const auto self = nativeNpc("SELF");
  const auto other = [&] { return nativeNpc("OTHER"); };
  if(npc_isinfightmode(other(),int(FightMode::FMODE_NONE)) ||
     npc_getdisttonpc(self,other())>npcRuleInt("PERC_DIST_DIALOG") || botheredByWeapon(self,other())==0)
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
  const auto self = nativeNpc("SELF");
  const auto other = nativeNpc("OTHER");
  if(npc_getdisttonpc(self,other)>npcRuleInt("PERC_DIST_INTERMEDIAT") ||
     !npc_canseenpc(self,other) || botheredByWeapon(self,other)==0)
    return;
  startHumanWarning(self,findFunction("ZS_REACTTOWEAPON"));
  }

void GameScript::runWarnReaction() {
  const auto self = nativeNpc("SELF");
  const auto source = nativeNpc("OTHER");
  const auto victim = storyActorSlot("VICTIM");
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
  storyActorSlot("C_NPCHASATTACKREASONTOKILL.SLF") = npc;
  const auto reason = npc!=nullptr ? npc->aivar[9] : 0; // AIV_ATTACKREASON
  return reason==npcRuleInt("AR_GUARDCALLEDTOKILL") || reason==npcRuleInt("AR_GUARDSTOPSINTRUDER") ||
         reason==npcRuleInt("AR_HUMANMURDEREDHUMAN") || reason==npcRuleInt("AR_GUILDENEMY") ||
         reason==npcRuleInt("AR_KILL");
  }

bool GameScript::wantsToAttackSheepKiller(const std::shared_ptr<daedalus::INpc>& self,
                                        const std::shared_ptr<daedalus::INpc>& other) {
  storyActorSlot("C_WANTTOATTACKSHEEPKILLER.OTH") = other;
  storyActorSlot("C_WANTTOATTACKSHEEPKILLER.SLF") = self;
  const auto guild = self->guild;
  constexpr size_t dontAttackSheepKiller = 56;
  if(guild==GIL_DJG || guild==GIL_BDT || guild==GIL_STRF || guild==GIL_DMT ||
     self->aivar[dontAttackSheepKiller]==1 || (self->type==NpcType::G2_FRIEND && npc_isplayer(other)))
    return false;
  if(hasAttackReasonToKill(other))
    return false;
  const auto source = nativeNpc("OTHER");
  if(source!=nullptr && source->aivar[9]!=0 && source->guild<int32_t(GIL_SEPERATOR_HUM))
    return false;
  return !isGateGuard(self);
  }

bool GameScript::wantsToAttackThief(const std::shared_ptr<daedalus::INpc>& self,
                                   const std::shared_ptr<daedalus::INpc>& other) {
  storyActorSlot("C_WANTTOATTACKTHIEF.OTH") = other;
  storyActorSlot("C_WANTTOATTACKTHIEF.SLF") = self;
  const auto guild = self ? self->guild : int32_t(GIL_NONE);
  constexpr size_t dontAttackThief = 55; // AIV_IGNORE_THEFT
  if(guild==GIL_DJG || guild==GIL_STRF || guild==GIL_DMT ||
     (self && (self->aivar[dontAttackThief]==1 || self->aivar[0]==npcRuleInt("FIGHT_LOST"))))
    return false;
  const bool player = npc_isplayer(other);
  if(self && self->type==NpcType::G2_FRIEND && player)
    return false;
  return !isGateGuard(self);
  }

bool GameScript::wantsToAttackMurderer(const std::shared_ptr<daedalus::INpc>& self,
                                      const std::shared_ptr<daedalus::INpc>& other) {
  storyActorSlot("C_WANTTOATTACKMURDER.OTH") = other;
  storyActorSlot("C_WANTTOATTACKMURDER.SLF") = self;
  constexpr size_t ignoreMurder = 54; // AIV_IGNORE_MURDER
  if(self) {
    const auto guild = self->guild;
    if(guild==GIL_BDT || guild==GIL_DJG || guild==GIL_STRF || guild==GIL_DMT ||
       self->aivar[ignoreMurder]==1 || (self->type==NpcType::G2_FRIEND && npc_isplayer(other)))
      return false;
    }
  return !hasAttackReasonToKill(other) && !isGateGuard(self);
  }

bool GameScript::belongsToBanditCamp(const std::shared_ptr<daedalus::INpc>& npc) {
  storyActorSlot("C_NPCBELONGSTOBL.SLF") = npc;
  return npc && (npc->type==NpcType::G2_BL_MAIN || npc->type==NpcType::G2_BL_AMBIENT);
  }

bool GameScript::isNpcDownInScript(const std::shared_ptr<daedalus::INpc>& npc) {
  storyActorSlot("C_NPCISDOWN.SLF") = npc;
  return npc_isdead(npc) || npc_isinstate(npc,int(findFunction("ZS_MAGICSLEEP").ptr)) ||
         npc_isinstate(npc,int(ZS_Unconscious));
  }

void GameScript::runFightSoundReaction() {
  using Actor = std::shared_ptr<daedalus::INpc>;
  const auto self = nativeNpc("SELF");
  const auto other = [&] { return nativeNpc("OTHER"); };
  const auto source = other();
  const auto victim = nativeNpc("VICTIM");
  if(self==nullptr || !hlp_isvalidnpc(victim))
    return;

  const auto value = [this](std::string_view name) { return npcRuleInt(name); };
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
    const bool victimDives = npcBodyStateContains(victim,BS_DIVE);
    const bool victimSwims = npcBodyStateContains(victim,BS_SWIM);
    const bool sourceDives = npcBodyStateContains(source,BS_DIVE);
    const bool sourceSwims = npcBodyStateContains(source,BS_SWIM);
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
    if(!isNpcDownInScript(target) && hlp_isvalidnpc(target))
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
  const auto self = nativeNpc("SELF");
  const auto other = [&] { return nativeNpc("OTHER"); };

  const auto quarhodron = hlp_getnpc(int(findSymbolIndex("NONE_ADDON_111_QUARHODRON")));
  storyActorSlot("B_ASSESSDAMAGE.QUARHO") = quarhodron;
  const auto rhademes = hlp_getnpc(int(findSymbolIndex("NONE_ADDON_112_RHADEMES")));
  storyActorSlot("B_ASSESSDAMAGE.RHADEM") = rhademes;
  if(self==nullptr)
    return;
  const auto selfId = hlp_getinstanceid(self);
  if(selfId==hlp_getinstanceid(rhademes) || selfId==hlp_getinstanceid(quarhodron)) {
    ghostSpecialDamage(other(),self);
    return;
    }
  beliarWeaponSpecialDamage(other(),self);

  auto& aivar = self->aivar;
  if(aivar[arenaFight]==npcRuleInt("AF_AFTER"))
    aivar[arenaFight] = npcRuleInt("AF_AFTER_PLUS_DAMAGE");
  if(aivar[enemyOverride]==1) {
    const auto raven = hlp_getnpc(int(findSymbolIndex("BDT_1090_ADDON_RAVEN")));
    storyActorSlot("B_ASSESSDAMAGE.RAV") = raven;
    if(hlp_getinstanceid(raven)==selfId)
      aivar[enemyOverride] = 0;
    }

  if(npc_isinstate(self,int(ZS_Attack))) {
    const bool player = npc_isplayer(other());
    if(player && (self->type==NpcType::G2_FRIEND || aivar[partyMember]==1))
      return;
    const auto sourceId = hlp_getinstanceid(other());
    if(aivar[lastTarget]!=sourceId) {
      if(sourceId!=hlp_getinstanceid(storyActorSlot("HERO")) || aivar[hitByOther]==sourceId)
        npc_settarget(self,other());
      else
        aivar[hitByOther] = sourceId;
      }
    return;
    }

  if(runEnemyReaction()!=0)
    return;
  const auto none = npcRuleInt("AR_NONE");
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
  startNpcAttack(self,attacker,npcRuleInt("AR_REACTTODAMAGE"),0);
  }

int32_t GameScript::runEnemyReaction() {
  constexpr size_t partyMember = 15; // AIV_PARTYMEMBER
  constexpr size_t followInWater = 28; // AIV_MM_FOLLOWINWATER
  constexpr size_t lastPlayerAttackReason = 47; // AIV_LASTPLAYERAR
  constexpr size_t enemyOverride = 61; // AIV_ENEMYOVERRIDE
  constexpr size_t noFightParker = 71; // AIV_NOFIGHTPARKER
  const auto self = nativeNpc("SELF");
  const auto other = nativeNpc("OTHER");
  const auto actorId = [&](std::string_view instance, std::string_view scratch) {
    const auto actor = hlp_getnpc(int(findSymbolIndex(instance)));
    storyActorSlot(scratch) = actor;
    return hlp_getinstanceid(actor);
    };
  const auto inspector = actorId("PC_LEVELINSPEKTOR","B_ASSESSENEMY.PCL");
  const auto otherId = hlp_getinstanceid(other);
  if(otherId==inspector || self==nullptr)
    return 0;
  const auto otherGuild = other ? int32_t(other->guild) : 0;
  const auto humanSeparator = npcRuleInt("GIL_SEPERATOR_HUM");
  const bool otherParker = other && other->aivar[noFightParker]==1;
  if((otherParker && otherGuild>humanSeparator) ||
     ((otherParker || self->aivar[noFightParker]==1) && otherGuild<humanSeparator &&
      otherId!=hlp_getinstanceid(storyActorSlot("HERO"))))
    return 0;

  const bool diving = npcBodyStateContains(other,int32_t(BS_DIVE));
  const bool swimming = npcBodyStateContains(other,int32_t(BS_SWIM));
  if((self->aivar[followInWater]==0 && (diving || swimming)) ||
     npc_getheighttonpc(self,other)>npcRuleInt("PERC_DIST_HEIGHT"))
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
    startNpcAttack(self,other,npcRuleInt("AR_GUILDENEMY"),0);
    return 1;
    }
  const bool near = npc_getdisttonpc(self,other)<=npcRuleInt("PERC_DIST_INTERMEDIAT");
  const bool observing = npc_isinstate(self,int(findFunction("ZS_OBSERVEPLAYER").ptr));
  const bool waited = npc_getstatetime(self)>2;
  if(near && (observing || waited) && npc_getattitude(self,other)==ATT_HOSTILE) {
    startNpcAttack(self,other,self->aivar[lastPlayerAttackReason],0);
    return 1;
    }
  return 0;
  }

void GameScript::runMoveMobReaction() {
  const auto self = nativeNpc("SELF");
  const auto scheme = npc_getdetectedmob(self);
  setNpcRuleString("B_MOVEMOB.DOOR",scheme);
  if(scheme!="DOOR")
    return;
  if(wld_getmobstate(self,scheme)==0) {
    npc_clearaiqueue(self);
    ai_usemob(self,scheme,1);
    ai_usemob(self,scheme,-1);
    }
  ai_continueroutine(self);
  }

void GameScript::runSurpriseReaction() {
  const auto self = nativeNpc("SELF");
  const auto other = nativeNpc("OTHER");
  npc_settarget(self,other);
  if(self!=nullptr) {
    constexpr size_t attackReason = 9; // AIV_ATTACKREASON
    self->aivar[attackReason] = npcRuleInt("AR_GUILDENEMY");
    }
  }

void GameScript::runFightDefeat() {
  constexpr size_t attackReason = 9; // AIV_ATTACKREASON
  const auto self = nativeNpc("SELF");
  const auto other = nativeNpc("OTHER");
  const auto victim = nativeNpc("VICTIM");
  npc_clearaiqueue(self);
  const bool playerFriend = self && self->type==NpcType::G2_FRIEND && npc_isplayer(other);
  const bool tough = isToughGuy(self);
  if(playerFriend || tough) {
    sayToNpc(self,other,npc_getattitude(self,other)==ATT_FRIENDLY ? "$GOODVICTORY" : "$NOTBAD");
    return;
    }
  if((victim ? victim->aivar[attackReason] : 0)!=npcRuleInt("AR_NONE")) {
    sayToNpc(self,other,"$OHMYGODHESDOWN");
    memorizePlayerCrime(self,other,npcRuleInt("CRIME_ATTACK"));
    } else {
    sayToNpc(self,other,"$NOTBAD");
    }
  }

void GameScript::runCheerFight() {
  const auto self = nativeNpc("SELF");
  const auto other = nativeNpc("OTHER");
  const auto victim = nativeNpc("VICTIM");
  const auto separator = npcRuleInt("GIL_SEPERATOR_HUM");
  if(npc_getstatetime(self)<=2 || (other && int32_t(other->guild)>separator) || (victim && int32_t(victim->guild)>separator))
    return;
  npc_setstatetime(self,0);
  if(!isToughGuy(self))
    return;

  static constexpr std::string_view concern[] = {"$OOH01","$OOH02","$OOH03"};
  static constexpr std::string_view cheer[] = {"$CHEERFRIEND01","$CHEERFRIEND02","$CHEERFRIEND03"};
  const auto variation = hlp_random(3);
  setNpcRuleInt("B_CHEERFIGHT.ZUFALL",variation);
  const bool friendAttacker = npc_getattitude(self,other)==ATT_FRIENDLY;
  const bool friendVictim = npc_getattitude(self,victim)==ATT_FRIENDLY;
  const bool upset = !friendAttacker && friendVictim;
  sayToNpc(self,self,upset ? concern[size_t(variation)] : cheer[size_t(variation)],true);
  ai_playani(self,upset ? "T_WATCHFIGHT_OHNO" : "T_WATCHFIGHT_YEAH");
  }

bool GameScript::usedMobIsPossession(const std::shared_ptr<daedalus::INpc>& self,
                                     const std::shared_ptr<daedalus::INpc>& other) {
  storyActorSlot("C_ISUSEDMOBMYPOSSESSION.OTH") = other;
  storyActorSlot("C_ISUSEDMOBMYPOSSESSION.SLF") = self;
  const auto witness = nativeNpc("SELF");
  const auto user = nativeNpc("OTHER");
  if(npc_isdetectedmobownedbynpc(user,witness))
    return true;
  const bool publicRoom = wld_getplayerportalguild()==int32_t(GIL_PUBLIC);
  const bool bothered = isBotheredByPlayerRoomGuild(witness);
  return publicRoom || bothered;
  }

bool GameScript::takenItemIsPossession(const std::shared_ptr<daedalus::INpc>& self,
                                       const std::shared_ptr<daedalus::INpc>& other,
                                       const std::shared_ptr<daedalus::IItem>& item) {
  storyActorSlot("C_ISTAKENITEMMYPOSSESSION.ITM") = item;
  storyActorSlot("C_ISTAKENITEMMYPOSSESSION.OTH") = other;
  storyActorSlot("C_ISTAKENITEMMYPOSSESSION.SLF") = self;
  setNpcRuleInt("C_ISTAKENITEMMYPOSSESSION.PORTALGUILD",wld_getplayerportalguild());
  if(npc_ownedbynpc(item,self)) {
    if(hlp_isitem(item,int(findSymbolIndex("HOLY_HAMMER_MIS"))))
      setNpcRuleInt("HAMMER_TAKEN",1);
    return true;
    }
  const auto dropped = uint32_t(npcRuleInt("ITEM_DROPPED"));
  if(item && (uint32_t(item->flags)&dropped)==dropped)
    return false;
  const bool publicRoom = wld_getplayerportalguild()==int32_t(GIL_PUBLIC);
  const auto witness = nativeNpc("SELF");
  const bool bothered = isBotheredByPlayerRoomGuild(witness);
  if(publicRoom || bothered)
    return true;
  const auto guild = self ? int32_t(self->guild) : int32_t(GIL_NONE);
  return wld_getguildattitude(guild,item ? item->owner_guild : int32_t(GIL_NONE))==ATT_FRIENDLY;
  }

void GameScript::runPropertyCrime(bool usingMob) {
  const auto self = nativeNpc("SELF");
  const auto other = [&] { return nativeNpc("OTHER"); };
  const auto item = [&] { return std::dynamic_pointer_cast<daedalus::IItem>(storyActorSlot("ITEM")); };
  const auto culprit = other();
  if(!npc_isplayer(culprit) || self==nullptr)
    return;
  const bool indoors = wld_getplayerportalguild()>=int32_t(GIL_NONE);
  if(indoors && npc_getheighttonpc(self,culprit)>npcRuleInt("PERC_DIST_INDOOR_HEIGHT"))
    return;
  if(wld_getguildattitude(int32_t(self->guild),int32_t(culprit->guild))==ATT_FRIENDLY) {
    const bool personallyOwned = usingMob ? npc_isdetectedmobownedbynpc(culprit,self)
                                          : npc_ownedbynpc(item(),self) && hlp_isvaliditem(item());
    if(!personallyOwned)
      return;
    }
  const bool possession = usingMob ? usedMobIsPossession(self,other()) : takenItemIsPossession(self,other(),item());
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
    setNpcRuleString("B_ASSESSUSEMOB.DETMOB",scheme);
    if(scheme!="CHESTSMALL" && scheme!="CHESTBIG")
      return;
    }
  if(!wantsToAttackThief(self,other())) {
    if(isGateGuard(self))
      memorizePlayerCrime(self,other(),npcRuleInt("CRIME_THEFT"));
    return;
    }
  const auto reason = npcRuleInt(usingMob ? "AR_USEMOB" : "AR_THEFT");
  startNpcAttack(self,other(),reason,0);
  }

void GameScript::runMurderReaction() {
  constexpr size_t dropDeadAndKill = 52; // AIV_DROPDEADANDKILL
  constexpr size_t toughGuy = 57; // AIV_TOUGHGUY
  constexpr size_t enemyOverride = 61; // AIV_ENEMYOVERRIDE
  const auto self = nativeNpc("SELF");
  const auto other = [&] { return nativeNpc("OTHER"); };
  const auto victim = nativeNpc("VICTIM");
  const auto constant = [&](std::string_view name) { return npcRuleInt(name); };
  const auto attack = [&](std::string_view reason) { startNpcAttack(self,other(),constant(reason),0); };
  const auto report = [&](std::string_view crime) { memorizePlayerCrime(self,other(),constant(crime)); };
  const auto guild = [](const std::shared_ptr<daedalus::INpc>& actor) { return actor ? int32_t(actor->guild) : 0; };
  const auto killer = other();
  if(self==nullptr || hlp_getinstanceid(killer)==hlp_getinstanceid(self))
    return;
  const auto range = constant("PERC_DIST_INTERMEDIAT");
  const auto height = constant("PERC_DIST_HEIGHT");
  if((npc_getdisttonpc(self,victim)>range && npc_getdisttonpc(self,killer)>range) ||
     (npc_getheighttonpc(self,victim)>height && npc_getheighttonpc(self,killer)>height) ||
     (!npc_canseenpcfreelos(self,victim) && !npc_canseenpcfreelos(self,killer)))
    return;
  if(runEnemyReaction()!=0)
    return;

  const auto victimGuild = guild(victim);
  if(victim && victim->aivar[toughGuy]==0 && victimGuild==GIL_SHEEP) {
    if(wantsToAttackSheepKiller(self,other())) {
      attack("AR_SHEEPKILLER");
      return;
      }
    if(isGateGuard(self))
      report("CRIME_SHEEPKILLER");
    }
  const auto separator = constant("GIL_SEPERATOR_HUM");
  if(victimGuild>separator)
    return;
  if(guild(other())>separator) {
    attack("AR_MONSTERMURDEREDHUMAN");
    return;
    }
  if(self->aivar[enemyOverride]==1) {
    self->aivar[enemyOverride] = 0;
    npc_perceiveall(self);
    npc_getnexttarget(self);
    if(!isNpcDownInScript(other()) && hlp_isvalidnpc(other()))
      attack("AR_GUILDENEMY");
    return;
    }

  const auto attitudeToKiller = npc_getattitude(self,other());
  const auto attitudeToVictim = npc_getattitude(self,victim);
  if((attitudeToKiller==ATT_NEUTRAL || attitudeToKiller==ATT_FRIENDLY) &&
     (attitudeToVictim==ATT_ANGRY || attitudeToVictim==ATT_HOSTILE))
    return;
  if(self->type==NpcType::G2_FRIEND && npc_isplayer(other()))
    return;
  if(!wantsToAttackMurderer(self,other())) {
    if(isGateGuard(self))
      report("CRIME_MURDER");
    return;
    }
  const auto attacker = other();
  if((victim && victim->aivar[dropDeadAndKill]==1) || (attacker && attacker->aivar[dropDeadAndKill]==1))
    return;
  if((!belongsToBanditCamp(victim) && victimGuild==GIL_BDT) || victimGuild==GIL_DMT)
    return;
  attack("AR_HUMANMURDEREDHUMAN");
  }

bool GameScript::wantsToReactToSneaker(const std::shared_ptr<daedalus::INpc>& self, const std::shared_ptr<daedalus::INpc>& other) {
  storyActorSlot("C_WANTTOREACTTOSNEAKER.OTH") = other;
  storyActorSlot("C_WANTTOREACTTOSNEAKER.SLF") = self;
  if(npc_getdisttonpc(self,other)>700)
    return false;
  const bool inBanditCamp = belongsToBanditCamp(self);
  const auto guild = self->guild;
  if((!inBanditCamp && guild==GIL_BDT) || guild==GIL_STRF || guild==GIL_DMT)
    return false;
  if(npc_isplayer(other) && self->type==NpcType::G2_FRIEND)
    return false;
  if(npc_getattitude(self,other)==ATT_FRIENDLY)
    return false;
  // The guard check uses the current script actor even if the caller passed a different NPC.
  return !isGateGuard(nativeNpc("SELF"));
  }

void GameScript::assignDementorTalk(const std::shared_ptr<daedalus::INpc>& npc) {
  storyActorSlot("B_ASSIGNDEMENTORTALK.SLF") = npc;
  if(npc==nullptr || npc->type!=NpcType::G2_AMBIENT || npc->guild!=GIL_DMT)
    return;
  const auto id = hlp_getinstanceid(npc);
  const auto& dialogs = baseNativeCatalog()->identities.dialogs;
  for(const auto name : {"DIA_AMBIENTDEMENTOR_EXIT", "DIA_AMBIENTDEMENTOR"}) {
    const auto* identity = NativeBaseCatalog::findIdentity(dialogs,name);
    if(identity==nullptr)
      throw std::runtime_error("missing ambient dialog: "+std::string(name));
    if(auto* info = findInfo(identity->id))
      info->npc = id;
    }
  }

void GameScript::runPlayerAssessment() {
  constexpr size_t startedTalk = 3; // AIV_NPCSTARTEDTALK
  constexpr size_t invincible = 4; // AIV_INVINCIBLE
  constexpr size_t guardPassageStatus = 12; // AIV_GUARDPASSAGE_STATUS
  constexpr size_t partyMember = 15; // AIV_PARTYMEMBER
  const auto self = nativeNpc("SELF");
  const auto other = [&] { return nativeNpc("OTHER"); };
  const auto constant = [&](std::string_view name) { return npcRuleInt(name); };
  const auto distance = [&] { return npc_getdisttonpc(self,other()); };
  const auto hasBodyState = [&](const std::shared_ptr<daedalus::INpc>& actor, int32_t state) {
    return npcBodyStateContains(actor,state);
    };
  const auto fakeGuild = [&] { return hasFakeGuild(self,other()); };
  const auto assessTalk = [&] {
    self->aivar[startedTalk] = 1;
    vm.unsafe_call_bl(findSymbol("B_ASSESSTALK"));
    };

  const auto inspector = hlp_getnpc(int(findSymbolIndex("PC_LEVELINSPEKTOR")));
  storyActorSlot("B_ASSESSPLAYER.PCL") = inspector;
  const auto source = other();
  if(hlp_getinstanceid(source)==hlp_getinstanceid(inspector) || self==nullptr || (source && source->aivar[invincible]==1) ||
     isNpcDownInScript(source))
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
  const bool pursueMurderer = wantsToAttackMurderer(self,other());
  const bool murdered = playerCrime(self)==constant("CRIME_MURDER");
  if(near && pursueMurderer && murdered) {
    startNpcAttack(self,other(),constant("AR_HUMANMURDEREDHUMAN"),0);
    return;
    }
  if(runEnterRoomReaction()!=0 || runDrawWeaponReaction()!=0)
    return;
  setNpcRuleInt("PLAYER_DRAWWEAPONCOMMENT",0);

  if(hasBodyState(other(),BS_SNEAK)) {
    const bool react = wantsToReactToSneaker(self,other());
    const auto observe = findFunction("ZS_OBSERVEPLAYER");
    if(react && !npc_isinstate(self,int(observe.ptr))) {
      npc_clearaiqueue(self);
      clearPerceptions(self);
      ai_startstate(self,int(observe.ptr),1,"");
      return;
      }
    }
  else if(hasBodyState(other(),BS_STAND)) {
    setNpcRuleInt("PLAYER_SNEAKERCOMMENT",0);
    }
  if(!hasBodyState(other(),BS_LIE))
    setNpcRuleInt("PLAYER_GETOUTOFMYBEDCOMMENT",0);

  assignDementorTalk(self);
  const auto talkDistance = constant("PERC_DIST_DIALOG");
  if(npc_checkinfo(self,1) && distance()<=talkDistance) {
    if(isGateGuard(self)) {
      assessTalk();
      return;
      }
    const bool disguise = fakeGuild();
    const bool refuse = refusesTalk(self,other());
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
    sayGuildGreeting(self,other());
    stopLookAtNpc(self);
    npc_setrefusetalk(other(),20);
    }
  const bool outsideTalkRange = distance()>talkDistance;
  if(isGateGuard(self) && outsideTalkRange)
    self->aivar[guardPassageStatus] = constant("GP_NONE");
  }

void GameScript::sayGuildGreeting(const std::shared_ptr<daedalus::INpc>& self, const std::shared_ptr<daedalus::INpc>& other) {
  storyActorSlot("B_SAY_GUILDGREETINGS.OTH") = other;
  storyActorSlot("B_SAY_GUILDGREETINGS.SLF") = self;
  const auto selfGuild = self ? self->guild : int32_t(GIL_NONE);
  const auto otherGuild = other ? other->guild : int32_t(GIL_NONE);
  if(selfGuild==GIL_MIL && (otherGuild==GIL_PAL || otherGuild==GIL_MIL)) {
    sayToNpc(self,other,"$MILGREETINGS",true);
    return;
    }
  if((selfGuild==GIL_PAL && (otherGuild==GIL_PAL || otherGuild==GIL_MIL || otherGuild==GIL_KDF)) ||
     (selfGuild==GIL_KDF && (otherGuild==GIL_PAL || otherGuild==GIL_NOV || otherGuild==GIL_KDF))) {
    sayToNpc(self,other,"$PALGREETINGS",true);
    return;
    }

  const auto selfId = hlp_getinstanceid(self);
  if(npcRuleInt("BARTOK_ORKGESAGT")==1 && selfId==hlp_getinstanceid(storyActorSlot("BARTOK"))) {
    sayAmbientGreeting(bartokOrcGreeting);
    return;
    }
  if(selfId==hlp_getinstanceid(storyActorSlot("KOCH"))) {
    sayAmbientGreeting(cookCleaverGreeting);
    return;
    }
  const auto roll = hlp_random(100);
  setNpcRuleInt("B_SAY_GUILDGREETINGS.ZUFALL",roll);
  if(wld_israining() && roll<=10)
    sayToNpc(self,other,"$WEATHER",true);
  }

void GameScript::sayAmbientGreeting(std::string_view line) {
  // The story outputs use the current context, independently of the greeting parameters.
  ai_output(nativeNpc("SELF"),
            nativeNpc("OTHER"),line);
  }

void GameScript::stopControlSpell(const std::shared_ptr<daedalus::INpc>& self, bool continueRoutine) {
  npc_percenable(self,PERC_ASSESSMAGIC,int(findFunction("B_ASSESSMAGIC").ptr));
  npc_clearaiqueue(self);
  ai_standup(self);
  if(int32_t(self->guild)<npcRuleInt("GIL_SEPERATOR_HUM"))
    runDamageReaction();
  else
    npc_settempattitude(self,ATT_HOSTILE);
  if(continueRoutine)
    ai_continueroutine(self);
  }

bool GameScript::runControlSpell(Npc& npc, std::string_view name, int& result) {
  const auto self = std::static_pointer_cast<daedalus::INpc>(npc.handlePtr());
  result = 0;
  if(name=="ZS_GREENTENTACLE") {
    npc_percenable(self,PERC_ASSESSMAGIC,int(restartTentacleCallback.ptr));
    npc_percenable(self,PERC_ASSESSDAMAGE,int(damageCallback.ptr));
    npc_stopani(self,"s_GreententacleA_Victim");
    npc_stopani(self,"s_GreententacleB_Victim");
    npc_stopani(self,"s_GreententacleC_Victim");
    npc_clearaiqueue(self);
    ai_standup(self);
    if(npcBodyStateContains(self,int32_t(BS_UNCONSCIOUS)))
      return true;
    if(int32_t(self->guild)<npcRuleInt("GIL_SEPERATOR_HUM")) {
      static constexpr std::string_view humanPoses[] = {
        "T_STAND_2_GREENTENTACLEA_VICTIM", "T_STAND_2_GREENTENTACLEB_VICTIM", "T_STAND_2_GREENTENTACLEC_VICTIM",
        };
      const auto pose = npcRuleInt("ZS_GREENTENTACLE.RANDY");
      if(size_t(pose)<std::size(humanPoses))
        ai_playanibs(self,humanPoses[size_t(pose)],BS_UNCONSCIOUS);
      } else {
      ai_playanibs(self,"T_STAND_2_FREEZE_VICTIM",BS_UNCONSCIOUS);
      }
    return true;
    }
  if(name=="ZS_GREENTENTACLE_LOOP") {
    result = LOOP_CONTINUE;
    if(npc_getstatetime(self)>npcRuleInt("SPL_TIME_GREENTENTACLE")) {
      stopControlSpell(self,false);
      result = LOOP_END;
      }
    return true;
    }
  if(name=="ZS_SHORTZAPPED") {
    npc_percenable(self,PERC_ASSESSSTOPMAGIC,int(stopShortZapCallback.ptr));
    if(npc_hasbodyflag(self,npcRuleInt("BS_FLAG_INTERRUPTABLE")))
      ai_standupquick(self);
    else
      ai_standup(self);
    if(int32_t(self->guild)<npcRuleInt("GIL_SEPERATOR_HUM"))
      ai_playani(self,"T_STAND_2_LIGHTNING_VICTIM");
    return true;
    }
  if(name=="ZS_SHORTZAPPED_LOOP") {
    if(npc_getstatetime(self)>npcRuleInt("SPL_TIME_SHORTZAPPED"))
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

bool GameScript::runHumanFlee(Npc& npc, std::string_view name, int& result) {
  constexpr size_t loadedGame = 65; // AIV_LOADGAME
  constexpr std::string_view fleeOverlay = "HUMANS_FLEE.MDS";
  const auto self = std::static_pointer_cast<daedalus::INpc>(npc.handlePtr());
  if(name=="ZS_FLEE") {
    npc_percenable(self,PERC_ASSESSMAGIC,int(findFunction("B_ASSESSMAGIC").ptr));
    validateCombatTarget(self);
    if(self->aivar[loadedGame]==0)
      sayToNpc(self,nativeNpc("OTHER"),"$RUNAWAY",true);
    ai_removeweapon(self);
    ai_setwalkmode(self,npcRuleInt("NPC_RUN"));
    mdl_applyoverlaymds(self,fleeOverlay);
    result = 0;
    return true;
    }
  if(name=="ZS_FLEE_LOOP") {
    npc_gettarget(self);
    const auto target = nativeNpc("OTHER");
    if(npc_getdisttonpc(self,target)>npcRuleInt("FIGHT_DIST_CANCEL") ||
       isNpcDownInScript(target)) {
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

bool GameScript::runWatchFight(Npc& npc, std::string_view name, int& result) {
  constexpr size_t inPosition = 19; // AIV_TAPOSITION
  constexpr size_t lastTurn = 68; // AIV_STATETIME
  const auto self = std::static_pointer_cast<daedalus::INpc>(npc.handlePtr());
  const auto other = [&] { return nativeNpc("OTHER"); };
  const auto victim = [&] { return nativeNpc("VICTIM"); };
  const auto constant = [&](std::string_view name) { return npcRuleInt(name); };
  const auto guild = [](const std::shared_ptr<daedalus::INpc>& actor) { return actor ? int32_t(actor->guild) : 0; };
  const auto turn = [&](const std::shared_ptr<daedalus::INpc>& actor) {
    turnToNpc(self,actor);
    };
  auto& aivar = self->aivar;
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
      const bool tough = isToughGuy(self);
      const std::string_view line = tough ? "$THERESAFIGHT" : "$OHMYGODITSAFIGHT";
      sayToNpc(self,other(),line);
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
    isToughGuy(self);
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

bool GameScript::runFakeGuildComment(Npc& npc, std::string_view functionName, int& result) {
  if(functionName!="ZS_COMMENTFAKEGUILD")
    return false;
  const auto self = std::static_pointer_cast<daedalus::INpc>(npc.handlePtr());
  const auto other = [&] { return nativeNpc("OTHER"); };
  const auto seated = [&](const std::shared_ptr<daedalus::INpc>& actor) {
    return npcBodyStateContains(actor,int32_t(BS_SIT));
    };
  setPerceptionPreset(false);
  ai_standup(self);
  if(!seated(self))
    turnToNpc(self,other());
  if(!seated(other())) {
    turnToNpc(other(),self);
    if(npc_getdisttonpc(other(),self)<80)
      ai_dodge(other());
    }

  std::string_view line;
  if(npc_hasequippedarmor(other())) {
    storyActorSlot("ZS_COMMENTFAKEGUILD.ITM") = npc_getequippedarmor(other());
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
    sayToNpc(self,other(),line);
  result = 0;
  return true;
  }

bool GameScript::runHumanPlunder(Npc& npc, std::string_view name, int& result) {
  const auto self = std::static_pointer_cast<daedalus::INpc>(npc.handlePtr());
  const auto other = [&] { return nativeNpc("OTHER"); };
  const auto transfer = [&](int item, int amount) {
    createinvitems(self,item,amount);
    npc_removeinvitems(other(),item,amount);
    };
  const auto heal = [&] {
    if(self->attribute[NpcAttribute::HITPOINTS]<self->attribute[NpcAttribute::HITPOINTS_MAX]/2)
      ai_startstate(self,int(findFunction("ZS_HEALSELF").ptr),0,"");
    };
  if(name=="ZS_GETMEAT") {
    setPerceptionPreset(false);
    ai_standup(self);
    ai_gotonpc(self,other());
    ai_turntonpc(self,other());
    ai_playani(self,"T_PLUNDER");
    const auto meat = int(findSymbolIndex("ITFOMUTTONRAW"));
    const auto amount = npc_hasitems(other(),meat);
    setNpcRuleInt("ZS_GETMEAT.X",amount);
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

  turnToNpc(self,other());
  ai_playani(self,"T_PLUNDER");
  const auto questStock = [&](std::string_view owner, int item) {
    const auto ownerId = hlp_getinstanceid(storyActorSlot(owner));
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
    setNpcRuleInt("ZS_RANSACKBODY_END.X",amount);
    transfer(gold,amount);
    }
  const std::string_view line = amount>0 ? "$ITOOKYOURGOLD" : "$SHITNOGOLD";
  sayToNpc(self,other(),line);

  npc_perceiveall(self);
  // Both scans can update ITEM. Melee weapons take precedence when both find a candidate.
  const bool foundRanged = wld_detectitem(self,npcRuleInt("ITEM_KAT_FF"));
  const bool foundMelee = wld_detectitem(self,npcRuleInt("ITEM_KAT_NF"));
  if(foundRanged || foundMelee) {
    const auto item = std::dynamic_pointer_cast<daedalus::IItem>(storyActorSlot("ITEM"));
    if(hlp_isvaliditem(item) && npc_getdisttoitem(self,item)<500) {
      ai_takeitem(self,item);
      sayToNpc(self,self,"$ITAKEYOURWEAPON");
      ai_equipbestmeleeweapon(self);
      ai_equipbestrangedweapon(self);
      }
    }
  heal();
  result = 0;
  return true;
  }

void GameScript::sayAttackReason() {
  constexpr size_t attackReason = 9; // AIV_ATTACKREASON
  constexpr size_t partyMember = 15; // AIV_PARTYMEMBER
  using Reason = AttackAnnouncementReason;

  const auto self = nativeNpc("SELF");
  const auto other = nativeNpc("OTHER");
  if(self==nullptr)
    return;
  const auto reason = Reason(self->aivar[attackReason]);
  const auto guild = other ? int32_t(other->guild) : int32_t(GIL_NONE);
  const auto say = [&](std::string_view line) { sayToNpc(self,other,line,true); };
  const auto fightSound = [&] { npc_sendpassiveperc(self,PERC_ASSESSFIGHTSOUND,self,other); };

  const bool personalHostility = npc_getattitude(self,other)==ATT_HOSTILE;
  const bool guildHostility = wld_getguildattitude(int32_t(self->guild),guild)==ATT_HOSTILE;
  if(personalHostility && !guildHostility) {
    if(reason!=Reason::Kill)
      say("$IGETYOUSTILL");
    return;
    }
  if(reason==Reason::GuildEnemy) {
    if(self->aivar[partyMember]==1) {
      const auto roll = hlp_random(100);
      setNpcRuleInt("B_SAY_ATTACKREASON.RND",roll);
      if(roll>15)
        return;
      }
    if(guild<int32_t(GIL_SEPERATOR_HUM)) {
      if(isPlayerFakeBandit(self,other))
        say("$ADDON_DIEBANDIT");
      else if(isPlayerFakePirate(self,other))
        say("$ADDON_DIRTYPIRATE");
      else
        say("$DIEENEMY");
      } else if(self->voice==9) {
      const auto roll = hlp_random(3);
      setNpcRuleInt("B_SAY_ATTACKREASON.RANDOM",roll);
      static constexpr std::string_view lines[] = {"$DIEMONSTER","$ADDON_DIEMONSTER","$ADDON_DIEMONSTER2"};
      say(lines[size_t(roll)]);
      } else {
      say("$DIEMONSTER");
      }
    fightSound();
    return;
    }

  switch(reason) {
    case Reason::Theft:                 say("$DIRTYTHIEF"); break;
    case Reason::UseMob:                say("$HANDSOFF"); break;
    case Reason::SheepKiller:           say(guild<int32_t(GIL_SEPERATOR_HUM) ? "$SHEEPKILLER" : "$SHEEPKILLERMONSTER"); break;
    case Reason::HumanMurderedHuman:    say("$YOUMURDERER"); fightSound(); break;
    case Reason::MonsterVsHuman:        say("$DIEMONSTER"); fightSound(); break;
    case Reason::MonsterCloseToGate:    say("$DIESTUPIDBEAST"); break;
    case Reason::ReactToDamage:         say(guild>int32_t(GIL_SEPERATOR_HUM) ? "$YOUASKEDFORIT" : "$YOUDAREHITME"); break;
    case Reason::ReactToWeapon:         say("$YOUASKEDFORIT"); break;
    case Reason::ClearRoom:             say("$THENIBEATYOUOUTOFHERE"); break;
    case Reason::LeftPortalRoom:        say("$WHATDIDYOUDOINTHERE"); break;
    case Reason::GuardStopsFight:       say("$WILLYOUSTOPFIGHTING"); break;
    default: break;
    }
  }

void GameScript::sayAttackEnd() {
  constexpr size_t lastTarget = 7; // AIV_LASTTARGET
  constexpr size_t attackReason = 9; // AIV_ATTACKREASON
  constexpr size_t partyMember = 15; // AIV_PARTYMEMBER
  constexpr size_t killedByPlayer = 67; // AIV_KILLEDBYPLAYER
  using Reason = AttackAnnouncementReason;

  const auto self = nativeNpc("SELF");
  const auto other = nativeNpc("OTHER");
  if(self==nullptr)
    return;
  const auto reason = Reason(self->aivar[attackReason]);
  const auto guild = other ? int32_t(other->guild) : int32_t(GIL_NONE);
  const auto say = [&](std::string_view line) { sayToNpc(self,other,line); };
  const auto sayHumanEnd = [&](std::string_view ownTarget) {
    if(npc_isdead(other))
      say("$ENEMYKILLED");
    else if(self->aivar[lastTarget]==hlp_getinstanceid(other))
      say(ownTarget);
    else
      say("$GOODKILL");
    };
  if(reason==Reason::GuildEnemy || reason==Reason::MonsterMurderedHuman) {
    if(guild<int32_t(GIL_SEPERATOR_HUM)) {
      sayHumanEnd("$KILLENEMY");
      return;
      }
    if(self->aivar[partyMember]==1) {
      const auto roll = hlp_random(100);
      setNpcRuleInt("B_SAY_ATTACKEND.RND",roll);
      if(guild!=GIL_DRAGON && roll>15)
        return;
      }
    if(other && other->aivar[killedByPlayer]!=0) {
      say("$GOODMONSTERKILL");
      } else if(self->voice==9) {
      const auto roll = hlp_random(2);
      setNpcRuleInt("B_SAY_ATTACKEND.RANDOM",roll);
      say(roll==0 ? "$ADDON_MONSTERKILLED" : "$ADDON_MONSTERKILLED2");
      } else if(self->voice==12) {
      static constexpr std::string_view gorn[] = {"GORN_DI","GORNNW_NACH_DJG","GORNNW_VOR_DJG","GORNDJG","GORNOW"};
      const auto selfId = hlp_getinstanceid(self);
      if(std::ranges::any_of(gorn,[&](std::string_view name) { return selfId==hlp_getinstanceid(storyActorSlot(name)); })) {
        const auto roll = hlp_random(2);
        setNpcRuleInt("B_SAY_ATTACKEND.RANDY",roll);
        say(roll==0 ? "$ADDON_MONSTERKILLED" : "$MONSTERKILLED");
        } else {
        say("$ADDON_MONSTERKILLED");
        }
      } else {
      say("$MONSTERKILLED");
      }
    return;
    }

  switch(reason) {
    case Reason::Theft:                 say("$THIEFDOWN"); break;
    case Reason::UseMob:
    case Reason::GuardCalledToThief:    say("$RUMFUMMLERDOWN"); break;
    case Reason::SheepKiller:           say(guild<int32_t(GIL_SEPERATOR_HUM) ? "$SHEEPATTACKERDOWN" : "$MONSTERKILLED"); break;
    case Reason::HumanMurderedHuman:    sayHumanEnd("$KILLMURDERER"); break;
    case Reason::MonsterVsHuman:        say(self->voice==12 || self->voice==9 ? "$ADDON_MONSTERKILLED" : "$MONSTERKILLED"); break;
    case Reason::MonsterCloseToGate:    say("$STUPIDBEASTKILLED"); break;
    case Reason::ReactToDamage:         say("$NEVERHITMEAGAIN"); break;
    case Reason::ReactToWeapon:         say("$YOUBETTERSHOULDHAVELISTENED"); break;
    case Reason::GuardCalledToRoom:
    case Reason::ClearRoom:             say(isBotheredByPlayerRoomGuild(self) ? "$GETUPANDBEGONE" : "$NEVERENTERROOMAGAIN"); break;
    case Reason::LeftPortalRoom:        say("$NEVERENTERROOMAGAIN"); break;
    case Reason::GuardStopsIntruder:    say("$KILLENEMY"); break;
    case Reason::GuardStopsFight:       if(guild!=GIL_DJG && guild!=GIL_SLD) say("$THEREISNOFIGHTINGHERE"); break;
    default: break;
    }
  }

void GameScript::rememberFleeCheck(const std::shared_ptr<daedalus::INpc>& self, const std::shared_ptr<daedalus::INpc>& other) {
  // The verified program always returns FALSE after assigning the parameters.
  storyActorSlot("C_WANTTOFLEE.OTH") = other;
  storyActorSlot("C_WANTTOFLEE.SLF") = self;
  }

bool GameScript::alwaysRansacks(const std::shared_ptr<daedalus::INpc>& self) {
  storyActorSlot("C_NPCRANSACKSALWAYS.SLF") = self;
  const auto other = nativeNpc("OTHER");
  const auto id = hlp_getinstanceid(self);
  const auto carriesItemFor = [&](std::string_view owner, std::string_view item) {
    return npc_hasitems(other,int(findSymbolIndex(item)))>0 && hlp_getinstanceid(storyActorSlot(owner))==id;
    };
  return carriesItemFor("GARWIG","HOLY_HAMMER_MIS") || carriesItemFor("ROD","ITMW_2H_ROD");
  }

bool GameScript::wantsToRansack(const std::shared_ptr<daedalus::INpc>& self) {
  storyActorSlot("C_WANTTORANSACK.SLF") = self;
  const auto other = nativeNpc("OTHER");
  if(self==nullptr || (npc_isplayer(other) && self->type==NpcType::G2_FRIEND) ||
     hlp_getinstanceid(self)==hlp_getinstanceid(storyActorSlot("RAVEN")))
    return false;
  const auto guild = self->guild;
  return guild!=GIL_KDW && guild!=GIL_NOV && guild!=GIL_KDF && guild!=GIL_PAL && guild!=GIL_ORC && guild!=GIL_DMT;
  }

void GameScript::queueFinishingMove(const std::shared_ptr<daedalus::INpc>& self, const std::shared_ptr<daedalus::INpc>& other) {
  storyActorSlot("B_FINISHINGMOVE.OTH") = other;
  storyActorSlot("B_FINISHINGMOVE.SLF") = self;
  if(!npc_isinfightmode(self,int(FightMode::FMODE_MELEE)))
    npc_settofightmode(self,int(findSymbolIndex("ITMW_1H_MISC_SWORD")));
  ai_finishingmove(self,other);
  }

bool GameScript::runHumanAttack(Npc& npc, std::string_view name, int& result) {
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
  const auto other = [&] { return nativeNpc("OTHER"); };
  const auto hero = [&] { return nativeNpc("HERO"); };
  const auto constant = [&](std::string_view name) { return npcRuleInt(name); };
  const auto absolution = [&] { return currentAbsolutionLevel(self); };
  const auto bodyState = [&](int32_t body) { return npcBodyStateContains(other(),body); };
  const auto targetDown = [&] { return isNpcDownInScript(other()); };
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
  if(name=="ZS_ATTACK") {
    result = 0;
    setPerceptionPreset(false);
    npc_percenable(self,PERC_ASSESSSURPRISE,int(findFunction("B_ASSESSSURPRISE").ptr));
    validateCombatTarget(self);
    aivar[lastTarget] = hlp_getinstanceid(other());
    rememberFleeCheck(self,other());
    if(aivar[loadedGame]==0)
      sayAttackReason();
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
    runGregRoutineTransition(*self);
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
      callGuards();
      aivar[guardCalls] = 1;
      }
    if(aivar[guardCalls]==1 && npc_getstatetime(self)>8) {
      callGuards();
      aivar[guardCalls] = 2;
      }
    createAmmo(self);
    selectWeapon(self,other());
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
  storyActorSlot("OTHER") = hlp_getnpc(aivar[lastTarget]);
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
      sayAttackEnd();
    }
  if(wld_getguildattitude(int(self->guild),guild(hero()))!=ATT_HOSTILE && targetValue(killedByPlayer)==1)
    setCombatAttitude(self,ATT_FRIENDLY);
  if(hasAttackReasonToKill(self) && npc_isinstate(other(),int(ZS_Unconscious)))
    queueFinishingMove(self,other());
  ai_removeweapon(self);
  const bool near = npc_getdisttonpc(self,other())<constant("PERC_DIST_INTERMEDIAT");
  const bool alwaysRansack = alwaysRansacks(self);
  const bool robbed = targetValue(ransacked)!=0;
  const bool wantToRansack = wantsToRansack(self);
  const bool down = targetDown();
  if(near && (alwaysRansack || !robbed) && wantToRansack && down) {
    if(const auto target = other())
      target->aivar[ransacked] = 1;
    if(guild(other())<constant("GIL_SEPERATOR_HUM")) {
      start("ZS_RANSACKBODY");
      return true;
      }
    if(npc_hasitems(other(),int(findSymbolIndex("ITFOMUTTONRAW")))>0 &&
       hlp_getinstanceid(storyActorSlot("ALLIGATORJACK"))==hlp_getinstanceid(self)) {
      start("ZS_GETMEAT");
      return true;
      }
    }
  if(self->attribute[NpcAttribute::HITPOINTS]<self->attribute[NpcAttribute::HITPOINTS_MAX]/2)
    start("ZS_HEALSELF");
  return true;
  }

bool GameScript::runHumanReaction(Npc& npc, std::string_view name, int& result) {
  constexpr size_t lastFightAgainstPlayer = 0; // AIV_LASTFIGHTAGAINSTPLAYER
  constexpr size_t repeatedWarning = 19; // AIV_TAPOSITION
  constexpr size_t lastPlayerAttackReason = 47; // AIV_LASTPLAYERAR
  constexpr size_t turnTime = 68; // AIV_STATETIME
  const auto self = std::static_pointer_cast<daedalus::INpc>(npc.handlePtr());
  const auto other = [&] { return storyActorSlot("OTHER"); };
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
    selectWeapon(self,otherNpc());
    turnToOther();
    if(weapon) {
      auto& comment = *findSymbol("PLAYER_DRAWWEAPONCOMMENT");
      if(npc_isplayer(otherNpc()) && aivar[lastPlayerAttackReason]!=npcRuleInt("AR_NONE") &&
         aivar[lastFightAgainstPlayer]==npcRuleInt("FIGHT_WON")) {
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
      const bool farAway = npc_getdisttonpc(self,otherNpc())>npcRuleInt("PERC_DIST_INTERMEDIAT");
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
        startNpcAttack(self,otherNpc(),npcRuleInt("AR_REACTTOWEAPON"),0);
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

bool GameScript::runObservePlayer(Npc& npc, std::string_view name, int& result) {
  constexpr size_t seenLeftRoom = 21; // AIV_SEENLEFTROOM in the verified program
  constexpr size_t turnInterval = 68; // AIV_STATETIME in the verified program
  const auto self = std::static_pointer_cast<daedalus::INpc>(npc.handlePtr());
  // Read OTHER from the live script context after each shared helper.
  const auto otherNpc = [&] { return nativeNpc("OTHER"); };
  const auto hasBodyState = [&](const std::shared_ptr<daedalus::INpc>& actor, int32_t state) {
    return ActivityDefinitions::bodyStateContains(npc_getbodystate(actor),state);
    };
  const auto say = [&](std::string_view line) {
    sayToNpc(self,otherNpc(),line);
    };
  const auto turnToOther = [&] {
    turnToNpc(self,otherNpc());
    };

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
    if(distance>npcRuleInt("PERC_DIST_INTERMEDIAT")) {
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
  const auto self = nativeNpc("SELF");
  const auto guild = wld_getplayerportalguild();
  setNpcRuleInt("B_EXITIFROOMLEFT.PORTALGUILD",guild);
  const bool bothered = isBotheredByPlayerRoomGuild(self);
  if(guild!=int32_t(GIL_PUBLIC) && bothered)
    return 0;

  npc_clearaiqueue(self);
  ai_standup(self);
  stopLookAtNpc(self);
  const bool wantsAttack = wantsToAttackRoomIntruder(self);
  sayToNpc(self,nativeNpc("OTHER"),
           wantsAttack ? "$YESGOOUTOFHERE" : "$WISEMOVE");
  ai_continueroutine(self);
  return 1;
  }

bool GameScript::runClearRoom(Npc& npc, std::string_view name, int& result) {
  if(!nativeRoomReactionsEnabled())
    return false;
  constexpr size_t routinePosition = 19; // AIV_TAPOSITION in the verified program
  constexpr int32_t notInPosition = 1;
  constexpr int32_t inPosition = 0;
  const auto self = std::static_pointer_cast<daedalus::INpc>(npc.handlePtr());
  const auto other = [&] { return storyActorSlot("OTHER"); };
  const auto otherNpc = [&] { return std::dynamic_pointer_cast<daedalus::INpc>(other()); };
  const auto gotoOther = [&] { ai_gotowp(self,npc_getnearestwp(otherNpc())); };
  const auto turnToOther = [&] { turnToNpc(self,otherNpc()); };
  const auto say = [&](std::string_view line) { sayToNpc(self,otherNpc(),line); };
  const auto wantsAttack = [&] { return wantsToAttackRoomIntruder(self); };

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
        selectWeapon(self,otherNpc());
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
        const auto reason = npcRuleInt("AR_CLEARROOM");
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

bool GameScript::wantsToAttackRoomIntruder(const std::shared_ptr<daedalus::INpc>& self) {
  storyActorSlot("C_WANTTOATTACKROOMINTRUDER.SLF") = self;
  const auto owner = nativeNpc("SELF");
  const auto crime = playerCrime(owner);
  const auto guild = owner ? uint32_t(owner->guild) : GIL_NONE;
  return crime!=npcRuleInt("CRIME_NONE") || guild==GIL_DJG || guild==GIL_SLD || guild==GIL_PAL || guild==GIL_MIL;
  }

int32_t GameScript::playerCrime(const std::shared_ptr<daedalus::INpc>& self) {
  storyActorSlot("B_GETPLAYERCRIME.SLF") = self;
  constexpr size_t crimeType = 1, crimeDay = 2, crimeAbsolution = 46;
  const auto none = npcRuleInt("CRIME_NONE");
  const auto crime = self ? self->aivar[crimeType] : 0;
  if(crime<=npcRuleInt("CRIME_ATTACK") && (self ? self->aivar[crimeDay] : 0)<wld_getday()-2)
    return none;
  const auto recordedLevel = self ? self->aivar[crimeAbsolution] : 0;
  return recordedLevel<currentAbsolutionLevel(self) ? none : crime;
  }

void GameScript::memorizePlayerCrime(const std::shared_ptr<daedalus::INpc>& witness,
                                     const std::shared_ptr<daedalus::INpc>& offender, int32_t crime) {
  setNpcRuleInt("B_MEMORIZEPLAYERCRIME.CRIME",crime);
  storyActorSlot("B_MEMORIZEPLAYERCRIME.TAETER") = offender;
  storyActorSlot("B_MEMORIZEPLAYERCRIME.SLF") = witness;
  if(!npc_isplayer(offender) || witness==nullptr || witness->guild==GIL_DMT)
    return;

  if(crime>playerCrime(witness)) {
    adjustPetzCrime(witness,std::nullopt);
    adjustPetzCrime(witness,crime);
    }
  if(crime>=playerCrime(witness)) {
    constexpr size_t crimeType = 1, crimeDay = 2, crimeAbsolution = 46;
    witness->aivar[crimeType] = crime;
    witness->aivar[crimeDay] = wld_getday();
    witness->aivar[crimeAbsolution] = currentAbsolutionLevel(witness);
    }
  }

namespace {

constexpr std::array<std::string_view,4> petzCrimeKinds={
  "CRIME_MURDER","CRIME_THEFT","CRIME_ATTACK","CRIME_SHEEPKILLER",
  };

constexpr struct {
  std::string_view witnessSlot;
  std::string_view absolution;
  std::array<std::string_view,4> counters;
  } crimeJurisdictions[] = {
  {"C_NPCBELONGSTOOLDCAMP.SLF",   "ABSOLUTIONLEVEL_OLDCAMP",
   {"PETZCOUNTER_OLDCAMP_MURDER",   "PETZCOUNTER_OLDCAMP_THEFT",   "PETZCOUNTER_OLDCAMP_ATTACK",   "PETZCOUNTER_OLDCAMP_SHEEPKILLER"}},
  {"C_NPCBELONGSTOCITY.SLF",      "ABSOLUTIONLEVEL_CITY",
   {"PETZCOUNTER_CITY_MURDER",      "PETZCOUNTER_CITY_THEFT",      "PETZCOUNTER_CITY_ATTACK",      "PETZCOUNTER_CITY_SHEEPKILLER"}},
  {"C_NPCBELONGSTOMONASTERY.SLF", "ABSOLUTIONLEVEL_MONASTERY",
   {"PETZCOUNTER_MONASTERY_MURDER", "PETZCOUNTER_MONASTERY_THEFT", "PETZCOUNTER_MONASTERY_ATTACK", "PETZCOUNTER_MONASTERY_SHEEPKILLER"}},
  {"C_NPCBELONGSTOFARM.SLF",      "ABSOLUTIONLEVEL_FARM",
   {"PETZCOUNTER_FARM_MURDER",      "PETZCOUNTER_FARM_THEFT",      "PETZCOUNTER_FARM_ATTACK",      "PETZCOUNTER_FARM_SHEEPKILLER"}},
  {"C_NPCBELONGSTOBL.SLF",        "ABSOLUTIONLEVEL_BL",
   {"PETZCOUNTER_BL_MURDER",        "PETZCOUNTER_BL_THEFT",        "PETZCOUNTER_BL_ATTACK",        {}}},
  };

std::array<bool,std::size(crimeJurisdictions)> crimeJurisdictionMembership(const daedalus::INpc* witness) {
  const auto guild = witness ? uint32_t(witness->guild) : GIL_NONE;
  const auto type = witness ? witness->type : NpcType::G2_AMBIENT;
  const bool city = guild==GIL_PAL || guild==GIL_MIL || guild==GIL_VLK;
  const bool oldCamp = city && (type==NpcType::G2_OC_AMBIENT || type==NpcType::G2_OC_MAIN);
  return {oldCamp,city && !oldCamp,guild==GIL_NOV || guild==GIL_KDF,guild==GIL_SLD || guild==GIL_BAU,
          type==NpcType::G2_BL_MAIN || type==NpcType::G2_BL_AMBIENT};
  }

}

void GameScript::loadNativeCrimeReports() {
  using Actor=std::shared_ptr<daedalus::INpc>;
  enum class Query { Greatest, Total, Absolution };
  static constexpr struct Report {
    const char* function;
    const char* witnessSlot;
    Query query;
    } reports[]={
    {"B_GETGREATESTPETZCRIME","B_GETGREATESTPETZCRIME.SLF",Query::Greatest},
    {"B_GETTOTALPETZCOUNTER","B_GETTOTALPETZCOUNTER.SLF",Query::Total},
    {"B_GETCURRENTABSOLUTIONLEVEL","B_GETCURRENTABSOLUTIONLEVEL.SLF",Query::Absolution},
    };
  for(const auto& report:reports) {
    auto callback = std::function<int32_t(Actor)>{[this,report](const Actor& witness) {
      storyActorSlot(report.witnessSlot) = witness;
      const auto membership=crimeJurisdictionMembership(witness.get());
      for(size_t i=0;i<std::size(crimeJurisdictions);++i) {
        const auto& jurisdiction=crimeJurisdictions[i];
        storyActorSlot(jurisdiction.witnessSlot) = witness;
        if(!membership[i])
          continue;
        if(report.query==Query::Absolution)
          return crimeAbsolutionLevels[i];
        uint32_t total=0;
        for(size_t j=0;j<jurisdiction.counters.size() && !jurisdiction.counters[j].empty();++j) {
          const auto count=petzCrimeCounters[i*jurisdiction.counters.size()+j];
          if(report.query==Query::Greatest && count>0)
            return npcRuleInt(petzCrimeKinds[j]);
          total+=uint32_t(count);
          }
        if(report.query==Query::Total)
          return std::bit_cast<int32_t>(total);
        }
      return report.query==Query::Greatest ? npcRuleInt("CRIME_NONE") : 0;
      }};
    nativeExternals.insert_or_assign(report.function,storygraph::makeNativeInternal(callback));
    }
  }

void GameScript::bindCrimeGlobals() {
  for(size_t i=0; i<std::size(crimeJurisdictions); ++i) {
    const auto& jurisdiction = crimeJurisdictions[i];
    bindRuntimeVariable(jurisdiction.absolution,crimeAbsolutionLevels[i]);
    for(size_t j=0; j<jurisdiction.counters.size(); ++j) {
      if(!jurisdiction.counters[j].empty())
        bindRuntimeVariable(jurisdiction.counters[j],petzCrimeCounters[i*jurisdiction.counters.size()+j]);
      }
    }
  }

int32_t GameScript::currentAbsolutionLevel(const std::shared_ptr<daedalus::INpc>& witness) {
  storyActorSlot("B_GETCURRENTABSOLUTIONLEVEL.SLF") = witness;
  const auto membership = crimeJurisdictionMembership(witness.get());
  for(size_t i=0; i<std::size(crimeJurisdictions); ++i) {
    const auto& jurisdiction = crimeJurisdictions[i];
    storyActorSlot(jurisdiction.witnessSlot) = witness;
    if(membership[i])
      return crimeAbsolutionLevels[i];
    }
  return 0;
  }

void GameScript::adjustPetzCrime(const std::shared_ptr<daedalus::INpc>& witness, std::optional<int32_t> newCrime) {
  if(newCrime) {
    setNpcRuleInt("B_ADDPETZCRIME.CRIME",*newCrime);
    storyActorSlot("B_ADDPETZCRIME.SLF") = witness;
    } else {
    storyActorSlot("B_DELETEPETZCRIME.SLF") = witness;
    }
  const auto membership = crimeJurisdictionMembership(witness.get());
  for(size_t i=0; i<std::size(crimeJurisdictions); ++i) {
    const auto& jurisdiction = crimeJurisdictions[i];
    storyActorSlot(jurisdiction.witnessSlot) = witness;
    if(!membership[i])
      continue;
    const auto crime = newCrime ? *newCrime : playerCrime(witness);
    for(size_t j=0; j<petzCrimeKinds.size(); ++j) {
      if(jurisdiction.counters[j].empty() || crime!=npcRuleInt(petzCrimeKinds[j]))
        continue;
      auto& counter = petzCrimeCounters[i*jurisdiction.counters.size()+j];
      counter += newCrime ? 1 : -1;
      }
    }
  }

bool GameScript::belongsToCity(const std::shared_ptr<daedalus::INpc>& npc) {
  constexpr size_t city = 1; // crimeJurisdictions index
  storyActorSlot("C_NPCBELONGSTOCITY.SLF") = npc;
  storyActorSlot("C_NPCBELONGSTOOLDCAMP.SLF") = npc;
  return crimeJurisdictionMembership(npc.get())[city];
  }

bool GameScript::isToughGuy(const std::shared_ptr<daedalus::INpc>& self) {
  storyActorSlot("C_NPCISTOUGHGUY.SLF") = self;
  constexpr size_t tough = 57;
  const auto guild = self ? uint32_t(self->guild) : GIL_NONE;
  return (self && self->aivar[tough]==1) || guild==GIL_PIR || guild==GIL_BDT || guild==GIL_DJG || guild==GIL_SLD;
  }

bool GameScript::wantsToCallGuards(const std::shared_ptr<daedalus::INpc>& npc) {
  storyActorSlot("C_WANTTOCALLGUARDS.SLF") = npc;
  const auto self = nativeNpc("SELF");
  constexpr size_t partyMember = 15; // AIV_PARTYMEMBER
  if(self==nullptr || self->aivar[partyMember]!=0 || npc==nullptr)
    return false;
  const auto guild = npc->guild;
  return guild==GIL_BAU || guild==GIL_SLD || guild==GIL_VLK || guild==GIL_MIL || guild==GIL_PAL;
  }

void GameScript::callGuards() {
  const auto self = nativeNpc("SELF");
  if(!wantsToCallGuards(self))
    return;
  constexpr size_t attackReason = 9; // AIV_ATTACKREASON
  const auto reason = [&](std::string_view name) { return self->aivar[attackReason]==npcRuleInt(name); };
  const auto shout = [&](std::string_view line, bool sendFightSound) {
    const auto other = nativeNpc("OTHER");
    sayToNpc(self,other,line,true);
    if(sendFightSound)
      npc_sendpassiveperc(self,PERC_ASSESSFIGHTSOUND,self,other);
    };
  const auto callForHelp = [&] {
    const auto guild = self->guild;
    if(guild==GIL_SLD || guild==GIL_PAL || guild==GIL_MIL)
      shout("$ALARM",true);
    else if(!isToughGuy(self))
      shout("$GUARDS",true);
    };

  if(reason("AR_GUARDCALLEDTOROOM") || reason("AR_GUARDCALLEDTOTHIEF") ||
     reason("AR_GUARDSTOPSFIGHT") || reason("AR_GUARDCALLEDTOKILL")) {
    shout("$ALARM",false);
    return;
    }
  if(reason("AR_HUMANMURDEREDHUMAN") || reason("AR_GUILDENEMY")) {
    callForHelp();
    return;
    }
  if(reason("AR_GUARDSTOPSINTRUDER")) {
    shout("$ALARM",true);
    return;
    }
  if(reason("AR_USEMOB") || reason("AR_THEFT") || reason("AR_REACTTOWEAPON")) {
    if(!isToughGuy(self))
      shout("$GUARDS",true);
    return;
    }
  if(reason("AR_CLEARROOM"))
    callForHelp();
  }

void GameScript::startNpcAttack(const std::shared_ptr<daedalus::INpc>& attacker,
                                const std::shared_ptr<daedalus::INpc>& target, int32_t reason, int32_t wait) {
  setNpcRuleInt("B_ATTACK.WAIT",wait);
  setNpcRuleInt("B_ATTACK.ATTACK_REASON",reason);
  storyActorSlot("B_ATTACK.OTH") = target;
  storyActorSlot("B_ATTACK.SLF") = attacker;
  const auto self = [&] { return nativeNpc("B_ATTACK.SLF"); };
  const auto other = [&] { return nativeNpc("B_ATTACK.OTH"); };
  const auto value = [this](std::string_view name) { return npcRuleInt(name); };
  const auto current = [&](uint16_t index) {
    const auto actor = self();
    return actor ? actor->aivar[index] : 0;
    };
  const auto assign = [&](uint16_t index,int32_t val) {
    if(const auto actor = self())
      actor->aivar[index] = val;
    };

  constexpr uint16_t invincible = 4, lastTarget = 7, attackReason = 9, waitBeforeAttack = 23;
  constexpr uint16_t lastPlayerReason = 47, enemyOverride = 61, lastFightComment = 64;
  const auto no = value("FALSE");
  assign(waitBeforeAttack,npcRuleInt("B_ATTACK.WAIT"));
  if(npcRuleInt("B_ATTACK.ATTACK_REASON")==value("AR_SUDDENENEMYINFERNO")) {
    assign(enemyOverride,no);
    npc_sendpassiveperc(self(),PERC_ASSESSFIGHTSOUND,self(),other());
    }

  if(npc_isinstate(self(),int(ZS_Talk))) {
    assign(invincible,no);
    if(const auto actor = other())
      actor->aivar[invincible] = no;
    }

  const auto targetMemory = self();
  const auto targetId = hlp_getinstanceid(other());
  const bool sameTarget = (targetMemory ? targetMemory->aivar[lastTarget] : 0)==targetId;
  const bool attacking = npc_isinstate(self(),int(ZS_Attack));
  bool changeReason = true;
  if(sameTarget && attacking) {
    const bool strongerReason = npcRuleInt("B_ATTACK.ATTACK_REASON")>current(attackReason);
    const bool wantsToKill = hasAttackReasonToKill(self());
    changeReason = strongerReason && !wantsToKill;
    }
  if(changeReason) {
    assign(attackReason,npcRuleInt("B_ATTACK.ATTACK_REASON"));
    if(npc_isplayer(other()))
      assign(lastPlayerReason,npcRuleInt("B_ATTACK.ATTACK_REASON"));
    }

  const auto hasReason = [&](std::string_view name) { return current(attackReason)==value(name); };
  const auto memorize = [&](std::string_view name) {
    memorizePlayerCrime(self(),other(),value(name));
    };
  if(hasReason("AR_SHEEPKILLER"))
    memorize("CRIME_SHEEPKILLER");
  if(hasReason("AR_REACTTOWEAPON") || hasReason("AR_REACTTODAMAGE")) {
    const auto actor = self();
    const bool friendNpc = actor && actor->type==NpcType::G2_FRIEND;
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
  if(hasReason("AR_KILL"))
    setCombatAttitude(self(),ATT_HOSTILE);
  if(npc_isplayer(other())) {
    assign(0,value("FIGHT_CANCEL"));
    assign(lastFightComment,no);
    }
  if(!npc_isinstate(self(),int(ZS_Talk)))
    npc_clearaiqueue(self());
  clearPerceptions(self());
  npc_settarget(self(),other());
  const auto mustStand = npcBodyStateContains(self(),BS_LIE);
  ai_startstate(self(),int(ZS_Attack),mustStand ? 1 : 0,"");
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
  vm.override_function("B_SELECTSPELL",std::function<bool(NpcRef,NpcRef)>{[this](NpcRef self,NpcRef other) {
    return chooseCombatSpell(self,other);
    }},enabled);
  vm.override_function("B_READYSPELL",std::function<void(NpcRef,int32_t,int32_t)>{[this](NpcRef self,int32_t spell,int32_t mana) {
    readySpell(self,spell,mana);
    }},enabled);
  vm.override_function("C_NPCHASATTACKREASONTOKILL",std::function<bool(NpcRef)>{[this](NpcRef self) {
    return hasAttackReasonToKill(self);
    }},enabled);
  vm.override_function("C_NPCISUNDEAD",std::function<bool(NpcRef)>{[this](NpcRef npc) {
    return isUndeadNpc(npc);
    }},enabled);
  vm.override_function("C_NPCISEVIL",std::function<bool(NpcRef)>{[this](NpcRef npc) {
    return isEvilNpc(npc);
    }},enabled);
  }

void GameScript::createAmmo(const std::shared_ptr<daedalus::INpc>& self) {
  storyActorSlot("B_CREATEAMMO.SLF") = self;
  std::shared_ptr<daedalus::IItem> weapon;
  if(npc_isinfightmode(self,int32_t(FightMode::FMODE_FAR)))
    weapon = npc_getreadiedweapon(self);
  else if(npc_hasequippedrangedweapon(self))
    weapon = npc_getequippedrangedweapon(self);
  else
    return;
  storyActorSlot("B_CREATEAMMO.RANGEDWEAPON") = weapon;
  if(!weapon)
    return;

  const auto ammo = weapon->munition;
  if((ammo==int32_t(findSymbolIndex("ITRW_ARROW")) || ammo==int32_t(findSymbolIndex("ITRW_BOLT"))) && npc_hasitems(self,ammo)<10)
    createinvitems(self,ammo,10);
  }

void GameScript::selectWeapon(const std::shared_ptr<daedalus::INpc>& self, const std::shared_ptr<daedalus::INpc>& other) {
  storyActorSlot("B_SELECTWEAPON.OTH") = other;
  storyActorSlot("B_SELECTWEAPON.SLF") = self;
  const auto body = npc_getbodystate(self);
  // All three stock checks run before branching, leaving the last helper arguments in its symbols.
  storyActorSlot("C_BODYSTATECONTAINS.SLF") = self;
  setNpcRuleInt("C_BODYSTATECONTAINS.BODYSTATE",BS_FALL);
  if(ActivityDefinitions::bodyStateContains(body,BS_DIVE) || ActivityDefinitions::bodyStateContains(body,BS_SWIM) ||
     ActivityDefinitions::bodyStateContains(body,BS_FALL))
    return;
  if(chooseCombatSpell(self,other))
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
  const auto inner = npcRuleInt("FIGHT_DIST_RANGED_INNER");
  const auto outer = npcRuleInt("FIGHT_DIST_RANGED_OUTER");
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
  if(hasAttackReasonToKill(self) && distance>inner && ranged) {
    ready(true);
    return;
    }
  if(mode(FightMode::FMODE_NONE))
    ready(!melee && ranged);
  }

bool GameScript::chooseCombatSpell(const std::shared_ptr<daedalus::INpc>& self, const std::shared_ptr<daedalus::INpc>& other) {
  storyActorSlot("B_SELECTSPELL.OTH") = other;
  storyActorSlot("B_SELECTSPELL.SLF") = self;
  constexpr size_t selection = 20; // AIV_SELECTSPELL
  constexpr size_t species = 43;   // AIV_MM_REAL_ID
  constexpr size_t magicUser = 51;  // AIV_MAGICUSER
  const auto number = [&](std::string_view name) { return npcRuleInt(name); };
  const auto grant = [&](std::initializer_list<std::string_view> items) {
    for(const auto name:items) {
      const auto item = int32_t(findSymbolIndex(name));
      if(npc_hasitems(self,item)==0)
        createinvitems(self,item,1);
      }
    };
  const auto ready = [&](std::string_view spell, std::string_view cost) {
    readySpell(self,number(spell),number(cost));
    return true;
    };
  const auto distance = [&] { return npc_getdisttonpc(self,other); };
  const auto frozen = [&] { return npc_isinstate(other,int32_t(findSymbolIndex("ZS_MAGICFREEZE"))); };

  const auto guild = self ? self->guild : 0;
  const auto otherGuild = other ? other->guild : 0;
  const auto magic = self && self->aivar[magicUser]==number("MAGIC_ALWAYS");
  const auto player = npc_isplayer(other);
  if(otherGuild<int32_t(GIL_SEPERATOR_HUM) && player && self && self->type==NpcType::G2_FRIEND) {
    if(!magic && guild!=GIL_KDF)
      return false;
    grant({"ITRU_SLEEP"});
    return ready("SPL_SLEEP","SPL_COST_SLEEP");
    }

  if(magic && guild==GIL_DMT) {
    grant({"ITRU_INSTANTFIREBALL","ITRU_DEATHBOLT","ITRU_DEATHBALL","ITRU_FIRERAIN",
           "ITRU_THUNDERSTORM","ITRU_LIGHTNINGFLASH","ITRU_FIRESTORM","ITRU_SKULL"});
    if(hlp_getinstanceid(storyActorSlot("DMT_1299_OBERDEMENTOR_DI"))==hlp_getinstanceid(self))
      return ready("SPL_SKULL","SPL_COST_SKULL");
    if(npc_isdrawingweapon(self)!=0)
      return true;
    auto& ordinal = self->aivar[selection];
    if(ordinal<=0) {
      const auto roll = hlp_random(10);
      setNpcRuleInt("B_SELECTSPELL.DK_RND",roll);
      ordinal+=roll;
      }
    if(ordinal<10) {
      const auto chapter = number("KAPITEL");
      if(chapter<=3)
        return ready("SPL_INSTANTFIREBALL","SPL_COST_INSTANTFIREBALL");
      if(chapter<=4)
        return ready("SPL_DEATHBOLT","SPL_COST_DEATHBOLT");
      return ready("SPL_DEATHBALL","SPL_COST_DEATHBALL");
      }
    if(ordinal==10) {
      ordinal=11;
      const auto roll = hlp_random(100);
      setNpcRuleInt("B_SELECTSPELL.DK_MEGA",roll);
      if(roll<=2)
        return ready("SPL_FIRERAIN","SPL_COST_FIRERAIN");
      if(roll<=5)
        return ready("SPL_THUNDERSTORM","SPL_COST_THUNDERSTORM");
      if(roll<=10)
        return ready("SPL_LIGHTNINGFLASH","SPL_COST_LIGHTNINGFLASH");
      return ready("SPL_FIRESTORM","SPL_COST_FIRESTORM");
      }
    if(ordinal==12)
      ordinal=0;
    return true;
    }

  if(magic || guild==GIL_KDF) {
    grant({"ITRU_CONCUSSIONBOLT","ITRU_INSTANTFIREBALL","ITRU_DEATHBALL","ITRU_FULLHEAL"});
    // Some stock checks intentionally read the invocation's SELF and OTHER.
    const auto current = storyActorSlot("SELF");
    if(vm.get_int(current,findSymbol("C_NPC.ATTRIBUTE"),ATR_HITPOINTS)<100)
      return ready("SPL_FULLHEAL","SPL_COST_FULLHEAL");
    if(!hasAttackReasonToKill(std::dynamic_pointer_cast<daedalus::INpc>(current)))
      return ready("SPL_CONCUSSIONBOLT","SPL_COST_CONCUSSIONBOLT");
    const auto raven = hlp_getinstanceid(storyActorSlot("RAVEN"));
    const auto currentTarget = hlp_getinstanceid(storyActorSlot("OTHER"));
    const auto immortal = vm.get_int(current,findSymbol("C_NPC.FLAGS"),0)==number("NPC_FLAG_IMMORTAL");
    if(raven==currentTarget || immortal)
      return ready("SPL_DEATHBALL","SPL_COST_DEATHBALL");
    return ready("SPL_INSTANTFIREBALL","SPL_COST_INSTANTFIREBALL");
    }

  if(guild==GIL_PAL) {
    if(int32_t(self->fight_tactic)==number("FAI_NAILED"))
      return false;
    grant({"ITRU_PALHOLYBOLT"});
    const auto evil = isEvilNpc(other);
    const auto far = distance()>number("FIGHT_DIST_MELEE");
    return evil && far && ready("SPL_PALHOLYBOLT","SPL_COST_PALHOLYBOLT");
    }

  if(guild==GIL_SKELETON_MAGE) {
    grant({"ITRU_SUMSKEL","ITRU_ICECUBE","ITRU_ICEBOLT"});
    auto& ordinal = self->aivar[selection];
    if(ordinal>=6)
      ordinal=1;
    const auto targetFrozen = frozen();
    if(ordinal==0 && !targetFrozen)
      return ready("SPL_ICECUBE","SPL_COST_ICECUBE");
    if(ordinal==1)
      return ready("SPL_SUMMONSKELETON","SPL_COST_SUMMONSKELETON");
    return ready("SPL_ICEBOLT","SPL_COST_ICEBOLT");
    }

  if(guild==GIL_ICEGOLEM) {
    grant({"ITRU_ICECUBE"});
    const auto targetFrozen = frozen();
    const auto close = distance()<number("FIGHT_DIST_MELEE");
    return !targetFrozen && !close && ready("SPL_ICECUBE","SPL_COST_ICECUBE");
    }
  if(guild==GIL_FIREGOLEM) {
    grant({"ITRU_INSTANTFIREBALL"});
    return distance()>number("FIGHT_DIST_MELEE") && ready("SPL_INSTANTFIREBALL","SPL_COST_INSTANTFIREBALL");
    }

  const auto kind = self ? self->aivar[species] : 0;
  if(kind==number("ID_DRAGON_SWAMP") || kind==number("ID_DRAGON_ROCK") ||
     kind==number("ID_DRAGON_FIRE") || kind==number("ID_DRAGON_ICE")) {
    grant({"ITRU_INSTANTFIREBALL"});
    return distance()>number("FIGHT_DIST_DRAGON_MAGIC") && ready("SPL_INSTANTFIREBALL","SPL_COST_INSTANTFIREBALL");
    }
  if(kind==number("ID_DRAGON_UNDEAD")) {
    npc_clearaiqueue(nativeNpc("SELF"));
    grant({"ITRU_DEATHBALL"});
    return distance()>number("FIGHT_DIST_DRAGON_MAGIC") && ready("SPL_DEATHBALL","SPL_COST_DEATHBALL");
    }
  if(kind==number("ID_ORCSHAMAN")) {
    grant({"ITRU_INSTANTFIREBALL"});
    return distance()>number("FIGHT_DIST_MELEE") && ready("SPL_INSTANTFIREBALL","SPL_COST_INSTANTFIREBALL");
    }
  return false;
  }

void GameScript::readySpell(const std::shared_ptr<daedalus::INpc>& self, int32_t spell, int32_t mana) {
  setNpcRuleInt("B_READYSPELL.MANA",mana);
  setNpcRuleInt("B_READYSPELL.SPELL",spell);
  storyActorSlot("B_READYSPELL.SLF") = self;
  if(self) {
    // The stock function adds the full cost whenever the corresponding attribute is too low.
    if(self->attribute[NpcAttribute::mana_max]<mana)
      npc_changeattribute(self,ATR_MANAMAX,mana);
    if(self->attribute[NpcAttribute::mana]<mana)
      npc_changeattribute(self,ATR_MANA,mana);
    }
  if(npc_isdrawingspell(self)==spell || npc_getactivespell(self)==spell)
    return;
  if(npc_getactivespell(self)!=-1)
    ai_removeweapon(self);
  ai_readyspell(self,spell,mana);
  }

bool GameScript::isCanyonRazor(const std::shared_ptr<daedalus::INpc>& actor) {
  storyActorSlot("C_IAMCANYONRAZOR.SLF") = actor;
  const auto id=hlp_getinstanceid(actor);
  bool found=false;
  for(const auto suffix:{"01","02","03","04","05","06","07","08","09","10"}) {
    const auto razor=hlp_getnpc(int32_t(findSymbolIndex(std::string("CANYONRAZOR")+suffix)));
    storyActorSlot(std::string("C_IAMCANYONRAZOR.RAZ")+suffix) = razor;
    found |= hlp_getinstanceid(razor)==id;
    }
  return found;
  }

bool GameScript::dropsUnconsciousNpc() {
  constexpr size_t dropDeadAndKill=52; // AIV_DROPDEADANDKILL
  constexpr auto humanSeparator=int32_t(GIL_SEPERATOR_HUM);
  const auto excluded=[](const auto& actor) {
    const auto guild=actor ? int32_t(actor->guild) : int32_t(GIL_NONE);
    return guild==GIL_BDT || guild==GIL_DMT;
    };
  const auto other=nativeNpc("OTHER");
  if(other && (other->aivar[dropDeadAndKill]==1 || int32_t(other->guild)>humanSeparator || excluded(other)))
    return false;
  const auto self=nativeNpc("SELF");
  return !self || (int32_t(self->guild)<humanSeparator && self->aivar[dropDeadAndKill]==0 && !excluded(self));
  }

void GameScript::showRemainingCanyonRazors() {
  auto* left=findSymbol("B_COUNTCANYONRAZOR.CANYONRAZORLEFT");
  auto* text=findSymbol("B_COUNTCANYONRAZOR.CANYONRAZORTEXT");
  vm.push_int(std::bit_cast<int32_t>(10u-uint32_t(deathProgress.canyonRazors)));
  vm.unsafe_call_external(findSymbol("INTTOSTRING"));
  left->set_string(vm.pop_string());
  vm.push_reference(left);
  vm.push_reference(findSymbol("PRINT_ADDON_CANYONRAZORSLEFT"));
  vm.unsafe_call_external(findSymbol("CONCATSTRINGS"));
  text->set_string(vm.pop_string());
  ai_printscreen(text->get_string(),-1,npcRuleInt("YPOS_GOLDGIVEN"),
                 npcRuleString("FONT_SCREENSMALL"),2);
  }

bool GameScript::runDeadState(Npc& npc, std::string_view functionName, int& result) {
  // These AIVAR slots belong to the verified NotR program.
  constexpr size_t sawCrime = 1;
  constexpr size_t ransacked = 10;
  constexpr size_t partyMember = 15;
  constexpr size_t victoryXpGiven = 16;
  constexpr size_t deathHandled = 19; // AIV_TAPOSITION
  constexpr size_t monsterId = 43;
  constexpr size_t killedByPlayer = 67;
  const auto self = std::static_pointer_cast<daedalus::INpc>(npc.handlePtr());

  if(functionName=="ZS_DEAD_LOOP") {
    if(self->aivar[deathHandled]==0) {
      vm.push_reference(vm.global_self());
      vm.unsafe_call_bl(findSymbol("B_DRAGONKILLCOUNTER"));
      (void)vm.pop_int(); // The stock Loop ignores the helper's result.
      self->aivar[deathHandled] = 1;
      }
    result = LOOP_CONTINUE;
    return true;
    }
  if(functionName!="ZS_DEAD")
    return false;

  const auto other = [&] { return nativeNpc("OTHER"); };
  const auto namedNpc = [&](std::string_view name) {
    return std::dynamic_pointer_cast<daedalus::INpc>(storyActorSlot(name));
    };
  const auto matches = [&](std::string_view name) {
    return hlp_getinstanceid(self)==hlp_getinstanceid(namedNpc(name));
    };
  const auto number = [&](std::string_view name) { return npcRuleInt(name); };
  const auto setNumber = [&](std::string_view name, int32_t value) { setNpcRuleInt(name,value); };
  const auto increment=[](int32_t& count) { count=std::bit_cast<int32_t>(uint32_t(count)+1u); };

  self->aivar[ransacked] = 0;
  self->aivar[partyMember] = 0;
  stopLookAtNpc(self);
  ai_stoppointat(self);

  const auto winner = other();
  if(self->aivar[victoryXpGiven]==0 && (npc_isplayer(winner) || (winner && winner->aivar[partyMember]==1))) {
    givePlayerXp(self->level*number("XP_PER_VICTORY"));
    self->aivar[victoryXpGiven] = 1;
    }

  if(isCanyonRazor(self)) {
    increment(deathProgress.canyonRazors);
    if(number("MIS_ADDON_GREG_CLEARCANYON")==number("LOG_RUNNING"))
      showRemainingCanyonRazors();
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
    if(dropsUnconsciousNpc())
      increment(deathProgress.madKills);
    const auto running = number("LOG_RUNNING");
    if(number("MIS_FESTER_KILLBUGS")==running && self->guild==number("GIL_GIANT_BUG"))
      increment(deathProgress.festerBugs);
    if(number("MIS_KROKOJAGD")==running && matches("SWAMPRAT"))
      increment(deathProgress.jackCrocodiles);
    if(matches("RAMON"))
      setNumber("PLAYER_HASTALKEDTOBANDITCAMP",1);
    if(matches("FRANCO")) {
      if(number("MIS_HLPLOGAN")==running)
        setNumber("MIS_HLPLOGAN",number("LOG_OBSOLETE"));
      if(number("MIS_HLPEDGOR")==running)
        setNumber("MIS_HLPEDGOR",number("LOG_OBSOLETE"));
      }
    if(matches("FORTUNO"))
      log_settopicstatus(npcRuleString("TOPIC_ADDON_FORTUNO"),number("LOG_OBSOLETE"));
    }

  if(matches("GOLDMINECRAWLER")) {
    if(deathProgress.bloodwynSpawned==0 && deathProgress.minecrawlers>=9) {
      ai_teleport(namedNpc("BLOODWYN"),"ADW_MINE_TO_MC_03");
      vm.push_instance(namedNpc("BLOODWYN"));
      vm.push_string("MINE");
      vm.unsafe_call_bl(findSymbol("B_STARTOTHERROUTINE"));
      givePlayerXp(number("XP_ADDON_BLOODYWYN"));
      deathProgress.bloodwynSpawned=1;
      } else {
      increment(deathProgress.minecrawlers);
      }
    }

  for(auto name : {"B_GIVETRADEINV","B_GIVEDEATHINV","B_CLEARRUNEINV"}) {
    vm.push_reference(vm.global_self());
    vm.unsafe_call_bl(findSymbol(name));
    }
  adjustPetzCrime(self,std::nullopt);
  self->aivar[sawCrime] = number("CRIME_NONE");
  ai_unequipweapons(self);
  self->aivar[deathHandled] = 0;
  result = 0;
  return true;
  }

bool GameScript::runUnconsciousState(Npc& npc, std::string_view functionName, int& result) {
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
  const auto other = [&] { return nativeNpc("OTHER"); };
  const auto hero = [&] { return nativeNpc("HERO"); };
  const auto number = [&](std::string_view name) { return npcRuleInt(name); };
  const auto namedId = [&](std::string_view name) { return hlp_getinstanceid(storyActorSlot(name)); };
  const auto startState = [&](std::string_view name) { ai_startstate(self,int(findFunction(name).ptr),0,""); };

  result = 0;
  if(functionName=="ZS_UNCONSCIOUS_LOOP") {
    result = npc_getstatetime(self)<number("HAI_TIME_UNCONSCIOUS") ? LOOP_CONTINUE : LOOP_END;
    return true;
    }

  if(functionName=="ZS_UNCONSCIOUS") {
    npc_percenable(self,PERC_ASSESSMAGIC,int(findFunction("B_ASSESSMAGIC").ptr));
    const auto body = npc_getbodystate(self);
    const bool diving = ActivityDefinitions::bodyStateContains(body,BS_DIVE);
    const bool swimming = ActivityDefinitions::bodyStateContains(body,BS_SWIM);
    // The second stock check leaves its arguments in the saved VM symbols.
    setNpcRuleInt("C_BODYSTATECONTAINS.BODYSTATE",BS_SWIM);
    storyActorSlot("C_BODYSTATECONTAINS.SLF") = self;
    if(diving || swimming) {
      npc_clearaiqueue(self);
      clearPerceptions(self);
      startState("ZS_DEAD");
      return true;
      }

    self->aivar[guardPassage] = number("GP_NONE");
    npc_setrefusetalk(self,0);
    npc_settempattitude(self,npc_getpermattitude(self,hero()));
    stopLookAtNpc(self);
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

    for(auto name : {"B_GIVETRADEINV","B_CLEARRUNEINV"}) {
      vm.push_reference(vm.global_self());
      vm.unsafe_call_bl(findSymbol(name));
      }
    const auto winner = other();
    if(self->aivar[victoryXpGiven]==0 && ((winner && winner->aivar[partyMember]==1) || npc_isplayer(winner))) {
      givePlayerXp(self->level*number("XP_PER_VICTORY"));
      self->aivar[victoryXpGiven] = 1;
      }
    ai_unequipweapons(self);
    if(namedId("CIPHER")==hlp_getinstanceid(other()) && namedId("DAR")==hlp_getinstanceid(self))
      setNpcRuleInt("DAR_LOSTAGAINSTCIPHER",1);
    return true;
    }

  if(functionName!="ZS_UNCONSCIOUS_END")
    return false;
  self->aivar[ransacked] = 0;
  ai_standup(self);
  if(npc_isplayer(self))
    return true;
  if(namedId("RAVEN")==hlp_getinstanceid(self)) {
    self->aivar[magic] = number("MAGIC_ALWAYS");
    startNpcAttack(self,hero(),number("AR_GUILDENEMY"),0);
    return true;
    }

  const auto distance = npc_getdisttonpc(self,other());
  const auto canSee = npc_canseenpcfreelos(self,other());
  if(distance<number("PERC_DIST_INTERMEDIAT") && canSee) {
    turnToNpc(self,other());
    const auto attitude = npc_getpermattitude(self,other());
    const auto toughGuy = isToughGuy(self);
    const bool threatens = int32_t(self->type)!=number("NPCTYPE_FRIEND") && attitude!=ATT_HOSTILE && toughGuy!=0;
    sayToNpc(self,other(),threatens ? "$NEXTTIMEYOUREINFORIT" : "$OHMYHEAD");
    }
  npc_perceiveall(self);
  // Both detections run. A melee item overrides ITEM when both searches succeed.
  const bool ranged = wld_detectitem(self,ITM_CAT_FF);
  const bool melee = wld_detectitem(self,ITM_CAT_NF);
  if(ranged || melee) {
    const auto item = std::dynamic_pointer_cast<daedalus::IItem>(storyActorSlot("ITEM"));
    if(hlp_isvaliditem(item) && npc_getdisttoitem(self,item)<=500)
      ai_takeitem(self,item);
    }
  ai_equipbestmeleeweapon(self);
  ai_equipbestrangedweapon(self);
  startState("ZS_HEALSELF");
  return true;
  }

bool GameScript::runHealSelf(Npc& npc, std::string_view functionName, int& result) {
  result = 0;
  if(functionName=="ZS_HEALSELF") {
    // The stock minimal perception setup only enables these entries.
    npc.handle().senses = int32_t(SensesBit::SENSE_SEE|SensesBit::SENSE_HEAR);
    npc.handle().senses_range = npcRuleInt("PERC_DIST_ACTIVE_MAX");
    npc.setPerceptionEnable(PERC_ASSESSMAGIC,findFunction("B_ASSESSMAGIC").ptr);
    npc.setPerceptionEnable(PERC_ASSESSDAMAGE,findFunction("B_ASSESSDAMAGE").ptr);
    npc.setPerceptionEnable(PERC_ASSESSMURDER,findFunction("B_ASSESSMURDER").ptr);
    npc.setPerceptionEnable(PERC_ASSESSTHEFT,findFunction("B_ASSESSTHEFT").ptr);
    npc.setPerceptionEnable(PERC_ASSESSUSEMOB,findFunction("B_ASSESSUSEMOB").ptr);
    npc.setPerceptionEnable(PERC_ASSESSENTERROOM,findFunction("B_ASSESSPORTALCOLLISION").ptr);
    return true;
    }
  if(functionName=="ZS_HEALSELF_END")
    return true;
  if(functionName!="ZS_HEALSELF_LOOP")
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

bool GameScript::runSpellVictimState(Npc& npc, std::string_view name, int& result) {
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
  const auto* spell = std::find_if(std::begin(spells),std::end(spells),[&](const auto& s) {
    return name.starts_with(s.state) || name==s.restart;
    });
  if(spell==std::end(spells))
    return false;

  const auto self = std::static_pointer_cast<daedalus::INpc>(npc.handlePtr());
  const auto other = [&] { return storyActorSlot("OTHER"); };
  const auto number = [&](std::string_view key) { return npcRuleInt(key); };
  const auto enableMagic = [&](std::string_view callback) {
    npc_percenable(self,PERC_ASSESSMAGIC,int(findFunction(callback).ptr));
    };
  const auto playTransition = [&] {
    storyActorSlot("C_BODYSTATECONTAINS.SLF") = self;
    setNpcRuleInt("C_BODYSTATECONTAINS.BODYSTATE",BS_UNCONSCIOUS);
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
      setNpcRuleInt("TEMP_SUCKENERGY_DISTTOPLAYER",npc_getdisttoplayer(self));
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
      vm.unsafe_call_bl(findSymbol("B_ASSESSDAMAGE"));
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
    wld_playeffect("spellFX_SuckEnergy_BloodFly",self,storyActorSlot("HERO"),0,0,0,0);
    }
  if(!line.empty())
    sayToNpc(self,std::dynamic_pointer_cast<daedalus::INpc>(other()),line);

  lastSecond = npc_getstatetime(self);
  const auto hp = self->attribute[NpcAttribute::hitpoints];
  const auto fullDamage = number(spell->damage);
  magicHurtNpc(std::dynamic_pointer_cast<daedalus::INpc>(other()),self,hp>fullDamage ? fullDamage : hp-1);
  if(drain && hp>fullDamage)
    npc_changeattribute(std::dynamic_pointer_cast<daedalus::INpc>(other()),ATR_HITPOINTS,fullDamage);
  return true;
  }

CollideMask GameScript::runStockSpellCollision(Npc& target, Npc& caster, int32_t spellId) {
  // Spell IDs in the verified NotR collision predicate.
  enum Spell : int32_t {
    PalHolyBolt=2, PalRepelEvil=4, PalDestroyEvil=6, FireBolt=19, IceBolt=20, InstantFireball=23, Zap=24,
    WindFist=26, LightningFlash=29, ChargeFireball=30, Fear=32, IceCube=33, ChargeZap=34, DestroyUndead=36,
    Pyrokinesis=37, Firestorm=38, IceWave=39, FireRain=42, BreathOfDeath=43, MassDeath=44, Shrink=46,
    MasterOfDisaster=60, DeathBolt=61, DeathBall=62, ConcussionBolt=63, Thunderstorm=70, Whirlwind=71,
    WaterFist=72, IceLance=73, Geyser=75, Swarm=81, GreenTentacle=82, EnergyBall=85, SuckEnergy=86, Skull=87,
    };
  constexpr int32_t humanSeparator = int32_t(GIL_SEPERATOR_HUM);
  constexpr auto nonlethalDamage = CollideMask(COLL_APPLYDAMAGE | COLL_DONTKILL);
  setItemRuntimeInt("C_CANNPCCOLLIDEWITHSPELL.SPELLTYPE",spellId);
  const auto self = std::static_pointer_cast<daedalus::INpc>(target.handlePtr());
  const auto other = std::static_pointer_cast<daedalus::INpc>(caster.handlePtr());
  const auto guild = int32_t(self->guild);
  const int32_t realId = self->aivar[43]; // AIV_MM_REAL_ID
  const auto idIs = [&](std::string_view name) { return realId==itemRuntimeInt(name); };
  const auto wet = [&] {
    const bool diving = npcBodyStateContains(self,BS_DIVE);
    const bool swimming = npcBodyStateContains(self,BS_SWIM);
    return diving || swimming;
    };
  const auto wetOrDown = [&] {
    const bool inWater = wet();
    const bool down = isNpcDownInScript(self);
    return inWater || down;
    };
  const auto heroTarget = [&] { return hlp_getinstanceid(self)==hlp_getinstanceid(storyActorSlot("HERO")); };
  const auto sturdy = [&] {
    return guild==GIL_DRAGON || guild==GIL_TROLL || guild==GIL_SUMMONED_DEMON || guild==GIL_DEMON ||
           guild==GIL_SUMMONED_GOLEM || guild==GIL_STONEGOLEM;
    };
  const auto fireCreature = [&] { return guild==GIL_Gargoyle || guild==GIL_FIREGOLEM || idIs("ID_FIREWARAN"); };
  const auto heat = [&] { return idIs("ID_DRAGON_FIRE") || fireCreature(); };
  const auto cold = [&] { return idIs("ID_DRAGON_ICE") || idIs("ID_ICEWOLF") || guild==GIL_ICEGOLEM; };
  const auto hpMax = self->attribute[ATR_HITPOINTSMAX];

  // Thunderstorm already returns in its own earlier branch in the stock program.
  if(other->guild==GIL_DMT) {
    if(spellId==LightningFlash || spellId==FireRain) {
      if(guild==GIL_DMT)
        return COLL_DONOTHING;
      if(heroTarget())
        return COLL_APPLYHALVEDAMAGE;
      }
    if(spellId==Firestorm && guild==GIL_DMT)
      return COLL_DONOTHING;
    }

  switch(spellId) {
    case Whirlwind: {
      const bool impaired = wetOrDown();
      if(impaired || sturdy() || guild==GIL_Gargoyle || guild==GIL_SHADOWBEAST || guild==GIL_Stoneguardian ||
         guild==GIL_FIREGOLEM || guild==GIL_ICEGOLEM || self->flags==NpcFlag::IMMORTAL)
        return COLL_DONOTHING;
      return isGateGuard(self) ? COLL_APPLYDAMAGE : COLL_DOEVERYTHING;
      }
    case IceLance:
    case Thunderstorm:
      if(wetOrDown())
        return COLL_DONOTHING;
      if(cold())
        return COLL_APPLYHALVEDAMAGE;
      if(heat())
        return COLL_APPLYDOUBLEDAMAGE;
      return spellId==IceLance ? COLL_APPLYDAMAGE : COLL_DOEVERYTHING;
    case Geyser: {
      const bool impaired = wetOrDown();
      return impaired || sturdy() || guild==GIL_Stoneguardian || guild==GIL_FIREGOLEM || guild==GIL_ICEGOLEM
             ? COLL_DONOTHING : nonlethalDamage;
      }
    case WaterFist:
      if(wetOrDown())
        return COLL_DONOTHING;
      if(guild==GIL_TROLL || guild==GIL_DRAGON)
        return COLL_APPLYHALVEDAMAGE;
      return fireCreature() ? COLL_APPLYDOUBLEDAMAGE : nonlethalDamage;
    case EnergyBall:
      if(wetOrDown())
        return COLL_DONOTHING;
      return isUndeadNpc(self) ? COLL_APPLYHALVEDAMAGE : COLL_DOEVERYTHING;
    case SuckEnergy: {
      const bool tooFar = npc_getdisttonpc(self,other)>1000;
      const bool impaired = wetOrDown();
      return impaired || tooFar || guild==GIL_DMT || guild>humanSeparator || self->flags==NpcFlag::IMMORTAL
             ? COLL_DONOTHING : COLL_DOEVERYTHING;
      }
    case GreenTentacle: {
      const bool guard = isGateGuard(self);
      const bool impaired = wetOrDown();
      return guard || impaired || idIs("ID_SKELETON_MAGE") || guild==GIL_Gargoyle || guild==GIL_HARPY || guild==GIL_DRAGON ||
             guild==GIL_TROLL || guild==GIL_DEMON || guild==GIL_BLOODFLY ? COLL_DONOTHING : COLL_DOEVERYTHING;
      }
    case Swarm: {
      const bool undead = isUndeadNpc(self);
      const bool impaired = wetOrDown();
      if(undead || impaired || sturdy() || guild==GIL_Stoneguardian || guild==GIL_DMT || guild==GIL_Gargoyle ||
         guild==GIL_BLOODFLY || guild==GIL_FIREGOLEM || guild==GIL_ICEGOLEM)
        return COLL_DONOTHING;
      const bool guard = isGateGuard(self);
      return guard || guild>humanSeparator ? COLL_APPLYDAMAGE : COLL_DOEVERYTHING;
      }
    case Skull: {
      const bool undead = isUndeadNpc(self);
      const bool impaired = wetOrDown();
      return undead || impaired ? COLL_DONOTHING : COLL_DOEVERYTHING;
      }
    case WindFist:
      return npc_getdisttonpc(other,self)>=1000 ? COLL_DONOTHING : COLL_DOEVERYTHING;
    case ConcussionBolt:
    case ChargeZap:
    case Zap:
      return isNpcDownInScript(self) ? COLL_DONOTHING : nonlethalDamage;
    case DeathBall:
    case DeathBolt:
    case Pyrokinesis:
    case Firestorm:
    case FireBolt:
    case FireRain:
    case InstantFireball:
    case ChargeFireball:
      if(wetOrDown())
        return COLL_DONOTHING;
      if(heat())
        return COLL_APPLYHALVEDAMAGE;
      if(cold())
        return COLL_APPLYDOUBLEDAMAGE;
      return sturdy() ? COLL_APPLYDAMAGE : COLL_DOEVERYTHING;
    case IceBolt:
    case IceWave:
    case IceCube:
      if(wetOrDown())
        return COLL_DONOTHING;
      if(heat())
        return COLL_APPLYDOUBLEDAMAGE;
      if(cold())
        return COLL_APPLYHALVEDAMAGE;
      return sturdy() ? COLL_APPLYDAMAGE : COLL_DOEVERYTHING;
    case LightningFlash:
      if(isNpcDownInScript(self))
        return COLL_DONOTHING;
      wet(); // The stock body probes both water states, then pushes the full mask before returning.
      return COLL_DOEVERYTHING;
    case Fear: {
      const bool guard = isGateGuard(self);
      const bool evil = isEvilNpc(self);
      return guard || evil || guild==GIL_TROLL || guild==GIL_SWAMPSHARK || guild==GIL_SUMMONED_GOLEM ||
             guild==GIL_STONEGOLEM || guild==GIL_ICEGOLEM || guild==GIL_FIREGOLEM ? COLL_DONOTHING : COLL_DOEVERYTHING;
      }
    case DestroyUndead: {
      const bool undead = isUndeadNpc(self);
      return undead && hpMax<=itemRuntimeInt("SPL_DAMAGE_DESTROYUNDEAD") ? COLL_DOEVERYTHING : COLL_DONOTHING;
      }
    case BreathOfDeath: {
      const bool undead = isUndeadNpc(self);
      const bool inRange = npc_getdisttonpc(other,self)<1000;
      if(undead || !inRange)
        return COLL_DONOTHING;
      return !heroTarget() || guild==GIL_DRAGON ? COLL_APPLYHALVEDAMAGE : COLL_DOEVERYTHING;
      }
    case MassDeath:
      if(isUndeadNpc(self))
        return COLL_DONOTHING;
      return guild==GIL_DRAGON ? COLL_APPLYHALVEDAMAGE : COLL_DOEVERYTHING;
    case MasterOfDisaster: {
      const bool evil = isEvilNpc(self);
      const bool impaired = wetOrDown();
      return evil && !impaired ? COLL_DOEVERYTHING : COLL_DONOTHING;
      }
    case Shrink: {
      const bool impaired = wetOrDown();
      return guild==GIL_DRAGON || impaired ? COLL_DONOTHING : COLL_DOEVERYTHING;
      }
    case PalHolyBolt:
      return isEvilNpc(self) ? COLL_DOEVERYTHING : COLL_DONOTHING;
    case PalRepelEvil:
      if(!isEvilNpc(self))
        return COLL_DONOTHING;
      return hpMax<=itemRuntimeInt("SPL_DAMAGE_PALREPELEVIL") ? COLL_DOEVERYTHING : COLL_APPLYHALVEDAMAGE;
    case PalDestroyEvil: {
      const bool evil = isEvilNpc(self);
      return evil && hpMax<=itemRuntimeInt("SPL_DAMAGE_PALDESTROYEVIL") ? COLL_DOEVERYTHING : COLL_DONOTHING;
      }
    default:
      return COLL_DOEVERYTHING;
    }
  }

std::optional<int> GameScript::runSpecialSpellMana(Npc& caster, int spell, int mana) {
  enum : int { Sleep=27, Charm=59, Inflate=74, Swarm=81, SuckEnergy=86 };
  struct Rule {
    int spell;
    std::string_view invested, runeCost;
    };
  static constexpr Rule rules[] = {
    {Sleep,     "SPELL_LOGIC_SLEEP.MANAINVESTED",      "SPL_COST_SLEEP"},
    {Charm,     "SPELL_LOGIC_CHARM.MANAINVESTED",      "SPL_COST_CHARM"},
    {Inflate,   "SPELL_LOGIC_INFLATE.MANAINVESTED",    "SPL_COST_INFLATE"},
    {Swarm,     "SPELL_LOGIC_SWARM.MANAINVESTED",      "SPL_COST_SWARM"},
    {SuckEnergy,"SPELL_LOGIC_SUCKENERGY.MANAINVESTED", "SPL_COST_SUCKENERGY"},
    };
  const auto rule=std::ranges::find(rules,spell,&Rule::spell);
  if(rule==std::end(rules))
    return {};

  const auto self=std::static_pointer_cast<daedalus::INpc>(caster.handlePtr());
  setItemRuntimeInt("SPELL_PROCESSMANA.MANAINVESTED",mana);
  setItemRuntimeInt("SPELL_PROCESSMANA.ACTIVESPELL",npc_getactivespell(self));
  setItemRuntimeInt(rule->invested,mana);
  if(spell==SuckEnergy && mana==0)
    return SPL_RECEIVEINVEST;

  const auto available=[&] { return self->attribute[ATR_MANA]; };
  const auto runeCost=itemRuntimeInt(rule->runeCost);
  const auto scrollCost=itemRuntimeInt("SPL_COST_SCROLL");
  if(spell==SuckEnergy) {
    const bool canPayScroll=available()>=scrollCost;
    if(npc_getactivespellisscroll(self) && canPayScroll)
      return SPL_SENDCAST;
    return available()>=runeCost ? SPL_SENDCAST : SPL_SENDSTOP;
    }
  const bool canPayRune=available()>=runeCost;
  const bool canPayScroll=available()>=scrollCost;
  const bool scroll=npc_getactivespellisscroll(self);
  if(!canPayRune && !(scroll && canPayScroll))
    return SPL_SENDSTOP;
  const auto pay=[&](int32_t cost) {
    self->attribute[ATR_MANA]=std::bit_cast<int32_t>(uint32_t(available())-uint32_t(cost));
    };
  if(spell==Swarm) {
    // Even a scroll that passed admission at five mana pays the rune cost.
    pay(runeCost);
    return SPL_SENDCAST;
    }

  const auto target=std::dynamic_pointer_cast<daedalus::INpc>(storyActorSlot("OTHER"));
  if(spell==Charm) {
    const auto crimeNone=itemRuntimeInt("CRIME_NONE");
    const bool questRunning=itemRuntimeInt("MIS_IGNAZ_CHARM")==itemRuntimeInt("LOG_RUNNING");
    const auto crime=target ? target->aivar[1] : 0;
    if(questRunning && crime!=crimeNone)
      setItemRuntimeInt("CHARM_TEST",itemRuntimeInt("TRUE"));
    adjustPetzCrime(target,std::nullopt);
    if(target) {
      target->aivar[1]=crimeNone;
      target->aivar[0]=itemRuntimeInt("FIGHT_NONE");
      }
    const auto guildAttitude=[&] { return wld_getguildattitude(target ? int32_t(target->guild) : 0,self->guild); };
    if(guildAttitude()!=ATT_HOSTILE && npc_getattitude(target,self)==ATT_HOSTILE)
      npc_settempattitude(target,guildAttitude());
    return SPL_SENDCAST;
    }

  // Sleep and Inflate spend mana before testing the target, with a second scroll query.
  pay(npc_getactivespellisscroll(self) ? scrollCost : runeCost);
  const auto targetGuild=target ? int32_t(target->guild) : 0;
  const auto distance=npc_getdisttonpc(self,target);
  const auto flags=target ? int32_t(target->flags) : 0;
  const bool down=isNpcDownInScript(target);
  const bool diving=npcBodyStateContains(target,BS_DIVE);
  const bool swimming=npcBodyStateContains(target,BS_SWIM);
  if(targetGuild!=GIL_PAL && targetGuild!=GIL_DMT && targetGuild!=GIL_KDF &&
     distance<=1000 && flags!=int32_t(NpcFlag::IMMORTAL) && targetGuild<int32_t(GIL_SEPERATOR_HUM) &&
     !down && !diving && !swimming) {
    npc_clearaiqueue(target);
    clearPerceptions(target);
    ai_startstate(target,int(findFunction(spell==Sleep ? "ZS_MAGICSLEEP" : "ZS_INFLATE").ptr),0,"");
    }
  return SPL_SENDCAST;
  }

std::optional<int> GameScript::runStockSpellMana(Npc& caster, int mana) {
  const auto* active=caster.activeWeapon();
  if(active==nullptr || !active->isSpellOrRune())
    return {};

  if(const auto special=runSpecialSpellMana(caster,active->spellId(),mana))
    return special;

  struct Admission {
    int spell;
    std::string_view invested, cost, transformTo = {};
    SpellCode denied = SPL_SENDSTOP;
    bool charge = false;
    };
  // Transforms also spend mana and publish their target form at admission.
  static constexpr Admission admissions[] = {
    { 0, "SPELL_LOGIC_PALLIGHT.MANAINVESTED", "SPL_COST_PALLIGHT"},
    { 1, "SPELL_LOGIC_PALLIGHTHEAL.MANAINVESTED", "SPL_COST_PALLIGHTHEAL"},
    { 2, "SPELL_LOGIC_PALHOLYBOLT.MANAINVESTED", "SPL_COST_PALHOLYBOLT"},
    { 3, "SPELL_LOGIC_PALMEDIUMHEAL.MANAINVESTED", "SPL_COST_PALMEDIUMHEAL"},
    { 4, "SPELL_LOGIC_PALREPELEVIL.MANAINVESTED", "SPL_COST_PALREPELEVIL"},
    { 5, "SPELL_LOGIC_PALFULLHEAL.MANAINVESTED", "SPL_COST_PALFULLHEAL"},
    { 6, "SPELL_LOGIC_PALDESTROYEVIL.MANAINVESTED", "SPL_COST_PALDESTROYEVIL"},
    { 7, "SPELL_LOGIC_PALTELEPORTSECRET.MANAINVESTED", "SPL_COST_TELEPORT", {}, SPL_NEXTLEVEL},
    { 8, "SPELL_LOGIC_TELEPORTSEAPORT.MANAINVESTED", "SPL_COST_TELEPORT", {}, SPL_NEXTLEVEL},
    { 9, "SPELL_LOGIC_TELEPORTMONASTERY.MANAINVESTED", "SPL_COST_TELEPORT", {}, SPL_NEXTLEVEL},
    {10, "SPELL_LOGIC_TELEPORTFARM.MANAINVESTED", "SPL_COST_TELEPORT", {}, SPL_NEXTLEVEL},
    {11, "SPELL_LOGIC_TELEPORTXARDAS.MANAINVESTED", "SPL_COST_TELEPORT", {}, SPL_NEXTLEVEL},
    {12, "SPELL_LOGIC_TELEPORTPASSNW.MANAINVESTED", "SPL_COST_TELEPORT", {}, SPL_NEXTLEVEL},
    {13, "SPELL_LOGIC_TELEPORTPASSOW.MANAINVESTED", "SPL_COST_TELEPORT", {}, SPL_NEXTLEVEL},
    {14, "SPELL_LOGIC_TELEPORTOC.MANAINVESTED", "SPL_COST_TELEPORT", {}, SPL_NEXTLEVEL},
    {15, "SPELL_LOGIC_TELEPORTOWDEMONTOWER.MANAINVESTED", "SPL_COST_TELEPORT", {}, SPL_NEXTLEVEL},
    {16, "SPELL_LOGIC_TELEPORTTAVERNE.MANAINVESTED", "SPL_COST_TELEPORT", {}, SPL_NEXTLEVEL},
    {18, "SPELL_LOGIC_LIGHT.MANAINVESTED", "SPL_COST_LIGHT"},
    {19, "SPELL_LOGIC_FIREBOLT.MANAINVESTED", "SPL_COST_FIREBOLT"},
    {20, "SPELL_LOGIC_ICEBOLT.MANAINVESTED", "SPL_COST_ICEBOLT"},
    {21, "SPELL_LOGIC_LIGHTHEAL.MANAINVESTED", "SPL_COST_LIGHTHEAL"},
    {22, "SPELL_LOGIC_SUMMONGOBLINSKELETON.MANAINVESTED", "SPL_COST_SUMMONGOBLINSKELETON"},
    {23, "SPELL_LOGIC_INSTANTFIREBALL.MANAINVESTED", "SPL_COST_INSTANTFIREBALL"},
    {24, "SPELL_LOGIC_ZAP.MANAINVESTED", "SPL_COST_ZAP"},
    {25, "SPELL_LOGIC_SUMMONWOLF.MANAINVESTED", "SPL_COST_SUMMONWOLF"},
    {26, "SPELL_LOGIC_WINDFIST.MANAINVESTED", "STEP_WINDFIST", {}, SPL_DONTINVEST, true},
    {28, "SPELL_LOGIC_MEDIUMHEAL.MANAINVESTED", "SPL_COST_MEDIUMHEAL"},
    {29, "SPELL_LOGIC_LIGHTNINGFLASH.MANAINVESTED", "SPL_COST_LIGHTNINGFLASH"},
    {30, "SPELL_LOGIC_CHARGEFIREBALL.MANAINVESTED", "STEP_CHARGEFIREBALL", {}, SPL_DONTINVEST, true},
    {31, "SPELL_LOGIC_SUMMONSKELETON.MANAINVESTED", "SPL_COST_SUMMONSKELETON"},
    {32, "SPELL_LOGIC_FEAR.MANAINVESTED", "SPL_COST_FEAR"},
    {33, "SPELL_LOGIC_ICECUBE.MANAINVESTED", "SPL_COST_ICECUBE"},
    {34, "SPELL_LOGIC_CHARGEZAP.MANAINVESTED", "STEP_CHARGEZAP", {}, SPL_DONTINVEST, true},
    {35, "SPELL_LOGIC_SUMMONGOLEM.MANAINVESTED", "SPL_COST_SUMMONGOLEM"},
    {36, "SPELL_LOGIC_DESTROYUNDEAD.MANAINVESTED", "SPL_COST_DESTROYUNDEAD"},
    {37, "SPELL_LOGIC_PYROKINESIS.MANAINVESTED", "STEP_FIRESTORM", {}, SPL_DONTINVEST, true},
    {38, "SPELL_LOGIC_FIRESTORM.MANAINVESTED", "SPL_COST_INSTANTFIRESTORM"},
    {39, "SPELL_LOGIC_ICEWAVE.MANAINVESTED", "SPL_COST_ICEWAVE"},
    {40, "SPELL_LOGIC_SUMMONDEMON.MANAINVESTED", "SPL_COST_SUMMONDEMON"},
    {41, "SPELL_LOGIC_FULLHEAL.MANAINVESTED", "SPL_COST_FULLHEAL"},
    {42, "SPELL_LOGIC_FIRERAIN.MANAINVESTED", "SPL_COST_FIRERAIN"},
    {43, "SPELL_LOGIC_BREATHOFDEATH.MANAINVESTED", "SPL_COST_BREATHOFDEATH"},
    {44, "SPELL_LOGIC_MASSDEATH.MANAINVESTED", "SPL_COST_MASSDEATH"},
    {45, "SPELL_LOGIC_ARMYOFDARKNESS.MANAINVESTED", "SPL_COST_ARMYOFDARKNESS"},
    {47, "SPELL_LOGIC_TRFSHEEP.MANAINVESTED", "SPL_COST_TRFSHEEP", "SHEEP"},
    {48, "SPELL_LOGIC_TRFSCAVENGER.MANAINVESTED", "SPL_COST_TRFSCAVENGER", "SCAVENGER"},
    {49, "SPELL_LOGIC_TRFGIANTRAT.MANAINVESTED", "SPL_COST_TRFGIANTRAT", "GIANT_RAT"},
    {50, "SPELL_LOGIC_TRFGIANTBUG.MANAINVESTED", "SPL_COST_TRFGIANTBUG", "GIANT_BUG"},
    {51, "SPELL_LOGIC_TRFWOLF.MANAINVESTED", "SPL_COST_TRFWOLF", "WOLF"},
    {52, "SPELL_LOGIC_TRFWARAN.MANAINVESTED", "SPL_COST_TRFWARAN", "WARAN"},
    {53, "SPELL_LOGIC_TRFSNAPPER.MANAINVESTED", "SPL_COST_TRFSNAPPER", "SNAPPER"},
    {54, "SPELL_LOGIC_TRFWARG.MANAINVESTED", "SPL_COST_TRFWARG", "WARG"},
    {55, "SPELL_LOGIC_TRFFIREWARAN.MANAINVESTED", "SPL_COST_TRFFIREWARAN", "FIREWARAN"},
    {56, "SPELL_LOGIC_TRFLURKER.MANAINVESTED", "SPL_COST_TRFLURKER", "LURKER"},
    {57, "SPELL_LOGIC_TRFSHADOWBEAST.MANAINVESTED", "SPL_COST_TRFSHADOWBEAST", "SHADOWBEAST"},
    {58, "SPELL_LOGIC_TRFDRAGONSNAPPER.MANAINVESTED", "SPL_COST_TRFDRAGONSNAPPER", "DRAGONSNAPPER"},
    {60, "SPELL_LOGIC_MASTEROFDISASTER.MANAINVESTED", "SPL_COST_MASTEROFDISASTER"},
    {61, "SPELL_LOGIC_DEATHBOLT.MANAINVESTED", "SPL_COST_DEATHBOLT"},
    {62, "SPELL_LOGIC_DEATHBALL.MANAINVESTED", "SPL_COST_DEATHBALL"},
    {63, "SPELL_LOGIC_CONCUSSIONBOLT.MANAINVESTED", "SPL_COST_CONCUSSIONBOLT"},
    {70, "SPELL_LOGIC_THUNDERSTORM.MANAINVESTED", "SPL_COST_THUNDERSTORM"},
    {71, "SPELL_LOGIC_WHIRLWIND.MANAINVESTED", "SPL_COST_WHIRLWIND"},
    {72, "SPELL_LOGIC_WATERFIST.MANAINVESTED", "SPL_COST_WATERFIST"},
    {73, "SPELL_LOGIC_ICELANCE.MANAINVESTED", "SPL_COST_ICELANCE"},
    {75, "SPELL_LOGIC_GEYSER.MANAINVESTED", "SPL_COST_GEYSER"},
    {82, "SPELL_LOGIC_GREENTENTACLE.MANAINVESTED", "SPL_COST_GREENTENTACLE"},
    {84, "SPELL_LOGIC_SUMMONGUARDIAN.MANAINVESTED", "SPL_COST_SUMMONGUARDIAN"},
    {85, "SPELL_LOGIC_ENERGYBALL.MANAINVESTED", "SPL_COST_ENERGYBALL"},
    {87, "SPELL_LOGIC_SKULL.MANAINVESTED", "SPL_COST_SKULL"},
    {88, "SPELL_LOGIC_SUMMONZOMBIE.MANAINVESTED", "SPL_COST_SUMMONZOMBIE"},
    {89, "SPELL_LOGIC_SUMMONMUD.MANAINVESTED", "SPL_COST_SUMMONMUD"},
    };
  const auto rule=std::ranges::find(admissions,active->spellId(),&Admission::spell);
  if(rule==std::end(admissions))
    return {};

  const auto self=std::static_pointer_cast<daedalus::INpc>(caster.handlePtr());
  setItemRuntimeInt("SPELL_PROCESSMANA.MANAINVESTED",mana);
  // This external also publishes the active item on its stock instance symbol.
  setItemRuntimeInt("SPELL_PROCESSMANA.ACTIVESPELL",npc_getactivespell(self));
  setItemRuntimeInt(rule->invested,mana);

  const auto available=self->attribute[ATR_MANA];
  if(rule->charge) {
    const auto step=itemRuntimeInt(rule->cost);
    if(available<step)
      return rule->denied;

    auto& chargeLevel=self->aivar[88]; // Stock charge level shared by the four spells.
    if(mana<=step) {
      chargeLevel=1;
      return SPL_STATUS_CANINVEST_NO_MANADEC;
      }
    for(int32_t level=1; level<=3; ++level) {
      if(chargeLevel<=level && mana>level*step) {
        self->attribute[ATR_MANA]=std::max(0,available-step);
        chargeLevel=level+1;
        return SPL_NEXTLEVEL;
        }
      }
    return chargeLevel==4 && mana>3*step ? rule->denied : SPL_STATUS_CANINVEST_NO_MANADEC;
    }

  const auto scroll=npc_getactivespellisscroll(self);
  const auto scrollCost=itemRuntimeInt("SPL_COST_SCROLL");
  const auto runeCost=itemRuntimeInt(rule->cost);
  if(!((scroll && available>=scrollCost) || available>=runeCost))
    return rule->denied;
  if(!rule->transformTo.empty()) {
    // Stock debits the full transform cost even when the scroll threshold allowed entry.
    self->attribute[ATR_MANA]-=runeCost;
    npc_setactivespellinfo(self,int32_t(storyNpcId(rule->transformTo).value()));
    }
  return SPL_SENDCAST;
  }

bool GameScript::runStockSpellCast(Npc& caster, std::string_view function, int32_t level) {
  const auto self=std::static_pointer_cast<daedalus::INpc>(caster.handlePtr());
  auto& mana=self->attribute[ATR_MANA];
  auto& selection=self->aivar[20]; // AIV_SELECTSPELL
  if(function=="SPELL_CAST_HEAL" || function=="SPELL_CAST_PALHEAL") {
    struct Heal { std::string_view spell, cost, amount; };
    static constexpr Heal mage[] = {
      {"SPL_LIGHTHEAL", "SPL_COST_LIGHTHEAL", "SPL_HEAL_LIGHTHEAL"},
      {"SPL_MEDIUMHEAL","SPL_COST_MEDIUMHEAL","SPL_HEAL_MEDIUMHEAL"},
      {"SPL_FULLHEAL",  "SPL_COST_FULLHEAL",  "SPL_HEAL_FULLHEAL"},
      };
    static constexpr Heal paladin[] = {
      {"SPL_PALLIGHTHEAL", "SPL_COST_PALLIGHTHEAL", "SPL_HEAL_PALLIGHTHEAL"},
      {"SPL_PALMEDIUMHEAL","SPL_COST_PALMEDIUMHEAL","SPL_HEAL_PALMEDIUMHEAL"},
      {"SPL_PALFULLHEAL",  "SPL_COST_PALFULLHEAL",  "SPL_HEAL_PALFULLHEAL"},
      };
    for(const auto& heal : function=="SPELL_CAST_PALHEAL" ? paladin : mage) {
      // Each stock test queries the active spell, publishing its item instance.
      if(npc_getactivespell(self)!=itemRuntimeInt(heal.spell))
        continue;
      mana-=itemRuntimeInt(npc_getactivespellisscroll(self) ? "SPL_COST_SCROLL" : heal.cost);
      npc_changeattribute(self,ATR_HITPOINTS,itemRuntimeInt(heal.amount));
      return true;
      }
    ++selection;
    return true;
    }

  enum class Selection { After, Before, Both };
  enum class Payment { ScrollOrRune, ChargeStep, ScrollOrDrain };
  struct Summon {
    std::string_view forHero, forNpc;
    int32_t count = 1;
    float lifeTime = 500.f;
    Selection selection = Selection::After;
    };
  struct Cast {
    std::string_view function, cost = {}, levelParameter = {};
    Summon summon = {};
    Payment payment = Payment::ScrollOrRune;
    };
  // Stock spell accounting and the summoned instances used by the ordinary world spawn call.
  static constexpr Cast casts[] = {
    {"SPELL_CAST_ARMYOFDARKNESS",   "SPL_COST_ARMYOFDARKNESS", "SPELL_CAST_ARMYOFDARKNESS.SPELLLEVEL",
     {"SUMMONED_SKELETON","SKELETON",6,800.f}},
    {"SPELL_CAST_BREATHOFDEATH",    "SPL_COST_BREATHOFDEATH", "SPELL_CAST_BREATHOFDEATH.SPELLLEVEL"},
    {"SPELL_CAST_CHARGEFIREBALL",   "STEP_CHARGEFIREBALL", "SPELL_CAST_CHARGEFIREBALL.SPELLLEVEL", {}, Payment::ChargeStep},
    {"SPELL_CAST_CHARGEZAP",        "STEP_CHARGEZAP", "SPELL_CAST_CHARGEZAP.SPELLLEVEL", {}, Payment::ChargeStep},
    {"SPELL_CAST_CHARM",            "SPL_COST_CHARM"},
    {"SPELL_CAST_CONCUSSIONBOLT",   "SPL_COST_CONCUSSIONBOLT"},
    {"SPELL_CAST_DEATHBALL",        "SPL_COST_DEATHBALL"},
    {"SPELL_CAST_DEATHBOLT",        "SPL_COST_DEATHBOLT"},
    {"SPELL_CAST_DESTROYUNDEAD",    "SPL_COST_DESTROYUNDEAD"},
    {"SPELL_CAST_ENERGYBALL",       "SPL_COST_ENERGYBALL"},
    {"SPELL_CAST_FEAR",             "SPL_COST_FEAR"},
    {"SPELL_CAST_FIREBOLT",         "SPL_COST_FIREBOLT"},
    {"SPELL_CAST_FIRERAIN",         "SPL_COST_FIRERAIN"},
    {"SPELL_CAST_FIRESTORM",        "SPL_COST_INSTANTFIRESTORM"},
    {"SPELL_CAST_GEYSER",           "SPL_COST_GEYSER"},
    {"SPELL_CAST_GREENTENTACLE",    "SPL_COST_GREENTENTACLE"},
    {"SPELL_CAST_ICEBOLT",          "SPL_COST_ICEBOLT"},
    {"SPELL_CAST_ICECUBE",          "SPL_COST_ICECUBE"},
    {"SPELL_CAST_ICELANCE",         "SPL_COST_ICELANCE"},
    {"SPELL_CAST_ICEWAVE",          "SPL_COST_ICEWAVE"},
    {"SPELL_CAST_INFLATE"},
    {"SPELL_CAST_INSTANTFIREBALL",  "SPL_COST_INSTANTFIREBALL"},
    {"SPELL_CAST_LIGHTNINGFLASH",   "SPL_COST_LIGHTNINGFLASH"},
    {"SPELL_CAST_PALLIGHT",         "SPL_COST_PALLIGHT"},
    {"SPELL_CAST_LIGHT",            "SPL_COST_LIGHT"},
    {"SPELL_CAST_MASSDEATH",        "SPL_COST_MASSDEATH"},
    {"SPELL_CAST_MASTEROFDISASTER", "SPL_COST_MASTEROFDISASTER"},
    {"SPELL_CAST_PALDESTROYEVIL",   "SPL_COST_PALDESTROYEVIL"},
    {"SPELL_CAST_PALHOLYBOLT",      "SPL_COST_PALHOLYBOLT"},
    {"SPELL_CAST_PALREPELEVIL",     "SPL_COST_PALREPELEVIL"},
    {"SPELL_CAST_PYROKINESIS",      "STEP_FIRESTORM", "SPELL_CAST_PYROKINESIS.SPELLLEVEL", {}, Payment::ChargeStep},
    {"SPELL_CAST_SKULL",            {}, {}, {}, Payment::ScrollOrDrain},
    {"SPELL_CAST_SLEEP"},
    {"SPELL_CAST_SUCKENERGY",       "SPL_COST_SUCKENERGY"},
    {"SPELL_CAST_SUMMONDEMON",      "SPL_COST_SUMMONDEMON", {}, {"SUMMONED_DEMON","DEMON",1,1000.f}},
    {"SPELL_CAST_SUMMONGOBLINSKELETON", "SPL_COST_SUMMONGOBLINSKELETON", {}, {"SUMMONED_GOBBO_SKELETON","GOBBO_SKELETON"}},
    {"SPELL_CAST_SUMMONGOLEM",      "SPL_COST_SUMMONGOLEM", {}, {"SUMMONED_GOLEM","STONEGOLEM"}},
    {"SPELL_CAST_SUMMONGUARDIAN",   "SPL_COST_SUMMONGUARDIAN", {}, {"SUMMONED_GUARDIAN","STONEGUARDIAN",1,500.f,Selection::Both}},
    {"SPELL_CAST_SUMMONMUD",        "SPL_COST_SUMMONMUD", {}, {"SUMMONED_MUD","UNDEAD_MUD",1,1000.f}},
    {"SPELL_CAST_SUMMONSKELETON",   "SPL_COST_SUMMONSKELETON", {}, {"SUMMONED_SKELETON","SKELETON",1,500.f,Selection::Before}},
    {"SPELL_CAST_SUMMONWOLF",       "SPL_COST_SUMMONWOLF", {}, {"SUMMONED_WOLF","WOLF"}},
    {"SPELL_CAST_SUMMONZOMBIE",     "SPL_COST_SUMMONZOMBIE", {}, {"SUMMONED_ZOMBIE","ZOMBIE01"}},
    {"SPELL_CAST_SWARM"},
    {"SPELL_CAST_THUNDERSTORM",     "SPL_COST_THUNDERSTORM"},
    {"SPELL_CAST_WATERFIST",        "SPL_COST_WATERFIST"},
    {"SPELL_CAST_WHIRLWIND",        "SPL_COST_WHIRLWIND"},
    {"SPELL_CAST_WINDFIST",         "STEP_WINDFIST", "SPELL_CAST_WINDFIST.SPELLLEVEL", {}, Payment::ChargeStep},
    {"SPELL_CAST_ZAP",              "SPL_COST_ZAP", "SPELL_CAST_ZAP.SPELLLEVEL"},
    };
  const auto cast=std::ranges::find(casts,function,&Cast::function);
  if(cast==std::end(casts))
    return runStockTeleportCast(self,function);
  if(!cast->levelParameter.empty())
    setItemRuntimeInt(cast->levelParameter,level);
  if(function=="SPELL_CAST_FEAR")
    ai_setnpcstostate(self,int(findFunction("ZS_MAGICFLEE").ptr),1000);
  if(cast->payment==Payment::ChargeStep) {
    mana-=itemRuntimeInt(cast->cost);
    if(mana<0)
      mana=0;
    }
  else if(cast->payment==Payment::ScrollOrDrain) {
    mana=npc_getactivespellisscroll(self) ? mana-itemRuntimeInt("SPL_COST_SCROLL") : 0;
    }
  else if(!cast->cost.empty()) {
    mana-=itemRuntimeInt(npc_getactivespellisscroll(self) ? "SPL_COST_SCROLL" : cast->cost);
    }
  const auto& summon = cast->summon;
  if(!summon.forHero.empty()) {
    if(summon.selection!=Selection::After)
      ++selection;
    const auto instance = storyNpcId(npc_isplayer(self) ? summon.forHero : summon.forNpc).value();
    wld_spawnnpcrange(self,int(instance),summon.count,summon.lifeTime);
    if(summon.selection==Selection::Before)
      return true;
    }
  ++selection;
  return true;
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
  return goldInstance;
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
