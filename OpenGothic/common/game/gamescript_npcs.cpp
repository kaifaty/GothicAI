#include "gamescript.h"

#include <bit>
#include <stdexcept>
#include <utility>
#include <vector>

#include "world/objects/npc.h"

void GameScript::initializeNpcDefinition(const std::shared_ptr<daedalus::INpc>& npc,
                                         daedalus::DaedalusSymbol* symbol, bool useRecords) {
  if(npc->user_ptr)
    static_cast<Npc*>(npc->user_ptr)->npcInstanceSymbol=symbol->index();
  // A director NPC keeps its own symbol while using its template's definition.
  auto* like=own.likeOf(symbol->index());
  const NpcDefinitionRuntime::Definitions empty;
  NpcDefinitionRuntime::initialize(vm,npc,symbol,useRecords ? npcDefinitions : empty,like);
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
    findSymbol(name)->bind_global_int(deathProgress.*field);
  const std::array flags={"SWAPDRAGNISDEAD","RCKDRAGNISDEAD","FREDRAGNISDEAD","ICDRAGNISDEAD"};
  for(size_t i=0;i<flags.size();++i)
    findSymbol(flags[i])->bind_global_int(deathProgress.dragonDeaths[i]);
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
  findSymbol("B_KILLNPC.NPCINSTANCE")->set_int(symbol);
  auto* local=findSymbol("B_KILLNPC.NPC");
  // Use the registered engine calls so a death during world init still runs
  // the startup floor barrier for the affected NPC and its neighbours.
  vm.push_int(symbol);
  vm.unsafe_call_external(findSymbol("HLP_GETNPC"));
  const auto npc=std::static_pointer_cast<daedalus::INpc>(vm.pop_instance());
  local->set_instance(npc);
  vm.push_instance(npc);
  vm.unsafe_call_external(findSymbol("NPC_ISDEAD"));
  const bool dead=vm.pop_int()!=0;
  vm.push_instance(npc);
  vm.unsafe_call_external(findSymbol("HLP_ISVALIDNPC"));
  const bool valid=vm.pop_int()!=0;
  if(dead || !valid)
    return;

  // The dispatch frame restores the member context on return.
  vm.unsafe_set_gi(npc);
  npc->flags=NpcFlag::NONE;
  vm.push_instance(npc);
  vm.push_int(int32_t(findSymbolIndex("ITMI_OLDCOIN")));
  vm.unsafe_call_external(findSymbol("CREATEINVITEM"));

  // The script reads its instance local and current HP after creating the item.
  const auto target=std::static_pointer_cast<daedalus::INpc>(local->get_instance());
  vm.unsafe_set_gi(target);
  vm.push_instance(target);
  vm.push_int(ATR_HITPOINTS);
  vm.push_int(target ? -target->attribute[ATR_HITPOINTS] : 0);
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

  // Capture a typed chapter default and ordered alias table once. World entry
  // resolves the source NPCs anew against the current world.
  using Op=daedalus::DaedalusOpcode;
  auto pc=findSymbol("B_INITNPCGLOBALS")->address();
  const auto take=[&](Op expected) {
    const auto instruction=vm.instruction_at(pc);
    if(instruction.op!=expected)
      throw std::runtime_error("Invalid stock NPC global binding at "+std::to_string(pc));
    pc+=instruction.size;
    return instruction;
    };
  const auto unsetChapter=take(Op::PUSHI).immediate;
  const auto chapter=take(Op::PUSHV).symbol;
  take(Op::EQ);
  const auto bindStart=take(Op::BZ).address;
  const auto defaultChapter=take(Op::PUSHI).immediate;
  const auto destination=take(Op::PUSHV).symbol;
  take(Op::MOVI);
  if(pc!=bindStart || chapter!=destination || chapter!=findSymbolIndex("KAPITEL"))
    throw std::runtime_error("Invalid stock NPC global chapter default");

  const auto lookup=findSymbol("HLP_GETNPC")->index();
  std::vector<std::pair<int32_t,uint32_t>> bindings;
  while(vm.instruction_at(pc).op!=Op::RSR) {
    const auto source=take(Op::PUSHI).immediate;
    const auto external=take(Op::BE).symbol;
    const auto target=take(Op::PUSHVI).symbol;
    take(Op::MOVVI);
    if(external!=lookup)
      throw std::runtime_error("Invalid stock NPC global lookup");
    bindings.emplace_back(source,target);
    }

  findSymbol(chapter)->bind_global_int(storyChapter);
  vm.override_function("B_INITNPCGLOBALS",std::function<void()>{[this, unsetChapter, defaultChapter,
                                                                lookup, bindings=std::move(bindings)] {
    if(storyChapter==unsetChapter)
      storyChapter=defaultChapter;
    for(const auto& [source,target]:bindings) {
      vm.push_int(source);
      vm.unsafe_call_external(findSymbol(lookup));
      findSymbol(target)->set_instance(vm.pop_instance());
      }
    }},NpcDefinitionRuntime::enabled);
  }

void GameScript::loadNativeMonsterAttitudes() {
  if(!nativeRoutineProgram)
    return;

  using Op=daedalus::DaedalusOpcode;
  using Type=daedalus::DaedalusDataType;
  const auto setter = findSymbol("B_SETMONSTERATTITUDE")->address();
  auto pc = findSymbol("B_INITMONSTERATTITUDES")->address();
  const auto literal = [&] {
    const auto instruction = vm.instruction_at(pc);
    const auto* value = instruction.op==Op::PUSHV ? findSymbol(instruction.symbol) : nullptr;
    if(value==nullptr || !value->is_const() || value->type()!=Type::INT)
      throw std::runtime_error("Invalid stock monster attitude literal at "+std::to_string(pc));
    pc += instruction.size;
    return value->get_int();
    };
  struct AttitudeRule { int32_t from, attitude, to; };
  std::vector<AttitudeRule> rules;
  rules.reserve(1932);
  while(vm.instruction_at(pc).op!=Op::RSR) {
    const AttitudeRule rule = {literal(),literal(),literal()};
    const auto call = vm.instruction_at(pc);
    if(call.op!=Op::BL || call.address!=setter)
      throw std::runtime_error("Invalid stock monster attitude call at "+std::to_string(pc));
    pc += call.size;
    rules.push_back(rule);
    }

  vm.override_function("B_INITMONSTERATTITUDES",std::function<void()>{[this,rules=std::move(rules)] {
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
