#include "gamescript.h"

#include <array>
#include <initializer_list>
#include <stdexcept>
#include <variant>
#include <vector>

namespace {
PlacementDefinitionRuntime::Definitions dragonIslandPlacement(const daedalus::DaedalusScript& vm) {
  using Value = PlacementDefinitions::Value;
  struct Captain {
    std::string_view flag, npc;
    };
  static constexpr Captain captains[] = {
    {"JORGENISCAPTAIN", "VLK_4250_JORGEN_DI"},
    {"TORLOFISCAPTAIN", "SLD_801_TORLOF_DI"},
    {"JACKISCAPTAIN",   "VLK_444_JACK_DI"},
    };
  struct Crew {
    std::string_view flag, npc, partner = {};
    };
  static constexpr Crew crew[] = {
    {"LEE_ISONBOARD",      "SLD_800_LEE_DI"},
    {"MILTENNW_ISONBOARD", "PC_MAGE_DI",        "LESTER_ISONBOARD"},
    {"LESTER_ISONBOARD",   "PC_PSIONIC_DI",     "MILTENNW_ISONBOARD"},
    {"MARIO_ISONBOARD",    "NONE_101_MARIO_DI"},
    {"WOLF_ISONBOARD",     "SLD_811_WOLF_DI"},
    {"VATRAS_ISONBOARD",   "VLK_439_VATRAS_DI"},
    {"BENNET_ISONBOARD",   "SLD_809_BENNET_DI"},
    {"DIEGO_ISONBOARD",    "PC_THIEF_DI",       "LARES_ISONBOARD"},
    {"GORN_ISONBOARD",     "PC_FIGHTER_DI"},
    {"LARES_ISONBOARD",    "VLK_449_LARES_DI",  "DIEGO_ISONBOARD"},
    {"BIFF_ISONBOARD",     "DJG_713_BIFF_DI"},
    {"ANGAR_ISONBOARD",    "DJG_705_ANGAR_DI"},
    {"GIRION_ISONBOARD",   "PAL_207_GIRION_DI"},
    };
  PlacementDefinitions::Library library;
  auto& steps = library.placements["STARTUP_DRAGONISLAND"].steps;
  const auto number = [&](std::string_view symbol) { return vm.find_symbol_by_name(symbol)->get_int(); };
  const auto name = [](std::string_view value) { return Value{.kind=Value::Kind::Symbol,.text=std::string(value)}; };
  const auto string = [](std::string_view value) { return Value{.kind=Value::Kind::String,.text=std::string(value)}; };
  const auto integer = [](int32_t value) { return Value{.integer=value}; };
  const auto call = [&](const char* function, std::initializer_list<Value> args, bool deferred = false) {
    steps.emplace_back(PlacementDefinitions::Call{function,args,deferred});
    };
  const auto aboard = [&](std::string_view npc) { call("WLD_INSERTNPC",{name(npc),string("SHIP_DECK_01")}); };
  // Stock NPC constructors and the sitting routine switches do not write the crew flags.
  const auto success = number("LOG_SUCCESS");
  const auto yes = number("TRUE");
  for(const auto& row : captains)
    if(number(row.flag)==yes)
      aboard(row.npc);
  for(const auto& row : crew) {
    if(number(row.flag)!=success)
      continue;
    aboard(row.npc);
    if(!row.partner.empty() && number(row.partner)!=success)
      call("B_STARTOTHERROUTINE",{name(row.npc),string("SittingShipDI")},true);
    }
  aboard("NOV_600_PEDRO_DI");
  for(const auto* point : {"FP_ITEM_SHIP_03", "FP_ITEM_SHIP_07"})
    call("WLD_INSERTITEM",{name("ITMI_FLASK"),string(point)});
  // These authored sections consist only of literal placement calls. Read their
  // data from the stock package without executing any of their instructions.
  using Op = daedalus::DaedalusOpcode;
  using Type = daedalus::DaedalusDataType;
  for(const auto* section : {"STARTUP_NEWWORLD_PART_DRAGON_ISLAND_01", "STARTUP_NEWWORLD_PART_DRAGON_UNDEAD_01",
                             "STARTUP_NEWWORLD_PART_DRAGON_FINAL_01"}) {
    auto pc = vm.find_symbol_by_name(section)->address();
    for(;;) {
      if(pc>=vm.size())
        throw std::runtime_error("Dragon Island placement: missing return in "+std::string(section));
      const auto instance = vm.instruction_at(pc);
      if(instance.op==Op::RSR)
        break;
      // PUSHI, PUSHV and BE each carry one four-byte operand.
      if(vm.size()-pc<15)
        throw std::runtime_error("Dragon Island placement: truncated call in "+std::string(section));
      pc+=instance.size;
      const auto point = vm.instruction_at(pc);
      pc+=point.size;
      const auto insert = vm.instruction_at(pc);
      pc+=insert.size;
      const auto* what = vm.find_symbol_by_index(uint32_t(instance.immediate));
      const auto* where = vm.find_symbol_by_index(point.symbol);
      const auto* function = vm.find_symbol_by_index(insert.symbol);
      if(instance.op!=Op::PUSHI || !what || what->type()!=Type::INSTANCE || !what->is_const() ||
         point.op!=Op::PUSHV || !where || where->type()!=Type::STRING || !where->is_const() ||
         insert.op!=Op::BE || !function || (function->name()!="WLD_INSERTNPC" && function->name()!="WLD_INSERTITEM"))
        throw std::runtime_error("Dragon Island placement: unexpected instruction in "+std::string(section));
      call(function->name().c_str(),{name(what->name()),string(where->get_string())});
      }
    }

  call("PLAYVIDEO",{string("SHIP.BIK")});
  const auto topic = string(vm.find_symbol_by_name("TOPIC_HALLENVONIRDORATH")->get_string());
  call("LOG_CREATETOPIC",{topic,integer(number("LOG_MISSION"))});
  call("LOG_SETTOPICSTATUS",{topic,integer(number("LOG_RUNNING"))});
  call("LOG_ADDENTRY",{topic,string(vm.find_symbol_by_name("$53061")->get_string())});
  call("B_KAPITELWECHSEL",{integer(6),integer(number("DRAGONISLAND_ZEN"))},true);
  return PlacementDefinitionRuntime::resolve(library,vm);
  }
}

void GameScript::runStartup(std::string_view fn) {
  const auto* function = vm.find_symbol_by_name(fn);
  if(nativeRoutineProgram && PlacementDefinitionRuntime::enabled() && function && function->name()=="STARTUP_DRAGONISLAND") {
    PlacementDefinitionRuntime::execute(vm,function,dragonIslandPlacement(vm));
    return;
    }
  PlacementDefinitionRuntime::execute(vm,function,placementDefinitions);
  }

void GameScript::runWorldInit(std::string_view fn) {
  const auto* function = vm.find_symbol_by_name(fn);
  const bool native = nativeRoutineProgram && PlacementDefinitionRuntime::enabled() && function;
  if(native && (function->name()=="INIT_NEWWORLD" || function->name()=="INIT_OLDWORLD")) {
    const bool oldWorld = function->name()=="INIT_OLDWORLD";
    if(oldWorld) {
      for(auto name : {"INIT_SUB_OLDCAMP","INIT_SUB_DEMONTOWER","INIT_SUB_SURFACE"})
        vm.unsafe_call_bl(findSymbol(name));
      }
    for(auto name : {"B_INITMONSTERATTITUDES","B_INITGUILDATTITUDES","B_INITNPCGLOBALS"})
      vm.unsafe_call_bl(findSymbol(name));
    vm.unsafe_call_bl(findSymbol(oldWorld ? "B_ENTER_OLDWORLD" : "B_ENTER_NEWWORLD"));

    auto* yes = findSymbol("TRUE");
    auto* no = findSymbol("FALSE");
    auto* world = findSymbol(oldWorld ? "OLDWORLD_ZEN" : "NEWWORLD_ZEN");
    const auto advance = [&](int32_t chapter,std::string_view latchName,std::string_view readyName) {
      auto* latch = findSymbol(latchName);
      const bool pending = latch->get_int()==no->get_int();
      const bool ready = findSymbol(readyName)->get_int()==yes->get_int();
      if(pending && ready) {
        vm.push_int(chapter);
        vm.push_reference(world);
        vm.unsafe_call_bl(findSymbol("B_KAPITELWECHSEL"));
        latch->set_int(yes->get_int());
        }
      };
    if(oldWorld) {
      advance(4,"B_CHAPTER4_ONETIME","MIS_READYFORCHAPTER4");
      return;
      }

    auto* thievesCleared = findSymbol("ANDRE_DIEBESGILDE_AUFGERAEUMT");
    const bool pending = thievesCleared->get_int()!=yes->get_int();
    const bool betrayed = findSymbol("DIEBESGILDE_VERRATEN")->get_int()!=0;
    if(pending && betrayed) {
      const auto alive = [&](std::string_view alias) {
        vm.push_reference(findSymbol(alias));
        vm.unsafe_call_external(findSymbol("NPC_ISDEAD"));
        return vm.pop_int()==0;
        };
      // The original checks all three aliases before deciding to clear the guild.
      const bool ramirez = alive("RAMIREZ");
      const bool jesper = alive("JESPER");
      const bool cassia = alive("CASSIA");
      if(ramirez || jesper || cassia) {
        for(auto name : {"VLK_447_CASSIA","VLK_446_JESPER","VLK_445_RAMIREZ"})
          invokeKillNpc(int32_t(findSymbolIndex(name)));
        thievesCleared->set_int(yes->get_int());
        }
      }
    for(auto name : {"INIT_SUB_NEWWORLD_PART_CITY_01","INIT_SUB_NEWWORLD_PART_FARM_01",
                     "INIT_SUB_NEWWORLD_PART_XARDAS_01","INIT_SUB_NEWWORLD_PART_MONASTERY_01",
                     "INIT_SUB_NEWWORLD_PART_GREATPEASANT_01","INIT_SUB_NEWWORLD_PART_TROLLAREA_01",
                     "INIT_SUB_NEWWORLD_PART_FOREST_01","INIT_SUB_NEWWORLD_PART_PASS_TO_OW_01"})
      vm.unsafe_call_bl(findSymbol(name));
    advance(3,"B_CHAPTER3_ONETIME","MIS_READYFORCHAPTER3");
    advance(5,"B_CHAPTER5_ONETIME","MIS_ALLDRAGONSDEAD");
    return;
    }
  if(native && function->name()=="INIT_ADDONWORLD") {
    for(const auto* stage:{"INIT_SUB_ADDON_PART_BANDITSCAMP_01", "INIT_SUB_ADDON_PART_PIRATESCAMP_01",
                           "INIT_SUB_ADDON_PART_ENTRANCE_01", "INIT_SUB_ADDON_PART_GOLDMINE_01",
                           "INIT_SUB_ADDON_PART_CANYON_01", "INIT_SUB_ADDON_PART_VALLEY_01",
                           "INIT_SUB_ADDON_PART_ADANOSTEMPLE_01", "B_INITMONSTERATTITUDES",
                           "B_INITGUILDATTITUDES", "B_INITNPCGLOBALS", "B_ENTER_ADDONWORLD"})
      vm.unsafe_call_bl(findSymbol(stage));
    return;
    }
  if(!native || function->name()!="INIT_DRAGONISLAND") {
    vm.call_function(function);
    return;
    }

  const auto* npcGlobals = findSymbol("B_INITNPCGLOBALS");
  vm.unsafe_call_bl(findSymbol("B_INITMONSTERATTITUDES"));
  vm.unsafe_call_bl(findSymbol("B_INITGUILDATTITUDES"));
  vm.unsafe_call_bl(npcGlobals);
  currentStoryWorld=findSymbol("DRAGONISLAND_ZEN")->get_int();

  const auto key = int32_t(findSymbolIndex("ITKE_SHIP_LEVELCHANGE_MIS"));
  vm.push_reference(vm.global_hero());
  vm.push_int(key);
  vm.unsafe_call_external(findSymbol("NPC_HASITEMS"));
  const bool hasKey = vm.pop_int()!=0;
  if(hasKey) {
    vm.push_reference(vm.global_hero());
    vm.push_int(key);
    vm.push_int(1);
    vm.unsafe_call_external(findSymbol("NPC_REMOVEINVITEMS"));
    }
  vm.unsafe_call_bl(npcGlobals);
  // The stock void root discards the removal result on return.
  if(hasKey)
    (void)vm.pop_int();
  }

void GameScript::loadNativeWorldEntry() {
  if(!nativeRoutineProgram)
    return;

  findSymbol("CURRENTLEVEL")->bind_global_int(currentStoryWorld);
  static constexpr std::pair<std::string_view,int32_t GhostProgress::*> ghostFlags[]={
    {"GHOST_SCKNOWSHOW2GETINADANOSTEMPEL",&GhostProgress::knowsTempleEntrance},
    {"SC_SUMMONEDANCIENTGHOST",&GhostProgress::summonedAncientGhost},
    {"B_REMOVEQUARHODRON_ONETIME",&GhostProgress::removedQuarhodron},
    {"ADW_ADANOSTEMPEL_STONEGRD_TRIGG_FUNC_01_ONETIME",&GhostProgress::stoneGuardianTrigger},
    {"SC_TOOKRHADEMESTRAP",&GhostProgress::tookRhademesTrap},
    {"SC_TALKEDTORHADEMAFTER",&GhostProgress::talkedToRhademesAfter},
    {"B_REMOVERHADEMES_ONETIME",&GhostProgress::removedRhademes},
    {"GHOSTATTACKWARN",&GhostProgress::attackWarn},
    };
  for(const auto& [name,field]:ghostFlags)
    findSymbol(name)->bind_global_int(ghostProgress.*field);

  static constexpr std::pair<const char*,ChapterWorld> worlds[]={
    {"B_ENTER_NEWWORLD",ChapterWorld::New}, {"B_ENTER_OLDWORLD",ChapterWorld::Old}, {"B_ENTER_ADDONWORLD",ChapterWorld::Addon},
    };
  for(const auto& [name,region]:worlds)
    vm.override_function(name,std::function<void()>{[this,region] { enterChapterWorld(region); }},
                         PlacementDefinitionRuntime::enabled);

  static constexpr std::pair<const char*,const char*> latches[]={
    {"B_ENTER_NEWWORLD_KAPITEL_6","ENTERNW_KAPITEL6"},
    {"B_ENTER_OLDWORLD_KAPITEL_1","ENTEROW_KAPITEL1"},
    {"B_ENTER_OLDWORLD_KAPITEL_2","ENTEROW_KAPITEL2"},
    {"B_ENTER_OLDWORLD_KAPITEL_6","ENTEROW_KAPITEL6"},
    {"B_ENTER_ADDONWORLD_KAPITEL_2","ENTERADW_KAPITEL2"},
    {"B_ENTER_ADDONWORLD_KAPITEL_3","ENTERADW_KAPITEL3"},
    {"B_ENTER_ADDONWORLD_KAPITEL_5","ENTERADW_KAPITEL5"},
    {"B_ENTER_ADDONWORLD_KAPITEL_6","ENTERADW_KAPITEL6"},
    };
  static_assert(std::size(latches)==std::tuple_size_v<decltype(worldEntryLatches)>);
  for(size_t i=0;i<std::size(latches);++i) {
    const auto& [name,flag]=latches[i];
    findSymbol(flag)->bind_global_int(worldEntryLatches[i]);
    vm.override_function(name,std::function<void()>{[this,i] {
      if(worldEntryLatches[i]==findSymbol("FALSE")->get_int())
        worldEntryLatches[i]=findSymbol("TRUE")->get_int();
      }},PlacementDefinitionRuntime::enabled);
    }
  for(const auto* stage:{"B_INITGUILDATTITUDES","INIT_SUB_DEMONTOWER","INIT_SUB_NEWWORLD_PART_XARDAS_01",
                         "INIT_SUB_NEWWORLD_PART_TROLLAREA_01","INIT_SUB_NEWWORLD_PART_FOREST_01",
                         "INIT_SUB_NEWWORLD_PART_PASS_TO_OW_01",
                         "INIT_SUB_ADDON_PART_PIRATESCAMP_01", "INIT_SUB_ADDON_PART_ENTRANCE_01",
                         "INIT_SUB_ADDON_PART_GOLDMINE_01", "INIT_SUB_ADDON_PART_CANYON_01",
                         "INIT_SUB_ADDON_PART_VALLEY_01", "INIT_SUB_ADDON_PART_ADANOSTEMPLE_01"})
    vm.override_function(stage,std::function<void()>{[] {}},PlacementDefinitionRuntime::enabled);

  using Op = daedalus::DaedalusOpcode;
  using Type = daedalus::DaedalusDataType;
  using Symbol = daedalus::DaedalusSymbol;
  struct Room {
    Symbol* label;
    Symbol* guild;
    };
  struct RoomChoice {
    Symbol* expected;
    Symbol* flag;
    Room equal, otherwise;
    };
  struct MobRoutine {
    int32_t hour, minute;
    Symbol* name;
    int32_t state;
    };
  const auto* assign = findSymbol("WLD_ASSIGNROOMTOGUILD");
  const auto* setMobRoutine = findSymbol("WLD_SETMOBROUTINE");
  const auto assignRoom = [this,assign](const Room& room) {
    vm.push_reference(room.label);
    if(room.guild)
      vm.push_reference(room.guild);
    else
      vm.push_int(-1);
    vm.unsafe_call_external(assign);
    };
  static constexpr struct Region {
    const char* name;
    size_t roomCount, choiceCount = 0, mobCount = 0;
    } roomSections[] = {
    {"INIT_SUB_ADDON_PART_BANDITSCAMP_01",6}, {"INIT_SUB_NEWWORLD_PART_MONASTERY_01",13}, {"INIT_SUB_OLDCAMP",25},
    {"INIT_SUB_NEWWORLD_PART_CITY_01",44,4,3}, {"INIT_SUB_NEWWORLD_PART_GREATPEASANT_01",13,2},
    };
  for(const auto& region : roomSections) {
    auto pc = findSymbol(region.name)->address();
    const auto malformed = [&] { throw std::runtime_error("Unexpected room initializer in "+std::string(region.name)); };
    const auto take = [&] {
      const auto op = vm.instruction_at(pc);
      pc += op.size;
      return op;
      };
    const auto reference = [&](const daedalus::DaedalusInstruction& op) {
      return op.op==Op::PUSHV ? findSymbol(op.symbol) : nullptr;
      };
    const auto constant = [](const Symbol* symbol,Type type) {
      return symbol && symbol->type()==type && symbol->is_const() && !symbol->is_member() && symbol->count()==1;
      };
    const auto readRoom = [&](Symbol* room) {
      const auto owner = take();
      auto* guild = reference(owner);
      if(!constant(room,Type::STRING))
        malformed();
      if(guild) {
        if(!constant(guild,Type::INT))
          malformed();
        } else if(owner.op!=Op::PUSHI || owner.immediate!=1 || take().op!=Op::NEGATE) {
        malformed();
        }
      const auto call = take();
      if(call.op!=Op::BE || call.symbol!=assign->index())
        malformed();
      return Room{room,guild};
      };

    std::vector<MobRoutine> mobs;
    for(size_t i=0;i<region.mobCount;++i) {
      const auto hour = take();
      const auto minute = take();
      auto* name = reference(take());
      const auto state = take();
      const auto call = take();
      if(hour.op!=Op::PUSHI || minute.op!=Op::PUSHI || !constant(name,Type::STRING) ||
         state.op!=Op::PUSHI || call.op!=Op::BE || call.symbol!=setMobRoutine->index())
        malformed();
      mobs.push_back({hour.immediate,minute.immediate,name,state.immediate});
      }
    std::vector<std::variant<Room,RoomChoice>> rooms;
    rooms.reserve(region.roomCount);
    size_t choices = 0;
    for(size_t i=0;i<region.roomCount;++i) {
      auto* first = reference(take());
      if(constant(first,Type::STRING)) {
        rooms.emplace_back(readRoom(first));
        continue;
        }

      auto* flag = reference(take());
      if(!constant(first,Type::INT) || !flag || flag->type()!=Type::INT || flag->is_member() ||
         flag->is_const() || flag->count()!=1 || take().op!=Op::EQ)
        malformed();
      const auto branch = take();
      const auto equal = readRoom(reference(take()));
      const auto skip = take();
      if(branch.op!=Op::BZ || skip.op!=Op::B || pc!=branch.address)
        malformed();
      const auto otherwise = readRoom(reference(take()));
      if(pc!=skip.address)
        malformed();
      rooms.emplace_back(RoomChoice{first,flag,equal,otherwise});
      ++choices;
      }
    if(choices!=region.choiceCount || take().op!=Op::RSR)
      malformed();
    vm.override_function(region.name,std::function<void()>{[this,assignRoom,setMobRoutine,mobs=std::move(mobs),rooms=std::move(rooms)] {
      for(const auto& mob : mobs) {
        vm.push_int(mob.hour);
        vm.push_int(mob.minute);
        vm.push_reference(mob.name);
        vm.push_int(mob.state);
        vm.unsafe_call_external(setMobRoutine);
        }
      for(const auto& room : rooms) {
        if(const auto* fixed = std::get_if<Room>(&room)) {
          assignRoom(*fixed);
          } else {
          const auto& choice = std::get<RoomChoice>(room);
          assignRoom(choice.flag->get_int()==choice.expected->get_int() ? choice.equal : choice.otherwise);
          }
        }
      }},PlacementDefinitionRuntime::enabled);
    }
  vm.override_function("INIT_SUB_NEWWORLD_PART_FARM_01",std::function<void()>{[this,assignRoom] {
    auto* publicRoom = findSymbol("GIL_PUBLIC");
    assignRoom({findSymbol("$54786"),publicRoom});
    assignRoom({findSymbol("$54787"),publicRoom});
    if(findSymbol("LOBART_KLEIDUNG_VERKAUFT")->get_int()==findSymbol("TRUE")->get_int())
      assignRoom({findSymbol("$54788"),findSymbol("GIL_NONE")});
    else
      assignRoom({findSymbol("$54789"),publicRoom});
    }},PlacementDefinitionRuntime::enabled);
  vm.override_function("INIT_SUB_SURFACE",std::function<void()>{[this,assignRoom] {
    auto* seekers = findSymbol("GIL_DMT");
    assignRoom({findSymbol("$54426"),seekers});
    assignRoom({findSymbol("$54427"),seekers});
    vm.unsafe_call_bl(findSymbol("B_ENTER_OLDWORLD"));
    }},PlacementDefinitionRuntime::enabled);

  vm.override_function("B_KAPITELWECHSEL",std::function<void(int32_t,int32_t)>{
    [this](int32_t chapter,int32_t level) { changeChapter(chapter,level); }},PlacementDefinitionRuntime::enabled);
  }

void GameScript::enterChapterWorld(ChapterWorld region) {
  struct Entry {
    const char* level;
    std::array<const char*,6> chapters;
    };
  static constexpr Entry entries[]={
    {"NEWWORLD_ZEN",{"B_ENTER_NEWWORLD_KAPITEL_1", "B_ENTER_NEWWORLD_KAPITEL_2", "B_ENTER_NEWWORLD_KAPITEL_3",
                    "B_ENTER_NEWWORLD_KAPITEL_4", "B_ENTER_NEWWORLD_KAPITEL_5", "B_ENTER_NEWWORLD_KAPITEL_6"}},
    {"OLDWORLD_ZEN",{"B_ENTER_OLDWORLD_KAPITEL_1", "B_ENTER_OLDWORLD_KAPITEL_2", "B_ENTER_OLDWORLD_KAPITEL_3",
                    "B_ENTER_OLDWORLD_KAPITEL_4", "B_ENTER_OLDWORLD_KAPITEL_5", "B_ENTER_OLDWORLD_KAPITEL_6"}},
    {"ADDONWORLD_ZEN",{"B_ENTER_ADDONWORLD_KAPITEL_1", "B_ENTER_ADDONWORLD_KAPITEL_2", "B_ENTER_ADDONWORLD_KAPITEL_3",
                      "B_ENTER_ADDONWORLD_KAPITEL_4", "B_ENTER_ADDONWORLD_KAPITEL_5", "B_ENTER_ADDONWORLD_KAPITEL_6"}},
    };
  const auto& entry=entries[size_t(region)];
  const auto* npcGlobals=findSymbol("B_INITNPCGLOBALS");
  vm.unsafe_call_bl(npcGlobals);
  for(size_t i=0;i<entry.chapters.size();++i) {
    if(storyChapter>=int32_t(i)+1)
      vm.unsafe_call_bl(findSymbol(entry.chapters[i]));
    }
  const auto number=[&](std::string_view name) { return findSymbol(name)->get_int(); };
  currentStoryWorld=number(entry.level);
  vm.unsafe_call_bl(npcGlobals);
  if(region!=ChapterWorld::Old)
    return;

  const auto isDead=[&](std::string_view alias) {
    vm.push_reference(findSymbol(alias));
    vm.unsafe_call_external(findSymbol("NPC_ISDEAD"));
    return vm.pop_int();
    };
  const bool biffAlive=isDead("BIFF")==number("FALSE");
  const bool biffParty=number("DJG_BIFFPARTY")==number("TRUE");
  if(biffAlive && biffParty) {
    vm.push_reference(findSymbol("BIFF"));
    vm.push_int(number("DJG_BIFF_HALBEHALBE")==number("TRUE") ? 500 : 300);
    vm.unsafe_call_external(findSymbol("NPC_SETREFUSETALK"));
    }
  const bool rescueRunning=number("MIS_RESCUEBILGOT")==number("LOG_RUNNING");
  const bool bilgotDead=isDead("BILGOT")==number("TRUE");
  if(rescueRunning && bilgotDead)
    findSymbol("MIS_RESCUEBILGOT")->set_int(number("LOG_FAILED"));
  }

void GameScript::changeChapter(int32_t chapter, int32_t level) {
  auto* savedLevel=findSymbol("B_KAPITELWECHSEL.AKTUELLES_LEVEL_ZEN");
  savedLevel->set_int(level);
  findSymbol("B_KAPITELWECHSEL.NEUES_KAPITEL")->set_int(chapter);
  storyChapter=chapter;

  if(chapter==2) {
    for(const auto* name:{"SEKOB","BENGAR"}) {
      const auto actor=findSymbol(name)->get_instance();
      vm.unsafe_set_gi(actor);
      if(actor)
        findSymbol("C_NPC.FLAGS")->set_int(0,0,actor.get());
      }
    }
  if(chapter==1 || chapter==2) {
    static constexpr std::array introductions={
      std::array{"KAPWECHSEL_1","KAPWECHSEL_1_TEXT","$28527","$28528"},
      std::array{"KAPWECHSEL_2","KAPWECHSEL_2_TEXT","$28529","$28530"},
      };
    for(const auto* name:introductions[size_t(chapter)-1])
      vm.push_reference(findSymbol(name));
    vm.push_int(6000);
    vm.unsafe_call_external(findSymbol("INTRODUCECHAPTER"));
    }
  else if(chapter>=4 && chapter<=6) {
    findSymbol("PLAYER_TALENT_ALCHEMY")->set_int(findSymbol("TRUE")->get_int(),12);
    }

  vm.unsafe_call_bl(findSymbol("B_CHECKLOG"));
  // Nested story calls can update the parameter symbols and the chapter.
  if(savedLevel->get_int()==findSymbol("OLDWORLD_ZEN")->get_int())
    vm.unsafe_call_bl(findSymbol("B_ENTER_OLDWORLD"));
  if(savedLevel->get_int()==findSymbol("NEWWORLD_ZEN")->get_int())
    vm.unsafe_call_bl(findSymbol("B_ENTER_NEWWORLD"));

  static constexpr std::array ambientXp={
    "XP_AMBIENTKAP1","XP_AMBIENTKAP2","XP_AMBIENTKAP3","XP_AMBIENTKAP4","XP_AMBIENTKAP5","XP_AMBIENTKAP6",
    };
  if(storyChapter>=1 && storyChapter<=int32_t(ambientXp.size()))
    findSymbol("XP_AMBIENT")->set_int(findSymbol(ambientXp[size_t(storyChapter)-1])->get_int());
  }
