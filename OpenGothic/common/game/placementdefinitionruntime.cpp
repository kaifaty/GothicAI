#include "placementdefinitionruntime.h"

#include "placementloadprobe.h"

#include <cstdlib>
#include <cstring>
#include <map>
#include <stdexcept>
#include <unordered_set>

namespace {
using Type = daedalus::DaedalusDataType;
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

}

bool PlacementDefinitionRuntime::enabled() {
  const auto* value=std::getenv("OPENGOTHIC_PLACEMENT_RECORDS");
  return value==nullptr || std::strcmp(value,"0")!=0;
  }

PlacementDefinitionRuntime::Definitions PlacementDefinitionRuntime::resolve(
    const PlacementDefinitions::Library& library, const daedalus::DaedalusScript& script) {
  Definitions definitions;
  const auto validate = [&script](const auto& rows,bool onlyNpcs) {
    for(const auto& row:rows) {
      const auto* instance=script.find_symbol_by_index(row.instance);
      const auto* point=script.find_symbol_by_index(row.point);
      const auto* external=script.find_symbol_by_index(row.external);
      if(!instance || instance->type()!=Type::INSTANCE || !instance->is_const() ||
         !point || point->type()!=Type::STRING || !point->is_const() || point->count()==0 || point->is_member() ||
         !external || !external->is_external() || (external->name()!="WLD_INSERTNPC" && (onlyNpcs || external->name()!="WLD_INSERTITEM")))
        throw std::runtime_error("placement definitions: invalid chapter placement symbol");
      }
    };
  if(library.khorinisLateChapters) {
    for(const auto& rows:library.khorinisLateChapters->chapter4) validate(rows,false);
    for(const auto& rows:library.khorinisLateChapters->chapter5) validate(rows,false);
    definitions.khorinisLateChapters=library.khorinisLateChapters;
    }
  if(library.khorinisEarlyChapters) {
    validate(library.khorinisEarlyChapters->chapter2,true);
    for(const auto& rows:library.khorinisEarlyChapters->chapter3) validate(rows,false);
    definitions.khorinisEarlyChapters=library.khorinisEarlyChapters;
    }
  for(const auto& [name,groups]:library.triggerInserts) {
    const bool onlyNpcs=name!="EVT_ORNAMENT_SWITCH_FOREST_01_FUNC" && name!="EVT_ORNAMENT_SWITCH_BIGFARM_01_FUNC" &&
                        name!="EVT_TELEPORTSTATION_FUNC";
    for(const auto& rows:groups)
      validate(rows,onlyNpcs);
    }
  definitions.triggerInserts=library.triggerInserts;
  validate(library.addonChapter4,true);
  definitions.addonChapter4=library.addonChapter4;
  if(library.oldWorldChapters) {
    for(const auto& rows:library.oldWorldChapters->chapter3) validate(rows,false);
    for(const auto& rows:library.oldWorldChapters->chapter4) validate(rows,false);
    definitions.oldWorldChapters=library.oldWorldChapters;
    }
  validate(library.dragonIsland,false);
  definitions.dragonIsland=library.dragonIsland;
  const auto scalar = [&script](uint32_t index,Type type,bool constant) {
    const auto* symbol=script.find_symbol_by_index(index);
    return symbol && symbol->type()==type && symbol->is_const()==constant && !symbol->is_member() && symbol->count()==1;
    };
  const auto roomAssignment = [&](const PlacementDefinitions::RoomAssignment& room) {
    return scalar(room.label,Type::STRING,true) && (!room.guild || scalar(*room.guild,Type::INT,true));
    };
  for(const auto& [name,region]:library.roomInitializers) {
    const auto* root=script.find_symbol_by_name(name);
    if(!root || root->type()!=Type::FUNCTION || root->is_external() || root->count()!=0 || root->has_return())
      throw std::runtime_error("placement definitions: invalid room initializer "+name);
    for(const auto& mob:region.mobs)
      if(!scalar(mob.name,Type::STRING,true))
        throw std::runtime_error("placement definitions: invalid mob routine in "+name);
    for(const auto& room:region.rooms) {
      bool valid=false;
      if(const auto* fixed=std::get_if<PlacementDefinitions::RoomAssignment>(&room))
        valid=roomAssignment(*fixed);
      else {
        const auto& choice=std::get<PlacementDefinitions::RoomChoice>(room);
        valid=scalar(choice.expected,Type::INT,true) && scalar(choice.flag,Type::INT,false) &&
              roomAssignment(choice.equal) && roomAssignment(choice.otherwise);
        }
      if(!valid)
        throw std::runtime_error("placement definitions: invalid room assignment in "+name);
      }
    }
  definitions.roomInitializers=library.roomInitializers;
  if(library.firstEntries) {
    for(const auto* actors:{&library.firstEntries->oldWorld,&library.firstEntries->addonWorld})
      for(const auto index:*actors) {
        const auto* actor=script.find_symbol_by_index(index);
        if(!actor || actor->type()!=Type::INSTANCE)
          throw std::runtime_error("placement definitions: invalid first-entry actor");
        }
    definitions.firstEntries=library.firstEntries;
    }
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
      std::vector<Step> steps;
      for(const auto& step:definition.steps) {
        if(const auto* write=std::get_if<PlacementDefinitions::GlobalWrite>(&step)) {
          const auto* target=script.find_symbol_by_name(write->symbol);
          if(target==nullptr || target->is_const() || target->is_member() || write->index>=target->count())
            throw std::runtime_error("invalid global write "+write->symbol+"["+std::to_string(write->index)+"]");
          const auto* parent=script.find_symbol_by_index(target->parent());
          const auto dot=target->name().find('.');
          const auto* owner=dot==std::string::npos ? nullptr : script.find_symbol_by_name(target->name().substr(0,dot));
          if((parent && parent->type()==Type::FUNCTION) || (owner && owner->type()==Type::FUNCTION))
            throw std::runtime_error("global write targets a function local: "+write->symbol);
          const auto kind=write->value.kind;
          if(!((target->type()==Type::INT && kind==Kind::Int) ||
               (target->type()==Type::FLOAT && kind==Kind::Float) ||
               (target->type()==Type::STRING && kind==Kind::String)))
            throw std::runtime_error("global write type differs from script binding: "+write->symbol);
          if(kind==Kind::String && !literals.contains(write->value.text))
            throw std::runtime_error("no immutable string literal for global write "+write->symbol);
          steps.emplace_back(GlobalWrite{target->index(),write->index,write->value});
          continue;
          }
        const auto& call=std::get<PlacementDefinitions::Call>(step);
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
        steps.emplace_back(std::in_place_type<Call>,std::move(resolved));
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
  auto library=PlacementDefinitions::read(base,sha256);
  const auto functionIds=library.startupFunctions ? std::move(*library.startupFunctions) : startupFunctions(vm);
  for(const auto index:functionIds)
    if(vm.find_symbol_by_index(index)==nullptr)
      throw std::runtime_error("placement probe: unknown startup function index "+std::to_string(index));
  std::unordered_set<uint32_t> functions(functionIds.begin(),functionIds.end());
  std::unordered_set<uint32_t> deferred;
  for(const auto& [name,definition]:library.placements) {
    (void)name;
    for(const auto& step:definition.steps)
      if(const auto* call=std::get_if<PlacementDefinitions::Call>(&step); call && call->deferred) {
        const auto* function=vm.find_symbol_by_name(call->function);
        if(function==nullptr) throw std::runtime_error("placement probe: unknown helper "+call->function);
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
  for(const auto& step:found->second) {
    if(const auto* write=std::get_if<GlobalWrite>(&step)) {
      auto* target=vm.find_symbol_by_index(write->symbol);
      if(write->value.kind==Kind::String) target->set_string(write->value.text,write->index);
      else if(write->value.kind==Kind::Float) target->set_float(write->value.real,write->index);
      else target->set_int(write->value.integer,write->index);
      continue;
      }
    const auto& call=std::get<Call>(step);
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
