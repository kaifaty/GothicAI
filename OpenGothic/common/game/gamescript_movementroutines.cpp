#include "gamescript.h"

#include "game/npcactivityhost.h"
#include "world/objects/npc.h"
#include "world/world.h"

#include <algorithm>
#include <bit>

namespace {
constexpr std::string_view bed = "BEDHIGH";

bool inBodyState(const Npc& npc, BodyState state) {
  return ActivityDefinitions::bodyStateContains(int32_t(npc.bodyState()),int32_t(state));
}
}

bool GameScript::runStockTeleportCast(const std::shared_ptr<daedalus::INpc>& actor, std::string_view function) {
  struct Destination {
    std::string_view function, world, waypoint;
    int32_t spell;
    };
  static constexpr Destination destinations[] = {
    {"SPELL_CAST_PALTELEPORTSECRET",   "NEWWORLD_ZEN", "NW_PAL_SECRETCHAMBER",     7},
    {"SPELL_CAST_TELEPORTSEAPORT",     "NEWWORLD_ZEN", "HAFEN",                    8},
    {"SPELL_CAST_TELEPORTMONASTERY",   "NEWWORLD_ZEN", "KLOSTER",                  9},
    {"SPELL_CAST_TELEPORTFARM",        "NEWWORLD_ZEN", "BIGFARM",                 10},
    {"SPELL_CAST_TELEPORTXARDAS",      "NEWWORLD_ZEN", "XARDAS",                  11},
    {"SPELL_CAST_TELEPORTPASSNW",      "NEWWORLD_ZEN", "LEVELCHANGE",             12},
    {"SPELL_CAST_TELEPORTPASSOW",      "OLDWORLD_ZEN", "SPAWN_MOLERAT02_SPAWN01",  13},
    {"SPELL_CAST_TELEPORTOC",          "OLDWORLD_ZEN", "OC_MAGE_CENTER",          14},
    {"SPELL_CAST_TELEPORTOWDEMONTOWER", "OLDWORLD_ZEN", "DT_E3_03",                15},
    {"SPELL_CAST_TELEPORTTAVERNE",     "NEWWORLD_ZEN", "NW_TAVERNE_04",            16},
    };
  const auto cast = [&](const Destination& destination) {
    const auto level = findSymbol(destination.world)->get_int();
    vm.set_int(nullptr,findSymbol("B_PRINTTELEPORTTOOFARAWAY.LEVEL"),0,level);
    if(level!=currentStoryWorld) {
      vm.push_reference(findSymbol("PRINT_TELEPORTTOOFARAWAY"));
      vm.push_int(-1);
      vm.push_reference(findSymbol("YPOS_LEVELUP"));
      vm.push_reference(findSymbol("FONT_SCREENSMALL"));
      vm.push_int(2);
      if(vm.unsafe_call_external(findSymbol("PRINTSCREEN")))
        (void)vm.pop_int();
      }
    const auto mana = findSymbol(npc_getactivespellisscroll(actor) ? "SPL_COST_SCROLL" : "SPL_COST_TELEPORT")->get_int();
    auto* attribute = findSymbol("C_NPC.ATTRIBUTE");
    const auto oldMana = vm.get_int(actor,attribute,ATR_MANA);
    vm.set_int(actor,attribute,ATR_MANA,std::bit_cast<int32_t>(uint32_t(oldMana)-uint32_t(mana)));
    ai_teleport(actor,destination.waypoint);
    ai_playani(actor,"T_HEASHOOT_2_STAND");
    };

  if(function=="SPELL_CAST_TELEPORT") {
    // The dispatcher queries and republishes the active item for every destination.
    for(const auto& destination : destinations)
      if(npc_getactivespell(actor)==destination.spell)
        cast(destination);
    return true;
    }
  const auto destination = std::ranges::find(destinations,function,&Destination::function);
  if(destination==std::end(destinations))
    return false;
  cast(*destination);
  return true;
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

  if(name=="ZS_GOTOBED" || name=="ZS_GOTOBED_DEEP") {
    setPerceptionPreset(true);
    resetRoutineNpc(self);
    npc.aiPush(AiQueue::aiSetWalkMode(WalkBit::WM_Walk));
    if(NpcEngineCalls::nearestWaypoint(world(),npc)!=properties.wp)
      goToWaypoint();
    // The transition also starts at an already reached bed and keeps the row's end time.
    const auto sleep = name=="ZS_GOTOBED_DEEP" ? "ZS_SLEEP_DEEP" : "ZS_SLEEP";
    ai_startstate(self,int(findFunction(sleep).ptr),0,"");
    result = 0;
    return true;
    }
  if(name=="ZS_SLEEP" || name=="ZS_SLEEP_DEEP") {
    if(name=="ZS_SLEEP")
      enable(PERC_ASSESSQUIETSOUND,"B_ASSESSQUIETSOUND");
    enable(PERC_ASSESSDAMAGE,"B_ASSESSDAMAGE");
    enable(PERC_ASSESSTALK,"B_ASSESSSLEEPTALK");
    npc.aiPush(AiQueue::aiSetWalkMode(WalkBit::WM_Walk));
    result = 0;
    return true;
    }
  if(name=="ZS_SLEEP_END" || name=="ZS_SLEEP_DEEP_END") {
    if(!npc_isinplayersroom(self))
      sayToNpc(self,self,"$AWAKE");
    npc.aiPush(AiQueue::aiUseMob(bed,-1));
    result = 0;
    return true;
    }
  if(name=="ZS_SLEEP_LOOP" || name=="ZS_SLEEP_DEEP_LOOP") {
    result = LOOP_CONTINUE;
    if(npcBodyStateContains(self,BS_LIE))
      return true;
    if(world().availableMob(npc,bed)!=nullptr) {
      npc.aiPush(AiQueue::aiUseMob(bed,1));
      return true;
      }
    const auto hero = std::dynamic_pointer_cast<daedalus::INpc>(vm.global_hero()->get_instance());
    const auto distance = npc_getdisttonpc(self,hero);
    const bool lyingHero = npcBodyStateContains(hero,BS_LIE);
    auto& complaint = *findSymbol("PLAYER_GETOUTOFMYBEDCOMMENT");
    if(distance<=500 && lyingHero && complaint.get_int()==0) {
      turnToNpc(self,hero);
      sayToNpc(self,std::dynamic_pointer_cast<daedalus::INpc>(vm.global_other()->get_instance()),"$GETOUTOFMYBED");
      complaint.set_int(1);
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
            diegoRouteBoundary(0)!=0) {
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
      runGuidePlayerAssessment();
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

void GameScript::assessSleepTalk(Npc& npc) {
  const auto self = std::static_pointer_cast<daedalus::INpc>(npc.handlePtr());
  if(npcBodyStateContains(self,BS_LIE))
    npc.aiPush(AiQueue::aiUseMob(bed,-1));
  ai_startstate(self,int(findFunction("ZS_OBSERVEPLAYER").ptr),0,"");
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
  sayToNpc(self,other,line,overlay);
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

void GameScript::runFollowerPlayerAssessment() {
  const auto self = std::dynamic_pointer_cast<daedalus::INpc>(vm.global_self()->get_instance());
  if(self==nullptr)
    return;
  const auto hero = std::dynamic_pointer_cast<daedalus::INpc>(vm.global_hero()->get_instance());
  constexpr size_t positionStage = 19, followDistance = 80;
  auto& stage = self->aivar[positionStage];
  const auto stand = [&] {
    npc_clearaiqueue(self);
    ai_standup(self);
    stage = 1;
    };
  if(stage==0 && npc_getdisttonpc(self,hero)<self->aivar[followDistance]) {
    stand();
    return;
    }
  if(hlp_getinstanceid(self)==hlp_getinstanceid(findSymbol("DIEGOOW")->get_instance())) {
    const auto boundary = diegoRouteBoundary(0);
    if(stage==0 && boundary!=0) {
      stand();
      if(npc_getdisttonpc(self,hero)>findSymbol("PERC_DIST_DIALOG")->get_int()) {
        const auto other = std::dynamic_pointer_cast<daedalus::INpc>(vm.global_other()->get_instance());
        sayToNpc(self,other,"$SC_HEYWAITASECOND");
        }
      return;
      }
    }
  runPlayerAssessment();
  }

void GameScript::runGuidePlayerAssessment() {
  const auto self = std::dynamic_pointer_cast<daedalus::INpc>(vm.global_self()->get_instance());
  if(self==nullptr)
    return;
  const auto hero = std::dynamic_pointer_cast<daedalus::INpc>(vm.global_hero()->get_instance());
  const auto distance = npc_getdisttonpc(self,hero);
  if(distance<=800) {
    runPlayerAssessment();
    return;
    }
  if(npc_getdisttowp(self,self->wp)<npc_getdisttowp(hero,self->wp)) {
    npc_clearaiqueue(self);
    ai_standup(self);
    turnToNpc(self,hero);
    }
  constexpr size_t enemyOverride = 61;
  if(npc_getattitude(self,hero)==ATT_HOSTILE && self->aivar[enemyOverride]==1 && distance>1200)
    self->aivar[enemyOverride] = 0;
  }

void GameScript::runCompanionFightSound(bool follower) {
  using Actor = std::shared_ptr<daedalus::INpc>;
  const auto source = std::dynamic_pointer_cast<daedalus::INpc>(vm.global_other()->get_instance());
  const auto victim = std::dynamic_pointer_cast<daedalus::INpc>(vm.global_victim()->get_instance());
  constexpr size_t partyMember = 15; // AIV_PARTYMEMBER
  const auto inParty = [](const Actor& actor) { return actor!=nullptr && actor->aivar[partyMember]==1; };
  if(npc_isplayer(victim) || npc_isplayer(source) || (follower && (inParty(victim) || inParty(source))))
    runFightSoundReaction();
  }

int32_t GameScript::diegoRouteBoundary(int32_t tolerance) {
  findSymbol("C_DIEGOTOOFAR.TOLERANCE")->set_int(tolerance);
  const auto hero = std::dynamic_pointer_cast<daedalus::INpc>(vm.global_hero()->get_instance());
  const auto near = [&](std::string_view point, int32_t radius) {
    return npc_getdisttowp(hero,point)<int64_t(radius)+tolerance;
    };
  struct Boundary {
    std::string_view point;
    int32_t radius;
    int32_t region;
    };
  static constexpr Boundary north[] = {
    {"SPAWN_OW_SHADOWBEAST_10_01",4000,1}, // Angar
    {"OW_ICEREGION_ENTRANCE_01",  3000,2},
    {"OW_PATH_046",              1800,4}, // Swamp
    {"CASTLE_5",                 1500,3}, // Fire dragon
    {"MT13",                     5500,5}, // Lake
    {"MT16",                     6500,5},
    };
  for(const auto& boundary : north)
    if(near(boundary.point,boundary.radius))
      return boundary.region;

  const auto self = std::dynamic_pointer_cast<daedalus::INpc>(vm.global_self()->get_instance());
  if((npc_getheighttonpc(self,hero)>1000 && npc_getdisttowp(hero,"DT_E3_03")<1000) ||
     wld_getplayerportalguild()==int32_t(GIL_DMT) || near("DT_MAINGATE",500))
    return 6;

  static constexpr Boundary minesAndCastle[] = {
    {"OW_NEWMINE_03_B",           2500,7},
    {"OW_NEWMINE_03",             2500,7},
    {"OW_MINE3_OUT",              1200,8},
    {"OW_PATH_266",               3000,9},
    {"OC_CENTER_05",             12000,10},
    {"OC_CENTER_02",             12000,10},
    {"PATH_OC_PSI_01",            5000,11},
    {"OC_ORCBARRIER_08",          5000,11},
    {"OW_PATH_BLOODFLY01_SPAWN01", 5000,12},
    };
  for(const auto& boundary : minesAndCastle)
    if(near(boundary.point,boundary.radius))
      return boundary.region;
  return 0;
  }
