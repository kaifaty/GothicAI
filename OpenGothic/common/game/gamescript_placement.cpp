#include "gamescript.h"

#include <initializer_list>
#include <stdexcept>

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
