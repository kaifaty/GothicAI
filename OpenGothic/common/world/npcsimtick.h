#pragma once

#include <cstdint>
#include <string>

class Npc;

// Far NPC tick rate (world streaming, user decision 7: far NPCs live by the same rules, as simply as
// possible). A Simplified NPC that qualifies gets Npc::tick about once per period of world time instead of
// every frame, with all the world time since its previous tick: the dt it gets add up to the world time, and
// nothing is lost or doubled. The time owed lives in the Npc itself (Npc::simTick), so NPCs added, removed or
// re-sorted by scripts never read another one's time. It is not saved: a load starts afresh.
//
// The ticks follow a grid on the world clock of the tick loop (the sum of the dt it handed out): every NPC has
// a constant phase in the period, from its creation number (as the animation LOD's seed, never its address:
// the frames chosen change the world and must replay), and its nodes are phase + k*period. It ticks in the
// frame whose interval (clock - dt, clock] holds one of its nodes, or when it cannot wait: one more frame like
// this one would stretch its step past 1.5 periods. The phases spread the NPCs evenly over the frames at any
// frame step, also after a long frame, since a node does not move. 8 ms frames, 33 ms period: steps of 32 or
// 40 ms; 30 ms frames: every frame (the 1.5-period limit). A frame step not below the period holds a node of
// every NPC: every NPC ticks every frame with the frame's dt, exactly as without the rule (the day stand
// steps 33 ms).
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
// Graph: actors on graph movement or distant idle monsters in their native Fist stance.
// Native actors also have to be clear of melee pairing and spell physics. Tier admits all such native actors.
bool qualifies(const Npc& npc, Scope scope);

// The NPC's node in the period, from its creation number: 0..periodMs-1.
uint64_t phase(uint32_t creation, uint64_t periodMs);
uint64_t phase(const Npc& npc, uint64_t periodMs);

struct State {
  uint64_t pending = 0; // world time not yet handed to Npc::tick
  };

// Whether a qualifying NPC ticks in this frame: `clock` is the world clock of the tick loop after the frame,
// `dt` the frame's step, `owed` the NPC's pending time plus dt.
bool due(uint64_t clock, uint64_t dt, uint64_t owed, uint64_t phase, uint64_t periodMs);

}
