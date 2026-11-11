// What the decisions section gives a world at load (docs/society-sim.md, «Решения»): the places of options by waynet
// point with their kinds, who may use them, and which needs of each person are in use — a need is, only when a way to
// fill it is within the travel limit of a point of the person's routine. The needs without a way are listed with the
// nearest way beyond the limit: content gaps, not a lower threshold.
#include "core.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace Society {
namespace detail {

namespace {

// Dijkstra over the waynet from `from`, nearest first, up to `reach` cm; `visit(point, cm)` returns false to stop.
template<class Visit>
void walkFrom(const World& w, uint32_t from, int64_t reach, std::vector<int64_t>& dist, std::vector<uint32_t>& stamp,
              uint32_t& round, Visit visit) {
  if(++round==0) {
    std::fill(stamp.begin(),stamp.end(),0u);
    round = 1;
    }
  const auto later = [](const std::pair<int64_t,uint32_t>& x, const std::pair<int64_t,uint32_t>& y) { return x>y; };
  std::vector<std::pair<int64_t,uint32_t>> heap;
  dist[from]  = 0;
  stamp[from] = round;
  heap.emplace_back(0,from);
  while(!heap.empty()) {
    std::pop_heap(heap.begin(),heap.end(),later);
    const auto [d,u] = heap.back();
    heap.pop_back();
    if(d!=dist[u])
      continue;
    if(!visit(u,d))
      return;
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
  }

void buildTargets(World& w, const std::unordered_map<std::string,std::string>& placeOf) {
  const auto& o = w.config.objectKinds;
  const size_t points = w.points.size();
  std::vector<std::vector<std::pair<uint32_t,uint16_t>>> at(points);
  for(uint32_t i=0; i<w.objects.size(); ++i)
    if(w.objectKind[i]!=NoKind)
      at[w.objects.waypoint[i]].emplace_back(i,w.objectKind[i]);
  std::vector<uint16_t> outdoors, ownRooms, places;
  for(uint16_t k=0; k<o.size(); ++k) {
    if(o.at[k]==uint8_t(KindAt::Outdoors))
      outdoors.push_back(k);
    if(o.at[k]==uint8_t(KindAt::OwnRooms))
      ownRooms.push_back(k);
    if(o.at[k]==uint8_t(KindAt::Places))
      places.push_back(k);
    }
  for(uint32_t i=0; i<points; ++i) {
    const uint32_t t = i | TargetPoint;
    const uint32_t g = w.points.graph[i];
    if(w.points.free[i]) {
      // Every kind whose prefix matches with the longest match of all.
      size_t best = 0;
      for(const auto& p:o.prefix)
        if(p.size()>best && w.points.name[i].rfind(p,0)==0)
          best = p.size();
      std::vector<uint16_t> kinds;
      for(size_t j=0; j<o.prefix.size() && best>0; ++j)
        if(o.prefix[j].size()==best && w.points.name[i].rfind(o.prefix[j],0)==0 &&
           std::find(kinds.begin(),kinds.end(),o.prefixKind[j])==kinds.end())
          kinds.push_back(o.prefixKind[j]);
      std::sort(kinds.begin(),kinds.end());
      for(auto k:kinds)
        at[g].emplace_back(t,k);
      }
    if(w.pointOutdoors[i])
      for(auto k:outdoors)
        at[i].emplace_back(t,k);
    if(w.pointOwnerNpc[i]!=NoNpc || w.pointOwnerGuild[i]!=-1)
      for(auto k:ownRooms)
        at[g].emplace_back(t,k);
    if(!places.empty())
      if(const auto it = placeOf.find(w.points.name[i]); it!=placeOf.end())
        for(auto k:places)
          if(std::find(o.places[k].begin(),o.places[k].end(),it->second)!=o.places[k].end())
            at[g].emplace_back(t,k);
    }
  w.targetBegin.assign(1,0);
  w.targets.clear();
  w.targetKind.clear();
  for(const auto& list:at) {
    for(const auto& [t,k]:list) {
      w.targets.push_back(t);
      w.targetKind.push_back(k);
      }
    w.targetBegin.push_back(uint32_t(w.targets.size()));
    }
  }

// The longest walk in cm whose time is within `limit` for this walk speed (travelSeconds rounds to the second).
int64_t reachCm(const World& w, double walk, Time limit) {
  int64_t ret = int64_t(std::floor((double(limit)+0.5)*walk/w.clockRate))+2;
  while(ret>0 && travelSeconds(ret,walk,w.clockRate)>limit)
    --ret;
  return ret;
  }

// Which needs of NPC i are in use, and for those that are not the nearest way beyond the limit.
void needsInUse(World& w, uint32_t i, std::vector<int64_t>& dist, std::vector<uint32_t>& stamp, uint32_t& round) {
  const size_t n = needCount(w);
  auto* active = w.needActive.data()+size_t(i)*n;
  std::vector<uint8_t> open(n,0);
  size_t left = 0;
  for(size_t k=0; k<n; ++k) {
    active[k] = 1;
    if(w.npcDecay[size_t(i)*n+k]>0) {
      active[k] = 0;
      open[k] = 1;
      ++left;
      }
    }
  const auto fillsOpen = [&](uint16_t state) {
    for(size_t k=0; k<n; ++k)
      if(open[k] && fills(w,state,k)) {
        open[k] = 0;
        active[k] = 1;
        --left;
        }
    };
  // The routine's own states (into bed is asleep there); without a routine its start_aistate and schedule.
  std::vector<uint32_t> origins;
  for(uint32_t e=w.npcs.routineBegin[i]; e<w.npcs.routineBegin[i+1]; ++e) {
    const auto state = w.routines.state[e];
    fillsOpen(state);
    if(w.followState[state]!=NoState)
      fillsOpen(w.followState[state]);
    if(w.routines.point[e]!=NoPoint)
      origins.push_back(w.routines.point[e]);
    }
  if(w.entryCount(i)==0) {
    if(w.npcs.startState[i]!=NoState)
      fillsOpen(w.npcs.startState[i]);
    for(uint32_t k=w.npcs.scheduleBegin[i]; k<w.npcs.scheduleBegin[i+1]; ++k)
      fillsOpen(w.schedules.state[k]);
    }
  if(origins.empty())
    origins.push_back(w.npcs.startPoint[i]);
  std::sort(origins.begin(),origins.end());
  origins.erase(std::unique(origins.begin(),origins.end()),origins.end());
  const double walk  = w.npcs.walkCmPerS[i];
  const Time   limit = w.config.decisions.travelLimit;
  const int64_t reach = reachCm(w,walk,limit);
  // A target the NPC may use for an open need: the walk from origin `o` in cm, -1 when none qualifies.
  const auto scan = [&](uint32_t o, uint32_t u, int64_t d, int64_t maxCm, auto&& found) {
    for(uint32_t k=w.targetBegin[u]; k<w.targetBegin[u+1]; ++k) {
      const uint32_t t = w.targets[k];
      const uint16_t kind = w.targetKind[k];
      const uint32_t id = targetId(t);
      const uint32_t point = targetIsPoint(t) ? id : w.objects.waypoint[id];
      const int64_t cm = point==o ? 0 : w.points.graphCm[o]+d+(targetIsPoint(t) ? w.points.graphCm[id] : 0);
      if(cm>maxCm || !guildMay(w,i,kind) || targetAccess(w,i,t,kind)==Access::Other)
        continue;
      for(size_t j=0; j<n; ++j)
        if(open[j] && fills(w,w.kindState[kind],j))
          found(j,kind,point,targetIsPoint(t) ? NoObject : id,cm);
      }
    };
  for(auto o:origins) {
    if(left==0)
      break;
    walkFrom(w,w.points.graph[o],reach,dist,stamp,round,[&](uint32_t u, int64_t d) {
      scan(o,u,d,reach,[&](size_t j, uint16_t, uint32_t, uint32_t, int64_t) {
        if(open[j]) {
          open[j] = 0;
          active[j] = 1;
          --left;
          }
        });
      return left>0;
      });
    }
  if(left==0)
    return;
  // Not in use: the nearest way anywhere in the component, from the nearest point of the routine.
  std::vector<World::Unmet> best(n);
  for(size_t k=0; k<n; ++k) {
    best[k].npc  = i;
    best[k].need = uint16_t(k);
    }
  for(auto o:origins)
    walkFrom(w,w.points.graph[o],std::numeric_limits<int64_t>::max()/4,dist,stamp,round,[&](uint32_t u, int64_t d) {
      scan(o,u,d,std::numeric_limits<int64_t>::max(),[&](size_t j, uint16_t kind, uint32_t point, uint32_t object, int64_t cm) {
        const Time t = travelSeconds(cm,walk,w.clockRate);
        auto& b = best[j];
        if(b.travel<0 || t<b.travel) {
          b.kind   = kind;
          b.point  = point;
          b.object = object;
          b.travel = t;
          }
        });
      return true;
      });
  for(size_t k=0; k<n; ++k)
    if(open[k]) {
      w.unmet.push_back(best[k]);
      w.npcDecay[size_t(i)*n+k] = 0;
      }
  }

}

bool fills(const World& w, uint16_t state, size_t need) {
  const auto row = state==NoState ? NoState : w.stateRow[state];
  return row!=NoState && w.config.states.fill[size_t(row)*needCount(w)+need]>0;
  }

bool guildMay(const World& w, uint32_t npc, uint16_t kind) {
  const auto& g = w.config.objectKinds.guilds[kind];
  return g.empty() || std::binary_search(g.begin(),g.end(),w.npcs.guild[npc]);
  }

Access access(const World& w, uint32_t npc, uint32_t ownerNpc, int32_t ownerGuild) {
  if(ownerNpc==NoNpc && ownerGuild==-1)
    return Access::Public;
  if(ownerNpc==npc)
    return Access::Own;
  if(ownerGuild>=0 && uint32_t(ownerGuild)==w.npcs.guild[npc])
    return Access::Guild;
  return Access::Other;
  }

Access targetAccess(const World& w, uint32_t npc, uint32_t target, uint16_t kind) {
  const auto at = KindAt(w.config.objectKinds.at[kind]);
  if(at==KindAt::Outdoors || at==KindAt::Places)
    return Access::Public;
  const uint32_t id = targetId(target);
  const auto acc = targetIsPoint(target) ? access(w,npc,w.pointOwnerNpc[id],w.pointOwnerGuild[id])
                                         : access(w,npc,w.objectOwnerNpc[id],w.objectOwnerGuild[id]);
  if(at==KindAt::OwnRooms)
    return acc==Access::Own || acc==Access::Guild ? acc : Access::Other;
  return acc;
  }

void applyReach(World& w, const std::unordered_map<std::string,std::string>& placeOf) {
  buildTargets(w,placeOf);
  const size_t n = needCount(w), npcs = w.npcs.size();
  w.needActive.assign(npcs*n,0);
  w.unmet.clear();
  std::vector<int64_t>  dist(w.points.size(),0);
  std::vector<uint32_t> stamp(w.points.size(),0);
  uint32_t round = 0;
  for(uint32_t i=0; i<npcs; ++i)
    if(w.npcNeeds[i])
      needsInUse(w,i,dist,stamp,round);
  }

}
}
