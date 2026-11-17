#include "gamescript.h"

int GameScript::initializeRoutine(const std::shared_ptr<daedalus::INpc>& npc,
                                   daedalus::DaedalusSymbol* function) {
  return RoutineDefinitionRuntime::initialize(vm,function,routineDefinitions,[&](const auto& row) {
    ta_min(npc,row.startHour,row.startMinute,row.endHour,row.endMinute,int(row.state),row.waypoint);
    });
  }
