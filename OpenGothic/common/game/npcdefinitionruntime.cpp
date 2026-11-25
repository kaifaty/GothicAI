#include "npcdefinitionruntime.h"

#include "game/definitions/nativebasecatalog.h"
#include "npcloadprobe.h"

#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <unordered_set>

namespace {
using Type = daedalus::DaedalusDataType;
using Value = NpcDefinitions::Value;
using Kind = Value::Kind;

template<class Find>
Value resolveValue(Value value, const Find& find) {
  if(value.kind==Kind::Symbol) {
    const auto id=find(value.text);
    if(!id) throw std::runtime_error("unknown symbol "+value.text);
    value.integer=int32_t(*id);
    value.text.clear();
    }
  return value;
  }

Type argumentType(NativeBaseCatalog::NpcConstructorType type) {
  using Prepared = NativeBaseCatalog::NpcConstructorType;
  switch(type) {
    case Prepared::Integer:  return Type::INT;
    case Prepared::Float:    return Type::FLOAT;
    case Prepared::String:   return Type::STRING;
    case Prepared::Instance: return Type::INSTANCE;
    case Prepared::Function: return Type::FUNCTION;
    }
  throw std::runtime_error("invalid NPC constructor type");
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

// init_instance restores both SELF and the VM's member context. External calls may
// execute item on_equip scripts, so both must remain the NPC throughout playback.
struct Context {
  daedalus::DaedalusVm& vm;
  std::shared_ptr<daedalus::DaedalusInstance> previous, self;
  Context(daedalus::DaedalusVm& vm, const std::shared_ptr<daedalus::INpc>& npc)
      :vm(vm),previous(vm.unsafe_get_gi()),self(vm.global_self() ? vm.global_self()->get_instance() : nullptr) {
    vm.unsafe_set_gi(npc);
    if(vm.global_self()) vm.global_self()->set_instance(npc);
    }
  ~Context() {
    vm.unsafe_set_gi(previous);
    if(vm.global_self()) vm.global_self()->set_instance(self);
    }
  };

void push(daedalus::DaedalusVm& vm, const NpcDefinitionRuntime::Argument& arg,
          const std::shared_ptr<daedalus::INpc>& npc) {
  const auto& value=arg.value;
  switch(value.kind) {
    case Kind::Int: vm.push_int(value.integer); break;
    case Kind::Float: vm.push_float(value.real); break;
    case Kind::String:
      if(arg.oracleLiteral) {
        const auto [symbol,index] = *arg.oracleLiteral;
        vm.push_reference(vm.find_symbol_by_index(symbol),index);
        } else if(vm.has_legacy_dispatcher()) {
        throw std::runtime_error("NPC definitions: no immutable string literal for the VM replay");
        } else {
        vm.push_string(value.text);
        }
      break;
    case Kind::Self: vm.push_instance(npc); break;
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

bool NpcDefinitionRuntime::enabled() {
  const auto* value=std::getenv("OPENGOTHIC_NPC_RECORDS");
  return value==nullptr || std::strcmp(value,"0")!=0;
  }

NpcDefinitionRuntime::Definitions NpcDefinitionRuntime::resolve(const NpcDefinitions::Library& library,
                                                               const daedalus::DaedalusScript& script) {
  Definitions definitions;
  const auto symbolId=[&](std::string_view name) -> std::optional<uint32_t> {
    const auto* symbol=script.find_symbol_by_name(name);
    return symbol ? std::optional(symbol->index()) : std::nullopt;
    };
  // Retain original constant references for VM comparisons when the input has them.
  std::map<std::string,std::pair<uint32_t,uint8_t>> literals;
  for(const auto& symbol:script.symbols())
    if(symbol.type()==Type::STRING && symbol.is_const() && !symbol.is_member())
      for(uint32_t i=0;i<symbol.count() && i<=UINT8_MAX;++i)
        literals.try_emplace(symbol.get_string(uint16_t(i)),symbol.index(),uint8_t(i));
  for(const auto& [name,definition]:library.npcs) {
    try {
      const auto* symbol=script.find_symbol_by_name(name);
      if(symbol==nullptr || symbol->type()!=Type::INSTANCE) throw std::runtime_error("unknown NPC instance");
      std::vector<Step> steps;
      for(const auto& step:definition.steps) {
        if(const auto* field=std::get_if<NpcDefinitions::Field>(&step)) {
          const auto* binding=script.find_symbol_by_name(field->binding);
          if(binding==nullptr || !binding->is_member() || !binding->name().starts_with("C_NPC.") ||
             field->index>=binding->count())
            throw std::runtime_error("invalid NPC field "+field->binding+"["+std::to_string(field->index)+"]");
          checkValue(field->value,binding->type());
          steps.emplace_back(Field{binding->index(),binding->name(),field->index,resolveValue(field->value,symbolId)});
          }
        else if(const auto* write=std::get_if<NpcDefinitions::GlobalIntWrite>(&step)) {
          const auto* binding=script.find_symbol_by_name(write->binding);
          if(library.programSha256!=NpcDefinitions::TalentWriteProgram || !NpcDefinitions::writesPlayerTalents(name) ||
             !binding || !NpcDefinitions::isPlayerTalentArray(binding->name()) ||
             binding->is_member() || binding->is_const() || binding->type()!=Type::INT || write->index>=binding->count())
            throw std::runtime_error("invalid NPC global write "+write->binding+"["+std::to_string(write->index)+"]");
          steps.emplace_back(GlobalIntWrite{binding->index(),write->index,write->value});
          }
        else {
          const auto& call=std::get<NpcDefinitions::Call>(step);
          const auto* function=script.find_symbol_by_name(call.function);
          if(function==nullptr || function->type()!=Type::FUNCTION || function->is_external()==call.deferred ||
             function->has_return()) throw std::runtime_error("invalid call "+call.function);
          auto parameters=script.find_parameters_for_function(function);
          if(parameters.size()!=call.args.size()) throw std::runtime_error("argument count differs for "+call.function);
          Call resolved;
          resolved.function=function->index(); resolved.name=function->name(); resolved.deferred=call.deferred;
          for(size_t i=0;i<parameters.size();++i) {
            checkValue(call.args[i],parameters[i].type());
            Argument argument{resolveValue(call.args[i],symbolId),parameters[i].type(),{}};
            if(argument.value.kind==Kind::String) {
              const auto literal=literals.find(argument.value.text);
              if(literal!=literals.end())
                argument.oracleLiteral=literal->second;
              }
            resolved.args.push_back(std::move(argument));
            }
          steps.emplace_back(std::move(resolved));
          }
        }
      definitions.npcs.emplace(symbol->index(),std::move(steps));
      }
    catch(const std::exception& e) { throw std::runtime_error("NPC definitions: "+name+": "+e.what()); }
    }
  return definitions;
  }

NpcDefinitionRuntime::Definitions NpcDefinitionRuntime::load(const std::filesystem::path& base,
                                                            const NativeBaseCatalog::Selection& catalog,
                                                            std::string_view sha256) {
  const auto library=NpcDefinitions::read(base,sha256);
  std::map<std::string_view,uint32_t,std::less<>> npcs,refs;
  for(const auto& npc : catalog.identities.npcs)
    npcs.emplace(npc.name,npc.id);
  for(const auto& item : catalog.identities.items)
    refs.emplace(item.name,item.id);
  for(const auto& state : catalog.states)
    refs.emplace(state.name,state.id);
  for(const auto& routine : catalog.routines)
    refs.emplace(routine.name,routine.id);
  std::map<std::string_view,const NativeBaseCatalog::InitialRow*,std::less<>> globals;
  for(const auto& row : catalog.initialState)
    if(NpcDefinitions::isPlayerTalentArray(row.name))
      globals.emplace(row.name,&row);
  const auto symbolId=[&](std::string_view name) -> std::optional<uint32_t> {
    const auto entry=refs.find(name);
    return entry!=refs.end() ? std::optional(entry->second) : std::nullopt;
    };

  Definitions definitions;
  for(const auto& [name,record] : library.npcs) {
    try {
      const auto npc=npcs.find(name);
      if(npc==npcs.end())
        throw std::runtime_error("unknown NPC identity");
      std::vector<Step> steps;
      steps.reserve(record.steps.size());
      for(const auto& step : record.steps) {
        if(const auto* field=std::get_if<NpcDefinitions::Field>(&step)) {
          const auto entry=catalog.npcConstructor.members.find(field->binding);
          if(entry==catalog.npcConstructor.members.end() || field->index>=entry->second.count)
            throw std::runtime_error("invalid NPC field "+field->binding+"["+std::to_string(field->index)+"]");
          checkValue(field->value,argumentType(entry->second.type));
          steps.emplace_back(Field{entry->second.id,field->binding,field->index,resolveValue(field->value,symbolId)});
          }
        else if(const auto* write=std::get_if<NpcDefinitions::GlobalIntWrite>(&step)) {
          const auto entry=globals.find(write->binding);
          const auto* values=entry==globals.end() ? nullptr : std::get_if<std::vector<int32_t>>(&entry->second->value);
          if(sha256!=NpcDefinitions::TalentWriteProgram || !NpcDefinitions::writesPlayerTalents(name) ||
             !values || write->index>=values->size())
            throw std::runtime_error("invalid NPC global write "+write->binding+"["+std::to_string(write->index)+"]");
          steps.emplace_back(GlobalIntWrite{entry->second->id,write->index,write->value});
          }
        else {
          const auto& call=std::get<NpcDefinitions::Call>(step);
          const auto entry=catalog.npcConstructor.calls.find(call.function);
          if(entry==catalog.npcConstructor.calls.end() || entry->second.deferred!=call.deferred ||
             entry->second.parameters.size()!=call.args.size())
            throw std::runtime_error("invalid NPC constructor call "+call.function);
          Call resolved;
          resolved.function=entry->second.id;
          resolved.name=call.function;
          resolved.deferred=call.deferred;
          resolved.args.reserve(call.args.size());
          for(size_t i=0;i<call.args.size();++i) {
            const auto type=argumentType(entry->second.parameters[i]);
            checkValue(call.args[i],type);
            resolved.args.push_back({resolveValue(call.args[i],symbolId),type,{}});
            }
          steps.emplace_back(std::move(resolved));
          }
        }
      definitions.npcs.emplace(npc->second,std::move(steps));
      }
    catch(const std::exception& e) { throw std::runtime_error("NPC definitions: "+name+": "+e.what()); }
    }
  return definitions;
  }

NpcDefinitionRuntime::Definitions NpcDefinitionRuntime::load(const std::filesystem::path& base,
                                                            const daedalus::DaedalusScript& program,
                                                            std::string_view sha256) {
  if(!enabled()) return {};
  auto definitions=resolve(NpcDefinitions::read(base,sha256),program);
  definitions.helpers=NpcHelperRuntime::load(base,program,sha256);
  return definitions;
  }

void NpcDefinitionRuntime::observeCalls(daedalus::DaedalusVm& vm, const Definitions& definitions,
                                       const std::filesystem::path& base, std::string_view sha256) {
  if(!LoadProbe::Npcs::enabled()) return;
  std::unordered_set<uint32_t> functions;
  if(enabled()) {
    for(const auto& [npc,steps]:definitions.npcs) {
      (void)npc;
      for(const auto& step:steps)
        if(const auto* call=std::get_if<Call>(&step); call && call->deferred)
          functions.insert(call->function);
      }
    }
  else {
    // Probe the same set of script functions on the constructor path, including
    // nested calls. Disabled playback otherwise needs no NPC definition file.
    const auto library=NpcDefinitions::read(base,sha256);
    for(const auto& [npc,definition]:library.npcs) {
      (void)npc;
      for(const auto& step:definition.steps)
        if(const auto* call=std::get_if<NpcDefinitions::Call>(&step); call && call->deferred) {
          const auto* function=vm.find_symbol_by_name(call->function);
          if(function==nullptr) throw std::runtime_error("NPC definitions: unknown probe function "+call->function);
          functions.insert(function->index());
          }
      }
    }
  vm.observe_calls([functions=std::move(functions)](const auto& function) {
    if(LoadProbe::Npcs::initializationDepth) LoadProbe::Npcs::scriptCalls.fetch_add(1,std::memory_order_relaxed);
    if(LoadProbe::Npcs::initializationDepth && functions.contains(function.index())) LoadProbe::Npcs::deferred();
    });
  }

bool NpcDefinitionRuntime::initialize(daedalus::DaedalusVm& vm, const std::shared_ptr<daedalus::INpc>& npc,
                                      daedalus::DaedalusSymbol* symbol, const Definitions& definitions,
                                      daedalus::DaedalusSymbol* definitionSymbol, const ApplyCall& applyCall,
                                      ApplyField applyField) {
  LoadProbe::Npcs::Initialization probe;
  const auto* source=definitionSymbol ? definitionSymbol : symbol;
  const auto found=source ? definitions.npcs.find(source->index()) : definitions.npcs.end();
  if(found==definitions.npcs.end()) {
    LoadProbe::Npcs::Constructor probe;
    vm.init_instance(npc,symbol);
    return false;
    }
  vm.allocate_instance(npc,symbol);
  Context context(vm,npc);
  for(const auto& step:found->second) {
    if(const auto* field=std::get_if<Field>(&step)) {
      if(applyField) {
        applyField(*npc,*field);
        continue;
        }
      auto* binding=vm.find_symbol_by_index(field->binding);
      const auto& value=field->value;
      if(value.kind==Kind::String) binding->set_string(value.text,field->index,npc.get());
      else if(value.kind==Kind::Float) binding->set_float(value.real,field->index,npc.get());
      else binding->set_int(value.integer,field->index,npc.get());
      }
    else if(const auto* write=std::get_if<GlobalIntWrite>(&step)) {
      vm.set_int(nullptr,vm.find_symbol_by_index(write->binding),write->index,write->value);
      }
    else {
      const auto& call=std::get<Call>(step);
      const auto* function=vm.find_symbol_by_index(call.function);
      const auto pushArguments = [&] {
        for(const auto& argument:call.args)
          push(vm,argument,npc);
        };
      if(applyCall) {
        std::function<void()> observe;
        if(!call.deferred && vm.has_external_observer())
          observe = [&] { vm.observe_bound_external(*function,pushArguments); };
        if(applyCall(*function,call,observe))
          continue;
        }
      pushArguments();
      if(call.deferred) {
        if(!NpcHelperRuntime::invoke(vm,npc,function,definitions.helpers)) vm.unsafe_call_bl(function);
        }
      else if(!vm.unsafe_call_external(function))
        throw std::runtime_error("NPC definitions: external is not registered: "+function->name());
      }
    }
  return true;
  }
