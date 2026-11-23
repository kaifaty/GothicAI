#include "gamescript.h"

#include "world/objects/npc.h"

#include <algorithm>

void GameScript::runActivityGesture(Npc& npc, const ActivityEvents::Record& activity) {
  const auto& loop=findSymbol(npc.aiState.funcLoop.ptr)->name();
  auto draw=[&](int32_t range,const char* variable) {
    const auto value=int32_t(rollRandom(uint32_t(range),&npc,true));
    if(auto* local=findSymbol(loop+variable))
      local->set_int(value);
    return value;
    };
  if(const auto* mob=activity.mob) {
    const auto& seated=*mob->seated;
    const auto roll=draw(seated.rollRange,".RANDOM");
    for(const auto& animation:seated.animationsFor(roll))
      npc.pushActivityCommand(AiQueue::aiPlayAnimBs(animation,BodyState(mob->busyBodyState)));
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
  if(phase==nullptr || (!activityEvents && (phase->record.point || phase->record.mob->seated)))
    return false;
  probe.fromRecord();
  probe.result = ActivityDefinitionRuntime::run(vm,world(),npc,*phase);
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
