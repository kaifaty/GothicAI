#include "society.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace Society {

// --- Paths -------------------------------------------------------------------------------------------------

Paths::Paths(const World& w)
  : world(&w), previous(w.points.size(),NoPoint), dist(w.points.size(),0), stamp(w.points.size(),0) {
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

const std::vector<uint32_t>& Paths::route(uint32_t from, uint32_t to) {
  const uint64_t key = (uint64_t(from)<<32) | to;
  if(auto it = routes.find(key); it!=routes.end())
    return it->second;
  auto& ret = routes[key];
  const auto& p = world->points;
  if(from==to) {
    ret.push_back(from);
    return ret;
    }
  if(p.component[from]!=p.component[to])
    return ret;
  ret.push_back(from);
  const uint32_t a = p.graph[from], b = p.graph[to];
  if(a!=b) {
    walk.clear();
    search(a,b,&walk);
    for(auto q:walk)
      if(q!=ret.back())
        ret.push_back(q);
    } else if(a!=from) {
    ret.push_back(a);
    }
  if(ret.back()!=to)
    ret.push_back(to);
  return ret;
  }

std::array<float,3> Paths::along(uint32_t from, uint32_t to, double fraction) {
  const auto& w = *world;
  const auto& r = route(from,to);
  if(r.size()<2)
    return w.points.pos[from];
  // The length of each step as length() sums it: the edge's integer length on the waynet, the straight segment
  // between a freepoint and its near_waypoint.
  const auto step = [&](uint32_t u, uint32_t v) -> int64_t {
    if(w.points.free[u] || w.points.free[v])
      return w.points.free[u] ? w.points.graphCm[u] : w.points.graphCm[v];
    for(uint32_t k=w.edgeBegin[u]; k<w.edgeBegin[u+1]; ++k)
      if(w.edgeTo[k]==v)
        return w.edgeCm[k];
    return 0;
    };
  int64_t total = 0;
  for(size_t i=1; i<r.size(); ++i)
    total += step(r[i-1],r[i]);
  double left = std::clamp(fraction,0.0,1.0)*double(total);
  for(size_t i=1; i<r.size(); ++i) {
    const double l = double(step(r[i-1],r[i]));
    if(left<=l || i+1==r.size()) {
      const double f = l>0 ? std::clamp(left/l,0.0,1.0) : 1.0;
      const auto& a = w.points.pos[r[i-1]];
      const auto& b = w.points.pos[r[i]];
      return {float(a[0]+(b[0]-a[0])*f),float(a[1]+(b[1]-a[1])*f),float(a[2]+(b[2]-a[2])*f)};
      }
    left -= l;
    }
  return w.points.pos[to];
  }

int64_t Paths::search(uint32_t from, uint32_t to, std::vector<uint32_t>* points) {
  ++searchCount;
  if(++round==0) {
    std::fill(stamp.begin(),stamp.end(),0u);
    round = 1;
    }
  const auto later = [](const std::pair<int64_t,uint32_t>& x, const std::pair<int64_t,uint32_t>& y) { return x>y; };
  heap.clear();
  dist[from]     = 0;
  stamp[from]    = round;
  previous[from] = NoPoint;
  heap.emplace_back(0,from);
  const auto& w = *world;
  while(!heap.empty()) {
    std::pop_heap(heap.begin(),heap.end(),later);
    const auto [d,u] = heap.back();
    heap.pop_back();
    if(d!=dist[u])
      continue;
    if(u==to) {
      if(points!=nullptr) {
        points->clear();
        for(uint32_t q=to; q!=NoPoint; q=previous[q])
          points->push_back(q);
        std::reverse(points->begin(),points->end());
        }
      return d;
      }
    for(uint32_t k=w.edgeBegin[u]; k<w.edgeBegin[u+1]; ++k) {
      const uint32_t v  = w.edgeTo[k];
      const int64_t  nd = d+w.edgeCm[k];
      if(stamp[v]!=round || nd<dist[v]) {
        stamp[v]    = round;
        dist[v]     = nd;
        previous[v] = u;
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

uint64_t push(State& s, Time t, uint32_t npc, Pending::Kind kind) {
  s.queue.push_back({t,s.seq,npc,kind});
  std::push_heap(s.queue.begin(),s.queue.end(),later);
  return s.seq++;
  }

// The one body event of an NPC in force: an earlier one left in the queue is skipped when it comes up.
void pushBody(State& s, Time t, uint32_t npc, Pending::Kind kind) {
  s.bodySeq[npc] = push(s,t,npc,kind)+1;
  }

void emit(State& s, EventKind kind, uint32_t npc, uint8_t reason, uint32_t a, uint32_t b, Time c = 0) {
  s.events.push_back({s.now,npc,kind,reason,s.entry[npc],a,b,c});
  ++s.emitted;
  }

uint32_t entryPoint(const Sim& sim, uint32_t npc, uint16_t e) {
  return e==NoEntry ? NoPoint : sim.world->routines.point[sim.world->entryIndex(npc,e)];
  }

uint16_t entryState(const Sim& sim, uint32_t npc, uint16_t e) {
  return e==NoEntry ? NoState : sim.world->routines.state[sim.world->entryIndex(npc,e)];
  }

void setState(Sim& sim, uint32_t npc, uint16_t state, ActivityReason reason, uint32_t at) {
  auto& s = sim.s;
  s.state[npc]      = state;
  s.stateStart[npc] = s.now;
  if(state!=NoState)
    emit(s,EventKind::ActivityStarted,npc,uint8_t(reason),state,at);
  }

bool near(const World& w, uint32_t a, uint32_t b) {
  const auto& p = w.points.pos[a];
  const auto& q = w.points.pos[b];
  const double dx = double(p[0])-q[0], dy = double(p[1])-q[1], dz = double(p[2])-q[2];
  const double xx = dx*dx, yy = dy*dy, zz = dz*dz;
  const double sum = xx+yy;
  return sum+zz<=double(NearPointCm)*double(NearPointCm);
  }

// The NPC stands where its entry wants it: a state with a follow-on passes to it after SettleDelay.
void settle(Sim& sim, uint32_t npc) {
  auto& s = sim.s;
  if(s.state[npc]!=NoState && sim.world->followState[s.state[npc]]!=NoState)
    pushBody(s,s.now+SettleDelay,npc,Pending::Settle); else
    s.bodySeq[npc] = 0;
  }

// A standing NPC goes to the point of the entry whose state it does; an entry without a point (`waypoint: null`, the
// engine's AI_GotoWP finds none) or a point within NearPointCm is done where it stands.
void follow(Sim& sim, uint32_t npc) {
  auto& s = sim.s;
  const auto& w = *sim.world;
  const uint32_t target = entryPoint(sim,npc,s.stateEntry[npc]);
  if(target==NoPoint || target==s.place[npc] || near(w,target,s.place[npc])) {
    settle(sim,npc);
    return;
    }
  s.flags[npc] = uint8_t(s.flags[npc] & ~AtStart);
  const int64_t cm = sim.paths.length(s.place[npc],target);
  if(cm<0) {
    emit(s,EventKind::Teleported,npc,uint8_t(TeleportReason::OtherComponent),s.place[npc],target);
    s.place[npc] = target;
    settle(sim,npc);
    return;
    }
  s.dest[npc]   = target;
  s.depart[npc] = s.now;
  s.arrive[npc] = s.now+travelSeconds(cm,w.npcs.walkCmPerS[npc],w.clockRate);
  s.flags[npc]  = uint8_t(s.flags[npc] | Moving);
  emit(s,EventKind::TravelStarted,npc,0,s.place[npc],target,s.arrive[npc]);
  pushBody(s,s.arrive[npc],npc,Pending::Arrival);
  }

// The state of the entry in force begins and runs until its end; the next state begins StartDelay after that.
void startEntry(Sim& sim, uint32_t npc, uint32_t at) {
  auto& s = sim.s;
  const auto e = entryAt(*sim.world,npc,s.now);
  s.stateEntry[npc] = e;
  s.stateEnd[npc]   = stateEndAt(*sim.world,npc,e,s.now);
  setState(sim,npc,entryState(sim,npc,e),ActivityReason::Entry,at);
  push(s,s.stateEnd[npc]+StartDelay,npc,Pending::Begin);
  }

// The state's end has passed: the entry in force starts its state and the NPC sets off to its point; on the way the
// walk goes on and the arrival looks at the new point. Without a routine start_aistate starts again.
void onBegin(Sim& sim, uint32_t npc) {
  auto& s = sim.s;
  const auto& w = *sim.world;
  if(w.entryCount(npc)==0) {
    // The engine restarts it even when the dispatcher picks the same state (zs_end, zs_start in its journal).
    setState(sim,npc,startStateAt(w,npc,s.now),ActivityReason::StartState,s.place[npc]);
    s.stateEnd[npc] = s.now+StartStateSeconds;
    push(s,s.stateEnd[npc]+StartDelay,npc,Pending::Begin);
    return;
    }
  const bool moving = (s.flags[npc] & Moving)!=0;
  startEntry(sim,npc,moving ? NoPoint : s.place[npc]);
  if(!moving)
    follow(sim,npc);
  }

void onSettle(Sim& sim, uint32_t npc) {
  auto& s = sim.s;
  s.bodySeq[npc] = 0;
  const auto next = s.state[npc]==NoState ? NoState : sim.world->followState[s.state[npc]];
  if(next!=NoState && (s.flags[npc] & Moving)==0)
    setState(sim,npc,next,ActivityReason::FollowOn,s.place[npc]);
  }

// The entry in force changes (as the engine's currentRoutine does at the entry's start); the state goes on until its
// own end, which for a routine without gaps is this same moment.
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
  emit(s,EventKind::RoutineEntry,npc,0,w.routines.state[w.entryIndex(npc,e)],entryPoint(sim,npc,e));
  }

void onArrival(Sim& sim, uint32_t npc) {
  auto& s = sim.s;
  const uint32_t from = s.place[npc];
  s.place[npc]   = s.dest[npc];
  s.dest[npc]    = NoPoint;
  s.depart[npc]  = 0;
  s.arrive[npc]  = 0;
  s.flags[npc]   = uint8_t(s.flags[npc] & ~Moving);
  s.bodySeq[npc] = 0;
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
  s.stateEntry.resize(n);
  s.stateEnd.resize(n);
  s.dest.resize(n);
  s.depart.resize(n);
  s.arrive.resize(n);
  s.bodySeq.resize(n);
  s.entry.resize(n);
  s.state.resize(n);
  s.stateStart.resize(n);
  s.flags.resize(n);
  s.queue.reserve(3*n);
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
    s.depart[i]     = 0;
    s.arrive[i]     = 0;
    s.bodySeq[i]    = 0;
    s.entry[i]      = NoEntry;
    s.stateEntry[i] = NoEntry;
    s.stateEnd[i]   = 0;
    s.state[i]      = NoState;
    s.stateStart[i] = s.now;
    s.flags[i]      = AtStart;
    emit(s,EventKind::Teleported,i,uint8_t(TeleportReason::StartPlacement),NoPoint,s.place[i]);
    if(w.entryCount(i)==0) {
      // Without a routine: start_aistate (the monster dispatcher's pick), started again every four hours.
      setState(sim,i,startStateAt(w,i,s.now),ActivityReason::StartState,s.place[i]);
      if(s.state[i]!=NoState) {
        s.stateEnd[i] = s.now+StartStateSeconds;
        push(s,s.stateEnd[i]+StartDelay,i,Pending::Begin);
        }
      continue;
      }
    Time next = 0;
    const auto e = entryAt(w,i,s.now,&next);
    s.entry[i] = e;
    emit(s,EventKind::RoutineEntry,i,0,w.routines.state[w.entryIndex(i,e)],entryPoint(sim,i,e));
    // The start does the entry in force where the engine's placement put the NPC: its point, or a free one
    // nearby when that freepoint is taken, or the point of the last entry with a point. It does not walk.
    startEntry(sim,i,s.place[i]);
    settle(sim,i);
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
    if(p.kind==Pending::RoutineChange) {
      onRoutineChange(sim,p.npc);
      continue;
      }
    if(p.kind==Pending::Begin) {
      onBegin(sim,p.npc);
      continue;
      }
    // A body event replaced by a later one is skipped.
    if(s.bodySeq[p.npc]!=p.seq+1)
      continue;
    if(p.kind==Pending::Arrival)
      onArrival(sim,p.npc); else
      onSettle(sim,p.npc);
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
  sim.s.queue.reserve(3*n);
  }

std::span<const Event> drainEvents(Sim& sim) {
  const auto& ev = sim.s.events;
  std::span<const Event> ret(ev.data()+sim.drained,ev.size()-sim.drained);
  sim.drained = ev.size();
  return ret;
  }

}
