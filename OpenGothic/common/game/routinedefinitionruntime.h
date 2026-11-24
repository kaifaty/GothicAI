#pragma once

#include "routinedefinitions.h"
#include "daedalus/vm.h"

#include <functional>
#include <unordered_map>

namespace RoutineDefinitionRuntime {
struct Row {
  int startHour=0, startMinute=0, endHour=0, endMinute=0;
  uint32_t state=0;
  std::string waypoint;
  };
struct Definitions {
  std::unordered_map<uint32_t,std::vector<Row>> routines;
  std::map<std::string,uint32_t,std::less<>> names;
  };
bool enabled();
Definitions resolve(const RoutineDefinitions::Library& library, const daedalus::DaedalusScript& script);
// Select the section by the verified program-record SHA and the game's environment switch.
Definitions load(const std::filesystem::path& base, const daedalus::DaedalusScript& program,
                 std::string_view sha256);
// The caller retains its existing SELF/OTHER/VICTIM scopes and table clearing policy.
// Both the game and its contract use this complete choice of record or VM execution.
int initialize(daedalus::DaedalusVm& vm, daedalus::DaedalusSymbol* function,
               const Definitions& definitions, const std::function<void(const Row&)>& add);
}
