// Observation tensors shared by the society simulator and its policies.
#include "core.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>

namespace Society {
namespace {

constexpr std::array OwnNames = {
  "time.sin", "time.cos", "moving", "activity.chosen", "activity.elapsed_minutes",
  "activity.remaining_minutes", "routine.anchor", "routine.time_limited", "routine.remaining_minutes",
  "place.outdoors", "place.freepoint", "place.object"
  };

constexpr std::array NeedNames = {"level", "active", "decay_per_hour", "rate_per_hour"};

constexpr std::array OptionNames = {
  "type.routine", "type.object", "type.point", "travel_minutes", "return_minutes", "return_beyond_reach",
  "access.public", "access.own", "access.guild", "access.other", "capacity", "holders", "free_slots",
  "duration_minutes", "current"
  };

float minutes(Time seconds) {
  return float(double(seconds)/60.0);
  }

float perHour(double perSecond) {
  return float(perSecond*3600.0);
  }

size_t ownWidth(const World& w) {
  return OwnNames.size()+needCount(w)*NeedNames.size()+w.config.scales.size()+w.config.objectKinds.size();
  }

size_t optionWidth(const World& w) {
  return OptionNames.size()+needCount(w)+w.config.objectKinds.size();
  }

Time routineRemaining(const Sim& sim, uint32_t npc, bool& limited) {
  Time next = 0;
  entryAt(*sim.world,npc,sim.s.now,&next);
  limited = next!=std::numeric_limits<Time>::max();
  return limited ? next-sim.s.now : 0;
  }

bool anchored(const Sim& sim, uint32_t npc) {
  const auto& w = *sim.world;
  const auto state = detail::entryState(sim,npc,sim.s.entry[npc]);
  const auto row = state==NoState ? NoState : w.stateRow[state];
  return row!=NoState && w.config.states.kind[row]==uint8_t(StateKind::Anchor);
  }

void encodeOwn(const Sim& sim, uint32_t npc, Time routineLeft, bool routineLimited,
               float timeSin, float timeCos, float* out) {
  const auto& w = *sim.world;
  const auto& c = w.config;
  const auto& s = sim.s;
  const Mode mode = Mode(s.mode[npc]);
  const bool chosen = mode==Mode::Chosen;
  const bool moving = detail::moving(s,npc);
  const bool activityStarted = chosen && s.actBegin[npc]>=0;
  const uint16_t kind = chosen ? s.actKind[npc] : s.routineKind[npc];
  const Time elapsed = activityStarted ? s.now-s.actBegin[npc]
                     : mode==Mode::Routine && s.state[npc]!=NoState ? s.now-s.stateStart[npc] : 0;
  const Time left = activityStarted ? std::max(Time(0),s.actEnd[npc]-s.now)
                  : chosen ? Time(c.objectKinds.minutes[kind])*60 : 0;
  const uint32_t point = moving ? NoPoint : s.place[npc];
  const bool atObject = activityStarted ? s.actObject[npc]!=NoObject
                      : s.routineTarget[npc]!=NoObject && !targetIsPoint(s.routineTarget[npc]);
  const std::array fixed = {
    timeSin, timeCos, float(moving), float(chosen), minutes(elapsed), minutes(left),
    float(anchored(sim,npc)), float(routineLimited), minutes(routineLeft),
    float(point!=NoPoint && w.pointOutdoors[point]!=0), float(point!=NoPoint && w.points.free[point]!=0),
    float(!moving && atObject)
    };
  static_assert(fixed.size()==OwnNames.size());
  out = std::copy(fixed.begin(),fixed.end(),out);
  const size_t needs = needCount(w);
  for(size_t k=0; k<needs; ++k) {
    const size_t i = size_t(npc)*needs+k;
    const std::array values = {float(needLevel(sim,npc,k)), float(w.needActive.empty() || w.needActive[i]!=0),
                               perHour(s.needDecay[i]), perHour(s.needRate[i])};
    static_assert(values.size()==NeedNames.size());
    out = std::copy(values.begin(),values.end(),out);
    }
  const size_t traits = c.scales.size();
  for(size_t k=0; k<traits; ++k)
    *out++ = float(int(w.npcScale[size_t(npc)*traits+k])-c.middle)*0.5f;
  if(kind!=NoKind)
    out[kind] = 1.0f;
  }

void encodeOption(const Sim& sim, uint32_t npc, const Option& o, Time routineLeft, float* out) {
  const auto& w = *sim.world;
  const auto& s = sim.s;
  const auto& kinds = w.config.objectKinds;
  const bool routine = o.type==OptionType::Routine;
  const bool chosen = Mode(s.mode[npc])==Mode::Chosen;
  const bool current = routine ? !chosen
                     : chosen && OptionType(s.actType[npc])==o.type && s.actKind[npc]==o.kind &&
                       s.actPoint[npc]==o.point && s.actObject[npc]==o.object;
  const bool farBack = o.back==FarBack;
  const uint32_t capacity = routine ? 0 : kinds.capacity[o.kind];
  const uint32_t free = capacity>o.use ? capacity-o.use : 0;
  const float duration = routine ? minutes(routineLeft)
                       : current && s.actBegin[npc]>=0 ? minutes(std::max(Time(0),s.actEnd[npc]-s.now))
                       : float(kinds.minutes[o.kind]);
  const std::array fixed = {
    float(routine), float(o.type==OptionType::Object), float(o.type==OptionType::Point),
    minutes(o.travel), farBack ? 0.0f : minutes(o.back), float(farBack),
    float(o.access==Access::Public), float(o.access==Access::Own),
    float(o.access==Access::Guild), float(o.access==Access::Other),
    float(capacity), float(o.use), float(free), duration, float(current)
    };
  static_assert(fixed.size()==OptionNames.size());
  out = std::copy(fixed.begin(),fixed.end(),out);
  // The end state of the routine is what it does at its point (going to bed becomes sleep).
  uint16_t state = o.state;
  if(routine && state!=NoState && w.followState[state]!=NoState)
    state = w.followState[state];
  const uint16_t stateRow = state==NoState ? NoState : w.stateRow[state];
  const size_t needs = needCount(w);
  for(size_t k=0; k<needs; ++k)
    *out++ = stateRow==NoState ? 0.0f : perHour(w.config.states.fill[size_t(stateRow)*needs+k]);
  if(o.kind!=NoKind)
    out[o.kind] = 1.0f;
  }

}

FeatureLayout featureLayout(const World& w) {
  FeatureLayout layout;
  layout.stride = w.config.decisions.menu;
  layout.own.reserve(ownWidth(w));
  layout.options.reserve(optionWidth(w));
  layout.own.assign(OwnNames.begin(),OwnNames.end());
  layout.options.assign(OptionNames.begin(),OptionNames.end());
  for(const auto& id:w.config.needs.id) {
    for(const auto suffix:NeedNames)
      layout.own.push_back("need."+id+"."+suffix);
    layout.options.push_back("fill."+id+".per_hour");
    }
  for(const auto& scale:w.config.scales)
    layout.own.push_back("trait."+scale);
  for(const auto& id:w.config.objectKinds.id) {
    layout.own.push_back("activity.kind."+id);
    layout.options.push_back("kind."+id);
    }
  return layout;
  }

void encode(const Sim& sim, DecisionBatch& b) {
  const auto& w = *sim.world;
  const size_t F = ownWidth(w), C = optionWidth(w);
  b.own.assign(b.size()*F,0.0f);
  b.options.assign(b.size()*b.stride*C,0.0f);
  if(b.size()==0)
    return;
  const double angle = double(sim.s.now%DaySeconds)*(2.0*std::numbers::pi/double(DaySeconds));
  const float timeSin = float(std::sin(angle)), timeCos = float(std::cos(angle));
  for(size_t r=0; r<b.size(); ++r) {
    const uint32_t npc = b.npc[r];
    bool routineLimited = false;
    const Time routineLeft = routineRemaining(sim,npc,routineLimited);
    encodeOwn(sim,npc,routineLeft,routineLimited,timeSin,timeCos,b.own.data()+r*F);
    const size_t row = r*b.stride;
    for(size_t j=0; j<b.count[r]; ++j)
      encodeOption(sim,npc,b.menu[row+j],routineLeft,b.options.data()+(row+j)*C);
    }
  }

}
