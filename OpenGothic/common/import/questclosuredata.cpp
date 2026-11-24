#include "baseimport.h"

#include "daedalus/script.h"
#include "definitionjson.h"
#include "game/definitions/gameplaydefinitions.h"
#include "game/definitions/questclosuredata.h"
#include "npcsnapshot.h"

namespace {

using namespace QuestClosureData;
using DefinitionJson::require;
using Op = daedalus::DaedalusOpcode;
using Type = daedalus::DaedalusDataType;

Groups capture(const daedalus::DaedalusScript& script) {
  const auto* close = script.find_symbol_by_name("B_CLOSETOPIC");
  const auto status = script.find_symbol_by_name("LOG_SETTOPICSTATUS")->index();
  const auto hero = script.find_symbol_by_name("HERO")->index();
  const auto guild = script.find_symbol_by_name("C_NPC.GUILD")->index();
  auto pc = script.find_symbol_by_name("B_CHECKLOG")->address();
  const auto valid = [&](bool ok) {
    require(ok,"invalid stock quest closure at "+std::to_string(pc));
    };
  const auto take = [&](Op expected) {
    const auto instruction = script.instruction_at(pc);
    valid(instruction.op==expected);
    pc += instruction.size;
    return instruction;
    };
  const auto reference = [&](Type type) {
    const auto id = take(Op::PUSHV).symbol;
    const auto* symbol = script.find_symbol_by_index(id);
    valid(symbol!=nullptr && symbol->type()==type && !symbol->is_member());
    return id;
    };
  const auto number = [&]() -> Number {
    if(script.instruction_at(pc).op==Op::PUSHI)
      return take(Op::PUSHI).immediate;
    return reference(Type::INT);
    };

  for(const auto name : names)
    valid(take(Op::BL).address==script.find_symbol_by_name(name)->address());
  take(Op::RSR);

  Groups groups;
  for(size_t i=0;i<names.size();++i) {
    pc = script.find_symbol_by_name(names[i])->address();
    auto& group = groups[i];
    while(script.instruction_at(pc).op!=Op::RSR) {
      const auto* first = script.find_symbol_by_index(script.instruction_at(pc).symbol);
      if(first!=nullptr && first->type()==Type::STRING) {
        group.rows.push_back({reference(Type::STRING),{number(),number(),number()}});
        valid(take(Op::BL).address==close->address());
        continue;
        }

      // Yoly's guild exception runs between two ordinary topic calls.
      valid(i+1==names.size() && !group.guildRule);
      auto& rule = group.guildRule.emplace();
      rule.before = uint32_t(group.rows.size());
      for(auto& excluded : rule.excluded) {
        excluded = reference(Type::INT);
        valid(take(Op::GMOVI).symbol==hero && take(Op::PUSHV).symbol==guild);
        take(Op::NEQ);
        }
      take(Op::AND);
      take(Op::AND);
      const auto skip = take(Op::BZ).address;
      rule.topic = reference(Type::STRING);
      rule.status = reference(Type::INT);
      valid(take(Op::BE).symbol==status && pc==skip);
      }
    valid(group.rows.size()==counts[i] && group.guildRule.has_value()==(i+1==names.size()) &&
          (!group.guildRule || group.guildRule->before<group.rows.size()));
    }
  return groups;
  }

}

void BaseImport::convertQuestClosures(const std::filesystem::path& base) {
  auto source = NpcSnapshot::load(base);
  if(source.sha256!=GameplayDefinitions::stockProgram)
    return;
  const auto script = daedalus::DaedalusScript::from_program(std::move(source.data));
  const auto expected = capture(script);
  QuestClosureData::write(base,expected);
  require(QuestClosureData::read(base)==expected,"quest closure rows differ after reading");
  }
