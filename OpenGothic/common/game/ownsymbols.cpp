#include "ownsymbols.h"

#include "game/definitions/nativebasecatalog.h"

#include <Tempest/Log>

#include <cctype>
#include <utility>

using namespace Tempest;

static std::string upper(std::string_view s) {
  std::string r(s);
  for(auto& c:r)
    c = char(std::toupper(static_cast<unsigned char>(c)));
  return r;
  }

const OwnSymbols::Entry* OwnSymbols::find(std::string_view name) const {
  const auto up = upper(name);
  for(auto& e:list)
    if(e.name==up)
      return &e;
  return nullptr;
  }

const OwnSymbols::Entry* OwnSymbols::find(uint32_t index) const {
  for(const auto& entry : list)
    if(entry.index==index)
      return &entry;
  return nullptr;
  }

std::optional<int32_t> OwnSymbols::intValue(std::string_view name) const {
  const auto* entry = find(name);
  if(entry!=nullptr && entry->kind==Kind::Int)
    return std::get<ScriptVars::Array<int32_t>>(*state.find(entry->index)).values.front();
  return std::nullopt;
  }

bool OwnSymbols::setInt(std::string_view name, int32_t value) {
  const auto* entry = find(name);
  if(entry==nullptr || entry->kind!=Kind::Int)
    return false;
  std::get<ScriptVars::Array<int32_t>>(*state.find(entry->index)).values.front() = value;
  return true;
  }

bool OwnSymbols::addInt(std::string_view name, std::string& why) {
  return add({Kind::Int,upper(name),{},0},why)!=nullptr;
  }

std::optional<uint32_t> OwnSymbols::addNpc(std::string_view name, std::string_view like, std::string& why) {
  if(const auto* entry = add({Kind::Npc,upper(name),upper(like),0},why))
    return entry->index;
  return std::nullopt;
  }

ScriptVars::Object* OwnSymbols::nativeNpc(uint32_t index) {
  if(catalog==nullptr || index<catalog->directorFirstId)
    return nullptr;
  const auto* entry = find(index);
  return entry!=nullptr && entry->kind==Kind::Npc ? std::get_if<ScriptVars::Object>(state.find(index)) : nullptr;
  }

const OwnSymbols::Entry* OwnSymbols::add(Entry entry, std::string& why) {
  if(catalog==nullptr || list.size()>=NativeBaseCatalog::directorCapacity) {
    why = "no room for " + entry.name;
    return nullptr;
    }
  if(entry.name.size()<=Prefix.size() || !entry.name.starts_with(Prefix) || upper(entry.name)!=entry.name) {
    why = entry.name + ": expected a director name beginning with " + std::string(Prefix);
    return nullptr;
    }
  if(entry.kind==Kind::Npc) {
    const auto* prototype = NativeBaseCatalog::findIdentity(catalog->identities.npcs,entry.like);
    if(prototype==nullptr || prototype->name!=entry.like) {
      why = entry.like + " is not an authored NPC";
      return nullptr;
      }
    }
  else if(entry.kind!=Kind::Int || !entry.like.empty()) {
    why = "invalid declaration for " + entry.name;
    return nullptr;
    }

  entry.index = catalog->directorFirstId+uint32_t(list.size());
  if(!state.declareDirector(entry)) {
    why = entry.name + " is taken";
    return nullptr;
    }
  Log::i("[director] ",entry.name," (",entry.index,")",entry.kind==Kind::Npc ? " made like " + entry.like : "");
  list.push_back(std::move(entry));
  return &list.back();
  }

void OwnSymbols::save(SaveArchive& fout) const {
  ScriptVars::writeSymbolDefinitions(fout,list);
  }

bool OwnSymbols::load(SaveArchive& fin) {
  const auto [format,count] = ScriptVars::readSymbolHeader(fin);
  if(format!=ScriptVars::SymbolFormat) {
    Log::e("[director] unsupported save declaration format ",int(format));
    return false;
    }
  if(catalog==nullptr || !list.empty() || count>NativeBaseCatalog::directorCapacity) {
    Log::e("[director] invalid native declaration count ",count);
    return false;
    }

  for(uint32_t i=0; i<count; ++i) {
    auto entry = ScriptVars::readSymbolDefinition(fin);
    if(entry.index!=catalog->directorFirstId+i) {
      Log::e("[director] ",entry.name," has unexpected saved ID ",entry.index);
      return false;
      }
    std::string why;
    if(add(std::move(entry),why)==nullptr) {
      Log::e("[director] could not restore declaration: ",why);
      return false;
      }
    }
  return true;
  }
