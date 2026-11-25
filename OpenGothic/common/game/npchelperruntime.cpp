#include "npchelperruntime.h"

#include "npchelperdefinitions.h"

#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <type_traits>

namespace {
using namespace NpcHelperDefinitions;
using namespace daedalus;
using Type = DaedalusDataType;
using Kind = Value::Kind;

DaedalusSymbol& symbol(DaedalusVm& vm,std::string_view name) {
  auto* found=vm.find_symbol_by_name(std::string(name));
  if(found==nullptr) throw std::runtime_error("NPC helper definitions: missing symbol "+std::string(name));
  return *found;
  }

class RuntimeHost final : public Host {
  public:
    RuntimeHost(DaedalusVm& vm,const std::shared_ptr<INpc>& npc,const NpcHelperRuntime::Definitions& definitions,
                const NpcHelperRuntime::InvokeExternal* invoke=nullptr)
      :vm(vm),npc(npc),definitions(definitions),invoke(invoke) {}
    int32_t get(std::string_view binding,uint16_t index) override {
      vm.unsafe_set_gi(npc);
      return symbol(vm,binding).get_int(index,npc.get());
      }
    void set(std::string_view binding,uint16_t index,int32_t value) override {
      vm.unsafe_set_gi(npc);
      symbol(vm,binding).set_int(value,index,npc.get());
      }
    void external(std::string_view name,std::span<const Value> arguments) override {
      call(name,arguments,false);
      }
    int32_t random(int32_t bound) override {
      Value argument;
      argument.integer=bound;
      return call("HLP_RANDOM",std::span(&argument,1),true);
      }
    void local(std::string_view name,const Value& value) override {
      auto& target=symbol(vm,name);
      switch(value.kind) {
        case Kind::Self: target.set_instance(npc); break;
        case Kind::Null:
          if(target.type()==Type::INSTANCE) target.set_instance(nullptr);
          else target.set_int(0);
          break;
        case Kind::String: target.set_string(value.text); break;
        case Kind::Float: target.set_float(value.real); break;
        case Kind::Symbol: target.set_int(int32_t(symbol(vm,value.text).index())); break;
        case Kind::Int: target.set_int(value.integer); break;
        }
      }

  private:
    int32_t call(std::string_view name,std::span<const Value> arguments,bool returns) {
      const auto& function=symbol(vm,name);
      const auto parameters=vm.find_parameters_for_function(&function);
      if(!function.is_external() || function.has_return()!=returns ||
         (returns && function.rtype()!=Type::INT) || parameters.size()!=arguments.size())
        throw std::runtime_error("NPC helper definitions: invalid engine call "+std::string(name));
      const auto pushArguments=[&] {
        for(size_t i=0;i<arguments.size();++i)
          push(arguments[i],parameters[i].type());
        };
      if(invoke!=nullptr) {
        std::function<void()> observe;
        if(vm.has_external_observer())
          observe=[&] { vm.observe_bound_external(function,pushArguments); };
        struct RestoreContext {
          DaedalusVm& vm;
          std::shared_ptr<DaedalusInstance> previous;
          ~RestoreContext() { vm.unsafe_set_gi(std::move(previous)); }
          } restore{vm,vm.unsafe_get_gi()};
        return (*invoke)(function,arguments,observe);
        }
      pushArguments();
      if(!vm.unsafe_call_external(&function))
        throw std::runtime_error("NPC helper definitions: external is not registered: "+std::string(name));
      return returns ? vm.pop_int() : 0;
      }

    void push(const Value& value,Type type) {
      switch(value.kind) {
        case Kind::Self: vm.push_instance(npc); break;
        case Kind::Null:
          if(type==Type::INSTANCE) vm.push_instance(nullptr); else vm.push_int(0);
          break;
        case Kind::Int: vm.push_int(value.integer); break;
        case Kind::Float: vm.push_float(value.real); break;
        case Kind::Symbol: {
          auto& target=symbol(vm,value.text);
          if(type==Type::INSTANCE) vm.push_reference(&target); else vm.push_int(int32_t(target.index()));
          break;
          }
        case Kind::String: {
          const auto found=definitions.literals.find(value.text);
          if(found!=definitions.literals.end())
            vm.push_reference(vm.find_symbol_by_index(found->second.first),found->second.second);
          else if(!vm.has_legacy_dispatcher()) // Director appearances can use head names outside the program.
            vm.push_string(value.text);
          else
            throw std::runtime_error("NPC helper definitions: no immutable string literal: "+value.text);
          break;
          }
        }
      }
    DaedalusVm& vm;
    const std::shared_ptr<INpc>& npc;
    const NpcHelperRuntime::Definitions& definitions;
    const NpcHelperRuntime::InvokeExternal* invoke;
  };

size_t arguments(const Definition& definition) {
  return std::visit([](const auto& helper)->size_t {
    using T=std::decay_t<decltype(helper)>;
    if constexpr(std::is_same_v<T,SetFightSkills> || std::is_same_v<T,AttributesToChapter>) return 2;
    if constexpr(std::is_same_v<T,SetNpcVisual>) return 6;
    if constexpr(std::is_same_v<T,AmbientInventory>) return 1;
    if constexpr(std::is_same_v<T,AddFightSkill>) return 3;
    return 0;
    },definition);
  }
}

bool NpcHelperRuntime::enabled() {
  const auto* value=std::getenv("OPENGOTHIC_NPC_HELPER_RECORDS");
  return value==nullptr || std::strcmp(value,"0")!=0;
  }

NpcHelperRuntime::Definitions NpcHelperRuntime::resolve(const Library& library,const DaedalusScript& script) {
  Definitions definitions;
  for(const auto& entry:script.symbols())
    if(entry.type()==Type::STRING && entry.is_const() && !entry.is_member())
      for(uint32_t i=0;i<entry.count() && i<=UINT8_MAX;++i)
        definitions.literals.try_emplace(entry.get_string(uint16_t(i)),entry.index(),uint8_t(i));
  for(const auto& [name,definition]:library.helpers) {
    const auto* function=script.find_symbol_by_name(name);
    if(function==nullptr || function->type()!=Type::FUNCTION || function->is_external() ||
       function->has_return() || function->count()!=arguments(definition))
      throw std::runtime_error("NPC helper definitions: invalid helper signature "+name);
    definitions.helpers.emplace(function->index(),definition);
    }
  return definitions;
  }

NpcHelperRuntime::Definitions NpcHelperRuntime::load(const std::filesystem::path& base,
                                                    const DaedalusScript& script,std::string_view sha256) {
  // Keep the library while disabled too: contracts and a running process can toggle the fallback.
  return resolve(NpcHelperDefinitions::read(base,sha256),script);
  }

bool NpcHelperRuntime::invoke(DaedalusVm& vm,const std::shared_ptr<INpc>& npc,
                              const DaedalusSymbol* function,const Definitions& definitions) {
  if(!enabled() || function==nullptr) return false;
  const auto found=definitions.helpers.find(function->index());
  if(found==definitions.helpers.end()) return false;
  const auto parameters=vm.find_parameters_for_function(function);
  std::vector<Value> args(parameters.size());
  for(size_t i=parameters.size();i>0;--i) {
    auto& value=args[i-1];
    switch(parameters[i-1].type()) {
      case Type::INSTANCE: {
        const auto instance=vm.pop_instance();
        if(instance!=npc) throw std::runtime_error("NPC helper definitions: helper targets another NPC");
        value.kind=Kind::Self;
        break;
        }
      case Type::STRING: value.kind=Kind::String; value.text=vm.pop_string(); break;
      case Type::FLOAT: value.kind=Kind::Float; value.real=vm.pop_float(); break;
      case Type::INT:
      case Type::FUNCTION: value.integer=vm.pop_int(); break;
      default: throw std::runtime_error("NPC helper definitions: unsupported helper parameter");
      }
    }
  RuntimeHost host(vm,npc,definitions);
  run(found->second,args,host);
  return true;
  }

bool NpcHelperRuntime::invokePrepared(DaedalusVm& vm,const std::shared_ptr<INpc>& npc,
                                     uint32_t function,const Definitions& definitions,
                                     std::span<const Value> args,const InvokeExternal& invokeExternal) {
  if(!enabled())
    return false;
  const auto found=definitions.helpers.find(function);
  if(found==definitions.helpers.end())
    return false;
  if(npc==nullptr || args.size()!=arguments(found->second) || (!args.empty() && args.front().kind!=Kind::Self))
    throw std::runtime_error("NPC helper definitions: invalid prepared helper arguments");
  struct RestoreContext {
    DaedalusVm& vm;
    std::shared_ptr<DaedalusInstance> previous;
    ~RestoreContext() { vm.unsafe_set_gi(std::move(previous)); }
    } restore{vm,vm.unsafe_get_gi()};
  RuntimeHost host(vm,npc,definitions,invokeExternal ? &invokeExternal : nullptr);
  run(found->second,args,host);
  return true;
  }
