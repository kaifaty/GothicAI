// Decisions of the society stand's core (docs/society-sim.md, «Решения»): when a decider is asked, the menu of
// options and their masks, what a choice does (the walk, the activity, its end, the reservations), the teacher and
// the random policy. The routine and the needs are sim.cpp's.
#include "core.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace Society {

// --- Nearby: the places of options around a waynet point ------------------------------------------------------

Nearby::Nearby(const World& w) : world(&w) {
  if(w.config.decisions.loaded && w.clockRate>0) {
    lists.resize(w.points.size());
    done.assign(w.points.size(),0);
    dist.assign(w.points.size(),0);
    stamp.assign(w.points.size(),0);
    // The farthest a decider walks in the travel limit (travelSeconds rounds to the second), and a metre more.
    const double cm = double(w.config.decisions.travelLimit+1)*w.deciderWalk/w.clockRate;
    reach = int64_t(std::ceil(cm))+100;
    }
  }

const std::vector<Nearby::Item>& Nearby::at(uint32_t g) {
  if(done[g])
    return lists[g];
  done[g] = 1;
  ++builtCount;
  const auto& w = *world;
  if(++round==0) {
    std::fill(stamp.begin(),stamp.end(),0u);
    round = 1;
    }
  // Dijkstra from g bounded by the reach: the exact shortest lengths of every waynet point within it.
  const auto later = [](const std::pair<int64_t,uint32_t>& x, const std::pair<int64_t,uint32_t>& y) { return x>y; };
  heap.clear();
  visited.clear();
  dist[g]  = 0;
  stamp[g] = round;
  heap.emplace_back(0,g);
  while(!heap.empty()) {
    std::pop_heap(heap.begin(),heap.end(),later);
    const auto [d,u] = heap.back();
    heap.pop_back();
    if(d!=dist[u])
      continue;
    visited.push_back(u);
    for(uint32_t k=w.edgeBegin[u]; k<w.edgeBegin[u+1]; ++k) {
      const uint32_t v  = w.edgeTo[k];
      const int64_t  nd = d+w.edgeCm[k];
      if(nd>reach)
        continue;
      if(stamp[v]!=round || nd<dist[v]) {
        stamp[v] = round;
        dist[v]  = nd;
        heap.emplace_back(nd,v);
        std::push_heap(heap.begin(),heap.end(),later);
        }
      }
    }
  auto& list = lists[g];
  for(auto u:visited)
    for(uint32_t k=w.targetBegin[u]; k<w.targetBegin[u+1]; ++k) {
      const uint32_t t = w.targets[k];
      // A freepoint is its near_waypoint and the straight segment to it, as Paths::length sums it.
      const int64_t cm = dist[u]+(targetIsPoint(t) ? w.points.graphCm[targetId(t)] : 0);
      if(cm<=reach)
        list.push_back({int32_t(cm),t,w.targetKind[k]});
      }
  std::sort(list.begin(),list.end(),[](const Item& a, const Item& b) {
    return a.cm!=b.cm ? a.cm<b.cm : a.target!=b.target ? a.target<b.target : a.kind<b.kind;
    });
  return list;
  }

namespace detail {
namespace {

constexpr Time Never = std::numeric_limits<Time>::max();

enum class Hold : uint8_t { Free = 0, Anchor = 1, Post = 2 };

const Config::Decisions& dec(const Sim& sim) {
  return sim.world->config.decisions;
  }

Mode modeOf(const State& s, uint32_t npc) {
  return Mode(s.mode[npc]);
  }

// What the routine's entry in force allows: a guard's post holds (unless posts release for needs), an anchor entry
// gives way to a critical need only, a free one to any decision.
Hold hold(const Sim& sim, uint32_t npc) {
  const auto& w = *sim.world;
  const auto state = entryState(sim,npc,sim.s.entry[npc]);
  if(state==NoState)
    return Hold::Free;
  if(w.statePost[state] && !dec(sim).postsRelease)
    return Hold::Post;
  const auto row = w.stateRow[state];
  return row!=NoState && w.config.states.kind[row]==uint8_t(StateKind::Anchor) ? Hold::Anchor : Hold::Free;
  }

bool critical(const Sim& sim, uint32_t npc) {
  const size_t n = needCount(*sim.world);
  for(size_t k=0; k<n; ++k)
    if(sim.s.needZone[size_t(npc)*n+k]==2)
      return true;
  return false;
  }

// The state fills a need that is critical now.
bool fillsCritical(const Sim& sim, uint32_t npc, uint16_t state) {
  const auto& w = *sim.world;
  const auto row = state==NoState ? NoState : w.stateRow[state];
  if(row==NoState)
    return false;
  const size_t n = needCount(w);
  for(size_t k=0; k<n; ++k)
    if(sim.s.needZone[size_t(npc)*n+k]==2 && w.config.states.fill[size_t(row)*n+k]>0)
      return true;
  return false;
  }

// The routine holds the NPC in this entry: an anchor entry or a guard's post.
bool anchorEntry(const Sim& sim, uint32_t npc, uint16_t e) {
  const auto& w = *sim.world;
  const auto state = entryState(sim,npc,e);
  if(state==NoState)
    return false;
  if(w.statePost[state])
    return true;
  const auto row = w.stateRow[state];
  return row!=NoState && w.config.states.kind[row]==uint8_t(StateKind::Anchor);
  }

// The next run of anchor entries after the free entry in force, within a day: where each of its entries begins and
// its state, and where the run ends.
struct AnchorRun {
  Time begin = -1, end = -1;
  std::array<std::pair<Time,uint16_t>,8> piece = {};
  size_t pieces = 0;
  };

bool nextAnchorRun(const Sim& sim, uint32_t npc, AnchorRun& run) {
  const auto& w = *sim.world;
  const Time now = sim.s.now;
  Time next = 0;
  auto e = entryAt(w,npc,now,&next);
  if(e==NoEntry || anchorEntry(sim,npc,e))
    return false;
  while(next!=Never && (run.pieces>0 || next-now<=DaySeconds)) {
    const Time t = next;
    e = entryAt(w,npc,t,&next);
    if(!anchorEntry(sim,npc,e)) {
      if(run.pieces>0) {
        run.end = t;
        return true;
        }
      continue;
      }
    if(run.pieces==run.piece.size()) {
      run.end = t;
      return true;
      }
    if(run.pieces==0)
      run.begin = t;
    run.piece[run.pieces++] = {t,entryState(sim,npc,e)};
    }
  return false;
  }

// The lowest level need k reaches by the end of that run: its free entry's state until the run, then each anchor
// entry's state as it is done at its point (into bed is asleep).
double forecastLow(const Sim& sim, uint32_t npc, size_t k, const AnchorRun& run) {
  const auto& w = *sim.world;
  const auto& s = sim.s;
  const size_t i = size_t(npc)*needCount(w)+k;
  const double decay = s.needDecay[i];
  double level = needLevel(sim,npc,k);
  level = needLevelAt(level,needRate(w,k,decay,entryState(sim,npc,s.entry[npc]),false),s.now,run.begin);
  double low = level;
  for(size_t j=0; j<run.pieces; ++j) {
    auto state = run.piece[j].second;
    if(state!=NoState && w.followState[state]!=NoState)
      state = w.followState[state];
    const Time from = run.piece[j].first, to = j+1<run.pieces ? run.piece[j+1].first : run.end;
    level = needLevelAt(level,needRate(w,k,decay,state,false),from,to);
    low   = std::min(low,level);
    }
  return low;
  }

// The needs of a decider in a free entry that would be critical in its next anchor run: by how much the forecast low
// is under the level now (0 for the others). Returns the run's start, -1 when none would be.
Time forecast(const Sim& sim, uint32_t npc, std::span<double> under) {
  const auto& w = *sim.world;
  const size_t n = needCount(w);
  std::fill(under.begin(),under.end(),0.0);
  AnchorRun run;
  if(sim.s.entry[npc]==NoEntry || !nextAnchorRun(sim,npc,run))
    return -1;
  bool any = false;
  for(size_t k=0; k<n; ++k) {
    const double low = forecastLow(sim,npc,k,run);
    if(low<=w.config.needs.critical[k]) {
      under[k] = low-needLevel(sim,npc,k);
      any = true;
      }
    }
  return any ? run.begin : -1;
  }

uint16_t& holders(State& s, OptionType type, uint32_t object, uint32_t point) {
  return type==OptionType::Object ? s.objectUse[object] : s.pointUse[point];
  }

void scheduleActivity(Sim& sim, uint32_t npc) {
  auto& s = sim.s;
  Time next = s.actEnd[npc];
  if(s.deferred[npc]!=0)
    next = std::min(next,s.actMin[npc]);
  s.actSeq[npc] = push(s,std::max(next,s.now),npc,Pending::Activity)+1;
  }

void release(Sim& sim, uint32_t npc) {
  auto& s = sim.s;
  auto& h = holders(s,OptionType(s.actType[npc]),s.actObject[npc],s.actPoint[npc]);
  if(h==0)
    throw std::logic_error("society core: a reservation is released twice");
  --h;
  s.actSeq[npc]   = 0;
  s.actKind[npc]  = NoKind;
  s.deferred[npc] = 0;
  }

void emitOption(Sim& sim, EventKind kind, uint32_t npc, uint8_t reason) {
  const auto& s = sim.s;
  emit(sim,kind,npc,reason,s.actKind[npc],s.actPoint[npc],s.actObject[npc]);
  }

// The chosen activity stops before its end: on the way it failed, at its place it finished.
void stopChosen(Sim& sim, uint32_t npc, FinishReason reason) {
  auto& s = sim.s;
  if(s.actBegin[npc]<0)
    emitOption(sim,EventKind::ActionFailed,npc,uint8_t(FailReason::Interrupted)); else
    emitOption(sim,EventKind::ActivityFinished,npc,uint8_t(reason));
  release(sim,npc);
  s.mode[npc] = uint8_t(Mode::Routine);
  }

// The routine's entry under way takes over again: its state, then the walk to its point.
void toRoutine(Sim& sim, uint32_t npc, ActivityReason reason) {
  auto& s = sim.s;
  s.mode[npc]  = uint8_t(Mode::Routine);
  s.flags[npc] = uint8_t(s.flags[npc] & ~PendingStart);
  const bool walking = moving(s,npc);
  setState(sim,npc,entryState(sim,npc,s.stateEntry[npc]),reason,walking ? NoPoint : s.place[npc]);
  if(!walking)
    follow(sim,npc);
  }

// What choosing the routine does in each mode.
void routine(Sim& sim, uint32_t npc) {
  auto& s = sim.s;
  switch(modeOf(s,npc)) {
    case Mode::Chosen:
      stopChosen(sim,npc,FinishReason::Switched);
      toRoutine(sim,npc,ActivityReason::Return);
      return;
    case Mode::Finished:
      toRoutine(sim,npc,ActivityReason::Return);
      return;
    case Mode::Routine:
      if(s.flags[npc] & PendingStart)
        toRoutine(sim,npc,ActivityReason::Entry);
      return;
    }
  }

// The routine's place of the NPC goes free.
void releaseRoutine(Sim& sim, uint32_t npc) {
  auto& s = sim.s;
  const uint32_t t = s.routineTarget[npc];
  if(t==NoObject)
    return;
  const uint32_t id = targetId(t);
  auto& h = targetIsPoint(t) ? s.pointUse[id] : s.objectUse[id];
  if(h==0)
    throw std::logic_error("society core: a routine's place is released twice");
  --h;
  const uint16_t kind = s.routineKind[npc];
  s.routineTarget[npc] = NoObject;
  s.routineKind[npc]   = NoKind;
  emit(sim,EventKind::PlaceReleased,npc,0,kind,targetIsPoint(t) ? id : sim.world->objects.waypoint[id],
       targetIsPoint(t) ? NoObject : id);
  }

// The place an option is: its target (TargetPoint for a point).
uint32_t targetOf(OptionType type, uint32_t object, uint32_t point) {
  return type==OptionType::Object ? object : (point | TargetPoint);
  }

void startChosen(Sim& sim, uint32_t npc, const Option& o) {
  auto& s = sim.s;
  s.mode[npc]      = uint8_t(Mode::Chosen);
  s.flags[npc]     = uint8_t(s.flags[npc] & ~PendingStart);
  s.actType[npc]   = uint8_t(o.type);
  s.actKind[npc]   = o.kind;
  s.actPoint[npc]  = o.point;
  s.actObject[npc] = o.object;
  s.actBegin[npc]  = -1;
  s.actMin[npc]    = Never;
  s.actEnd[npc]    = Never;
  s.actFinish[npc] = uint8_t(FinishReason::MaxTime);
  s.actSeq[npc]    = 0;
  s.deferred[npc]  = 0;
  releaseRoutine(sim,npc);
  ++holders(s,o.type,o.object,o.point);
  const bool walking = moving(s,npc);
  if(!walking)
    s.bodySeq[npc] = 0; // a follow-on of the routine (into bed) no longer comes
  setState(sim,npc,o.state,ActivityReason::Chosen,walking ? NoPoint : s.place[npc]);
  if(!walking)
    follow(sim,npc);
  }

// How long an activity of `state` lasts for these levels: until every need it fills is at the setpoint (1), within
// [least, longest] of its kind: the least is min_dwell_min, but never over the longest. A state that fills nothing
// lasts its kind's longest time. In an anchor entry the activity only takes the needs it fills out of danger: it
// lasts until each of them that is low or critical is above low. `full`: the target comes first.
Time duration(const Sim& sim, uint32_t npc, uint16_t kind, uint16_t state, std::span<const double> levels, bool anchored,
              bool& full) {
  const auto& w = *sim.world;
  const size_t n = needCount(w);
  const Time longest = Time(w.config.objectKinds.minutes[kind])*60;
  const Time least   = std::min(dec(sim).minDwell,longest);
  const auto row = w.stateRow[state];
  Time t = -1;
  for(size_t k=0; k<n && row!=NoState; ++k) {
    if(!(w.config.states.fill[size_t(row)*n+k]>0))
      continue;
    const double rate = needRate(w,k,sim.s.needDecay[size_t(npc)*n+k],state,false);
    const double low  = w.config.needs.low[k];
    const double goal = anchored ? (levels[k]<=low ? low : levels[k]) : 1.0;
    double need = 0;
    if(levels[k]<goal || (anchored && levels[k]<=low))
      need = rate>0 ? std::floor((goal-levels[k])/rate)+1 : 1e18;
    t = std::max(t,Time(std::min(need,1e15)));
    }
  if(t<0) {
    full = false;
    return longest;
    }
  full = t<=longest;
  return std::clamp(t,least,longest);
  }

void levelsNow(const Sim& sim, uint32_t npc, std::vector<double>& out) {
  const size_t n = needCount(*sim.world);
  out.resize(n);
  for(size_t k=0; k<n; ++k)
    out[k] = needLevel(sim,npc,k);
  }

// Game seconds of walking from `from` to `to` as follow() would do it: none within NearPointCm, for no point or
// across components (a teleport).
Time walk(Sim& sim, uint32_t npc, uint32_t from, uint32_t to) {
  const auto& w = *sim.world;
  if(to==NoPoint || to==from || near(w,to,from))
    return 0;
  const int64_t cm = sim.paths.length(from,to);
  return cm<0 ? 0 : travelSeconds(cm,w.npcs.walkCmPerS[npc],w.clockRate);
  }

// The menu of one row: the routine, then the options within the travel limit (docs/society-sim.md, «Меню»).
void buildRow(Sim& sim, uint32_t npc, DecisionBatch& b) {
  auto& s = sim.s;
  const auto& w = *sim.world;
  const auto& d = dec(sim);
  const size_t K = b.stride;
  const size_t at = b.menu.size();
  b.menu.resize(at+K);
  b.mask.resize(at+K,0);
  const bool walking = moving(s,npc);
  const uint32_t origin = walking ? s.dest[npc] : s.place[npc];
  const Time     ahead  = walking ? s.arrive[npc]-s.now : 0;
  const Mode     mode   = modeOf(s,npc);
  const Hold     h      = hold(sim,npc);

  Option r;
  r.type  = OptionType::Routine;
  r.state = entryState(sim,npc,s.stateEntry[npc]);
  r.point = entryPoint(sim,npc,s.stateEntry[npc]);
  if(mode==Mode::Routine && !(s.flags[npc] & PendingStart))
    r.travel = ahead; else
    r.travel = ahead+walk(sim,npc,origin,r.point);
  b.menu[at] = r;
  b.mask[at] = 1;

  // Candidates nearest first; the nearest allowed of every kind leads, an outdoor kind offers only that one.
  struct Cand { Option o; bool ok; };
  thread_local std::vector<Cand> cands;
  thread_local std::vector<uint8_t> seen;
  cands.clear();
  seen.assign(w.config.objectKinds.size(),0);
  const auto& list = sim.nearby.at(w.points.graph[origin]);
  const int64_t offset = w.points.graphCm[origin];
  const double  speed  = w.npcs.walkCmPerS[npc];
  const bool    actPoint = mode==Mode::Chosen;
  const auto consider = [&](OptionType type, uint16_t kind, uint32_t object, uint32_t point, Access acc, Time travel) {
    Option o;
    o.type   = type;
    o.access = acc;
    o.kind   = kind;
    o.state  = w.kindState[kind];
    o.point  = point;
    o.object = object;
    o.travel = ahead+travel;
    const bool mine = (actPoint && OptionType(s.actType[npc])==type && s.actKind[npc]==kind && s.actPoint[npc]==point &&
                       s.actObject[npc]==object) || s.routineTarget[npc]==targetOf(type,object,point);
    o.use = uint32_t(holders(s,type,object,point))-(mine ? 1u : 0u);
    const bool ok = acc!=Access::Other && guildMay(w,npc,kind) && o.use<w.config.objectKinds.capacity[kind] &&
                    o.travel<=d.travelLimit && (h==Hold::Free || fillsCritical(sim,npc,o.state));
    cands.push_back({o,ok});
    return ok;
    };
  // The limit holds for the whole walk from where the NPC is: the rest of a walk under way, then from its end. The
  // longest walk within what is left, in cm (travelSeconds rounds to the second), so the scan compares integers.
  const Time budget = d.travelLimit-ahead;
  int64_t reach = budget<0 ? -1 : int64_t(std::floor((double(budget)+0.5)*speed/w.clockRate))+2;
  while(reach>=0 && travelSeconds(reach,speed,w.clockRate)>budget)
    --reach;
  // A few of each kind are enough: the nearest allowed leads, the rest only fill the menu. The closed ones do not use
  // up a kind's allowed places (a full bed nearby must not hide a free one farther away), and fill at most two slots.
  constexpr uint8_t PerKind = 4, ClosedPerKind = 2;
  thread_local std::vector<uint8_t> allowedOf, closedOf;
  allowedOf.assign(w.config.objectKinds.size(),0);
  closedOf.assign(w.config.objectKinds.size(),0);
  for(const auto& it:list) {
    const uint32_t id = targetId(it.target);
    const uint32_t point = targetIsPoint(it.target) ? id : w.objects.waypoint[id];
    const int64_t cm = point==origin ? 0 : offset+it.cm;
    // Nearest first: the first one past the limit ends the list. Within NearPointCm it is done where it stands (a
    // walk is never shorter than the straight line, so only short walks can be that near).
    if(cm>reach)
      break;
    const auto kind = it.kind;
    const auto travel = [&]() { return cm<=NearPointCm && near(w,point,origin) ? Time(0) : travelSeconds(cm,speed,w.clockRate); };
    const auto type = targetIsPoint(it.target) ? OptionType::Point : OptionType::Object;
    const auto object = targetIsPoint(it.target) ? NoObject : id;
    if(KindAt(w.config.objectKinds.at[kind])==KindAt::Listed) {
      if(allowedOf[kind]>=PerKind)
        continue;
      if(consider(type,kind,object,point,targetAccess(w,npc,it.target,kind),travel()))
        ++allowedOf[kind];
      else if(closedOf[kind]<ClosedPerKind)
        ++closedOf[kind];
      else
        cands.pop_back();
      continue;
      }
    // Outdoors, own rooms, places: the nearest allowed one of each such kind this NPC may use.
    if(seen[kind])
      continue;
    if(!guildMay(w,npc,kind)) {
      seen[kind] = 1;
      continue;
      }
    const auto acc = targetAccess(w,npc,it.target,kind);
    if(acc==Access::Other)
      continue;
    consider(type,kind,object,point,acc,travel());
    if(cands.back().ok)
      seen[kind] = 1; else
      cands.pop_back();
    }
  size_t count = 1;
  const auto add = [&](const Cand& c) {
    if(count<K) {
      b.menu[at+count] = c.o;
      b.mask[at+count] = c.ok ? 1 : 0;
      ++count;
      }
    };
  // The leaders of the kinds that fill a need in want (low, critical, or forecast critical in the next anchor run)
  // go first: with more kinds than slots a far bed must not lose its place to a near chair.
  const size_t n = needCount(w);
  thread_local std::vector<double> under;
  under.resize(n);
  if(h==Hold::Free)
    forecast(sim,npc,under); else
    std::fill(under.begin(),under.end(),0.0);
  const auto wanted = [&](uint16_t state) {
    for(size_t k=0; k<n; ++k)
      if((s.needZone[size_t(npc)*n+k]>0 || under[k]<0) && fills(w,state,k))
        return true;
    return false;
    };
  thread_local std::vector<uint8_t> lead, taken;
  lead.assign(w.config.objectKinds.size(),0);
  taken.assign(cands.size(),0);
  for(int pass=0; pass<2; ++pass)
    for(size_t i=0; i<cands.size(); ++i)
      if(cands[i].ok && !lead[cands[i].o.kind] && (pass==1 || wanted(cands[i].o.state))) {
        lead[cands[i].o.kind] = 1;
        taken[i] = 1;
        add(cands[i]);
        }
  for(size_t i=0; i<cands.size(); ++i)
    if(cands[i].ok && !taken[i])
      add(cands[i]);
  for(size_t i=0; i<cands.size(); ++i)
    if(!cands[i].ok)
      add(cands[i]);
  b.count.push_back(uint8_t(count));
  }

}

// --- What the core calls ------------------------------------------------------------------------------------------

void sizeDecisions(Sim& sim) {
  auto& s = sim.s;
  const auto& w = *sim.world;
  const size_t n = w.npcs.size();
  s.mode.resize(n);
  s.want.resize(n);
  s.deferred.resize(n);
  s.actType.resize(n);
  s.actKind.resize(n);
  s.actPoint.resize(n);
  s.actObject.resize(n);
  s.actBegin.resize(n);
  s.actMin.resize(n);
  s.actEnd.resize(n);
  s.actFinish.resize(n);
  s.actSeq.resize(n);
  s.forecastFor.resize(n);
  s.routineTarget.resize(n);
  s.routineKind.resize(n);
  s.objectUse.resize(w.objects.size());
  s.pointUse.resize(w.points.size());
  s.asked.reserve(n);
  s.batch.stride = w.config.decisions.menu;
  }

void resetDecisions(Sim& sim) {
  auto& s = sim.s;
  std::fill(s.mode.begin(),s.mode.end(),uint8_t(Mode::Routine));
  std::fill(s.want.begin(),s.want.end(),0);
  std::fill(s.deferred.begin(),s.deferred.end(),0);
  std::fill(s.actType.begin(),s.actType.end(),0);
  std::fill(s.actKind.begin(),s.actKind.end(),NoKind);
  std::fill(s.actPoint.begin(),s.actPoint.end(),NoPoint);
  std::fill(s.actObject.begin(),s.actObject.end(),NoObject);
  std::fill(s.actBegin.begin(),s.actBegin.end(),-1);
  std::fill(s.actMin.begin(),s.actMin.end(),Never);
  std::fill(s.actEnd.begin(),s.actEnd.end(),Never);
  std::fill(s.actFinish.begin(),s.actFinish.end(),uint8_t(FinishReason::MaxTime));
  std::fill(s.actSeq.begin(),s.actSeq.end(),0);
  std::fill(s.forecastFor.begin(),s.forecastFor.end(),-1);
  std::fill(s.routineTarget.begin(),s.routineTarget.end(),NoObject);
  std::fill(s.routineKind.begin(),s.routineKind.end(),NoKind);
  std::fill(s.objectUse.begin(),s.objectUse.end(),0);
  std::fill(s.pointUse.begin(),s.pointUse.end(),0);
  s.asked.clear();
  s.batch.clear();
  s.decisions = 0;
  }

// A decision is asked now, or held until the chosen activity's least time is over (the walk to it included).
void ask(Sim& sim, uint32_t npc, uint8_t cause) {
  auto& s = sim.s;
  if(modeOf(s,npc)==Mode::Chosen && (s.actBegin[npc]<0 || s.now<s.actMin[npc])) {
    const bool first = s.deferred[npc]==0;
    s.deferred[npc] = uint8_t(s.deferred[npc] | cause);
    if(first && s.actBegin[npc]>=0)
      scheduleActivity(sim,npc);
    return;
    }
  if(s.want[npc]==0)
    s.asked.push_back(npc);
  s.want[npc] = uint8_t(s.want[npc] | cause);
  }

void startDecisions(Sim& sim, uint32_t npc) {
  const auto h = hold(sim,npc);
  if(h==Hold::Free)
    ask(sim,npc,CauseStart);
  else if(h==Hold::Anchor && critical(sim,npc))
    ask(sim,npc,CauseAnchorCritical);
  }

void beginEntry(Sim& sim, uint32_t npc) {
  auto& s = sim.s;
  const auto& w = *sim.world;
  const auto e = entryAt(w,npc,s.now);
  s.stateEntry[npc] = e;
  s.stateEnd[npc]   = stateEndAt(w,npc,e,s.now);
  push(s,s.stateEnd[npc]+StartDelay,npc,Pending::Begin);
  const auto h = hold(sim,npc);
  const bool yields = h==Hold::Free || (h==Hold::Anchor && critical(sim,npc));
  const uint8_t cause = h==Hold::Free ? CauseFreeEntry : CauseAnchorCritical;
  const auto mode = modeOf(s,npc);
  if(mode!=Mode::Routine) {
    if(yields) {
      ask(sim,npc,cause);
      return;
      }
    // A post or an anchor entry without a critical need takes the NPC back at once.
    if(mode==Mode::Chosen)
      stopChosen(sim,npc,FinishReason::Anchor);
    toRoutine(sim,npc,ActivityReason::Entry);
    return;
    }
  const bool walking = moving(s,npc);
  if(!yields || walking) {
    // As the routine alone: the state now (on the way the walk goes on and its arrival looks at the new point).
    setState(sim,npc,entryState(sim,npc,e),ActivityReason::Entry,walking ? NoPoint : s.place[npc]);
    if(!walking)
      follow(sim,npc);
    if(yields)
      ask(sim,npc,cause);
    return;
    }
  // Standing: the entry's state and its walk wait for the decision of this second.
  s.flags[npc]   = uint8_t(s.flags[npc] | PendingStart);
  s.bodySeq[npc] = 0;
  ask(sim,npc,cause);
  }

void onDown(Sim& sim, uint32_t npc, uint8_t zone) {
  const uint8_t cause = zone>=2 ? CauseNeedCritical : CauseNeedLow;
  if(modeOf(sim.s,npc)!=Mode::Routine) {
    ask(sim,npc,cause);
    return;
    }
  const auto h = hold(sim,npc);
  if(h==Hold::Free)
    ask(sim,npc,cause);
  else if(h==Hold::Anchor && zone>=2)
    ask(sim,npc,CauseAnchorCritical);
  }

void onSegment(Sim& sim, uint32_t npc) {
  auto& s = sim.s;
  if(modeOf(s,npc)!=Mode::Routine || (s.flags[npc] & PendingStart) || s.want[npc]!=0 || s.entry[npc]==NoEntry ||
     hold(sim,npc)!=Hold::Free)
    return;
  thread_local std::vector<double> under;
  under.resize(needCount(*sim.world));
  const Time run = forecast(sim,npc,under);
  if(run<0 || s.forecastFor[npc]==run)
    return;
  s.forecastFor[npc] = run;
  ask(sim,npc,CauseForecast);
  }

// A chosen activity at `target` gives way to the place's routine user: on the way to it it failed, at it it finished
// (Evicted); its NPC is asked again at this second, with the causes it held. False when no chosen activity holds the
// place. The first such NPC by index: evictions are rare, a scan of the NPCs is cheaper than an index kept up.
bool evict(Sim& sim, uint32_t target) {
  auto& s = sim.s;
  for(uint32_t j=0; j<sim.world->npcs.size(); ++j) {
    if(modeOf(s,j)!=Mode::Chosen || targetOf(OptionType(s.actType[j]),s.actObject[j],s.actPoint[j])!=target)
      continue;
    const uint8_t held = s.deferred[j];
    if(s.actBegin[j]<0)
      emitOption(sim,EventKind::ActionFailed,j,uint8_t(FailReason::Evicted)); else
      emitOption(sim,EventKind::ActivityFinished,j,uint8_t(FinishReason::Evicted));
    release(sim,j);
    s.mode[j] = uint8_t(Mode::Finished);
    ask(sim,j,uint8_t(CauseFinished | held));
    return true;
    }
  return false;
  }

void routinePlace(Sim& sim, uint32_t npc) {
  auto& s = sim.s;
  const auto& w = *sim.world;
  uint32_t want = NoObject;
  uint16_t wantKind = NoKind;
  const uint16_t state = s.state[npc];
  // Only at the point its routine entry wants it at (within NearPointCm it does the entry where it stands): an NPC
  // passing a place on its way does not take it.
  const uint32_t entry = entryPoint(sim,npc,s.stateEntry[npc]);
  const bool atEntry = entry==NoPoint || entry==s.place[npc] || near(w,entry,s.place[npc]);
  if(modeOf(s,npc)==Mode::Routine && !moving(s,npc) && state!=NoState && atEntry && !(s.flags[npc] & PendingStart)) {
    const uint16_t next = w.followState[state];
    const auto fits = [&](uint16_t kind) { return w.kindState[kind]==state || w.kindState[kind]==next; };
    // The places of the entry's point, then of where it stands (World::spots, nearest first).
    const uint32_t refs[2] = {entry, entry==s.place[npc] ? NoPoint : s.place[npc]};
    const auto spotOf = [&](uint32_t r, uint32_t t, uint16_t kind) {
      for(uint32_t k=r==NoPoint ? 0 : w.spotBegin[r]; r!=NoPoint && k<w.spotBegin[r+1]; ++k)
        if(w.spots[k]==t && w.spotKind[k]==kind)
          return true;
      return false;
      };
    // The place it holds stays its own while it does that kind's state there.
    const uint32_t mine = s.routineTarget[npc];
    if(mine!=NoObject && fits(s.routineKind[npc]) &&
       (spotOf(refs[0],mine,s.routineKind[npc]) || spotOf(refs[1],mine,s.routineKind[npc])))
      return;
    // The nearest place of a kind whose state it does (or passes to) with room.
    thread_local std::vector<std::pair<uint32_t,uint16_t>> full;
    full.clear();
    for(const uint32_t r:refs) {
      if(r==NoPoint || want!=NoObject)
        continue;
      for(uint32_t k=w.spotBegin[r]; k<w.spotBegin[r+1] && want==NoObject; ++k) {
        const uint16_t kind = w.spotKind[k];
        if(!fits(kind))
          continue;
        const uint32_t t  = w.spots[k];
        const uint32_t id = targetId(t);
        const uint16_t used = targetIsPoint(t) ? s.pointUse[id] : s.objectUse[id];
        if(used<w.config.objectKinds.capacity[kind]) {
          want     = t;
          wantKind = kind;
          } else {
          full.emplace_back(t,kind);
          }
        }
      }
    // None: the routine's user comes first, a chosen activity at the nearest full place that has one gives way.
    for(size_t i=0; want==NoObject && i<full.size(); ++i)
      if(evict(sim,full[i].first)) {
        want     = full[i].first;
        wantKind = full[i].second;
        }
    }
  if(want==s.routineTarget[npc] && wantKind==s.routineKind[npc])
    return;
  releaseRoutine(sim,npc);
  if(want==NoObject)
    return;
  const uint32_t id = targetId(want);
  ++(targetIsPoint(want) ? s.pointUse[id] : s.objectUse[id]);
  s.routineTarget[npc] = want;
  s.routineKind[npc]   = wantKind;
  emit(sim,EventKind::PlaceHeld,npc,0,wantKind,targetIsPoint(want) ? id : w.objects.waypoint[id],
       targetIsPoint(want) ? NoObject : id);
  }

void arrivedChosen(Sim& sim, uint32_t npc) {
  auto& s = sim.s;
  s.bodySeq[npc]  = 0;
  s.actBegin[npc] = s.now;
  thread_local std::vector<double> levels;
  levelsNow(sim,npc,levels);
  bool full = false;
  // In an anchor entry the goal is out of danger, not the setpoint: its end says so (FinishReason::AboveLow).
  const bool anchored = hold(sim,npc)==Hold::Anchor;
  const Time dur = duration(sim,npc,s.actKind[npc],s.state[npc],levels,anchored,full);
  const Time longest = Time(sim.world->config.objectKinds.minutes[s.actKind[npc]])*60;
  s.actEnd[npc]    = s.now+dur;
  s.actFinish[npc] = uint8_t(!full ? FinishReason::MaxTime : anchored ? FinishReason::AboveLow : FinishReason::Setpoint);
  s.actMin[npc]  = s.now+std::min(dec(sim).minDwell,longest);
  scheduleActivity(sim,npc);
  }

void onActivity(Sim& sim, uint32_t npc) {
  auto& s = sim.s;
  s.actSeq[npc] = 0;
  if(modeOf(s,npc)!=Mode::Chosen || s.actBegin[npc]<0)
    return;
  if(s.now>=s.actEnd[npc]) {
    const uint8_t held = s.deferred[npc];
    emitOption(sim,EventKind::ActivityFinished,npc,s.actFinish[npc]);
    release(sim,npc);
    s.mode[npc] = uint8_t(Mode::Finished);
    ask(sim,npc,uint8_t(CauseFinished | held));
    return;
    }
  if(s.deferred[npc]!=0 && s.now>=s.actMin[npc]) {
    const uint8_t held = s.deferred[npc];
    s.deferred[npc] = 0;
    ask(sim,npc,held);
    }
  scheduleActivity(sim,npc);
  }

size_t buildBatch(Sim& sim, DecisionBatch& out) {
  auto& s = sim.s;
  auto& b = s.batch;
  b.clear();
  b.stride = dec(sim).menu;
  thread_local std::vector<uint32_t> asked;
  asked.swap(s.asked);
  s.asked.clear();
  std::sort(asked.begin(),asked.end());
  for(auto npc:asked) {
    const uint8_t cause = s.want[npc];
    s.want[npc] = 0;
    if(cause==0)
      continue;
    // A guard whose post is in force does not decide, whatever it did (a chosen activity may end in the seconds
    // between the post's start and its state's): back to the routine.
    if(hold(sim,npc)==Hold::Post) {
      routine(sim,npc);
      continue;
      }
    b.npc.push_back(npc);
    b.cause.push_back(cause);
    buildRow(sim,npc,b);
    // A decision in a free entry already sees the forecast of the next anchor run: no second ask for it.
    if(hold(sim,npc)==Hold::Free) {
      thread_local std::vector<double> under;
      under.resize(needCount(*sim.world));
      if(const Time run = forecast(sim,npc,under); run>=0)
        s.forecastFor[npc] = run;
      }
    const size_t row = b.npc.size()-1, K = b.stride;
    uint32_t allowed = 0;
    for(size_t j=0; j<b.count[row]; ++j)
      allowed += b.mask[row*K+j];
    emit(sim,EventKind::DecisionRequested,npc,cause,b.count[row],allowed);
    ++s.decisions;
    if(allowed>1)
      continue;
    // Only the routine is allowed: the core answers itself.
    b.npc.pop_back();
    b.cause.pop_back();
    b.count.pop_back();
    b.menu.resize(row*K);
    b.mask.resize(row*K);
    emit(sim,EventKind::ActionChosen,npc,uint8_t(Chooser::Routine),NoKind,NoPoint,NoObject);
    routine(sim,npc);
    }
  asked.clear();
  out = b;
  return out.size();
  }

void applyChoices(Sim& sim, std::span<const Choice> choices) {
  auto& s = sim.s;
  auto& b = s.batch;
  const size_t K = b.stride;
  if(choices.size()!=b.size())
    throw std::logic_error("society core: "+std::to_string(choices.size())+" choices for a batch of "+
                           std::to_string(b.size())+" rows");
  for(size_t r=0; r<choices.size(); ++r) {
    const auto& c = choices[r];
    if(c.npc!=b.npc[r])
      throw std::logic_error("society core: choice "+std::to_string(r)+" is for another NPC than its row");
    if(c.option>=b.count[r] || b.mask[r*K+c.option]==0)
      throw std::logic_error("society core: the choice of "+sim.world->npcs.key[c.npc]+", option "+
                             std::to_string(c.option)+", is out of its mask");
    }
  const auto rows = std::move(b);
  b = DecisionBatch();
  b.stride = K;
  for(size_t r=0; r<choices.size(); ++r) {
    const uint32_t npc = rows.npc[r];
    const auto& o = rows.menu[r*K+choices[r].option];
    emit(sim,EventKind::ActionChosen,npc,uint8_t(choices[r].by),o.kind,o.point,o.object);
    if(o.type==OptionType::Routine) {
      routine(sim,npc);
      continue;
      }
    const auto mode = modeOf(s,npc);
    const bool same = mode==Mode::Chosen && OptionType(s.actType[npc])==o.type && s.actKind[npc]==o.kind &&
                      s.actPoint[npc]==o.point && s.actObject[npc]==o.object;
    if(same)
      continue;
    // A place taken since the menu was made (by an earlier row of this batch): a chosen activity goes on; otherwise
    // the NPC is asked again at this second, with the menu as it is now.
    const uint32_t own = s.routineTarget[npc]==targetOf(o.type,o.object,o.point) ? 1u : 0u;
    if(uint32_t(holders(s,o.type,o.object,o.point))-own>=sim.world->config.objectKinds.capacity[o.kind]) {
      emit(sim,EventKind::ActionFailed,npc,uint8_t(FailReason::Taken),o.kind,o.point,o.object);
      if(mode!=Mode::Chosen)
        ask(sim,npc,rows.cause[r]);
      continue;
      }
    if(mode==Mode::Chosen)
      stopChosen(sim,npc,FinishReason::Switched);
    startChosen(sim,npc,o);
    }
  }

}

// --- Policies -----------------------------------------------------------------------------------------------------

double drive(const World& w, std::span<const double> h) {
  const auto& d = w.config.decisions;
  double sum = 0;
  for(size_t k=0; k<h.size(); ++k) {
    const double x = std::abs(1.0-h[k]);
    double p = 0;
    if(d.n==4) {
      const double x2 = x*x;
      p = x2*x2;
      } else {
      p = std::pow(x,d.n);
      }
    const double term = d.weight[k]*p;
    sum += term;
    }
  return d.m==2 ? std::sqrt(sum) : std::pow(sum,1.0/d.m);
  }

namespace {

// Levels after `dt` game seconds at the rates of `state` (standing) or walking.
void run(const World& w, std::span<const double> decay, uint16_t state, bool walking, Time dt, std::vector<double>& h) {
  if(dt<=0)
    return;
  for(size_t k=0; k<h.size(); ++k)
    h[k] = needLevelAt(h[k],needRate(w,k,decay[k],state,walking),0,dt);
  }

}

void teacherScores(const Sim& sim, const DecisionBatch& b, std::span<float> scores) {
  const auto& w = *sim.world;
  const auto& s = sim.s;
  const auto& d = w.config.decisions;
  const size_t K = b.stride, n = needCount(w);
  if(scores.size()<b.size()*K)
    throw std::invalid_argument("society core: the teacher's scores need [rows × K] slots");
  thread_local std::vector<double> now, h, decay, under, seen;
  for(size_t r=0; r<b.size(); ++r) {
    const uint32_t npc = b.npc[r];
    now.resize(n);
    decay.resize(n);
    for(size_t k=0; k<n; ++k) {
      now[k]   = needLevel(sim,npc,k);
      decay[k] = s.needDecay[size_t(npc)*n+k];
      }
    // Foresight: in a free entry, a need that would be critical in the next anchor run counts at its forecast low,
    // so what fills it now is worth what it saves there.
    const auto held = Society::detail::hold(sim,npc);
    under.resize(n);
    if(held==Society::detail::Hold::Free)
      Society::detail::forecast(sim,npc,under); else
      std::fill(under.begin(),under.end(),0.0);
    const auto ahead = [&](std::vector<double>& x) {
      for(size_t k=0; k<n; ++k)
        x[k] = std::clamp(x[k]+under[k],0.0,1.0);
      return drive(w,x);
      };
    const auto mode = Mode(s.mode[npc]);
    const bool anchored = held==Society::detail::Hold::Anchor;
    // The routine's state as it is done at its point: into bed is asleep there (FollowStates).
    const auto& r0 = b.menu[r*K];
    uint16_t routineState = r0.state;
    if(routineState!=NoState && w.followState[routineState]!=NoState)
      routineState = w.followState[routineState];
    // The drive after `span` seconds of the routine: its walk, then its state.
    const auto routineOver = [&](Time span) {
      seen = now;
      const Time walkT = std::min(r0.travel,span);
      run(w,decay,NoState,true,walkT,seen);
      run(w,decay,routineState,false,span-walkT,seen);
      return ahead(seen);
      };
    const double base = routineOver(d.horizon);
    for(size_t j=0; j<K; ++j) {
      auto& out = scores[r*K+j];
      out = -std::numeric_limits<float>::infinity();
      if(j>=b.count[r] || !b.mask[r*K+j])
        continue;
      const auto& o = b.menu[r*K+j];
      if(o.type==OptionType::Routine) {
        out = float(mode!=Mode::Chosen ? d.inertia : 0.0);
        continue;
        }
      // The walk, the activity, then the routine, over the window — at least the walk and an hour of the activity, so
      // a far place gets its due — against the routine over the same window.
      h = now;
      run(w,decay,NoState,true,o.travel,h);
      bool full = false;
      const Time dur  = Society::detail::duration(sim,npc,o.kind,o.state,h,anchored,full);
      const Time span = std::max(d.horizon,o.travel+std::min(dur,d.horizon));
      const Time act  = std::min(dur,span-o.travel);
      run(w,decay,o.state,false,act,h);
      run(w,decay,routineState,false,span-o.travel-act,h);
      const bool current = mode==Mode::Chosen && OptionType(s.actType[npc])==o.type && s.actKind[npc]==o.kind &&
                           s.actPoint[npc]==o.point && s.actObject[npc]==o.object;
      double score = (span==d.horizon ? base : routineOver(span))-ahead(h);
      score -= d.travelCost*double(o.travel);
      if(current)
        score += d.inertia;
      out = float(score);
      }
    }
  }

void teacher(Sim& sim, const DecisionBatch& b, std::vector<Choice>& out) {
  const auto& d = sim.world->config.decisions;
  const size_t K = b.stride;
  thread_local std::vector<float> scores;
  scores.assign(b.size()*K,0.f);
  teacherScores(sim,b,scores);
  out.clear();
  thread_local std::vector<uint16_t> order;
  for(size_t r=0; r<b.size(); ++r) {
    order.clear();
    for(uint16_t j=0; j<b.count[r]; ++j)
      if(b.mask[r*K+j])
        order.push_back(j);
    const float* sc = scores.data()+r*K;
    std::stable_sort(order.begin(),order.end(),[&](uint16_t x, uint16_t y) { return sc[x]>sc[y]; });
    const size_t top = std::min<size_t>(d.top,order.size());
    uint16_t pick = order[0];
    if(d.temperature>0 && top>1) {
      double weight[MenuSize] = {}, sum = 0;
      for(size_t i=0; i<top; ++i) {
        weight[i] = std::exp((double(sc[order[i]])-double(sc[order[0]]))/d.temperature);
        sum += weight[i];
        }
      double u = sim.s.rng.unit()*sum;
      for(size_t i=0; i<top; ++i) {
        pick = order[i];
        if(u<weight[i])
          break;
        u -= weight[i];
        }
      }
    out.push_back({b.npc[r],pick,Chooser::Teacher});
    }
  }

void randomPolicy(Sim& sim, const DecisionBatch& b, std::vector<Choice>& out) {
  const size_t K = b.stride;
  out.clear();
  for(size_t r=0; r<b.size(); ++r) {
    uint16_t allowed = 0;
    for(size_t j=0; j<b.count[r]; ++j)
      allowed = uint16_t(allowed+b.mask[r*K+j]);
    auto k = uint16_t(sim.s.rng.unit()*double(allowed));
    uint16_t pick = 0;
    for(uint16_t j=0; j<b.count[r]; ++j)
      if(b.mask[r*K+j] && k--==0) {
        pick = j;
        break;
        }
    out.push_back({b.npc[r],pick,Chooser::Random});
    }
  }

}
