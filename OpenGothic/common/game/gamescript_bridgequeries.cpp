#include "gamescript.h"

#include "game/definitions/basescripts.h"
#include "game/definitions/nativebasecatalog.h"
#include "world/bodytraces.h"
#include "world/objects/npc.h"

#include <algorithm>

std::optional<int32_t> GameScript::globalIntSymbol(std::string_view name) const {
  if(const auto value = own.intValue(name))
    return value;
  if(baseNativeCatalog()!=nullptr) {
    if(const auto* field = std::get_if<ScriptVars::Array<int32_t>>(runtimeState.find(name)))
      return field->values.front();
    return intConstant(name);
    }
  const auto* symbol = nativeRoutineProgram ? storySlot(name) : vm.find_symbol_by_name(name);
  if(symbol==nullptr || symbol->type()!=daedalus::DaedalusDataType::INT || symbol->is_member())
    return std::nullopt;
  const auto* field = std::get_if<ScriptVars::Array<int32_t>>(runtimeState.find(symbol->index()));
  return field!=nullptr ? field->values.front() : symbol->get_int(0);
  }

std::optional<int32_t> GameScript::intConstant(std::string_view name) const {
  if(name.empty())
    return std::nullopt;
  if(const auto* catalog = baseNativeCatalog()) {
    if(const auto* constant = NativeBaseCatalog::findConstant(*catalog,name))
      if(const auto* values = std::get_if<std::vector<int32_t>>(&constant->values))
        return values->front();
    return std::nullopt;
    }
  const auto* symbol = nativeRoutineProgram ? storySlot(name) : vm.find_symbol_by_name(name);
  if(symbol!=nullptr && symbol->is_const() && symbol->type()==daedalus::DaedalusDataType::INT)
    return symbol->get_int(0);
  return std::nullopt;
  }

std::optional<uint32_t> GameScript::findItemInstance(std::string_view name) const {
  using Type = daedalus::DaedalusDataType;
  if(nativeRoutineProgram) {
    SymbolKind kind = SymbolKind::Other;
    const auto* symbol = storySlot(name,&kind);
    if(symbol!=nullptr && kind==SymbolKind::Item)
      return symbol->index();
    return std::nullopt;
    }
  const auto* symbol = vm.find_symbol_by_name(name);
  if(symbol==nullptr || symbol->type()!=Type::INSTANCE)
    return std::nullopt;
  auto* parent = vm.find_symbol_by_index(symbol->parent());
  while(parent!=nullptr && parent->type()!=Type::CLASS)
    parent = vm.find_symbol_by_index(parent->parent());
  if(parent!=nullptr && parent==vm.find_symbol_by_name("C_ITEM"))
    return symbol->index();
  return std::nullopt;
  }

std::optional<uint32_t> GameScript::storyNpcId(std::string_view name) {
  if(storySymbolKind(name)==SymbolKind::Npc)
    if(const auto id = storySymbolIndex(name))
      return uint32_t(*id);
  return std::nullopt;
  }

std::string_view GameScript::storyNpcName(uint32_t id) {
  const auto name = storySymbolName(id);
  return storySymbolKind(name)==SymbolKind::Npc ? name : std::string_view{};
  }

std::string_view GameScript::storyItemName(uint32_t id) {
  const auto name = storySymbolName(id);
  return storySymbolKind(name)==SymbolKind::Item ? name : std::string_view{};
  }

std::string GameScript::addStoryVariable(std::string_view name, int32_t first) {
  if(const auto* entry = own.find(name))
    return entry->kind==OwnSymbols::Kind::Int ? std::string() : std::string(name) + " is taken";
  std::string error;
  if(own.addInt(vm,name,error))
    own.setInt(name,first);
  return error;
  }

std::optional<uint32_t> GameScript::addDirectorNpcSymbol(std::string_view name, std::string_view like, std::string& why) {
  SymbolKind kind = SymbolKind::Other;
  const auto* prototype = storySlot(like,&kind);
  if(prototype==nullptr || prototype->type()!=daedalus::DaedalusDataType::INSTANCE || prototype->is_generated()) {
    why = std::string(like) + " is not an instance of the scripts";
    return std::nullopt;
    }
  if(kind!=SymbolKind::Npc || !prototype->is_const()) {
    why = std::string(like) + " is not an npc";
    return std::nullopt;
    }
  if(auto* symbol = own.addNpc(vm,name,*prototype,why))
    return symbol->index();
  return std::nullopt;
  }

std::string_view GameScript::stringSymbol(std::string_view name, std::string_view fallback) const {
  if(const auto* catalog = baseNativeCatalog()) {
    if(const auto* field = std::get_if<ScriptVars::Array<std::string>>(runtimeState.find(name)))
      return field->values.front();
    if(const auto* constant = NativeBaseCatalog::findConstant(*catalog,name))
      if(const auto* values = std::get_if<std::vector<std::string>>(&constant->values))
        return values->front();
    return fallback;
    }
  const auto* symbol = vm.find_symbol_by_name(name);
  if(symbol!=nullptr && symbol->type()==daedalus::DaedalusDataType::STRING)
    return symbol->get_string();
  return fallback;
  }

int32_t GameScript::intSymbol(std::string_view name, int32_t fallback) {
  if(baseNativeCatalog()!=nullptr)
    return globalIntSymbol(name).value_or(fallback);
  auto* symbol = findSymbol(name);
  if(symbol==nullptr || symbol->type()!=daedalus::DaedalusDataType::INT)
    return fallback;
  return symbol->get_int(0);
  }

std::string_view GameScript::stringSymbol(std::string_view name) {
  if(baseNativeCatalog()!=nullptr)
    return stringSymbol(name,{});
  if(auto* symbol = findSymbol(name))
    return symbol->get_string();
  return "";
  }

int GameScript::whirlwindDuration() {
  if(baseNativeCatalog()!=nullptr)
    return std::clamp(intSymbol("SPL_TIME_WHIRLWIND",10),1,60);
  if(auto* duration = findSymbol("SPL_TIME_WHIRLWIND"))
    return std::clamp(duration->get_int(),1,60);
  return 10;
  }

std::array<int32_t,3> GameScript::storyNpcTypes() {
  const char* names[] = {"NPCTYPE_MAIN","NPCTYPE_OCMAIN","NPCTYPE_BL_MAIN"};
  std::array<std::optional<int32_t>,3> symbols;
  const bool nativeValues = baseNativeCatalog()!=nullptr;
  for(size_t i=0;i<symbols.size();++i) {
    if(nativeValues) {
      symbols[i] = globalIntSymbol(names[i]);
      continue;
      }
    auto* symbol = vm.find_symbol_by_name(names[i]);
    if(symbol!=nullptr && symbol->type()==daedalus::DaedalusDataType::INT)
      symbols[i] = symbol->get_int(0);
    }
  return BodyTraces::storyTypes(symbols);
  }

bool GameScript::isPartyMember(const Npc& npc) {
  const auto index = intConstant("AIV_PARTYMEMBER").value_or(-1);
  return index>=0 && size_t(index)<NpcRecord::aivar_count && npc.handlePtr()->aivar[size_t(index)]!=0;
  }

bool GameScript::setGlobalIntSymbol(std::string_view name, int32_t value) {
  if(own.setInt(name,value))
    return true;
  if(baseNativeCatalog()!=nullptr) {
    auto* field = std::get_if<ScriptVars::Array<int32_t>>(runtimeState.find(name));
    if(field==nullptr)
      return false;
    field->values.front() = value;
    return true;
    }
  auto* symbol = nativeRoutineProgram ? storySlot(name) : vm.find_symbol_by_name(name);
  if(symbol==nullptr || symbol->type()!=daedalus::DaedalusDataType::INT || symbol->is_member() || symbol->is_const())
    return false;
  auto* field = std::get_if<ScriptVars::Array<int32_t>>(runtimeState.find(symbol->index()));
  if(field!=nullptr)
    field->values.front() = value;
  else
    symbol->set_int(value);
  return true;
  }

void GameScript::setPartyMember(Npc& npc, bool active) {
  const auto index = intConstant("AIV_PARTYMEMBER").value_or(0);
  if(index>=0 && size_t(index)<NpcRecord::aivar_count)
    npc.handlePtr()->aivar[size_t(index)] = active ? 1 : 0;
  }

std::optional<bool> GameScript::queryNpcKnowsInfo(const Npc& npc, std::string_view name) {
  if(const auto* info = storyDialog(name))
    return doesNpcKnowInfo(*npc.handlePtr(),info->symbol_index());
  return std::nullopt;
  }
