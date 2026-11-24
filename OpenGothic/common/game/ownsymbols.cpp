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

std::optional<int32_t> OwnSymbols::intValue(std::string_view name) const {
  if(!integers.empty()) {
    const auto entry = integers.find(upper(name));
    if(entry!=integers.end())
      return entry->second;
    }
  return std::nullopt;
  }

bool OwnSymbols::setInt(std::string_view name, int32_t value) {
  if(!integers.empty()) {
    const auto entry = integers.find(upper(name));
    if(entry!=integers.end()) {
      entry->second = value;
      return true;
      }
    }
  return false;
  }

daedalus::DaedalusSymbol* OwnSymbols::likeOf(daedalus::DaedalusVm& vm, uint32_t index) {
  const auto* entry = find(index);
  return entry!=nullptr && entry->kind==Kind::Npc ? vm.find_symbol_by_name(entry->like) : nullptr;
  }
std::string OwnSymbols::whyNot(const daedalus::DaedalusVm& vm, const std::string& name) const {
  if(name.size()<=Prefix.size() || name.rfind(Prefix,0)!=0)
    return name + ": a symbol of ours is named " + std::string(Prefix) + "...";
  if(vm.find_symbol_by_name(name)!=nullptr)
    return name + " is taken";
  return {};
  }

daedalus::DaedalusSymbol* OwnSymbols::addInt(daedalus::DaedalusVm& vm, std::string_view name, std::string& why) {
  return add(vm,Entry{Kind::Int,upper(name),{},0},why);
  }

daedalus::DaedalusSymbol* OwnSymbols::addNpc(daedalus::DaedalusVm& vm, std::string_view name, std::string_view like, std::string& why) {
  return add(vm,Entry{Kind::Npc,upper(name),upper(like),0},why);
  }

daedalus::DaedalusSymbol* OwnSymbols::add(daedalus::DaedalusVm& vm, const Entry& e, std::string& why) {
  why = whyNot(vm,e.name);
  if(!why.empty())
    return nullptr;

  daedalus::DaedalusSymbol* s = nullptr;
  if(e.kind==Kind::Int) {
    s = vm.add_int_symbol(e.name);
    if(s!=nullptr)
      s->bind_global_int(integers.try_emplace(e.name,0).first->second);
    }
  else if(e.kind==Kind::Npc) {
    auto* like = vm.find_symbol_by_name(e.like);
    if(like==nullptr || like->type()!=DaedalusDataType::INSTANCE || like->is_generated()) {
      why = e.like + " is not an instance of the scripts";
      return nullptr;
      }
    // of the class the engine has npc's in: what the instance is made like decides its kind
    auto* cls = vm.find_symbol_by_index(like->parent());
    while(cls!=nullptr && cls->type()!=DaedalusDataType::CLASS)
      cls = vm.find_symbol_by_index(cls->parent());
    auto* npc = vm.find_symbol_by_name("C_NPC");
    if(cls==nullptr || npc==nullptr || cls->index()!=npc->index()) {
      why = e.like + " is not an npc";
      return nullptr;
      }
    s = vm.add_instance_symbol(e.name,*like);
    }
  if(s==nullptr) {
    why = "no room for " + e.name;
    return nullptr;
    }

  Entry made = e;
  made.index = s->index();
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
