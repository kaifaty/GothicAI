#include "society.h"

#include <rapidjson/document.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <numeric>
#include <sstream>
#include <stdexcept>

namespace Society {
namespace {

using Value = rapidjson::Value;

[[noreturn]] void bad(const std::string& what) {
  throw std::runtime_error("society-world: "+what);
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
  need(version.IsInt() && (version.GetInt()==1 || version.GetInt()==2),"only versions 1 and 2 are supported");
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

void readPoints(World& w, const Value& doc) {
  const auto& list = array(doc,"waypoints");
  auto& p = w.points;
  const size_t n = list.Size();
  p.name.resize(n); p.pos.resize(n); p.free.resize(n); p.graph.resize(n); p.graphCm.resize(n); p.component.resize(n);
  for(rapidjson::SizeType i=0; i<list.Size(); ++i) {
    const auto& v = list[i];
    need(uint(v,"id")==i,"waypoint ids must be 0..n-1 in order");
    p.name[i] = str(v,"name");
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

World parseWorld(std::string_view json) {
  rapidjson::Document doc;
  doc.Parse(json.data(),json.size());
  need(!doc.HasParseError(),"the file is not valid JSON");
  World w;
  w.exportHash  = fnv1a64(json);
  w.exportBytes = json.size();
  readHeader(w,doc);
  readPoints(w,doc);
  readEdges(w,doc);
  readObjects(w,doc);
  readNpcs(w,doc);
  buildTimelines(w);
  return w;
  }

World loadWorld(const std::filesystem::path& exportPath) {
  std::ifstream in(exportPath,std::ios::binary);
  if(!in)
    bad("cannot read "+exportPath.string());
  std::ostringstream s;
  s << in.rdbuf();
  return parseWorld(s.str());
  }

}
