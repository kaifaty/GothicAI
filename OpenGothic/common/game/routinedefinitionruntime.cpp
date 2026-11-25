#include "routinedefinitionruntime.h"
#include "game/definitions/nativebasecatalog.h"
#include "routineloadprobe.h"

#include <cstdlib>
#include <cstring>
#include <stdexcept>

namespace {
using namespace RoutineDefinitionRuntime;

template<class ResolveRoutine, class ResolveState>
Definitions resolveRows(const RoutineDefinitions::Library& library, ResolveRoutine routineId, ResolveState stateId) {
  Definitions definitions;
  for(const auto& [name,rows]:library.routines) {
    try {
      const auto function=routineId(name);
      if(!function)
        throw std::runtime_error("expected a parameterless void routine function");
      std::vector<Row> resolved;
      resolved.reserve(rows.size());
      for(size_t i=0;i<rows.size();++i) {
        const auto& row=rows[i];
        const auto state=stateId(row.state);
        if(!state)
          throw std::runtime_error("row "+std::to_string(i+1)+": unknown state function "+row.state);
        resolved.push_back({row.startHour,row.startMinute,row.endHour,row.endMinute,*state,row.waypoint});
        }
      if(!definitions.routines.emplace(*function,Routine{name,std::move(resolved)}).second)
        throw std::runtime_error("duplicate routine ID");
      definitions.names.emplace(name,*function);
      }
    catch(const std::exception& e) { throw std::runtime_error("routine definitions: "+name+": "+e.what()); }
    }
  return definitions;
  }
}

#ifndef GOTHIC_NATIVE_V3
RoutineDefinitionRuntime::Definitions RoutineDefinitionRuntime::resolve(const RoutineDefinitions::Library& library,
                                                                        const daedalus::DaedalusScript& script) {
  return resolveRows(library,[&](std::string_view name) -> std::optional<uint32_t> {
    const auto* function=script.find_symbol_by_name(name);
    if(function==nullptr || function->type()!=daedalus::DaedalusDataType::FUNCTION ||
       function->is_external() || function->count()!=0 || function->has_return())
      return std::nullopt;
    return function->index();
    },[&](std::string_view name) -> std::optional<uint32_t> {
    const auto* function=script.find_symbol_by_name(name);
    if(function==nullptr || function->type()!=daedalus::DaedalusDataType::FUNCTION)
      return std::nullopt;
    return function->index();
    });
  }
#endif

bool RoutineDefinitionRuntime::enabled() {
  const auto* value=std::getenv("OPENGOTHIC_ROUTINE_RECORDS");
  return value==nullptr || std::strcmp(value,"0")!=0;
  }

#ifndef GOTHIC_NATIVE_V3
RoutineDefinitionRuntime::Definitions RoutineDefinitionRuntime::load(const std::filesystem::path& base,
                                                                     const daedalus::DaedalusScript& program,
                                                                     std::string_view sha256) {
  if(enabled())
    return resolve(RoutineDefinitions::read(base,sha256),program);
  return {};
  }
#endif

RoutineDefinitionRuntime::Definitions RoutineDefinitionRuntime::load(const std::filesystem::path& base,
                                                                     const NativeBaseCatalog::Selection& catalog,
                                                                     std::string_view sha256) {
  const auto library=RoutineDefinitions::read(base,sha256);
  if(catalog.routines.size()!=library.routines.size())
    throw std::runtime_error("routine definitions: prepared routine count differs from the catalog");
  std::map<std::string_view,uint32_t,std::less<>> routines,states;
  for(const auto& call : catalog.routines)
    routines.emplace(call.name,call.id);
  for(const auto& state : catalog.states)
    states.emplace(state.name,state.id);
  const auto find = [](const auto& names,std::string_view name) -> std::optional<uint32_t> {
    const auto entry=names.find(name);
    return entry!=names.end() ? std::optional(entry->second) : std::nullopt;
    };
  auto definitions=resolveRows(library,[&](std::string_view name) { return find(routines,name); },
                                       [&](std::string_view name) { return find(states,name); });
  definitions.preparedOnly=true;
  return definitions;
  }

#ifndef GOTHIC_NATIVE_V3
int RoutineDefinitionRuntime::initialize(daedalus::DaedalusVm& vm, daedalus::DaedalusSymbol* function,
                                         const Definitions& definitions, const std::function<void(const Row&)>& add) {
  if(function==nullptr) return 0;
  const auto found=definitions.routines.find(function->index());
  if(found!=definitions.routines.end()) {
    // Pure Rtn/TA wrappers restore PC, context and stack. Their only surviving
    // symbols are wrapper parameters, overwritten in each wrapper's prologue.
    for(const auto& row:found->second.rows) add(row);
    return 0;
    }
  LoadProbe::Routines::Function probe;
  if(function->rtype()==daedalus::DaedalusDataType::INT) return vm.call_function<int>(function);
  vm.call_function<void>(function);
  return 0;
  }
#endif
