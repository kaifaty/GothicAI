#include "storycast.h"

#include <Tempest/Log>

#include <cctype>
#include <optional>
#include <vector>

#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include "game/gamescript.h"
#include "world/objects/npc.h"
#include "world/waypoint.h"
#include "world/world.h"

#include "jsonread.h"
#include "textcodec.h"

using namespace Tempest;

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
  std::optional<int32_t> guild;
  std::optional<GameScript::DirectorNpcLook> look;
  std::vector<std::pair<uint32_t,int32_t>> items;
  std::vector<Entry> routine;
  };

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
  auto& script = world.script();
  d.like = upper(JsonRead::str(msg,"like"));
  d.name = TextCodec::fromUtf8(JsonRead::str(msg,"name"));
  if(d.name.empty())
    return "needs a name";
  const auto at = JsonRead::str(msg,"at");
  d.at = world.findPoint(at,false);
  if(d.at==nullptr)
    return "no waypoint " + std::string(at);
  if(auto g = upper(JsonRead::str(msg,"guild")); !g.empty()) {
    d.guild = script.intConstant(g);
    if(!d.guild)
      return "no guild " + g;
    }

  if(auto it = msg.FindMember("look"); it!=msg.MemberEnd() && it->value.IsObject()) {
    auto& look = it->value;
    const auto gender = upper(JsonRead::str(look,"gender"));
    const auto face   = upper(JsonRead::str(look,"face"));
    const auto skin   = upper(JsonRead::str(look,"skin"));
    const auto genderId = script.intConstant(gender);
    if(!genderId)
      return "look: no gender " + gender;
    const auto faceId = script.intConstant(face);
    if(!faceId)
      return "look: no face " + face;
    const auto skinId = script.intConstant(skin);
    if(!skinId)
      return "look: no skin " + skin;
    auto head = std::string(JsonRead::str(look,"head"));
    if(head.empty())
      return "look: needs a head";
    int32_t armorId = -1;
    if(auto armor = upper(JsonRead::str(look,"armor")); !armor.empty()) {
      const auto id = script.findItemInstance(armor);
      if(!id)
        return "look: no armor " + armor;
      armorId = int32_t(*id);
      }
    if(!script.hasDirectorNpcVisual())
      return "look: no prepared visual helper";
    d.look = GameScript::DirectorNpcLook{std::move(head),*genderId,*faceId,*skinId,armorId};
    }

  if(auto it = msg.FindMember("items"); it!=msg.MemberEnd() && it->value.IsArray()) {
    for(auto& x : it->value.GetArray()) {
      const auto item  = upper(JsonRead::str(x,"item"));
      const auto count = JsonRead::integer(x,"count",1);
      const auto id    = script.findItemInstance(item);
      if(!id)
        return "items: no item " + item;
      if(count<=0 || count>1000)
        return "items: " + item + ": count must be 1..1000";
      d.items.emplace_back(*id,int32_t(count));
      }
    }

  if(auto it = msg.FindMember("routine"); it!=msg.MemberEnd() && it->value.IsArray()) {
    for(auto& x : it->value.GetArray()) {
      Entry      e;
      const auto state = upper(JsonRead::str(x,"state"));
      if(!clock(JsonRead::str(x,"start"),e.start) || !clock(JsonRead::str(x,"end"),e.end))
        return "routine: times are HH:MM";
      const auto fn = script.storyScriptFunctionId(state);
      if(!state.starts_with("ZS_") || !fn)
        return "routine: no state " + state;
      e.state    = *fn;
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
  const auto index = script.addDirectorNpcSymbol(id,d.like,error);
  if(!index)
    return {};
  // at its point, not at the template's: its day is its own
  Npc* npc = world.addNpc(*index,d.at->position());
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
  if(d.guild) {
    h.guild = *d.guild;
    npc->setTrueGuild(*d.guild);
    }

  script.setupDirectorNpcEquipment(*npc,d.look ? &*d.look : nullptr,d.items);

  if(!d.routine.empty()) {
    // its own day in place of the template's; it starts on the npc's first tick
    npc->clearRoutine();
    for(auto& e : d.routine)
      npc->addRoutine(e.start,e.end,e.state,e.waypoint);
    }
  Log::i("[story] ",id," made like ",d.like," at ",JsonRead::str(msg,"at"));
  return answer(id,"made",*index);
  }
