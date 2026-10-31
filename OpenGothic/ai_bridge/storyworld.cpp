#include "storyworld.h"

#include <Tempest/Log>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <map>

#include <zenkit/DaedalusVm.hh>
#include <zenkit/addon/daedalus.hh>

#include "game/inventory.h"
#include "physics/dynamicworld.h"
#include "world/objects/interactive.h"
#include "world/objects/item.h"
#include "world/objects/npc.h"
#include "world/world.h"

#include "eval.h"
#include "chesttake.h"
#include "jsonread.h"
#include "jsonwriter.h"
#include "npcfind.h"
#include "npcchests.h"
#include "npcsteal.h"
#include "npcview.h"
#include "storyeffects.h"
#include "textcodec.h"

using namespace Tempest;

namespace {

// the npc goes with the hero: a companion's dialog, or the server's `party` effect (v30), marked it
bool partyMember(World& world, const Npc& npc) {
  auto* s = world.script().getVm().find_symbol_by_name("AIV_PARTYMEMBER");
  if(s==nullptr || s->type()!=zenkit::DaedalusDataType::INT)
    return false;
  const int32_t aiv = s->get_int(0);
  return aiv>=0 && uint32_t(aiv)<zenkit::INpc::aivar_count && npc.handlePtr()->aivar[size_t(aiv)]!=0;
  }

constexpr uint64_t ScanMs   = 500;   // how often deaths are looked for
constexpr uint64_t ReportMs = 1000;  // how often tags are reported

std::string upper(std::string_view s) {
  std::string r(s);
  for(auto& c:r)
    c = char(std::toupper(static_cast<unsigned char>(c)));
  return r;
  }

std::string symbolOf(World& world, const Npc& npc) {
  if(auto sym = world.script().findSymbol(npc.instanceSymbol()))
    return sym->name();
  return {};
  }

Npc* bySymbol(World& world, std::string_view symbol) {
  const size_t id = world.script().findSymbolIndex(upper(symbol));
  return id==size_t(-1) ? nullptr : world.findNpcByInstance(id);
  }

float distance(const Npc& a, const Npc& b) {
  auto d = a.position()-b.position();
  return std::sqrt(d.x*d.x+d.y*d.y+d.z*d.z);
  }

// the class an instance (or prototype) is of: C_NPC, C_ITEM, C_INFO...
std::string classOf(zenkit::DaedalusVm& vm, const zenkit::DaedalusSymbol& sym) {
  auto* s = &sym;
  for(int i=0; i<8 && s!=nullptr && s->type()!=zenkit::DaedalusDataType::CLASS; ++i)
    s = vm.find_symbol_by_index(s->parent());
  return s!=nullptr && s->type()==zenkit::DaedalusDataType::CLASS ? std::string(s->name()) : std::string();
  }

std::string kindOf(zenkit::DaedalusVm& vm, const zenkit::DaedalusSymbol& sym) {
  using zenkit::DaedalusDataType;
  if(sym.is_member())
    return "other";
  switch(sym.type()) {
    case DaedalusDataType::INSTANCE: {
      const auto cls = classOf(vm,sym);
      return cls=="C_NPC" ? "npc" : cls=="C_ITEM" ? "item" : cls=="C_INFO" ? "info" : "other";
      }
    case DaedalusDataType::INT:
      return sym.is_const() ? "const" : "var";
    case DaedalusDataType::FUNCTION:
      return "func";
    default:
      return "other";
    }
  }

template<class F>
void eachString(const rapidjson::Value& msg, const char* key, F f) {
  auto it = msg.FindMember(key);
  if(it==msg.MemberEnd() || !it->value.IsArray())
    return;
  for(auto& v : it->value.GetArray())
    if(v.IsString())
      f(upper(std::string_view(v.GetString(),v.GetStringLength())));
  }

}

StoryWorld::StoryWorld(std::function<void(std::string&&)> send, std::function<void(World&,Npc&)> release)
  :send(std::move(send)), release(std::move(release)) {
  }

void StoryWorld::clear() {
  tags.clear();
  deaths.clear();
  nextScan = nextReport = 0;
  }

int StoryWorld::spawn(World& world, const rapidjson::Value& msg, std::string& error) {
  Npc*       pl    = world.player();
  const auto inst  = upper(JsonRead::str(msg,"instance"));
  const auto tag   = std::string(JsonRead::str(msg,"tag"));
  const auto near  = JsonRead::str(msg,"near");
  const auto count = std::clamp<int64_t>(JsonRead::integer(msg,"count",1),1,20);
  const size_t id  = world.script().findSymbolIndex(inst);
  Npc*       at    = near.empty() ? pl : bySymbol(world,near);
  if(id==size_t(-1) || at==nullptr || pl==nullptr || tag.empty()) {
    error = tag.empty() ? "no tag" : id==size_t(-1) ? "unknown instance " + inst : "unknown npc to spawn near";
    return 0;
    }
  if(auto name = JsonRead::str(msg,"waypoint"); !name.empty()) {
    // where the scripts would put them, as Wld_InsertNpc does
    if(world.findPoint(name)==nullptr) {
      error = "unknown waypoint " + std::string(name);
      return 0;
      }
    int made = 0;
    for(int64_t i=0; i<count; ++i)
      if(Npc* n = world.addNpc(id,name)) {
        tags[tag].push_back(n);
        made++;
        }
    Log::i("[story] spawned ",made," ",inst," at ",name," as ",tag);
    if(made==0)
      error = "could not spawn";
    return made;
    }
  // in a row across the way from the npc to the player, so they stand on ground both have walked
  const float dist = float(JsonRead::integer(msg,"distance",600));
  auto        base = at->position();
  float       dx   = std::cos(at->rotationRad()), dz = std::sin(at->rotationRad());
  if(at!=pl) {
    const auto  d   = pl->position()-base;
    const float len = std::sqrt(d.x*d.x+d.z*d.z);
    if(len>1) {
      dx = d.x/len;
      dz = d.z/len;
      }
    }
  int made = 0;
  for(int64_t i=0; i<count; ++i) {
    auto pos   = base;
    float side = float(i-count/2)*150.f;
    pos.x += dist*dx - side*dz;
    pos.z += dist*dz + side*dx;
    if(Npc* n = world.addNpc(id,pos)) {
      tags[tag].push_back(n);
      made++;
      }
    }
  Log::i("[story] spawned ",made," ",inst," as ",tag);
  if(made==0)
    error = "could not spawn";
  return made;
  }

int StoryWorld::tag(World& world, const rapidjson::Value& msg) {
  const auto t     = std::string(JsonRead::str(msg,"tag"));
  int        found = 0;
  if(t.empty())
    return 0;
  auto& list = tags[t];
  eachString(msg,"npcs",[&](const std::string& name) {
    if(Npc* n = bySymbol(world,name)) {
      if(std::find(list.begin(),list.end(),n)==list.end())
        list.push_back(n);
      found++;
      }
    });
  Log::i("[story] tagged ",found," npc(s) as ",t);
  return found;
  }

void StoryWorld::watch(const rapidjson::Value& msg) {
  auto& list = watchers[std::string(JsonRead::str(msg,"owner"))];
  list.clear();
  eachString(msg,"npcs",[&](const std::string& name) { list.push_back(name); });
  }

bool StoryWorld::effects(World& world, const rapidjson::Value& msg, std::string& error) {
  Npc* npc = bySymbol(world,JsonRead::str(msg,"npc"));
  Npc* pl  = world.player();
  if(npc==nullptr || pl==nullptr) {
    error = "no npc " + std::string(JsonRead::str(msg,"npc"));
    return false;
    }
  auto& vm = world.script().getVm();
  std::vector<StoryEffect> list;
  if(auto it = msg.FindMember("effects"); it!=msg.MemberEnd() && it->value.IsArray()) {
    for(auto& x : it->value.GetArray()) {
      StoryEffect e;
      error = StoryEffect::read(x,&vm,e);
      if(!error.empty())
        return false;
      list.push_back(std::move(e));
      }
    }
  std::vector<ChestTake::Request> takes;
  for(auto& e : list) {
    if(e.kind=="chest_take")
      takes.push_back({e.chest,e.item,e.count});
    // v39: a mission item (ITEM_MISSION) never goes from one npc to another
    const size_t id = e.kind=="give" && e.toNpc ? world.script().findSymbolIndex(e.item) : size_t(-1);
    if(auto* it = id==size_t(-1) ? nullptr : const_cast<Inventory&>(npc->inventory()).getItem(id); it!=nullptr && it->isMission()) {
      error = "a mission item: " + e.item;
      return false;
      }
    }
  // Mandatory for chest_take even without check:true: no partial take, no mission stack, and no
  // earlier effect may run before every requested source stack passes preflight.
  error = ChestTake::check(takes,[&](const std::string& chest, const std::string& item)
      -> std::optional<ChestTake::Stock> {
    auto* source = NpcChests::find(world,chest);
    if(source==nullptr)
      return std::nullopt;
    const auto id = world.script().findSymbolIndex(item);
    auto* stock = source->inventory().getItem(id);
    return ChestTake::Stock{int64_t(source->inventory().itemCount(id)),stock!=nullptr && stock->isMission()};
    });
  if(!error.empty())
    return false;
  if(JsonRead::boolean(msg,"check",false)) {
    // all or nothing (v10, a deal): each giver must have all it gives, or nothing happens; what the npc
    // creates (v17, its stash) is its own to give, and the hero must have the learning points of all
    // that is taught
    std::map<std::pair<bool,std::string>,int64_t> need;  // (the hero gives, item) -> count
    std::map<std::string,int64_t>                 made;  // item -> created for the npc
    int64_t                                       lp = 0;
    for(auto& e : list) {
      if(e.kind=="give")
        need[{!e.toHero && !e.toNpc,e.item}] += e.count;
      else if(e.kind=="create")
        made[e.item] += e.count;
      else if(e.kind=="teach") {
        const int32_t cost = e.learnCost(vm,*pl,error);
        if(cost<0)
          return false;
        lp += cost;
        }
      }
    for(auto& [k,count] : need) {
      Npc&         giver = k.first ? *pl : *npc;
      const size_t id    = world.script().findSymbolIndex(k.second);
      const auto   has   = int64_t(id==size_t(-1) ? 0 : giver.itemCount(id)) + (k.first ? 0 : made[k.second]);
      if(has<count) {
        error = "missing: " + std::string(k.first ? "hero " : "npc ") + k.second + " " + std::to_string(count) +
                " (has " + std::to_string(has) + ")";
        return false;
        }
      }
    if(lp>pl->learningPoints()) {
      error = "missing: hero LP " + std::to_string(lp) + " (has " + std::to_string(pl->learningPoints()) + ")";
      return false;
      }
    }
  const auto who = symbolOf(world,*npc);
  for(auto& e : list) {
    if(e.kind=="attack" || e.kind=="refuse_talk" || e.kind=="calm")
      release(world,*npc);  // the scripts carry these out: an npc the server holds is let go first
    e.run(vm,*npc,*pl,who);
    }
  return true;
  }

std::string StoryWorld::nearby(World& world, const rapidjson::Value& msg) {
  Npc*        at     = bySymbol(world,JsonRead::str(msg,"npc"));
  const float radius = float(JsonRead::integer(msg,"radius",2000));
  if(at==nullptr)
    return {};
  rapidjson::StringBuffer                    buf;
  rapidjson::Writer<rapidjson::StringBuffer> w(buf);
  w.StartArray();
  for(uint32_t i=0; i<world.npcCount(); ++i) {
    Npc* n = world.npcById(i);
    if(n==nullptr || n==at || n->isPlayer() || n->isDead() || n->isMonster())
      continue;
    const float d = distance(*at,*n);
    if(d>radius)
      continue;
    auto sym  = symbolOf(world,*n);
    auto name = TextCodec::toUtf8(n->displayName());
    w.StartObject();
    w.Key("npc");      w.String(sym.data(),rapidjson::SizeType(sym.size()));
    w.Key("name");     w.String(name.data(),rapidjson::SizeType(name.size()));
    w.Key("distance"); w.Int(int(d));
    w.Key("guild");    w.Uint(n->guild());
    w.EndObject();
    }
  w.EndArray();
  return std::string(buf.GetString(),buf.GetSize());
  }

std::string StoryWorld::state(World& world, const rapidjson::Value& msg) {
  using zenkit::DaedalusDataType;
  auto&  vm = world.script().getVm();
  Npc*   pl = world.player();
  rapidjson::StringBuffer                    buf;
  rapidjson::Writer<rapidjson::StringBuffer> w(buf);
  auto key = [&](const std::string& k) { w.Key(k.data(),rapidjson::SizeType(k.size())); };
  w.StartObject();

  w.Key("vars");
  w.StartObject();
  eachString(msg,"vars",[&](const std::string& name) {
    auto* s = vm.find_symbol_by_name(name);
    if(s!=nullptr && s->type()==DaedalusDataType::INT && !s->is_member()) {
      key(name);
      w.Int(s->get_int(0));
      }
    });
  w.EndObject();

  w.Key("npcs");
  w.StartObject();
  Npc* near = bySymbol(world,JsonRead::str(msg,"near"));
  eachString(msg,"npcs",[&](const std::string& name) {
    Npc* n = bySymbol(world,name);
    key(name);
    w.StartObject();
    w.Key("state");
    w.String(n==nullptr ? "absent" : n->isDead() ? "dead" : "alive");
    if(n!=nullptr) {
      auto display = TextCodec::toUtf8(n->displayName());
      w.Key("name");
      w.String(display.data(),rapidjson::SizeType(display.size()));
      // v48: authoritative positions for a batch of far NPCs, without running their bodies.
      const auto pos = n->position();
      w.Key("pos");
      w.StartArray(); w.Double(pos.x); w.Double(pos.y); w.Double(pos.z); w.EndArray();
      }
    if(n!=nullptr && near!=nullptr) {
      // alive or dead: how far an npc would go to see for itself, and whether it sees it from here (v12)
      w.Key("distance");
      w.Int(int(distance(*near,*n)));
      w.Key("seen");
      w.Bool(near!=n && npcSees(*near,*n));
      }
    w.EndObject();
    });
  w.EndObject();

  w.Key("knows");
  w.StartObject();
  auto* knows = vm.find_symbol_by_name("NPC_KNOWSINFO");
  eachString(msg,"knows",[&](const std::string& name) {
    auto*       s = vm.find_symbol_by_name(name);
    std::string err;
    auto        r = s!=nullptr && pl!=nullptr && knows!=nullptr
                    ? storygraph::callFunction(vm,*knows,{storygraph::Value::instance(pl->handlePtr()),
                                                          storygraph::Value::integer(int32_t(s->index()))},err)
                    : std::nullopt;
    if(r && r->type==storygraph::Value::Int) {
      key(name);
      w.Bool(r->i!=0);
      }
    });
  w.EndObject();

  w.Key("symbols");
  w.StartObject();
  eachString(msg,"symbols",[&](const std::string& name) {
    if(auto* s = vm.find_symbol_by_name(name)) {
      key(name);
      auto kind = kindOf(vm,*s);
      w.String(kind.data(),rapidjson::SizeType(kind.size()));
      }
    });
  w.EndObject();

  w.Key("items");
  w.StartObject();
  eachString(msg,"items",[&](const std::string& name) {
    auto* s = vm.find_symbol_by_name(name);
    if(s==nullptr || kindOf(vm,*s)!="item")
      return;
    // the engine reads names the same way (the gold's); the symbol keeps the instance it had
    auto prev = s->get_instance();
    try {
      auto item = vm.init_instance<zenkit::IItem>(s);
      auto text = TextCodec::toUtf8(item->name);
      key(name);
      w.String(text.data(),rapidjson::SizeType(text.size()));
      }
    catch(const std::exception&) {
      }
    s->set_instance(prev);
    });
  w.EndObject();

  if(auto target = msg.FindMember("deed_target"); target!=msg.MemberEnd() && target->value.IsObject()) {
    const auto preview = NpcSteal::preview(world,target->value);
    w.Key("deed_target");
    w.RawValue(preview.data(),preview.size(),rapidjson::kObjectType);
    }

  const auto now = world.time();
  w.Key("dialog");   w.Bool(world.isInDialog());
  w.Key("cutscene"); w.Bool(world.currentCs()!=nullptr);
  w.Key("day");      w.Int64(now.day());
  w.Key("hour");     w.Int64(now.hour());
  w.Key("minute");   w.Int64(now.minute());
  w.EndObject();
  return std::string(buf.GetString(),buf.GetSize());
  }

std::string StoryWorld::people(World& world) {
  rapidjson::StringBuffer                    buf;
  rapidjson::Writer<rapidjson::StringBuffer> w(buf);
  auto str = [&](std::string_view v) { w.String(v.data(),rapidjson::SizeType(v.size())); };
  w.StartArray();
  for(uint32_t i=0; i<world.npcCount(); ++i) {
    Npc* n = world.npcById(i);
    if(n==nullptr || n->isPlayer() || n->isDead() || n->isMonster())
      continue;
    auto sym  = symbolOf(world,*n);
    auto name = TextCodec::toUtf8(n->displayName());
    w.StartObject();
    w.Key("npc");   str(sym);
    w.Key("name");  str(name);
    w.Key("guild"); w.Uint(n->guild());
    std::string homeRoom;
    w.Key("routine");
    w.StartArray();
    n->forEachRoutine([&](const Npc::RoutineInfo& r) {
      w.StartObject();
      w.Key("start");    w.Int(int(r.start.hour()*60+r.start.minute()));
      w.Key("end");      w.Int(int(r.end.hour()*60+r.end.minute()));
      const auto state = AiNpc::stateName(world,r.state);
      w.Key("state");    str(state);
      w.Key("waypoint"); str(r.point);
      if(auto* wp = world.findPoint(r.point,false)) {
        auto p = wp->position();
        if(state=="ZS_GOTOBED" || state=="ZS_SLEEP") {
          // TA_Sleep schedules ZS_GotoBed. Use the same ground sector as the
          // actor's portalName; a bed can hide it, so retain the BSP room then.
          const auto ground = world.physic()->landRay(p+Tempest::Vec3(0,50,0),500);
          homeRoom = ground.hasCol && ground.sector!=nullptr && ground.sector[0]!='\0' ?
                     std::string_view(ground.sector) : world.roomAt(p);
          }
        w.Key("pos");
        w.StartArray(); w.Int(int(p.x)); w.Int(int(p.y)); w.Int(int(p.z)); w.EndArray();
        }
      w.EndObject();
      });
    w.EndArray();
    w.Key("home_room"); str(homeRoom);
    w.EndObject();
    }
  w.EndArray();
  return std::string(buf.GetString(),buf.GetSize());
  }

void StoryWorld::tick(World& world) {
  const uint64_t now = world.tickCount();
  if(now>=nextScan || nextScan>now+ScanMs) {
    nextScan = now+ScanMs;
    // deaths: of people (a quest giver, a witness) and of the director's creatures
    std::unordered_set<const Npc*> tagged;
    for(auto& [tag,list] : tags)
      for(auto* n : list)
        tagged.insert(n);
    for(auto& death : deaths.look(world)) {
      Npc* n  = death.npc;
      Npc* by = death.killer;
      if(by!=nullptr && (by->isPlayer() || partyMember(world,*by))) {
        // whoever the hero killed, person or creature (v17): an npc's contract may count it. One of his
        // companions killed counts too (v30), as the scripts give him the experience for it (ZS_Dead)
        JsonWriter k("hero_kill");
        k.str("npc",symbolOf(world,*n)).str("name",TextCodec::toUtf8(n->displayName()))
         .integer("guild",int64_t(n->guild())).boolean("monster",n->isMonster());
        send(k.finish());
        }
      if(n->isMonster() && !tagged.count(n))
        continue;
      std::string tag;
      for(auto& [t,list] : tags)
        if(std::find(list.begin(),list.end(),n)!=list.end())
          tag = t;
      JsonWriter j("npc_died");
      j.str("npc",symbolOf(world,*n)).str("name",TextCodec::toUtf8(n->displayName()));
      if(!tag.empty())
        j.str("tag",tag);
      j.str("killer",by==nullptr ? std::string() : by->isPlayer() ? std::string("PLAYER") : symbolOf(world,*by));
      send(j.finish());
      }
    }
  if(!tags.empty() && (now>=nextReport || nextReport>now+ReportMs)) {
    nextReport = now+ReportMs;
    report(world);
    }
  }

void StoryWorld::report(World& world) {
  JsonWriter j("story_world");
  j.array("tags");
  for(auto& [tag,list] : tags) {
    int alive = 0;
    for(auto* n : list)
      alive += !n->isDead();
    j.object().str("tag",tag).integer("alive",alive).integer("dead",int64_t(list.size())-alive);
    std::set<std::string> all;
    for(auto& [owner,list] : watchers)
      all.insert(list.begin(),list.end());
    j.array("near");
    for(auto& w : all) {
      Npc* who = bySymbol(world,w);
      if(who==nullptr)
        continue;
      // the nearest living one, and the nearest it sees (v12)
      float best = std::numeric_limits<float>::max(), seen = best;
      for(auto* n : list)
        if(!n->isDead()) {
          const float d = distance(*who,*n);
          best = std::min(best,d);
          if(d<seen && npcSees(*who,*n))
            seen = d;
          }
      if(best<std::numeric_limits<float>::max()) {
        j.object().str("npc",w).integer("distance",int64_t(best));
        if(seen<std::numeric_limits<float>::max())
          j.integer("seen",int64_t(seen));
        j.end();
        }
      }
    j.endArray();
    j.end();
    }
  j.endArray();
  send(j.finish());
  }
