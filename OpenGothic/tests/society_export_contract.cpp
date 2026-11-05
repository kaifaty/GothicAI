// The society stand's world (docs/society-sim.md, «Интерфейсы → 1. Выгрузка мира»): runs
// `gothic-body-runner --world NEWWORLD.ZEN --society-export` twice on the game's data (GOTHIC_DIR, as the other
// world contracts) and checks the file: every NPC of the world, the people and their routines entry by entry
// against evals/npc-decisions/day_people.json, every routine and start point resolved, the point names against
// server/src/gothic_ai/gamedata/places.json, symmetric edges, a scheme and a waynet point for every object, and
// the same bytes from both runs.
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
#include <vector>
#include <unistd.h>

namespace {

namespace fs = std::filesystem;
using Value = rapidjson::Value;

constexpr size_t WorldNpcs = 1053;  // the startup scripts insert every NPC of Khorinis (docs/world-streaming.md)
// Routine points the world does not have. The engine does not find them either (Npc::addRoutine keeps the name,
// Routine::point stays null, the routine placement skips the entry), so the export writes "waypoint": null.
const std::map<std::string,std::string> KnownMissingPoints = {
  {"NW_MONASTERY_SNACTUM_02","Garwig's (NOV_608_GARWIG) night entry: a typo of NW_MONASTERY_SANCTUM_02 in the scripts"},
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
  const auto cmd = shellQuote(SOCIETY_RUNNER)+" -g "+shellQuote(gothic)+" -g2 --world NEWWORLD.ZEN --society-export "+
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

struct Waypoint { std::string name; bool free = false; };

std::vector<Waypoint> checkWaypoints(const Value& doc) {
  const auto& list = array(doc,"waypoints");
  std::vector<Waypoint> ret;
  for(rapidjson::SizeType i=0; i<list.Size(); ++i) {
    const auto& w = list[i];
    need(uint(w,"id")==i,"waypoint ids must be 0..n-1 in order");
    need(member(w,"freepoint").IsBool(),"freepoint must be a bool");
    need(array(w,"pos").Size()==3,"pos must have three numbers");
    ret.push_back({str(w,"name"),member(w,"freepoint").GetBool()});
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

void checkObjects(const Value& doc, const std::vector<Waypoint>& points, const std::set<std::string>& npcIds) {
  const auto& list = array(doc,"objects");
  std::map<std::string,size_t> schemes;
  size_t npcOwned = 0, guildOwned = 0, owned = 0, containers = 0, strangers = 0, noScheme = 0;
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
    need((npc.IsNull() || (npc.IsString() && npc.GetStringLength()>0)) &&
         (guild.IsNull() || (guild.IsString() && guild.GetStringLength()>0)),"owners are null or a name");
    need(member(o,"container").IsBool(),"container must be a bool");
    npcOwned   += npc.IsString() ? 1 : 0;
    guildOwned += guild.IsString() ? 1 : 0;
    owned      += npc.IsString() || guild.IsString() ? 1 : 0;
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
  if(strangers>0) {
    std::string names;
    for(const auto& n:unknownOwners)
      names += (names.empty() ? "" : ", ")+n;
    std::printf("  %zu objects name an owner NPC that is not in the world at the start: %s\n",strangers,names.c_str());
    }
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

std::set<std::string> checkNpcs(const Value& doc, const std::vector<Waypoint>& points, const fs::path& dayPeoplePath) {
  const auto& list = array(doc,"npcs");
  std::printf("npcs: %u\n",list.Size());
  if(list.Size()!=WorldNpcs)
    fail("the world must have all "+std::to_string(WorldNpcs)+" NPCs, the export has "+std::to_string(list.Size()));
  std::set<std::string> ids, keys;
  std::map<std::string,std::vector<std::string>> people;
  std::string previous;
  uint32_t ordinal = 0;
  size_t entries = 0, persons = 0, unresolved = 0;
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
      ++unresolved;
      const bool inWorld = std::any_of(points.begin(),points.end(),[&](const Waypoint& p) { return p.name==name; });
      auto known = KnownMissingPoints.find(name);
      if(inWorld)
        fail("npc "+key+": routine point "+name+" is in the world but has no id");
      else if(known==KnownMissingPoints.end())
        fail("npc "+key+": routine point "+name+" is not in the world");
      else
        std::printf("  npc %s: routine point %s is not in the world, the engine does not find it either: %s\n",
                    key.c_str(),name.c_str(),known->second.c_str());
      }
    if(n["person"].GetBool()) {
      ++persons;
      people[id].push_back(personRecord(str(n,"name"),uint(n,"guild"),routine));
      }
    }
  std::printf("people: %zu; routine entries of all NPCs: %zu, %zu without a point of the world\n",persons,entries,unresolved);

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
  size_t differences = 0;
  for(const auto& s:symbols) {
    auto a = people.count(s) ? people[s] : std::vector<std::string>();
    auto b = reference.count(s) ? reference[s] : std::vector<std::string>();
    std::sort(a.begin(),a.end());
    std::sort(b.begin(),b.end());
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
    std::printf("people and routines match day_people.json record by record\n");
  return ids;
  }

void checkHeader(const Value& doc) {
  need(str(doc,"format")=="society-world" && integer(doc,"version")==1,"format must be society-world version 1");
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
    checkObjects(doc,points,ids);
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
