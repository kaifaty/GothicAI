#include "gamescript.h"

#include <bit>
#include <stdexcept>
#include <utility>
#include <vector>

#include "game/definitions/npcworlddata.h"
#include "resources.h"
#include "world/objects/npc.h"

namespace {
using HelperValue = NpcHelperDefinitions::Value;
using HelperKind = HelperValue::Kind;

class PreparedNpcHelperHost final : public NpcHelperDefinitions::Host {
  public:
    using Call = std::function<int32_t(std::string_view,std::span<const HelperValue>,bool)>;
    using ResolveId = std::function<int32_t(std::string_view)>;

    PreparedNpcHelperHost(const std::shared_ptr<daedalus::INpc>& actor, ScriptVars::State& state,
                          Call call, ResolveId resolveId)
      : actor(actor), state(state), call(std::move(call)), resolveId(std::move(resolveId)) {}

    int32_t get(std::string_view binding,uint16_t index) override {
      return field(binding,index);
      }

    void set(std::string_view binding,uint16_t index,int32_t value) override {
      field(binding,index)=value;
      }

    void external(std::string_view name,std::span<const HelperValue> arguments) override {
      call(name,arguments,false);
      }

    int32_t random(int32_t bound) override {
      const HelperValue argument{.integer=bound};
      return call("HLP_RANDOM",std::span(&argument,1),true);
      }

    void local(std::string_view name,const HelperValue& value) override {
      auto* slot=state.find(name);
      switch(value.kind) {
        case HelperKind::Self:
          if(auto* target=std::get_if<ScriptVars::Object>(slot)) {
            *target=actor;
            return;
            }
          break;
        case HelperKind::Null:
          if(auto* target=std::get_if<ScriptVars::Object>(slot)) {
            target->reset();
            return;
            }
          if(write(slot,int32_t(0)))
            return;
          break;
        case HelperKind::Int:
        case HelperKind::Symbol:
          if(write(slot,value.kind==HelperKind::Int ? value.integer : resolveId(value.text)))
            return;
          break;
        case HelperKind::Float:
          if(write(slot,value.real))
            return;
          break;
        case HelperKind::String:
          if(write(slot,value.text))
            return;
          break;
        }
      throw std::runtime_error("NPC helper definitions: invalid saved field "+std::string(name));
      }

  private:
    template<class T>
    static bool write(ScriptVars::Slot* slot,const T& value) {
      if(auto* values=std::get_if<ScriptVars::Array<T>>(slot); values!=nullptr && values->values.size()==1) {
        values->values.front()=value;
        return true;
        }
      return false;
      }

    int32_t& field(std::string_view binding,uint16_t index) {
      NpcRecord& record=*actor;
      std::span<int32_t> fields;
      if(binding=="C_NPC.ATTRIBUTE") fields=record.attribute;
      else if(binding=="C_NPC.HITCHANCE") fields=record.hitchance;
      else if(binding=="C_NPC.AIVAR") fields=record.aivar;
      else if(binding=="C_NPC.GUILD") fields={&record.guild,1};
      else if(binding=="C_NPC.LEVEL") fields={&record.level,1};
      else if(binding=="C_NPC.EXP") fields={&record.exp,1};
      else if(binding=="C_NPC.EXP_NEXT") fields={&record.exp_next,1};
      if(index>=fields.size())
        throw std::runtime_error("NPC helper definitions: invalid member "+std::string(binding));
      return fields[index];
      }

    const std::shared_ptr<daedalus::INpc>& actor;
    ScriptVars::State& state;
    Call call;
    ResolveId resolveId;
  };
}

int32_t GameScript::callNpcHelperOperation(const std::shared_ptr<daedalus::INpc>& npc,std::string_view name,
                                           std::span<const HelperValue> arguments,bool returns,
                                           const std::function<void()>& observe) {
  using storygraph::NativeValue;
  constexpr storygraph::NativeInstance self=1;
  const auto external=nativeExternals.find(name);
  std::array<NativeValue,8> values;
  if(external==nativeExternals.end() || arguments.size()>values.size() ||
     external->second.signature.parameters.size()!=arguments.size() ||
     external->second.signature.result!=(returns ? std::optional(NativeValue::Int) : std::nullopt))
    throw std::runtime_error("NPC helper definitions: unavailable native operation "+std::string(name));
  for(size_t index=0; index<arguments.size(); ++index) {
    const auto& argument=arguments[index];
    switch(argument.kind) {
      case HelperKind::Int: values[index]=NativeValue::integer(argument.integer); break;
      case HelperKind::Float: values[index]=NativeValue::real(argument.real); break;
      case HelperKind::String: values[index]=NativeValue::str(argument.text); break;
      case HelperKind::Self: values[index]=NativeValue::instance(self); break;
      case HelperKind::Null:
        values[index]=external->second.signature.parameters[index]==NativeValue::Inst ?
                      NativeValue::instance(0) : NativeValue::integer(0);
        break;
      case HelperKind::Symbol: {
        const auto id=findSymbolIndex(argument.text);
        if(id==size_t(-1))
          throw std::runtime_error("NPC helper definitions: unknown operation argument "+argument.text);
        values[index]=NativeValue::integer(int32_t(id));
        break;
        }
      }
    if(values[index].type!=external->second.signature.parameters[index])
      throw std::runtime_error("NPC helper definitions: wrong native argument for "+std::string(name));
    }
  if(observe)
    observe();
  const auto resolve=[&npc](storygraph::NativeInstance id) -> storygraph::BoundNativeCall::Instance {
    return id==self ? npc : nullptr;
    };
  return external->second.call(std::span(values).first(arguments.size()),resolve,{}).value.i;
  }

std::shared_ptr<daedalus::INpc> GameScript::nativeNpc(std::string_view name) {
  return std::dynamic_pointer_cast<daedalus::INpc>(storyActorSlot(name));
  }

int32_t GameScript::npcRuleInt(std::string_view name,uint16_t index) {
  if(index==0)
    if(const auto value = globalIntSymbol(name))
      return *value;
  if(const auto* values = std::get_if<ScriptVars::Array<int32_t>>(runtimeState.find(name)))
    if(index<values->values.size())
      return values->values[index];
  if(const auto* constant = NativeBaseCatalog::findConstant(*baseNativeCatalog(),name))
    if(const auto* values = std::get_if<std::vector<int32_t>>(&constant->values); values && index<values->size())
      return (*values)[index];
  throw std::runtime_error("missing NPC rule value: "+std::string(name));
  }

void GameScript::setNpcRuleInt(std::string_view name,int32_t value,uint16_t index) {
  if(auto* field = std::get_if<ScriptVars::Array<int32_t>>(runtimeState.find(name)); field && index<field->values.size()) {
    field->values[index] = value;
    return;
    }
  throw std::runtime_error("missing writable NPC rule value: "+std::string(name));
  }

std::string_view GameScript::npcRuleString(std::string_view name) const {
  if(const auto* values = std::get_if<ScriptVars::Array<std::string>>(runtimeState.find(name)))
    return values->values.front();
  if(const auto* constant = NativeBaseCatalog::findConstant(*baseNativeCatalog(),name))
    if(const auto* values = std::get_if<std::vector<std::string>>(&constant->values))
      return values->front();
  throw std::runtime_error("missing NPC rule string: "+std::string(name));
  }

void GameScript::setNpcRuleString(std::string_view name,std::string_view value) {
  if(auto* field = std::get_if<ScriptVars::Array<std::string>>(runtimeState.find(name))) {
    field->values.front() = value;
    return;
    }
  throw std::runtime_error("missing writable NPC rule string: "+std::string(name));
  }

bool GameScript::invokeNpcHelper(const std::shared_ptr<daedalus::INpc>& npc,uint32_t function,
                                 std::span<const HelperValue> args) {
  if(baseNativeCatalog()!=nullptr) {
    if(npc==nullptr)
      throw std::runtime_error("NPC helper definitions: missing prepared actor");
    PreparedNpcHelperHost host(npc,runtimeState,[this,&npc](std::string_view name,std::span<const HelperValue> values,bool returns) {
      return callNpcHelperOperation(npc,name,values,returns);
      },[this](std::string_view name) {
      const auto id=findSymbolIndex(name);
      if(id==size_t(-1))
        throw std::runtime_error("NPC helper definitions: unknown prepared identity "+std::string(name));
      return int32_t(id);
      });
    return NpcHelperRuntime::invokePrepared(function,npcDefinitions.helpers,args,host);
    }
  if(vm.has_legacy_dispatcher())
    return NpcHelperRuntime::invokePrepared(vm,npc,function,npcDefinitions.helpers,args);
  return NpcHelperRuntime::invokePrepared(vm,npc,function,npcDefinitions.helpers,args,
    [this,&npc](const daedalus::DaedalusSymbol& operation,std::span<const HelperValue> arguments,
                const std::function<void()>& observe) {
      return callNpcHelperOperation(npc,operation.name(),arguments,operation.has_return(),observe);
      });
  }

bool GameScript::applyNpcConstructorCall(const std::shared_ptr<daedalus::INpc>& npc,
                                         const NpcDefinitionRuntime::Call& call,const std::function<void()>& observe) {
  using Type = daedalus::DaedalusDataType;
  using storygraph::NativeValue;
  if(call.deferred) {
    if(!npcDefinitions.helpers.helpers.contains(call.function))
      return false;
    std::array<HelperValue,6> arguments;
    if(call.args.size()>arguments.size())
      throw std::runtime_error("NPC helper definitions: too many recorded helper arguments");
    for(size_t index=0; index<call.args.size(); ++index) {
      const auto& argument=call.args[index];
      auto& value=arguments[index];
      value=argument.value;
      switch(argument.type) {
        case Type::STRING: break;
        case Type::INT:
        case Type::FUNCTION: value.kind=HelperKind::Int; break;
        case Type::FLOAT: break;
        case Type::INSTANCE:
          if(value.kind!=HelperKind::Self)
            throw std::runtime_error("NPC helper definitions: helper targets another NPC");
          break;
        default: throw std::runtime_error("NPC helper definitions: unsupported prepared argument");
        }
      }
    return invokeNpcHelper(npc,call.function,std::span(arguments).first(call.args.size()));
    }

  const auto external=nativeExternals.find(call.name);
  // The stock constructor corpus calls externals on SELF, with up to eight arguments.
  std::array<NativeValue,8> values;
  if(external==nativeExternals.end() ||
     call.args.empty() || call.args.front().value.kind!=HelperKind::Self || call.args.size()>values.size())
    return false;
  const auto& native=external->second;
  if(native.signature.result || native.signature.parameters.size()!=call.args.size())
    return false;
  constexpr storygraph::NativeInstance self=1;
  for(size_t index=0; index<call.args.size(); ++index) {
    const auto& argument=call.args[index];
    const auto& value=argument.value;
    switch(argument.type) {
      case Type::FLOAT: values[index]=NativeValue::real(value.real); break;
      case Type::STRING: values[index]=NativeValue::str(value.text); break;
      case Type::INT:
      case Type::FUNCTION: values[index]=NativeValue::integer(value.integer); break;
      case Type::INSTANCE:
        if(value.kind!=HelperKind::Self)
          return false;
        values[index]=NativeValue::instance(self);
        break;
      default:
        return false;
      }
    if(values[index].type!=native.signature.parameters[index])
      return false;
    }
  if(observe)
    observe();
  const auto resolve=[&npc](storygraph::NativeInstance id) -> storygraph::BoundNativeCall::Instance {
    return id==self ? npc : nullptr;
    };
  native.call(std::span(values).first(call.args.size()),resolve,{});
  return true;
  }

void GameScript::initializeNpcDefinition(const std::shared_ptr<daedalus::INpc>& npc,uint32_t templateId) {
  const auto definition=npcDefinitions.npcs.find(templateId);
  if(npc==nullptr || definition==npcDefinitions.npcs.end())
    throw std::runtime_error("Missing prepared NPC constructor for "+std::to_string(templateId));
  auto& self=storyActorSlot("SELF");
  struct RestoreSelf {
    ScriptVars::Object& slot;
    ScriptVars::Object previous;
    ~RestoreSelf() { slot=std::move(previous); }
    } scope{self,std::exchange(self,npc)};
  for(const auto& step:definition->second) {
    if(const auto* field=std::get_if<NpcDefinitionRuntime::Field>(&step)) {
      applyNpcConstructorField(*npc,*field);
      }
    else if(const auto* write=std::get_if<NpcDefinitionRuntime::GlobalIntWrite>(&step)) {
      auto* target=std::get_if<ScriptVars::Array<int32_t>>(runtimeState.find(write->binding));
      if(target==nullptr || write->index>=target->values.size())
        throw std::runtime_error("Missing prepared NPC constructor field "+std::to_string(write->binding));
      target->values[write->index]=write->value;
      }
    else {
      const auto& call=std::get<NpcDefinitionRuntime::Call>(step);
      if(!applyNpcConstructorCall(npc,call))
        throw std::runtime_error("NPC constructor "+std::to_string(templateId)+": unavailable prepared call "+call.name);
      }
    }
  }

void GameScript::initializeNpcDefinition(const std::shared_ptr<daedalus::INpc>& npc,
                                         daedalus::DaedalusSymbol* symbol, bool useRecords) {
  if(npc->user_ptr)
    static_cast<Npc*>(npc->user_ptr)->npcInstanceSymbol=symbol->index();
  // A director NPC keeps its own symbol while using its template's definition.
  auto* like=own.likeOf(vm,symbol->index());
  const auto applyCall=[this,&npc](const daedalus::DaedalusSymbol&,const NpcDefinitionRuntime::Call& call,
                                   const std::function<void()>& observe) {
    return !vm.has_legacy_dispatcher() && applyNpcConstructorCall(npc,call,observe);
    };
  const NpcDefinitionRuntime::Definitions empty;
  const auto applyField=vm.has_legacy_dispatcher() ? nullptr : &GameScript::applyNpcConstructorField;
  NpcDefinitionRuntime::initialize(vm,npc,symbol,useRecords ? npcDefinitions : empty,like,applyCall,applyField);
  }

void GameScript::refreshNpcAtInsert(Npc& npc) {
  auto& record = npc.handle();
  if(record.attribute[ATR_HITPOINTS]<=0)
    return;

  // Retain the saved local and compare instance identities, as the stock script
  // does even when the currently controlled character is not PC_HERO.
  const auto hero = hlp_getnpc(int(findSymbolIndex("PC_HERO")));
  findSymbol("B_REFRESHATINSERT.HER")->set_instance(hero);
  const auto self = std::static_pointer_cast<daedalus::INpc>(npc.handlePtr());
  if(hlp_getinstanceid(self)==hlp_getinstanceid(hero) || record.guild>=int32_t(GIL_SEPERATOR_HUM))
    return;

  record.attribute[ATR_HITPOINTS] = record.attribute[ATR_HITPOINTSMAX];
  if(npc_hasequippedweapon(self))
    return;

  switch(record.guild) {
    case GIL_MIL:
    case GIL_PAL:
    case GIL_SLD:
    case GIL_BAU:
    case GIL_NOV:
      createinvitems(self,int(findSymbolIndex("ITMW_1H_BAU_MACE")),1);
      break;
    }
  }

void GameScript::initPerceptions() {
  // New sessions can call this before a world is attached.
  if(!nativeRoutineProgram || !NpcDefinitionRuntime::enabled()) {
    if(hasSymbolName("initPerceptions"))
      vm.call_function("initPerceptions");
    return;
    }

  perc_setrange(PERC_ASSESSDAMAGE,       9999);
  perc_setrange(PERC_ASSESSOTHERSDAMAGE, 1000);
  perc_setrange(PERC_ASSESSDEFEAT,       2000);
  perc_setrange(PERC_ASSESSMURDER,       2000);
  perc_setrange(PERC_ASSESSTHREAT,       1000);
  perc_setrange(PERC_DRAWWEAPON,          500);
  perc_setrange(PERC_ASSESSFIGHTSOUND,   3000);
  perc_setrange(PERC_ASSESSQUIETSOUND,   1000);
  perc_setrange(PERC_ASSESSTHEFT,        1000);
  perc_setrange(PERC_ASSESSUSEMOB,       1000);
  perc_setrange(PERC_ASSESSENTERROOM,    1000);
  perc_setrange(PERC_ASSESSMAGIC,        9999);
  perc_setrange(PERC_ASSESSSTOPMAGIC,    9999);
  perc_setrange(PERC_ASSESSTALK,          500);
  perc_setrange(PERC_ASSESSWARN,         1000);
  perc_setrange(PERC_MOVEMOB,             500);
  perc_setrange(PERC_ASSESSSURPRISE,     3500);
  perc_setrange(PERC_OBSERVEINTRUDER,     100);
  perc_setrange(PERC_ASSESSREMOVEWEAPON,  100);
  perc_setrange(PERC_CATCHTHIEF,          100);
  perc_setrange(PERC_ASSESSCALL,          100);
  perc_setrange(PERC_MOVENPC,             100);
  perc_setrange(PERC_ASSESSCASTER,        100);
  perc_setrange(PERC_NPCCOMMAND,          100);
  perc_setrange(PERC_OBSERVESUSPECT,      100);
  }

void GameScript::loadNativeNpcDeath() {
  if(!nativeRoutineProgram)
    return;
  static constexpr std::pair<std::string_view,int32_t DeathProgress::*> state[]={
    {"CANYONRAZORBODYCOUNT",&DeathProgress::canyonRazors},
    {"MADKILLERCOUNT",&DeathProgress::madKills},
    {"FESTERS_GIANT_BUG_KILLED",&DeathProgress::festerBugs},
    {"ALLIGATORJACK_KROKOSKILLED",&DeathProgress::jackCrocodiles},
    {"MINECRAWLER_KILLED",&DeathProgress::minecrawlers},
    {"BLOODWYN_SPAWN",&DeathProgress::bloodwynSpawned},
    {"RAVENISDEAD",&DeathProgress::ravenDead},
    {"MIS_KILLEDDRAGONS",&DeathProgress::dragonsKilled},
    {"MIS_ALLDRAGONSDEAD",&DeathProgress::allDragonsDead},
    {"UNDEADDRAGONISDEAD",&DeathProgress::undeadDragonDead},
    {"DJG_BIFFSURVIVEDLASTDRAGON",&DeathProgress::biffSurvived},
    };
  for(const auto& [name,field]:state)
    bindRuntimeVariable(name,deathProgress.*field);
  const std::array flags={"SWAPDRAGNISDEAD","RCKDRAGNISDEAD","FREDRAGNISDEAD","ICDRAGNISDEAD"};
  for(size_t i=0;i<flags.size();++i)
    bindRuntimeVariable(flags[i],deathProgress.dragonDeaths[i]);
  using NpcRef = std::shared_ptr<daedalus::INpc>;
  vm.override_function("B_KILLNPC",std::function<void(int32_t)>{[this](int32_t symbol) { killNpc(symbol); }},
                       NpcDefinitionRuntime::enabled);
  vm.override_function("B_REMOVENPC",std::function<void(int32_t)>{[this](int32_t symbol) { removeNpc(symbol); }},
                       NpcDefinitionRuntime::enabled);
  vm.override_function("B_DRAGONKILLCOUNTER",std::function<int32_t(NpcRef)>{[this](NpcRef actor) {
    return countDragonDeaths(actor);
    }},[this] {
    return activityEvents && !floorDefer.active() && ActivityDefinitionRuntime::mode()!=ActivityDefinitionRuntime::Mode::Off;
    });
  }

void GameScript::invokeKillNpc(int32_t symbol) {
  if(!nativeRoutineProgram) {
    vm.call_function<void>("B_KILLNPC",symbol);
    return;
    }
  // Use the same call frame and gated dispatch as a script BL instruction.
  vm.push_int(symbol);
  vm.unsafe_call_bl(findSymbol("B_KILLNPC"));
  }

void GameScript::killNpc(int32_t symbol) {
  auto* parameter = findSymbol("B_KILLNPC.NPCINSTANCE");
  auto* local = findSymbol("B_KILLNPC.NPC");
  vm.set_int(vm.unsafe_get_gi(),parameter,0,symbol);

  // Use the engine bindings to keep the startup floor barrier for these NPC calls.
  vm.push_reference(parameter);
  vm.unsafe_call_external(findSymbol("HLP_GETNPC"));
  local->set_instance(vm.pop_instance());
  vm.push_reference(local);
  vm.unsafe_call_external(findSymbol("NPC_ISDEAD"));
  vm.push_int(vm.pop_int()==0);
  vm.push_reference(local);
  vm.unsafe_call_external(findSymbol("HLP_ISVALIDNPC"));
  const bool valid = vm.pop_int()!=0;
  const bool alive = vm.pop_int()!=0;
  if(!alive || !valid)
    return;

  // The dispatch frame restores the member context on return.
  const auto npc = local->get_instance();
  vm.unsafe_set_gi(npc);
  vm.set_int(npc,findSymbol("C_NPC.FLAGS"),0,0);
  vm.push_reference(local);
  vm.push_int(int32_t(findSymbolIndex("ITMI_OLDCOIN")));
  vm.unsafe_call_external(findSymbol("CREATEINVITEM"));

  // The stock damage reads the saved actor's maximum HP after the inventory call.
  vm.push_reference(local);
  vm.push_reference(findSymbol("ATR_HITPOINTS"));
  const auto target = local->get_instance();
  vm.unsafe_set_gi(target);
  const auto maximum = vm.get_int(target,findSymbol("C_NPC.ATTRIBUTE"),ATR_HITPOINTSMAX);
  vm.push_int(std::bit_cast<int32_t>(0u-uint32_t(maximum)));
  vm.unsafe_call_external(findSymbol("NPC_CHANGEATTRIBUTE"));
  }

void GameScript::removeNpc(int32_t symbol) {
  auto* parameter = findSymbol("B_REMOVENPC.NPCINSTANCE");
  auto* actor = findSymbol("B_REMOVENPC.NPC");
  parameter->set_int(symbol);
  vm.push_reference(parameter);
  vm.unsafe_call_external(findSymbol("HLP_GETNPC"));
  actor->set_instance(vm.pop_instance());

  vm.push_reference(actor);
  vm.unsafe_call_external(findSymbol("NPC_ISDEAD"));
  const bool dead = vm.pop_int()!=0;
  vm.push_reference(actor);
  vm.unsafe_call_external(findSymbol("HLP_ISVALIDNPC"));
  const bool valid = vm.pop_int()!=0;
  if(dead || !valid)
    return;

  const auto initial = std::static_pointer_cast<daedalus::INpc>(actor->get_instance());
  vm.unsafe_set_gi(initial);
  initial->flags = NpcFlag::NONE;
  vm.push_reference(actor);
  vm.push_reference(findSymbol("$13017")); // Stock TOT destination.
  vm.unsafe_call_external(findSymbol("AI_TELEPORT"));
  vm.push_reference(actor);
  vm.push_reference(findSymbol("$13018")); // Stock TOT routine.
  vm.unsafe_call_bl(findSymbol("B_STARTOTHERROUTINE"));

  const auto target = std::static_pointer_cast<daedalus::INpc>(actor->get_instance());
  vm.unsafe_set_gi(target);
  const auto maximumHp = target ? target->attribute[ATR_HITPOINTSMAX] : 0;
  vm.push_reference(actor);
  vm.push_reference(findSymbol("ATR_HITPOINTS"));
  vm.push_int(std::bit_cast<int32_t>(0u-uint32_t(maximumHp)));
  vm.unsafe_call_external(findSymbol("NPC_CHANGEATTRIBUTE"));
  vm.push_reference(actor);
  vm.push_reference(findSymbol("$13019"));
  vm.unsafe_call_external(findSymbol("AI_TELEPORT"));
  }

int32_t GameScript::countDragonDeaths(const std::shared_ptr<daedalus::INpc>& actor) {
  auto* current = findSymbol("B_DRAGONKILLCOUNTER.CURRENT_DRAGON");
  auto* raven = findSymbol("B_DRAGONKILLCOUNTER.RAVN");
  auto* yes = findSymbol("TRUE");
  auto* no = findSymbol("FALSE");
  const auto findNpc = [&](std::string_view name, daedalus::DaedalusSymbol* local) {
    vm.push_int(int32_t(findSymbolIndex(name)));
    vm.unsafe_call_external(findSymbol("HLP_GETNPC"));
    local->set_instance(vm.pop_instance());
    };
  const auto instanceId = [&](daedalus::DaedalusSymbol* npc) {
    vm.push_reference(npc);
    vm.unsafe_call_external(findSymbol("HLP_GETINSTANCEID"));
    return vm.pop_int();
    };
  const auto matchesCurrent = [&](daedalus::DaedalusSymbol* npc) {
    const auto id = instanceId(npc);
    return id==instanceId(current);
    };
  const auto memberNpc = [&](daedalus::DaedalusSymbol* symbol) {
    const auto npc = std::static_pointer_cast<daedalus::INpc>(symbol->get_instance());
    vm.unsafe_set_gi(npc);
    return npc;
    };
  bool moviePlayed = false;
  const auto play = [&](std::string_view movie) {
    vm.push_reference(findSymbol(movie));
    vm.push_reference(yes);
    vm.push_reference(no);
    vm.unsafe_call_external(findSymbol("PLAYVIDEOEX"));
    moviePlayed = true;
    };

  current->set_instance(actor);
  findNpc("BDT_1090_ADDON_RAVEN",raven);
  if(matchesCurrent(raven) && deathProgress.ravenDead==no->get_int()) {
    play("$17823"); // Raven.
    deathProgress.ravenDead=yes->get_int();
    vm.push_int(int32_t(findSymbolIndex("MYXIR_ADW")));
    vm.unsafe_call_bl(findSymbol("B_REMOVENPC"));
    }

  const auto dragon = memberNpc(current);
  if(dragon && dragon->guild==GIL_DRAGON) {
    constexpr std::pair<std::string_view,std::string_view> dragons[] = {
      {"DRAGON_SWAMP","B_DRAGONKILLCOUNTER.SWAPDRAGN"},
      {"DRAGON_ROCK", "B_DRAGONKILLCOUNTER.RCKDRAGN"},
      {"DRAGON_FIRE", "B_DRAGONKILLCOUNTER.FREDRAGN"},
      {"DRAGON_ICE",  "B_DRAGONKILLCOUNTER.ICDRAGN"},
      };
    for(const auto& [name,local]:dragons)
      findNpc(name,findSymbol(local));
    for(size_t i=0;i<std::size(dragons);++i) {
      auto& latch=deathProgress.dragonDeaths[i];
      const bool pending=latch==no->get_int();
      // The instance comparisons run even when the dragon is already counted.
      const bool same=matchesCurrent(findSymbol(dragons[i].second));
      if(pending && same) {
        deathProgress.dragonsKilled=std::bit_cast<int32_t>(uint32_t(deathProgress.dragonsKilled)+1u);
        latch=yes->get_int();
        }
      }
    }

  if(deathProgress.dragonsKilled==4) {
    deathProgress.allDragonsDead=yes->get_int();
    vm.push_reference(findSymbol("BIFF"));
    vm.unsafe_call_external(findSymbol("NPC_ISDEAD"));
    const bool biffAlive = vm.pop_int()==no->get_int();
    const bool biffInParty = findSymbol("DJG_BIFFPARTY")->get_int()==yes->get_int();
    if(biffAlive && biffInParty)
      deathProgress.biffSurvived=yes->get_int();
    }

  constexpr size_t monsterId = 43; // Stock AIVAR slot.
  const auto defeated = memberNpc(current);
  if(defeated && defeated->aivar[monsterId]==findSymbol("ID_DRAGON_UNDEAD")->get_int()) {
    deathProgress.undeadDragonDead=yes->get_int();
    auto* topic = findSymbol("TOPIC_BACKTOSHIP");
    vm.push_reference(topic);
    vm.push_reference(findSymbol("LOG_MISSION"));
    vm.unsafe_call_external(findSymbol("LOG_CREATETOPIC"));
    vm.push_reference(topic);
    vm.push_reference(findSymbol("LOG_RUNNING"));
    vm.unsafe_call_external(findSymbol("LOG_SETTOPICSTATUS"));
    vm.push_reference(topic);
    vm.push_reference(findSymbol("PRINT_DRAGKILLCOUNT"));
    vm.unsafe_call_bl(findSymbol("B_LOGENTRY"));
    vm.push_reference(vm.global_hero());
    vm.push_reference(findSymbol("$17824")); // Undead dragon exit waypoint.
    vm.unsafe_call_external(findSymbol("AI_TELEPORT"));

    const auto guildIs = [&](int32_t guild) {
      const auto hero = memberNpc(vm.global_hero());
      return hero && hero->guild==guild;
      };
    const bool paladin = guildIs(GIL_PAL);
    const bool militia = guildIs(GIL_MIL);
    if(paladin || militia) {
      play("$17825");
      } else {
      const bool dragonHunter = guildIs(GIL_DJG);
      const bool mercenary = guildIs(GIL_SLD);
      play(dragonHunter || mercenary ? "$17826" : "$17827");
      }
    }

  // PLAYVIDEOEX leaves its result on the stock stack. The call frame returns
  // the latest movie result, or the VM's default INT when no movie was played.
  return moviePlayed ? vm.pop_int() : 0;
  }

void GameScript::loadNativeNpcGlobals() {
  if(!nativeRoutineProgram)
    return;

  bindRuntimeVariable("KAPITEL",storyChapter);
  auto globals = NpcWorldData::loadGlobals(Resources::baseDir());
  const bool prepared = baseNativeCatalog()!=nullptr;
  if(!globals) {
    if(prepared)
      throw std::runtime_error("missing native NPC global bindings");
    return;
    }

  const auto initialize = std::function<void()>{[this,prepared,record=std::move(*globals)] {
    if(storyChapter==record.unsetChapter)
      storyChapter=record.defaultChapter;
    for(const auto& [source,target]:record.bindings) {
      if(prepared) {
        auto* alias = std::get_if<ScriptVars::Object>(runtimeState.find(target));
        if(alias==nullptr)
          throw std::runtime_error("missing native NPC alias: "+std::to_string(target));
        *alias = hlp_getnpc(source);
        }
      else {
        vm.push_int(source);
        vm.unsafe_call_external(findSymbol("HLP_GETNPC"));
        findSymbol(target)->set_instance(vm.pop_instance());
        }
      }
    }};
  if(prepared)
    nativeExternals.insert_or_assign("B_INITNPCGLOBALS",storygraph::makeNativeInternal(initialize));
  else
    vm.override_function("B_INITNPCGLOBALS",initialize,NpcDefinitionRuntime::enabled);
  }

void GameScript::loadNativeMonsterAttitudes() {
  if(!nativeRoutineProgram)
    return;

  auto rules = NpcWorldData::loadAttitudes(Resources::baseDir());
  const bool prepared = baseNativeCatalog()!=nullptr;
  if(!rules) {
    if(prepared)
      throw std::runtime_error("missing native monster attitudes");
    return;
    }

  const auto initialize = std::function<void()>{[this,prepared,rules=std::move(*rules)] {
    static constexpr int32_t humanGuilds[] = {
      GIL_NONE,GIL_PAL,GIL_MIL,GIL_VLK,GIL_KDF,GIL_NOV,GIL_DJG,GIL_SLD,GIL_BAU,GIL_BDT,GIL_STRF,GIL_OUT,GIL_PIR,GIL_KDW,
      };
    constexpr int32_t humanSeparator = int32_t(GIL_SEPERATOR_HUM);
    const auto write = [this,prepared](std::string_view name,int32_t value) {
      if(prepared) {
        if(!setGlobalIntSymbol(name,value))
          throw std::runtime_error("missing native monster attitude variable: "+std::string(name));
        }
      else {
        findSymbol(name)->set_int(value);
        }
      };
    for(const auto& rule : rules) {
      write("B_SETMONSTERATTITUDE.TOGUILD",rule.to);
      write("B_SETMONSTERATTITUDE.ATTITUDE",rule.attitude);
      write("B_SETMONSTERATTITUDE.FROMGUILD",rule.from);
      if(rule.to==humanSeparator) {
        for(const auto guild : humanGuilds)
          wld_setguildattitude(rule.from,rule.attitude,guild);
        }
      else if(rule.from==humanSeparator) {
        for(const auto guild : humanGuilds)
          wld_setguildattitude(guild,rule.attitude,rule.to);
        }
      else {
        wld_setguildattitude(rule.from,rule.attitude,rule.to);
        }
      }
    }};
  if(prepared)
    nativeExternals.insert_or_assign("B_INITMONSTERATTITUDES",storygraph::makeNativeInternal(initialize));
  else
    vm.override_function("B_INITMONSTERATTITUDES",initialize,NpcDefinitionRuntime::enabled);
  }
