#include "ownsymbols.h"

#include "daedalus/vm.h"

#include <Tempest/Log>

#include <algorithm>
#include <cctype>

using namespace Tempest;
using daedalus::DaedalusDataType;

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

daedalus::DaedalusSymbol* OwnSymbols::binding(std::string_view name) const {
  if(!name.starts_with(Prefix))
    return nullptr;
  const auto found = bindings.find(name);
  return found!=bindings.end() ? found->second : nullptr;
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

daedalus::DaedalusSymbol* OwnSymbols::likeOf(daedalus::DaedalusVm& vm, uint32_t index) {
  const auto* entry = find(index);
  return entry!=nullptr && entry->kind==Kind::Npc ? vm.find_symbol_by_name(entry->like) : nullptr;
  }
daedalus::DaedalusSymbol* OwnSymbols::addInt(daedalus::DaedalusVm& vm, std::string_view name, std::string& why) {
  return add(vm,Entry{Kind::Int,upper(name),{},0},why);
  }

daedalus::DaedalusSymbol* OwnSymbols::addNpc(daedalus::DaedalusVm& vm, std::string_view name, std::string_view like, std::string& why) {
  return add(vm,Entry{Kind::Npc,upper(name),upper(like),0},why);
  }

daedalus::DaedalusSymbol* OwnSymbols::addNpc(daedalus::DaedalusVm& vm, std::string_view name,
                                          const daedalus::DaedalusSymbol& prototype, std::string& why) {
  return add(vm,Entry{Kind::Npc,upper(name),prototype.name(),0},why,&prototype);
  }

daedalus::DaedalusSymbol* OwnSymbols::add(daedalus::DaedalusVm& vm, const Entry& e, std::string& why,
                                        const daedalus::DaedalusSymbol* prototype) {
  if(e.name.size()<=Prefix.size() || !e.name.starts_with(Prefix)) {
    why = e.name + ": a symbol of ours is named " + std::string(Prefix) + "...";
    return nullptr;
    }
  if(bindings.contains(e.name)) {
    why = e.name + " is taken";
    return nullptr;
    }

  daedalus::DaedalusSymbol* s = nullptr;
  if(e.kind==Kind::Int) {
    s = vm.add_int_symbol(e.name);
    }
  else if(e.kind==Kind::Npc) {
    if(prototype==nullptr) {
      prototype = vm.find_symbol_by_name(e.like);
      if(prototype==nullptr || prototype->type()!=DaedalusDataType::INSTANCE || prototype->is_generated()) {
        why = e.like + " is not an instance of the scripts";
        return nullptr;
        }
      auto* cls = vm.find_symbol_by_index(prototype->parent());
      while(cls!=nullptr && cls->type()!=DaedalusDataType::CLASS)
        cls = vm.find_symbol_by_index(cls->parent());
      auto* npc = vm.find_symbol_by_name("C_NPC");
      if(cls==nullptr || npc==nullptr || cls->index()!=npc->index()) {
        why = e.like + " is not an npc";
        return nullptr;
        }
      }
    s = vm.add_instance_symbol(e.name,*prototype);
    }
  if(s==nullptr) {
    why = "no room for " + e.name;
    return nullptr;
    }

  state.bind(*s);
  Entry made = e;
  made.index = s->index();
  bindings.emplace(made.name,s);
  Log::i("[symbols] ",made.name," (",made.index,")",made.kind==Kind::Npc ? " made like " + made.like : "");
  list.push_back(std::move(made));
  return s;
  }

void OwnSymbols::save(SaveArchive& fout) const {
  ScriptVars::writeSymbolDefinitions(fout,list);
  }

bool OwnSymbols::load(daedalus::DaedalusVm& vm, SaveArchive& fin) {
  const auto [format,count] = ScriptVars::readSymbolHeader(fin);
  if(format!=ScriptVars::SymbolFormat) {
    Log::e("[symbols] a save of format ",int(format),", this game reads ",int(ScriptVars::SymbolFormat),": none made");
    return false;
    }

  bool same = true;
  for(uint32_t i=0; i<count; ++i) {
    const auto e = ScriptVars::readSymbolDefinition(fin);
    std::string why;
    auto*       s = e.kind==Kind::Int || e.kind==Kind::Npc ? add(vm,e,why) : nullptr;
    if(s==nullptr) {
      Log::e("[symbols] ",e.name," is not made again: ",why.empty() ? "unknown kind" : why);
      same = false;
      }
    else if(s->index()!=e.index) {
      Log::e("[symbols] ",e.name," is ",s->index()," now, the save has ",e.index,
             ": the scripts are not those of the save");
      same = false;
      }
    }
  return same;
  }
