#include "gamescript.h"

#include "game/npcactivityhost.h"
#include "world/objects/npc.h"

#include <algorithm>

void GameScript::loadNativeRoutines() {
  if(!nativeRoutineProgram || !activityEvents || ActivityDefinitionRuntime::mode()==ActivityDefinitionRuntime::Mode::Off)
    return;

  static constexpr NativeRoutinePhase routines[] = {
    {"ZS_GOTOBED",            &GameScript::runMovementRoutine},
    {"ZS_SLEEP",              &GameScript::runMovementRoutine, 0, RoutineWake::Sleep},
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
    {monsterScheduler,       &GameScript::runMonsterScheduler},
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
      const auto* symbol = vm.find_symbol_by_name(std::string(routine.state).append(suffix));
      if(symbol==nullptr)
        continue; // Some stock states have only Init or Init and Loop.
      auto phase = routine;
      phase.enters = suffix.empty();
      nativeRoutinePhases.emplace(symbol->index(),phase);
      }
    }

  using Handle=std::shared_ptr<daedalus::INpc>;
  vm.override_function("B_MAGICHURTNPC",std::function<void(Handle,Handle,int32_t)>{
    [this](Handle attacker, Handle target, int32_t damage) { magicHurtNpc(attacker,target,damage); }},
    [this] {
      return activityEvents && !floorDefer.active() && ActivityDefinitionRuntime::mode()!=ActivityDefinitionRuntime::Mode::Off;
      });
  }

bool GameScript::runMonsterScheduler(Npc& npc, const daedalus::DaedalusSymbol&, int& result) {
  auto& aivar = npc.handle().aivar;
  aivar[size_t(findSymbol("AIV_MM_PRIORITY")->get_int())] = findSymbol("PRIO_EAT")->get_int();
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
  if(desynchronize) {
    const auto delay = rollRandom(1000,&npc,true);
    findSymbol("B_MM_DESYNCHRONIZE.MSEC")->set_int(int32_t(delay));
    if(delay>0)
      npc.aiPush(AiQueue::aiWait(delay));
    }
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
  // B_KILLNPC resolves the live NPC through the VM's SELF symbol.
  if(npc.handle().guild==GIL_Stoneguardian && findSymbol("RAVENISDEAD")->get_int()==1)
    vm.call_function<void>("B_KILLNPC",int32_t(vm.global_self()->index()));
  }

bool GameScript::invokeNativeRoutine(Npc& npc, const daedalus::DaedalusSymbol& symbol, NpcActivityProbe::Phase& probe) {
  if(!activityEvents || floorDefer.active())
    return false;
  const auto it = nativeRoutinePhases.find(symbol.index());
  if(it==nativeRoutinePhases.end() || !(this->*it->second.callback)(npc,symbol,probe.result))
    return false;
  const auto& phase = it->second;
  probe.fromRecord();
  ActivityEvents::countWakes(phase.state,phase.enters ? ActivityEvents::Entered : 0,true);
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
  const auto& loop=findSymbol(npc.aiState.funcLoop.ptr)->name();
  auto draw=[&](int32_t range,const std::string& variable) {
    const auto value=int32_t(rollRandom(uint32_t(range),&npc,true));
    if(auto* local=findSymbol(loop+variable))
      local->set_int(value);
    return value;
    };
  if(const auto* mob=activity.mob) {
    const auto& timer=*mob->timer;
    if(timer.rollRange>0) {
      const auto roll=draw(timer.rollRange,timer.rollVariable);
      for(const auto& gesture:timer.gestures)
        if(roll<=gesture.throughRoll)
          npc.pushActivityCommand(AiQueue::aiPlayAnimBs(gesture.animation,BodyState(mob->busyBodyState)));
      }
    npc.setStateTime(0);
    return;
    }
  const auto& point=*activity.point;
  const auto& gesture=point.gestures[size_t(draw(point.rollRange,".RANDOM"))];
  if(!gesture.animation.empty())
    npc.pushActivityCommand(AiQueue::aiPlayAnim(gesture.animation));
  if(point.portal) {
    const auto& portal=*point.portal;
    const auto roll=draw(portal.rollRange,".EVENTRANDY");
    const auto guild=npc.handle().guild;
    const auto dead=findSymbol("RAVENISDEAD")->get_int();
    const auto level=findSymbol("CURRENTLEVEL")->get_int();
    if(roll==portal.triggerRoll && guild!=portal.excludedGuild && dead==portal.ravenDead && level==portal.world)
      npc.invokeRoutine(findFunction(portal.function));
    }
  if(gesture.resetTimer)
    npc.setStateTime(0);
  }

bool GameScript::invokeActivity(Npc& npc, uint32_t function, NpcActivityProbe::Phase& probe) {
  // The startup floor fix wraps the script externals; inside its window the phase keeps the VM path.
  if(floorDefer.active())
    return false;
  const auto* phase = ActivityDefinitionRuntime::find(activityDefinitions,function);
  if(phase==nullptr || (!activityEvents && (phase->record.point || phase->record.mob->timer ||
                                             phase->record.mob->skipApproachWhenBusy)))
    return false;
  probe.fromRecord();
  probe.result = ActivityDefinitionRuntime::run(vm,world(),npc,*phase,[this,&npc](int32_t item) {
    storeItem(npc.addItem(uint32_t(item),1));
    });
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
     !ActivityDefinitionRuntime::accepted(vm.find_symbol_by_index(uint32_t(init.ptr))->name())))
    return nullptr;
  return &phase->record;
  }

std::vector<std::string_view> GameScript::eventActivityNames() {
  std::vector<std::string_view> names;
  for(const auto& [id,phase]:activityDefinitions.phases)
    if(phase.phase==ActivityDefinitions::Phase::Init && eventActivity(ScriptFn(id)))
      names.push_back(symbolName(id));
  std::sort(names.begin(),names.end());
  return names;
  }
