#include "gamescript.h"

#include "game/npcactivityhost.h"
#include "world/objects/npc.h"

#include <algorithm>
#include <stdexcept>

void GameScript::loadNativeRoutines() {
  if(!nativeRoutineProgram)
    return;

  static constexpr std::pair<std::string_view,int32_t PointGestureRolls::*> pointRolls[] = {
    {"ZS_STAND_GUARDING_LOOP.RANDOM",       &PointGestureRolls::guard},
    {"ZS_STAND_ARMSCROSSED_LOOP.RANDOM",    &PointGestureRolls::crossedArms},
    {"ZS_STAND_WP_LOOP.RANDOM",             &PointGestureRolls::waypoint},
    {"ZS_STAND_ARMSCROSSED_LOOP.EVENTRANDY",&PointGestureRolls::portal},
    {"B_EVENT_PORTAL_EARTHQUAKE.RANDY",     &PointGestureRolls::earthquake},
    };
  for(const auto& [name,field] : pointRolls)
    bindRuntimeVariable(name,pointGestureRolls.*field);

  if(!activityEvents || ActivityDefinitionRuntime::mode()==ActivityDefinitionRuntime::Mode::Off)
    return;

  static constexpr NativeRoutinePhase routines[] = {
    {"ZS_GOTOBED",            &GameScript::runMovementRoutine},
    {"ZS_GOTOBED_DEEP",       &GameScript::runMovementRoutine},
    {"ZS_SLEEP",              &GameScript::runMovementRoutine, 0, RoutineWake::Sleep},
    {"ZS_SLEEP_DEEP",         &GameScript::runMovementRoutine, 0, RoutineWake::Sleep},
    {"ZS_FLEETOWP",           &GameScript::runMovementRoutine, 6000},
    {"ZS_RUNTOWP",            &GameScript::runMovementRoutine, 6000},
    {"ZS_FOLLOW_PLAYER",      &GameScript::runMovementRoutine},
    {"ZS_GUIDE_PLAYER",       &GameScript::runMovementRoutine},
    {"ZS_PLAY_LUTE",          &GameScript::runItemRoutine},
    {"ZS_RAKE_FP",            &GameScript::runItemRoutine},
    {"ZS_SMOKE_JOINT",        &GameScript::runItemRoutine},
    {"ZS_SPIT_FIRE",          &GameScript::runItemRoutine},
    {"ZS_STAND_DRINKING",     &GameScript::runItemRoutine},
    {"ZS_STAND_EATING",       &GameScript::runItemRoutine},
    {"ZS_STAND_SWEEPING",     &GameScript::runItemRoutine},
    {"ZS_SWEEP_FP",           &GameScript::runItemRoutine},
    {"ZS_WASH_FP",            &GameScript::runItemRoutine},
    {"ZS_GUARD_HAMMER",       &GameScript::runItemRoutine},
    {"ZS_GUARD_PASSAGE",      &GameScript::runPointRoutine, 3000},
    {"ZS_PICK_FP",            &GameScript::runPointRoutine},
    {"ZS_PRACTICE_MAGIC",     &GameScript::runPointRoutine},
    {"ZS_PRACTICE_SWORD",     &GameScript::runPointRoutine},
    {"ZS_PRAY_INNOS_FP",      &GameScript::runPointRoutine},
    {"ZS_PRAY_SLEEPER",       &GameScript::runPointRoutine},
    {"ZS_PRAY_SLEEPER_FP",    &GameScript::runPointRoutine},
    {"ZS_STUDY_WP",           &GameScript::runPointRoutine},
    {"ZS_ANNOUNCE_HEROLD",    &GameScript::runSocialRoutine, 70000, RoutineWake::Herald},
    {"ZS_CIRCLE",             &GameScript::runSocialRoutine},
    {"ZS_CONCERT",            &GameScript::runSocialRoutine, 0, RoutineWake::Concert},
    {"ZS_DANCE",              &GameScript::runSocialRoutine},
    {"ZS_GHOST",              &GameScript::runSocialRoutine, 5000},
    {"ZS_GHOSTWUSEL",         &GameScript::runSocialRoutine, 3000},
    {"ZS_PREACH_VATRAS",      &GameScript::runSocialRoutine, 13000},
    {"ZS_SMALLTALK",          &GameScript::runSocialRoutine, 8000, RoutineWake::Smalltalk},
    {"ZS_STAND_DEMENTOR",     &GameScript::runSocialRoutine},
    {"ZS_STAND_RANGERMEETING",&GameScript::runSocialRoutine},
    {"ZS_OBSERVEPLAYER",      &GameScript::runObservePlayer},
    {"ZS_CLEARROOM",          &GameScript::runClearRoom},
    {"ZS_REACTTODAMAGE",       &GameScript::runHumanReaction},
    {"ZS_REACTTOWEAPON",       &GameScript::runHumanReaction},
    {"ZS_ATTACK",              &GameScript::runHumanAttack},
    {"ZS_FLEE",                &GameScript::runHumanFlee},
    {"ZS_WATCHFIGHT",          &GameScript::runWatchFight},
    {"ZS_COMMENTFAKEGUILD",    &GameScript::runFakeGuildComment},
    {"ZS_GETMEAT",             &GameScript::runHumanPlunder},
    {"ZS_RANSACKBODY",         &GameScript::runHumanPlunder},
    {"ZS_GREENTENTACLE",       &GameScript::runControlSpell},
    {"ZS_SHORTZAPPED",         &GameScript::runControlSpell},
    {"ZS_DEAD",               &GameScript::runDeadState},
    {"ZS_UNCONSCIOUS",        &GameScript::runUnconsciousState},
    {"ZS_HEALSELF",           &GameScript::runHealSelf},
    {"ZS_BANDIT",             &GameScript::runBanditCamp},
    {"ZS_GOLEMDOWN",          &GameScript::runDormantGolem},
    {"ZS_TALK",               &GameScript::runTalk},
    {"ZS_MAGICFLEE",          &GameScript::runFearOrSleepSpell},
    {"ZS_MAGICSLEEP",         &GameScript::runFearOrSleepSpell},
    {"ZS_MAGICFREEZE",        &GameScript::runFreezeOrWhirlwindSpell},
    {"ZS_WHIRLWIND",          &GameScript::runFreezeOrWhirlwindSpell},
    {"ZS_PAL_ZOMBIE",         &GameScript::runMonsterRoutine},
    {"ZS_INFLATE",            &GameScript::runSpellVictimState},
    {"ZS_SUCKENERGY",         &GameScript::runSpellVictimState},
    {"ZS_SWARM",              &GameScript::runSpellVictimState},
    {"B_RESTARTINFLATE",      &GameScript::runSpellVictimState},
    {"B_RESTARTSUCKENERGY",   &GameScript::runSpellVictimState},
    {"B_RESTARTSWARM",        &GameScript::runSpellVictimState},
    {monsterScheduler,       &GameScript::runMonsterScheduler},
    {"ZS_MM_ATTACK",         &GameScript::runMonsterAttack},
    {"ZS_MM_THREATENENEMY",   &GameScript::runMonsterThreat},
    {"ZS_MM_FLEE",            &GameScript::runMonsterFlee},
    {"ZS_MM_HUNT",            &GameScript::runMonsterHunt},
    {"ZS_MM_EATBODY",         &GameScript::runMonsterEatBody},
    {"ZS_MM_RTN_DRAGONREST",  &GameScript::runMonsterDragonRest},
    {"ZS_MM_RTN_FOLLOW_SHEEP", &GameScript::runMonsterSheepFollow},
    {"ZS_MM_RTN_FOLLOW_SHEEP_BALTHASAR", &GameScript::runMonsterSheepFollow},
    {"ZS_MM_RTN_SUMMONED",    &GameScript::runMonsterSummoned},
    {"ZS_MM_RTN_SLEEP",       &GameScript::runMonsterRoutine},
    {"ZS_MM_RTN_REST",        &GameScript::runMonsterRoutine},
    {"ZS_MM_RTN_ROAM",        &GameScript::runRoamingRoutine},
    {"ZS_MM_RTN_EATGROUND",   &GameScript::runMonsterRoutine},
    {"ZS_MM_RTN_WUSEL",       &GameScript::runRoamingRoutine},
    {"ZS_MM_RTN_ORCSIT",      &GameScript::runMonsterRoutine},
    {"ZS_MAGICSHRINK",        &GameScript::runShrinkState},
    {"ZS_MAGICSHRUNK",        &GameScript::runShrinkState},
    {"ZS_MAGICBURN",          &GameScript::runMagicHurtState},
    {"ZS_MAGICBURNSHORT",     &GameScript::runMagicHurtState},
    {"ZS_PYRO",               &GameScript::runMagicHurtState},
    {"ZS_ZAPPED",             &GameScript::runMagicHurtState},
    };
  for(const auto& routine : routines) {
    for(const std::string_view suffix : {"", "_LOOP", "_END"}) {
      const auto function = findFunction(std::string(routine.state).append(suffix));
      if(!function.isValid())
        continue; // Some stock states have only Init or Init and Loop.
      auto phase = routine;
      phase.enters = suffix.empty();
      nativeRoutinePhases.emplace(uint32_t(function.ptr),phase);
      }
    }

  using Actor = std::shared_ptr<daedalus::INpc>;
  const auto bind = [this]<class R,class... P>(std::string_view name,const std::function<R(P...)>& callback) {
    nativeExternals.insert_or_assign(std::string(name),storygraph::makeNativeInternal(callback));
    };
  bind("B_MM_ASSESSENEMY",std::function<void()>{[this] { runMonsterAssessEnemy(); }});
  bind("B_GIVEINVITEMS",std::function<int32_t(Actor,Actor,int32_t,int32_t)>{
    [this](Actor giver,Actor taker,int32_t item,int32_t amount) { return giveInventoryItems(giver,taker,item,amount); }});
  bind("B_MM_WISPDETECT",std::function<int32_t()>{[this] { return detectWisp(); }});
  bind("B_WISPDETECTEDITEM",std::function<int32_t()>{[this] { return detectWispItem(); }});
  bind("B_GETWISPDETECTEDITEM",std::function<void()>{[this] { approachWispItem(); }});
  bind("B_ASSIGNDRAGONTALK",std::function<void(Actor)>{[this](const Actor& dragon) { assignDragonTalk(dragon); }});
  bind("C_WANTTOEAT",std::function<int32_t(Actor,Actor)>{
    [this](Actor monster,Actor body) { return int32_t(monsterWantsToEat(monster,body)); }});
  bind("C_NPCISMONSTERMAGE",std::function<int32_t(Actor)>{[this](Actor npc) { return int32_t(isMonsterMage(npc)); }});
  bind("PERCEPTION_SET_MONSTER_RTN",std::function<void()>{[this] {
    if(auto* npc = findNpc(nativeNpc("SELF")))
      enableMonsterRoutinePerceptions(*npc);
    }});
  bind("B_MAGICHURTNPC",std::function<void(Actor,Actor,int32_t)>{
    [this](Actor attacker,Actor target,int32_t damage) { magicHurtNpc(attacker,target,damage); }});
  bind("B_AWAKE_STONEGUARDIAN",std::function<void(Actor)>{[this](Actor actor) { awakenStoneGuardian(actor); }});
  bind("B_REMOVEGHOST",std::function<int32_t()>{[this] {
    removeFinishedGhosts();
    return 0; // The stock function declares an integer and returns without a value.
    }});
  }

bool GameScript::runMonsterScheduler(Npc& npc, std::string_view, int& result) {
  auto& aivar = npc.handle().aivar;
  aivar[size_t(npcRuleInt("AIV_MM_PRIORITY"))] = npcRuleInt("PRIO_EAT");
  removeRavensStoneGuardians(npc);

  // Overlapping windows keep the priority of the original scheduler. With no match it starts REST.
  std::string_view next = "ZS_MM_RTN_REST";
  for(const auto& window : monsterRoutineWindows) {
    if(monsterRoutineWindowActive(npc,window)) {
      next = window.state;
      break;
      }
    }
  const auto self = std::static_pointer_cast<daedalus::INpc>(npc.handlePtr());
  ai_startstate(self,int(findFunction(next).ptr),1,"");
  result = 0;
  return true;
  }

bool GameScript::monsterRoutineWindowActive(const Npc& npc, const MonsterRoutineWindow& window) {
  const auto& aivar = npc.handlePtr()->aivar;
  const auto start = aivar[window.stockStart];
  const auto end = aivar[window.stockEnd];
  return start==-1 || wld_istime(start,0,end,0); // ONLYROUTINE
  }

void GameScript::enableMonsterRoutinePerceptions(Npc& npc) {
  npc.setPerceptionTime(1000);
  npc.setPerceptionEnable(PERC_ASSESSENEMY,findFunction("B_MM_ASSESSENEMY").ptr);
  npc.setPerceptionEnable(PERC_ASSESSBODY,findFunction("B_MM_ASSESSBODY").ptr);
  npc.setPerceptionEnable(PERC_ASSESSMAGIC,findFunction("B_ASSESSMAGIC").ptr);
  npc.setPerceptionEnable(PERC_ASSESSDAMAGE,findFunction("B_MM_ASSESSDAMAGE").ptr);
  const auto othersDamage = findFunction("B_MM_ASSESSOTHERSDAMAGE").ptr;
  npc.setPerceptionEnable(PERC_ASSESSOTHERSDAMAGE,othersDamage);
  npc.setPerceptionEnable(PERC_ASSESSMURDER,othersDamage);
  npc.setPerceptionEnable(PERC_ASSESSWARN,findFunction("B_MM_ASSESSWARN").ptr);
  }

void GameScript::beginMonsterRoutine(Npc& npc, bool desynchronize, WalkBit mode) {
  npc.aiPush(AiQueue::aiSetWalkMode(mode));
  if(desynchronize)
    desynchronizeNpc();
  const auto& waypoint = npc.handle().wp;
  if(NpcEngineCalls::nearestWaypoint(world(),npc)!=waypoint)
    NpcEngineCalls::gotoWaypoint(world(),npc,waypoint);
  }

bool GameScript::endMonsterRoutine(Npc& npc, MonsterRoutine routine) {
  if(monsterRoutineWindowActive(npc,monsterRoutineWindows[routine]))
    return false;
  const auto self = std::static_pointer_cast<daedalus::INpc>(npc.handlePtr());
  ai_startstate(self,int(findFunction(monsterScheduler).ptr),1,"");
  return true;
  }

void GameScript::removeRavensStoneGuardians(Npc& npc) {
  // The stock rule resolves the live guardian through the SELF alias.
  if(npc.handle().guild==GIL_Stoneguardian && deathProgress.ravenDead==1)
    invokeKillNpc(hlp_getinstanceid(nativeNpc("SELF")));
  }

bool GameScript::invokeNativeRoutine(Npc& npc, uint32_t function, std::string_view name, NpcActivityProbe::Phase& probe) {
  if(!activityEvents || floorDefer.active())
    return false;
  const auto it = nativeRoutinePhases.find(function);
  if(it==nativeRoutinePhases.end() || !(this->*it->second.callback)(npc,name,probe.result))
    return false;
  const auto& phase = it->second;
  probe.fromRecord();
  ActivityEvents::countWakes(phase.state,phase.enters ? ActivityEvents::Entered : 0,true);
  return true;
  }

bool GameScript::invokeNativeRoutineReaction(Npc& npc, std::string_view name,
                                               NpcActivityProbe::Phase& phase) {
  if((name!="B_GOLEMRISE" && name!="B_ASSESSSLEEPTALK" && name!="B_EVENT_PORTAL_EARTHQUAKE") ||
     !nativeRoomReactionsEnabled())
    return false;
  phase.fromRecord();
  if(name=="B_GOLEMRISE")
    wakeDormantGolem(npc);
  else if(name=="B_ASSESSSLEEPTALK")
    assessSleepTalk(npc);
  else
    runPortalEarthquake(npc);
  return true;
  }

bool GameScript::invokeNativeStatusPerception(Npc& npc, std::string_view name,
                                              NpcActivityProbe::Phase& phase) {
  const bool stopDamage = name=="B_STOPMAGICBURN" || name=="B_STOPMAGICBURNSHORT" || name=="B_STOPPYRO" || name=="B_STOPZAPPED";
  const bool restartBurn = name=="B_RESTARTBURN" || name=="B_RESTARTBURNSHORT";
  if((name!="B_ASSESSMAGIC" && name!="B_STOPMAGICFLEE" && name!="B_STOPMAGICSLEEP" && name!="B_ASSESSMAGICSLEEPTALK" &&
      name!="B_PAL_ZOMBIE_RISE" && name!="B_RESTARTFREEZE" && name!="B_RESTARTWHIRLWIND" && !stopDamage && !restartBurn) ||
     !nativeRoomReactionsEnabled())
    return false;
  phase.fromRecord();
  if(name=="B_ASSESSMAGIC")
    assessMagic(npc);
  else if(name=="B_STOPMAGICFLEE")
    stopMagicFear(npc);
  else if(name=="B_STOPMAGICSLEEP")
    stopMagicSleep(npc);
  else if(name=="B_ASSESSMAGICSLEEPTALK") {
    sayMagicReaction(npc,"$YOUDISTURBEDMYSLUMBER",false);
    ai_startstate(std::static_pointer_cast<daedalus::INpc>(npc.handlePtr()),int(findFunction("ZS_OBSERVEPLAYER").ptr),1,"");
    }
  else if(name=="B_PAL_ZOMBIE_RISE") {
    raisePaladinZombie(npc);
    }
  else if(stopDamage) {
    stopInterruptedSpell(npc,true);
    }
  else if(restartBurn) {
    const auto self = std::static_pointer_cast<daedalus::INpc>(npc.handlePtr());
    const auto spell = npc.lastHitSpellId();
    const auto id = [this](std::string_view symbol) { return npcRuleInt(symbol); };
    const bool shortBurn = name=="B_RESTARTBURNSHORT";
    const bool sameEffect = spell==id("SPL_CHARGEFIREBALL") || (shortBurn
      ? spell==id("SPL_FIRESTORM")
      : spell==id("SPL_FIREBOLT") || spell==id("SPL_INSTANTFIREBALL") || spell==id("SPL_FIRERAIN"));
    if(sameEffect) {
      npc.setStateTime(0);
      }
    else if(spell==id("SPL_ICECUBE") || spell==id("SPL_ICEWAVE")) {
      npc.clearAiQueue();
      clearPerceptions(self);
      ai_startstate(self,int(findFunction("ZS_MAGICFREEZE").ptr),0,"");
      }
    }
  else {
    constexpr int32_t iceCube=33, iceWave=39, whirlwind=71; // Spell IDs in the verified program.
    const auto spell = npc.lastHitSpellId();
    const bool matches = name=="B_RESTARTFREEZE" ? spell==iceCube || spell==iceWave : spell==whirlwind;
    if(matches)
      npc.setStateTime(0);
    }
  return true;
  }

bool GameScript::routineTimerDue(const Npc& npc, ScriptFn loop) const {
  if(!activityEvents || nativeRoutinePhases.empty() || floorDefer.active())
    return true;
  const auto it = nativeRoutinePhases.find(uint32_t(loop.ptr));
  if(it==nativeRoutinePhases.end())
    return true;
  const auto& phase = it->second;
  switch(phase.wake) {
    case RoutineWake::Sleep:
      return !ActivityDefinitions::bodyStateContains(int32_t(npc.bodyState()),BS_LIE);
    case RoutineWake::Smalltalk:
      return npc.stateTime()>=phase.minStateTime || !npc.isOnFreePoint("SMALLTALK");
    case RoutineWake::Herald:
      return npc.stateTime()>=phase.minStateTime || npc.handlePtr()->aivar[19]==1;
    case RoutineWake::Concert:
      return npc.handlePtr()->aivar[19]==1;
    case RoutineWake::Timer:
      return npc.stateTime()>=phase.minStateTime;
    }
  return true;
  }

void GameScript::runActivityGesture(Npc& npc, const ActivityEvents::Record& activity) {
  const auto storeLoopRoll = [&](std::string_view variable, int32_t roll) {
    const auto loop = storyProgramName(uint32_t(npc.aiState.funcLoop.ptr));
    setNpcRuleInt(std::string(loop).append(variable),roll);
    };
  if(const auto* mob=activity.mob) {
    const auto& timer=*mob->timer;
    if(timer.rollRange>0) {
      const auto roll = int32_t(rollRandom(uint32_t(timer.rollRange),&npc,true));
      storeLoopRoll(timer.rollVariable,roll);
      for(const auto& gesture:timer.gestures)
        if(roll<=gesture.throughRoll)
          npc.pushActivityCommand(AiQueue::aiPlayAnimBs(gesture.animation,BodyState(mob->busyBodyState)));
      }
    npc.setStateTime(0);
    return;
    }
  const auto& point = *activity.point;
  const auto gestureRoll = int32_t(rollRandom(uint32_t(point.rollRange),&npc,true));
  using Form = ActivityDefinitions::PointActivity::Form;
  switch(point.form) {
    case Form::Guard:       pointGestureRolls.guard = gestureRoll; break;
    case Form::ArmsCrossed: pointGestureRolls.crossedArms = gestureRoll; break;
    case Form::Waypoint:    pointGestureRolls.waypoint = gestureRoll; break;
    default: break;
    }
  const auto& gesture = point.gestures[size_t(gestureRoll)];
  if(!gesture.animation.empty())
    npc.pushActivityCommand(AiQueue::aiPlayAnim(gesture.animation));
  if(point.portal) {
    const auto& portal = *point.portal;
    const auto roll = int32_t(rollRandom(uint32_t(portal.rollRange),&npc,true));
    pointGestureRolls.portal = roll;
    const auto guild = npc.handle().guild;
    if(roll==portal.triggerRoll && guild!=portal.excludedGuild && deathProgress.ravenDead==portal.ravenDead &&
       currentStoryWorld==portal.world)
      npc.invokeRoutine(findFunction(portal.function));
    }
  if(gesture.resetTimer)
    npc.setStateTime(0);
  }

bool GameScript::invokeActivity(Npc& npc, uint32_t function, NpcActivityProbe::Phase& probe) {
  const auto* phase = ActivityDefinitionRuntime::find(activityDefinitions,function);
  if(phase==nullptr || (!activityEvents && (phase->record.point || phase->record.mob->timer ||
                                             phase->record.mob->skipApproachWhenBusy)))
    return false;
  probe.fromRecord();
  probe.result = ActivityDefinitionRuntime::run(world(),npc,*phase,[this,&npc](std::string_view) {
    setPerceptionPreset(true,std::static_pointer_cast<daedalus::INpc>(npc.handlePtr()));
    },[this,&npc](int32_t item) { storeItem(npc.addItem(uint32_t(item),1)); });
  return true;
  }

const ActivityEvents::Record* GameScript::eventActivity(ScriptFn init) {
  // Only the accepted states (also under OPENGOTHIC_ACTIVITY_RECORDS=all, a measuring mode). A Loop that ends
  // the state is not an activity to hold: such a record keeps the polled Loop.
  if(!activityEvents || !init.isValid() || floorDefer.active())
    return nullptr;
  const auto* phase = ActivityDefinitionRuntime::find(activityDefinitions,uint32_t(init.ptr));
  if(phase==nullptr || phase->phase!=ActivityDefinitions::Phase::Init)
    return nullptr;
  if(phase->record.mob && (phase->record.mob->loopResult!=LOOP_CONTINUE ||
     !ActivityDefinitionRuntime::accepted(phase->name)))
    return nullptr;
  return &phase->record;
  }

std::vector<std::string_view> GameScript::eventActivityNames() {
  std::vector<std::string_view> names;
  for(const auto& [id,phase]:activityDefinitions.phases)
    if(phase.phase==ActivityDefinitions::Phase::Init && eventActivity(ScriptFn(id)))
      names.push_back(phase.name);
  std::sort(names.begin(),names.end());
  return names;
  }
