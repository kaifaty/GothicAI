#include "routinedefinitionruntime.h"
#include "routineloadprobe.h"

#include <cstdlib>
#include <cstring>
#include <stdexcept>

RoutineDefinitionRuntime::Definitions RoutineDefinitionRuntime::resolve(const RoutineDefinitions::Library& library,
                                                                        const daedalus::DaedalusScript& script) {
  Definitions definitions;
  for(const auto& [name,rows]:library.routines) {
    try {
      const auto* function=script.find_symbol_by_name(name);
      if(function==nullptr || function->type()!=daedalus::DaedalusDataType::FUNCTION ||
         function->is_external() || function->count()!=0 || function->has_return())
        throw std::runtime_error("expected a parameterless void routine function");
      std::vector<Row> resolved;
      resolved.reserve(rows.size());
      for(size_t i=0;i<rows.size();++i) {
        const auto& row=rows[i];
        const auto* state=script.find_symbol_by_name(row.state);
        if(state==nullptr || state->type()!=daedalus::DaedalusDataType::FUNCTION)
          throw std::runtime_error("row "+std::to_string(i+1)+": unknown state function "+row.state);
        resolved.push_back({row.startHour,row.startMinute,row.endHour,row.endMinute,state->index(),row.waypoint});
        }
      definitions.routines.emplace(function->index(),std::move(resolved));
      }
    catch(const std::exception& e) { throw std::runtime_error("routine definitions: "+name+": "+e.what()); }
    }
  return definitions;
  }

RoutineDefinitionRuntime::Definitions RoutineDefinitionRuntime::load(const std::filesystem::path& base,
                                                                     const daedalus::DaedalusScript& program,
                                                                     std::string_view sha256) {
  const auto* enabled=std::getenv("OPENGOTHIC_ROUTINE_RECORDS");
  if(enabled==nullptr || std::strcmp(enabled,"0")!=0)
    return resolve(RoutineDefinitions::read(base,sha256),program);
  return {};
  }

int RoutineDefinitionRuntime::initialize(daedalus::DaedalusVm& vm, daedalus::DaedalusSymbol* function,
                                         const Definitions& definitions, const std::function<void(const Row&)>& add) {
  if(function==nullptr) return 0;
  const auto found=definitions.routines.find(function->index());
  if(found!=definitions.routines.end()) {
    // Pure Rtn/TA wrappers restore PC, context and stack. Their only surviving
    // symbols are wrapper parameters, overwritten in each wrapper's prologue.
    for(const auto& row:found->second) add(row);
    return 0;
    }
  LoadProbe::Routines::Function probe;
  if(function->rtype()==daedalus::DaedalusDataType::INT) return vm.call_function<int>(function);
  vm.call_function<void>(function);
  return 0;
  }
