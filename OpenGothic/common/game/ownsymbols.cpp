#include "ownsymbols.h"

#include <Tempest/Log>

#include <algorithm>
#include <cctype>

#include "game/serialize.h"

using namespace Tempest;
using daedalus::DaedalusDataType;

static constexpr uint8_t SaveFormat = 1;

static std::string upper(std::string_view s) {
  std::string r(s);
  for(auto& c:r)
    c = char(std::toupper(static_cast<unsigned char>(c)));
  return r;
  }

OwnSymbols::OwnSymbols(daedalus::DaedalusVm& vm)
  :vm(vm) {
  }

const OwnSymbols::Entry* OwnSymbols::find(std::string_view name) const {
  const auto up = upper(name);
  for(auto& e:list)
    if(e.name==up)
      return &e;
  return nullptr;
  }

daedalus::DaedalusSymbol* OwnSymbols::likeOf(uint32_t index) {
  for(auto& e:list)
    if(e.index==index)
      return e.kind==Kind::Npc ? vm.find_symbol_by_name(e.like) : nullptr;
  return nullptr;
  }

std::string OwnSymbols::whyNot(const std::string& name) const {
  if(name.size()<=Prefix.size() || name.rfind(Prefix,0)!=0)
    return name + ": a symbol of ours is named " + std::string(Prefix) + "...";
  if(vm.find_symbol_by_name(name)!=nullptr)
    return name + " is taken";
  return {};
  }

daedalus::DaedalusSymbol* OwnSymbols::addInt(std::string_view name, std::string& why) {
  return add(Entry{Kind::Int,upper(name),{},0},why);
  }

daedalus::DaedalusSymbol* OwnSymbols::addNpc(std::string_view name, std::string_view like, std::string& why) {
  return add(Entry{Kind::Npc,upper(name),upper(like),0},why);
  }

daedalus::DaedalusSymbol* OwnSymbols::add(const Entry& e, std::string& why) {
  why = whyNot(e.name);
  if(!why.empty())
    return nullptr;

  daedalus::DaedalusSymbol* s = nullptr;
  if(e.kind==Kind::Int) {
    s = vm.add_int_symbol(e.name);
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

void OwnSymbols::save(Serialize& fout) const {
  fout.write(SaveFormat,uint32_t(list.size()));
  for(auto& e:list)
    fout.write(uint8_t(e.kind),e.name,e.like,e.index);
  }

bool OwnSymbols::load(Serialize& fin) {
  uint8_t  format = 0;
  uint32_t size   = 0;
  fin.read(format,size);
  if(format!=SaveFormat) {
    Log::e("[symbols] a save of format ",int(format),", this game reads ",int(SaveFormat),": none made");
    return false;
    }

  bool same = true;
  for(uint32_t i=0; i<size; ++i) {
    uint8_t kind = 0;
    Entry   e;
    fin.read(kind,e.name,e.like,e.index);
    e.kind = Kind(kind);

    std::string why;
    auto*       s = kind<=uint8_t(Kind::Npc) ? add(e,why) : nullptr;
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
