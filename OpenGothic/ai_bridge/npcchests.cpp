#include "npcchests.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <vector>

#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include "daedalus/vm.h"
#include "daedalus/classes.h"

#include "game/inventory.h"
#include "world/objects/interactive.h"
#include "world/objects/item.h"
#include "world/objects/npc.h"
#include "world/waypoint.h"
#include "world/world.h"

#include "textcodec.h"

namespace {

constexpr float HomeRange = 1500;  // cm: a chest nobody owns this close to a point of its routine is the npc's

std::string upper(std::string_view s) {
  std::string out(s);
  for(auto& c:out)
    c = char(std::toupper(static_cast<unsigned char>(c)));
  return out;
  }

std::string symbolOf(World& world, size_t cls) {
  if(auto sym = world.script().findSymbol(cls))
    return sym->name();
  return {};
  }

std::string npcSymbol(World& world, const Npc& npc) {
  return upper(symbolOf(world,npc.instanceSymbol()));
  }

float dist(const Tempest::Vec3& a, const Tempest::Vec3& b) {
  auto d = a-b;
  return std::sqrt(d.x*d.x+d.y*d.y+d.z*d.z);
  }

bool hasKey(World& world, Npc& npc, Interactive& chest) {
  if(chest.keyName().empty())
    return false;
  const size_t key = world.script().findSymbolIndex(upper(chest.keyName()));
  return key!=size_t(-1) && npc.inventory().itemCount(key)>0;
  }

// an item's name by its instance, as the engine reads it (the symbol keeps the instance it had)
std::string itemName(World& world, std::string_view inst) {
  auto& vm = world.script().getVm();
  auto* s  = vm.find_symbol_by_name(upper(inst));
  if(s==nullptr)
    return {};
  std::string out;
  auto prev = s->get_instance();
  try {
    out = TextCodec::toUtf8(vm.init_instance<daedalus::IItem>(s)->name);
    }
  catch(const std::exception&) {
    }
  s->set_instance(prev);
  return out;
  }

// what the world's data put in the chest at the start: "ItMi_Gold:29, ItRw_Arrow:15"
void writeStart(World& world, Interactive& chest, rapidjson::Writer<rapidjson::StringBuffer>& w) {
  w.StartArray();
  std::string_view rest = chest.startItems();
  while(!rest.empty()) {
    const size_t     comma = rest.find(',');
    std::string_view part  = rest.substr(0,comma);
    rest = comma==std::string_view::npos ? std::string_view() : rest.substr(comma+1);
    while(!part.empty() && std::isspace(static_cast<unsigned char>(part.front())))
      part.remove_prefix(1);
    while(!part.empty() && std::isspace(static_cast<unsigned char>(part.back())))
      part.remove_suffix(1);
    if(part.empty())
      continue;
    const size_t colon = part.find(':');
    const auto   inst  = upper(part.substr(0,colon));
    const long   count = colon==std::string_view::npos ? 1 : std::strtol(std::string(part.substr(colon+1)).c_str(),nullptr,10);
    if(count<=0 || world.script().findSymbolIndex(inst)==size_t(-1))
      continue;
    const auto name = itemName(world,inst);
    w.StartObject();
    w.Key("item");  w.String(inst.data(),rapidjson::SizeType(inst.size()));
    w.Key("name");  w.String(name.data(),rapidjson::SizeType(name.size()));
    w.Key("count"); w.Int64(count);
    w.EndObject();
    }
  w.EndArray();
  }

void writeItems(World& world, Interactive& chest, rapidjson::Writer<rapidjson::StringBuffer>& w) {
  w.StartArray();
  for(auto it = chest.inventory().iterator(Inventory::T_Ransack); it.isValid(); ++it) {
    const auto sym  = symbolOf(world,it->clsId());
    const auto name = TextCodec::toUtf8(it->displayName());
    w.StartObject();
    w.Key("item");  w.String(sym.data(),rapidjson::SizeType(sym.size()));
    w.Key("name");  w.String(name.data(),rapidjson::SizeType(name.size()));
    w.Key("count"); w.Uint64(uint64_t(it.count()));
    w.EndObject();
    }
  w.EndArray();
  }

}

std::string NpcChests::id(Interactive& chest) {
  if(!chest.vobNameStr().empty())
    return upper(chest.vobNameStr());
  const auto p = chest.position();
  return "MOB_" + std::to_string(int(std::lround(p.x))) + "_" + std::to_string(int(std::lround(p.y))) + "_" +
         std::to_string(int(std::lround(p.z)));
  }

Interactive* NpcChests::find(World& world, std::string_view id) {
  const auto want = upper(id);
  for(uint32_t i=0; Interactive* m = world.mobsiById(i); ++i)
    if(m->isContainer() && NpcChests::id(*m)==want)
      return m;
  return nullptr;
  }

std::string NpcChests::whose(World& world, Npc& npc, Interactive& chest) {
  if(!chest.isContainer())
    return {};
  const auto me    = npcSymbol(world,npc);
  const auto owner = upper(chest.ownerName());
  if(!owner.empty() && owner==me)
    return "owner";
  if(hasKey(world,npc,chest))
    return "key";
  if(!owner.empty())
    return {};  // another's
  bool home = false;
  npc.forEachRoutine([&](const Npc::RoutineInfo& r) {
    if(home || r.point.empty())
      return;
    if(auto wp = world.findPoint(r.point))
      home = dist(wp->position(),chest.position())<=HomeRange;
    });
  return home ? "home" : std::string();
  }

std::string NpcChests::items(World& world, Interactive& chest) {
  rapidjson::StringBuffer                    buf;
  rapidjson::Writer<rapidjson::StringBuffer> w(buf);
  writeItems(world,chest,w);
  return std::string(buf.GetString(),buf.GetSize());
  }

std::string NpcChests::of(World& world, Npc& npc) {
  struct Mine { Interactive* chest; std::string why; float d; };
  std::vector<Mine> mine;
  for(uint32_t i=0; Interactive* m = world.mobsiById(i); ++i) {
    if(!m->isContainer())
      continue;
    auto why = whose(world,npc,*m);
    if(!why.empty())
      mine.push_back({m,why,dist(npc.position(),m->position())});
    }
  std::sort(mine.begin(),mine.end(),[](const Mine& a, const Mine& b){ return a.d<b.d; });

  rapidjson::StringBuffer                    buf;
  rapidjson::Writer<rapidjson::StringBuffer> w(buf);
  auto str = [&](const char* key, std::string_view v) {
    w.Key(key);
    w.String(v.data(),rapidjson::SizeType(v.size()));
    };
  w.StartArray();
  for(auto& m:mine) {
    Interactive& c = *m.chest;
    const auto   p = c.position();
    w.StartObject();
    str("id",NpcChests::id(c));
    str("why",m.why);
    w.Key("pos"); w.StartArray(); w.Double(p.x); w.Double(p.y); w.Double(p.z); w.EndArray();
    w.Key("distance"); w.Int(int(m.d));
    if(auto wp = world.findWayPoint(p))
      str("waypoint",wp->name);
    w.Key("locked"); w.Bool(c.isLocked());
    if(!c.keyName().empty())
      str("key",upper(c.keyName()));
    w.Key("has_key"); w.Bool(hasKey(world,npc,c));
    w.Key("items");
    writeItems(world,c,w);
    w.Key("start");
    writeStart(world,c,w);
    w.EndObject();
    }
  w.EndArray();
  return std::string(buf.GetString(),buf.GetSize());
  }
