#include "placementdefinitionruntime.h"

#include "placementloadprobe.h"

#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <stdexcept>
#include <unordered_set>

namespace {
using Type = daedalus::DaedalusDataType;
using Op = daedalus::DaedalusOpcode;
using Value = PlacementDefinitions::Value;
using Kind = Value::Kind;

Value resolveValue(Value value, const daedalus::DaedalusScript& script) {
  if(value.kind==Kind::Symbol) {
    const auto* symbol=script.find_symbol_by_name(value.text);
    if(symbol==nullptr) throw std::runtime_error("unknown symbol "+value.text);
    value.integer=int32_t(symbol->index());
    value.text.clear();
    }
  return value;
  }

void checkValue(const Value& value, Type type) {
  bool ok=false;
  switch(type) {
    case Type::STRING: ok=value.kind==Kind::String; break;
    case Type::FLOAT: ok=value.kind==Kind::Float; break;
    case Type::INSTANCE: ok=value.kind==Kind::Self || value.kind==Kind::Symbol || value.kind==Kind::Null; break;
    case Type::INT:
    case Type::FUNCTION: ok=value.kind==Kind::Int || value.kind==Kind::Symbol || value.kind==Kind::Null; break;
    default: break;
    }
  if(!ok) throw std::runtime_error("value type differs from script binding");
  }

void push(daedalus::DaedalusVm& vm, const PlacementDefinitionRuntime::Argument& arg) {
  const auto& value=arg.value;
  switch(value.kind) {
    case Kind::Int: vm.push_int(value.integer); break;
    case Kind::Float: vm.push_float(value.real); break;
    case Kind::String: vm.push_reference(vm.find_symbol_by_index(arg.literal),arg.literalIndex); break;
    case Kind::Self: vm.push_instance(vm.global_self() ? vm.global_self()->get_instance() : nullptr); break;
    case Kind::Symbol:
      if(arg.type==Type::INSTANCE) vm.push_reference(vm.find_symbol_by_index(uint32_t(value.integer)));
      else vm.push_int(value.integer);
      break;
    case Kind::Null:
      if(arg.type==Type::INSTANCE) vm.push_instance(nullptr);
      else vm.push_int(0);
      break;
    }
  }

std::unordered_set<uint32_t> placementFunctions(const daedalus::DaedalusScript& script) {
  std::map<uint32_t,uint32_t> addresses;
  std::vector<uint32_t> pending;
  for(const auto& symbol:script.symbols())
    if(!symbol.is_external() && (symbol.type()==Type::FUNCTION || symbol.type()==Type::PROTOTYPE ||
        (symbol.type()==Type::INSTANCE && symbol.is_const() && symbol.address()>0 && symbol.name().find('.')==std::string::npos)) &&
       symbol.address()<script.size()) {
      addresses.emplace(symbol.address(),symbol.index());
      if(symbol.name().starts_with("STARTUP_")) pending.push_back(symbol.address());
      }
  std::set<uint32_t> visited;
  std::unordered_set<uint32_t> result;
  while(!pending.empty()) {
    const auto address=pending.back(); pending.pop_back();
    if(!visited.insert(address).second) continue;
    const auto found=addresses.find(address);
    if(found==addresses.end()) throw std::runtime_error("placement probe: missing function at "+std::to_string(address));
    result.insert(found->second);
    const auto next=std::next(found);
    const auto end=next==addresses.end() ? script.size() : next->first;
    for(auto pc=address;pc<end;) {
      const auto op=script.instruction_at(pc);
      if(op.op==Op::BL) pending.push_back(op.address);
      pc+=op.size;
      }
    }
  return result;
  }
}

bool PlacementDefinitionRuntime::enabled() {
  const auto* value=std::getenv("OPENGOTHIC_PLACEMENT_RECORDS");
  return value==nullptr || std::strcmp(value,"0")!=0;
  }

PlacementDefinitionRuntime::Definitions PlacementDefinitionRuntime::resolve(
    const PlacementDefinitions::Library& library, const daedalus::DaedalusScript& script) {
  Definitions definitions;
  std::map<std::string,std::pair<uint32_t,uint8_t>> literals;
  for(const auto& symbol:script.symbols())
    if(symbol.type()==Type::STRING && symbol.is_const() && !symbol.is_member())
      for(uint32_t i=0;i<symbol.count() && i<=UINT8_MAX;++i)
        literals.try_emplace(symbol.get_string(uint16_t(i)),symbol.index(),uint8_t(i));
  for(const auto& [name,definition]:library.placements) {
    try {
      const auto* symbol=script.find_symbol_by_name(name);
      if(symbol==nullptr || symbol->type()!=Type::FUNCTION || symbol->is_external() ||
         symbol->has_return() || symbol->count()!=0 || !symbol->name().starts_with("STARTUP_"))
        throw std::runtime_error("invalid startup function");
      std::vector<Call> steps;
      for(const auto& call:definition.steps) {
        const auto* function=script.find_symbol_by_name(call.function);
        if(function==nullptr || function->type()!=Type::FUNCTION || function->is_external()==call.deferred ||
           (function->has_return() && (call.deferred || function->rtype()!=Type::INT)))
          throw std::runtime_error("invalid call "+call.function);
        const auto parameters=script.find_parameters_for_function(function);
        if(parameters.size()!=call.args.size()) throw std::runtime_error("argument count differs for "+call.function);
        Call resolved;
        resolved.function=function->index(); resolved.deferred=call.deferred;
        for(size_t i=0;i<parameters.size();++i) {
          checkValue(call.args[i],parameters[i].type());
          Argument argument{resolveValue(call.args[i],script),parameters[i].type()};
          if(argument.value.kind==Kind::String) {
            const auto literal=literals.find(argument.value.text);
            if(literal==literals.end()) throw std::runtime_error("no immutable string literal for "+call.function);
            argument.literal=literal->second.first;
            argument.literalIndex=literal->second.second;
            argument.value.text.clear();
            }
          resolved.args.push_back(std::move(argument));
          }
        steps.push_back(std::move(resolved));
        }
      definitions.placements.emplace(symbol->index(),std::move(steps));
      }
    catch(const std::exception& e) { throw std::runtime_error("placement definitions: "+name+": "+e.what()); }
    }
  return definitions;
  }

PlacementDefinitionRuntime::Definitions PlacementDefinitionRuntime::load(
    const std::filesystem::path& base, const daedalus::DaedalusScript& program, std::string_view sha256) {
  return enabled() ? resolve(PlacementDefinitions::read(base,sha256),program) : Definitions{};
  }

void PlacementDefinitionRuntime::observeCalls(daedalus::DaedalusVm& vm, const std::filesystem::path& base,
                                             std::string_view sha256) {
  if(!LoadProbe::Placement::enabled()) return;
  auto functions=placementFunctions(vm);
  std::unordered_set<uint32_t> deferred;
  const auto library=PlacementDefinitions::read(base,sha256);
  for(const auto& [name,definition]:library.placements) {
    (void)name;
    for(const auto& call:definition.steps)
      if(call.deferred) {
        const auto* function=vm.find_symbol_by_name(call.function);
        if(function==nullptr) throw std::runtime_error("placement probe: unknown helper "+call.function);
        deferred.insert(function->index());
        }
    }
  vm.add_call_observer([functions=std::move(functions),deferred=std::move(deferred)](const auto& function) {
    if(!LoadProbe::Placement::depth) return;
    if(functions.contains(function.index()))
      LoadProbe::Placement::functions.fetch_add(1,std::memory_order_relaxed);
    if(LoadProbe::Placement::fallbackDepth && deferred.contains(function.index()))
      LoadProbe::Placement::deferredCalls.fetch_add(1,std::memory_order_relaxed);
    });
  }

bool PlacementDefinitionRuntime::execute(daedalus::DaedalusVm& vm, const daedalus::DaedalusSymbol* function,
                                         const Definitions& definitions) {
  const auto found=function ? definitions.placements.find(function->index()) : definitions.placements.end();
  const bool playback=enabled() && found!=definitions.placements.end();
  LoadProbe::Placement::Scope probe(!playback);
  if(!playback) {
    vm.call_function(function);
    return false;
    }
  for(const auto& call:found->second) {
    for(const auto& argument:call.args) push(vm,argument);
    const auto* target=vm.find_symbol_by_index(call.function);
    if(call.deferred) {
      if(LoadProbe::Placement::enabled())
        LoadProbe::Placement::deferredCalls.fetch_add(1,std::memory_order_relaxed);
      vm.unsafe_call_bl(target);
      }
    else if(!vm.unsafe_call_external(target))
      throw std::runtime_error("placement definitions: external is not registered: "+target->name());
    else if(target->has_return())
      (void)vm.pop_int(); // Import only records external results discarded by a void script frame.
    }
  return true;
  }
