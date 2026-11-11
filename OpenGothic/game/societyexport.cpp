#include "societyexport.h"

#include "enginecommit.h"
#include "resources.h"
#include "game/gamescript.h"
#include "game/gamesession.h"
#include "physics/dynamicworld.h"
#include "world/objects/interactive.h"
#include "world/objects/npc.h"
#include "world/waypoint.h"
#include "world/world.h"
#include "../ai_bridge/textcodec.h"

#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <optional>
#include <set>
#include <stdexcept>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace SocietyExport {
namespace {

using Buffer = rapidjson::StringBuffer;
using Writer = rapidjson::Writer<Buffer>;

std::string upper(std::string_view s) {
  std::string ret(s);
  for(auto& c:ret)
    c = char(std::toupper(static_cast<unsigned char>(c)));
  return ret;
  }

// Game strings are Windows-1251; the file is UTF-8 (as the AI bridge's).
void text(Writer& w, std::string_view game) {
  const auto utf8 = TextCodec::toUtf8(game);
  w.String(utf8.data(),rapidjson::SizeType(utf8.size()));
  }

void ascii(Writer& w, std::string_view s) {
  w.String(s.data(),rapidjson::SizeType(s.size()));
  }

// Fixed precision: the same float always prints the same, and 0.1 cm is far below anything the stand needs.
void number(Writer& w, double v) {
  char buf[64];
  const int n = std::snprintf(buf,sizeof(buf),"%.1f",v);
  w.RawValue(buf,size_t(n),rapidjson::kNumberType);
  }

void position(Writer& w, const Tempest::Vec3& p) {
  w.StartArray();
  number(w,double(p.x));
  number(w,double(p.y));
  number(w,double(p.z));
  w.EndArray();
  }

void optionalId(Writer& w, const std::optional<uint32_t>& id) {
  if(id)
    w.Uint(*id); else
    w.Null();
  }

void optionalText(Writer& w, std::string_view s) {
  if(s.empty())
    w.Null(); else
    text(w,s);
  }

// As the npc-day journal names script symbols (common/npcdayjournal.cpp): routine states, NPC instances.
std::string symbolName(World& world, size_t symbol) {
  if(symbol==size_t(-1))
    return "-";
  auto* sym = world.script().findSymbol(symbol);
  return sym!=nullptr ? std::string(sym->name()) : std::to_string(symbol);
  }

bool npcSymbol(World& world, daedalus::DaedalusSymbol* s) {
  if(s==nullptr)
    return false;
  auto* cls = s;
  for(size_t i=0; i<16 && cls!=nullptr && cls->type()!=daedalus::DaedalusDataType::CLASS; ++i)
    cls = world.script().findSymbol(size_t(cls->parent()));
  return cls!=nullptr && cls->name()=="C_NPC";
  }

std::vector<std::string> npcInstances(World& world) {
  std::vector<std::string> ret;
  for(const auto& s:world.script().getVm().symbols())
    if(s.type()==daedalus::DaedalusDataType::INSTANCE && s.is_const() &&
       npcSymbol(world,world.script().findSymbol(s.index())))
      ret.push_back(s.name());
  std::sort(ret.begin(),ret.end());
  return ret;
  }

struct RoomOwner {
  std::string npc, name, ambiguous;
  };

RoomOwner resolveOwner(World& world, std::string_view name, const std::vector<std::string>& instances) {
  auto* s = world.script().findSymbol(name);
  if(s!=nullptr && s->type()==daedalus::DaedalusDataType::INSTANCE && npcSymbol(world,s)) {
    // The alias's handle carries the actual C_NPC instance, rather than the variable's symbol index.
    if(s->is_instance_of<daedalus::INpc>()) {
      const auto id = symbolName(world,s->get_instance()->symbol_index());
      if(std::binary_search(instances.begin(),instances.end(),id))
        return {id,std::string(name),{}};
      }
    if(s->is_const())
      return {s->name(),std::string(name),{}};
    }
  RoomOwner ret;
  for(const auto& id:instances) {
    // A short name omits the prefix and numeric character id (VLK_409_ZURIS).
    // An archetype such as GIANT_BUG does not have the short name BUG.
    const std::string_view full = id;
    const auto first = full.find('_');
    const auto second = first==std::string_view::npos ? first : full.find('_',first+1);
    if(first==0 || second==std::string_view::npos || second==first+1)
      continue;
    const auto serial = full.substr(first+1,second-first-1);
    if(!std::all_of(serial.begin(),serial.end(),[](char c) { return '0'<=c && c<='9'; }) || full.substr(second+1)!=name)
      continue;
    if(ret.npc.empty())
      ret.npc = id;
    else {
      if(ret.ambiguous.empty())
        ret.ambiguous = ret.npc;
      ret.ambiguous += ", "+id;
      }
    }
  if(!ret.npc.empty())
    ret.name = name;
  if(!ret.ambiguous.empty())
    ret.npc.clear();
  return ret;
  }

// As HeroDeed, prefer the longest owner name after ':'; canonicalize aliases and unique short names for the stand.
RoomOwner resolveRoomOwner(World& world, std::string_view room, const std::vector<std::string>& instances) {
  const auto colon = room.find(':');
  const auto name = colon==std::string_view::npos ? room : room.substr(colon+1);
  for(size_t end=name.size(); end!=std::string_view::npos;) {
    auto owner = resolveOwner(world,name.substr(0,end),instances);
    if(!owner.npc.empty() || !owner.ambiguous.empty())
      return owner;
    if(end==0)
      break;
    end = name.rfind('_',end-1);
    }
  return {{},std::string(name.substr(0,name.find('_'))),{}};
  }

struct RoomGuild {
  int32_t id = -1;
  std::string name, unresolved;
  };

RoomGuild resolveRoomGuild(World& world, std::string_view room, bool npcOwner) {
  const auto colon = room.find(':');
  // guildOfRoom expects portal syntax even when roomAt returned a bare BSP sector name.
  const auto native = colon==std::string_view::npos ? "S:"+std::string(room)+"_" : std::string(room);
  const auto assigned = world.guildOfRoom(native);
  // PAL_200_HAGEN names a private NPC, not the guild GIL_PAL.
  if(npcOwner)
    return {assigned,{},{}};
  const auto name = colon==std::string_view::npos ? room : room.substr(colon+1);
  auto* max = world.script().findSymbol("GIL_MAX");
  for(size_t end=name.size(); end!=std::string_view::npos;) {
    const auto part = name.substr(0,end);
    const auto key = part.starts_with("GIL_") ? std::string(part) : "GIL_"+std::string(part);
    auto* sym = world.script().findSymbol(key);
    if(sym!=nullptr && sym->type()==daedalus::DaedalusDataType::INT && sym->is_const() &&
       sym->get_int()>=0 && max!=nullptr && sym->get_int()<max->get_int())
      return {sym->get_int(),key,{}};
    if(end==0)
      break;
    end = name.rfind('_',end-1);
    }
  if(name.starts_with("GIL_"))
    return {-1,{},std::string(name)};
  // Otherwise this is a native room assignment (Wld_AssignRoomToGuild), already a numeric script guild.
  return {assigned,{},{}};
  }

std::string roomAt(World& world, const Tempest::Vec3& pos) {
  // HeroDeed's ownership-position query: native ground first, BSP sector as fallback. Only S:/P:
  // materials name rooms; an ordinary outdoor ground material must not become a room in the export.
  const auto ground = world.physic()->landRay(pos+Tempest::Vec3(0,50,0),500);
  const std::string_view sector = ground.hasCol && ground.sector!=nullptr ? ground.sector : "";
  if(sector.starts_with("S:") || sector.starts_with("P:"))
    return std::string(sector);
  return std::string(world.roomAt(pos));
  }

int minuteOfDay(const gtime& t) {
  return int(t.hour()*60+t.minute());
  }

double median(std::vector<double> v) {
  if(v.empty())
    return 0;
  std::sort(v.begin(),v.end());
  return v[v.size()/2];
  }

// The monster schedule (G2 scripts): ZS_MM_AllScheduler starts the first of these states whose aivar hours hold the
// time of day (Wld_IsTime(start,0,end,0)) or whose start is OnlyRoutine (-1). The export names the state and carries
// the two aivar values; the order is the order of the scheduler's checks.
struct MmWindow { const char* state; const char* start; const char* end; };
constexpr MmWindow MmSchedule[] = {
  {"ZS_MM_RTN_SLEEP",     "AIV_MM_SLEEPSTART",     "AIV_MM_SLEEPEND"},
  {"ZS_MM_RTN_REST",      "AIV_MM_RESTSTART",      "AIV_MM_RESTEND"},
  {"ZS_MM_RTN_ROAM",      "AIV_MM_ROAMSTART",      "AIV_MM_ROAMEND"},
  {"ZS_MM_RTN_EATGROUND", "AIV_MM_EATGROUNDSTART", "AIV_MM_EATGROUNDEND"},
  {"ZS_MM_RTN_WUSEL",     "AIV_MM_WUSELSTART",     "AIV_MM_WUSELEND"},
  {"ZS_MM_RTN_ORCSIT",    "AIV_MM_ORCSITSTART",    "AIV_MM_ORCSITEND"},
  };
constexpr const char* MmScheduler = "ZS_MM_ALLSCHEDULER";

// The aivar index a script constant names; the export fails when the scripts have no such constant.
size_t aivarIndex(World& world, const char* name) {
  auto* sym = world.script().findSymbol(name);
  if(sym==nullptr || sym->get_int()<0 || size_t(sym->get_int())>=daedalus::INpc::aivar_count)
    throw std::runtime_error(std::string("society export: the scripts have no aivar constant ")+name);
  return size_t(sym->get_int());
  }

struct Records {
  std::string out;
  size_t      count = 0;

  void add(const Buffer& b) {
    out += count++==0 ? "\n" : ",\n";
    out.append(b.GetString(),b.GetSize());
    }
  };

}

Result build(World& world) {
  // Points: the waynet's waypoints in ZEN order, then the freepoints (World::forEachWayPoint). Start points
  // (START_*) are not points of the waynet and stay out, as in the AI bridge's `waypoints`.
  std::vector<const WayPoint*> points;
  world.forEachWayPoint([&](const WayPoint& w) { points.push_back(&w); });
  std::unordered_map<const WayPoint*,uint32_t> pointId;
  pointId.reserve(points.size());
  for(size_t i=0; i<points.size(); ++i)
    pointId.emplace(points[i],uint32_t(i));
  const auto idOf = [&](const WayPoint* p) -> std::optional<uint32_t> {
    if(p==nullptr)
      return std::nullopt;
    auto it = pointId.find(p);
    return it!=pointId.end() ? std::optional<uint32_t>(it->second) : std::nullopt;
    };
  // The nearest waynet point (not a freepoint): World::findWayPoint, the engine's own nearest search.
  const auto nearestGraph = [&](const Tempest::Vec3& at) -> uint32_t {
    auto id = idOf(world.findWayPoint(at));
    if(!id || points[*id]->isFreePoint())
      throw std::runtime_error("society export: the world has no waynet point near a position");
    return *id;
    };

  Records waypoints, edges, objects, npcs, rooms;
  std::set<std::string> roomNames;
  const auto writeRoom = [&](Writer& w, const Tempest::Vec3& at) {
    const auto room = roomAt(world,at);
    optionalText(w,room);
    if(!room.empty())
      roomNames.insert(room);
    };
  size_t freepoints = 0;
  for(size_t i=0; i<points.size(); ++i) {
    const auto& p  = *points[i];
    const bool  fp = p.isFreePoint();
    Buffer b; Writer w(b);
    w.StartObject();
    w.Key("id");            w.Uint(uint32_t(i));
    w.Key("name");          text(w,p.name);
    w.Key("pos");           position(w,p.position());
    w.Key("room");          writeRoom(w,p.position());
    w.Key("freepoint");     w.Bool(fp);
    w.Key("near_waypoint"); optionalId(w,fp ? std::optional<uint32_t>(nearestGraph(p.position())) : std::nullopt);
    w.EndObject();
    waypoints.add(b);
    freepoints += fp ? 1 : 0;
    }

  // Edges as the engine walks them: each point's connections (WayPoint::connect, both directions) with the
  // integer length WayMatrix::wayTo sums; a repeated pair of the ZEN once.
  std::vector<std::tuple<uint32_t,uint32_t,int32_t>> edgeList;
  for(size_t i=0; i<points.size(); ++i)
    for(const auto& c:points[i]->connections())
      if(auto to = idOf(c.point))
        edgeList.emplace_back(uint32_t(i),*to,c.len);
  std::sort(edgeList.begin(),edgeList.end());
  edgeList.erase(std::unique(edgeList.begin(),edgeList.end()),edgeList.end());
  for(const auto& [a,b,len]:edgeList) {
    Buffer buf; Writer w(buf);
    w.StartArray(); w.Uint(a); w.Uint(b); w.Int(len); w.EndArray();
    edges.add(buf);
    }

  // Mobsi in ZEN order (vob id), then by position for objects without one.
  struct Mob { Interactive* mob; uint32_t zen; Tempest::Vec3 pos; std::string vob; };
  std::vector<Mob> mobs;
  for(uint32_t i=0; auto* m = world.mobsiById(i); ++i)
    mobs.push_back({m,m->zenId(),m->position(),std::string(m->vobNameStr())});
  std::sort(mobs.begin(),mobs.end(),[](const Mob& a, const Mob& b) {
    return std::tie(a.zen,a.pos.x,a.pos.y,a.pos.z,a.vob)<std::tie(b.zen,b.pos.x,b.pos.y,b.pos.z,b.vob);
    });
  size_t containers = 0, owned = 0;
  for(size_t i=0; i<mobs.size(); ++i) {
    auto& m = *mobs[i].mob;
    const auto ownerNpc   = upper(m.ownerName());
    const auto ownerGuild = upper(m.ownerGuildName());
    Buffer b; Writer w(b);
    w.StartObject();
    w.Key("id");          w.Uint(uint32_t(i));
    w.Key("vob");         text(w,m.vobNameStr());
    w.Key("scheme");      text(w,m.schemeName());
    w.Key("focus");       text(w,m.focusName());
    w.Key("pos");         position(w,mobs[i].pos);
    w.Key("room");        writeRoom(w,mobs[i].pos);
    w.Key("waypoint");    w.Uint(nearestGraph(mobs[i].pos));
    w.Key("owner_npc");   optionalText(w,ownerNpc);
    w.Key("owner_guild"); optionalText(w,ownerGuild);
    w.Key("container");   w.Bool(m.isContainer());
    w.EndObject();
    objects.add(b);
    containers += m.isContainer() ? 1 : 0;
    owned      += !ownerNpc.empty() || !ownerGuild.empty() ? 1 : 0;
    }

  const auto instances = npcInstances(world);
  for(const auto& room:roomNames) {
    const auto owner = resolveRoomOwner(world,room,instances);
    const auto guild = resolveRoomGuild(world,room,!owner.npc.empty() || !owner.ambiguous.empty());
    if(!owner.npc.empty())
      std::fprintf(stderr,"society room npc: %s -> %s\n",room.c_str(),owner.npc.c_str());
    else if(!owner.ambiguous.empty())
      std::fprintf(stderr,"society room npc ambiguous: %s -> %s -> %s\n",room.c_str(),owner.name.c_str(),owner.ambiguous.c_str());
    else if(guild.name.empty())
      std::fprintf(stderr,"society room npc missing: %s -> %s\n",room.c_str(),owner.name.c_str());
    if(!guild.name.empty())
      std::fprintf(stderr,"society room guild: %s -> %s -> %d\n",room.c_str(),guild.name.c_str(),int(guild.id));
    if(!guild.unresolved.empty())
      std::fprintf(stderr,"society room guild unresolved: %s -> %s\n",room.c_str(),guild.unresolved.c_str());
    Buffer b; Writer w(b);
    w.StartObject();
    w.Key("name");        text(w,room);
    w.Key("owner_npc");   optionalText(w,owner.npc);
    // A room assigned to a guild, not the guild of its private NPC owner; GIL_PUBLIC grants public access.
    w.Key("owner_guild");
    if(guild.id>GIL_NONE && guild.id!=GIL_PUBLIC)
      w.Int(guild.id); else
      w.Null();
    w.EndObject();
    rooms.add(b);
    }

  // NPCs ordered and keyed as the npc-day journal keys them (NpcDayJournal::start): the instance symbol,
  // then the position, then the place in the world's array; `key` is SYMBOL#ordinal among the same symbol.
  struct Start { Npc* npc; std::string symbol; float x, z, y; uint32_t index; };
  std::vector<Start> all;
  for(uint32_t i=0; i<world.npcCount(); ++i) {
    auto* npc = world.npcById(i);
    const auto p = npc->position();
    all.push_back({npc,symbolName(world,size_t(npc->instanceSymbol())),p.x,p.z,p.y,i});
    }
  std::sort(all.begin(),all.end(),[](const Start& a, const Start& b) {
    return std::tie(a.symbol,a.x,a.z,a.y,a.index)<std::tie(b.symbol,b.x,b.z,b.y,b.index);
    });
  std::vector<double> walk, run;
  size_t people = 0, entries = 0, unresolved = 0, aistates = 0, schedules = 0;
  std::vector<std::pair<size_t,size_t>> mmAivars;
  const auto scheduler = world.script().findSymbolIndex(MmScheduler);
  std::vector<std::string> startNearest;
  uint32_t ordinal = 0;
  for(size_t i=0; i<all.size(); ++i) {
    auto& npc = *all[i].npc;
    ordinal = i>0 && all[i-1].symbol==all[i].symbol ? ordinal+1 : 0;
    // A person as the AI bridge's story_people (and so day_people.json) has it.
    const bool person = !npc.isPlayer() && !npc.isDead() && !npc.isMonster();
    // Where the routine placement put it (Npc::resetPositionToTA: its routine point, or the next free one
    // when that is taken); the hero and anything without such a point: the nearest waynet point.
    auto start = idOf(npc.currentWayPoint());
    const auto key = all[i].symbol+"#"+std::to_string(ordinal);
    if(!start) {
      start = nearestGraph(npc.position());
      startNearest.push_back(key);
      }
    Buffer b; Writer w(b);
    w.StartObject();
    w.Key("id");             ascii(w,all[i].symbol);
    w.Key("key");            ascii(w,key);
    w.Key("name");           text(w,npc.displayName());
    w.Key("guild");          w.Uint(npc.guild());
    w.Key("person");         w.Bool(person);
    w.Key("start_waypoint"); w.Uint(*start);
    // The state Npc::tickRoutine starts when the NPC has no routine entry (daedalus::INpc::start_aistate), or null.
    const auto aistate = npc.handle().start_aistate;
    w.Key("start_aistate");
    if(aistate!=0)
      ascii(w,symbolName(world,size_t(aistate))); else
      w.Null();
    // This NPC's own walk speed, as movement.walk_cm_per_s is the median of; 0 without a walk animation.
    const float walkSpeed = npc.simplifiedMoveSpeed(WalkBit::WM_Walk);
    w.Key("walk_cm_per_s");  number(w,walkSpeed>0.f ? double(walkSpeed)*1000.0 : 0.0);
    aistates += aistate!=0 ? 1 : 0;
    // The hours of ZS_MM_AllScheduler for an NPC that starts with it: [state, start aivar, end aivar] in the order
    // of its checks (MmSchedule); null for any other NPC.
    w.Key("mm_schedule");
    if(aistate!=0 && size_t(aistate)==scheduler) {
      if(mmAivars.empty())
        for(const auto& m:MmSchedule)
          mmAivars.emplace_back(aivarIndex(world,m.start),aivarIndex(world,m.end));
      const auto& aiv = npc.handle().aivar;
      w.StartArray();
      for(size_t k=0; k<mmAivars.size(); ++k) {
        w.StartArray();
        ascii(w,MmSchedule[k].state);
        w.Int(aiv[mmAivars[k].first]);
        w.Int(aiv[mmAivars[k].second]);
        w.EndArray();
        }
      w.EndArray();
      ++schedules;
      } else {
      w.Null();
      }
    w.Key("routine");
    w.StartArray();
    npc.forEachRoutine([&](const Npc::RoutineInfo& r) {
      // null only where the engine found no point; a start point (outside the export) is not supported.
      const auto id = idOf(r.waypoint);
      if(r.waypoint!=nullptr && !id)
        throw std::runtime_error("society export: routine point "+std::string(r.point)+" is not a waynet or free point");
      w.StartObject();
      w.Key("start");         w.Int(minuteOfDay(r.start));
      w.Key("end");           w.Int(minuteOfDay(r.end));
      w.Key("state");         ascii(w,symbolName(world,r.state.ptr));
      w.Key("waypoint");      optionalId(w,id);
      w.Key("waypoint_name"); text(w,r.point);
      w.EndObject();
      ++entries;
      unresolved += id ? 0 : 1;
      });
    w.EndArray();
    w.EndObject();
    npcs.add(b);
    if(person) {
      ++people;
      // Npc::simplifiedMoveSpeed is cm per ms of world time; the file has cm per second.
      if(const float v = npc.simplifiedMoveSpeed(WalkBit::WM_Walk); v>0.f)
        walk.push_back(double(v)*1000.0);
      if(const float v = npc.simplifiedMoveSpeed(WalkBit::WM_Run); v>0.f)
        run.push_back(double(v)*1000.0);
      }
    }
  if(walk.empty() || run.empty())
    throw std::runtime_error("society export: no person has a walk or run animation");

  const auto fingerprint = Resources::selectedBaseFingerprint();
  if(fingerprint.empty())
    throw std::runtime_error("society export: no game base is selected");
  const auto now = world.time();
  char source[512];
  std::snprintf(source,sizeof(source),
                "Npc::simplifiedMoveSpeed: horizontal root displacement of the walk/run loop "
                "(AnimationSolver::Move, no weapon, the NPC's overlays) over its duration, times the pose's "
                "playback speed; median of %zu/%zu people at the start, walk %.1f..%.1f, run %.1f..%.1f cm/s",
                walk.size(),run.size(),*std::min_element(walk.begin(),walk.end()),*std::max_element(walk.begin(),walk.end()),
                *std::min_element(run.begin(),run.end()),*std::max_element(run.begin(),run.end()));

  Buffer head; Writer w(head);
  w.StartObject();
  w.Key("format");           ascii(w,"society-world");
  w.Key("version");          w.Int(3);
  w.Key("world");            text(w,world.name());
  w.Key("data_fingerprint"); ascii(w,fingerprint);
  w.Key("engine_commit");    ascii(w,OPENGOTHIC_ENGINE_COMMIT);
  w.Key("start");
  w.StartObject();
  w.Key("day");    w.Int64(now.day());
  w.Key("minute"); w.Int(minuteOfDay(now));
  w.EndObject();
  w.Key("clock");
  w.StartObject();
  w.Key("game_seconds_per_real_second"); number(w,GameSession::clockRate());
  w.EndObject();
  w.Key("movement");
  w.StartObject();
  w.Key("walk_cm_per_s"); number(w,median(walk));
  w.Key("run_cm_per_s");  number(w,median(run));
  w.Key("source");        ascii(w,source);
  w.EndObject();
  w.EndObject();
  // The header object without its closing brace; then one record per line, so files diff line by line.
  Result ret;
  ret.json.assign(head.GetString(),head.GetSize()-1);
  const auto list = [&](const char* key, const Records& r, bool last) {
    ret.json += std::string(",\n\"")+key+"\":["+r.out+(r.count>0 ? "\n" : "")+"]";
    if(last)
      ret.json += "}\n";
    };
  list("waypoints",waypoints,false);
  list("edges",edges,false);
  list("objects",objects,false);
  list("rooms",rooms,false);
  list("npcs",npcs,true);

  std::string nearest;
  for(const auto& k:startNearest)
    nearest += (nearest.empty() ? "" : " ")+k;
  char summary[512];
  std::snprintf(summary,sizeof(summary),
                "waypoints=%zu (freepoints %zu) edges=%zu objects=%zu (containers %zu, owned %zu) npcs=%zu "
                "(people %zu, routine entries %zu, unresolved points %zu, start by nearest point %zu, start_aistate %zu, "
                "mm_schedule %zu) "
                "walk=%.1f run=%.1f cm/s",
                waypoints.count,freepoints,edges.count,objects.count,containers,owned,npcs.count,people,entries,
                unresolved,startNearest.size(),aistates,schedules,median(walk),median(run));
  ret.summary = std::string(summary)+(nearest.empty() ? "" : "; start by nearest point: "+nearest);
  return ret;
  }

}
