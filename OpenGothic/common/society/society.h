#pragma once

// The society stand's core (docs/society-sim.md, «Интерфейсы → 2. Ядро на C++»): a world of events over the
// engine's `society-world` export (v1, v2 or v3). It plays every NPC by its routine only, with the engine's rules
// measured against the npc-day-parity journal (scripts/society_parity.py). With a config file (data/society/
// config.json, «Файл настройки») people also have needs: levels that fall with game time and rise with what the
// routine has them do; they decide nothing yet. No objects in use or decisions yet.
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

// The engine's routine rules the core repeats (docs/society-sim.md, «Правила»), each measured on the npc-day-parity
// journal of a day of the whole world:
// - a state runs until its end (stateEndAt, Npc::endTime of the entry it was started for), not until the entry in
//   force changes; then the entry in force at that moment starts its state this long after: the old state ends at
//   its loop's next tick and the new one starts at the next routine tick (median of 1552 changes of people: 19 s);
constexpr Time StartDelay   = 20;
// - a state with a follow-on (FollowStates) passes to it this long after the NPC stands at the entry's point:
//   ZS_GotoBed uses the bed and starts ZS_Sleep;
constexpr Time SettleDelay  = 15;
// - an NPC without a routine runs its start_aistate with an end four hours on (Npc::tickRoutine), then starts it
//   again: a monster's ZS_MM_AllScheduler picks its state anew every four hours from the start;
constexpr Time StartStateSeconds = 4*60*60;
// - the routine states walk to the entry's point only when it is farther than TA_DIST_SELFWP_MAX (5 m, the scripts'
//   `if (Npc_GetDistToWP(self,self.wp) > TA_DIST_SELFWP_MAX) AI_GotoWP`): a nearer point is done where it stands.
constexpr int64_t NearPointCm = 500;

// A state the engine passes to by itself once the NPC is at its point (the scripts start it with AI_StartState).
struct FollowState { const char* from; const char* to; };
inline constexpr FollowState FollowStates[] = {{"ZS_GOTOBED","ZS_SLEEP"}};
// The monster dispatcher: its state is the first window of the NPC's `mm_schedule` that holds the hour.
inline constexpr const char* MmScheduler = "ZS_MM_ALLSCHEDULER";

// The config file (docs/society-sim.md, «Файл настройки»), format "society-config" version 1: the needs, what each
// ZS_* state does to them, the kinds of objects (loaded and checked only: С3's next step uses them) and the traits.
// Every rate is per game second here; the file gives hours. Lists keep the file's order: need = index in needs[].
enum class StateKind : uint8_t { Free = 0, Anchor = 1 };

struct Config {
  bool        loaded  = false;               // a file was given
  int         version = 0;
  uint64_t    hash    = 0;                   // FNV-1a 64 of its bytes
  size_t      bytes   = 0;

  struct Needs {
    std::vector<std::string> id, name;
    std::vector<double>      decay;          // per game second: 1 / empty_hours / 3600
    std::vector<double>      low, critical;  // thresholds: low at or below `low`, critical at or below `critical`
    std::vector<double>      start;          // the level at the start of the run
    std::vector<uint8_t>     awakeOnly;      // decays only while not asleep (sleep)
    // The weight of the decay by guild: weightDefault[k] for a guild that is not listed.
    std::vector<double>      weightDefault;
    std::vector<std::unordered_map<uint32_t,double>> weightByGuild;
    size_t size() const { return id.size(); }
    } needs;

  // The ZS_* table, sorted by name: anchor or free, asleep, and the rise of each need per game second while the NPC
  // does the state standing: fill[row*needs + k] = 1 / fill_hours / 3600.
  struct States {
    std::vector<std::string> name;
    std::vector<uint8_t>     kind, asleep;
    std::vector<double>      fill;
    size_t size() const { return name.size(); }
    } states;

  // Kinds of objects: a mobsi scheme (exact) or a freepoint name prefix (the longest wins) -> kind; the state the
  // engine does there, the rise of the needs (as in States), how many at once and for how long.
  struct ObjectKinds {
    std::vector<std::string> id, state;
    std::vector<double>      fill;
    std::vector<uint32_t>    capacity, minutes;
    std::vector<std::string> scheme, prefix;          // sorted
    std::vector<uint16_t>    schemeKind, prefixKind;
    size_t size() const { return id.size(); }
    } objectKinds;

  // Traits: the card scales (characters.json) -> decay multipliers 1 + perPoint × (value − middle); an NPC without
  // a card has `middle` on every scale. Each person's decay of each need also gets its own noise, uniform in
  // ±noise, from the seed.
  std::vector<std::string> scales;           // the scales a card has, in the file's order
  int         middle = 3;
  double      noise  = 0;
  std::string cardsPath;                     // "" for none
  size_t      cards  = 0;                    // cards in that file
  struct TraitRule { uint16_t scale = 0; uint16_t need = 0; double perPoint = 0; };
  std::vector<TraitRule> traits;
  };

// The world that does not change during a run: everything of the export (v1, v2 or v3), read once. Capacities are fixed.
// Every list keeps the export's order and ids: point = `waypoints[].id`, NPC = index in `npcs[]`, object =
// `objects[].id`, routine entry = index in the NPC's `routine[]`.
struct World {
  // Header.
  int         version = 0;                   // of the export: 1, 2 or 3
  std::string worldName, dataFingerprint, engineCommit, movementSource;
  int64_t     startDay = 0;
  int32_t     startMinute = 0;
  double      clockRate = 0;                 // clock.game_seconds_per_real_second
  double      walkCmPerS = 0, runCmPerS = 0; // movement, cm per second of the world's tick
  uint64_t    exportHash = 0;                // FNV-1a 64 of the export's bytes
  size_t      exportBytes = 0;

  struct Points {
    std::vector<std::string>          name;
    std::vector<std::string>          room;      // v3 room name; "" for null or a v1/v2 export
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
    std::vector<std::string>          room; // v3 room name; "" for null or a v1/v2 export
    std::vector<std::array<float,3>>  pos;
    std::vector<uint32_t>             waypoint;
    std::vector<uint8_t>              container;
    size_t size() const { return vob.size(); }
    } objects;
  size_t containers = 0, ownedObjects = 0;

  // v3: distinct room names used by points and objects, sorted; empty in v1/v2. Guild ownership is the room's
  // explicit assignment, not the private owner's guild. No ownership is "" / -1.
  struct Rooms {
    std::vector<std::string> name, ownerNpc;
    std::vector<int32_t>     ownerGuild;
    size_t size() const { return name.size(); }
    } rooms;

  struct Npcs {
    std::vector<std::string> id, key, name;
    std::vector<uint32_t>    guild, startPoint;
    std::vector<uint8_t>     person;
    // v2: the state run without a routine (start_aistate; NoState for none or a v1 export) and the NPC's own walk
    // speed (the world's median when the export has none).
    std::vector<uint16_t>    startState;
    std::vector<double>      walkCmPerS;
    std::vector<uint32_t>    scheduleBegin;  // n+1: mm_schedule windows of NPC i are [scheduleBegin[i], scheduleBegin[i+1])
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

  // The monster schedule (v2 `mm_schedule`): the state and its hours, in the order ZS_MM_AllScheduler checks them;
  // a start of -1 is the scripts' OnlyRoutine (always).
  struct Schedules {
    std::vector<uint16_t> state;
    std::vector<int16_t>  start, end;
    size_t size() const { return state.size(); }
    } schedules;

  std::vector<std::string> stateNames;       // ZS_* names, sorted
  std::vector<uint16_t>    followState;      // by state: its follow-on (FollowStates) or NoState
  uint16_t                 schedulerState = NoState; // ZS_MM_ALLSCHEDULER, when an NPC uses it

  // The config and what it gives this world (empty without a file).
  Config                   config;
  std::vector<uint16_t>    stateRow;         // by state: its row in config.states, NoState when the table has none
  std::vector<uint8_t>     npcCard;          // by NPC: 1 when characters.json has its card
  std::vector<uint8_t>     npcScale;         // [npc × scales]: the card's values, `middle` without a card
  std::vector<double>      npcDecay;         // [npc × needs]: decay per game second by traits and guild, before the
                                             // seed's noise; 0 for an NPC that is not a person (needs are people's)
  std::vector<uint16_t>    objectKind;       // by object: its kind (config.objectKinds) or NoKind
  std::vector<uint16_t>    pointKind;        // by point: a freepoint's kind by its prefix, or NoKind

  Time     startTime() const { return Time(startDay)*DaySeconds+Time(startMinute)*60; }
  uint32_t entryCount(uint32_t npc) const { return npcs.routineBegin[npc+1]-npcs.routineBegin[npc]; }
  uint32_t entryIndex(uint32_t npc, uint16_t entry) const { return npcs.routineBegin[npc]+entry; }
  };

constexpr uint16_t NoKind = 0xFFFFu;

// Reads the export (format "society-world", version 1, 2 or 3) and, when a path is given, the config file; throws
// std::runtime_error with the reason. Without a config, or with `"needs": []`, the run is the routine-only one byte
// for byte. The config's `cards` path is relative to the config's folder.
World loadWorld(const std::filesystem::path& exportPath, const std::filesystem::path& configPath = {});
World parseWorld(std::string_view json, std::string_view config = {}, const std::filesystem::path& configDir = {});

// The routine entry in force at `t`, exactly as Npc::currentRoutine(requirePoint): the first entry (by start)
// whose [start, end) holds the time of day, an entry with end < start over midnight; otherwise the entry that
// ended last before it (the last of equal ends). requirePoint skips entries with `waypoint: null`.
uint16_t routineAt(const World& world, uint32_t npc, Time t, bool requirePoint = false);
// The entry in force at `t` and the time of its next change (Time max when it never changes), by the timeline.
uint16_t entryAt(const World& world, uint32_t npc, Time t, Time* nextChange = nullptr);
// When a state started at `t` for routine entry `entry` ends, as Npc::endTime: the entry's end today or tomorrow,
// whichever is next; an entry that does not hold `t` (a gap in the routine: the entry that ended last is in force)
// ends at the same time tomorrow, so its state runs a whole day. An entry ending at 00:xx with start < end ends
// tomorrow; start == end ends at midnight for 00:00, otherwise at once.
Time     stateEndAt(const World& world, uint32_t npc, uint16_t entry, Time t);
// The state an NPC without a routine runs at `t`: its start_aistate, or for ZS_MM_ALLSCHEDULER the first window of
// its mm_schedule whose start is OnlyRoutine (-1) or whose hours hold the time (Wld_IsTime(start,0,end,0)); the
// dispatcher itself when none does. NoState without a start_aistate.
uint16_t startStateAt(const World& world, uint32_t npc, Time t);

// --- Needs ---------------------------------------------------------------------------------------------------
// A person's level of need k is linear between the journal events that can change its rate (activity_started,
// travel_started, arrived, teleported: «отрезок»), held in 0..1. The rate on a segment is the NPC's own decay
// (none while asleep for an awake-only need) plus, standing, the rise its state gives. The moment a level crosses
// a threshold is found in advance and queued: no ticks. Zone: 0 above `low`, 1 low, 2 critical (at or below).
inline size_t needCount(const World& world) { return world.config.needs.size(); }
// The level per game second of a person with own decay `decay` doing `state` (NoState for none).
double  needRate(const World& world, size_t need, double decay, uint16_t state, bool moving);
// The level at `t` of a segment that began at `since` with `level` and goes at `rate`, held in 0..1. One operation
// per statement: the same bits on every machine.
double  needLevelAt(double level, double rate, Time since, Time t);
uint8_t needZone(const World& world, size_t need, double level);
// The first whole second after `now` at which a segment's zone leaves `zone`; Time max when it never does.
Time    needCrossing(const World& world, size_t need, double level, double rate, Time since, uint8_t zone, Time now);

// Length in cm of the shortest walk between two points over the waynet (a freepoint through its near_waypoint and
// a straight segment); -1 when they are in different components. Dijkstra, cached by the pair of waynet points.
class Paths {
  public:
    explicit Paths(const World& world);
    int64_t length(uint32_t from, uint32_t to);
    // The points of that walk in order, both ends included; empty when there is none. For the marks' positions.
    const std::vector<uint32_t>& route(uint32_t from, uint32_t to);
    // Where a walk from `from` to `to` is after `fraction` (0..1) of its length; integer cm summed as length() does.
    std::array<float,3> along(uint32_t from, uint32_t to, double fraction);
    size_t  cached() const { return cache.size(); }
    size_t  searches() const { return searchCount; }

  private:
    int64_t search(uint32_t from, uint32_t to, std::vector<uint32_t>* points = nullptr);

    const World*                         world;
    std::unordered_map<uint64_t,int64_t> cache;
    std::unordered_map<uint64_t,std::vector<uint32_t>> routes;
    std::vector<uint32_t>                previous;
    std::vector<uint32_t>                walk;
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
  ActivityStarted = 4, // a = state, b = the point it starts at (NoPoint on the way), reason
  Teleported      = 5, // a = from (NoPoint at the start), b = to, reason
  NeedThreshold   = 6, // a = need, b = threshold (1 low, 2 critical), c = the state being done, reason = direction
  };

enum class ActivityReason : uint8_t {
  Entry      = 0, // a routine entry's state, StartDelay after the entry's start (at once at the start)
  StartState = 1, // start_aistate of an NPC without a routine, at the start and every StartStateSeconds
  FollowOn   = 2, // FollowStates: SettleDelay after the NPC stands at its entry's point
  };

enum class NeedDirection : uint8_t {
  Down = 0, // fell to the threshold or below
  Up   = 1, // rose above it
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
  uint64_t seed = 0;
  };

// A small generator of its own per world (xoshiro256**): part of the state and of every snapshot. The routine and
// the needs do not draw from it (each person's noise is a hash of the seed); the teacher of С3 will.
struct Rng {
  std::array<uint64_t,4> s = {};
  void     seed(uint64_t seed);
  uint64_t next();
  };

// What the queue holds: per NPC at most one routine change, one state end, one body event (an arrival or a
// follow-on state) and one need event in force; a body or need event replaced before its time stays in the queue
// and is skipped by its sequence number (State::bodySeq, State::needSeq).
struct Pending {
  // RoutineChange: the entry in force changes (the timeline); Begin: the state's end has passed and the next state
  // starts (StartDelay after stateEnd); Arrival, Settle: body events; Need: a level may cross a threshold.
  enum Kind : uint8_t { RoutineChange = 0, Arrival = 1, Begin = 2, Settle = 3, Need = 4 };
  Time     time = 0;
  uint64_t seq  = 0;
  uint32_t npc  = 0;
  Kind     kind = RoutineChange;
  };

enum NpcFlag : uint8_t {
  Moving  = 1, // on the way: from `place` to `dest`, arrives at `arrive`
  AtStart = 2, // has not left the start placement yet
  };

// The changing state: arrays by field (SoA), indices only, no pointers; capacities fixed by makeSim.
struct State {
  Time                  now = 0;
  uint64_t              seq = 0;
  Rng                   rng;
  std::vector<Pending>  queue;      // binary heap by (time, seq)
  std::vector<uint32_t> place;      // the point it stands at, or the walk's start while Moving
  std::vector<uint32_t> dest;       // the walk's end, NoPoint when not Moving
  std::vector<Time>     depart;     // when the walk began, while Moving
  std::vector<Time>     arrive;     // when it arrives, while Moving
  std::vector<uint64_t> bodySeq;    // the sequence number of the body event in force, 0 for none
  std::vector<uint16_t> entry;      // routine entry in force (Npc::currentRoutine), NoEntry
  std::vector<uint16_t> stateEntry; // the entry whose state is done and whose point it goes to, NoEntry
  std::vector<Time>     stateEnd;   // when that state ends (stateEndAt; four hours on for start_aistate)
  std::vector<uint16_t> state;      // ZS_* being done (index in World::stateNames), also on the way, as the engine
  std::vector<Time>     stateStart; // when that state began
  std::vector<uint8_t>  flags;      // NpcFlag
  // Needs, only with a config that has them (empty otherwise), [npc × needs] or by NPC:
  std::vector<double>   needLevel;  // [npc × needs]: the level at needSince
  std::vector<double>   needRate;   // [npc × needs]: per game second on the segment
  std::vector<double>   needDecay;  // [npc × needs]: own decay per game second, World::npcDecay with the seed's noise
  std::vector<uint8_t>  needZone;   // [npc × needs]: the zone the journal last gave
  std::vector<Time>     needSince;  // by NPC: when its segment began
  std::vector<uint64_t> needSeq;    // by NPC: the sequence number + 1 of its need event in force, 0 for none
  std::vector<Time>     needAt;     // by NPC: that event's time (never later than the first crossing)
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
// Back to the export's start (day and minute of `start`): every NPC at start_waypoint doing its entry in force, or its
// start_aistate without a routine; every person's needs at their `start` level with its own noise from `seed`.
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
// A person's level of need k at the sim's current time (its segment goes on until the next rate event).
double   needLevel(const Sim& sim, uint32_t npc, size_t need);

}
