#include "gamescript.h"
#include "game/definitions/basescripts.h"

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
                                           std::span<const HelperValue> arguments,bool returns) {
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
  const auto resolve=[&npc](storygraph::NativeInstance id) -> ScriptVars::Object {
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

bool GameScript::applyNpcConstructorCall(const std::shared_ptr<daedalus::INpc>& npc,
                                         const NpcDefinitionRuntime::Call& call) {
  using Type = NativeBaseCatalog::NpcConstructorType;
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
        case Type::String: break;
        case Type::Integer:
        case Type::Function: value.kind=HelperKind::Int; break;
        case Type::Float: break;
        case Type::Instance:
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
      case Type::Float: values[index]=NativeValue::real(value.real); break;
      case Type::String: values[index]=NativeValue::str(value.text); break;
      case Type::Integer:
      case Type::Function: values[index]=NativeValue::integer(value.integer); break;
      case Type::Instance:
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
  const auto resolve=[&npc](storygraph::NativeInstance id) -> ScriptVars::Object {
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

void GameScript::refreshNpcAtInsert(Npc& npc) {
  auto& record = npc.handle();
  if(record.attribute[ATR_HITPOINTS]<=0)
    return;

  // Retain the saved local and compare instance identities, as the stock script
  // does even when the currently controlled character is not PC_HERO.
  const auto hero = hlp_getnpc(int(findSymbolIndex("PC_HERO")));
  storyActorSlot("B_REFRESHATINSERT.HER") = hero;
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
  nativeExternals.insert_or_assign("B_KILLNPC",storygraph::makeNativeInternal(
    std::function<void(int32_t)>{[this](int32_t symbol) { killNpc(symbol); }}));
  nativeExternals.insert_or_assign("B_REMOVENPC",storygraph::makeNativeInternal(
    std::function<void(int32_t)>{[this](int32_t symbol) { removeNpc(symbol); }}));
  nativeExternals.insert_or_assign("B_DRAGONKILLCOUNTER",storygraph::makeNativeInternal(
    std::function<int32_t(NpcRef)>{[this](NpcRef actor) { return countDragonDeaths(actor); }}));
  }

void GameScript::invokeKillNpc(int32_t symbol) {
  killNpc(symbol);
  }

void GameScript::killNpc(int32_t symbol) {
  setNpcRuleInt("B_KILLNPC.NPCINSTANCE",symbol);
  const auto npc = hlp_getnpc(symbol);
  storyActorSlot("B_KILLNPC.NPC") = npc;
  const bool alive = !npc_isdead(npc);
  if(!hlp_isvalidnpc(npc) || !alive)
    return;

  npc->flags = NpcFlag::NONE;
  createinvitem(npc,int32_t(findSymbolIndex("ITMI_OLDCOIN")));
  const auto target = nativeNpc("B_KILLNPC.NPC");
  const auto maximum = target ? target->attribute[ATR_HITPOINTSMAX] : 0;
  npc_changeattribute(target,ATR_HITPOINTS,std::bit_cast<int32_t>(0u-uint32_t(maximum)));
  }

void GameScript::removeNpc(int32_t symbol) {
  setNpcRuleInt("B_REMOVENPC.NPCINSTANCE",symbol);
  const auto npc = hlp_getnpc(symbol);
  storyActorSlot("B_REMOVENPC.NPC") = npc;
  const bool dead = npc_isdead(npc);
  if(!hlp_isvalidnpc(npc) || dead)
    return;

  npc->flags = NpcFlag::NONE;
  ai_teleport(npc,"TOT");
  startOtherRoutine(npc,"TOT");

  const auto target = nativeNpc("B_REMOVENPC.NPC");
  const auto maximum = target ? target->attribute[ATR_HITPOINTSMAX] : 0;
  npc_changeattribute(target,ATR_HITPOINTS,std::bit_cast<int32_t>(0u-uint32_t(maximum)));
  ai_teleport(nativeNpc("B_REMOVENPC.NPC"),"TOT");
  }

int32_t GameScript::countDragonDeaths(const std::shared_ptr<daedalus::INpc>& actor) {
  const auto lookup = [&](std::string_view name,std::string_view local) {
    storyActorSlot(local) = hlp_getnpc(int32_t(findSymbolIndex(name)));
    };
  const auto matchesCurrent = [&](std::string_view local) {
    return hlp_getinstanceid(nativeNpc(local))==hlp_getinstanceid(nativeNpc("B_DRAGONKILLCOUNTER.CURRENT_DRAGON"));
    };
  int32_t movieResult=0;
  const auto play = [&](std::string_view movie) {
    using storygraph::NativeValue;
    const std::array args={NativeValue::str(std::string(movie)),NativeValue::integer(1),NativeValue::integer(0)};
    movieResult=nativeExternals.at("PLAYVIDEOEX").call(args,{},{}).value.i;
    };

  storyActorSlot("B_DRAGONKILLCOUNTER.CURRENT_DRAGON") = actor;
  lookup("BDT_1090_ADDON_RAVEN","B_DRAGONKILLCOUNTER.RAVN");
  if(matchesCurrent("B_DRAGONKILLCOUNTER.RAVN") && deathProgress.ravenDead==0) {
    play("EXTRO_RAVEN.BIK");
    deathProgress.ravenDead=1;
    removeNpc(int32_t(findSymbolIndex("MYXIR_ADW")));
    }

  const auto dragon = nativeNpc("B_DRAGONKILLCOUNTER.CURRENT_DRAGON");
  if(dragon && dragon->guild==GIL_DRAGON) {
    constexpr std::pair<std::string_view,std::string_view> dragons[] = {
      {"DRAGON_SWAMP","B_DRAGONKILLCOUNTER.SWAPDRAGN"},
      {"DRAGON_ROCK", "B_DRAGONKILLCOUNTER.RCKDRAGN"},
      {"DRAGON_FIRE", "B_DRAGONKILLCOUNTER.FREDRAGN"},
      {"DRAGON_ICE",  "B_DRAGONKILLCOUNTER.ICDRAGN"},
      };
    for(const auto& [name,local]:dragons)
      lookup(name,local);
    for(size_t i=0;i<std::size(dragons);++i) {
      auto& latch=deathProgress.dragonDeaths[i];
      if(matchesCurrent(dragons[i].second) && latch==0) {
        deathProgress.dragonsKilled=std::bit_cast<int32_t>(uint32_t(deathProgress.dragonsKilled)+1u);
        latch=1;
        }
      }
    }

  if(deathProgress.dragonsKilled==4) {
    deathProgress.allDragonsDead=1;
    if(!npc_isdead(nativeNpc("BIFF")) && npcRuleInt("DJG_BIFFPARTY")==1)
      deathProgress.biffSurvived=1;
    }

  constexpr size_t monsterId = 43; // Stock AIVAR slot.
  const auto defeated = nativeNpc("B_DRAGONKILLCOUNTER.CURRENT_DRAGON");
  if(defeated && defeated->aivar[monsterId]==npcRuleInt("ID_DRAGON_UNDEAD")) {
    deathProgress.undeadDragonDead=1;
    const auto topic = npcRuleString("TOPIC_BACKTOSHIP");
    log_createtopic(topic,npcRuleInt("LOG_MISSION"));
    log_settopicstatus(topic,npcRuleInt("LOG_RUNNING"));
    const auto entry = callStoryFunction("B_LOGENTRY",findNpc(nativeNpc("SELF")),findNpc(nativeNpc("OTHER")),
                                         {std::string(topic),std::string(npcRuleString("PRINT_DRAGKILLCOUNT"))});
    if(!entry.ok)
      throw std::runtime_error("B_LOGENTRY: "+entry.error);
    const auto hero = nativeNpc("HERO");
    ai_teleport(hero,"UNDEAD_ENDTELEPORT");

    const auto guild = hero ? int32_t(hero->guild) : int32_t(GIL_NONE);
    if(guild==GIL_PAL || guild==GIL_MIL)
      play("EXTRO_PAL.BIK");
    else if(guild==GIL_DJG || guild==GIL_SLD)
      play("EXTRO_DJG.BIK");
    else
      play("EXTRO_KDF.BIK");
    }

  return movieResult;
  }

void GameScript::loadNativeNpcGlobals() {
  if(!nativeRoutineProgram)
    return;

  bindRuntimeVariable("KAPITEL",storyChapter);
  auto globals = NpcWorldData::loadGlobals(Resources::baseDir());
  if(!globals)
    throw std::runtime_error("missing native NPC global bindings");

  const auto initialize = std::function<void()>{[this,record=std::move(*globals)] {
    if(storyChapter==record.unsetChapter)
      storyChapter=record.defaultChapter;
    for(const auto& [source,target]:record.bindings) {
      auto* alias = std::get_if<ScriptVars::Object>(runtimeState.find(target));
      if(alias==nullptr)
        throw std::runtime_error("missing native NPC alias: "+std::to_string(target));
      *alias = hlp_getnpc(source);
      }
    }};
  nativeExternals.insert_or_assign("B_INITNPCGLOBALS",storygraph::makeNativeInternal(initialize));
  }

void GameScript::loadNativeMonsterAttitudes() {
  if(!nativeRoutineProgram)
    return;

  auto rules = NpcWorldData::loadAttitudes(Resources::baseDir());
  if(!rules)
    throw std::runtime_error("missing native monster attitudes");

  const auto initialize = std::function<void()>{[this,rules=std::move(*rules)] {
    static constexpr int32_t humanGuilds[] = {
      GIL_NONE,GIL_PAL,GIL_MIL,GIL_VLK,GIL_KDF,GIL_NOV,GIL_DJG,GIL_SLD,GIL_BAU,GIL_BDT,GIL_STRF,GIL_OUT,GIL_PIR,GIL_KDW,
      };
    constexpr int32_t humanSeparator = int32_t(GIL_SEPERATOR_HUM);
    for(const auto& rule : rules) {
      setNpcRuleInt("B_SETMONSTERATTITUDE.TOGUILD",rule.to);
      setNpcRuleInt("B_SETMONSTERATTITUDE.ATTITUDE",rule.attitude);
      setNpcRuleInt("B_SETMONSTERATTITUDE.FROMGUILD",rule.from);
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
  nativeExternals.insert_or_assign("B_INITMONSTERATTITUDES",storygraph::makeNativeInternal(initialize));
  }
