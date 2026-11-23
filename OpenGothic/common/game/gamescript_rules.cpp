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

void GameScript::loadNativeRoomReaction() {
  if(!nativeRoutineProgram)
    return;
  vm.override_function("B_ASSESSENTERROOM",std::function<int32_t()>{[this] { return runEnterRoomReaction(); }},[this] {
    return activityEvents && !floorDefer.active() && ActivityDefinitionRuntime::mode()!=ActivityDefinitionRuntime::Mode::Off;
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
      vm.call_function<void>("B_DRAGONKILLCOUNTER",self);
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
    if(ActivityDefinitions::bodyStateContains(body,BS_DIVE) || ActivityDefinitions::bodyStateContains(body,BS_SWIM)) {
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
