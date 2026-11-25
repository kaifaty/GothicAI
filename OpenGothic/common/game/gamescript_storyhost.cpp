#include "gamescript_programs.h"

#include <Tempest/Log>

#include "game/definitions/basescripts.h"
#include "game/definitions/nativebasecatalog.h"
#include "storygraph/vocab.h"
#include "world/objects/npc.h"

#include <algorithm>
#include <bit>
#include <map>
#include <type_traits>
#include <utility>

using namespace storygraph;
using daedalus::DaedalusDataType;
using daedalus::DaedalusInstance;
using daedalus::DaedalusSymbol;

namespace {

#include "storyrecordfields.h"

template<auto Member, class Record>
decltype(auto) recordElement(Record& record, size_t index) {
  auto& field = record.*Member;
  if constexpr(std::is_array_v<std::remove_reference_t<decltype(field)>>)
    return (field[index]);
  else
    return (field);
  }

template<class Record>
struct RecordField {
  size_t size;
  NativeValue::Type type;
  NativeValue (*read)(const Record&,size_t);
  void (*write)(Record&,size_t,const NativeValue&);
  };

template<class Record, auto Member>
RecordField<Record> recordFieldDescriptor() {
  using Field = std::remove_reference_t<decltype(std::declval<Record&>().*Member)>;
  using Value = std::remove_extent_t<Field>;
  constexpr size_t size = std::is_array_v<Field> ? std::extent_v<Field> : 1;
  constexpr bool text = std::is_same_v<Value,std::string>;
  constexpr bool real = std::is_same_v<Value,float>;
  static_assert(text || real || std::is_same_v<Value,int32_t> || std::is_enum_v<Value>);
  return {size, text ? NativeValue::Str : real ? NativeValue::Float : NativeValue::Int,
          [](const Record& record, size_t index) {
            const auto& value = recordElement<Member>(record,index);
            if constexpr(text)
              return NativeValue::str(value);
            else if constexpr(real)
              return NativeValue::real(value);
            else if constexpr(std::is_enum_v<Value>)
              return NativeValue::integer(std::bit_cast<int32_t>(static_cast<std::underlying_type_t<Value>>(value)));
            else
              return NativeValue::integer(value);
            },
          [](Record& record, size_t index, const NativeValue& value) {
            auto& field = recordElement<Member>(record,index);
            if constexpr(text)
              field = value.s;
            else if constexpr(real)
              field = value.type==NativeValue::Float ? value.f : std::bit_cast<float>(value.i);
            else if constexpr(std::is_enum_v<Value>)
              field = static_cast<Value>(std::bit_cast<std::underlying_type_t<Value>>(value.i));
            else
              field = value.i;
            }};
  }

template<class Record>
const RecordField<Record>* recordField(std::string_view name) {
  static const auto fields = [] {
    std::map<std::string_view,RecordField<Record>,std::less<>> result;
    recordFields(static_cast<Record*>(nullptr),[&]<auto Member>(std::string_view name) {
      result.emplace(name,recordFieldDescriptor<Record,Member>());
      });
    return result;
    }();
  // Returned member references carry the full binding name.
  constexpr auto prefix = recordPrefix(static_cast<Record*>(nullptr));
  if(name.starts_with(prefix))
    name.remove_prefix(prefix.size());
  const auto found = fields.find(name);
  return found!=fields.end() ? &found->second : nullptr;
  }

template<class Record>
std::optional<NativeValue> readRecordField(const Record& record, const NativeSlot& slot, std::string& error) {
  if(const auto* field = recordField<Record>(slot.name); field!=nullptr && slot.index<field->size)
    return field->read(record,slot.index);
  error = "Invalid member field or index: "+std::string(slot.name);
  return std::nullopt;
  }

template<class Record>
bool writeRecordField(Record& record, const NativeSlot& slot, const NativeValue& value, std::string& error) {
  if(const auto* field = recordField<Record>(slot.name); field!=nullptr && slot.index<field->size) {
    if(value.type==field->type || (field->type==NativeValue::Float && value.type==NativeValue::Int)) {
      field->write(record,slot.index,value);
      return true;
      }
    }
  error = "Invalid member field, index or value: "+std::string(slot.name);
  return false;
  }

template<class... Records>
struct RecordFields {
  static bool contains(std::string_view name, uint16_t index) {
    const auto valid = [index](const auto* field) { return field!=nullptr && index<field->size; };
    return (valid(recordField<Records>(name)) || ...);
    }

  static NativeValue readNull(std::string_view name) {
    auto type = NativeValue::Int;
    const auto first = [&](const auto* field) {
      if(field==nullptr)
        return false;
      type = field->type;
      return true;
      };
    (void)(first(recordField<Records>(name)) || ...);
    return type==NativeValue::Str ? NativeValue::str("") : NativeValue::integer(0);
    }

  template<class Visitor>
  static bool visit(DaedalusInstance* context, Visitor visit) {
    const auto apply = [&]<class Record>() {
      if(auto* record = dynamic_cast<Record*>(context)) {
        visit(*record);
        return true;
        }
      return false;
      };
    return (apply.template operator()<Records>() || ...);
    }
  };

using StoryRecordFields = RecordFields<NpcRecord,ItemRecord,daedalus::IInfo,FocusRecord,MenuDefinitions::MenuItem>;

}

void GameScript::applyNpcConstructorField(NpcRecord& npc, const NpcDefinitionRuntime::Field& field) {
  using Kind = NpcDefinitions::Value::Kind;
  const auto& input = field.value;
  NativeValue value;
  switch(input.kind) {
    case Kind::Int:
    case Kind::Symbol: value = NativeValue::integer(input.integer); break;
    case Kind::Float:  value = NativeValue::real(input.real); break;
    case Kind::String: value = NativeValue::str(input.text); break;
    case Kind::Null:   value = NativeValue::integer(0); break;
    case Kind::Self:   throw std::runtime_error("Invalid NPC constructor instance field: "+field.name);
    }
  std::string error;
  if(!writeRecordField(npc,NativeSlot{NativeSlot::Member,field.name,field.index},value,error))
    throw std::runtime_error("NPC constructor: "+error);
  }

class GameScript::StockPrograms::Host final : public NativeExecutionHost {
  public:
    explicit Host(StockPrograms& owner, bool effects) : owner(owner), effects(effects) {}
    ~Host() override {
      if(selfAlias!=nullptr) {
        *selfAlias = std::move(savedSelf);
        *otherAlias = std::move(savedOther);
        }
      }

    void bindContext(Npc* self, Npc* other) {
      auto& selfSlot = owner.script.storyActorSlot("SELF");
      auto& otherSlot = owner.script.storyActorSlot("OTHER");
      selfAlias = &selfSlot;
      otherAlias = &otherSlot;
      savedSelf = std::exchange(selfSlot,actor(self));
      savedOther = std::exchange(otherSlot,actor(other));
      }

    static std::shared_ptr<DaedalusInstance> actor(Npc* npc) {
      if(npc==nullptr)
        return {};
      return std::static_pointer_cast<daedalus::INpc>(npc->handlePtr());
      }

    bool hasVariable(std::string_view name, bool instanceOnly) const override {
      if(const auto* catalog = baseNativeCatalog()) {
        if(const auto* state = owner.script.runtimeState.find(name))
          return !instanceOnly || std::holds_alternative<ScriptVars::Object>(*state);
        if(NativeBaseCatalog::findConstant(*catalog,name)!=nullptr)
          return !instanceOnly;
        }
      const auto* symbol = owner.binding(name).first;
      if(symbol==nullptr || symbol->is_member() || symbol->is_external())
        return false;
      using enum DaedalusDataType;
      const auto type = symbol->type();
      if(type==INSTANCE)
        return true;
      return !instanceOnly && (type==INT || type==FUNCTION || type==FLOAT || type==STRING);
      }

    bool hasMember(std::string_view name, uint16_t index) const override {
      return StoryRecordFields::contains(name,index);
      }

    bool acceptsCall(std::string_view name, bool external) const override {
      if(baseNativeCatalog()!=nullptr) {
        const auto key = nativeOperationName(name);
        return external ? owner.script.nativeExternals.contains(key) : owner.program(key)!=nullptr;
        }
      const auto* symbol = owner.binding(name).first;
      return symbol!=nullptr && symbol->is_external()==external;
      }

    std::optional<int32_t> symbolIndex(std::string_view name) const override {
      if(const auto* catalog = baseNativeCatalog()) {
        if(const auto id = owner.script.runtimeState.id(name))
          return int32_t(*id);
        if(const auto* constant = NativeBaseCatalog::findConstant(*catalog,name))
          return int32_t(constant->id);
        if(owner.prepared) {
          const auto found = owner.prepared->symbols.find(nativeOperationName(name));
          if(found!=owner.prepared->symbols.end())
            return int32_t(found->second);
          }
        }
      const auto* slot = owner.binding(name).first;
      if(slot==nullptr)
        return std::nullopt;
      return int32_t(slot->index());
      }

    std::string instanceName(NativeInstance id) const override {
      if(auto value = resolveInstance(id))
        return std::string(owner.script.storySymbolName(value->symbol_index()));
      return {};
      }

    std::optional<NativeValue> read(const NativeSlot& slot, std::string& error) override {
      using enum DaedalusDataType;
      if(slot.kind==NativeSlot::Member) {
        const auto context = resolveInstance(slot.context);
        if(context==nullptr)
          return StoryRecordFields::readNull(slot.name);
        std::optional<NativeValue> result;
        if(!StoryRecordFields::visit(context.get(),[&](const auto& record) { result=readRecordField(record,slot,error); }))
          error = "Invalid member context: "+std::string(slot.name);
        return result;
        }
      try {
        if(const auto* catalog = baseNativeCatalog()) {
          if(const auto* state = owner.script.runtimeState.find(slot.name)) {
            if(slot.kind==NativeSlot::Instance && !std::holds_alternative<ScriptVars::Object>(*state))
              throw std::runtime_error("Expected a runtime actor or item reference");
            return readState(*state,slot.index);
            }
          if(const auto* constant = NativeBaseCatalog::findConstant(*catalog,slot.name)) {
            if(slot.kind==NativeSlot::Instance)
              throw std::runtime_error("Expected a runtime actor or item reference");
            return std::visit([&](const auto& field) -> NativeValue {
              using T = std::remove_cvref_t<decltype(field)>;
              if constexpr(std::is_same_v<T,NativeBaseCatalog::Alias>)
                throw std::runtime_error("Invalid constant type");
              else
                return readValues(std::span(field),slot.index);
              },constant->values);
            }
          }
        auto* symbol = resolveValue(slot.name);
        if(symbol==nullptr || symbol->is_member() || (slot.kind==NativeSlot::Instance && symbol->type()!=INSTANCE)) {
          error = "Unknown story slot "+std::string(slot.name);
          return std::nullopt;
          }
        if(const auto* state = owner.script.runtimeState.find(symbol->index()))
          return readState(*state,slot.index);
        switch(symbol->type()) {
          case INT:
          case FUNCTION: return NativeValue::integer(symbol->get_int(slot.index));
          case FLOAT:    return NativeValue::real(symbol->get_float(slot.index));
          case STRING:   return NativeValue::str(symbol->get_string(slot.index));
          case INSTANCE: return NativeValue::instance(bind(symbol->get_instance()));
          default:       break;
          }
        }
      catch(const std::exception& e) {
        error = e.what();
        return std::nullopt;
        }
      error = "Unsupported value type for "+std::string(slot.name);
      return std::nullopt;
      }

    bool write(const NativeSlot& slot, const NativeValue& value, std::string& error) override {
      using enum DaedalusDataType;
      if(!effects) {
        error = "Cannot write story slot "+std::string(slot.name);
        return false;
        }
      if(slot.kind==NativeSlot::Member) {
        const auto context = resolveInstance(slot.context);
        bool written = false;
        if(!StoryRecordFields::visit(context.get(),[&](auto& record) { written=writeRecordField(record,slot,value,error); }))
          error = "Invalid member context: "+std::string(slot.name);
        return written;
        }
      if(const auto* catalog = baseNativeCatalog()) {
        if(auto* state = owner.script.runtimeState.find(slot.name)) {
          if(writeState(*state,slot,value))
            return true;
          error = "Invalid value for runtime field "+std::string(slot.name);
          return false;
          }
        if(NativeBaseCatalog::findConstant(*catalog,slot.name)!=nullptr) {
          error = "Cannot write story constant "+std::string(slot.name);
          return false;
          }
        }
      auto* symbol = resolveValue(slot.name);
      if(symbol==nullptr || symbol->is_member() || (symbol->is_const() && slot.kind!=NativeSlot::Instance)) {
        error = "Cannot write story slot "+std::string(slot.name);
        return false;
        }
      if(auto* state = owner.script.runtimeState.find(symbol->index())) {
        if(writeState(*state,slot,value))
          return true;
        error = "Invalid value for runtime field "+std::string(slot.name);
        return false;
        }
      const auto type = symbol->type();
      if((type==INT || type==FUNCTION) && value.type==NativeValue::Int)
        symbol->set_int(value.i,slot.index);
      else if(type==FLOAT && (value.type==NativeValue::Int || value.type==NativeValue::Float))
        symbol->set_float(value.type==NativeValue::Float ? value.f : std::bit_cast<float>(value.i),slot.index);
      else if(type==STRING && value.type==NativeValue::Str)
        symbol->set_string(value.s,slot.index);
      else if(type==INSTANCE && slot.kind==NativeSlot::Instance && value.type==NativeValue::Inst)
        symbol->set_instance(resolveInstance(value.inst));
      else {
        error = "Wrong value type for story slot "+std::string(slot.name);
        return false;
        }
      return true;
      }

    std::optional<NativeFunction> findFunction(std::string_view name) const override {
      if(!effects) {
        const auto key = nativeOperationName(name);
        if(!vocab::PredicateExternals.contains(key) && !vocab::PureExternals.contains(key))
          return std::nullopt;
        }
      return owner.function(name);
      }

    std::optional<NativeValue> invoke(std::string_view name, const std::vector<NativeValue>& args, std::string& error) override {
      auto call = invokeBound(name,args,error);
      if(!call)
        return std::nullopt;
      if(call->reference)
        return read(*call->reference,error);
      return std::move(call->value);
      }

    std::optional<NativeCall> invokeBound(std::string_view name, const std::vector<NativeValue>& args,
                                          std::string& error) override {
      const bool nativeCatalog = baseNativeCatalog()!=nullptr;
      auto* slot = nativeCatalog ? nullptr : owner.binding(name).first;
      const auto fn = findFunction(name);
      if((!nativeCatalog && slot==nullptr) || !fn || fn->program!=nullptr ||
         !std::equal(args.begin(),args.end(),fn->parameters.begin(),fn->parameters.end(),
                     [](const NativeValue& actual, NativeValue::Type expected) { return actual.type==expected; })) {
        error = "Unknown native story operation "+std::string(name);
        return std::nullopt;
        }
      const auto key = nativeCatalog ? nativeOperationName(name) : slot->name();
      // Journal writes have a direct engine API shared with other native game systems.
      if(effects && key.starts_with("LOG_") && args.size()==2 && args[0].type==NativeValue::Str) {
        if(key=="LOG_CREATETOPIC" && args[1].type==NativeValue::Int) {
          owner.script.log_createtopic(args[0].s,args[1].i);
          return NativeCall{};
          }
        if(key=="LOG_SETTOPICSTATUS" && args[1].type==NativeValue::Int) {
          owner.script.log_settopicstatus(args[0].s,args[1].i);
          return NativeCall{};
          }
        if(key=="LOG_ADDENTRY" && args[1].type==NativeValue::Str) {
          owner.script.log_addentry(args[0].s,args[1].s);
          return NativeCall{};
          }
        }
      if(const auto direct = owner.script.nativeExternals.find(key); direct!=owner.script.nativeExternals.end()) {
        const auto resolve = [this](NativeInstance id) { return resolveInstance(id); };
        const auto bindValue = [this](const auto& value) { return bind(value); };
        try {
          if(nativeCatalog)
            return direct->second.call(args,resolve,bindValue);
          struct RestoreContext {
            daedalus::DaedalusVm& vm;
            std::shared_ptr<DaedalusInstance> previous;
            ~RestoreContext() { vm.unsafe_set_gi(std::move(previous)); }
            } restore{owner.vm,owner.vm.unsafe_get_gi()};
          if(direct->second.observe)
            BoundNativeCall::observeExternal(owner.vm,*slot,args,resolve);
          return direct->second.call(args,resolve,bindValue);
          }
        catch(const std::exception& ex) {
          error = "Native function " + key + " failed: " + ex.what();
          return std::nullopt;
          }
        }
      if(slot==nullptr) {
        error = "Missing native story operation "+key;
        return std::nullopt;
        }
      const auto call = owner.calls.find(key);
      const auto& engineCall = call!=owner.calls.end() ? call->second : BoundNativeCall(owner.vm,*slot);
      return engineCall.invoke(args,
        [this](NativeInstance id) { return resolveInstance(id); },
        [this](const auto& value) { return bind(value); },
        [this](const auto& symbol,uint16_t index,const auto& context) { return reference(symbol,index,context); },
        error);
      }

    NativeInterpreter::Result run(std::string_view name, const std::vector<NativeValue>& args) {
      const auto fn = findFunction(name);
      if(!fn || fn->parameters.size()!=args.size())
        return {false,"Unavailable story function or wrong signature: "+std::string(name),{},false};

      std::vector<ExprP> parameters;
      parameters.reserve(args.size());
      for(size_t i=0; i<args.size(); ++i)
        parameters.push_back(ex::param(int32_t(i)));
      Program program;
      program.returns = fn->result.has_value();
      auto& step = program.steps.emplace_back();
      auto call = acceptsCall(name,true) ? ex::call(std::string(name),std::move(parameters)) :
                                           ex::fn(std::string(name),std::move(parameters));
      if(program.returns) {
        step.kind = Step::Return;
        step.value = std::move(call);
        } else {
        step.kind = Step::Call;
        step.call = std::move(call);
        }
      return NativeInterpreter(*this).run(program,args);
      }

    NativeInstance bind(const std::shared_ptr<DaedalusInstance>& value) {
      if(value==nullptr)
        return 0;
      const auto found = std::find(instances.begin(),instances.end(),value);
      if(found!=instances.end())
        return NativeInstance(found-instances.begin()+1);
      instances.push_back(value);
      return instances.size();
      }

  private:
    template<class T>
    static NativeValue readValues(std::span<T> values, uint16_t index) {
      if(index>=values.size())
        throw std::runtime_error("Runtime field index out of range");
      using Value = typename decltype(values)::value_type;
      if constexpr(std::is_same_v<Value,int32_t>)
        return NativeValue::integer(values[index]);
      else if constexpr(std::is_same_v<Value,float>)
        return NativeValue::real(values[index]);
      else
        return NativeValue::str(values[index]);
      }

    NativeValue readState(const ScriptVars::Slot& state, uint16_t index) {
      return std::visit([&](const auto& field) -> NativeValue {
        using T = std::remove_cvref_t<decltype(field)>;
        if constexpr(std::is_same_v<T,ScriptVars::Object>)
          return NativeValue::instance(bind(field));
        else
          return readValues(field.values,index);
        },state);
      }

    bool writeState(ScriptVars::Slot& state, const NativeSlot& slot, const NativeValue& value) {
      if(auto* object = std::get_if<ScriptVars::Object>(&state)) {
        if(slot.kind!=NativeSlot::Instance || value.type!=NativeValue::Inst)
          return false;
        *object = resolveInstance(value.inst);
        return true;
        }
      const auto store = [&]<class T>(const T& update) {
        if(auto* array = std::get_if<ScriptVars::Array<T>>(&state); array!=nullptr && slot.index<array->values.size()) {
          array->values[slot.index] = update;
          return true;
          }
        return false;
        };
      switch(value.type) {
        case NativeValue::Int:   return store(value.i) || store(std::bit_cast<float>(value.i));
        case NativeValue::Float: return store(value.f);
        case NativeValue::Str:   return store(value.s);
        case NativeValue::Inst:  break;
        }
      return false;
      }

    std::shared_ptr<DaedalusInstance> resolveInstance(NativeInstance index) const {
      if(index==0 || index>instances.size())
        return {};
      return instances[size_t(index)-1];
      }

    DaedalusSymbol* resolveValue(std::string_view name) const {
      if(const auto bound = returned.find(name); bound!=returned.end())
        return bound->second;
      return owner.binding(name).first;
      }

    std::optional<NativeSlot> reference(const DaedalusSymbol& symbol, uint16_t index,
                                        const std::shared_ptr<DaedalusInstance>& context) {
      // A registered engine function may return a reference instead of a value.
      // Keep its exact slot so that later operands can update it before it is read.
      const auto kind = symbol.is_member() ? NativeSlot::Member :
                        symbol.type()==DaedalusDataType::INSTANCE ? NativeSlot::Instance : NativeSlot::Variable;
      if(kind!=NativeSlot::Member)
        returned.emplace(symbol.name(),const_cast<DaedalusSymbol*>(&symbol));
      return NativeSlot{kind,symbol.name(),index,bind(context)};
      }

    StockPrograms& owner;
    bool effects;
    ScriptVars::Object* selfAlias = nullptr;
    ScriptVars::Object* otherAlias = nullptr;
    ScriptVars::Object savedSelf, savedOther;
    std::vector<std::shared_ptr<DaedalusInstance>> instances;
    std::unordered_map<std::string_view,DaedalusSymbol*> returned;
  };

std::unique_ptr<GameScript::StockPrograms::Host> GameScript::StockPrograms::host(bool effects) {
  return std::make_unique<Host>(*this,effects);
  }

bool GameScript::StockPrograms::run(uint32_t id, int32_t& result, bool* failed, bool preparedOnly) {
  const auto* entry = program(id);
  const bool nativeCatalog = baseNativeCatalog()!=nullptr;
  std::string_view name;
  if(nativeCatalog) {
    if(entry==nullptr || !entry->parameters.empty())
      return false;
    name = entry->name;
    } else {
    const auto found = ids.find(id);
    if(found==ids.end() || found->second->type()!=DaedalusDataType::FUNCTION || (preparedOnly && entry==nullptr))
      return false;
    name = found->second->name();
    }
  if(runPrepared(id,name,result,failed))
    return true;
  if(!nativeCatalog) {
    const auto fn = function(name);
    if(!fn || !fn->parameters.empty())
      return false;
    }

  Host execution(*this,true);
  const auto run = nativeCatalog || preparedOnly ? NativeInterpreter(execution).run(entry->program) :
                                                   execution.run(name,{});
  if(failed!=nullptr)
    *failed = !run.ok;
  if(!run.ok)
    Tempest::Log::e("[story] program ",name,": ",run.error);
  result = run.returned.type==NativeValue::Int ? run.returned.i : 0;
  return run.started;
  }

ScriptVars::Object& GameScript::storyActorSlot(std::string_view name) {
  ScriptVars::Slot* state = nullptr;
  if(baseNativeCatalog()!=nullptr)
    state = runtimeState.find(name);
  else
    if(const auto* binding = storySlot(name))
      state = runtimeState.find(binding->index());
  if(auto* object = std::get_if<ScriptVars::Object>(state))
    return *object;
  throw std::runtime_error("Missing runtime story actor "+std::string(name));
  }

std::pair<Npc*,Npc*> GameScript::storyActors() {
  return {findNpc(dynamic_cast<daedalus::INpc*>(storyActorSlot("SELF").get())),
          findNpc(dynamic_cast<daedalus::INpc*>(storyActorSlot("OTHER").get()))};
  }

std::unique_ptr<NativeExecutionHost> GameScript::makeStoryHost(Npc* self, Npc* other, bool effects) {
  auto host = storyPrograms().host(effects);
  host->bindContext(self,other);
  return host;
  }

std::string GameScript::checkStoryProgram(const Program& program, size_t arguments, bool choices, bool effects) {
  auto host = storyPrograms().host(effects);
  NativeInterpreter interpreter(*host);
  if(choices)
    interpreter.choices = [](const Step&) { return true; };
  return interpreter.check(program,arguments);
  }

NativeInterpreter::Result GameScript::runStoryProgram(const Program& program, Npc* self, Npc* other, bool effects,
   std::function<std::string(const std::string&,const std::string&)> ownLine,
   std::function<bool(const Step&)> choices, const std::vector<NativeValue>& args) {
  auto host = makeStoryHost(self,other,effects);
  NativeInterpreter interpreter(*host);
  interpreter.ownLine = std::move(ownLine);
  interpreter.choices = std::move(choices);
  return interpreter.run(program,args);
  }

NativeInterpreter::Result GameScript::callStoryFunction(std::string_view name, Npc* self, Npc* other,
                                                        const std::vector<StoryArgument>& args) {
  auto host = storyPrograms().host(true);
  host->bindContext(self,other);
  std::vector<NativeValue> native;
  native.reserve(args.size());
  for(const auto& argument : args) {
    native.push_back(std::visit([&](const auto& value) -> NativeValue {
      using T = std::remove_cvref_t<decltype(value)>;
      if constexpr(std::is_same_v<T,Npc*>)
        return NativeValue::instance(host->bind(StockPrograms::Host::actor(value)));
      else if constexpr(std::is_same_v<T,int32_t>)
        return NativeValue::integer(value);
      else if constexpr(std::is_same_v<T,float>)
        return NativeValue::real(value);
      else
        return NativeValue::str(value);
      },argument));
    }
  return host->run(name,native);
  }
