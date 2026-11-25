#include "gamescript_programs.h"

#include "game/definitions/basescripts.h"
#include "game/definitions/nativebasecatalog.h"
#include "world/bodytraces.h"
#include "world/objects/npc.h"

#include <algorithm>

std::optional<int32_t> GameScript::globalIntSymbol(std::string_view name) const {
  if(const auto* field = BennetOre::progressField(*this,name))
    return *field;
  if(const auto* field = std::get_if<ScriptVars::Array<int32_t>>(runtimeState.find(name)))
    return field->values.front();
  return intConstant(name);
  }

std::optional<int32_t> GameScript::intConstant(std::string_view name) const {
  if(const auto* constant = NativeBaseCatalog::findConstant(*baseNativeCatalog(),name))
    if(const auto* values = std::get_if<std::vector<int32_t>>(&constant->values))
      return values->front();
  return std::nullopt;
  }

std::optional<uint32_t> GameScript::findItemInstance(std::string_view name) const {
  const auto* item = NativeBaseCatalog::findIdentity(baseNativeCatalog()->identities.items,name);
  return item!=nullptr ? std::optional(item->id) : std::nullopt;
  }

std::optional<uint32_t> GameScript::storyNpcId(std::string_view name) {
  if(const auto* npc = NativeBaseCatalog::findIdentity(baseNativeCatalog()->identities.npcs,name))
    return npc->id;
  const auto* generated = own.find(name);
  return generated!=nullptr && generated->kind==OwnSymbols::Kind::Npc ? std::optional(generated->index) : std::nullopt;
  }

std::string_view GameScript::storyNpcName(uint32_t id) {
  if(const auto* npc = NativeBaseCatalog::findIdentity(baseNativeCatalog()->identities.npcs,id))
    return npc->name;
  const auto* generated = own.find(id);
  return generated!=nullptr && generated->kind==OwnSymbols::Kind::Npc ? std::string_view(generated->name) : std::string_view{};
  }

std::string_view GameScript::storyItemName(uint32_t id) {
  const auto* item = NativeBaseCatalog::findIdentity(baseNativeCatalog()->identities.items,id);
  return item!=nullptr ? std::string_view(item->name) : std::string_view{};
  }

std::string GameScript::addStoryVariable(std::string_view name, int32_t first) {
  if(const auto* entry = own.find(name))
    return entry->kind==OwnSymbols::Kind::Int ? std::string() : std::string(name) + " is taken";
  std::string error;
  if(own.addInt(name,error))
    own.setInt(name,first);
  return error;
  }

std::optional<uint32_t> GameScript::addDirectorNpcSymbol(std::string_view name, std::string_view like, std::string& why) {
  return own.addNpc(name,like,why);
  }

std::string_view GameScript::stringSymbol(std::string_view name, std::string_view fallback) const {
  if(const auto* field = std::get_if<ScriptVars::Array<std::string>>(runtimeState.find(name)))
    return field->values.front();
  if(const auto* constant = NativeBaseCatalog::findConstant(*baseNativeCatalog(),name))
    if(const auto* values = std::get_if<std::vector<std::string>>(&constant->values))
      return values->front();
  return fallback;
  }

int32_t GameScript::intSymbol(std::string_view name, int32_t fallback) {
  return globalIntSymbol(name).value_or(fallback);
  }

std::string_view GameScript::stringSymbol(std::string_view name) {
  return stringSymbol(name,{});
  }

int GameScript::whirlwindDuration() {
  return std::clamp(intSymbol("SPL_TIME_WHIRLWIND",10),1,60);
  }

std::array<int32_t,3> GameScript::storyNpcTypes() {
  return BodyTraces::storyTypes({globalIntSymbol("NPCTYPE_MAIN"),globalIntSymbol("NPCTYPE_OCMAIN"),
                                 globalIntSymbol("NPCTYPE_BL_MAIN")});
  }

bool GameScript::isPartyMember(const Npc& npc) {
  const auto index = intConstant("AIV_PARTYMEMBER").value_or(-1);
  return index>=0 && size_t(index)<NpcRecord::aivar_count && npc.handlePtr()->aivar[size_t(index)]!=0;
  }

bool GameScript::setGlobalIntSymbol(std::string_view name, int32_t value) {
  if(auto* field = BennetOre::progressField(*this,name)) {
    *field = value;
    return true;
    }
  auto* field = std::get_if<ScriptVars::Array<int32_t>>(runtimeState.find(name));
  if(field==nullptr)
    return false;
  field->values.front() = value;
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
