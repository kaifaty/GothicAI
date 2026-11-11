#include "core.h"

#include <algorithm>
#include <cmath>
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

double Rng::unit() {
  return double(next()>>11)*0x1.0p-53;
  }

const char* eventName(EventKind kind) {
  switch(kind) {
    case EventKind::RoutineEntry:      return "routine_entry";
    case EventKind::TravelStarted:     return "travel_started";
    case EventKind::Arrived:           return "arrived";
    case EventKind::ActivityStarted:   return "activity_started";
    case EventKind::Teleported:        return "teleported";
    case EventKind::NeedThreshold:     return "need_threshold";
    case EventKind::DecisionRequested: return "decision_requested";
    case EventKind::ActionChosen:      return "action_chosen";
    case EventKind::ActionFailed:      return "action_failed";
    case EventKind::ActivityFinished:  return "activity_finished";
    case EventKind::PlaceHeld:         return "place_held";
    case EventKind::PlaceReleased:     return "place_released";
    }
  return "?";
  }

// --- Needs -------------------------------------------------------------------------------------------------

double needRate(const World& w, size_t k, double decay, uint16_t state, bool moving) {
  const auto& c   = w.config;
  const auto  row = state==NoState ? NoState : w.stateRow[state];
  const bool  doing = !moving && row!=NoState;
  double ret = doing && c.states.asleep[row] && c.needs.awakeOnly[k] ? 0.0 : -decay;
  if(doing)
    ret += c.states.fill[size_t(row)*c.needs.size()+k];
  return ret;
  }

double needLevelAt(double level, double rate, Time since, Time t) {
  const double change = rate*double(t-since);
  const double v      = level+change;
  return std::clamp(v,0.0,1.0);
  }

uint8_t needZone(const World& w, size_t k, double level) {
  if(level<=w.config.needs.critical[k])
    return 2;
  return level<=w.config.needs.low[k] ? 1 : 0;
  }

Time needCrossing(const World& w, size_t k, double level, double rate, Time since, uint8_t zone, Time now) {
  constexpr Time Never = std::numeric_limits<Time>::max();
  const auto& n = w.config.needs;
  double target = 0;
  if(rate<0 && zone<2)
    target = zone==0 ? n.low[k] : n.critical[k];
  else if(rate>0 && zone>0)
    target = zone==2 ? n.critical[k] : n.low[k];
  else
    return Never;
  // Seconds from `since` to the threshold, then the exact whole second by the level itself (rounding moves it by one
  // at most): the first second after `now` whose zone is not `zone`.
  const double dt = (target-level)/rate;
  if(!(dt<1e12))
    return Never;
  Time t = since+Time(std::ceil(std::max(dt,0.0)));
  if(t<=now)
    t = now+1;
  const auto zoneAt = [&](Time x) { return needZone(w,k,needLevelAt(level,rate,since,x)); };
  for(int i=0; zoneAt(t)==zone; ++i) {
    if(i>1000)
      throw std::logic_error("society core: a need's crossing is not where its rate puts it");
    ++t;
    }
  while(t-1>now && zoneAt(t-1)!=zone)
    --t;
  return t;
  }

double needLevel(const Sim& sim, uint32_t npc, size_t k) {
  const auto& s = sim.s;
  const size_t i = size_t(npc)*needCount(*sim.world)+k;
  return needLevelAt(s.needLevel[i],s.needRate[i],s.needSince[npc],s.now);
  }

// --- Rules: routine only -----------------------------------------------------------------------------------

namespace detail {

namespace {

bool later(const Pending& x, const Pending& y) {
  return x.time!=y.time ? x.time>y.time : x.seq>y.seq;
  }

void record(State& s, EventKind kind, uint32_t npc, uint8_t reason, uint32_t a, uint32_t b, Time c = 0) {
  s.events.push_back({s.now,npc,kind,reason,s.entry[npc],a,b,c});
  ++s.emitted;
  }

bool hasNeeds(const Sim& sim, uint32_t npc) {
  return !sim.s.needSince.empty() && sim.world->npcNeeds[npc]!=0;
  }

// The zone of each need at `now` on the current segment; every threshold passed since the journal last said is
// written now, in order (low before critical going down, critical before low going up), need by need. A decider's
// need that falls may ask for a decision (decide.cpp).
void crossings(Sim& sim, uint32_t npc) {
  auto& s = sim.s;
  const auto& w = *sim.world;
  const size_t n = needCount(w);
  const bool asks = deciding(sim) && decides(w,npc);
  for(size_t k=0; k<n; ++k) {
    const size_t i = size_t(npc)*n+k;
    const auto zone = needZone(w,k,needLevelAt(s.needLevel[i],s.needRate[i],s.needSince[npc],s.now));
    const uint32_t state = s.state[npc]==NoState ? 0xFFFFu : s.state[npc];
    while(s.needZone[i]<zone) {
      ++s.needZone[i];
      record(s,EventKind::NeedThreshold,npc,uint8_t(NeedDirection::Down),uint32_t(k),s.needZone[i],state);
      if(asks)
        onDown(sim,npc,s.needZone[i]);
      }
    while(s.needZone[i]>zone) {
      record(s,EventKind::NeedThreshold,npc,uint8_t(NeedDirection::Up),uint32_t(k),s.needZone[i],state);
      --s.needZone[i];
      }
    }
  }

// The NPC's need event: at the first crossing of any need, or earlier when one is queued already (it then finds
// nothing, and the next one is queued). An earlier event is never replaced by a later one.
void scheduleNeed(Sim& sim, uint32_t npc) {
  auto& s = sim.s;
  const auto& w = *sim.world;
  const size_t n = needCount(w);
  Time next = std::numeric_limits<Time>::max();
  for(size_t k=0; k<n; ++k) {
    const size_t i = size_t(npc)*n+k;
    next = std::min(next,needCrossing(w,k,s.needLevel[i],s.needRate[i],s.needSince[npc],s.needZone[i],s.now));
    }
  if(next==std::numeric_limits<Time>::max() || (s.needSeq[npc]!=0 && s.needAt[npc]<=next))
    return;
  s.needSeq[npc] = push(s,next,npc,Pending::Need)+1;
  s.needAt[npc]  = next;
  }

// A new segment from now: the levels so far, the rates of what the NPC does now.
void segment(Sim& sim, uint32_t npc) {
  auto& s = sim.s;
  const auto& w = *sim.world;
  const size_t n = needCount(w);
  const bool walking = moving(s,npc);
  for(size_t k=0; k<n; ++k) {
    const size_t i = size_t(npc)*n+k;
    s.needLevel[i] = needLevelAt(s.needLevel[i],s.needRate[i],s.needSince[npc],s.now);
    s.needRate[i]  = needRate(w,k,s.needDecay[i],s.state[npc],walking);
    }
  s.needSince[npc] = s.now;
  scheduleNeed(sim,npc);
  if(deciding(sim) && decides(w,npc))
    onSegment(sim,npc);
  }

// The events that can change what a person does (its state, standing or on the way): the ends of its needs' segments.
bool changesRate(EventKind kind) {
  return kind==EventKind::ActivityStarted || kind==EventKind::TravelStarted || kind==EventKind::Arrived ||
         kind==EventKind::Teleported;
  }

void onNeed(Sim& sim, uint32_t npc) {
  sim.s.needSeq[npc] = 0;
  crossings(sim,npc);
  scheduleNeed(sim,npc);
  }

// The NPC stands where its entry wants it: a state with a follow-on passes to it after SettleDelay.
void settle(Sim& sim, uint32_t npc) {
  auto& s = sim.s;
  if(s.state[npc]!=NoState && sim.world->followState[s.state[npc]]!=NoState)
    pushBody(s,s.now+SettleDelay,npc,Pending::Settle); else
    s.bodySeq[npc] = 0;
  if(deciding(sim))
    routinePlace(sim,npc);
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
// walk goes on and the arrival looks at the new point. Without a routine start_aistate starts again. A decider's
// entry begins under the rules of decisions.
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
  if(deciding(sim) && decides(w,npc)) {
    beginEntry(sim,npc);
    return;
    }
  const bool walking = moving(s,npc);
  startEntry(sim,npc,walking ? NoPoint : s.place[npc]);
  if(!walking)
    follow(sim,npc);
  }

void onSettle(Sim& sim, uint32_t npc) {
  auto& s = sim.s;
  s.bodySeq[npc] = 0;
  const auto next = s.state[npc]==NoState ? NoState : sim.world->followState[s.state[npc]];
  if(next!=NoState && !moving(s,npc))
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
  emit(sim,EventKind::RoutineEntry,npc,0,w.routines.state[w.entryIndex(npc,e)],entryPoint(sim,npc,e));
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
  emit(sim,EventKind::Arrived,npc,0,s.place[npc],from);
  follow(sim,npc);
  }

// A uniform number in [0, 1) of the seed and a slot (splitmix64): a person's noise does not depend on the order of
// anything else and draws nothing from the world's generator.
double unit(uint64_t seed, uint64_t slot) {
  uint64_t z = seed+0x9e3779b97f4a7c15ull*(slot+1);
  z = (z^(z>>30))*0xbf58476d1ce4e5b9ull;
  z = (z^(z>>27))*0x94d049bb133111ebull;
  z = z^(z>>31);
  return double(z>>11)*0x1.0p-53;
  }

// Every person at the start level of each need, in zone 0 (a start below a threshold crosses it at the start), with
// its own decay: traits and guild (World::npcDecay) times 1 ± noise from the seed.
void resetNeeds(Sim& sim, uint64_t seed) {
  auto& s = sim.s;
  if(s.needSince.empty())
    return;
  const auto& w = *sim.world;
  const size_t n = needCount(w);
  for(uint32_t npc=0; npc<w.npcs.size(); ++npc) {
    s.needSince[npc] = s.now;
    s.needSeq[npc]   = 0;
    s.needAt[npc]    = 0;
    for(size_t k=0; k<n; ++k) {
      const size_t i = size_t(npc)*n+k;
      const double u = unit(seed,i);
      const double spread = 2.0*u-1.0;
      const double noise  = w.config.noise*spread;
      s.needDecay[i] = w.npcDecay[i]*(1.0+noise);
      // A need not in use stays at 1 (it never falls), whatever the start.
      s.needLevel[i] = w.needActive.empty() || w.needActive[i] ? w.config.needs.start[k] : 1.0;
      s.needRate[i]  = 0;
      s.needZone[i]  = 0;
      }
    }
  }

// Drops the journal already handed out by drainEvents.
void compact(Sim& sim) {
  auto& ev = sim.s.events;
  if(sim.drained==ev.size())
    ev.clear(); else
    ev.erase(ev.begin(),ev.begin()+std::ptrdiff_t(sim.drained));
  sim.drained = 0;
  }

size_t queueCapacity(const Sim& sim) {
  const size_t n = sim.world->npcs.size();
  return (sim.s.needSince.empty() ? 3 : deciding(sim) ? 5 : 4)*n;
  }

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

// An event of the journal. One that can change what a person does ends its needs' segment: thresholds passed until
// now come first, a new segment starts after the event.
void emit(Sim& sim, EventKind kind, uint32_t npc, uint8_t reason, uint32_t a, uint32_t b, Time c) {
  auto& s = sim.s;
  const bool needs = changesRate(kind) && hasNeeds(sim,npc);
  if(needs)
    crossings(sim,npc);
  record(s,kind,npc,reason,a,b,c);
  if(needs)
    segment(sim,npc);
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
    emit(sim,EventKind::ActivityStarted,npc,uint8_t(reason),state,at);
  else if(hasNeeds(sim,npc)) {
    // No state and so no event: only a person without a routine and without a start_aistate (none in Khorinis).
    crossings(sim,npc);
    segment(sim,npc);
    }
  if(deciding(sim))
    routinePlace(sim,npc);
  }

bool near(const World& w, uint32_t a, uint32_t b) {
  const auto& p = w.points.pos[a];
  const auto& q = w.points.pos[b];
  const double dx = double(p[0])-q[0], dy = double(p[1])-q[1], dz = double(p[2])-q[2];
  const double xx = dx*dx, yy = dy*dy, zz = dz*dz;
  const double sum = xx+yy;
  return sum+zz<=double(NearPointCm)*double(NearPointCm);
  }

// A standing NPC goes to the point of the entry whose state it does, or of its chosen activity; an entry without a
// point (`waypoint: null`, the engine's AI_GotoWP finds none) or a point within NearPointCm is done where it stands.
void follow(Sim& sim, uint32_t npc) {
  auto& s = sim.s;
  const auto& w = *sim.world;
  const bool chosen = deciding(sim) && s.mode[npc]==uint8_t(Mode::Chosen);
  const uint32_t target = chosen ? s.actPoint[npc] : entryPoint(sim,npc,s.stateEntry[npc]);
  const auto there = [&]() {
    if(chosen)
      arrivedChosen(sim,npc); else
      settle(sim,npc);
    };
  if(target==NoPoint || target==s.place[npc] || near(w,target,s.place[npc])) {
    there();
    return;
    }
  s.flags[npc] = uint8_t(s.flags[npc] & ~AtStart);
  const int64_t cm = sim.paths.length(s.place[npc],target);
  if(cm<0) {
    emit(sim,EventKind::Teleported,npc,uint8_t(TeleportReason::OtherComponent),s.place[npc],target);
    s.place[npc] = target;
    there();
    return;
    }
  s.dest[npc]   = target;
  s.depart[npc] = s.now;
  s.arrive[npc] = s.now+travelSeconds(cm,w.npcs.walkCmPerS[npc],w.clockRate);
  s.flags[npc]  = uint8_t(s.flags[npc] | Moving);
  emit(sim,EventKind::TravelStarted,npc,0,s.place[npc],target,s.arrive[npc]);
  pushBody(s,s.arrive[npc],npc,Pending::Arrival);
  if(deciding(sim))
    routinePlace(sim,npc);
  }

}

using namespace detail;

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
  if(needCount(world)>0) {
    const size_t m = n*needCount(world);
    s.needLevel.resize(m);
    s.needRate.resize(m);
    s.needDecay.resize(m);
    s.needZone.resize(m);
    s.needSince.resize(n);
    s.needSeq.resize(n);
    s.needAt.resize(n);
    }
  if(config.decisions) {
    if(!world.config.decisions.loaded || needCount(world)==0)
      throw std::invalid_argument("society core: decisions need a config with needs and its decisions section");
    sizeDecisions(sim);
    }
  s.queue.reserve(queueCapacity(sim));
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
  resetNeeds(sim,seed);
  if(deciding(sim))
    resetDecisions(sim);
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
    emit(sim,EventKind::Teleported,i,uint8_t(TeleportReason::StartPlacement),NoPoint,s.place[i]);
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
    emit(sim,EventKind::RoutineEntry,i,0,w.routines.state[w.entryIndex(i,e)],entryPoint(sim,i,e));
    // The start does the entry in force where the engine's placement put the NPC: its point, or a free one
    // nearby when that freepoint is taken, or the point of the last entry with a point. It does not walk.
    startEntry(sim,i,s.place[i]);
    settle(sim,i);
    if(next!=std::numeric_limits<Time>::max())
      push(s,next,i,Pending::RoutineChange);
    if(deciding(sim) && decides(w,i))
      startDecisions(sim,i);
    }
  }

size_t advance(Sim& sim, DecisionBatch& out, Time horizon) {
  out.clear();
  if(!sim.s.batch.npc.empty())
    throw std::logic_error("society core: advance before act answered the last batch");
  compact(sim);
  auto& s = sim.s;
  const bool decide = deciding(sim);
  Time   builtAt = -1;
  size_t builds  = 0;
  for(;;) {
    const bool more = !s.queue.empty() && s.queue.front().time<=horizon;
    // Decisions asked at this second are handed out once every event of the second is played.
    if(decide && !s.asked.empty() && (!more || s.queue.front().time>s.now)) {
      builds = builtAt==s.now ? builds+1 : 1;
      builtAt = s.now;
      if(builds>1000)
        throw std::logic_error("society core: decisions keep asking for more at one second");
      if(buildBatch(sim,out)>0)
        return out.size();
      continue;
      }
    if(!more)
      break;
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
    if(p.kind==Pending::Need) {
      if(s.needSeq[p.npc]==p.seq+1)
        onNeed(sim,p.npc);
      continue;
      }
    if(p.kind==Pending::Activity) {
      if(s.actSeq[p.npc]==p.seq+1)
        onActivity(sim,p.npc);
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

void act(Sim& sim, std::span<const Choice> choices) {
  if(!deciding(sim)) {
    if(!choices.empty())
      throw std::logic_error("society core: this run asks for no decisions: the routine decides");
    return;
    }
  applyChoices(sim,choices);
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
  if(snapshot.state.needSince.size()!=sim.s.needSince.size() || snapshot.state.mode.size()!=sim.s.mode.size())
    throw std::invalid_argument("society core: the snapshot is of a run with other needs or decisions");
  sim.s       = snapshot.state;
  sim.drained = 0;
  sim.s.queue.reserve(queueCapacity(sim));
  }

std::span<const Event> drainEvents(Sim& sim) {
  const auto& ev = sim.s.events;
  std::span<const Event> ret(ev.data()+sim.drained,ev.size()-sim.drained);
  sim.drained = ev.size();
  return ret;
  }

}
