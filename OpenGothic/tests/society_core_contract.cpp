// The society stand's core (docs/society-sim.md, «Интерфейсы → 2. Ядро на C++») on the game's data: gets the
// `society-world` v2 export from `gothic-body-runner --society-export` (GOTHIC_DIR, as the export's contract),
// loads it and checks:
// - the engine's rules the core repeats (stateEndAt, startStateAt, the routine timeline) on a small world of its own;
// - the load: the counts the runner printed for the file (and the 1053 NPCs of Khorinis);
// - a day by the routine only: every person at every 5-minute mark does the state of the entry the engine's rule
//   gives (the entry in force when the previous state ended), on the way to its point or at it; a person without a
//   routine stays at start_waypoint doing its start_aistate;
// - repeat: two runs with one seed give the same journal bytes;
// - a snapshot in the middle of the day and a restore give the same tail of the journal;
// - speed: a day of the whole world on one thread, fails above 1 s (the document's target is 0.1 s);
// - gothic-society-day plays 7 days and writes the journal and the marks;
// - needs (data/society/config.json): the rules on the small world (decay, fill only standing, sleep only awake,
//   traits, guild weights, noise from the seed, moments of crossings); on the game's data an empty config gives the
//   routine-only journal and marks byte for byte, with needs the routine is the same event for event, every
//   threshold in the journal is at the moment a recomputation from the journal gives and none is missing, the
//   marks' levels are the recomputed ones and in 0..1, repeat and snapshot hold, a day takes under 0.5 s.
#include "society/journal.h"
#include "society/society.h"

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cmath>
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
constexpr double NeedsDayLimit = 0.5; // seconds: a day of the whole world with needs (the document's target)
// People without a single routine entry, as in day_people.json; another number fails. Their own criterion is
// checked: they stay at start_waypoint all day, not on the way, doing their start_aistate as the core resolves it
// (22 bandits ZS_BANDIT, 23 orcs ZS_MM_ALLSCHEDULER with their mm_schedule); each must have one.
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
  const auto cmd = shellQuote(SOCIETY_RUNNER)+" -g "+shellQuote(gothic)+" --world NEWWORLD.ZEN --society-export "+
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
  size_t aistates = 0, schedules = 0;
  double slow = 1e9, fast = 0;
  for(uint32_t i=0; i<w.npcs.size(); ++i) {
    aistates  += w.npcs.startState[i]!=Society::NoState ? 1 : 0;
    schedules += w.npcs.scheduleBegin[i+1]>w.npcs.scheduleBegin[i] ? 1 : 0;
    if(w.npcs.person[i]) {
      slow = std::min(slow,w.npcs.walkCmPerS[i]);
      fast = std::max(fast,w.npcs.walkCmPerS[i]);
      }
    }
  std::printf("export v%d: %zu NPCs with start_aistate, %zu with mm_schedule (%zu windows); people walk %.1f..%.1f cm/s\n",
              w.version,aistates,schedules,w.schedules.size(),slow,fast);
  if(w.version!=3 || aistates==0 || schedules==0 || !(slow<fast))
    fail("the export must be v3 with start_aistate, mm_schedule and the people's own walk speeds");
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

// --- The engine's rules on a small world ------------------------------------------------------------------

// Two points and four NPCs: a routine without gaps, one with a gap (the entry that ended last is in force, and its
// state runs until the same time tomorrow), one over midnight, and a monster schedule.
constexpr const char* RulesWorld = R"({"format":"society-world","version":2,"world":"RULES.ZEN","data_fingerprint":"x",
"engine_commit":"x","start":{"day":0,"minute":480},"clock":{"game_seconds_per_real_second":14.5},
"movement":{"walk_cm_per_s":100.0,"run_cm_per_s":300.0,"source":"test"},
"waypoints":[{"id":0,"name":"A","pos":[0,0,0],"freepoint":false,"near_waypoint":null},
{"id":1,"name":"B","pos":[10000,0,0],"freepoint":false,"near_waypoint":null}],
"edges":[[0,1,10000],[1,0,10000]],"objects":[],
"npcs":[
{"id":"DAY","key":"DAY#0","name":"d","guild":1,"person":true,"start_waypoint":1,"start_aistate":null,"walk_cm_per_s":50.0,"mm_schedule":null,
 "routine":[{"start":480,"end":1200,"state":"ZS_WORK","waypoint":1,"waypoint_name":"B"},{"start":1200,"end":480,"state":"ZS_GOTOBED","waypoint":0,"waypoint_name":"A"}]},
{"id":"GAP","key":"GAP#0","name":"g","guild":1,"person":true,"start_waypoint":0,"start_aistate":null,"walk_cm_per_s":0.0,"mm_schedule":null,
 "routine":[{"start":420,"end":960,"state":"ZS_WORK","waypoint":0,"waypoint_name":"A"},{"start":967,"end":420,"state":"ZS_TALK","waypoint":1,"waypoint_name":"B"}]},
{"id":"ORC","key":"ORC#0","name":"o","guild":60,"person":true,"start_waypoint":1,"start_aistate":"ZS_MM_ALLSCHEDULER","walk_cm_per_s":0.0,
 "mm_schedule":[["ZS_MM_RTN_SLEEP",22,6],["ZS_MM_RTN_REST",0,0],["ZS_MM_RTN_ROAM",0,0],["ZS_MM_RTN_EATGROUND",6,22]],"routine":[]},
{"id":"BDT","key":"BDT#0","name":"b","guild":1,"person":true,"start_waypoint":1,"start_aistate":"ZS_BANDIT","walk_cm_per_s":0.0,"mm_schedule":null,"routine":[]}
]})";

void checkRules() {
  const auto w = Society::parseWorld(RulesWorld);
  const auto hm = [](int day, int h, int m) { return Time(day)*Society::DaySeconds+Time(h)*3600+Time(m)*60; };
  const auto name = [&](uint16_t s) { return s==Society::NoState ? std::string("-") : w.stateNames[s]; };
  const auto expect = [&](const std::string& what, const std::string& got, const std::string& want) {
    if(got!=want)
      fail("rules: "+what+" is "+got+", expected "+want);
    };
  const auto clock = [](Time t) {
    char buf[32];
    std::snprintf(buf,sizeof(buf),"day %lld %02lld:%02lld",(long long)(t/Society::DaySeconds),
                  (long long)(t%Society::DaySeconds/3600),(long long)(t%3600/60));
    return std::string(buf);
    };
  // Npc::endTime: a routine without gaps ends its state at the entry's end, over midnight too.
  expect("DAY's work started at 08:00 ends",clock(Society::stateEndAt(w,0,0,hm(0,8,0))),clock(hm(0,20,0)));
  expect("DAY's bed started at 20:00 ends",clock(Society::stateEndAt(w,0,1,hm(0,20,0))),clock(hm(1,8,0)));
  expect("DAY's bed started at 02:00 ends",clock(Society::stateEndAt(w,0,1,hm(1,2,0))),clock(hm(1,8,0)));
  // A gap (16:00-16:07): the entry that ended last is in force and its state runs until 16:00 tomorrow.
  const auto gapEntry = Society::routineAt(w,1,hm(0,16,3));
  expect("GAP's entry in the gap",std::to_string(gapEntry),"0");
  expect("GAP's work restarted at 16:00:20 ends",clock(Society::stateEndAt(w,1,0,hm(0,16,0)+20)),clock(hm(1,16,0)));
  expect("GAP's entry at 16:07",std::to_string(Society::routineAt(w,1,hm(0,16,7))),"1");
  // The NPC's own walk speed, the world's median for 0.
  expect("DAY's walk",std::to_string(int(w.npcs.walkCmPerS[0])),"50");
  expect("GAP's walk (none of its own)",std::to_string(int(w.npcs.walkCmPerS[1])),"100");
  // start_aistate: the monster dispatcher picks by its hours (Wld_IsTime), a plain state stays.
  expect("ORC at 08:00",name(Society::startStateAt(w,2,hm(0,8,0))),"ZS_MM_RTN_EATGROUND");
  expect("ORC at 23:00",name(Society::startStateAt(w,2,hm(0,23,0))),"ZS_MM_RTN_SLEEP");
  expect("ORC at 06:00",name(Society::startStateAt(w,2,hm(1,6,0))),"ZS_MM_RTN_EATGROUND");
  expect("BDT",name(Society::startStateAt(w,3,hm(0,8,0))),"ZS_BANDIT");
  const auto gotobed = std::lower_bound(w.stateNames.begin(),w.stateNames.end(),std::string("ZS_GOTOBED"));
  expect("ZS_GOTOBED's follow-on",name(w.followState[size_t(gotobed-w.stateNames.begin())]),"ZS_SLEEP");

  // A day of that world: DAY works at B from the start; at 20:00 its state ends, after StartDelay it goes to bed at
  // A, 100 m at its own 50 cm/s (2900 game s), and once there it sleeps.
  Society::SimConfig config;
  auto sim = Society::makeSim(w,config);
  Society::DecisionBatch batch;
  const auto at = [&](uint32_t i) {
    return name(sim.s.state[i])+((sim.s.flags[i] & Society::Moving)!=0 ? " moving" : " at "+w.points.name[sim.s.place[i]]);
    };
  Society::advance(sim,batch,hm(0,8,1));
  expect("DAY at 08:01",at(0),"ZS_WORK at B");
  Society::advance(sim,batch,hm(0,20,0)+Society::StartDelay-1);
  expect("DAY just before its state's end and the delay",at(0),"ZS_WORK at B");
  Society::advance(sim,batch,hm(0,20,1));
  expect("DAY at 20:01",at(0),"ZS_GOTOBED moving");
  expect("DAY's walk to bed, game s after 20:00",std::to_string(sim.s.arrive[0]-hm(0,20,0)),
         std::to_string(Society::StartDelay+2900));
  Society::advance(sim,batch,hm(0,21,0));
  expect("DAY at 21:00",at(0),"ZS_SLEEP at A");
  // GAP: its work restarted in the gap at 16:00; on the next day at 16:30 the entry in force is the talk at B, but it
  // keeps working at A, as the engine's VLK_430 keeps cooking.
  Society::advance(sim,batch,hm(1,16,30));
  expect("GAP on day 1 at 16:30",at(1)+" entry "+std::to_string(sim.s.entry[1]),"ZS_WORK at A entry 1");
  expect("ORC on day 1 at 16:30",at(2),"ZS_MM_RTN_EATGROUND at B");
  expect("BDT on day 1 at 16:30",at(3),"ZS_BANDIT at B");
  // The same start_aistate starts anew every StartStateSeconds+StartDelay from 08:00: last at 16:02:40.
  expect("BDT's state began",clock(sim.s.stateStart[3])+std::to_string(sim.s.stateStart[3]%60),
         clock(hm(1,16,2))+"40");
  Society::advance(sim,batch,hm(2,0,30));
  expect("ORC on day 2 at 00:30 (picked anew every four hours from 08:00)",at(2),"ZS_MM_RTN_SLEEP at B");
  std::printf("rules: Npc::endTime with a gap and over midnight, own walk speed, start_aistate and the monster schedule, "
              "ZS_GOTOBED -> ZS_SLEEP, the start delay %lld s: checked on a world of 4 NPCs\n",(long long)Society::StartDelay);
  }

// --- A day with marks ------------------------------------------------------------------------------------

struct MarkStats {
  size_t marks = 0, rows = 0, moving = 0, atPoint = 0, nearPoint = 0, substitute = 0, nullPoint = 0, followOn = 0;
  size_t otherEntry = 0, stayRows = 0, violations = 0;
  std::set<uint32_t> substitutePeople, noRoutinePeople, otherEntryPeople;
  std::map<std::string,size_t> startStates;
  };

void violation(const Society::World& w, MarkStats& st, Time t, uint32_t npc, const std::string& what) {
  if(++st.violations<=size_t(PrintLimit))
    std::printf("  %s %s: %s\n",clock(w,t).c_str(),w.npcs.key[npc].c_str(),what.c_str());
  }

// The engine's rule replayed on its own: a state runs until stateEndAt of its entry, then after StartDelay the entry
// in force (routineAt) starts its state; without a routine start_aistate starts again every StartStateSeconds.
// Returns the entry whose state is done at `t` and when that state began.
std::pair<uint16_t,Time> stateEntryAt(const Society::World& w, uint32_t npc, Time t) {
  Time     begin = w.startTime();
  uint16_t entry = Society::routineAt(w,npc,begin);
  for(;;) {
    const Time next = Society::stateEndAt(w,npc,entry,begin)+Society::StartDelay;
    if(next>t)
      return {entry,begin};
    begin = next;
    entry = Society::routineAt(w,npc,begin);
    }
  }

Time startStateBegin(const Society::World& w, Time t) {
  const Time period = Society::StartStateSeconds+Society::StartDelay;
  return w.startTime()+(t-w.startTime())/period*period;
  }

// Every person with a routine: the entry in force is the timeline's (Npc::currentRoutine); the state is that of the
// entry the engine's rule gives (stateEntryAt) or its follow-on (ZS_GOTOBED -> ZS_SLEEP) standing; it is on the way,
// or at that entry's point (within NearPointCm counts: no walk), or does an entry with `waypoint: null` where it
// stands; until its first walk it may stand at start_waypoint instead (the engine's placement, Npc::resetPositionToTA,
// takes the next point when that one is taken).
// Every person without a routine: at start_waypoint, not on the way, doing startStateAt of its last start.
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
      const auto want = Society::startStateAt(w,i,startStateBegin(w,t));
      if(want==Society::NoState)
        violation(w,st,t,i,"has neither a routine nor a start_aistate");
      else if(t==w.startTime())
        ++st.startStates[w.stateNames[want]];
      if(s.entry[i]!=Society::NoEntry || (s.flags[i] & Society::Moving)!=0 || s.place[i]!=w.npcs.startPoint[i] ||
         s.state[i]!=want)
        violation(w,st,t,i,"has no routine but left start_waypoint "+pointName(w,w.npcs.startPoint[i])+" or does "+
                  (s.state[i]==Society::NoState ? std::string("nothing") : w.stateNames[s.state[i]])+" instead of "+
                  (want==Society::NoState ? std::string("nothing") : w.stateNames[want]));
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
      violation(w,st,t,i,"has entry "+std::to_string(s.entry[i])+" in force, the timeline's is "+std::to_string(e));
      continue;
      }
    const auto [k,began] = stateEntryAt(w,i,t);
    if(s.stateEntry[i]!=k) {
      violation(w,st,t,i,"does the state of entry "+std::to_string(s.stateEntry[i])+", the engine's rule gives "+
                std::to_string(k)+" (begun "+clock(w,began)+")");
      continue;
      }
    // Past the entry's start delay the state is another entry's only in a gap of the routine: its state runs on.
    if(k!=e && Society::stateEndAt(w,i,k,began)>t) {
      ++st.otherEntry;
      st.otherEntryPeople.insert(i);
      }
    const auto idx    = w.entryIndex(i,k);
    const auto state  = w.routines.state[idx];
    const auto follow = w.followState[state];
    const bool moving = (s.flags[i] & Society::Moving)!=0;
    if(s.state[i]!=state && !(s.state[i]==follow && follow!=Society::NoState && !moving)) {
      violation(w,st,t,i,std::string(moving ? "on the way" : "at "+pointName(w,s.place[i]))+" does "+
                (s.state[i]==Society::NoState ? std::string("nothing") : w.stateNames[s.state[i]])+
                ", the entry's state is "+w.stateNames[state]);
      continue;
      }
    st.followOn += s.state[i]==follow ? 1 : 0;
    if(moving) {
      ++st.moving;
      continue;
      }
    const auto point = w.routines.point[idx];
    if(point==Society::NoPoint)
      ++st.nullPoint;
    else if(s.place[i]==point)
      ++st.atPoint;
    else if(std::hypot(double(w.points.pos[point][0])-w.points.pos[s.place[i]][0],
                       double(w.points.pos[point][1])-w.points.pos[s.place[i]][1],
                       double(w.points.pos[point][2])-w.points.pos[s.place[i]][2])<=double(Society::NearPointCm))
      ++st.nearPoint;
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
  std::string         marks;          // society-marks.csv of the day
  std::vector<size_t> movingAtMark;   // all NPCs on the way at each mark
  std::vector<size_t> eventsAtMark;   // events up to and including each mark
  std::vector<Society::Event> events;
  std::vector<double> decay;          // the sim's State::needDecay after reset
  };

Day dayWithMarks(const Society::World& w) {
  Society::SimConfig config;
  config.seed = Seed;
  auto sim = Society::makeSim(w,config);
  Society::DecisionBatch batch;
  Day day;
  day.journal = Society::Journal::header(w,Seed);
  day.marks   = Society::Marks::header(w);
  day.decay   = sim.s.needDecay;
  MarkStats st;
  const auto take = [&]() {
    const auto ev = Society::drainEvents(sim);
    Society::Journal::append(day.journal,ev);
    day.events.insert(day.events.end(),ev.begin(),ev.end());
    day.eventsAtMark.push_back(day.events.size());
    Society::Marks::append(day.marks,sim);
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
              "(on the way %zu, at the entry's point %zu, within %lld cm of it %zu, at the start placement instead of it "
              "%zu (%zu people), entry with waypoint null %zu; doing the follow-on state %zu; doing the state of an entry "
              "that is no longer in force and has not ended %zu (%zu people: a gap in the routine)), stay at start %zu rows; "
              "violations %zu\n",st.marks,w.people,w.people-st.noRoutinePeople.size(),st.noRoutinePeople.size(),st.rows,
              st.moving,st.atPoint,(long long)Society::NearPointCm,st.nearPoint,st.substitute,st.substitutePeople.size(),
              st.nullPoint,st.followOn,st.otherEntry,st.otherEntryPeople.size(),st.stayRows,st.violations);
  std::string kept;
  for(auto i:st.otherEntryPeople)
    kept += (kept.empty() ? "" : ", ")+w.npcs.key[i];
  std::printf("  kept by a gap in the routine: %s\n",kept.c_str());
  std::string starts;
  for(const auto& [name,n]:st.startStates)
    starts += (starts.empty() ? "" : ", ")+name+" "+std::to_string(n);
  std::printf("  people without a routine at the start: %s\n",starts.c_str());
  if(st.noRoutinePeople.size()!=PeopleWithoutRoutine)
    fail(std::to_string(st.noRoutinePeople.size())+" people have no routine, expected "+std::to_string(PeopleWithoutRoutine));
  if(st.followOn==0 || st.otherEntry==0)
    fail("a day by the routine must have people asleep (ZS_GOTOBED -> ZS_SLEEP) and people kept by a gap in their routine");
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

void checkSpeed(const Society::World& w, double limit) {
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
  const bool needs = Society::needCount(w)>0;
  const double target = needs ? NeedsDayLimit : 0.1;
  std::printf("speed%s: a day of the whole world (%zu NPCs) on one thread: %.2f ms with an empty path cache (%zu Dijkstra "
              "searches), %.2f ms median and %.2f ms min over %d more days; %zu events and %" PRIu64 " queue pops a day; "
              "%.2f M events/s (median); limit %.1f s, target %.1f s: %s\n",needs ? " with needs" : "",w.npcs.size(),
              cold*1000.0,searches,median*1000.0,warm.front()*1000.0,WarmDays,events,sim.s.processed,
              median>0 ? double(events)/median/1e6 : 0.0,limit,target,std::max(cold,median)<=target ? "met" : "not met");
  if(cold>limit || median>limit)
    fail(std::string("a day of the whole world")+(needs ? " with needs" : "")+" must take under "+std::to_string(limit)+" s");
  }

// --- Needs on the small world ------------------------------------------------------------------------------

// The rules world with five needs: food and work fall from 1 at 08:00; DAY (guild 1, a card with talk 5) works at
// B 08:00-20:00, walks 2900 s to bed at A and sleeps; its faith has guild weight 0. CARDS and NOISE are filled in.
constexpr const char* RulesConfig = R"({"format":"society-config","version":1,
"needs":[
 {"id":"food","name":"f","empty_hours":16,"low":0.4,"critical":0.15,"start":1.0,"awake_only":false},
 {"id":"sleep","name":"s","empty_hours":18,"low":0.35,"critical":0.1,"start":1.0,"awake_only":true},
 {"id":"social","name":"c","empty_hours":24,"low":0.3,"critical":0.1,"start":1.0,"awake_only":false},
 {"id":"work","name":"w","empty_hours":12,"low":0.3,"critical":0.1,"start":1.0,"awake_only":false},
 {"id":"faith","name":"v","empty_hours":48,"low":0.3,"critical":0.1,"start":1.0,"awake_only":false,
  "guild_weight":{"default":1.0,"guilds":[{"guild":1,"name":"p","weight":0.0}]}}],
"states":[
 {"state":"ZS_WORK","kind":"anchor","asleep":false,"fill_hours":{"work":2}},
 {"state":"ZS_GOTOBED","kind":"anchor","asleep":false,"fill_hours":{"social":1}},
 {"state":"ZS_SLEEP","kind":"anchor","asleep":true,"fill_hours":{"sleep":8}},
 {"state":"ZS_TALK","kind":"free","asleep":false,"fill_hours":{"social":1}}],
"object_kinds":[{"kind":"bench","schemes":["BENCH"],"freepoints":["FP_SIT"],"state":"ZS_TALK","fill_hours":{"social":2},
 "capacity":2,"minutes":30}],
"traits":{"cards":CARDS,"scales":["talk"],"middle":3,"noise":NOISE,"decay":[{"scale":"talk","need":"social","per_point":0.15}]}})";

constexpr const char* EmptyConfig = R"({"format":"society-config","version":1,"needs":[],"states":[],"object_kinds":[],
"traits":{"cards":null,"scales":[],"middle":3,"noise":0,"decay":[]}})";

std::string replaced(std::string s, const std::string& what, const std::string& with) {
  const auto at = s.find(what);
  need(at!=std::string::npos,"no "+what+" to replace");
  return s.replace(at,what.size(),with);
  }

// A whole run's journal: events up to `end`.
std::string runJournal(const Society::World& w, uint64_t seed, Time end, std::vector<Society::Event>* events = nullptr) {
  Society::SimConfig config;
  config.seed = seed;
  auto sim = Society::makeSim(w,config);
  Society::DecisionBatch batch;
  std::string ret = Society::Journal::header(w,seed);
  Society::advance(sim,batch,end);
  const auto ev = Society::drainEvents(sim);
  Society::Journal::append(ret,ev);
  if(events!=nullptr)
    events->assign(ev.begin(),ev.end());
  return ret;
  }

void checkNeedRules(const fs::path& root) {
  const auto cards = root/"cards.json";
  {
    std::ofstream out(cards,std::ios::binary);
    out << R"({"version":1,"cards":{"DAY":{"talk":5},"NOBODY":{"talk":1}}})";
  }
  const auto config = [&](const char* noise) {
    return replaced(replaced(RulesConfig,"CARDS","\""+cards.string()+"\""),"NOISE",noise);
    };
  const auto w = Society::parseWorld(RulesWorld,config("0"));
  const auto hm = [](int day, int h, int m, int sec = 0) { return Time(day)*Society::DaySeconds+Time(h)*3600+Time(m)*60+sec; };
  const auto clock = [](Time t) {
    char buf[64];
    std::snprintf(buf,sizeof(buf),"day %lld %02lld:%02lld:%02lld",(long long)(t/Society::DaySeconds),
                  (long long)(t%Society::DaySeconds/3600),(long long)(t%3600/60),(long long)(t%60));
    return std::string(buf);
    };
  const auto perHour = [](double perSecond) { return perSecond*3600.0; };
  const auto close = [](double a, double b) { return std::abs(a-b)<=1e-12*std::max(1.0,std::abs(b)); };
  const auto& c = w.config;
  if(Society::needCount(w)!=5 || c.states.size()!=4 || c.objectKinds.size()!=1 || c.cards!=2 || !w.npcCard[0] || w.npcCard[1])
    fail("needs rules: the config loads 5 needs, 4 states, 1 object kind, 2 cards (DAY's, not GAP's)");
  // Traits and guilds: DAY talks 5, social falls 1 + 0.15 x 2 = 1.3 times faster; GAP has no card (3); faith of guild 1
  // weighs 0, of the orc's guild 60 the default 1.
  const size_t n = Society::needCount(w);
  if(!close(perHour(w.npcDecay[0*n+2]),1.3/24) || !close(perHour(w.npcDecay[1*n+2]),1.0/24) ||
     w.npcDecay[0*n+4]!=0 || !close(perHour(w.npcDecay[2*n+4]),1.0/48))
    fail("needs rules: social decay by talk (DAY 1.3/24, GAP 1/24 an hour) or faith by guild (0 for guild 1, 1/48 for 60)");

  Society::SimConfig sc;
  auto sim = Society::makeSim(w,sc);
  Society::DecisionBatch batch;
  std::vector<Society::Event> events;
  std::vector<std::pair<Time,std::string>> rates;
  const auto advanceTo = [&](Time t) {
    Society::advance(sim,batch,t);
    const auto ev = Society::drainEvents(sim);
    events.insert(events.end(),ev.begin(),ev.end());
    };
  // Moving: no fill (ZS_GOTOBED fills social here, standing only); standing at the bed it fills; asleep the awake-only
  // sleep does not fall and its fill is all of the rate, food falls on.
  advanceTo(hm(0,20,1));
  const auto& st = sim.s;
  if(!close(st.needRate[0*n+2],-w.npcDecay[0*n+2]))
    fail("needs rules: on the way to bed DAY's social must only fall");
  advanceTo(hm(0,20,48,45));
  if(!close(st.needRate[0*n+2],1.0/3600-w.npcDecay[0*n+2]))
    fail("needs rules: standing at the bed in ZS_GOTOBED DAY's social must rise by its fill minus its decay");
  advanceTo(hm(0,21,0));
  if(!close(st.needRate[0*n+1],1.0/8/3600) || !close(st.needRate[0*n+0],-w.npcDecay[0*n+0]))
    fail("needs rules: asleep DAY's sleep must rise by its fill only and its food fall on");
  advanceTo(hm(2,8,0));
  bool inRange = true;
  for(uint32_t i=0; i<w.npcs.size(); ++i)
    for(size_t k=0; k<n; ++k) {
      const double l = Society::needLevel(sim,i,k);
      inRange = inRange && l>=0 && l<=1;
      }
  if(!inRange)
    fail("needs rules: a level is out of 0..1");
  if(Society::needLevel(sim,0,4)!=1.0)
    fail("needs rules: DAY's faith (guild weight 0) must stay 1");

  // The moments, known in advance (±1 s for rounding): food low 08:00 + 9.6 h; sleep low 08:00 + 11.7 h awake; work
  // falls from 20:00:20 (the bed's state begins) to low in 8.4 h and critical in 10.8 h, is 0 on the walk back to B
  // until 08:48:40 and rises by 1/2 - 1/12 an hour there: above critical in 0.24 h, above low in 0.72 h.
  struct Want { const char* what; size_t need; uint32_t threshold; Society::NeedDirection dir; Time at; };
  const Want wants[] = {
    {"food low",          0,1,Society::NeedDirection::Down,hm(0,17,36)},
    {"food critical",     0,2,Society::NeedDirection::Down,hm(0,21,36)},
    {"sleep low",         1,1,Society::NeedDirection::Down,hm(0,19,42)},
    {"work low",          3,1,Society::NeedDirection::Down,hm(1,4,24,20)},
    {"work critical",     3,2,Society::NeedDirection::Down,hm(1,6,48,20)},
    {"work above critical",3,2,Society::NeedDirection::Up, hm(1,9,3,4)},
    {"work above low",    3,1,Society::NeedDirection::Up,  hm(1,9,31,52)},
    };
  for(const auto& want:wants) {
    const Society::Event* got = nullptr;
    for(const auto& e:events)
      if(e.npc==0 && e.kind==Society::EventKind::NeedThreshold && e.a==want.need && e.b==want.threshold &&
         e.reason==uint8_t(want.dir) && std::abs(e.time-want.at)<=3600) {
        got = &e;
        break;
        }
    if(got==nullptr || std::abs(got->time-want.at)>1)
      fail(std::string("needs rules: DAY's ")+want.what+" at "+(got ? clock(got->time) : std::string("none"))+", expected "+
           clock(want.at)+" (±1 s)");
    }
  // DAY's sleep rises above low again within its first half hour asleep (from 0.288 at 20:48:55 by 1/8 an hour).
  bool slept = false;
  for(const auto& e:events)
    slept = slept || (e.npc==0 && e.kind==Society::EventKind::NeedThreshold && e.a==1 && e.b==1 &&
                      e.reason==uint8_t(Society::NeedDirection::Up) && e.time>hm(0,20,48,55) && e.time<hm(0,21,20));
  if(!slept)
    fail("needs rules: asleep, DAY's sleep must rise above low before 21:20");

  // Without needs: an empty config gives the routine-only journal byte for byte.
  const auto plain = Society::parseWorld(RulesWorld);
  const auto empty = Society::parseWorld(RulesWorld,EmptyConfig);
  sameBytes("needs rules: the journal with \"needs\": [] differs from the one without a config",
            runJournal(plain,Seed,hm(2,8,0)),runJournal(empty,Seed,hm(2,8,0)));

  // Noise: each person's decay within ±15% of its traits' and guild's, drawn from the seed.
  const auto noisy = Society::parseWorld(RulesWorld,config("0.15"));
  auto a = Society::makeSim(noisy,Society::SimConfig{1});
  auto b = Society::makeSim(noisy,Society::SimConfig{2});
  bool within = true;
  for(size_t i=0; i<noisy.npcDecay.size(); ++i) {
    const double base = noisy.npcDecay[i];
    within = within && a.s.needDecay[i]>=base*0.85 && a.s.needDecay[i]<=base*1.15;
    }
  if(!within || a.s.needDecay==b.s.needDecay)
    fail("needs rules: the noise must stay within ±15% and differ between seeds 1 and 2");

  // Bad configs are refused with the reason.
  const std::pair<std::string,std::string> broken[] = {
    {replaced(config("0"),"\"low\":0.4","\"low\":0.1"),"thresholds"},
    {replaced(config("0"),"\"fill_hours\":{\"work\":2}","\"fill_hours\":{\"wrk\":2}"),"no need wrk"},
    {replaced(config("0"),"\"capacity\":2","\"capacity\":2,\"capasity\":2"),"unknown field capasity"},
    {replaced(config("0"),"\"state\":\"ZS_TALK\",\"fill_hours\":{\"social\":2}","\"state\":\"ZS_EAT\",\"fill_hours\":{\"social\":2}"),
     "not in the states table"},
    };
  for(const auto& [text,reason]:broken) {
    std::string got = "accepted";
    try { Society::parseWorld(RulesWorld,text); } catch(const std::exception& e) { got = e.what(); }
    if(got.find(reason)==std::string::npos || got.rfind("society-config: ",0)!=0)
      fail("needs rules: a bad config must fail with \""+reason+"\", got: "+got);
    }
  std::printf("needs rules: decay by talk and guild weight, fill only standing, awake-only sleep, %zu crossings at their "
              "moments, \"needs\": [] the routine's journal byte for byte, noise within ±15%% by seed, %zu bad configs "
              "refused: checked on a world of 4 NPCs\n",sizeof(wants)/sizeof(wants[0]),sizeof(broken)/sizeof(broken[0]));
  }

// --- Needs on the game's data ------------------------------------------------------------------------------

// What the config gives Khorinis; every state a person does must be in its table.
void checkConfig(const Society::World& w) {
  const auto& c = w.config;
  size_t carded = 0;
  for(uint32_t i=0; i<w.npcs.size(); ++i)
    carded += w.npcs.person[i] && w.npcCard[i] ? 1 : 0;
  std::set<std::string> missing;
  const auto use = [&](uint16_t state) {
    if(state!=Society::NoState && w.stateRow[state]==Society::NoState)
      missing.insert(w.stateNames[state]);
    };
  for(uint32_t i=0; i<w.npcs.size(); ++i) {
    if(!w.npcs.person[i])
      continue;
    for(uint32_t k=w.npcs.routineBegin[i]; k<w.npcs.routineBegin[i+1]; ++k) {
      use(w.routines.state[k]);
      use(w.followState[w.routines.state[k]]);
      }
    use(w.npcs.startState[i]);
    for(uint32_t k=w.npcs.scheduleBegin[i]; k<w.npcs.scheduleBegin[i+1]; ++k)
      use(w.schedules.state[k]);
    }
  std::map<std::string,size_t> objects, points;
  for(auto k:w.objectKind)
    if(k!=Society::NoKind)
      ++objects[c.objectKinds.id[k]];
  for(auto k:w.pointKind)
    if(k!=Society::NoKind)
      ++points[c.objectKinds.id[k]];
  std::string kinds;
  for(size_t k=0; k<c.objectKinds.size(); ++k) {
    const auto& id = c.objectKinds.id[k];
    kinds += (kinds.empty() ? "" : ", ")+id+" "+std::to_string(objects[id])+"+"+std::to_string(points[id]);
    }
  std::printf("config %s: %zu needs, %zu states, %zu object kinds (objects+freepoints: %s); cards %zu, people with one %zu "
              "of %zu\n",SOCIETY_CONFIG,c.needs.size(),c.states.size(),c.objectKinds.size(),kinds.c_str(),c.cards,carded,
              w.people);
  if(c.needs.size()!=7)
    fail("the config must have the 7 needs of v1, it has "+std::to_string(c.needs.size()));
  if(!missing.empty()) {
    std::string list;
    for(const auto& m:missing)
      list += (list.empty() ? "" : ", ")+m;
    fail("states people do that the config's table lacks: "+list);
    }
  }

// The marks without the need columns and the journal without need_threshold: with needs the routine is the same.
void checkSameRoutine(const Society::World& w, const Day& plain, const Day& needs) {
  std::vector<Society::Event> routine;
  for(const auto& e:needs.events)
    if(e.kind!=Society::EventKind::NeedThreshold)
      routine.push_back(e);
  bool same = routine.size()==plain.events.size();
  for(size_t i=0; same && i<routine.size(); ++i) {
    const auto& a = routine[i];
    const auto& b = plain.events[i];
    same = a.time==b.time && a.npc==b.npc && a.kind==b.kind && a.reason==b.reason && a.entry==b.entry && a.a==b.a &&
           a.b==b.b && a.c==b.c;
    }
  if(!same)
    fail("with needs the routine's events differ from the run without them");
  // Drop the last `needs` columns of every row.
  const size_t n = Society::needCount(w);
  std::string cut;
  cut.reserve(plain.marks.size());
  for(size_t at=0; at<needs.marks.size();) {
    const size_t end = needs.marks.find('\n',at);
    size_t stop = end;
    for(size_t k=0; k<n; ++k)
      stop = needs.marks.rfind(',',stop-1);
    cut.append(needs.marks,at,stop-at);
    cut += '\n';
    at = end+1;
    }
  sameBytes("with needs the marks without their need columns differ from the run without them",plain.marks,cut);
  if(same && cut==plain.marks)
    std::printf("needs change nothing in the routine: %zu events of %zu are need_threshold, the rest and the marks without "
                "the %zu need columns are the routine-only run's\n",needs.events.size()-routine.size(),needs.events.size(),n);
  }

// The journal is the trajectory: every person's levels are recomputed from its own events, as the document defines
// them. A segment runs from one rate event (activity_started, travel_started, arrived, teleported) to the next, at
// needRate of what the person then does; each need_threshold must be at the first whole second its segment leaves the
// zone, and at every rate event and mark the recomputed zone must be the journal's (none missing). The marks' levels
// must be the recomputed ones (four decimals) and in 0..1.
void checkNeeds(const Society::World& w, const Day& day) {
  const size_t n = Society::needCount(w), npcs = w.npcs.size();
  struct Seg { double level = 1, rate = 0; Time since = 0; };
  std::vector<Seg>      seg(npcs*n);
  std::vector<uint8_t>  zone(npcs*n,0);
  std::vector<uint16_t> state(npcs,Society::NoState);
  std::vector<uint8_t>  moving(npcs,0);
  for(uint32_t i=0; i<npcs; ++i)
    for(size_t k=0; k<n; ++k)
      seg[i*n+k] = {w.config.needs.start[k],0,w.startTime()};
  size_t wrong = 0, missing = 0, outside = 0, levels = 0, rateEvents = 0, thresholds = 0;
  const auto report = [&](size_t& counter, const std::string& what) {
    if(++counter<=size_t(PrintLimit))
      std::printf("  %s\n",what.c_str());
    };
  const auto zoneAt = [&](size_t i, size_t k, Time t) {
    const auto& g = seg[i];
    return Society::needZone(w,k,Society::needLevelAt(g.level,g.rate,g.since,t));
    };
  // Per need: who went low and critical, and the events by threshold and direction.
  std::vector<std::set<uint32_t>> wentLow(n), wentCritical(n);
  std::map<std::string,size_t> byKind;
  // Mark rows: the need columns of person rows, in the marks' order.
  std::istringstream marks(day.marks);
  std::string row;
  std::getline(marks,row);
  size_t from = 0;
  for(size_t m=0; m<day.eventsAtMark.size(); ++m) {
    for(size_t e=from; e<day.eventsAtMark[m]; ++e) {
      const auto& ev = day.events[e];
      const uint32_t i = ev.npc;
      if(!w.npcs.person[i])
        continue;
      if(ev.kind==Society::EventKind::NeedThreshold) {
        ++thresholds;
        const size_t k = ev.a, x = i*n+k;
        const bool down = ev.reason==uint8_t(Society::NeedDirection::Down);
        const uint8_t before = zone[x], after = uint8_t(down ? before+1 : before-1);
        ++byKind[w.config.needs.id[k]+(ev.b==1 ? " low" : " critical")+(down ? " down" : " up")];
        if(down)
          (ev.b==1 ? wentLow : wentCritical)[k].insert(i);
        // Passed at this second, not before it (two thresholds can go in one second: the zone is then beyond `after`).
        const auto now  = zoneAt(x,k,ev.time);
        const bool order = down ? (before<2 && ev.b==after) : (before>0 && ev.b==before);
        const bool at    = down ? now>=after : now<=after;
        const bool first = ev.time==seg[x].since || (down ? zoneAt(x,k,ev.time-1)<=before : zoneAt(x,k,ev.time-1)>=before);
        if(!order || !at || !first)
          report(wrong,clock(w,ev.time)+" "+w.npcs.key[i]+" "+w.config.needs.id[k]+": the crossing is not where its "+
                 "segment puts it (zone "+std::to_string(before)+" -> "+std::to_string(after)+")");
        zone[x] = after;
        continue;
        }
      if(ev.kind!=Society::EventKind::ActivityStarted && ev.kind!=Society::EventKind::TravelStarted &&
         ev.kind!=Society::EventKind::Arrived && ev.kind!=Society::EventKind::Teleported)
        continue;
      ++rateEvents;
      if(ev.kind==Society::EventKind::ActivityStarted)
        state[i] = uint16_t(ev.a);
      if(ev.kind==Society::EventKind::TravelStarted)
        moving[i] = 1;
      if(ev.kind==Society::EventKind::Arrived)
        moving[i] = 0;
      for(size_t k=0; k<n; ++k) {
        const size_t x = i*n+k;
        auto& g = seg[x];
        if(zoneAt(x,k,ev.time)!=zone[x])
          report(missing,clock(w,ev.time)+" "+w.npcs.key[i]+" "+w.config.needs.id[k]+": a crossing before this event is "+
                 "not in the journal");
        const double l = Society::needLevelAt(g.level,g.rate,g.since,ev.time);
        if(!(l>=0 && l<=1))
          report(outside,w.npcs.key[i]+" level "+std::to_string(l));
        g = {l,Society::needRate(w,k,day.decay[x],state[i],moving[i]!=0),ev.time};
        }
      }
    from = day.eventsAtMark[m];
    // The mark: every NPC's row, people's need columns against the recomputation.
    const Time t = w.startTime()+Time(m)*Society::Marks::Period;
    for(uint32_t i=0; i<npcs; ++i) {
      std::getline(marks,row);
      if(!w.npcs.person[i])
        continue;
      size_t at = row.size();
      std::vector<double> got(n);
      for(size_t k=n; k-- > 0;) {
        const size_t comma = row.rfind(',',at-1);
        got[k] = std::strtod(row.c_str()+comma+1,nullptr);
        at = comma;
        }
      for(size_t k=0; k<n; ++k) {
        const size_t x = i*n+k;
        const auto& g = seg[x];
        const double l = Society::needLevelAt(g.level,g.rate,g.since,t);
        ++levels;
        if(!(got[k]>=0 && got[k]<=1) || !(l>=0 && l<=1))
          report(outside,clock(w,t)+" "+w.npcs.key[i]+" "+w.config.needs.id[k]+" level "+std::to_string(got[k]));
        if(std::abs(got[k]-l)>0.00005+1e-9)
          report(wrong,clock(w,t)+" "+w.npcs.key[i]+" "+w.config.needs.id[k]+": the mark says "+std::to_string(got[k])+
                 ", the journal gives "+std::to_string(l));
        if(zoneAt(x,k,t)!=zone[x])
          report(missing,clock(w,t)+" "+w.npcs.key[i]+" "+w.config.needs.id[k]+": a crossing before this mark is not in "+
                 "the journal");
        }
      }
    }
  std::string kinds;
  for(const auto& [k,c]:byKind)
    kinds += (kinds.empty() ? "" : ", ")+k+" "+std::to_string(c);
  std::string people;
  for(size_t k=0; k<n; ++k)
    people += (people.empty() ? "" : "; ")+w.config.needs.id[k]+" "+std::to_string(wentLow[k].size())+"/"+
              std::to_string(wentCritical[k].size());
  std::printf("needs recomputed from the journal: %zu need_threshold events (%s), %zu rate events, %zu mark levels; "
              "people low/critical in the day of %zu: %s; misplaced %zu, missing %zu, out of 0..1 %zu\n",thresholds,
              kinds.c_str(),rateEvents,levels,w.people,people.c_str(),wrong,missing,outside);
  if(wrong>0 || missing>0 || outside>0)
    fail("needs: "+std::to_string(wrong)+" misplaced crossings or levels, "+std::to_string(missing)+" missing crossings, "+
         std::to_string(outside)+" levels out of 0..1");
  if(thresholds==0)
    fail("needs: a day by the routine must cross thresholds");
  }

// The config file with only its needs emptied ("needs": []) on the game's data: the journal and the marks of the
// routine-only run byte for byte, though its states, object kinds and traits still name the needs.
void checkEmptyNeeds(const fs::path& exportPath, const fs::path& root, const Day& plain) {
  const auto config = root/"empty-config.json";
  {
    std::string text = read(SOCIETY_CONFIG);
    const auto from = text.find("\"needs\":"), to = text.find("\"states\":");
    need(from!=std::string::npos && to!=std::string::npos && from<to,"the config must list needs before states");
    text.replace(from,to-from,"\"needs\": [],\n  ");
    std::ofstream out(config,std::ios::binary);
    out << text;
  }
  const auto w = Society::loadWorld(exportPath,config);
  Society::SimConfig sc;
  sc.seed = Seed;
  auto sim = Society::makeSim(w,sc);
  Society::DecisionBatch batch;
  std::string journal = Society::Journal::header(w,Seed), marks = Society::Marks::header(w);
  const Time start = w.startTime();
  Society::Journal::append(journal,Society::drainEvents(sim));
  Society::Marks::append(marks,sim);
  for(Time t=start+Society::Marks::Period; t<=start+Society::DaySeconds; t+=Society::Marks::Period) {
    Society::advance(sim,batch,t);
    Society::Journal::append(journal,Society::drainEvents(sim));
    Society::Marks::append(marks,sim);
    }
  sameBytes("an empty config: the journal differs from the run without a config",plain.journal,journal);
  sameBytes("an empty config: the marks differ from the run without a config",plain.marks,marks);
  if(journal==plain.journal && marks==plain.marks)
    std::printf("\"needs\": [] on the game's data: the journal (%zu bytes) and the marks (%zu bytes) are the routine-only "
                "run's byte for byte\n",journal.size(),marks.size());
  }

// --- The tool --------------------------------------------------------------------------------------------

void checkTool(const Society::World& w, const fs::path& exportPath, const fs::path& dir, const fs::path& config = {}) {
  const auto log = dir/"day.log";
  const auto cmd = shellQuote(SOCIETY_DAY)+" --world "+shellQuote(exportPath.string())+" --out "+shellQuote(dir.string())+
                   " --days "+std::to_string(ToolDays)+" --seed "+std::to_string(Seed)+
                   (config.empty() ? std::string() : " --config "+shellQuote(config.string()))+" >"+shellQuote(log.string())+" 2>&1";
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
    if(line+"\n"!=Society::Marks::header(w))
      fail("gothic-society-day's marks have a wrong header: "+line);
    while(std::getline(in,line))
      ++rows;
  }
  const size_t expected = (ToolDays*size_t(Society::DaySeconds/Society::Marks::Period)+1)*w.npcs.size();
  if(rows!=expected)
    fail("gothic-society-day wrote "+std::to_string(rows)+" mark rows, expected "+std::to_string(expected));
  else
    std::printf("gothic-society-day%s: %lu days, journal %zu bytes (%zu events), marks %zu rows (%zu NPCs x %zu marks)\n",
                config.empty() ? "" : " --config",ToolDays,journal.size(),
                (journal.size()-header.size())/Society::Journal::RecordBytes,rows,w.npcs.size(),rows/w.npcs.size());
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
    checkRules();
    checkNeedRules(root);
    const auto exportPath = root/"world.json";
    const auto counts = exportWorld(gothic,exportPath,root/"export.log");
    const auto from = Clock::now();
    const auto world = Society::loadWorld(exportPath);
    checkLoad(world,counts,since(from));

    const auto day = dayWithMarks(world);
    checkRepeat(world,day);
    checkSnapshot(world,day);
    checkSpeed(world,DayLimit);
    checkTool(world,exportPath,root/"day");

    // Needs: the same day with the config file.
    checkEmptyNeeds(exportPath,root,day);
    const auto needsWorld = Society::loadWorld(exportPath,SOCIETY_CONFIG);
    checkConfig(needsWorld);
    std::printf("with needs:\n");
    const auto needsDay = dayWithMarks(needsWorld);
    checkSameRoutine(needsWorld,day,needsDay);
    checkNeeds(needsWorld,needsDay);
    checkRepeat(needsWorld,needsDay);
    checkSnapshot(needsWorld,needsDay);
    checkSpeed(needsWorld,NeedsDayLimit);
    checkTool(needsWorld,exportPath,root/"needs",SOCIETY_CONFIG);
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
