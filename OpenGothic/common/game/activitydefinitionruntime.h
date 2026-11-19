#pragma once

#include "activitydefinitions.h"
#include "daedalus/vm.h"

#include <functional>
#include <unordered_map>

class Npc;
class World;

// Routine activities from records (D4.1): a phase of a state with a record runs the template
// walk instead of the script; the perception helper stays in the VM at its place.
namespace ActivityDefinitionRuntime {
// OPENGOTHIC_ACTIVITY_RECORDS: unset (or any other value) runs the accepted states from records,
// "0" runs every state in the VM, "all" runs every captured state from its record (D4.2 measures).
enum class Mode : uint8_t { Off, Accepted, All };
Mode mode();
// The states whose record parity is accepted; other captured states need "all".
bool accepted(std::string_view state);

struct Phase {
  const ActivityDefinitions::MobActivity* activity = nullptr;
  ActivityDefinitions::Phase              phase = ActivityDefinitions::Phase::Init;
  uint32_t                                helper = 0;
  };
// Phases point into `activities`: moving keeps the nodes, a copy would not.
struct Definitions {
  Definitions() = default;
  Definitions(Definitions&&) = default;
  Definitions& operator=(Definitions&&) = default;
  Definitions(const Definitions&) = delete;
  Definitions& operator=(const Definitions&) = delete;
  std::unordered_map<std::string,ActivityDefinitions::MobActivity> activities;
  std::unordered_map<uint32_t,Phase> phases; // Init, Loop and End function symbols
  };
Definitions resolve(const ActivityDefinitions::Library& library, const daedalus::DaedalusScript& script, Mode mode);
Definitions load(const std::filesystem::path& base, const daedalus::DaedalusScript& program, std::string_view sha256);

// Offline instrumentation, unset in the game: every template call of a record phase as text.
inline std::function<void(const Npc&, std::string_view)> observer;
// The record phase of a state function, null when the function runs in the VM.
const Phase* find(const Definitions& definitions, uint32_t function);
// Runs a record phase of `npc` and returns the phase result. SELF, OTHER and VICTIM are set by
// the caller, as for the VM.
int run(daedalus::DaedalusVm& vm, World& world, Npc& npc, const Phase& phase);
}
