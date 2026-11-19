#include "activityevents.h"

#include "utils/jsonescape.h"

#include <array>

namespace {
constexpr std::array<std::string_view,ActivityEvents::WakeCount> wakeNames = {
  "entered","loaded","queue","attached","attach_failed","detached","freed","row_end","interrupted","watchdog",
  };
constexpr std::array<std::string_view,3> phaseNames = {"init","loop","end"};
}

std::string_view ActivityEvents::wakeName(size_t bit) {
  return bit<wakeNames.size() ? wakeNames[bit] : std::string_view("?");
  }

ActivityEvents::Stats& ActivityEvents::stats() {
  static Stats value;
  return value;
  }

void ActivityEvents::reset() {
  stats() = Stats();
  }

void ActivityEvents::countWakes(std::string_view state, uint16_t wakes, bool step) {
  auto& st = stats();
  auto  it = st.states.find(state);
  if(it==st.states.end())
    it = st.states.emplace(std::string(state),StateStats()).first;
  for(size_t i=0; i<WakeCount; ++i)
    if(wakes & (1u<<i))
      ++it->second.wakes[i];
  if(step)
    ++it->second.steps;
  if(wakes & Watchdog)
    ++st.watchdog;
  }

void ActivityEvents::countPhase(std::string_view state, size_t phase) {
  auto& st = stats();
  auto  it = st.states.find(state);
  if(it==st.states.end())
    it = st.states.emplace(std::string(state),StateStats()).first;
  if(phase<phaseNames.size())
    ++it->second.phases[phase];
  }

std::string ActivityEvents::json() {
  const auto& st = stats();
  std::string ret = "{\"watchdog\":"+std::to_string(st.watchdog)+",\"reservation_expired\":"+std::to_string(st.expired)+
                    ",\"reservation_ownerless\":"+std::to_string(st.ownerless)+",\"double_occupancy\":"+std::to_string(st.doubled)+
                    ",\"reservations\":"+std::to_string(st.reserved)+",\"reservation_bypassed\":"+std::to_string(st.bypassed)+
                    ",\"states\":{";
  bool first = true;
  for(const auto& [name,s]:st.states) {
    ret += (first ? "" : ",")+jsonEscape(name)+":{\"steps\":"+std::to_string(s.steps)+",\"wakes\":{";
    first = false;
    for(size_t i=0; i<WakeCount; ++i)
      ret += (i ? "," : "")+jsonEscape(wakeNames[i])+":"+std::to_string(s.wakes[i]);
    ret += "},\"record_phases\":{";
    for(size_t i=0; i<phaseNames.size(); ++i)
      ret += (i ? "," : "")+jsonEscape(phaseNames[i])+":"+std::to_string(s.phases[i]);
    ret += "}}";
    }
  return ret+"}}";
  }
