#include "storycast.h"

#include <Tempest/Log>

#include <cctype>
#include <optional>
#include <vector>

#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include "daedalus/vm.h"

#include "game/gamescript.h"
#include "world/objects/item.h"
#include "world/objects/npc.h"
#include "world/waypoint.h"
#include "world/world.h"

#include "eval.h"
#include "jsonread.h"
#include "textcodec.h"

using namespace Tempest;
using daedalus::DaedalusDataType;
using storygraph::Value;

namespace {

std::string upper(std::string_view s) {
  std::string r(s);
  for(auto& c:r)
    c = char(std::toupper(static_cast<unsigned char>(c)));
  return r;
  }

struct Entry final {
  gtime       start, end;
  uint32_t    state = 0;  // ZS_* function
  std::string waypoint;
  };

struct Description final {
  std::string     like, name;
  const WayPoint* at    = nullptr;
  bool            guilded = false;
  int32_t         guild = 0;
  bool            dressed = false;
  int32_t         gender = 0, face = 0, skin = 0, armor = -1;  // armor: an item instance, -1 none
  std::string     head;
  std::vector<std::pair<uint32_t,int32_t>> items;
  std::vector<Entry> routine;
  };

bool intConst(daedalus::DaedalusVm& vm, std::string_view name, int32_t& v) {
  auto* s = name.empty() ? nullptr : vm.find_symbol_by_name(name);
  if(s==nullptr || !s->is_const() || s->type()!=DaedalusDataType::INT)
    return false;
  v = s->get_int(0);
  return true;
  }

// an instance of the scripts of this class (C_ITEM...)
bool ofClass(daedalus::DaedalusVm& vm, const daedalus::DaedalusSymbol* s, std::string_view cls) {
  if(s==nullptr || s->type()!=DaedalusDataType::INSTANCE)
    return false;
  auto* c = vm.find_symbol_by_index(s->parent());
  while(c!=nullptr && c->type()!=DaedalusDataType::CLASS)
    c = vm.find_symbol_by_index(c->parent());
  auto* want = vm.find_symbol_by_name(cls);
  return c!=nullptr && want!=nullptr && c->index()==want->index();
  }

// "HH:MM"
bool clock(std::string_view s, gtime& t) {
  auto digit = [&](size_t i) { return std::isdigit(static_cast<unsigned char>(s[i])) ? int32_t(s[i]-'0') : -100; };
  if(s.size()!=5 || s[2]!=':')
    return false;
  const int32_t h = digit(0)*10+digit(1), m = digit(3)*10+digit(4);
  if(h<0 || h>23 || m<0 || m>59)
    return false;
  t = gtime(h,m);
  return true;
  }

// everything is checked before anything is made: a half-made npc would stay in the save
std::string read(World& world, const rapidjson::Value& msg, Description& d) {
  auto& vm = world.script().getVm();
  d.like = upper(JsonRead::str(msg,"like"));
  d.name = TextCodec::fromUtf8(JsonRead::str(msg,"name"));
  if(d.name.empty())
    return "needs a name";
  const auto at = JsonRead::str(msg,"at");
  d.at = world.findPoint(at,false);
  if(d.at==nullptr)
    return "no waypoint " + std::string(at);
  if(auto g = upper(JsonRead::str(msg,"guild")); !g.empty()) {
    if(!intConst(vm,g,d.guild))
      return "no guild " + g;
    d.guilded = true;
    }

  if(auto it = msg.FindMember("look"); it!=msg.MemberEnd() && it->value.IsObject()) {
    auto& look = it->value;
    const auto gender = upper(JsonRead::str(look,"gender"));
    const auto face   = upper(JsonRead::str(look,"face"));
    const auto skin   = upper(JsonRead::str(look,"skin"));
    d.head = std::string(JsonRead::str(look,"head"));
    if(!intConst(vm,gender,d.gender))
      return "look: no gender " + gender;
    if(!intConst(vm,face,d.face))
      return "look: no face " + face;
    if(!intConst(vm,skin,d.skin))
      return "look: no skin " + skin;
    if(d.head.empty())
      return "look: needs a head";
    if(auto armor = upper(JsonRead::str(look,"armor")); !armor.empty()) {
      auto* s = vm.find_symbol_by_name(armor);
      if(!ofClass(vm,s,"C_ITEM"))
        return "look: no armor " + armor;
      d.armor = int32_t(s->index());
      }
    if(vm.find_symbol_by_name("B_SETNPCVISUAL")==nullptr)
      return "look: the scripts have no B_SETNPCVISUAL";
    d.dressed = true;
    }

  if(auto it = msg.FindMember("items"); it!=msg.MemberEnd() && it->value.IsArray()) {
    for(auto& x : it->value.GetArray()) {
      const auto item  = upper(JsonRead::str(x,"item"));
      const auto count = JsonRead::integer(x,"count",1);
      auto*      s     = vm.find_symbol_by_name(item);
      if(!ofClass(vm,s,"C_ITEM"))
        return "items: no item " + item;
      if(count<=0 || count>1000)
        return "items: " + item + ": count must be 1..1000";
      d.items.emplace_back(s->index(),int32_t(count));
      }
    }

  if(auto it = msg.FindMember("routine"); it!=msg.MemberEnd() && it->value.IsArray()) {
    for(auto& x : it->value.GetArray()) {
      Entry      e;
      const auto state = upper(JsonRead::str(x,"state"));
      auto*      fn    = vm.find_symbol_by_name(state);
      if(!clock(JsonRead::str(x,"start"),e.start) || !clock(JsonRead::str(x,"end"),e.end))
        return "routine: times are HH:MM";
      if(fn==nullptr || fn->type()!=DaedalusDataType::FUNCTION || state.rfind("ZS_",0)!=0)
        return "routine: no state " + state;
      e.state    = fn->index();
      e.waypoint = std::string(JsonRead::str(x,"waypoint"));
      if(world.findPoint(e.waypoint,false)==nullptr)
        return "routine: no waypoint " + e.waypoint;
      d.routine.push_back(std::move(e));
      }
    if(d.routine.empty())
      return "routine: no entries";
    }
  return {};
  }

std::string answer(std::string_view npc, std::string_view state, uint32_t index) {
  rapidjson::StringBuffer                    buf;
  rapidjson::Writer<rapidjson::StringBuffer> w(buf);
  w.StartObject();
  w.Key("npc");   w.String(npc.data(),rapidjson::SizeType(npc.size()));
  w.Key("state"); w.String(state.data(),rapidjson::SizeType(state.size()));
  w.Key("index"); w.Uint(index);
  w.EndObject();
  return std::string(buf.GetString(),buf.GetSize());
  }

}

std::string StoryCast::npc(World& world, const rapidjson::Value& msg, std::string& error) {
  auto&      script = world.script();
  auto&      vm     = script.getVm();
  auto&      own    = script.ownSymbols();
  const auto id     = upper(JsonRead::str(msg,"npc"));

  if(auto* e = own.find(id)) {
    // made before (the save has it): what became of it stays
    if(e->kind!=OwnSymbols::Kind::Npc) {
      error = id + " is not an npc";
      return {};
      }
    Npc* n = world.findNpcByInstance(e->index);
    return answer(id,n==nullptr ? "absent" : n->isDead() ? "dead" : "there",e->index);
    }

  Description d;
  if(error = read(world,msg,d); !error.empty())
    return {};
  auto* sym = own.addNpc(id,d.like,error);
  if(sym==nullptr)
    return {};
  // at its point, not at the template's: its day is its own
  Npc* npc = world.addNpc(sym->index(),d.at->position());
  if(npc==nullptr) {
    error = "could not put " + id + " in the world";
    return {};
    }
  if(npc->visualSkeleton()==nullptr) {
    // the template's code gave it no body: the world cannot have it (perception needs a head)
    world.removeNpc(*npc);
    error = "the code of " + d.like + " gives " + id + " no body";
    return {};
    }
  npc->setDirection(d.at->direction());

  auto& h = npc->handle();
  h.name[0] = d.name;
  if(d.guilded) {
    h.guild = d.guild;
    npc->setTrueGuild(d.guild);
    }

  // the scripts' own functions, with it as self
  auto* self = vm.global_self();
  auto  prev = self->get_instance();
  self->set_instance(npc->handlePtr());
  auto call = [&](const char* fn, std::vector<Value> args) {
    std::string err;
    auto*       s = vm.find_symbol_by_name(fn);
    if(s==nullptr || !storygraph::callFunction(vm,*s,args,err))
      Log::e("[story] ",id,": ",fn," failed: ",s ? err : "no such function");
    };
  if(d.dressed) {
    // the template's clothes go: B_SetNpcVisual puts the new ones on
    if(Item* old = npc->currentArmor()) {
      const size_t cls = old->clsId();
      npc->unequipItem(cls);
      npc->delItem(cls,1);
      }
    call("B_SETNPCVISUAL",{Value::instance(npc->handlePtr()),Value::integer(d.gender),Value::str(d.head),
                           Value::integer(d.face),Value::integer(d.skin),Value::integer(d.armor)});
    }
  for(auto& [item,count] : d.items)
    call("CREATEINVITEMS",{Value::instance(npc->handlePtr()),Value::integer(int32_t(item)),Value::integer(count)});
  self->set_instance(prev);

  if(!d.routine.empty()) {
    // its own day in place of the template's; it starts on the npc's first tick
    npc->clearRoutine();
    for(auto& e : d.routine)
      npc->addRoutine(e.start,e.end,e.state,e.waypoint);
    }
  Log::i("[story] ",id," made like ",d.like," at ",JsonRead::str(msg,"at"));
  return answer(id,"made",sym->index());
  }
