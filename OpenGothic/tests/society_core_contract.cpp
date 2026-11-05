// The society stand's core (docs/society-sim.md, «Интерфейсы → 2. Ядро на C++») on the game's data: gets the
// `society-world` v1 export from `gothic-body-runner --society-export` (GOTHIC_DIR, as the export's contract),
// loads it and checks:
// - the load: the counts the runner printed for the file (and the 1053 NPCs of Khorinis);
// - a day by the routine only: every person at every 5-minute mark is on the way, or at the point of the routine
//   entry in force doing its state;
// - repeat: two runs with one seed give the same journal bytes;
// - a snapshot in the middle of the day and a restore give the same tail of the journal;
// - speed: a day of the whole world on one thread, fails above 1 s (the document's target is 0.1 s);
// - gothic-society-day plays 7 days and writes the journal and the marks.
#include "society/journal.h"
#include "society/society.h"

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <unistd.h>

namespace {

namespace fs = std::filesystem;
using Society::Time;
using Clock = std::chrono::steady_clock;

constexpr size_t WorldNpcs   = 1053; // the startup scripts insert every NPC of Khorinis (docs/world-streaming.md)
constexpr double DayLimit    = 1.0;  // seconds; the document's target is 0.1 s
constexpr int    WarmDays    = 20;
constexpr int    PrintLimit  = 20;
constexpr uint64_t Seed      = 1;
constexpr unsigned long ToolDays = 7;
// People without a single routine entry, as in day_people.json; another number fails. In v1 there is no entry in
// force for them, so their own criterion is checked: they stay at start_waypoint all day, not on the way, without a
// state (the engine runs their start_aistate, which the export v1 does not carry yet).
constexpr size_t PeopleWithoutRoutine = 45;

int failures = 0;

void fail(const std::string& what) {
  std::printf("FAIL: %s\n",what.c_str());
  ++failures;
  }

void need(bool condition, const std::string& what) {
  if(!condition)
    throw std::runtime_error(what);
  }

double since(Clock::time_point from) {
  return std::chrono::duration<double>(Clock::now()-from).count();
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

// "+HH:MM" since the start.
std::string clock(const Society::World& w, Time t) {
  const Time d = t-w.startTime();
  char buf[32];
  std::snprintf(buf,sizeof(buf),"+%02lld:%02lld:%02lld",(long long)(d/3600),(long long)(d/60%60),(long long)(d%60));
  return buf;
  }

std::string pointName(const Society::World& w, uint32_t p) {
  return p==Society::NoPoint ? std::string("-") : w.points.name[p];
  }

// --- The export ------------------------------------------------------------------------------------------

struct Counts {
  size_t waypoints = 0, freepoints = 0, edges = 0, objects = 0, containers = 0, owned = 0;
  size_t npcs = 0, people = 0, entries = 0, unresolved = 0;
  };

// Runs the runner once and reads its "society export:" line: the counts it wrote into the file.
Counts exportWorld(const std::string& gothic, const fs::path& out, const fs::path& log) {
  const auto cmd = shellQuote(SOCIETY_RUNNER)+" -g "+shellQuote(gothic)+" -g2 --world NEWWORLD.ZEN --society-export "+
                   shellQuote(out.string())+" >"+shellQuote(log.string())+" 2>&1";
  const auto from = Clock::now();
  const int  rc   = std::system(cmd.c_str());
  const auto sec  = since(from);
  std::string text;
  try { text = read(log); } catch(const std::exception&) {}
  const auto all = lines(text);
  if(rc!=0) {
    for(size_t i=all.size()>40 ? all.size()-40 : 0; i<all.size(); ++i)
      std::printf("  | %s\n",all[i].c_str());
    throw std::runtime_error("gothic-body-runner --society-export exited with "+std::to_string(rc));
    }
  Counts c;
  bool found = false;
  for(const auto& line:all) {
    if(line.rfind("society export:",0)!=0)
      continue;
    std::printf("%s\n",line.c_str());
    const auto at = line.find("waypoints=");
    need(at!=std::string::npos,"the runner's society export line has no counts");
    found = std::sscanf(line.c_str()+at,"waypoints=%zu (freepoints %zu) edges=%zu objects=%zu (containers %zu, owned %zu) "
                        "npcs=%zu (people %zu, routine entries %zu, unresolved points %zu",&c.waypoints,&c.freepoints,
                        &c.edges,&c.objects,&c.containers,&c.owned,&c.npcs,&c.people,&c.entries,&c.unresolved)==10;
    }
  need(found,"the runner printed no society export line with counts");
  std::printf("runner wall time: %.1f s\n",sec);
  return c;
  }

void same(const char* what, size_t core, size_t runner) {
  if(core!=runner)
    fail(std::string(what)+": the core loaded "+std::to_string(core)+", the runner wrote "+std::to_string(runner));
  }

void checkLoad(const Society::World& w, const Counts& c, double seconds) {
  std::printf("loaded %zu bytes (hash %016" PRIx64 ") in %.1f ms; world %s, base %s, engine %s, start day %lld %02d:%02d, "
              "clock %.1f, walk %.1f cm/s\n",w.exportBytes,w.exportHash,seconds*1000.0,w.worldName.c_str(),
              w.dataFingerprint.c_str(),w.engineCommit.c_str(),(long long)w.startDay,w.startMinute/60,w.startMinute%60,
              w.clockRate,w.walkCmPerS);
  std::printf("waypoints: %zu (waynet %zu, freepoints %zu)\n",w.points.size(),w.points.size()-w.freepoints,w.freepoints);
  std::printf("edges: %zu directed (%zu pairs); waynet components: %zu\n",w.edgeTo.size(),w.edgeTo.size()/2,w.components);
  std::printf("objects: %zu, containers %zu, with an owner %zu\n",w.objects.size(),w.containers,w.ownedObjects);
  std::printf("npcs: %zu\n",w.npcs.size());
  std::printf("people: %zu; routine entries of all NPCs: %zu; with waypoint null: %zu; people's entries: %zu; states: %zu\n",
              w.people,w.routines.size(),w.unresolvedEntries,w.peopleEntries,w.stateNames.size());
  same("waypoints",w.points.size(),c.waypoints);
  same("freepoints",w.freepoints,c.freepoints);
  same("edges",w.edgeTo.size(),c.edges);
  same("objects",w.objects.size(),c.objects);
  same("containers",w.containers,c.containers);
  same("objects with an owner",w.ownedObjects,c.owned);
  same("npcs",w.npcs.size(),c.npcs);
  same("people",w.people,c.people);
  same("routine entries",w.routines.size(),c.entries);
  same("entries with waypoint null",w.unresolvedEntries,c.unresolved);
  if(w.npcs.size()!=WorldNpcs)
    fail("the world must have all "+std::to_string(WorldNpcs)+" NPCs, the export has "+std::to_string(w.npcs.size()));
  if(w.startMinute!=480 || w.startDay!=0)
    fail("the export must start at day 0, 08:00");
  }

// --- Travel time -----------------------------------------------------------------------------------------

void checkTravelTime() {
  struct Case { int64_t cm; double walk, clock; Time seconds; };
  // 100 m at 1 m/s is 100 s of the world's tick, 1450 game seconds at 14.5; 11.31 m at 113.1 cm/s is 10 s, 145.
  const Case cases[] = {{10000,100.0,14.5,1450},{1131,113.1,14.5,145},{0,113.1,14.5,0},{-5,113.1,14.5,0},
                        {4,113.1,14.5,1},{3,113.1,14.5,0},{5000,113.1,1.0,44}};
  for(const auto& c:cases) {
    const auto got = Society::travelSeconds(c.cm,c.walk,c.clock);
    if(got!=c.seconds)
      fail("travelSeconds("+std::to_string(c.cm)+" cm, "+std::to_string(c.walk)+" cm/s, clock "+std::to_string(c.clock)+
           ") = "+std::to_string(got)+", expected "+std::to_string(c.seconds));
    }
  std::printf("travel time: length / walk x clock in whole game seconds: 100 m at 100 cm/s = 1450 s; checked %zu cases\n",
              sizeof(cases)/sizeof(cases[0]));
  }

// --- A day with marks ------------------------------------------------------------------------------------

struct MarkStats {
  size_t marks = 0, rows = 0, moving = 0, atPoint = 0, substitute = 0, nullPoint = 0, stayRows = 0, violations = 0;
  std::set<uint32_t> substitutePeople, noRoutinePeople;
  };

void violation(const Society::World& w, MarkStats& st, Time t, uint32_t npc, const std::string& what) {
  if(++st.violations<=size_t(PrintLimit))
    std::printf("  %s %s: %s\n",clock(w,t).c_str(),w.npcs.key[npc].c_str(),what.c_str());
  }

// Every person with a routine: on the way, or at the point of the entry in force doing its state. An entry with
// `waypoint: null` is done where the NPC stands; until its first walk an NPC may stand at start_waypoint instead of the
// entry's point (the engine's placement, Npc::resetPositionToTA, takes the next point when that one is taken).
// Every person without a routine: at start_waypoint, not on the way, without an entry and without a state change.
void checkMark(const Society::World& w, const Society::Sim& sim, MarkStats& st) {
  const auto& s = sim.s;
  const Time  t = s.now;
  ++st.marks;
  for(uint32_t i=0; i<w.npcs.size(); ++i) {
    if(!w.npcs.person[i])
      continue;
    if(w.entryCount(i)==0) {
      ++st.stayRows;
      st.noRoutinePeople.insert(i);
      if(s.entry[i]!=Society::NoEntry || (s.flags[i] & Society::Moving)!=0 || s.place[i]!=w.npcs.startPoint[i] ||
         s.state[i]!=Society::NoState || s.stateStart[i]!=w.startTime())
        violation(w,st,t,i,"has no routine but left start_waypoint "+pointName(w,w.npcs.startPoint[i])+" or changed state");
      continue;
      }
    ++st.rows;
    const auto e      = Society::entryAt(w,i,t);
    const auto direct = Society::routineAt(w,i,t);
    if(e!=direct) {
      violation(w,st,t,i,"the timeline's entry "+std::to_string(e)+" is not Npc::currentRoutine's "+std::to_string(direct));
      continue;
      }
    if(s.entry[i]!=e) {
      violation(w,st,t,i,"does entry "+std::to_string(s.entry[i])+", the entry in force is "+std::to_string(e));
      continue;
      }
    if((s.flags[i] & Society::Moving)!=0) {
      ++st.moving;
      continue;
      }
    const auto k = w.entryIndex(i,e);
    const auto point = w.routines.point[k];
    if(s.state[i]!=w.routines.state[k])
      violation(w,st,t,i,"at "+pointName(w,s.place[i])+" does "+(s.state[i]==Society::NoState ? std::string("nothing") :
                w.stateNames[s.state[i]])+", the entry's state is "+w.stateNames[w.routines.state[k]]);
    else if(point==Society::NoPoint)
      ++st.nullPoint;
    else if(s.place[i]==point)
      ++st.atPoint;
    else if((s.flags[i] & Society::AtStart)!=0 && s.place[i]==w.npcs.startPoint[i]) {
      ++st.substitute;
      st.substitutePeople.insert(i);
      }
    else
      violation(w,st,t,i,"stands at "+pointName(w,s.place[i])+", the entry's point is "+pointName(w,point));
    }
  }

struct Day {
  std::string         journal;
  std::vector<size_t> movingAtMark;   // all NPCs on the way at each mark
  std::vector<Society::Event> events;
  };

Day dayWithMarks(const Society::World& w) {
  Society::SimConfig config;
  config.seed = Seed;
  auto sim = Society::makeSim(w,config);
  Society::DecisionBatch batch;
  Day day;
  day.journal = Society::Journal::header(w,Seed);
  MarkStats st;
  const auto take = [&]() {
    const auto ev = Society::drainEvents(sim);
    Society::Journal::append(day.journal,ev);
    day.events.insert(day.events.end(),ev.begin(),ev.end());
    checkMark(w,sim,st);
    size_t moving = 0;
    for(auto f:sim.s.flags)
      moving += (f & Society::Moving)!=0 ? 1 : 0;
    day.movingAtMark.push_back(moving);
    };
  take();
  const Time start = w.startTime();
  for(Time t=start+Society::Marks::Period; t<=start+Society::DaySeconds; t+=Society::Marks::Period) {
    Society::advance(sim,batch,t);
    take();
    }
  std::printf("day by the routine, %zu marks: %zu people checked: %zu by routine, %zu stay at start; by routine %zu rows "
              "(on the way %zu, at the entry's point %zu, at the start placement instead of it %zu (%zu people), entry with "
              "waypoint null %zu), stay at start %zu rows; violations %zu\n",st.marks,w.people,
              w.people-st.noRoutinePeople.size(),st.noRoutinePeople.size(),st.rows,st.moving,st.atPoint,st.substitute,
              st.substitutePeople.size(),st.nullPoint,st.stayRows,st.violations);
  if(st.noRoutinePeople.size()!=PeopleWithoutRoutine)
    fail(std::to_string(st.noRoutinePeople.size())+" people have no routine, expected "+std::to_string(PeopleWithoutRoutine));
  if(!st.substitutePeople.empty()) {
    std::string list;
    size_t n = 0;
    for(auto i:st.substitutePeople)
      if(n++<8)
        list += (list.empty() ? "" : ", ")+w.npcs.key[i]+" at "+pointName(w,w.npcs.startPoint[i])+" for "+
                pointName(w,w.routines.point[w.entryIndex(i,Society::entryAt(w,i,start))]);
    std::printf("  start placement instead of the entry's point, e.g. %s\n",list.c_str());
    }
  if(st.violations>0)
    fail(std::to_string(st.violations)+" person-marks break their criterion (by routine or stay at start)");
  if(st.moving==0 || st.atPoint==0)
    fail("a day by the routine must have people on the way and at their points");

  // What the day was made of.
  std::map<Society::EventKind,size_t> kinds;
  std::vector<Time> trips;
  Time longest = -1;
  const Society::Event* longestTrip = nullptr;
  std::vector<const Society::Event*> jumps;
  for(const auto& e:day.events) {
    ++kinds[e.kind];
    if(e.kind==Society::EventKind::TravelStarted) {
      trips.push_back(e.c-e.time);
      if(e.c-e.time>longest) {
        longest     = e.c-e.time;
        longestTrip = &e;
        }
      }
    if(e.kind==Society::EventKind::Teleported && e.reason==uint8_t(Society::TeleportReason::OtherComponent))
      jumps.push_back(&e);
    }
  std::string text;
  for(const auto& [k,n]:kinds)
    text += (text.empty() ? "" : ", ")+std::string(Society::eventName(k))+" "+std::to_string(n);
  std::printf("events of the day: %zu (%s)\n",day.events.size(),text.c_str());
  if(!trips.empty()) {
    std::sort(trips.begin(),trips.end());
    std::printf("walks: %zu, game minutes median %.1f, p90 %.1f, max %.1f (%s %s -> %s)\n",trips.size(),
                double(trips[trips.size()/2])/60.0,double(trips[trips.size()*9/10])/60.0,double(longest)/60.0,
                w.npcs.key[longestTrip->npc].c_str(),pointName(w,longestTrip->a).c_str(),pointName(w,longestTrip->b).c_str());
    }
  std::printf("teleports to another waynet component: %zu\n",jumps.size());
  for(size_t i=0; i<jumps.size() && i<size_t(PrintLimit); ++i)
    std::printf("  %s %s: %s -> %s\n",clock(w,jumps[i]->time).c_str(),w.npcs.key[jumps[i]->npc].c_str(),
                pointName(w,jumps[i]->a).c_str(),pointName(w,jumps[i]->b).c_str());
  return day;
  }

// --- Repeat and snapshot ---------------------------------------------------------------------------------

std::string wholeDay(const Society::World& w) {
  Society::SimConfig config;
  config.seed = Seed;
  auto sim = Society::makeSim(w,config);
  Society::DecisionBatch batch;
  std::string ret = Society::Journal::header(w,Seed);
  Society::advance(sim,batch,w.startTime()+Society::DaySeconds);
  Society::Journal::append(ret,Society::drainEvents(sim));
  return ret;
  }

void sameBytes(const char* what, const std::string& a, const std::string& b) {
  if(a==b)
    return;
  size_t at = 0;
  while(at<a.size() && at<b.size() && a[at]==b[at])
    ++at;
  fail(std::string(what)+": "+std::to_string(a.size())+" and "+std::to_string(b.size())+" bytes, first difference at byte "+
       std::to_string(at));
  }

void checkRepeat(const Society::World& w, const Day& day) {
  const auto again = wholeDay(w);
  sameBytes("two runs with one seed (marks every 5 minutes, then one advance over the day) give different journals",
            day.journal,again);
  if(day.journal==again)
    std::printf("repeat: two runs with seed %" PRIu64 " give the same journal, %zu bytes (%zu events)\n",Seed,again.size(),
                day.events.size());
  }

void checkSnapshot(const Society::World& w, const Day& day) {
  // In the middle of the busiest walking of the day, so that walks are cut by the snapshot.
  const auto marks = day.movingAtMark.size();
  size_t best = marks/4;
  for(size_t i=marks/4; i<marks*3/4; ++i)
    if(day.movingAtMark[i]>day.movingAtMark[best])
      best = i;
  const Time at = w.startTime()+Time(best)*Society::Marks::Period+150;

  Society::SimConfig config;
  config.seed = Seed;
  auto sim = Society::makeSim(w,config);
  Society::DecisionBatch batch;
  const Time end = w.startTime()+Society::DaySeconds;
  std::string head = Society::Journal::header(w,Seed);
  Society::advance(sim,batch,at);
  Society::Journal::append(head,Society::drainEvents(sim));
  size_t moving = 0;
  for(auto f:sim.s.flags)
    moving += (f & Society::Moving)!=0 ? 1 : 0;
  const auto snap = Society::save(sim);
  std::string tail1, tail2;
  Society::advance(sim,batch,end);
  Society::Journal::append(tail1,Society::drainEvents(sim));
  Society::restore(sim,snap);
  if(sim.s.now!=at)
    fail("restore did not return the time of the snapshot");
  Society::advance(sim,batch,end);
  Society::Journal::append(tail2,Society::drainEvents(sim));
  sameBytes("the tail after a restore differs from the tail before it",tail1,tail2);
  sameBytes("the journal cut by a snapshot differs from the whole day's",day.journal,head+tail1);
  if(tail1==tail2 && day.journal==head+tail1)
    std::printf("snapshot at %s (%zu NPCs on the way, %zu queued): the tail after a restore is the same %zu bytes "
                "(%zu events), head and tail give the whole day's journal\n",clock(w,at).c_str(),moving,
                snap.state.queue.size(),tail2.size(),tail2.size()/Society::Journal::RecordBytes);
  if(moving==0)
    fail("the snapshot must cut walks: nobody is on the way at "+clock(w,at));
  }

// --- Speed -----------------------------------------------------------------------------------------------

void checkSpeed(const Society::World& w) {
  Society::SimConfig config;
  config.seed = Seed;
  auto sim = Society::makeSim(w,config);
  Society::DecisionBatch batch;
  const Time end = w.startTime()+Society::DaySeconds;
  const auto oneDay = [&]() {
    const auto from = Clock::now();
    Society::reset(sim,Seed);
    Society::advance(sim,batch,end);
    const auto ev = Society::drainEvents(sim);
    const double sec = since(from);
    return std::make_pair(sec,ev.size());
    };
  const auto [cold,events] = oneDay();
  const size_t searches = sim.paths.searches();
  std::vector<double> warm;
  for(int i=0; i<WarmDays; ++i)
    warm.push_back(oneDay().first);
  std::sort(warm.begin(),warm.end());
  const double median = warm[warm.size()/2];
  std::printf("speed: a day of the whole world (%zu NPCs) on one thread: %.2f ms with an empty path cache (%zu Dijkstra "
              "searches), %.2f ms median and %.2f ms min over %d more days; %zu events and %" PRIu64 " queue pops a day; "
              "%.2f M events/s (median); limit %.1f s, target 0.1 s: %s\n",w.npcs.size(),cold*1000.0,searches,median*1000.0,
              warm.front()*1000.0,WarmDays,events,sim.s.processed,median>0 ? double(events)/median/1e6 : 0.0,DayLimit,
              std::max(cold,median)<=0.1 ? "met" : "not met");
  if(cold>DayLimit || median>DayLimit)
    fail("a day of the whole world must take under "+std::to_string(DayLimit)+" s");
  }

// --- The tool --------------------------------------------------------------------------------------------

void checkTool(const Society::World& w, const fs::path& exportPath, const fs::path& dir) {
  const auto log = dir/"day.log";
  const auto cmd = shellQuote(SOCIETY_DAY)+" --world "+shellQuote(exportPath.string())+" --out "+shellQuote(dir.string())+
                   " --days "+std::to_string(ToolDays)+" --seed "+std::to_string(Seed)+" >"+shellQuote(log.string())+" 2>&1";
  fs::create_directories(dir);
  const int rc = std::system(cmd.c_str());
  std::string text;
  try { text = read(log); } catch(const std::exception&) {}
  for(const auto& line:lines(text))
    std::printf("  | %s\n",line.c_str());
  if(rc!=0) {
    fail("gothic-society-day exited with "+std::to_string(rc));
    return;
    }
  const auto journal = read(dir/"society-day.journal");
  const auto header  = Society::Journal::header(w,Seed);
  if(journal.compare(0,header.size(),header)!=0 || (journal.size()-header.size())%Society::Journal::RecordBytes!=0)
    fail("gothic-society-day's journal has a wrong header or a cut record");
  size_t rows = 0;
  {
    std::ifstream in(dir/"society-marks.csv",std::ios::binary);
    std::string line;
    std::getline(in,line);
    if(line+"\n"!=Society::Marks::header())
      fail("gothic-society-day's marks have a wrong header: "+line);
    while(std::getline(in,line))
      ++rows;
  }
  const size_t expected = (ToolDays*size_t(Society::DaySeconds/Society::Marks::Period)+1)*w.npcs.size();
  if(rows!=expected)
    fail("gothic-society-day wrote "+std::to_string(rows)+" mark rows, expected "+std::to_string(expected));
  else
    std::printf("gothic-society-day: %lu days, journal %zu bytes (%zu events), marks %zu rows (%zu NPCs x %zu marks)\n",
                ToolDays,journal.size(),(journal.size()-header.size())/Society::Journal::RecordBytes,rows,w.npcs.size(),
                rows/w.npcs.size());
  }

}

int main() {
  try {
    const auto* gothic = std::getenv("GOTHIC_DIR");
    need(gothic!=nullptr && *gothic!='\0',"GOTHIC_DIR must point to prepared Gothic II settings");
    const fs::path root = fs::current_path()/("society-core-contract-"+std::to_string(getpid()));
    struct Cleanup { fs::path path; ~Cleanup() { std::error_code e; fs::remove_all(path,e); } } cleanup{root};
    fs::create_directories(root);

    checkTravelTime();
    const auto exportPath = root/"world.json";
    const auto counts = exportWorld(gothic,exportPath,root/"export.log");
    const auto from = Clock::now();
    const auto world = Society::loadWorld(exportPath);
    checkLoad(world,counts,since(from));

    const auto day = dayWithMarks(world);
    checkRepeat(world,day);
    checkSnapshot(world,day);
    checkSpeed(world);
    checkTool(world,exportPath,root/"day");
    if(failures>0) {
      std::fprintf(stderr,"FAIL: society core contract: %d checks failed\n",failures);
      return EXIT_FAILURE;
      }
    std::puts("Society core contracts passed");
    return EXIT_SUCCESS;
    }
  catch(const std::exception& e) {
    std::fprintf(stderr,"FAIL: society core contract: %s\n",e.what());
    return EXIT_FAILURE;
    }
  }
