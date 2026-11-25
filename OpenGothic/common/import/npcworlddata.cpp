#include "baseimport.h"
#include "npcsnapshot.h"

#include "daedalus/script.h"
#include "game/definitions/gameplaydefinitions.h"
#include "game/definitions/npcworlddata.h"

namespace {
using Op = daedalus::DaedalusOpcode;
using Type = daedalus::DaedalusDataType;
using DefinitionJson::require;

NpcWorldData::Globals captureGlobals(const daedalus::DaedalusScript& script) {
  auto pc = script.find_symbol_by_name("B_INITNPCGLOBALS")->address();
  const auto take = [&](Op expected) {
    const auto instruction = script.instruction_at(pc);
    require(pc<script.size() && instruction.op==expected,"Invalid stock NPC global binding at "+std::to_string(pc));
    pc += instruction.size;
    return instruction;
    };

  NpcWorldData::Globals result;
  result.unsetChapter = take(Op::PUSHI).immediate;
  const auto chapter = take(Op::PUSHV).symbol;
  take(Op::EQ);
  const auto bindStart = take(Op::BZ).address;
  result.defaultChapter = take(Op::PUSHI).immediate;
  const auto destination = take(Op::PUSHV).symbol;
  take(Op::MOVI);
  require(pc==bindStart && chapter==destination && chapter==script.find_symbol_by_name("KAPITEL")->index(),
          "Invalid stock NPC global chapter default");
  const auto lookup = script.find_symbol_by_name("HLP_GETNPC")->index();
  while(pc<script.size() && script.instruction_at(pc).op!=Op::RSR) {
    const auto source = take(Op::PUSHI).immediate;
    const auto external = take(Op::BE).symbol;
    const auto target = take(Op::PUSHVI).symbol;
    take(Op::MOVVI);
    const auto* alias = script.find_symbol_by_index(target);
    require(external==lookup && alias!=nullptr && alias->type()==Type::INSTANCE && !alias->is_member(),
            "Invalid stock NPC global lookup");
    result.bindings.emplace_back(source,target);
    }
  take(Op::RSR);
  require(result.bindings.size()==310,"Stock NPC global alias count changed");
  return result;
  }

std::vector<NpcWorldData::AttitudeRule> captureAttitudes(const daedalus::DaedalusScript& script) {
  const auto setter = script.find_symbol_by_name("B_SETMONSTERATTITUDE")->address();
  auto pc = script.find_symbol_by_name("B_INITMONSTERATTITUDES")->address();
  const auto literal = [&] {
    const auto instruction = script.instruction_at(pc);
    const auto* value = instruction.op==Op::PUSHV ? script.find_symbol_by_index(instruction.symbol) : nullptr;
    require(pc<script.size() && value!=nullptr && value->is_const() && value->type()==Type::INT,
            "Invalid stock monster attitude literal at "+std::to_string(pc));
    pc += instruction.size;
    return value->get_int();
    };

  std::vector<NpcWorldData::AttitudeRule> rules;
  rules.reserve(1932);
  while(pc<script.size() && script.instruction_at(pc).op!=Op::RSR) {
    const NpcWorldData::AttitudeRule rule = {literal(),literal(),literal()};
    const auto call = script.instruction_at(pc);
    require(pc<script.size() && call.op==Op::BL && call.address==setter,
            "Invalid stock monster attitude call at "+std::to_string(pc));
    pc += call.size;
    rules.push_back(rule);
    }
  require(pc<script.size() && rules.size()==1932,"Stock monster attitude rule count changed");
  return rules;
  }
}

void BaseImport::convertNpcWorldData(const std::filesystem::path& base) {
  auto source = NpcSnapshot::load(base);
  if(source.sha256!=GameplayDefinitions::stockProgram)
    return;

  const auto script = daedalus::DaedalusScript::from_program(std::move(source.data));
  NpcWorldData::write(base,captureGlobals(script),captureAttitudes(script));
  }
