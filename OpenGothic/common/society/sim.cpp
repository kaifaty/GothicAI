#include "society.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace Society {

// --- Paths -------------------------------------------------------------------------------------------------

Paths::Paths(const World& w)
  : world(&w), dist(w.points.size(),0), stamp(w.points.size(),0) {
  }

int64_t Paths::length(uint32_t from, uint32_t to) {
  if(from==to)
    return 0;
  const auto& p = world->points;
  const uint32_t a = p.graph[from], b = p.graph[to];
  if(p.component[from]!=p.component[to])
    return -1;
  const int64_t ends = int64_t(p.graphCm[from])+p.graphCm[to];
  if(a==b)
    return ends;
  // The waynet is symmetric (the export's contract checks it): one entry per unordered pair.
  const uint64_t key = a<b ? ((uint64_t(a)<<32) | b) : ((uint64_t(b)<<32) | a);
  auto it = cache.find(key);
  if(it==cache.end())
    it = cache.emplace(key,search(a,b)).first;
  return it->second<0 ? -1 : ends+it->second;
  }

int64_t Paths::search(uint32_t from, uint32_t to) {
  ++searchCount;
  if(++round==0) {
    std::fill(stamp.begin(),stamp.end(),0u);
    round = 1;
    }
  const auto later = [](const std::pair<int64_t,uint32_t>& x, const std::pair<int64_t,uint32_t>& y) { return x>y; };
  heap.clear();
  dist[from]  = 0;
  stamp[from] = round;
  heap.emplace_back(0,from);
  const auto& w = *world;
  while(!heap.empty()) {
    std::pop_heap(heap.begin(),heap.end(),later);
    const auto [d,u] = heap.back();
    heap.pop_back();
    if(d!=dist[u])
      continue;
    if(u==to)
      return d;
    for(uint32_t k=w.edgeBegin[u]; k<w.edgeBegin[u+1]; ++k) {
      const uint32_t v  = w.edgeTo[k];
      const int64_t  nd = d+w.edgeCm[k];
      if(stamp[v]!=round || nd<dist[v]) {
        stamp[v] = round;
        dist[v]  = nd;
        heap.emplace_back(nd,v);
        std::push_heap(heap.begin(),heap.end(),later);
        }
      }
    }
  return -1;
  }

// --- Generator ---------------------------------------------------------------------------------------------

void Rng::seed(uint64_t x) {
  // splitmix64 spreads the seed over the four words.
  for(auto& w:s) {
    x += 0x9e3779b97f4a7c15ull;
    uint64_t z = x;
    z = (z^(z>>30))*0xbf58476d1ce4e5b9ull;
    z = (z^(z>>27))*0x94d049bb133111ebull;
    w = z^(z>>31);
    }
  }

uint64_t Rng::next() {
  const auto rotl = [](uint64_t v, int k) { return (v<<k) | (v>>(64-k)); };
  const uint64_t ret = rotl(s[1]*5,7)*9;
  const uint64_t t   = s[1]<<17;
  s[2] ^= s[0];
  s[3] ^= s[1];
  s[1] ^= s[2];
  s[0] ^= s[3];
  s[2] ^= t;
  s[3]  = rotl(s[3],45);
  return ret;
  }

const char* eventName(EventKind kind) {
  switch(kind) {
    case EventKind::RoutineEntry:    return "routine_entry";
    case EventKind::TravelStarted:   return "travel_started";
    case EventKind::Arrived:         return "arrived";
    case EventKind::ActivityStarted: return "activity_started";
    case EventKind::Teleported:      return "teleported";
    }
  return "?";
  }

// --- Rules: routine only -----------------------------------------------------------------------------------

namespace {

bool later(const Pending& x, const Pending& y) {
  return x.time!=y.time ? x.time>y.time : x.seq>y.seq;
  }

void push(State& s, Time t, uint32_t npc, Pending::Kind kind) {
  // At most one routine change and one arrival per NPC: the capacity reserved by makeSim is never exceeded.
  s.queue.push_back({t,s.seq++,npc,kind});
  std::push_heap(s.queue.begin(),s.queue.end(),later);
  }

void emit(State& s, EventKind kind, uint32_t npc, uint8_t reason, uint32_t a, uint32_t b, Time c = 0) {
  s.events.push_back({s.now,npc,kind,reason,s.entry[npc],a,b,c});
  ++s.emitted;
  }

uint32_t entryPoint(const Sim& sim, uint32_t npc) {
  const auto e = sim.s.entry[npc];
  return e==NoEntry ? NoPoint : sim.world->routines.point[sim.world->entryIndex(npc,e)];
  }

void startActivity(Sim& sim, uint32_t npc) {
  auto& s = sim.s;
  const auto e = s.entry[npc];
  if(e==NoEntry) {
    s.state[npc] = NoState;
    return;
    }
  s.state[npc]      = sim.world->routines.state[sim.world->entryIndex(npc,e)];
  s.stateStart[npc] = s.now;
  emit(s,EventKind::ActivityStarted,npc,0,s.state[npc],s.place[npc]);
  }

// A standing NPC goes to its entry's point and starts the entry's state there; an entry without a point
// (`waypoint: null`) is done where the NPC stands, as the engine's AI_GotoWP finds no point for it.
void follow(Sim& sim, uint32_t npc) {
  auto& s = sim.s;
  const auto& w = *sim.world;
  const uint32_t target = entryPoint(sim,npc);
  if(target==NoPoint || target==s.place[npc]) {
    startActivity(sim,npc);
    return;
    }
  s.state[npc]  = NoState;
  s.flags[npc]  = uint8_t(s.flags[npc] & ~AtStart);
  const int64_t cm = sim.paths.length(s.place[npc],target);
  if(cm<0) {
    emit(s,EventKind::Teleported,npc,uint8_t(TeleportReason::OtherComponent),s.place[npc],target);
    s.place[npc] = target;
    startActivity(sim,npc);
    return;
    }
  s.dest[npc]   = target;
  s.arrive[npc] = s.now+travelSeconds(cm,w.walkCmPerS,w.clockRate);
  s.flags[npc]  = uint8_t(s.flags[npc] | Moving);
  emit(s,EventKind::TravelStarted,npc,0,s.place[npc],target,s.arrive[npc]);
  push(s,s.arrive[npc],npc,Pending::Arrival);
  }

void onRoutineChange(Sim& sim, uint32_t npc) {
  auto& s = sim.s;
  const auto& w = *sim.world;
  Time next = 0;
  const auto e = entryAt(w,npc,s.now,&next);
  if(next!=std::numeric_limits<Time>::max())
    push(s,next,npc,Pending::RoutineChange);
  if(e==s.entry[npc] || e==NoEntry)
    return;
  s.entry[npc] = e;
  emit(s,EventKind::RoutineEntry,npc,0,w.routines.state[w.entryIndex(npc,e)],entryPoint(sim,npc));
  // On the way the walk goes on; the arrival looks at the entry in force then.
  if((s.flags[npc] & Moving)==0)
    follow(sim,npc);
  }

void onArrival(Sim& sim, uint32_t npc) {
  auto& s = sim.s;
  const uint32_t from = s.place[npc];
  s.place[npc]   = s.dest[npc];
  s.dest[npc]    = NoPoint;
  s.arrive[npc]  = 0;
  s.flags[npc]   = uint8_t(s.flags[npc] & ~Moving);
  emit(s,EventKind::Arrived,npc,0,s.place[npc],from);
  follow(sim,npc);
  }

// Drops the journal already handed out by drainEvents.
void compact(Sim& sim) {
  auto& ev = sim.s.events;
  if(sim.drained==ev.size())
    ev.clear(); else
    ev.erase(ev.begin(),ev.begin()+std::ptrdiff_t(sim.drained));
  sim.drained = 0;
  }

}

Sim makeSim(const World& world, const SimConfig& config) {
  Sim sim(world);
  sim.config = config;
  const size_t n = world.npcs.size();
  auto& s = sim.s;
  s.place.resize(n);
  s.dest.resize(n);
  s.arrive.resize(n);
  s.entry.resize(n);
  s.state.resize(n);
  s.stateStart.resize(n);
  s.flags.resize(n);
  s.queue.reserve(2*n);
  s.events.reserve(8*n);
  reset(sim,config.seed);
  return sim;
  }

void reset(Sim& sim, uint64_t seed) {
  const auto& w = *sim.world;
  auto& s = sim.s;
  sim.seed    = seed;
  sim.drained = 0;
  s.now       = w.startTime();
  s.seq       = 0;
  s.processed = 0;
  s.emitted   = 0;
  s.rng.seed(seed);
  s.queue.clear();
  s.events.clear();
  for(uint32_t i=0; i<w.npcs.size(); ++i) {
    s.place[i]      = w.npcs.startPoint[i];
    s.dest[i]       = NoPoint;
    s.arrive[i]     = 0;
    s.entry[i]      = NoEntry;
    s.state[i]      = NoState;
    s.stateStart[i] = s.now;
    s.flags[i]      = AtStart;
    emit(s,EventKind::Teleported,i,uint8_t(TeleportReason::StartPlacement),NoPoint,s.place[i]);
    Time next = 0;
    const auto e = entryAt(w,i,s.now,&next);
    if(e==NoEntry)
      continue;
    s.entry[i] = e;
    emit(s,EventKind::RoutineEntry,i,0,w.routines.state[w.entryIndex(i,e)],entryPoint(sim,i));
    // The start does the entry in force where the engine's placement put the NPC: its point, or a free one
    // nearby when that freepoint is taken, or the point of the last entry with a point.
    startActivity(sim,i);
    if(next!=std::numeric_limits<Time>::max())
      push(s,next,i,Pending::RoutineChange);
    }
  }

size_t advance(Sim& sim, DecisionBatch& out, Time horizon) {
  out.clear();
  compact(sim);
  auto& s = sim.s;
  while(!s.queue.empty() && s.queue.front().time<=horizon) {
    std::pop_heap(s.queue.begin(),s.queue.end(),later);
    const Pending p = s.queue.back();
    s.queue.pop_back();
    s.now = p.time;
    ++s.processed;
    if(p.kind==Pending::Arrival)
      onArrival(sim,p.npc); else
      onRoutineChange(sim,p.npc);
    }
  s.now = std::max(s.now,horizon);
  return out.size();
  }

void act(Sim&, std::span<const Choice> choices) {
  if(!choices.empty())
    throw std::logic_error("society core v1 asks for no decisions: the routine decides");
  }

Snapshot save(const Sim& sim) {
  Snapshot ret{sim.s};
  auto& ev = ret.state.events;
  ev.erase(ev.begin(),ev.begin()+std::ptrdiff_t(sim.drained));
  return ret;
  }

void restore(Sim& sim, const Snapshot& snapshot) {
  const size_t n = sim.world->npcs.size();
  if(snapshot.state.place.size()!=n)
    throw std::invalid_argument("society core: the snapshot is of another world");
  sim.s       = snapshot.state;
  sim.drained = 0;
  sim.s.queue.reserve(2*n);
  }

std::span<const Event> drainEvents(Sim& sim) {
  const auto& ev = sim.s.events;
  std::span<const Event> ret(ev.data()+sim.drained,ev.size()-sim.drained);
  sim.drained = ev.size();
  return ret;
  }

}
