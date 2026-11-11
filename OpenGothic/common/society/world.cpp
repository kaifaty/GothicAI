#include "core.h"

#include <rapidjson/document.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <tuple>

namespace Society {
namespace {

using Value = rapidjson::Value;

// A fault of the file being read; parseWorld names the file (the export or the config) in front of it.
struct Bad : std::runtime_error {
  using std::runtime_error::runtime_error;
  };

[[noreturn]] void bad(const std::string& what) {
  throw Bad(what);
  }

void need(bool condition, const std::string& what) {
  if(!condition)
    bad(what);
  }

const Value& member(const Value& v, const char* key) {
  need(v.IsObject() && v.HasMember(key),std::string("missing field ")+key);
  return v[key];
  }

std::string str(const Value& v, const char* key) {
  const auto& m = member(v,key);
  need(m.IsString(),std::string(key)+" must be a string");
  return {m.GetString(),m.GetStringLength()};
  }

// A string or null; null reads as "".
std::string optionalStr(const Value& v, const char* key) {
  const auto& m = member(v,key);
  need(m.IsNull() || m.IsString(),std::string(key)+" must be a string or null");
  return m.IsNull() ? std::string() : std::string(m.GetString(),m.GetStringLength());
  }

uint32_t uint(const Value& v, const char* key) {
  const auto& m = member(v,key);
  need(m.IsUint(),std::string(key)+" must be a non-negative integer");
  return m.GetUint();
  }

double real(const Value& v, const char* key) {
  const auto& m = member(v,key);
  need(m.IsNumber(),std::string(key)+" must be a number");
  return m.GetDouble();
  }

bool flag(const Value& v, const char* key) {
  const auto& m = member(v,key);
  need(m.IsBool(),std::string(key)+" must be a bool");
  return m.GetBool();
  }

const Value& array(const Value& v, const char* key) {
  const auto& m = member(v,key);
  need(m.IsArray(),std::string(key)+" must be an array");
  return m;
  }

std::array<float,3> position(const Value& v, const char* key) {
  const auto& m = array(v,key);
  need(m.Size()==3 && m[0].IsNumber() && m[1].IsNumber() && m[2].IsNumber(),std::string(key)+" must be three numbers");
  return {float(m[0].GetDouble()),float(m[1].GetDouble()),float(m[2].GetDouble())};
  }

uint64_t fnv1a64(std::string_view bytes) {
  uint64_t h = 14695981039346656037ull;
  for(char c:bytes) {
    h ^= uint8_t(c);
    h *= 1099511628211ull;
    }
  return h;
  }

void readHeader(World& w, const Value& doc) {
  need(doc.IsObject(),"the file is not a JSON object");
  need(str(doc,"format")=="society-world","format must be society-world");
  const auto& version = member(doc,"version");
  need(version.IsInt() && version.GetInt()>=1 && version.GetInt()<=3,"only versions 1, 2 and 3 are supported");
  w.version = version.GetInt();
  w.worldName       = str(doc,"world");
  w.dataFingerprint = str(doc,"data_fingerprint");
  w.engineCommit    = str(doc,"engine_commit");
  const auto& start = member(doc,"start");
  const auto& day   = member(start,"day");
  need(day.IsInt64() && day.GetInt64()>=0,"start.day must be a non-negative integer");
  w.startDay    = day.GetInt64();
  w.startMinute = int32_t(uint(start,"minute"));
  need(w.startMinute<24*60,"start.minute must be a minute of the day");
  w.clockRate = real(member(doc,"clock"),"game_seconds_per_real_second");
  const auto& movement = member(doc,"movement");
  w.walkCmPerS     = real(movement,"walk_cm_per_s");
  w.runCmPerS      = real(movement,"run_cm_per_s");
  w.movementSource = str(movement,"source");
  need(w.clockRate>0 && w.walkCmPerS>0 && w.runCmPerS>0,"clock and movement speeds must be above 0");
  }

void readRooms(World& w, const Value& doc) {
  if(w.version<3)
    return;
  const auto& list = array(doc,"rooms");
  auto& r = w.rooms;
  for(const auto& v:list.GetArray()) {
    auto name = str(v,"name");
    need(!name.empty() && (r.name.empty() || r.name.back()<name),"room names must be nonempty, sorted and unique");
    r.name.push_back(name);
    r.ownerNpc.push_back(optionalStr(v,"owner_npc"));
    const auto& guild = member(v,"owner_guild");
    need(guild.IsNull() || (guild.IsInt() && guild.GetInt()>0),"room owner_guild must be a positive integer or null");
    r.ownerGuild.push_back(guild.IsNull() ? -1 : guild.GetInt());
    }
  }

std::string roomOf(const World& w, const Value& v) {
  if(w.version<3)
    return {};
  auto name = optionalStr(v,"room");
  need(name.empty() || std::binary_search(w.rooms.name.begin(),w.rooms.name.end(),name),"unknown room "+name);
  return name;
  }

void readPoints(World& w, const Value& doc) {
  const auto& list = array(doc,"waypoints");
  auto& p = w.points;
  const size_t n = list.Size();
  p.name.resize(n); p.room.resize(n); p.pos.resize(n); p.free.resize(n); p.graph.resize(n); p.graphCm.resize(n); p.component.resize(n);
  for(rapidjson::SizeType i=0; i<list.Size(); ++i) {
    const auto& v = list[i];
    need(uint(v,"id")==i,"waypoint ids must be 0..n-1 in order");
    p.name[i] = str(v,"name");
    p.room[i] = roomOf(w,v);
    p.pos[i]  = position(v,"pos");
    p.free[i] = flag(v,"freepoint") ? 1 : 0;
    }
  for(rapidjson::SizeType i=0; i<list.Size(); ++i) {
    const auto& near = member(list[i],"near_waypoint");
    if(!p.free[i]) {
      need(near.IsNull(),"waynet point "+p.name[i]+" must have near_waypoint null");
      p.graph[i]   = i;
      p.graphCm[i] = 0;
      continue;
      }
    need(near.IsUint() && near.GetUint()<n && !p.free[near.GetUint()],"freepoint "+p.name[i]+" has no near_waypoint on the waynet");
    const uint32_t g = near.GetUint();
    // One operation per statement: no fused multiply-add, the same centimetres on every machine.
    const double dx = double(p.pos[i][0])-p.pos[g][0], dy = double(p.pos[i][1])-p.pos[g][1], dz = double(p.pos[i][2])-p.pos[g][2];
    const double xx = dx*dx, yy = dy*dy, zz = dz*dz;
    const double sum = xx+yy;
    p.graph[i]   = g;
    p.graphCm[i] = int32_t(std::llround(std::sqrt(sum+zz)));
    ++w.freepoints;
    }
  }

void readEdges(World& w, const Value& doc) {
  const auto& list = array(doc,"edges");
  const auto  n    = uint32_t(w.points.size());
  std::vector<std::array<uint32_t,3>> edges;
  edges.reserve(list.Size());
  for(rapidjson::SizeType i=0; i<list.Size(); ++i) {
    const auto& e = list[i];
    need(e.IsArray() && e.Size()==3 && e[0].IsUint() && e[1].IsUint() && e[2].IsInt(),"an edge is [a, b, length_cm]");
    const uint32_t a = e[0].GetUint(), b = e[1].GetUint();
    const int      l = e[2].GetInt();
    need(a<n && b<n && a!=b && !w.points.free[a] && !w.points.free[b] && l>0,
         "edge "+std::to_string(a)+" -> "+std::to_string(b)+" must join two waynet points and be longer than 0");
    need(i==0 || std::make_pair(edges.back()[0],edges.back()[1])<std::make_pair(a,b),"edges must be sorted by (a, b) without repeats");
    edges.push_back({a,b,uint32_t(l)});
    }
  w.edgeBegin.assign(n+1,0);
  for(const auto& e:edges)
    ++w.edgeBegin[e[0]+1];
  std::partial_sum(w.edgeBegin.begin(),w.edgeBegin.end(),w.edgeBegin.begin());
  w.edgeTo.resize(edges.size());
  w.edgeCm.resize(edges.size());
  // Sorted by a, so each point's edges are already one run.
  for(size_t i=0; i<edges.size(); ++i) {
    w.edgeTo[i] = edges[i][1];
    w.edgeCm[i] = int32_t(edges[i][2]);
    }

  // Components of the waynet, numbered by their smallest point; a freepoint belongs to its near_waypoint's.
  std::vector<uint32_t> parent(n);
  std::iota(parent.begin(),parent.end(),0u);
  const auto root = [&](uint32_t x) {
    while(parent[x]!=x)
      x = parent[x] = parent[parent[x]];
    return x;
    };
  for(const auto& e:edges) {
    const auto ra = root(e[0]), rb = root(e[1]);
    if(ra!=rb)
      parent[std::max(ra,rb)] = std::min(ra,rb);
    }
  std::vector<uint32_t> number(n,NoPoint);
  for(uint32_t i=0; i<n; ++i) {
    if(w.points.free[i])
      continue;
    auto& c = number[root(i)];
    if(c==NoPoint)
      c = uint32_t(w.components++);
    }
  for(uint32_t i=0; i<n; ++i)
    w.points.component[i] = number[root(w.points.graph[i])];
  }

void readObjects(World& w, const Value& doc) {
  const auto& list = array(doc,"objects");
  auto& o = w.objects;
  for(rapidjson::SizeType i=0; i<list.Size(); ++i) {
    const auto& v = list[i];
    need(uint(v,"id")==i,"object ids must be 0..n-1 in order");
    o.vob.push_back(str(v,"vob"));
    o.scheme.push_back(str(v,"scheme"));
    o.focus.push_back(str(v,"focus"));
    o.room.push_back(roomOf(w,v));
    o.pos.push_back(position(v,"pos"));
    const auto wp = uint(v,"waypoint");
    need(wp<w.points.size() && !w.points.free[wp],"object "+std::to_string(i)+" has no waynet point");
    o.waypoint.push_back(wp);
    o.ownerNpc.push_back(optionalStr(v,"owner_npc"));
    o.ownerGuild.push_back(optionalStr(v,"owner_guild"));
    o.container.push_back(flag(v,"container") ? 1 : 0);
    w.containers   += o.container.back();
    w.ownedObjects += !o.ownerNpc.back().empty() || !o.ownerGuild.back().empty() ? 1 : 0;
    }
  }

void readNpcs(World& w, const Value& doc) {
  const auto& list = array(doc,"npcs");
  auto& n = w.npcs;
  auto& r = w.routines;
  auto& m = w.schedules;
  // Every state name the NPCs use, in reading order; `stateOf` keeps where each one goes: a routine entry,
  // a start_aistate, a schedule window.
  std::vector<std::string> states;
  enum Slot : uint8_t { Entry, Start, Window };
  std::vector<std::pair<Slot,size_t>> stateOf;
  n.routineBegin.push_back(0);
  n.scheduleBegin.push_back(0);
  for(rapidjson::SizeType i=0; i<list.Size(); ++i) {
    const auto& v = list[i];
    n.id.push_back(str(v,"id"));
    n.key.push_back(str(v,"key"));
    n.name.push_back(str(v,"name"));
    n.guild.push_back(uint(v,"guild"));
    n.person.push_back(flag(v,"person") ? 1 : 0);
    const auto start = uint(v,"start_waypoint");
    need(start<w.points.size(),"npc "+n.key.back()+": start_waypoint is not a point");
    n.startPoint.push_back(start);
    // v2: the state the engine starts without a routine, the NPC's own walk speed and the monster schedule.
    n.startState.push_back(NoState);
    n.walkCmPerS.push_back(w.walkCmPerS);
    if(w.version>=2) {
      const auto aistate = optionalStr(v,"start_aistate");
      if(!aistate.empty()) {
        states.push_back(aistate);
        stateOf.emplace_back(Start,i);
        }
      const double walk = real(v,"walk_cm_per_s");
      need(walk>=0,"npc "+n.key.back()+": walk_cm_per_s must not be negative");
      if(walk>0)
        n.walkCmPerS.back() = walk;
      const auto& schedule = member(v,"mm_schedule");
      need(schedule.IsNull() || schedule.IsArray(),"npc "+n.key.back()+": mm_schedule must be an array or null");
      if(schedule.IsArray())
        for(rapidjson::SizeType j=0; j<schedule.Size(); ++j) {
          const auto& x = schedule[j];
          need(x.IsArray() && x.Size()==3 && x[0].IsString() && x[1].IsInt() && x[2].IsInt(),
               "npc "+n.key.back()+": an mm_schedule window is [state, start hour, end hour]");
          states.emplace_back(x[0].GetString(),x[0].GetStringLength());
          stateOf.emplace_back(Window,m.size());
          m.state.push_back(0);
          m.start.push_back(int16_t(x[1].GetInt()));
          m.end.push_back(int16_t(x[2].GetInt()));
          }
      }
    n.scheduleBegin.push_back(uint32_t(m.size()));
    const auto& routine = array(v,"routine");
    need(routine.Size()<NoEntry,"npc "+n.key.back()+": too many routine entries");
    for(rapidjson::SizeType j=0; j<routine.Size(); ++j) {
      const auto& e  = routine[j];
      const auto  s  = uint(e,"start"), t = uint(e,"end");
      need(s<24*60 && t<24*60,"npc "+n.key.back()+": routine entry "+std::to_string(j)+" has a bad time");
      r.start.push_back(uint16_t(s));
      r.end.push_back(uint16_t(t));
      states.push_back(str(e,"state"));
      stateOf.emplace_back(Entry,r.state.size());
      r.state.push_back(0);
      const auto& wp = member(e,"waypoint");
      need(wp.IsNull() || (wp.IsUint() && wp.GetUint()<w.points.size()),"npc "+n.key.back()+": routine point is not a point");
      r.point.push_back(wp.IsNull() ? NoPoint : wp.GetUint());
      r.pointName.push_back(str(e,"waypoint_name"));
      w.unresolvedEntries += wp.IsNull() ? 1 : 0;
      }
    n.routineBegin.push_back(uint32_t(r.size()));
    if(n.person.back()) {
      ++w.people;
      w.peopleEntries += routine.Size();
      }
    }
  // State names sorted, with the follow-on states of the rules (ZS_GOTOBED -> ZS_SLEEP): the index is stable for
  // the same set of states.
  w.stateNames = states;
  for(const auto& f:FollowStates)
    if(std::find(states.begin(),states.end(),f.from)!=states.end())
      w.stateNames.emplace_back(f.to);
  std::sort(w.stateNames.begin(),w.stateNames.end());
  w.stateNames.erase(std::unique(w.stateNames.begin(),w.stateNames.end()),w.stateNames.end());
  need(w.stateNames.size()<NoState,"too many states");
  const auto index = [&](std::string_view name) {
    return uint16_t(std::lower_bound(w.stateNames.begin(),w.stateNames.end(),name)-w.stateNames.begin());
    };
  for(size_t k=0; k<states.size(); ++k) {
    const auto [slot,at] = stateOf[k];
    const auto id = index(states[k]);
    if(slot==Entry)
      r.state[at] = id;
    else if(slot==Start)
      n.startState[at] = id;
    else
      m.state[at] = id;
    }
  w.followState.assign(w.stateNames.size(),NoState);
  for(const auto& f:FollowStates)
    if(std::binary_search(w.stateNames.begin(),w.stateNames.end(),std::string_view(f.from)))
      w.followState[index(f.from)] = index(f.to);
  const auto scheduler = std::find(w.stateNames.begin(),w.stateNames.end(),MmScheduler);
  w.schedulerState = scheduler==w.stateNames.end() ? NoState : uint16_t(scheduler-w.stateNames.begin());
  }

// --- The config file ---------------------------------------------------------------------------------------

// Every field of an entry must be known: a typo would otherwise drop a number silently.
void onlyKeys(const Value& v, std::initializer_list<std::string_view> keys, const std::string& where) {
  need(v.IsObject(),where+" must be an object");
  for(auto it=v.MemberBegin(); it!=v.MemberEnd(); ++it) {
    const std::string_view k(it->name.GetString(),it->name.GetStringLength());
    need(std::find(keys.begin(),keys.end(),k)!=keys.end(),where+": unknown field "+std::string(k));
    }
  }

double hours(const Value& v, const char* key, const std::string& where) {
  const double h = real(v,key);
  need(h>0 && std::isfinite(h),where+": "+key+" must be hours above 0");
  return h;
  }

uint16_t needIndex(const Config& c, std::string_view id, const std::string& where) {
  const auto it = std::find(c.needs.id.begin(),c.needs.id.end(),id);
  need(it!=c.needs.id.end(),where+": no need "+std::string(id));
  return uint16_t(it-c.needs.id.begin());
  }

// {"need": hours to fill it from 0 to 1, ...} -> per game second, into row `at` of `fill`.
void readFill(const Config& c, const Value& v, std::vector<double>& fill, size_t at, const std::string& where) {
  need(v.IsObject(),where+": fill_hours must be an object of need: hours");
  for(auto it=v.MemberBegin(); it!=v.MemberEnd(); ++it) {
    const std::string_view id(it->name.GetString(),it->name.GetStringLength());
    const auto k = needIndex(c,id,where);
    need(it->value.IsNumber() && it->value.GetDouble()>0,where+": fill_hours of "+std::string(id)+" must be above 0");
    fill[at*c.needs.size()+k] = 1.0/it->value.GetDouble()/3600.0;
    }
  }

void readNeeds(Config& c, const Value& doc) {
  const auto& list = array(doc,"needs");
  auto& n = c.needs;
  for(rapidjson::SizeType i=0; i<list.Size(); ++i) {
    const auto& v = list[i];
    const std::string where = "needs["+std::to_string(i)+"]";
    onlyKeys(v,{"id","name","empty_hours","low","critical","start","awake_only","guild_weight"},where);
    const auto id = str(v,"id");
    need(!id.empty() && std::find(n.id.begin(),n.id.end(),id)==n.id.end(),where+": id must be unique and not empty");
    n.id.push_back(id);
    n.name.push_back(str(v,"name"));
    n.decay.push_back(1.0/hours(v,"empty_hours",where)/3600.0);
    n.low.push_back(real(v,"low"));
    n.critical.push_back(real(v,"critical"));
    n.start.push_back(real(v,"start"));
    need(0<=n.critical.back() && n.critical.back()<n.low.back() && n.low.back()<1,where+": thresholds must be 0 <= critical < low < 1");
    need(0<=n.start.back() && n.start.back()<=1,where+": start must be in 0..1");
    n.awakeOnly.push_back(flag(v,"awake_only") ? 1 : 0);
    n.weightDefault.push_back(1.0);
    n.weightByGuild.emplace_back();
    if(v.HasMember("guild_weight")) {
      const auto& g = v["guild_weight"];
      onlyKeys(g,{"default","guilds"},where+".guild_weight");
      n.weightDefault.back() = real(g,"default");
      need(n.weightDefault.back()>=0,where+": guild weights must not be negative");
      const auto& guilds = array(g,"guilds");
      for(rapidjson::SizeType j=0; j<guilds.Size(); ++j) {
        const auto& x = guilds[j];
        onlyKeys(x,{"guild","name","weight"},where+".guild_weight.guilds["+std::to_string(j)+"]");
        const double weight = real(x,"weight");
        need(weight>=0,where+": guild weights must not be negative");
        need(n.weightByGuild.back().emplace(uint(x,"guild"),weight).second,where+": a guild is listed twice");
        }
      }
    }
  need(n.size()<NoKind,"too many needs");
  }

void readStates(Config& c, const Value& doc) {
  const auto& list = array(doc,"states");
  const size_t needs = c.needs.size();
  struct Row { std::string name; uint8_t kind, asleep; std::vector<double> fill; };
  std::vector<Row> rows;
  for(rapidjson::SizeType i=0; i<list.Size(); ++i) {
    const auto& v = list[i];
    const std::string where = "states["+std::to_string(i)+"]";
    onlyKeys(v,{"state","kind","asleep","fill_hours","note"},where);
    Row r;
    r.name = str(v,"state");
    need(r.name.rfind("ZS_",0)==0,where+": state must be a ZS_* name");
    const auto kind = str(v,"kind");
    need(kind=="anchor" || kind=="free",where+": kind must be anchor or free");
    r.kind   = uint8_t(kind=="anchor" ? StateKind::Anchor : StateKind::Free);
    r.asleep = flag(v,"asleep") ? 1 : 0;
    r.fill.assign(needs,0.0);
    readFill(c,member(v,"fill_hours"),r.fill,0,where);
    rows.push_back(std::move(r));
    }
  std::sort(rows.begin(),rows.end(),[](const Row& a, const Row& b) { return a.name<b.name; });
  for(size_t i=1; i<rows.size(); ++i)
    need(rows[i-1].name!=rows[i].name,"states: "+rows[i].name+" is listed twice");
  need(rows.size()<NoState,"too many states");
  auto& st = c.states;
  for(auto& r:rows) {
    st.name.push_back(r.name);
    st.kind.push_back(r.kind);
    st.asleep.push_back(r.asleep);
    st.fill.insert(st.fill.end(),r.fill.begin(),r.fill.end());
    }
  }

void readObjectKinds(Config& c, const Value& doc) {
  const auto& list = array(doc,"object_kinds");
  auto& o = c.objectKinds;
  std::vector<std::pair<std::string,uint16_t>> schemes, prefixes;
  for(rapidjson::SizeType i=0; i<list.Size(); ++i) {
    const auto& v = list[i];
    const std::string where = "object_kinds["+std::to_string(i)+"]";
    onlyKeys(v,{"kind","schemes","freepoints","at","places","guilds","state","fill_hours","capacity","minutes","note"},where);
    const auto id = str(v,"kind");
    need(!id.empty() && std::find(o.id.begin(),o.id.end(),id)==o.id.end(),where+": kind must be unique and not empty");
    const auto k = uint16_t(o.id.size());
    o.id.push_back(id);
    o.state.push_back(str(v,"state"));
    need(std::binary_search(c.states.name.begin(),c.states.name.end(),o.state.back()),
         where+": state "+o.state.back()+" is not in the states table");
    o.fill.resize(o.fill.size()+c.needs.size(),0.0);
    readFill(c,member(v,"fill_hours"),o.fill,k,where);
    o.capacity.push_back(uint(v,"capacity"));
    o.minutes.push_back(uint(v,"minutes"));
    need(o.capacity.back()>0 && o.minutes.back()>0,where+": capacity and minutes must be above 0");
    const auto& sc = array(v,"schemes");
    for(rapidjson::SizeType j=0; j<sc.Size(); ++j) {
      need(sc[j].IsString() && sc[j].GetStringLength()>0,where+": schemes must be names");
      schemes.emplace_back(sc[j].GetString(),k);
      }
    const auto& fp = array(v,"freepoints");
    for(rapidjson::SizeType j=0; j<fp.Size(); ++j) {
      need(fp[j].IsString() && std::string_view(fp[j].GetString()).rfind("FP_",0)==0,where+": freepoints must be FP_* prefixes");
      prefixes.emplace_back(fp[j].GetString(),k);
      }
    // Optional: where else it is done (outdoors, in the NPC's own or guild's rooms, at the points of places of
    // places.json) and the only guilds that may use it.
    o.at.push_back(uint8_t(KindAt::Listed));
    o.places.emplace_back();
    if(v.HasMember("at")) {
      const auto at = str(v,"at");
      need(at=="outdoors" || at=="own_rooms" || at=="places",where+": at must be outdoors, own_rooms or places");
      o.at.back() = uint8_t(at=="outdoors" ? KindAt::Outdoors : at=="own_rooms" ? KindAt::OwnRooms : KindAt::Places);
      }
    if(v.HasMember("places")) {
      const auto& pl = array(v,"places");
      for(rapidjson::SizeType j=0; j<pl.Size(); ++j) {
        need(pl[j].IsString(),where+": places must be place ids of places.json");
        o.places.back().emplace_back(pl[j].GetString(),pl[j].GetStringLength());
        }
      }
    need((o.at.back()==uint8_t(KindAt::Places))==!o.places.back().empty(),where+": places go with at: places, and it needs them");
    o.guilds.emplace_back();
    if(v.HasMember("guilds")) {
      const auto& g = array(v,"guilds");
      for(rapidjson::SizeType j=0; j<g.Size(); ++j) {
        need(g[j].IsUint(),where+": guilds must be guild numbers");
        o.guilds.back().push_back(g[j].GetUint());
        }
      std::sort(o.guilds.back().begin(),o.guilds.back().end());
      }
    need(sc.Size()+fp.Size()>0 || o.at.back()!=uint8_t(KindAt::Listed),where+": a kind needs a scheme, a freepoint prefix or at");
    }
  need(o.size()<NoKind,"too many object kinds");
  // A mobsi has one kind; a freepoint may have several (eating at a campfire is another option than sitting at it).
  std::sort(schemes.begin(),schemes.end());
  for(size_t i=1; i<schemes.size(); ++i)
    need(schemes[i-1].first!=schemes[i].first,"object_kinds: "+schemes[i].first+" belongs to two kinds");
  std::sort(prefixes.begin(),prefixes.end());
  for(size_t i=1; i<prefixes.size(); ++i)
    need(prefixes[i-1]!=prefixes[i],"object_kinds: "+prefixes[i].first+" is listed twice in one kind");
  for(auto& [name,k]:schemes) { o.scheme.push_back(name); o.schemeKind.push_back(k); }
  for(auto& [name,k]:prefixes) { o.prefix.push_back(name); o.prefixKind.push_back(k); }
  }

// characters.json: {"version": 1, "cards": {"<instance>": {"greed": 1..5, ...}}}; the scales an NPC's card gives,
// by the export's id.
std::unordered_map<std::string,std::vector<uint8_t>> readCards(Config& c, const std::filesystem::path& path) {
  std::ifstream in(path,std::ios::binary);
  need(bool(in),"cannot read the cards "+path.string());
  std::ostringstream text;
  text << in.rdbuf();
  rapidjson::Document doc;
  const auto json = text.str();
  doc.Parse(json.data(),json.size());
  need(!doc.HasParseError() && doc.IsObject(),"the cards "+path.string()+" are not a JSON object");
  const auto& cards = member(doc,"cards");
  need(cards.IsObject(),"cards must be an object by NPC");
  std::unordered_map<std::string,std::vector<uint8_t>> ret;
  for(auto it=cards.MemberBegin(); it!=cards.MemberEnd(); ++it) {
    std::vector<uint8_t> values;
    for(const auto& scale:c.scales) {
      const auto& x = member(it->value,scale.c_str());
      need(x.IsInt() && x.GetInt()>=1 && x.GetInt()<=5,"card "+std::string(it->name.GetString())+": "+scale+" must be 1..5");
      values.push_back(uint8_t(x.GetInt()));
      }
    ret.emplace(std::string(it->name.GetString(),it->name.GetStringLength()),std::move(values));
    }
  c.cards = ret.size();
  return ret;
  }

void readTraits(Config& c, const Value& doc, const std::filesystem::path& dir,
                std::unordered_map<std::string,std::vector<uint8_t>>& cards) {
  const auto& t = member(doc,"traits");
  onlyKeys(t,{"cards","scales","middle","noise","decay","note"},"traits");
  const auto& scales = array(t,"scales");
  for(rapidjson::SizeType i=0; i<scales.Size(); ++i) {
    need(scales[i].IsString(),"traits.scales must be names");
    c.scales.emplace_back(scales[i].GetString(),scales[i].GetStringLength());
    }
  const auto& middle = member(t,"middle");
  need(middle.IsInt() && middle.GetInt()>=1 && middle.GetInt()<=5,"traits.middle must be 1..5");
  c.middle = middle.GetInt();
  c.noise  = real(t,"noise");
  need(0<=c.noise && c.noise<1,"traits.noise must be in 0..1");
  const auto path = optionalStr(t,"cards");
  if(!path.empty()) {
    const std::filesystem::path p(path);
    c.cardsPath = (p.is_absolute() ? p : dir/p).lexically_normal().string();
    cards = readCards(c,c.cardsPath);
    }
  const auto& rules = array(t,"decay");
  for(rapidjson::SizeType i=0; i<rules.Size(); ++i) {
    const auto& v = rules[i];
    const std::string where = "traits.decay["+std::to_string(i)+"]";
    onlyKeys(v,{"scale","need","per_point"},where);
    Config::TraitRule r;
    const auto scale = str(v,"scale");
    const auto at = std::find(c.scales.begin(),c.scales.end(),scale);
    need(at!=c.scales.end(),where+": no scale "+scale+" in traits.scales");
    r.scale    = uint16_t(at-c.scales.begin());
    r.need     = needIndex(c,str(v,"need"),where);
    r.perPoint = real(v,"per_point");
    // The multiplier over 1..5 stays above 0.
    need(std::abs(r.perPoint)*double(std::max(c.middle-1,5-c.middle))<1,where+": per_point would make the decay 0 or less");
    c.traits.push_back(r);
    }
  }

// decisions: the menu, the masks, the switches and the teacher (docs/society-sim.md, «Файл настройки → decisions»).
void readDecisions(Config& c, const Value& doc, const std::filesystem::path& dir) {
  if(!doc.HasMember("decisions"))
    return;
  const auto& v = doc["decisions"];
  const std::string where = "decisions";
  onlyKeys(v,{"note","menu","travel_limit_min","min_dwell_min","posts_release_for_needs","post_states","no_needs_guilds",
              "protected","places","guild_names","teacher"},where);
  auto& d = c.decisions;
  d.loaded = true;
  d.menu   = uint(v,"menu");
  need(d.menu>=1 && d.menu<=MenuSize,where+": menu must be 1.."+std::to_string(MenuSize));
  d.travelLimit = int64_t(std::llround(hours(v,"travel_limit_min",where)*60.0));
  d.minDwell    = int64_t(std::llround(hours(v,"min_dwell_min",where)*60.0));
  need(d.minDwell>=60,where+": min_dwell_min must be a minute or more");
  d.postsRelease = flag(v,"posts_release_for_needs");
  const auto& posts = array(v,"post_states");
  for(rapidjson::SizeType i=0; i<posts.Size(); ++i) {
    need(posts[i].IsString() && std::string_view(posts[i].GetString()).rfind("ZS_",0)==0,where+": post_states must be ZS_* names");
    d.postStates.emplace_back(posts[i].GetString(),posts[i].GetStringLength());
    }
  const auto& guilds = array(v,"no_needs_guilds");
  for(rapidjson::SizeType i=0; i<guilds.Size(); ++i) {
    need(guilds[i].IsUint(),where+": no_needs_guilds must be guild numbers");
    d.noNeedsGuilds.push_back(guilds[i].GetUint());
    }
  const auto path = optionalStr(v,"protected");
  if(!path.empty()) {
    const std::filesystem::path p(path);
    d.protectedPath = (p.is_absolute() ? p : dir/p).lexically_normal().string();
    }
  if(v.HasMember("places")) {
    const std::filesystem::path p(str(v,"places"));
    d.placesPath = (p.is_absolute() ? p : dir/p).lexically_normal().string();
    }
  const auto& names = member(v,"guild_names");
  need(names.IsObject(),where+": guild_names must be an object of GIL_* name: guild number");
  for(auto it=names.MemberBegin(); it!=names.MemberEnd(); ++it) {
    need(it->value.IsUint(),where+": guild_names values must be guild numbers");
    d.guildNames.emplace(std::string(it->name.GetString(),it->name.GetStringLength()),it->value.GetUint());
    }
  const auto& t = member(v,"teacher");
  onlyKeys(t,{"note","n","m","weights","horizon_min","travel_cost_per_min","inertia","temperature","top"},where+".teacher");
  d.n = real(t,"n");
  d.m = real(t,"m");
  need(d.n>=1 && d.m>=1 && std::isfinite(d.n) && std::isfinite(d.m),where+".teacher: n and m must be 1 or more");
  d.horizon = int64_t(std::llround(hours(t,"horizon_min",where+".teacher")*60.0));
  d.travelCost  = real(t,"travel_cost_per_min")/60.0;
  d.inertia     = real(t,"inertia");
  d.temperature = real(t,"temperature");
  need(d.travelCost>=0 && d.inertia>=0 && d.temperature>=0,where+".teacher: travel_cost_per_min, inertia and temperature must not be negative");
  d.top = uint(t,"top");
  need(d.top>=1,where+".teacher: top must be 1 or more");
  d.weight.assign(c.needs.size(),1.0);
  const auto& w = member(t,"weights");
  need(w.IsObject(),where+".teacher: weights must be an object of need: weight");
  for(auto it=w.MemberBegin(); it!=w.MemberEnd(); ++it) {
    const auto k = needIndex(c,std::string_view(it->name.GetString(),it->name.GetStringLength()),where+".teacher");
    need(it->value.IsNumber() && it->value.GetDouble()>=0,where+".teacher: weights must not be negative");
    d.weight[k] = it->value.GetDouble();
    }
  }

// The protected list (data/society/protected.json): [{"id": instance, "reasons": [...]}].
std::vector<std::string> readProtected(const std::string& path) {
  std::vector<std::string> ret;
  if(path.empty())
    return ret;
  std::ifstream in(path,std::ios::binary);
  need(bool(in),"cannot read the protected list "+path);
  std::ostringstream text;
  text << in.rdbuf();
  rapidjson::Document doc;
  const auto json = text.str();
  doc.Parse(json.data(),json.size());
  need(!doc.HasParseError() && doc.IsArray(),"the protected list "+path+" is not a JSON array");
  for(const auto& e:doc.GetArray())
    ret.push_back(str(e,"id"));
  return ret;
  }

// The states of the object kinds that the export does not use go after its own, sorted among themselves: the
// export's state indices (and so a routine run's journal records) stay.
void appendKindStates(World& w) {
  std::vector<std::string> extra;
  for(const auto& state:w.config.objectKinds.state)
    if(!std::binary_search(w.stateNames.begin(),w.stateNames.end(),state))
      extra.push_back(state);
  std::sort(extra.begin(),extra.end());
  extra.erase(std::unique(extra.begin(),extra.end()),extra.end());
  w.stateNames.insert(w.stateNames.end(),extra.begin(),extra.end());
  w.followState.resize(w.stateNames.size(),NoState);
  need(w.stateNames.size()<NoState,"too many states");
  }

uint16_t stateIndex(const World& w, std::string_view name) {
  for(size_t i=0; i<w.stateNames.size(); ++i)
    if(w.stateNames[i]==name)
      return uint16_t(i);
  return NoState;
  }

// Who decides, the posts, the owners of objects and points (a mobsi's own, otherwise its room's), the places of
// options by waynet point.
void applyDecisions(World& w, const std::vector<std::string>& protectedIds) {
  const auto& c = w.config;
  const auto& d = c.decisions;
  const size_t n = w.npcs.size();
  std::unordered_map<std::string,uint32_t> byId;
  for(uint32_t i=0; i<n; ++i)
    byId.emplace(w.npcs.id[i],i);
  w.npcProtected.assign(n,0);
  w.protectedListed = protectedIds.size();
  for(const auto& id:protectedIds)
    if(auto it = byId.find(id); it!=byId.end()) {
      w.npcProtected[it->second] = 1;
      ++w.protectedFound;
      }
  w.npcDecider.assign(n,0);
  for(uint32_t i=0; i<n; ++i) {
    const bool guild = std::find(d.noNeedsGuilds.begin(),d.noNeedsGuilds.end(),w.npcs.guild[i])==d.noNeedsGuilds.end();
    w.npcDecider[i] = w.npcs.person[i] && w.entryCount(i)>0 && !w.npcProtected[i] && guild ? 1 : 0;
    if(w.npcDecider[i])
      w.deciderWalk = std::max(w.deciderWalk,w.npcs.walkCmPerS[i]);
    }
  w.statePost.assign(w.stateNames.size(),0);
  for(size_t i=0; i<w.stateNames.size(); ++i)
    for(const auto& p:d.postStates) {
      const bool prefix = !p.empty() && p.back()=='*';
      const auto base = prefix ? std::string_view(p).substr(0,p.size()-1) : std::string_view(p);
      if(prefix ? std::string_view(w.stateNames[i]).rfind(base,0)==0 : w.stateNames[i]==base)
        w.statePost[i] = 1;
      }
  w.kindState.resize(c.objectKinds.size());
  for(size_t k=0; k<c.objectKinds.size(); ++k)
    w.kindState[k] = stateIndex(w,c.objectKinds.state[k]);
  // A kind advertises what its state does: the core fills the needs by the state, so the two must agree.
  const size_t needs = c.needs.size();
  for(size_t k=0; k<c.objectKinds.size(); ++k) {
    const auto row = size_t(std::lower_bound(c.states.name.begin(),c.states.name.end(),c.objectKinds.state[k])-c.states.name.begin());
    for(size_t j=0; j<needs; ++j)
      need(std::abs(c.objectKinds.fill[k*needs+j]-c.states.fill[row*needs+j])<=1e-12,
           "object_kinds "+c.objectKinds.id[k]+": fill_hours of "+c.needs.id[j]+" differ from its state "+c.objectKinds.state[k]);
    }

  // Owners: a room's NPC by its full instance, its guild by number; a mobsi's NPC by instance, its guild by GIL_* name.
  const auto ownerNpc = [&](const std::string& id) {
    if(id.empty())
      return NoNpc;
    const auto it = byId.find(id);
    return it==byId.end() ? AbsentNpc : it->second;
    };
  std::vector<uint32_t> roomNpc(w.rooms.size());
  for(size_t r=0; r<w.rooms.size(); ++r)
    roomNpc[r] = ownerNpc(w.rooms.ownerNpc[r]);
  const auto room = [&](const std::string& name) -> std::pair<uint32_t,int32_t> {
    if(name.empty())
      return {NoNpc,-1};
    const auto it = std::lower_bound(w.rooms.name.begin(),w.rooms.name.end(),name);
    const auto r  = size_t(it-w.rooms.name.begin());
    return {roomNpc[r],w.rooms.ownerGuild[r]};
    };
  const size_t objects = w.objects.size(), points = w.points.size();
  w.objectOwnerNpc.resize(objects);
  w.objectOwnerGuild.resize(objects);
  for(size_t i=0; i<objects; ++i) {
    const auto& npc = w.objects.ownerNpc[i];
    const auto& gil = w.objects.ownerGuild[i];
    if(npc.empty() && gil.empty()) {
      std::tie(w.objectOwnerNpc[i],w.objectOwnerGuild[i]) = room(w.objects.room[i]);
      continue;
      }
    w.objectOwnerNpc[i] = ownerNpc(npc);
    const auto it = d.guildNames.find(gil);
    w.objectOwnerGuild[i] = gil.empty() ? -1 : it==d.guildNames.end() ? -2 : int32_t(it->second);
    }
  w.pointOwnerNpc.resize(points);
  w.pointOwnerGuild.resize(points);
  w.pointOutdoors.assign(points,0);
  for(size_t i=0; i<points; ++i) {
    std::tie(w.pointOwnerNpc[i],w.pointOwnerGuild[i]) = room(w.points.room[i]);
    w.pointOutdoors[i] = !w.points.free[i] && w.points.room[i].empty() ? 1 : 0;
    }

  }

// What the config gives this world: the state rows, each person's decay by traits and guild, the object kinds.
void applyConfig(World& w, const std::unordered_map<std::string,std::vector<uint8_t>>& cards) {
  const auto& c = w.config;
  w.stateRow.assign(w.stateNames.size(),NoState);
  for(size_t i=0; i<w.stateNames.size(); ++i) {
    const auto it = std::lower_bound(c.states.name.begin(),c.states.name.end(),w.stateNames[i]);
    if(it!=c.states.name.end() && *it==w.stateNames[i])
      w.stateRow[i] = uint16_t(it-c.states.name.begin());
    }
  const size_t n = w.npcs.size(), scales = c.scales.size(), needs = c.needs.size();
  w.npcCard.assign(n,0);
  w.npcScale.assign(n*scales,uint8_t(c.middle));
  w.npcDecay.assign(n*needs,0.0);
  // Needs are people's, but for the guilds of decisions.no_needs_guilds (orcs): those have none at all.
  const auto& noNeeds = c.decisions.noNeedsGuilds;
  w.npcNeeds.assign(n,0);
  for(uint32_t i=0; i<n; ++i) {
    if(auto it = cards.find(w.npcs.id[i]); it!=cards.end()) {
      w.npcCard[i] = 1;
      std::copy(it->second.begin(),it->second.end(),w.npcScale.begin()+std::ptrdiff_t(i*scales));
      }
    w.npcNeeds[i] = w.npcs.person[i] && std::find(noNeeds.begin(),noNeeds.end(),w.npcs.guild[i])==noNeeds.end() ? 1 : 0;
    if(!w.npcNeeds[i])
      continue;
    for(size_t k=0; k<needs; ++k) {
      double mult = 1.0;
      for(const auto& r:c.traits)
        if(r.need==k) {
          const double points = double(int(w.npcScale[i*scales+r.scale])-c.middle);
          const double change = r.perPoint*points;
          mult *= 1.0+change;
          }
      const auto& byGuild = c.needs.weightByGuild[k];
      const auto  g       = byGuild.find(w.npcs.guild[i]);
      const double weight = g==byGuild.end() ? c.needs.weightDefault[k] : g->second;
      const double decay  = c.needs.decay[k]*mult;
      w.npcDecay[i*needs+k] = decay*weight;
      }
    }
  const auto& o = c.objectKinds;
  w.objectKind.assign(w.objects.size(),NoKind);
  for(size_t i=0; i<w.objects.size(); ++i) {
    const auto it = std::lower_bound(o.scheme.begin(),o.scheme.end(),w.objects.scheme[i]);
    if(it!=o.scheme.end() && *it==w.objects.scheme[i])
      w.objectKind[i] = o.schemeKind[size_t(it-o.scheme.begin())];
    }
  w.pointKind.assign(w.points.size(),NoKind);
  for(size_t i=0; i<w.points.size(); ++i) {
    if(!w.points.free[i])
      continue;
    size_t best = 0;
    for(size_t j=0; j<o.prefix.size(); ++j)
      if(o.prefix[j].size()>best && w.points.name[i].rfind(o.prefix[j],0)==0) {
        best = o.prefix[j].size();
        w.pointKind[i] = o.prefixKind[j];
        }
    }
  }

// places.json of the server (places[] with ids, points: name -> place index): the place id of each point name.
std::unordered_map<std::string,std::string> readPlaces(const std::string& path) {
  std::unordered_map<std::string,std::string> ret;
  if(path.empty())
    return ret;
  std::ifstream in(path,std::ios::binary);
  need(bool(in),"cannot read the places "+path);
  std::ostringstream text;
  text << in.rdbuf();
  rapidjson::Document doc;
  const auto json = text.str();
  doc.Parse(json.data(),json.size());
  need(!doc.HasParseError() && doc.IsObject(),"the places "+path+" are not a JSON object");
  const auto& places = array(doc,"places");
  std::vector<std::string> ids;
  for(const auto& p:places.GetArray())
    ids.push_back(str(p,"id"));
  const auto& points = member(doc,"points");
  need(points.IsObject(),"places: points must be an object of name: place");
  for(auto it=points.MemberBegin(); it!=points.MemberEnd(); ++it) {
    need(it->value.IsUint() && it->value.GetUint()<ids.size(),"places: a point's place must be an index of places");
    ret.emplace(std::string(it->name.GetString(),it->name.GetStringLength()),ids[it->value.GetUint()]);
    }
  return ret;
  }

void readConfig(World& w, std::string_view json, const std::filesystem::path& dir) {
  rapidjson::Document doc;
  doc.Parse(json.data(),json.size());
  need(!doc.HasParseError(),"the file is not valid JSON");
  auto& c = w.config;
  c.loaded = true;
  c.hash   = fnv1a64(json);
  c.bytes  = json.size();
  onlyKeys(doc,{"format","version","note","needs","states","object_kinds","traits","decisions"},"the config");
  need(str(doc,"format")=="society-config","format must be society-config");
  const auto& version = member(doc,"version");
  need(version.IsInt() && version.GetInt()==1,"only version 1 is supported");
  c.version = version.GetInt();
  readNeeds(c,doc);
  std::unordered_map<std::string,std::vector<uint8_t>> cards;
  // "needs": [] turns the needs off: the sections that refer to them are not read.
  std::vector<std::string> protectedIds;
  if(c.needs.size()>0) {
    readStates(c,doc);
    readObjectKinds(c,doc);
    readTraits(c,doc,dir,cards);
    readDecisions(c,doc,dir);
    if(c.decisions.loaded) {
      protectedIds = readProtected(c.decisions.protectedPath);
      appendKindStates(w);
      }
    }
  applyConfig(w,cards);
  if(c.decisions.loaded) {
    applyDecisions(w,protectedIds);
    detail::applyReach(w,readPlaces(c.decisions.placesPath));
    }
  }

// The entry in force at each minute of the day, compressed to the minutes where it changes (cyclically).
void buildTimelines(World& w) {
  auto& n = w.npcs;
  n.timelineBegin.assign(1,0);
  std::vector<uint16_t> day(24*60);
  for(uint32_t i=0; i<n.size(); ++i) {
    if(w.entryCount(i)>0) {
      for(uint32_t m=0; m<day.size(); ++m)
        day[m] = routineAt(w,i,Time(m)*60);
      bool changes = false;
      for(uint32_t m=0; m<day.size(); ++m) {
        const auto before = day[m==0 ? day.size()-1 : m-1];
        if(day[m]!=before) {
          n.timelineMinute.push_back(uint16_t(m));
          n.timelineEntry.push_back(day[m]);
          changes = true;
          }
        }
      if(!changes) {
        n.timelineMinute.push_back(0);
        n.timelineEntry.push_back(day[0]);
        }
      }
    n.timelineBegin.push_back(uint32_t(n.timelineMinute.size()));
    }
  }

}

Time travelSeconds(int64_t lengthCm, double walkCmPerS, double gameSecondsPerRealSecond) {
  if(lengthCm<=0)
    return 0;
  return Time(std::llround(double(lengthCm)/walkCmPerS*gameSecondsPerRealSecond));
  }

uint16_t routineAt(const World& w, uint32_t npc, Time t, bool requirePoint) {
  const Time tod   = ((t%DaySeconds)+DaySeconds)%DaySeconds;
  const auto first = w.npcs.routineBegin[npc], last = w.npcs.routineBegin[npc+1];
  const auto& r    = w.routines;
  for(uint32_t i=first; i<last; ++i) {
    if(requirePoint && r.point[i]==NoPoint)
      continue;
    const Time s = Time(r.start[i])*60, e = Time(r.end[i])*60;
    if(e<s && (tod<e || s<=tod))
      return uint16_t(i-first);
    if(s<=tod && tod<e)
      return uint16_t(i-first);
    }
  // None holds the time: the entry that ended last before it, the last of equal ends.
  Time     delta = std::numeric_limits<Time>::max();
  uint16_t ret   = NoEntry;
  for(uint32_t i=first; i<last; ++i) {
    if(requirePoint && r.point[i]==NoPoint)
      continue;
    Time d = tod-Time(r.end[i])*60;
    if(d<0)
      d += DaySeconds;
    if(d<=delta) {
      ret   = uint16_t(i-first);
      delta = d;
      }
    }
  return ret;
  }

uint16_t entryAt(const World& w, uint32_t npc, Time t, Time* nextChange) {
  const auto first = w.npcs.timelineBegin[npc], last = w.npcs.timelineBegin[npc+1];
  if(nextChange!=nullptr)
    *nextChange = std::numeric_limits<Time>::max();
  if(first==last)
    return NoEntry;
  if(last-first==1)
    return w.npcs.timelineEntry[first];
  const Time tod  = ((t%DaySeconds)+DaySeconds)%DaySeconds;
  const Time base = t-tod;
  const auto* m   = w.npcs.timelineMinute.data();
  // The last change at or before the time of day; before the first change of the day, the day's last one.
  const auto it = std::upper_bound(m+first,m+last,tod,[](Time x, uint16_t minute) { return x<Time(minute)*60; });
  const uint32_t k = it==m+first ? last-1 : uint32_t(it-m)-1;
  if(nextChange!=nullptr)
    *nextChange = it==m+last ? base+DaySeconds+Time(m[first])*60 : base+Time(*it)*60;
  return w.npcs.timelineEntry[k];
  }

Time stateEndAt(const World& w, uint32_t npc, uint16_t entry, Time t) {
  const Time tod = ((t%DaySeconds)+DaySeconds)%DaySeconds;
  const Time day = t-tod;
  const auto k   = w.entryIndex(npc,entry);
  const Time s   = Time(w.routines.start[k])*60, e = Time(w.routines.end[k])*60;
  if(e<s)
    return tod<e ? day+e : day+DaySeconds+e;
  if(s<e)
    return (e<60*60 || e<tod) ? day+DaySeconds+e : day+e;
  if(e==0)
    return day+DaySeconds;
  return t;
  }

uint16_t startStateAt(const World& w, uint32_t npc, Time t) {
  const auto start = w.npcs.startState[npc];
  if(start==NoState || start!=w.schedulerState)
    return start;
  // Wld_IsTime(start,0,end,0) on the hour and minute of the day: [start, end), over midnight when end < start, never
  // when they are equal.
  const Time tod = ((t%DaySeconds)+DaySeconds)%DaySeconds;
  const auto& m  = w.schedules;
  for(uint32_t k=w.npcs.scheduleBegin[npc]; k<w.npcs.scheduleBegin[npc+1]; ++k) {
    if(m.start[k]==-1)
      return m.state[k];
    const Time b = Time(m.start[k])*3600, e = Time(m.end[k])*3600;
    if((b<=e && b<=tod && tod<e) || (e<b && (tod<e || b<=tod)))
      return m.state[k];
    }
  return start;
  }

World parseWorld(std::string_view json, std::string_view config, const std::filesystem::path& configDir) {
  World w;
  try {
    rapidjson::Document doc;
    doc.Parse(json.data(),json.size());
    need(!doc.HasParseError(),"the file is not valid JSON");
    w.exportHash  = fnv1a64(json);
    w.exportBytes = json.size();
    readHeader(w,doc);
    readRooms(w,doc);
    readPoints(w,doc);
    readEdges(w,doc);
    readObjects(w,doc);
    readNpcs(w,doc);
    buildTimelines(w);
    }
  catch(const Bad& e) {
    throw std::runtime_error(std::string("society-world: ")+e.what());
    }
  if(config.empty())
    return w;
  try {
    readConfig(w,config,configDir);
    }
  catch(const Bad& e) {
    throw std::runtime_error(std::string("society-config: ")+e.what());
    }
  return w;
  }

namespace {

std::string readFile(const std::filesystem::path& path, const char* what) {
  std::ifstream in(path,std::ios::binary);
  if(!in)
    throw std::runtime_error(std::string(what)+": cannot read "+path.string());
  std::ostringstream s;
  s << in.rdbuf();
  return s.str();
  }

}

World loadWorld(const std::filesystem::path& exportPath, const std::filesystem::path& configPath) {
  const auto json = readFile(exportPath,"society-world");
  if(configPath.empty())
    return parseWorld(json);
  const auto config = readFile(configPath,"society-config");
  if(config.empty())
    throw std::runtime_error("society-config: "+configPath.string()+" is empty");
  return parseWorld(json,config,configPath.parent_path());
  }

}
