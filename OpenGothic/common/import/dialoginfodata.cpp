#include "baseimport.h"
#include "npcsnapshot.h"

#include "daedalus/script.h"
#include "game/definitions/dialoginfodata.h"
#include "game/definitions/gameplaydefinitions.h"

namespace {
using Op = daedalus::DaedalusOpcode;
using Type = daedalus::DaedalusDataType;
using DefinitionJson::require;

std::vector<DialogInfoData::Write> capture(const daedalus::DaedalusScript& script, const daedalus::DaedalusSymbol& symbol) {
  const auto valid = [&](bool value) {
    require(value,"dialog infos: unsupported constructor "+symbol.name());
    };
  const auto* parent = script.find_symbol_by_index(symbol.parent());
  valid(parent!=nullptr && parent->type()==Type::CLASS);

  uint32_t pc = symbol.address();
  const auto next = [&] {
    valid(pc<script.size());
    const auto instruction = script.instruction_at(pc);
    pc += instruction.size;
    return instruction;
    };
  const auto scalar = [&](const daedalus::DaedalusInstruction& operand) {
    const auto* value = operand.op==Op::PUSHV ? script.find_symbol_by_index(operand.symbol) : nullptr;
    return value && value->is_const() && !value->is_member() && value->count()==1 ? value : nullptr;
    };
  const auto integer = [&](const daedalus::DaedalusInstruction& operand) -> std::optional<int32_t> {
    if(operand.op==Op::PUSHI)
      return operand.immediate;
    const auto* value = scalar(operand);
    if(value && value->type()==Type::INT)
      return value->get_int();
    return std::nullopt;
    };

  std::vector<DialogInfoData::Write> fields;
  for(;;) {
    const auto source = next();
    if(source.op==Op::RSR)
      break;
    valid(fields.size()<8 && (source.op==Op::PUSHI || source.op==Op::PUSHV));
    auto target = next();
    const auto* constant = scalar(source);
    std::optional<DialogInfoData::Lesson> lesson;
    if(target.op==Op::PUSHVI && target.symbol==script.find_symbol_by_name("OTHER")->index() &&
       constant && constant->type()==Type::STRING) {
      // Only these topics use fixed prices in the constructor's learn-string expression.
      static constexpr std::string_view teachers[] = {
        "DIA_BENNET_TEACHCOMMON", "DIA_WOLF_TEACHCRAWLERPLATES", "DIA_DOBAR_TEACH",
        "DIA_BARTOK_TEACHSNEAK", "DIA_THORBEN_TEACH",
        };
      const auto talent = integer(next());
      const auto skill = integer(next());
      const auto costCall = next();
      const auto textCall = next();
      valid(std::ranges::find(teachers,symbol.name())!=std::end(teachers) && talent && skill &&
            costCall.op==Op::BL && costCall.address==script.find_symbol_by_name("B_GETLEARNCOSTTALENT")->address() &&
            textCall.op==Op::BL && textCall.address==script.find_symbol_by_name("B_BUILDLEARNSTRING")->address());
      lesson = DialogInfoData::Lesson{*talent,*skill,constant->get_string()};
      target = next();
      }

    const auto store = next().op;
    const auto* member = target.op==Op::PUSHV ? script.find_symbol_by_index(target.symbol) : nullptr;
    valid(member!=nullptr && member->parent()==parent->index() && member->is_member() &&
          !member->is_const() && member->count()==1 && (source.op!=Op::PUSHV || constant!=nullptr));
    auto& field = fields.emplace_back();
    field.member = member->index();
    if(member->type()==Type::STRING && store==Op::MOVS && constant && constant->type()==Type::STRING) {
      if(lesson)
        field.value = std::move(*lesson);
      else
        field.value = constant->get_string();
      }
    else if(!lesson && member->type()==Type::INT && store==Op::MOVI && (!constant || constant->type()==Type::INT))
      field.value = constant ? constant->get_int() : source.immediate;
    else if(!lesson && member->type()==Type::FUNCTION && store==Op::MOVVF && source.op==Op::PUSHI)
      field.value = source.immediate;
    else
      valid(false);
    }
  return fields;
  }
}

void BaseImport::convertDialogInfos(const std::filesystem::path& base) {
  auto source = NpcSnapshot::load(base);
  if(source.sha256!=GameplayDefinitions::stockProgram)
    return;

  auto script = daedalus::DaedalusScript::from_program(std::move(source.data));
  DialogInfoData::Rows rows;
  script.enumerate_instances_by_class_name("C_INFO",[&](const auto& symbol) {
    rows.emplace(symbol.index(),capture(script,symbol));
    });
  require(rows.size()==4139,"dialog infos: incomplete stock catalogue");
  DialogInfoData::write(base,rows);
  }
