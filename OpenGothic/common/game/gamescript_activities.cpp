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
