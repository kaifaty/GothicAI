#include "gamescript.h"

#include "world/world.h"

bool GameScript::invokeActivity(Npc& npc, uint32_t function, NpcActivityProbe::Phase& probe) {
  // The startup floor fix wraps the script externals; inside its window the phase keeps the VM path.
  if(floorDefer.active())
    return false;
  const auto* phase = ActivityDefinitionRuntime::find(activityDefinitions,function);
  if(phase==nullptr)
    return false;
  probe.fromRecord();
  probe.result = ActivityDefinitionRuntime::run(vm,world(),npc,*phase);
  return true;
  }

const ActivityDefinitions::MobActivity* GameScript::eventActivity(ScriptFn init) {
  // A Loop that ends the state is not an activity to hold: such a record keeps the polled Loop.
  if(!activityEvents || !init.isValid() || floorDefer.active())
    return nullptr;
  const auto* phase = ActivityDefinitionRuntime::find(activityDefinitions,uint32_t(init.ptr));
  if(phase==nullptr || phase->phase!=ActivityDefinitions::Phase::Init || phase->activity->loopResult!=LOOP_CONTINUE)
    return nullptr;
  return phase->activity;
  }
