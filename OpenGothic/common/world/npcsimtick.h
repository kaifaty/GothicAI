#pragma once

#include <cstdint>
#include <string>

class Npc;

// Far NPC tick rate (world streaming, user decision 7: far NPCs live by the same rules, as simply as
// possible). A Simplified NPC that qualifies gets Npc::tick about once per period of world time instead of
// every frame, with all the world time since its previous tick: the dt it gets add up to the world time, and
// nothing is lost or doubled. The state lives in the Npc itself (Npc::simTick), so NPCs added, removed or
// re-sorted by scripts never read another one's time. It is not saved: a load starts afresh.
//
// The NPC ticks in the frame where the world time since its own previous tick is nearest to the period, the
// current frame step standing for the next one (ties tick earlier): 8 ms frames, 33 ms period - every 4th
// frame; 30 ms frames - every frame. The first tick after the NPC begins to qualify comes at its phase within
// the frames of one period, from its creation number (as the animation LOD's, never its address: the frames
// chosen change the world and must replay); this spreads the NPCs evenly over the frames. The spread only
// shifts that first frame and never adds time. At a frame step not below the period every NPC ticks every
// frame with the frame's dt, exactly as without the rule (the day stand steps 33 ms).
//
// OPENGOTHIC_NPC_SIM_TICK_MS: the period, 16..66, default 33; 0 turns the rule off.
// OPENGOTHIC_NPC_SIM_TICK_SCOPE: who qualifies, graph (default) or tier (see qualifies).
namespace NpcSimTick {

enum class Scope : uint8_t { Graph, Tier };

struct Config {
  uint64_t periodMs = 33;
  Scope    scope    = Scope::Graph;
  };

constexpr uint64_t MinPeriodMs = 16, MaxPeriodMs = 66;

// An invalid value gives its default and a description in `problem`.
Config        parse(const char* period, const char* scope, std::string* problem);
const Config& config();                     // the environment, read once per process
void          setConfig(const Config& cfg); // contracts: replaces it for the rest of the process
const char*   name(Scope scope);

// Whether the NPC may skip frames now, from its own state, read again every frame. Never the hero, a Full NPC
// (the arena and training actors always are), a body the physics step moves or reads every frame (ragdoll,
// living body) or an actor driven or sampled step by step (body policy, stand keys, observations).
// Graph: besides, it executes graph movement (Npc::usesSimplifiedMovement). Tier: any other Simplified NPC
// but melee fighters and bodies under spell physics (frozen, whirled or in the air).
bool qualifies(const Npc& npc, Scope scope);

// The NPC's place in the frames of one period, from its creation number.
uint32_t phase(const Npc& npc);

struct State {
  uint64_t pending = 0;     // world time not yet handed to Npc::tick
  bool     rhythm  = false; // ticked by the rule since it last ticked unqualified: the next tick is not its first
  };

// Whether the NPC's turn has come, if it qualifies: `owed` is its pending time plus this frame's step dt.
bool due(const State& state, uint64_t owed, uint64_t dt, uint32_t phase, const Config& cfg);

}
