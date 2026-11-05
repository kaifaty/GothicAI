#include "societyexport.h"

#include "enginecommit.h"
#include "resources.h"
#include "game/gamescript.h"
#include "game/gamesession.h"
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

int minuteOfDay(const gtime& t) {
  return int(t.hour()*60+t.minute());
  }

double median(std::vector<double> v) {
  if(v.empty())
    return 0;
  std::sort(v.begin(),v.end());
  return v[v.size()/2];
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

  Records waypoints, edges, objects, npcs;
  size_t freepoints = 0;
  for(size_t i=0; i<points.size(); ++i) {
    const auto& p  = *points[i];
    const bool  fp = p.isFreePoint();
    Buffer b; Writer w(b);
    w.StartObject();
    w.Key("id");            w.Uint(uint32_t(i));
    w.Key("name");          text(w,p.name);
    w.Key("pos");           position(w,p.position());
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
    w.Key("waypoint");    w.Uint(nearestGraph(mobs[i].pos));
    w.Key("owner_npc");   optionalText(w,ownerNpc);
    w.Key("owner_guild"); optionalText(w,ownerGuild);
    w.Key("container");   w.Bool(m.isContainer());
    w.EndObject();
    objects.add(b);
    containers += m.isContainer() ? 1 : 0;
    owned      += !ownerNpc.empty() || !ownerGuild.empty() ? 1 : 0;
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
  size_t people = 0, entries = 0, unresolved = 0;
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
  w.Key("version");          w.Int(1);
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
  list("npcs",npcs,true);

  std::string nearest;
  for(const auto& k:startNearest)
    nearest += (nearest.empty() ? "" : " ")+k;
  char summary[512];
  std::snprintf(summary,sizeof(summary),
                "waypoints=%zu (freepoints %zu) edges=%zu objects=%zu (containers %zu, owned %zu) npcs=%zu "
                "(people %zu, routine entries %zu, unresolved points %zu, start by nearest point %zu) "
                "walk=%.1f run=%.1f cm/s",
                waypoints.count,freepoints,edges.count,objects.count,containers,owned,npcs.count,people,entries,
                unresolved,startNearest.size(),median(walk),median(run));
  ret.summary = std::string(summary)+(nearest.empty() ? "" : "; start by nearest point: "+nearest);
  return ret;
  }

}
