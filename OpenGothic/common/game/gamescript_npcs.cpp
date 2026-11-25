#include "gamescript.h"

#include <bit>
#include <stdexcept>
#include <utility>
#include <vector>

#include "game/definitions/npcworlddata.h"
#include "resources.h"
#include "world/objects/npc.h"

bool GameScript::invokeNpcHelper(const std::shared_ptr<daedalus::INpc>& npc,uint32_t function,
                                 std::span<const NpcHelperDefinitions::Value> args) {
  if(vm.has_legacy_dispatcher())
    return NpcHelperRuntime::invokePrepared(vm,npc,function,npcDefinitions.helpers,args);
  return NpcHelperRuntime::invokePrepared(vm,npc,function,npcDefinitions.helpers,args,
    [this,&npc](const daedalus::DaedalusSymbol& operation,std::span<const NpcHelperDefinitions::Value> arguments,
                const std::function<void()>& observe) {
      using Kind=NpcHelperDefinitions::Value::Kind;
      using storygraph::NativeValue;
      constexpr storygraph::NativeInstance self=1;
      const auto external=nativeExternals.find(operation.name());
      std::array<NativeValue,8> values;
      if(external==nativeExternals.end() || arguments.size()>values.size() ||
         external->second.signature.parameters.size()!=arguments.size())
        throw std::runtime_error("NPC helper definitions: unavailable native operation "+operation.name());
      for(size_t index=0; index<arguments.size(); ++index) {
        const auto& argument=arguments[index];
        switch(argument.kind) {
          case Kind::Int:    values[index]=NativeValue::integer(argument.integer); break;
          case Kind::Float:  values[index]=NativeValue::real(argument.real); break;
          case Kind::String: values[index]=NativeValue::str(argument.text); break;
          case Kind::Self:   values[index]=NativeValue::instance(self); break;
          case Kind::Null:
            if(external->second.signature.parameters[index]==NativeValue::Inst)
              values[index]=NativeValue::instance(0);
            break;
          case Kind::Symbol: {
            const auto* symbol=vm.find_symbol_by_name(argument.text);
            if(symbol==nullptr)
              throw std::runtime_error("NPC helper definitions: unknown operation argument "+argument.text);
            values[index]=NativeValue::integer(int32_t(symbol->index()));
            break;
            }
          }
        if(values[index].type!=external->second.signature.parameters[index])
          throw std::runtime_error("NPC helper definitions: wrong native argument for "+operation.name());
        }
      if(observe)
        observe();
      const auto resolve=[&npc](storygraph::NativeInstance id) -> storygraph::BoundNativeCall::Instance {
        return id==self ? npc : nullptr;
        };
      return external->second.call(std::span(values).first(arguments.size()),resolve,{}).value.i;
      });
  }

void GameScript::initializeNpcDefinition(const std::shared_ptr<daedalus::INpc>& npc,
                                         daedalus::DaedalusSymbol* symbol, bool useRecords) {
  if(npc->user_ptr)
    static_cast<Npc*>(npc->user_ptr)->npcInstanceSymbol=symbol->index();
  // A director NPC keeps its own symbol while using its template's definition.
  auto* like=own.likeOf(vm,symbol->index());
  const auto applyCall = [this,&npc](const daedalus::DaedalusSymbol& function,
                                         const NpcDefinitionRuntime::Call& call,
                                         const std::function<void()>& observe) {
    using Type = daedalus::DaedalusDataType;
    using Kind = NpcDefinitions::Value::Kind;
    using storygraph::NativeValue;
    if(vm.has_legacy_dispatcher())
      return false;
    if(call.deferred) {
      if(!NpcHelperRuntime::enabled() || !npcDefinitions.helpers.helpers.contains(function.index()))
        return false;
      std::array<NpcHelperDefinitions::Value,6> arguments;
      if(call.args.size()>arguments.size())
        throw std::runtime_error("NPC helper definitions: too many recorded helper arguments");
      for(size_t index=0; index<call.args.size(); ++index) {
        const auto& argument=call.args[index];
        auto& value=arguments[index];
        value=argument.value;
        switch(argument.type) {
          case Type::STRING: value.text=vm.find_symbol_by_index(argument.literal)->get_string(argument.literalIndex); break;
          case Type::INT:
          case Type::FUNCTION: value.kind=Kind::Int; break;
          case Type::FLOAT: break;
          case Type::INSTANCE:
            if(value.kind!=Kind::Self)
              throw std::runtime_error("NPC helper definitions: helper targets another NPC");
            break;
          default: throw std::runtime_error("NPC helper definitions: unsupported prepared argument");
          }
        }
      return invokeNpcHelper(npc,function.index(),std::span(arguments).first(call.args.size()));
      }
    const auto external = nativeExternals.find(function.name());
    // The stock constructor corpus calls externals on SELF, with up to eight arguments.
    std::array<NativeValue,8> values;
    if(external==nativeExternals.end() ||
       call.args.empty() || call.args.front().value.kind!=Kind::Self || call.args.size()>values.size())
      return false;
    const auto& native = external->second;
    if(native.signature.result || native.signature.parameters.size()!=call.args.size())
      return false;
    constexpr storygraph::NativeInstance self = 1;
    for(size_t index=0; index<call.args.size(); ++index) {
      const auto& argument = call.args[index];
      const auto& value = argument.value;
      switch(argument.type) {
        case Type::FLOAT:  values[index] = NativeValue::real(value.real); break;
        case Type::STRING: values[index] = NativeValue::str(vm.find_symbol_by_index(argument.literal)->get_string(argument.literalIndex)); break;
        case Type::INT:
        case Type::FUNCTION: values[index] = NativeValue::integer(value.integer); break;
        case Type::INSTANCE:
          if(value.kind!=Kind::Self)
            return false;
          values[index] = NativeValue::instance(self);
          break;
        default:
          return false;
        }
      if(values[index].type!=native.signature.parameters[index])
        return false;
      }
    if(observe)
      observe();
    const auto resolve = [&npc](storygraph::NativeInstance id) -> storygraph::BoundNativeCall::Instance {
      return id==self ? npc : nullptr;
      };
    native.call(std::span(values).first(call.args.size()),resolve,{});
    return true;
    };

  const NpcDefinitionRuntime::Definitions empty;
  const auto applyField = vm.has_legacy_dispatcher() ? nullptr : &GameScript::applyNpcConstructorField;
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
  if(!globals)
    return;

  const auto lookup = findSymbol("HLP_GETNPC")->index();
  vm.override_function("B_INITNPCGLOBALS",std::function<void()>{[this,lookup,record=std::move(*globals)] {
    if(storyChapter==record.unsetChapter)
      storyChapter=record.defaultChapter;
    for(const auto& [source,target]:record.bindings) {
      vm.push_int(source);
      vm.unsafe_call_external(findSymbol(lookup));
      findSymbol(target)->set_instance(vm.pop_instance());
      }
    }},NpcDefinitionRuntime::enabled);
  }

void GameScript::loadNativeMonsterAttitudes() {
  if(!nativeRoutineProgram)
    return;

  auto rules = NpcWorldData::loadAttitudes(Resources::baseDir());
  if(!rules)
    return;

  vm.override_function("B_INITMONSTERATTITUDES",std::function<void()>{[this,rules=std::move(*rules)] {
    static constexpr int32_t humanGuilds[] = {
      GIL_NONE,GIL_PAL,GIL_MIL,GIL_VLK,GIL_KDF,GIL_NOV,GIL_DJG,GIL_SLD,GIL_BAU,GIL_BDT,GIL_STRF,GIL_OUT,GIL_PIR,GIL_KDW,
      };
    constexpr int32_t humanSeparator = int32_t(GIL_SEPERATOR_HUM);
    auto* fromGuild = findSymbol("B_SETMONSTERATTITUDE.FROMGUILD");
    auto* attitude = findSymbol("B_SETMONSTERATTITUDE.ATTITUDE");
    auto* toGuild = findSymbol("B_SETMONSTERATTITUDE.TOGUILD");
    for(const auto& rule : rules) {
      toGuild->set_int(rule.to);
      attitude->set_int(rule.attitude);
      fromGuild->set_int(rule.from);
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
    }},NpcDefinitionRuntime::enabled);
  }
