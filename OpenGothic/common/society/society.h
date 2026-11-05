#pragma once

// The society stand's core (docs/society-sim.md, «Интерфейсы → 2. Ядро на C++»): a world of events over the
// engine's `society-world` v1 export. v1 plays every NPC by its routine only: no needs, objects or decisions yet.
// Folder rule (README.md here): the standard library and rapidjson only, nothing from the engine.

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Society {

// Integer game seconds since day 0, 00:00. Routine bounds in the export are minutes of the day.
using Time = int64_t;
constexpr Time DaySeconds = 24*60*60;

constexpr uint32_t NoPoint = 0xFFFFFFFFu;
constexpr uint16_t NoEntry = 0xFFFFu;
constexpr uint16_t NoState = 0xFFFFu;

// Walking time in game seconds for a path of `lengthCm`: the export gives the walk speed in cm per second of the
// world's tick and the clock runs `gameSecondsPerRealSecond` (14.5) game seconds per such second, so
// time = length / walk × clock, rounded to the nearest second.
Time travelSeconds(int64_t lengthCm, double walkCmPerS, double gameSecondsPerRealSecond);

// The world that does not change during a run: everything of the export v1, read once. Capacities are fixed here.
// Every list keeps the export's order and ids: point = `waypoints[].id`, NPC = index in `npcs[]`, object =
// `objects[].id`, routine entry = index in the NPC's `routine[]`.
struct World {
  // Header.
  std::string worldName, dataFingerprint, engineCommit, movementSource;
  int64_t     startDay = 0;
  int32_t     startMinute = 0;
  double      clockRate = 0;                 // clock.game_seconds_per_real_second
  double      walkCmPerS = 0, runCmPerS = 0; // movement, cm per second of the world's tick
  uint64_t    exportHash = 0;                // FNV-1a 64 of the export's bytes
  size_t      exportBytes = 0;

  struct Points {
    std::vector<std::string>          name;
    std::vector<std::array<float,3>>  pos;
    std::vector<uint8_t>              free;      // a freepoint (FP_*), not on the waynet
    std::vector<uint32_t>             graph;     // the waynet point: itself, or near_waypoint of a freepoint
    std::vector<int32_t>              graphCm;   // the straight segment to `graph`, 0 for a waynet point
    std::vector<uint32_t>             component; // the waynet component of `graph`
    size_t size() const { return name.size(); }
    } points;
  size_t freepoints = 0, components = 0;

  // The waynet as CSR: edges of point p are [edgeBegin[p], edgeBegin[p+1]) in the export's order.
  std::vector<uint32_t> edgeBegin, edgeTo;
  std::vector<int32_t>  edgeCm;

  struct Objects {
    std::vector<std::string>          vob, scheme, focus, ownerNpc, ownerGuild; // owners: "" for null
    std::vector<std::array<float,3>>  pos;
    std::vector<uint32_t>             waypoint;
    std::vector<uint8_t>              container;
    size_t size() const { return vob.size(); }
    } objects;
  size_t containers = 0, ownedObjects = 0;

  struct Npcs {
    std::vector<std::string> id, key, name;
    std::vector<uint32_t>    guild, startPoint;
    std::vector<uint8_t>     person;
    std::vector<uint32_t>    routineBegin;   // n+1: entries of NPC i are [routineBegin[i], routineBegin[i+1])
    // The day as the engine's Npc::currentRoutine() sees it, flat: from minute timelineMinute[k] of the day the
    // entry in force is timelineEntry[k], until the next change, cyclically over midnight. NPC i owns
    // [timelineBegin[i], timelineBegin[i+1]): one element (minute 0) when its entry never changes, none without
    // a routine.
    std::vector<uint32_t>    timelineBegin;
    std::vector<uint16_t>    timelineMinute, timelineEntry;
    size_t size() const { return id.size(); }
    } npcs;
  size_t people = 0;

  struct Routines {
    std::vector<uint16_t>    start, end;     // minutes of the day; end < start crosses midnight
    std::vector<uint16_t>    state;          // index in stateNames
    std::vector<uint32_t>    point;          // NoPoint for `waypoint: null`
    std::vector<std::string> pointName;
    size_t size() const { return start.size(); }
    } routines;
  size_t peopleEntries = 0, unresolvedEntries = 0;

  std::vector<std::string> stateNames;       // ZS_* names, sorted

  Time     startTime() const { return Time(startDay)*DaySeconds+Time(startMinute)*60; }
  uint32_t entryCount(uint32_t npc) const { return npcs.routineBegin[npc+1]-npcs.routineBegin[npc]; }
  uint32_t entryIndex(uint32_t npc, uint16_t entry) const { return npcs.routineBegin[npc]+entry; }
  };

// Reads the export (format "society-world", version 1); throws std::runtime_error with the reason.
World loadWorld(const std::filesystem::path& exportPath);
World parseWorld(std::string_view json);

// The routine entry in force at `t`, exactly as Npc::currentRoutine(requirePoint): the first entry (by start)
// whose [start, end) holds the time of day, an entry with end < start over midnight; otherwise the entry that
// ended last before it (the last of equal ends). requirePoint skips entries with `waypoint: null`.
uint16_t routineAt(const World& world, uint32_t npc, Time t, bool requirePoint = false);
// The entry in force at `t` and the time of its next change (Time max when it never changes), by the timeline.
uint16_t entryAt(const World& world, uint32_t npc, Time t, Time* nextChange = nullptr);

// Length in cm of the shortest walk between two points over the waynet (a freepoint through its near_waypoint and
// a straight segment); -1 when they are in different components. Dijkstra, cached by the pair of waynet points.
class Paths {
  public:
    explicit Paths(const World& world);
    int64_t length(uint32_t from, uint32_t to);
    size_t  cached() const { return cache.size(); }
    size_t  searches() const { return searchCount; }

  private:
    int64_t search(uint32_t from, uint32_t to);

    const World*                         world;
    std::unordered_map<uint64_t,int64_t> cache;
    std::vector<int64_t>                 dist;
    std::vector<uint32_t>                stamp;
    std::vector<std::pair<int64_t,uint32_t>> heap;
    uint32_t                             round = 0;
    size_t                               searchCount = 0;
  };

enum class EventKind : uint8_t {
  RoutineEntry    = 1, // a = state, b = the entry's point (NoPoint for null)
  TravelStarted   = 2, // a = from, b = to, c = arrival time
  Arrived         = 3, // a = point, b = where the walk began
  ActivityStarted = 4, // a = state, b = the point it is done at
  Teleported      = 5, // a = from (NoPoint at the start), b = to, reason
  };

enum class TeleportReason : uint8_t {
  None           = 0,
  StartPlacement = 1, // the start: the NPC appears at start_waypoint
  OtherComponent = 2, // the target is in another waynet component: no walk exists
  };

const char* eventName(EventKind kind);

// One record of the journal; `entry` is the routine entry in force (NoEntry for an NPC without a routine).
struct Event {
  Time      time  = 0;
  uint32_t  npc   = 0;
  EventKind kind  = EventKind::RoutineEntry;
  uint8_t   reason = 0;
  uint16_t  entry = NoEntry;
  uint32_t  a = 0, b = 0;
  Time      c = 0;
  };

// Decision points: v1 has none (the routine decides), so the batch stays empty and act() accepts no choices.
// The layout follows docs/society-sim.md (`DecisionBatch` — flat arrays) for С3.
struct DecisionBatch {
  std::vector<uint32_t> npc;
  std::vector<float>    own, options; // [n, F_s], [n, K, F_c]
  std::vector<uint8_t>  mask;         // [n, K]
  size_t size() const { return npc.size(); }
  void   clear() { npc.clear(); own.clear(); options.clear(); mask.clear(); }
  };

struct Choice {
  uint32_t npc    = 0;
  uint16_t option = 0;
  };

struct SimConfig {
  uint64_t seed       = 0;
  bool     peopleOnly = false; // simulate only `person` NPCs; the others stay where the start put them
  };

// A small generator of its own per world (xoshiro256**): part of the state and of every snapshot. The v1 rules do
// not draw from it; the needs and the teacher of С3 will.
struct Rng {
  std::array<uint64_t,4> s = {};
  void     seed(uint64_t seed);
  uint64_t next();
  };

// What the queue holds: at most one routine change and one arrival per NPC at any time.
struct Pending {
  enum Kind : uint8_t { RoutineChange = 0, Arrival = 1 };
  Time     time = 0;
  uint64_t seq  = 0;
  uint32_t npc  = 0;
  Kind     kind = RoutineChange;
  };

enum NpcFlag : uint8_t {
  Simulated = 1, // in the run (SimConfig::peopleOnly leaves others out)
  Moving    = 2, // on the way: from `place` to `dest`, arrives at `arrive`
  AtStart   = 4, // has not left the start placement yet
  };

// The changing state: arrays by field (SoA), indices only, no pointers; capacities fixed by makeSim.
struct State {
  Time                  now = 0;
  uint64_t              seq = 0;
  Rng                   rng;
  std::vector<Pending>  queue;      // binary heap by (time, seq)
  std::vector<uint32_t> place;      // the point it stands at, or the walk's start while Moving
  std::vector<uint32_t> dest;       // the walk's end, NoPoint when not Moving
  std::vector<Time>     arrive;     // when it arrives, while Moving
  std::vector<uint16_t> entry;      // routine entry in force, NoEntry
  std::vector<uint16_t> state;      // ZS_* being done (index in World::stateNames), NoState while walking
  std::vector<Time>     stateStart; // when that state began
  std::vector<uint8_t>  flags;      // NpcFlag
  std::vector<Event>    events;     // the journal since the last drainEvents
  uint64_t              processed = 0, emitted = 0;
  };

struct Snapshot {
  State state;
  };

struct Sim {
  const World* world = nullptr;
  SimConfig    config;
  State        s;
  Paths        paths;
  uint64_t     seed = 0;
  size_t       drained = 0;
  explicit Sim(const World& w) : world(&w), paths(w) {}
  };

Sim      makeSim(const World& world, const SimConfig& config);
// Back to the export's start (day and minute of `start`): every NPC at start_waypoint doing its entry in force.
void     reset(Sim& sim, uint64_t seed);
// Plays every event up to and including `horizon` and stops there; returns the decision rows put in `out`
// (always 0 in v1: the routine decides).
size_t   advance(Sim& sim, DecisionBatch& out, Time horizon);
// The choices for the rows of the last batch; v1 asks for none, so any choice is a logic error.
void     act(Sim& sim, std::span<const Choice> choices);
Snapshot save(const Sim& sim);
void     restore(Sim& sim, const Snapshot& snapshot);
// The journal since the previous call; valid until the next advance, reset or restore.
std::span<const Event> drainEvents(Sim& sim);

}
