#pragma once

// The society stand's core (docs/society-sim.md, «Интерфейсы → 2. Ядро на C++»): a world of events over the
// engine's `society-world` export (v1, v2 or v3). It plays every NPC by its routine only, with the engine's rules
// measured against the npc-day-parity journal (scripts/society_parity.py). With a config file (data/society/
// config.json, «Файл настройки») people also have needs: levels that fall with game time and rise with what the
// routine has them do. With the config's `decisions` section and a policy that decides (teacher, random), a person
// chooses what to do at decision points from a menu with masks (docs/society-sim.md, «Решения»); the routine policy
// asks nothing, and the run is the routine-only one.
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
constexpr uint16_t NoKind  = 0xFFFFu;
constexpr uint16_t NoEntry = 0xFFFFu;
constexpr uint16_t NoState = 0xFFFFu;
constexpr uint32_t NoObject = 0xFFFFFFFFu;
constexpr uint32_t NoNpc    = 0xFFFFFFFFu;
constexpr uint32_t AbsentNpc = 0xFFFFFFFEu; // an owner named in the export who is not among its NPCs

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
// - a state done at a freepoint takes the nearest free one of its name within this distance of where the NPC stands
//   (World::findFreePoint, WayMatrix::distanceThreshold): with decisions a routine's user holds that place (Sim's
//   routine places, docs/society-sim.md «Решения»), as it holds a mobsi of its point.
constexpr int64_t FreePointCm = 800;

// A state the engine passes to by itself once the NPC is at its point (the scripts start it with AI_StartState).
struct FollowState { const char* from; const char* to; };
inline constexpr FollowState FollowStates[] = {{"ZS_GOTOBED","ZS_SLEEP"}};
// The monster dispatcher: its state is the first window of the NPC's `mm_schedule` that holds the hour.
inline constexpr const char* MmScheduler = "ZS_MM_ALLSCHEDULER";

// The config file (docs/society-sim.md, «Файл настройки»), format "society-config" version 1: the needs, what each
// ZS_* state does to them, the kinds of objects (loaded and checked only: С3's next step uses them) and the traits.
// Every rate is per game second here; the file gives hours. Lists keep the file's order: need = index in needs[].
enum class StateKind : uint8_t { Free = 0, Anchor = 1 };
// Where a kind of object is done besides its schemes and freepoints: at any waynet point outside rooms, at any point of
// a room the NPC or its guild owns, at the points of places of places.json (a tavern: anyone may eat there).
enum class KindAt : uint8_t { Listed = 0, Outdoors = 1, OwnRooms = 2, Places = 3 };

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
  // engine does there, the rise of the needs (as in States), how many at once and for how long. `at` (KindAt) places
  // it elsewhere too; `guilds`, when not empty, are the only guilds that may use the kind (sleeping on the ground).
  // A freepoint may belong to several kinds, a mobsi scheme to one.
  struct ObjectKinds {
    std::vector<std::string> id, state;
    std::vector<double>      fill;
    std::vector<uint32_t>    capacity, minutes;
    std::vector<uint8_t>     at;                      // KindAt
    std::vector<std::vector<std::string>> places;     // place ids for KindAt::Places
    std::vector<std::vector<uint32_t>> guilds;
    std::vector<std::string> scheme, prefix;          // sorted
    std::vector<uint16_t>    schemeKind, prefixKind;
    size_t size() const { return id.size(); }
    } objectKinds;

  // Decisions (docs/society-sim.md, «Решение: когда, из чего и что»): the menu, the masks and the teacher. Only with
  // the `decisions` section and needs; read but unused by a routine-only run.
  struct Decisions {
    bool        loaded = false;
    uint32_t    menu = 32;                   // K: options per decision, the routine included
    int64_t     travelLimit = 15*60;         // game seconds of walking to an option
    int64_t     minDwell = 10*60;            // game seconds a chosen activity lasts at least
    bool        postsRelease = false;        // a critical need takes a guard off its post
    std::vector<std::string> postStates;     // ZS_* names; a trailing * is a prefix
    std::vector<uint32_t>    noNeedsGuilds;  // guilds without needs (and so without decisions)
    std::string protectedPath;               // data/society/protected.json, from the config's folder
    std::string placesPath;                  // the server's places.json, for kinds at places (taverns)
    std::unordered_map<std::string,uint32_t> guildNames; // a mobsi's owner_guild (GIL_*) -> the export's guild id
    // The teacher: drive D = (Σ weight·|1 − h|^n)^(1/m), its fall over `horizon` seconds, minus `travelCost` per
    // game second of walking; `inertia` for the activity in hand; softmax with `temperature` among the `top` best.
    double      n = 4, m = 2;
    int64_t     horizon = 60*60;
    double      travelCost = 0, inertia = 0, temperature = 0;
    uint32_t    top = 3;
    std::vector<double> weight;              // by need
    } decisions;

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
                                             // seed's noise; 0 for an NPC without needs and for a need not in use
  std::vector<uint8_t>     npcNeeds;         // by NPC: has needs: a person, not of decisions.no_needs_guilds
  std::vector<uint16_t>    objectKind;       // by object: its kind (config.objectKinds) or NoKind
  std::vector<uint16_t>    pointKind;        // by point: a freepoint's kind by its prefix, or NoKind

  // What the decisions section gives this world (empty without it). The states of the object kinds the export lacks
  // (ZS_SLEEP_GROUND) are appended to stateNames, sorted among themselves, so the export's indices stay.
  std::vector<uint16_t>    kindState;        // by object kind: its state in stateNames
  std::vector<uint8_t>     statePost;        // by state: a guard's post (decisions.post_states)
  std::vector<uint8_t>     npcProtected;     // by NPC: in the protected list
  std::vector<uint8_t>     npcDecider;       // by NPC: a person with a routine, not protected, of a guild that decides
  std::vector<uint32_t>    objectOwnerNpc;   // by object: the private owner (NPC index), NoNpc, or AbsentNpc
  std::vector<int32_t>     objectOwnerGuild; // by object: the owning guild, -1 none, -2 a guild nobody here has
  std::vector<uint32_t>    pointOwnerNpc;    // by point: the same from its room
  std::vector<int32_t>     pointOwnerGuild;
  std::vector<uint8_t>     pointOutdoors;    // by point: a waynet point outside rooms
  // The places of options by waynet point, CSR, each with its kind: kinded objects at that point (objects[].waypoint);
  // kinded freepoints whose near_waypoint it is; the point itself when outdoors; points of owned rooms and of places
  // whose waynet point it is. Encoded by TargetPoint.
  std::vector<uint32_t>    targetBegin, targets;
  std::vector<uint16_t>    targetKind;
  // The places a routine's user may hold where it stands, by point, CSR, nearest first, each with its kind (listed
  // kinds only, encoded by TargetPoint): a freepoint is its own place; a waynet point has its kinded mobsi
  // (objects[].waypoint) and the kinded freepoints within FreePointCm of it.
  std::vector<uint32_t>    spotBegin, spots;
  std::vector<uint16_t>    spotKind;
  // Needs in use: [npc × needs] 1 when a way to fill the need (its routine's own states, or an option it may use) is
  // within the travel limit of a point of its routine (start_waypoint without one); a need without a way neither
  // falls nor counts. A need of zero decay (faith of guild weight 0) is in use and never falls. Empty without
  // decisions: every need is in use.
  std::vector<uint8_t>     needActive;
  // The needs not in use with the nearest way beyond the limit: content gaps, not a lower threshold.
  struct Unmet {
    uint32_t npc = 0;
    uint16_t need = 0;
    uint16_t kind = NoKind;        // the nearest way's kind, NoKind for none in the waynet component
    uint32_t point = 0xFFFFFFFFu;  // its place
    uint32_t object = 0xFFFFFFFFu; // its mobsi, or none
    int64_t  travel = -1;          // game seconds of walking from the nearest point of the routine
    };
  std::vector<Unmet>       unmet;
  double                   deciderWalk = 0;  // the fastest decider's walk, cm/s: the reach of the menu
  size_t                   protectedListed = 0, protectedFound = 0;

  Time     startTime() const { return Time(startDay)*DaySeconds+Time(startMinute)*60; }
  uint32_t entryCount(uint32_t npc) const { return npcs.routineBegin[npc+1]-npcs.routineBegin[npc]; }
  uint32_t entryIndex(uint32_t npc, uint16_t entry) const { return npcs.routineBegin[npc]+entry; }
  };

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
  RoutineEntry      = 1, // a = state, b = the entry's point (NoPoint for null)
  TravelStarted     = 2, // a = from, b = to, c = arrival time
  Arrived           = 3, // a = point, b = where the walk began
  ActivityStarted   = 4, // a = state, b = the point it starts at (NoPoint on the way), reason
  Teleported        = 5, // a = from (NoPoint at the start), b = to, reason
  NeedThreshold     = 6, // a = need, b = threshold (1 low, 2 critical), c = the state being done, reason = direction
  // Decisions (journal v4), each with the option's kind (NoKind for the routine), point and object (NoObject):
  DecisionRequested = 7, // a = options in the menu, b = of them allowed, c = 0; reason = the causes (Cause bits)
  ActionChosen      = 8, // a = kind, b = point, c = object; reason = who chose (Chooser)
  ActionFailed      = 9, // a = kind, b = point, c = object; reason = FailReason
  ActivityFinished  = 10,// a = kind, b = point, c = object; reason = FinishReason
  // A place the routine uses (journal v5, only with decisions): the NPC stands at it doing its kind's state, or lets it
  // go; a = kind, b = point, c = object (NoObject for a freepoint). Places count the routine's users as they count the
  // chosen activities.
  PlaceHeld         = 11,
  PlaceReleased     = 12,
  };

enum class ActivityReason : uint8_t {
  Entry      = 0, // a routine entry's state, StartDelay after the entry's start (at once at the start)
  StartState = 1, // start_aistate of an NPC without a routine, at the start and every StartStateSeconds
  FollowOn   = 2, // FollowStates: SettleDelay after the NPC stands at its entry's point
  Chosen     = 3, // a chosen option's state, before the walk to it
  Return     = 4, // the routine's state again after a chosen activity
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

// Why a decision is asked (bits of decision_requested's reason).
enum Cause : uint8_t {
  CauseStart          = 1,  // the run starts in a free entry
  CauseFreeEntry      = 2,  // the state of a free routine entry begins
  CauseNeedLow        = 4,  // a need fell to low in a free entry or a chosen activity
  CauseNeedCritical   = 8,  // a need fell to critical in a free entry or a chosen activity
  CauseAnchorCritical = 16, // a need is critical in an anchor entry (it fell there, or the entry begins so)
  CauseFinished       = 32, // the chosen activity is over
  CauseForecast       = 64, // in a free entry a need would be critical during the next anchor entry
  };

enum class Chooser : uint8_t {
  Routine  = 0, // the only allowed option was the routine: the core takes it itself
  Teacher  = 1,
  Random   = 2,
  Net      = 3,
  External = 4,
  };

enum class FailReason : uint8_t {
  Taken       = 1, // full: an earlier choice of the same batch took the last place
  Interrupted = 2, // an anchor entry began on the way to it
  Evicted     = 3, // the place's routine user came to it while the NPC was on the way (journal v5)
  };

enum class FinishReason : uint8_t {
  Setpoint = 0, // its needs are at their setpoint (1)
  MaxTime  = 1, // its kind's longest time
  Switched = 2, // a decision chose something else
  Anchor   = 3, // an anchor entry (or a guard's post) began without a critical need
  Evicted  = 4, // the place's routine user came to it (journal v5)
  AboveLow = 5, // in an anchor entry: its needs that were low or critical are above low (journal v5; v4 wrote 0)
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

// --- Decisions -----------------------------------------------------------------------------------------------
// The menu of a decision (docs/society-sim.md, «Решения»): option 0 is the routine, always allowed; then up to K-1
// options to use an object of a kind in the config (a mobsi, a kinded freepoint, an outdoor point) within the travel
// limit, nearest first: the nearest allowed of every kind, the other allowed ones, then closed ones (mask 0).
constexpr size_t MenuSize = 32;

enum class OptionType : uint8_t { Routine = 0, Object = 1, Point = 2 };
enum class Access : uint8_t { Public = 0, Own = 1, Guild = 2, Other = 3 };

struct Option {
  OptionType type   = OptionType::Routine;
  Access     access = Access::Public;
  uint16_t   kind   = NoKind;   // config.objectKinds; NoKind for the routine
  uint16_t   state  = NoState;  // what it does there (the routine's: its entry's state)
  uint32_t   point  = NoPoint;  // where it is done (the routine's: its entry's point)
  uint32_t   object = NoObject;
  uint32_t   use    = 0;        // holders now, reservations included, without the deciding NPC
  Time       travel = 0;        // game seconds until it stands there (a walk under way included)
  };

// Flat arrays, `stride` = K slots per row: menu and mask [n, K], `count` options per row. The feature arrays `own`
// and `options` are for the encoder of С4 and stay empty.
struct DecisionBatch {
  size_t                stride = MenuSize;
  std::vector<uint32_t> npc;
  std::vector<uint8_t>  cause;        // by row: Cause bits
  std::vector<uint8_t>  count;        // by row: options in the menu
  std::vector<Option>   menu;         // [n, K]
  std::vector<uint8_t>  mask;         // [n, K]: 1 allowed
  std::vector<float>    own, options; // [n, F_s], [n, K, F_c]
  size_t size() const { return npc.size(); }
  void   clear() { npc.clear(); cause.clear(); count.clear(); menu.clear(); mask.clear(); own.clear(); options.clear(); }
  };

struct Choice {
  uint32_t npc    = 0;
  uint16_t option = 0;
  Chooser  by     = Chooser::External;
  };

struct SimConfig {
  uint64_t seed = 0;
  // People decide (the config needs its `decisions` section): advance hands out the decision rows and act takes a
  // choice for each. Off, the routine decides as before, byte for byte.
  bool     decisions = false;
  };

// A small generator of its own per world (xoshiro256**): part of the state and of every snapshot. The routine and
// the needs do not draw from it (each person's noise is a hash of the seed); the teacher and the random policy do.
struct Rng {
  std::array<uint64_t,4> s = {};
  void     seed(uint64_t seed);
  uint64_t next();
  double   unit();               // uniform in [0, 1)
  };

// What the queue holds: per NPC at most one routine change, one state end, one body event (an arrival or a
// follow-on state), one need event and one activity event in force; a body, need or activity event replaced before
// its time stays in the queue and is skipped by its sequence number (State::bodySeq, needSeq, actSeq).
struct Pending {
  // RoutineChange: the entry in force changes (the timeline); Begin: the state's end has passed and the next state
  // starts (StartDelay after stateEnd); Arrival, Settle: body events; Need: a level may cross a threshold;
  // Activity: a chosen activity's end or the end of its least time with a decision deferred.
  enum Kind : uint8_t { RoutineChange = 0, Arrival = 1, Begin = 2, Settle = 3, Need = 4, Activity = 5 };
  Time     time = 0;
  uint64_t seq  = 0;
  uint32_t npc  = 0;
  Kind     kind = RoutineChange;
  };

enum NpcFlag : uint8_t {
  Moving       = 1, // on the way: from `place` to `dest`, arrives at `arrive`
  AtStart      = 2, // has not left the start placement yet
  PendingStart = 4, // a routine entry's state waits for the decision asked at its start
  };

// What a decider does: its routine, a chosen activity (walking to it or at it), or a chosen activity just over,
// waiting for the decision of this same second.
enum class Mode : uint8_t { Routine = 0, Chosen = 1, Finished = 2 };

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
  // Decisions, only with SimConfig::decisions (empty otherwise), by NPC unless said:
  std::vector<uint8_t>  mode;       // Mode
  std::vector<uint8_t>  want;       // Cause bits asked at this second, 0 for none
  std::vector<uint8_t>  deferred;   // Cause bits held until the chosen activity's least time is over
  std::vector<uint8_t>  actType;    // the chosen activity: OptionType, kind, where, the object
  std::vector<uint16_t> actKind;
  std::vector<uint32_t> actPoint, actObject;
  std::vector<Time>     actBegin;   // when it began at its place, -1 on the way
  std::vector<Time>     actMin;     // its least time is over
  std::vector<Time>     actEnd;     // its end: needs at the setpoint or its kind's longest time
  std::vector<uint8_t>  actFinish;  // the FinishReason at actEnd: Setpoint, AboveLow (an anchor entry) or MaxTime
  std::vector<uint64_t> actSeq;     // the sequence number + 1 of its activity event in force, 0 for none
  std::vector<uint16_t> objectUse;  // by object: holders — the routine's users, chosen activities, reservations
  std::vector<uint16_t> pointUse;   // by point: the same
  std::vector<uint32_t> routineTarget; // the place the routine uses now (World::targets' encoding), NoObject for none
  std::vector<uint16_t> routineKind;   // that place's kind
  std::vector<Time>     forecastFor; // the start of the anchor entry a forecast decision was asked for, -1 none
  std::vector<uint32_t> asked;      // NPCs with `want`, in the order asked
  DecisionBatch         batch;      // the rows handed out and not answered yet
  uint64_t              decisions = 0;
  std::vector<Event>    events;     // the journal since the last drainEvents
  uint64_t              processed = 0, emitted = 0;
  };

struct Snapshot {
  State state;
  };

// Options by waynet point within the menu's reach (the fastest decider walking the travel limit), nearest first:
// a bounded Dijkstra over the waynet once per point, then the places of World::targets. A pure function of the world,
// outside the snapshot.
class Nearby {
  public:
    explicit Nearby(const World& world);
    struct Item { int32_t cm; uint32_t target; uint16_t kind; };
    const std::vector<Item>& at(uint32_t waynetPoint);
    size_t built() const { return builtCount; }

  private:
    const World*                         world;
    int64_t                              reach = 0;
    std::vector<std::vector<Item>>       lists;
    std::vector<uint8_t>                 done;
    std::vector<int64_t>                 dist;
    std::vector<uint32_t>                stamp;
    std::vector<uint32_t>                visited;
    std::vector<std::pair<int64_t,uint32_t>> heap;
    uint32_t                             round = 0;
    size_t                               builtCount = 0;
  };

// World::targets: an object or a point.
constexpr uint32_t TargetPoint = 0x80000000u;
inline bool     targetIsPoint(uint32_t t) { return (t & TargetPoint)!=0; }
inline uint32_t targetId(uint32_t t) { return t & ~TargetPoint; }

struct Sim {
  const World* world = nullptr;
  SimConfig    config;
  State        s;
  Paths        paths;
  Nearby       nearby;
  uint64_t     seed = 0;
  size_t       drained = 0;
  explicit Sim(const World& w) : world(&w), paths(w), nearby(w) {}
  };

Sim      makeSim(const World& world, const SimConfig& config);
// Back to the export's start (day and minute of `start`): every NPC at start_waypoint doing its entry in force, or its
// start_aistate without a routine; every person's needs at their `start` level with its own noise from `seed`.
void     reset(Sim& sim, uint64_t seed);
// Plays every event up to and including `horizon` and stops there, or earlier at the first second with decisions
// asked: then `out` holds a row per deciding NPC (in NPC order) and the time stays at that second. Returns the rows.
size_t   advance(Sim& sim, DecisionBatch& out, Time horizon);
// The choices for the rows of the last batch, one per row in its order. A choice out of its row's mask is a logic
// error (std::logic_error): the policy broke the rules. A place taken by an earlier choice of the same batch is
// `action_failed`, and the NPC keeps what it did.
void     act(Sim& sim, std::span<const Choice> choices);
Snapshot save(const Sim& sim);
void     restore(Sim& sim, const Snapshot& snapshot);
// The journal since the previous call; valid until the next advance, reset or restore.
std::span<const Event> drainEvents(Sim& sim);
// A person's level of need k at the sim's current time (its segment goes on until the next rate event).
double   needLevel(const Sim& sim, uint32_t npc, size_t need);

// --- Policies: a batch in, a choice per row out -----------------------------------------------------------------
// The teacher's score of every option of the batch ([n, K], -inf for a closed slot): how much lower the drive
// D = (Σ w·|1 − h|^n)^(1/m) is after the option (the walk, the activity, then the routine) than after the routine over
// the same window — the horizon, but no shorter than the walk and an hour of the activity — minus the walk's cost,
// plus the inertia of the activity in hand (the routine's, scored 0, when it is not a chosen one). In a free entry a
// need forecast to be critical in the next anchor run counts at its forecast low.
void     teacherScores(const Sim& sim, const DecisionBatch& batch, std::span<float> scores);
// The teacher's pick: a weighted draw among the `top` best allowed options, softmax with the temperature (the best
// at 0), on the world's generator.
void     teacher(Sim& sim, const DecisionBatch& batch, std::vector<Choice>& out);
// A uniform draw among the allowed options of each row, on the world's generator: the check of menus and masks.
void     randomPolicy(Sim& sim, const DecisionBatch& batch, std::vector<Choice>& out);
// The drive of levels h (needCount values) by the config's teacher.
double   drive(const World& world, std::span<const double> levels);
// Whether `npc` decides at all: a person with a routine, not protected, of a guild that decides.
inline bool decides(const World& world, uint32_t npc) { return !world.npcDecider.empty() && world.npcDecider[npc]!=0; }

}
