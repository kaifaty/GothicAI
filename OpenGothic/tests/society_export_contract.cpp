// The society stand's world (docs/society-sim.md, «Интерфейсы → 1. Выгрузка мира», format v3): runs
// `gothic-body-runner --world NEWWORLD.ZEN --society-export` twice on the game's data (GOTHIC_DIR, as the other
// world contracts) and checks the file: every NPC of the world, the people and their routines entry by entry
// against evals/npc-decisions/day_people.json, every routine and start point resolved, the point names against
// server/src/gothic_ai/gamedata/places.json, symmetric edges, a scheme and a waynet point for every object, the v2
// fields (start_aistate, own walk speed, mm_schedule), room ownership and the same bytes from both runs.
#include "../common/game/constants.h"

#include <rapidjson/document.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <numeric>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include <unistd.h>

namespace {

namespace fs = std::filesystem;
using Value = rapidjson::Value;

constexpr size_t WorldNpcs = 1053;  // the startup scripts insert every NPC of Khorinis (docs/world-streaming.md)

// The one expected difference from day_people.json, the secondary reference (the export is the fresh world).
// Xardas starts with Rtn_PreStart_100 (TA_Stand_ArmsCrossed at NW_XARDAS_START), as the npc-day-parity journal
// keeps him all day. day_people.json is a SCENARIO=places dump taken after his first talk, whose exit
// (DIA_Xardas_FirstEXIT) calls Npc_ExchangeRoutine(self,"START"): TA_Read_Bookstand at NW_XARDAS_TOWER_IN1_28.
// Anything else for him, or no difference at all, fails: then this exception is out of date.
struct ExpectedDifference { const char* symbol; const char* exportRoutine; const char* dayPeopleRoutine; };
constexpr ExpectedDifference Xardas = {
  "NONE_100_XARDAS",
  "480-1380 ZS_STAND_ARMSCROSSED NW_XARDAS_START; 1380-480 ZS_STAND_ARMSCROSSED NW_XARDAS_START;",
  "480-1380 ZS_READ_BOOKSTAND NW_XARDAS_TOWER_IN1_28; 1380-480 ZS_READ_BOOKSTAND NW_XARDAS_TOWER_IN1_28;",
  };
int failures = 0;

void fail(const std::string& what) {
  std::printf("FAIL: %s\n",what.c_str());
  ++failures;
  }

void need(bool condition, const std::string& what) {
  if(!condition)
    throw std::runtime_error(what);
  }

std::string read(const fs::path& path) {
  std::ifstream in(path,std::ios::binary);
  need(bool(in),"cannot read "+path.string());
  std::ostringstream s;
  s << in.rdbuf();
  return s.str();
  }

std::string shellQuote(const std::string& s) {
  std::string ret = "'";
  for(char c:s)
    ret += c=='\'' ? std::string("'\\''") : std::string(1,c);
  return ret+"'";
  }

std::vector<std::string> lines(const std::string& text) {
  std::vector<std::string> ret;
  std::istringstream in(text);
  for(std::string line; std::getline(in,line);)
    ret.push_back(line);
  return ret;
  }

double exportOnce(const std::string& gothic, const fs::path& out, const fs::path& log) {
  const auto cmd = shellQuote(SOCIETY_RUNNER)+" -g "+shellQuote(gothic)+" --world NEWWORLD.ZEN --society-export "+
                   shellQuote(out.string())+" >"+shellQuote(log.string())+" 2>&1";
  const auto from = std::chrono::steady_clock::now();
  const int  rc   = std::system(cmd.c_str());
  const auto sec  = std::chrono::duration<double>(std::chrono::steady_clock::now()-from).count();
  std::string text;
  try { text = read(log); } catch(const std::exception&) {}
  const auto all = lines(text);
  if(rc!=0) {
    for(size_t i=all.size()>40 ? all.size()-40 : 0; i<all.size(); ++i)
      std::printf("  | %s\n",all[i].c_str());
    throw std::runtime_error("gothic-body-runner --society-export exited with "+std::to_string(rc));
    }
  for(const auto& line:all)
    if(line.rfind("society export:",0)==0)
      std::printf("%s\n",line.c_str());
  std::printf("runner wall time: %.1f s\n",sec);
  return sec;
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

uint32_t uint(const Value& v, const char* key) {
  const auto& m = member(v,key);
  need(m.IsUint(),std::string(key)+" must be a non-negative integer");
  return m.GetUint();
  }

int integer(const Value& v, const char* key) {
  const auto& m = member(v,key);
  need(m.IsInt(),std::string(key)+" must be an integer");
  return m.GetInt();
  }

double real(const Value& v, const char* key) {
  const auto& m = member(v,key);
  need(m.IsNumber(),std::string(key)+" must be a number");
  return m.GetDouble();
  }

const Value& array(const Value& v, const char* key) {
  const auto& m = member(v,key);
  need(m.IsArray(),std::string(key)+" must be an array");
  return m;
  }

std::string upper(std::string s) {
  for(auto& c:s)
    c = char(std::toupper(static_cast<unsigned char>(c)));
  return s;
  }

// "1×37" for runs of equal sizes.
std::string sizes(const std::vector<size_t>& v) {
  std::string ret;
  for(size_t i=0; i<v.size();) {
    size_t j = i;
    while(j<v.size() && v[j]==v[i])
      ++j;
    ret += (ret.empty() ? "" : ", ")+std::to_string(v[i])+(j-i>1 ? "x"+std::to_string(j-i) : "");
    i = j;
    }
  return ret;
  }

std::string nullableText(const Value& v, const char* key) {
  const auto& value = member(v,key);
  need(value.IsNull() || (value.IsString() && value.GetStringLength()>0),std::string(key)+" is null or a nonempty name");
  return value.IsString() ? std::string(value.GetString(),value.GetStringLength()) : std::string();
  }

struct Waypoint { std::string name; bool free = false; std::string room; };

std::vector<Waypoint> checkWaypoints(const Value& doc) {
  const auto& list = array(doc,"waypoints");
  std::vector<Waypoint> ret;
  for(rapidjson::SizeType i=0; i<list.Size(); ++i) {
    const auto& w = list[i];
    need(uint(w,"id")==i,"waypoint ids must be 0..n-1 in order");
    need(member(w,"freepoint").IsBool(),"freepoint must be a bool");
    need(array(w,"pos").Size()==3,"pos must have three numbers");
    ret.push_back({str(w,"name"),member(w,"freepoint").GetBool(),nullableText(w,"room")});
    if(ret.back().name.empty())
      fail("waypoint "+std::to_string(i)+" has no name");
    }
  size_t free = 0;
  for(rapidjson::SizeType i=0; i<list.Size(); ++i) {
    const auto& near = member(list[i],"near_waypoint");
    if(!ret[i].free) {
      if(!near.IsNull())
        fail("waynet point "+ret[i].name+" must have near_waypoint null");
      continue;
      }
    ++free;
    if(!near.IsUint() || near.GetUint()>=ret.size() || ret[near.GetUint()].free)
      fail("freepoint "+ret[i].name+" has no near_waypoint on the waynet");
    }
  std::printf("waypoints: %zu (waynet %zu, freepoints %zu)\n",ret.size(),ret.size()-free,free);
  return ret;
  }

struct Room { std::string owner; int32_t guild = -1; };
using Rooms = std::map<std::string,Room>;

Rooms checkRooms(const Value& doc, const std::vector<Waypoint>& points) {
  Rooms ret;
  std::string previous;
  for(const auto& r:array(doc,"rooms").GetArray()) {
    const auto name = str(r,"name");
    need(!name.empty() && (ret.empty() || previous<name),"rooms must have nonempty names in sorted order without repeats");
    previous = name;
    Room room;
    room.owner = nullableText(r,"owner_npc");
    const auto& guild = member(r,"owner_guild");
    need(guild.IsNull() || (guild.IsInt() && guild.GetInt()>0),"room owner_guild must be null or a positive guild id");
    if(guild.IsInt()) {
      need(guild.GetInt()!=int32_t(GIL_PUBLIC),"public rooms must have owner_guild null");
      room.guild = guild.GetInt();
      }
    ret.emplace(name,room);
    }
  for(const auto& p:points)
    if(!p.room.empty() && !ret.count(p.room))
      fail("waypoint "+p.name+" names missing room "+p.room);
  return ret;
  }

void checkRoomOwners(const fs::path& log, const Rooms& rooms) {
  constexpr std::string_view mapping = "society room npc: ";
  constexpr std::string_view missing = "society room npc missing: ";
  constexpr std::string_view ambiguous = "society room npc ambiguous: ";
  constexpr std::string_view guildError = "society room guild unresolved: ";
  std::set<std::string> mappings;
  std::vector<std::string> errors;
  size_t absent = 0, conflicts = 0, guildErrors = 0;
  for(const auto& line:lines(read(log))) {
    if(!line.starts_with("society room "))
      continue;
    std::printf("%s\n",line.c_str());
    if(line.starts_with(mapping))
      mappings.insert(line.substr(mapping.size()));
    else if(line.starts_with(missing))
      ++absent;
    else if(line.starts_with(ambiguous)) {
      ++conflicts;
      errors.push_back(line);
      }
    else if(line.starts_with(guildError)) {
      ++guildErrors;
      errors.push_back(line);
      }
    }
  size_t owned = 0;
  for(const auto& [name,room]:rooms) {
    owned += !room.owner.empty() || room.guild>=0 ? 1 : 0;
    if(!room.owner.empty() && !mappings.count(name+" -> "+room.owner))
      fail("room "+name+" has no canonical NPC mapping in the export log");
    }
  std::printf("rooms: %zu, with an owner: %zu\n",rooms.size(),owned);
  std::printf("room NPC mappings: %zu; missing: %zu; ambiguous: %zu; unresolved guilds: %zu\n",
              mappings.size(),absent,conflicts,guildErrors);
  for(const auto& error:errors)
    fail(error);
  }

void checkPlaces(const std::vector<Waypoint>& points, const fs::path& placesPath) {
  rapidjson::Document places;
  const auto text = read(placesPath);
  places.Parse(text.data(),text.size());
  need(!places.HasParseError() && places.IsObject() && places.HasMember("points") && places["points"].IsObject(),
       "places.json has no points");
  std::set<std::string> book;
  for(auto it=places["points"].MemberBegin(); it!=places["points"].MemberEnd(); ++it)
    book.insert(upper(it->name.GetString()));
  std::map<std::string,std::vector<const Waypoint*>> names;
  for(const auto& p:points)
    names[upper(p.name)].push_back(&p);
  size_t repeated = 0, extra = 0;
  std::vector<std::string> examples;
  for(const auto& [name,list]:names)
    if(list.size()>1) {
      ++repeated;
      extra += list.size()-1;
      if(examples.size()<8)
        examples.push_back(name+" x"+std::to_string(list.size()));
      }
  std::printf("point names: %zu points, %zu names; places.json: %zu names\n",points.size(),names.size(),book.size());
  if(repeated>0) {
    std::string list;
    for(const auto& e:examples)
      list += (list.empty() ? "" : ", ")+e;
    std::printf("  %zu names repeat (%zu points more than names): places.json keys its points by name, so a repeated "
                "name is one key there; ids in the export tell them apart. E.g. %s\n",repeated,extra,list.c_str());
    }
  std::vector<std::string> onlyWorld, onlyBook;
  for(const auto& [name,list]:names)
    if(!book.count(name))
      onlyWorld.push_back(name+(list.front()->free ? " (freepoint)" : " (waynet)"));
  for(const auto& name:book)
    if(!names.count(name))
      onlyBook.push_back(name);
  for(const auto& n:onlyWorld)
    std::printf("  only in the world: %s\n",n.c_str());
  for(const auto& n:onlyBook)
    std::printf("  only in places.json: %s\n",n.c_str());
  if(!onlyWorld.empty() || !onlyBook.empty())
    fail(std::to_string(onlyWorld.size())+" point names only in the world and "+std::to_string(onlyBook.size())+
         " only in places.json: the book of places was built from a SCENARIO=places dump of the world "
         "(docs/npc-agent.md, «Названия мест»), so the world's points changed since; rebuild the book or explain");
  }

void checkEdges(const Value& doc, const std::vector<Waypoint>& points) {
  const auto& list = array(doc,"edges");
  std::map<std::pair<uint32_t,uint32_t>,int> len;
  std::pair<uint32_t,uint32_t> previous = {0,0};
  size_t bad = 0;
  for(rapidjson::SizeType i=0; i<list.Size(); ++i) {
    const auto& e = list[i];
    need(e.IsArray() && e.Size()==3 && e[0].IsUint() && e[1].IsUint() && e[2].IsInt(),"an edge is [a, b, length_cm]");
    const auto a = e[0].GetUint(), b = e[1].GetUint();
    const auto l = e[2].GetInt();
    if(a>=points.size() || b>=points.size() || points[a].free || points[b].free || a==b || l<=0) {
      if(++bad<=10)
        fail("edge "+std::to_string(a)+" -> "+std::to_string(b)+" ("+std::to_string(l)+" cm) must join two waynet points, longer than 0");
      continue;
      }
    need(i==0 || previous<std::make_pair(a,b),"edges must be sorted by (a, b) without repeats");
    previous = {a,b};
    len[{a,b}] = l;
    }
  size_t oneWay = 0;
  for(const auto& [ab,l]:len) {
    auto back = len.find({ab.second,ab.first});
    if(back==len.end() || back->second!=l) {
      if(++oneWay<=10)
        fail("edge "+points[ab.first].name+" -> "+points[ab.second].name+" has no reverse of the same length");
      }
    }
  if(oneWay>10)
    fail(std::to_string(oneWay)+" edges without a reverse in all");
  std::vector<uint32_t> parent(points.size());
  std::iota(parent.begin(),parent.end(),0u);
  std::function<uint32_t(uint32_t)> root = [&](uint32_t x) { return parent[x]==x ? x : parent[x] = root(parent[x]); };
  for(const auto& [ab,l]:len)
    parent[root(ab.first)] = root(ab.second);
  std::map<uint32_t,size_t> component;
  for(uint32_t i=0; i<points.size(); ++i)
    if(!points[i].free)
      ++component[root(i)];
  std::vector<size_t> size;
  for(const auto& [r,n]:component)
    size.push_back(n);
  std::sort(size.rbegin(),size.rend());
  std::printf("edges: %zu directed (%zu pairs), lengths > 0, all symmetric: %s\n",len.size(),len.size()/2,
              oneWay==0 && bad==0 ? "yes" : "no");
  std::printf("waynet components: %zu; points in each: %s\n",size.size(),sizes(size).c_str());
  }

void checkObjects(const Value& doc, const std::vector<Waypoint>& points, const std::set<std::string>& npcIds,
                  const Rooms& rooms) {
  const auto& list = array(doc,"objects");
  std::map<std::string,size_t> schemes;
  size_t npcOwned = 0, guildOwned = 0, owned = 0, containers = 0, strangers = 0, noScheme = 0;
  size_t beds = 0, ownedBeds = 0, mobOwnedBeds = 0, roomOwnedBeds = 0;
  std::set<std::string> unknownOwners;
  for(rapidjson::SizeType i=0; i<list.Size(); ++i) {
    const auto& o = list[i];
    need(uint(o,"id")==i,"object ids must be 0..n-1 in order");
    str(o,"vob"); str(o,"focus");
    need(array(o,"pos").Size()==3,"object pos must have three numbers");
    const auto scheme = str(o,"scheme");
    if(scheme.empty() && ++noScheme<=10)
      fail("object "+std::to_string(i)+" ("+str(o,"vob")+") has no scheme");
    ++schemes[scheme];
    const auto wp = uint(o,"waypoint");
    if(wp>=points.size() || points[wp].free)
      fail("object "+std::to_string(i)+" has no nearest waynet point");
    const auto& npc   = member(o,"owner_npc");
    const auto& guild = member(o,"owner_guild");
    const auto roomName = nullableText(o,"room");
    const auto room = rooms.find(roomName);
    if(!roomName.empty() && room==rooms.end())
      fail("object "+std::to_string(i)+" names missing room "+roomName);
    need((npc.IsNull() || (npc.IsString() && npc.GetStringLength()>0)) &&
         (guild.IsNull() || (guild.IsString() && guild.GetStringLength()>0)),"owners are null or a name");
    need(member(o,"container").IsBool(),"container must be a bool");
    npcOwned   += npc.IsString() ? 1 : 0;
    guildOwned += guild.IsString() ? 1 : 0;
    owned      += npc.IsString() || guild.IsString() ? 1 : 0;
    if(upper(scheme).starts_with("BED")) {
      ++beds;
      const bool mobOwned = npc.IsString() || guild.IsString();
      const bool roomOwned = room!=rooms.end() && (!room->second.owner.empty() || room->second.guild>=0);
      mobOwnedBeds += mobOwned ? 1 : 0;
      roomOwnedBeds += roomOwned ? 1 : 0;
      ownedBeds += mobOwned || roomOwned ? 1 : 0;
      }
    containers += o["container"].GetBool() ? 1 : 0;
    if(npc.IsString() && !npcIds.count(npc.GetString())) {
      ++strangers;
      unknownOwners.insert(npc.GetString());
      }
    }
  if(noScheme>10)
    fail(std::to_string(noScheme)+" objects without a scheme in all");
  std::vector<std::pair<size_t,std::string>> bySize;
  for(const auto& [s,n]:schemes)
    bySize.push_back({n,s});
  std::sort(bySize.begin(),bySize.end(),[](const auto& a, const auto& b) { return a.first!=b.first ? a.first>b.first : a.second<b.second; });
  std::string text;
  for(const auto& [n,s]:bySize)
    text += (text.empty() ? "" : ", ")+(s.empty() ? std::string("(none)") : s)+" "+std::to_string(n);
  std::printf("objects: %u, containers %zu; by scheme: %s\n",list.Size(),containers,text.c_str());
  std::printf("objects with an owner: %zu (an NPC %zu, a guild %zu)\n",owned,npcOwned,guildOwned);
  std::printf("beds: %zu, with an owner from mobsi or room: %zu (mobsi %zu, room %zu)\n",
              beds,ownedBeds,mobOwnedBeds,roomOwnedBeds);
  if(strangers>0) {
    std::string names;
    for(const auto& n:unknownOwners)
      names += (names.empty() ? "" : ", ")+n;
    std::printf("  %zu objects name an owner NPC that is not in the world at the start: %s\n",strangers,names.c_str());
    }
  }

void checkSleepRooms(const Value& doc, const std::vector<Waypoint>& points, const Rooms& rooms) {
  size_t sleepers = 0, ownRooms = 0;
  for(const auto& n:array(doc,"npcs").GetArray()) {
    if(!member(n,"person").GetBool())
      continue;
    bool sleeps = false, own = false;
    for(const auto& r:array(n,"routine").GetArray()) {
      const auto state = str(r,"state");
      if(!state.starts_with("ZS_GOTOBED") && !state.starts_with("ZS_SLEEP"))
        continue;
      sleeps = true;
      const auto& wp = member(r,"waypoint");
      if(!wp.IsUint() || wp.GetUint()>=points.size())
        continue;
      const auto room = rooms.find(points[wp.GetUint()].room);
      if(room==rooms.end())
        continue;
      own |= room->second.owner==str(n,"id") || room->second.guild==int32_t(uint(n,"guild"));
      }
    sleepers += sleeps ? 1 : 0;
    ownRooms += own ? 1 : 0;
    }
  std::printf("people with a sleep routine: %zu; sleep waypoint in own or guild room: %zu\n",sleepers,ownRooms);
  }

// The record of a person as day_people.json has it: name, guild, routine entries (start, end, state, point name).
std::string personRecord(const std::string& name, uint32_t guild, const Value& routine) {
  std::string ret = name+" | guild "+std::to_string(guild)+" |";
  for(rapidjson::SizeType i=0; i<routine.Size(); ++i) {
    const auto& r = routine[i];
    // The export names the point in waypoint_name; day_people.json in waypoint.
    ret += " "+std::to_string(integer(r,"start"))+"-"+std::to_string(integer(r,"end"))+" "+str(r,"state")+" "+
           str(r,r.HasMember("waypoint_name") ? "waypoint_name" : "waypoint")+";";
    }
  return ret;
  }

// v2: start_aistate (the state the engine runs without a routine), the NPC's own walk speed and, for the monster
// dispatcher, its hours. Every NPC without a routine but the hero must have a start_aistate; every person a walk
// speed; mm_schedule exactly for ZS_MM_ALLSCHEDULER, with the six ZS_MM_RTN_* windows of its hours.
struct V2 {
  std::map<std::string,size_t> noRoutineStates;
  size_t aistates = 0, schedules = 0, peopleWalk = 0;
  std::vector<double> walk;

  void print(size_t persons) const {
    std::string text;
    for(const auto& [s,n]:noRoutineStates)
      text += (text.empty() ? "" : ", ")+s+" "+std::to_string(n);
    std::printf("v2: start_aistate %zu, mm_schedule %zu; NPCs without a routine by start_aistate: %s\n",aistates,
                schedules,text.c_str());
    auto v = walk;
    std::sort(v.begin(),v.end());
    if(!v.empty())
      std::printf("v2: own walk speed of %zu of %zu people, %.1f..%.1f cm/s, median %.1f\n",peopleWalk,persons,v.front(),
                  v.back(),v[v.size()/2]);
    if(peopleWalk!=persons)
      fail(std::to_string(persons-peopleWalk)+" people have no walk speed of their own");
    }
  };

void checkV2(const Value& n, const std::string& key, bool hasRoutine, V2& v2) {
  const auto& aistate  = member(n,"start_aistate");
  const auto& schedule = member(n,"mm_schedule");
  const double walk    = real(n,"walk_cm_per_s");
  need(aistate.IsNull() || (aistate.IsString() && aistate.GetStringLength()>0),"start_aistate is null or a name");
  if(walk<0)
    fail("npc "+key+": walk_cm_per_s is negative");
  if(n["person"].GetBool() && walk>0) {
    ++v2.peopleWalk;
    v2.walk.push_back(walk);
    }
  const std::string state = aistate.IsString() ? aistate.GetString() : "";
  v2.aistates += state.empty() ? 0 : 1;
  if(!hasRoutine) {
    ++v2.noRoutineStates[state.empty() ? "null" : state];
    if(state.empty() && key!="PC_HERO#0")
      fail("npc "+key+" has neither a routine nor a start_aistate");
    }
  if((state=="ZS_MM_ALLSCHEDULER")!=schedule.IsArray()) {
    fail("npc "+key+": mm_schedule must be given exactly for start_aistate ZS_MM_ALLSCHEDULER");
    return;
    }
  if(!schedule.IsArray())
    return;
  ++v2.schedules;
  static const char* names[] = {"ZS_MM_RTN_SLEEP","ZS_MM_RTN_REST","ZS_MM_RTN_ROAM","ZS_MM_RTN_EATGROUND",
                                "ZS_MM_RTN_WUSEL","ZS_MM_RTN_ORCSIT"};
  bool ok = schedule.Size()==std::size(names);
  for(rapidjson::SizeType j=0; ok && j<schedule.Size(); ++j) {
    const auto& x = schedule[j];
    ok = x.IsArray() && x.Size()==3 && x[0].IsString() && std::string(x[0].GetString())==names[j] && x[1].IsInt() &&
         x[2].IsInt() && x[1].GetInt()>=-1 && x[1].GetInt()<24 && x[2].GetInt()>=-1 && x[2].GetInt()<24;
    }
  if(!ok)
    fail("npc "+key+": mm_schedule must be the six [ZS_MM_RTN_*, start hour, end hour] windows in the scheduler's order");
  }

std::set<std::string> checkNpcs(const Value& doc, const std::vector<Waypoint>& points, const fs::path& dayPeoplePath) {
  const auto& list = array(doc,"npcs");
  V2 v2;
  std::printf("npcs: %u\n",list.Size());
  if(list.Size()!=WorldNpcs)
    fail("the world must have all "+std::to_string(WorldNpcs)+" NPCs, the export has "+std::to_string(list.Size()));
  std::set<std::string> ids, keys;
  std::map<std::string,std::vector<std::string>> people;
  std::string previous;
  uint32_t ordinal = 0;
  size_t entries = 0, persons = 0;
  std::vector<std::string> unresolved;
  for(rapidjson::SizeType i=0; i<list.Size(); ++i) {
    const auto& n  = list[i];
    const auto id  = str(n,"id");
    ordinal = i>0 && id==previous ? ordinal+1 : 0;
    need(i==0 || previous<=id,"npcs must be sorted by id");
    previous = id;
    const auto key = str(n,"key");
    if(key!=id+"#"+std::to_string(ordinal) || !keys.insert(key).second)
      fail("npc "+id+" has key "+key+", expected "+id+"#"+std::to_string(ordinal));
    ids.insert(id);
    need(member(n,"person").IsBool(),"person must be a bool");
    if(uint(n,"start_waypoint")>=points.size())
      fail("npc "+key+": start_waypoint is not a point");
    const auto& routine = array(n,"routine");
    for(rapidjson::SizeType j=0; j<routine.Size(); ++j) {
      const auto& r     = routine[j];
      const auto  start = integer(r,"start"), end = integer(r,"end");
      const auto  name  = str(r,"waypoint_name");
      if(start<0 || start>=1440 || end<0 || end>=1440 || str(r,"state").empty())
        fail("npc "+key+": routine entry "+std::to_string(j)+" has a bad time or no state");
      ++entries;
      const auto& wp = member(r,"waypoint");
      if(wp.IsUint() && wp.GetUint()<points.size() && points[wp.GetUint()].name==name)
        continue;
      if(!wp.IsNull()) {
        fail("npc "+key+": routine point "+name+" has a wrong id");
        continue;
        }
      // v1 allows null only where the engine itself finds no point (World::findPoint): no point of that name.
      const bool inWorld = std::any_of(points.begin(),points.end(),[&](const Waypoint& p) { return p.name==name; });
      if(inWorld)
        fail("npc "+key+": routine point "+name+" is in the world but has no id");
      else
        unresolved.push_back(key+" "+std::to_string(start)+"-"+std::to_string(end)+" "+str(r,"state")+" "+name);
      }
    if(n["person"].GetBool()) {
      ++persons;
      people[id].push_back(personRecord(str(n,"name"),uint(n,"guild"),routine));
      }
    checkV2(n,key,routine.Size()>0,v2);
    }
  v2.print(persons);
  std::printf("people: %zu; routine entries of all NPCs: %zu; with waypoint null (no such point in the world): %zu\n",
              persons,entries,unresolved.size());
  for(const auto& u:unresolved)
    std::printf("  %s\n",u.c_str());

  rapidjson::Document day;
  const auto text = read(dayPeoplePath);
  day.Parse(text.data(),text.size());
  need(!day.HasParseError() && day.IsArray(),"day_people.json must be an array");
  std::map<std::string,std::vector<std::string>> reference;
  size_t referenceEntries = 0, exportEntries = 0;
  for(rapidjson::SizeType i=0; i<day.Size(); ++i) {
    const auto& p = day[i];
    reference[str(p,"npc")].push_back(personRecord(str(p,"name"),uint(p,"guild"),array(p,"routine")));
    referenceEntries += array(p,"routine").Size();
    }
  for(rapidjson::SizeType i=0; i<list.Size(); ++i)
    if(list[i]["person"].GetBool())
      exportEntries += list[i]["routine"].Size();
  std::printf("day_people.json: %u people, %zu routine entries; export: %zu people, %zu routine entries\n",
              day.Size(),referenceEntries,persons,exportEntries);
  std::set<std::string> symbols;
  for(const auto& [s,r]:people) symbols.insert(s);
  for(const auto& [s,r]:reference) symbols.insert(s);
  if(!symbols.count(Xardas.symbol))
    fail(std::string(Xardas.symbol)+" is in neither the export nor day_people.json (the exception is out of date)");
  size_t differences = 0;
  for(const auto& s:symbols) {
    auto a = people.count(s) ? people[s] : std::vector<std::string>();
    auto b = reference.count(s) ? reference[s] : std::vector<std::string>();
    std::sort(a.begin(),a.end());
    std::sort(b.begin(),b.end());
    if(s==Xardas.symbol) {
      const auto routine = [](const std::string& r) { return r.substr(r.rfind(" | ")+3); };
      const auto person  = [](const std::string& r) { return r.substr(0,r.rfind(" | ")); };
      if(a.size()==1 && b.size()==1 && person(a[0])==person(b[0]) &&
         routine(a[0])==Xardas.exportRoutine && routine(b[0])==Xardas.dayPeopleRoutine) {
        std::printf("  %s: the expected difference: the fresh world's %s day_people.json's %s\n",s.c_str(),
                    Xardas.exportRoutine,Xardas.dayPeopleRoutine);
        continue;
        }
      fail(s+": not the expected difference from day_people.json (the exception is out of date)");
      }
    if(a==b)
      continue;
    ++differences;
    std::printf("  %s:\n",s.c_str());
    for(const auto& r:b)
      if(!std::binary_search(a.begin(),a.end(),r))
        std::printf("    day_people.json: %s\n",r.c_str());
    for(const auto& r:a)
      if(!std::binary_search(b.begin(),b.end(),r))
        std::printf("    export:          %s\n",r.c_str());
    }
  if(differences>0)
    fail(std::to_string(differences)+" people differ from day_people.json (listed above)");
  else
    std::printf("people and routines match day_people.json record by record, but the expected difference above\n");
  return ids;
  }

void checkHeader(const Value& doc) {
  need(str(doc,"format")=="society-world" && integer(doc,"version")==3,"format must be society-world version 3");
  if(str(doc,"world")!="NEWWORLD.ZEN")
    fail("world must be NEWWORLD.ZEN");
  const auto fingerprint = str(doc,"data_fingerprint");
  if(fingerprint.size()!=64 || fingerprint.find_first_not_of("0123456789abcdef")!=std::string::npos)
    fail("data_fingerprint must be the base's 64 hex digits, not "+fingerprint);
  const auto commit = str(doc,"engine_commit");
  if(commit.empty())
    fail("engine_commit must be a git sha or unknown");
  const auto& start = member(doc,"start");
  if(integer(start,"day")!=0 || integer(start,"minute")!=480)
    fail("the export must be taken at day 0, 08:00");
  const double clock = real(member(doc,"clock"),"game_seconds_per_real_second");
  if(clock!=14.5)
    fail("game_seconds_per_real_second must be 14.5");
  const auto& movement = member(doc,"movement");
  const double walk = real(movement,"walk_cm_per_s"), run = real(movement,"run_cm_per_s");
  if(!(walk>0 && run>walk) || str(movement,"source").empty())
    fail("movement needs walk > 0, run > walk and a source");
  std::printf("world %s, base %s, engine %s, start day 0 08:00, clock %.1f\n",str(doc,"world").c_str(),
              fingerprint.c_str(),commit.c_str(),clock);
  std::printf("movement: walk %.1f cm/s, run %.1f cm/s; %s\n",walk,run,str(movement,"source").c_str());
  }

void sameBytes(const std::string& a, const std::string& b) {
  if(a==b) {
    std::printf("two exports in a row: the same %zu bytes\n",a.size());
    return;
    }
  const auto la = lines(a), lb = lines(b);
  for(size_t i=0; i<std::max(la.size(),lb.size()); ++i) {
    const auto& x = i<la.size() ? la[i] : std::string("(end)");
    const auto& y = i<lb.size() ? lb[i] : std::string("(end)");
    if(x!=y) {
      std::printf("  line %zu differs:\n    1: %s\n    2: %s\n",i+1,x.c_str(),y.c_str());
      break;
      }
    }
  fail("two exports in a row differ ("+std::to_string(a.size())+" and "+std::to_string(b.size())+" bytes)");
  }

}

int main() {
  try {
    const auto* gothic = std::getenv("GOTHIC_DIR");
    need(gothic!=nullptr && *gothic!='\0',"GOTHIC_DIR must point to prepared Gothic II settings");
    const fs::path repo = SOCIETY_REPO;
    const fs::path root = fs::current_path()/("society-export-contract-"+std::to_string(getpid()));
    struct Cleanup { fs::path path; ~Cleanup() { std::error_code e; fs::remove_all(path,e); } } cleanup{root};
    fs::create_directories(root);
    exportOnce(gothic,root/"a.json",root/"a.log");
    exportOnce(gothic,root/"b.json",root/"b.log");
    const auto a = read(root/"a.json");
    sameBytes(a,read(root/"b.json"));

    rapidjson::Document doc;
    doc.Parse(a.data(),a.size());
    need(!doc.HasParseError() && doc.IsObject(),"the export is not a JSON object");
    checkHeader(doc);
    const auto points = checkWaypoints(doc);
    checkPlaces(points,repo/"server/src/gothic_ai/gamedata/places.json");
    checkEdges(doc,points);
    const auto ids = checkNpcs(doc,points,repo/"evals/npc-decisions/day_people.json");
    const auto rooms = checkRooms(doc,points);
    checkRoomOwners(root/"a.log",rooms);
    checkObjects(doc,points,ids,rooms);
    checkSleepRooms(doc,points,rooms);
    std::fflush(stdout);
    if(failures>0) {
      std::fprintf(stderr,"FAIL: society export contract: %d checks failed\n",failures);
      return EXIT_FAILURE;
      }
    std::puts("Society export contracts passed");
    return EXIT_SUCCESS;
    }
  catch(const std::exception& e) {
    std::fprintf(stderr,"FAIL: society export contract: %s\n",e.what());
    return EXIT_FAILURE;
    }
  }
