#pragma once

#include <cstdint>
#include <string>

class Npc;

// Far NPC tick rate (world streaming, user decision 7: far NPCs live by the same rules, as simply as
// possible). A Simplified NPC that qualifies gets Npc::tick about once per period of world time instead of
// every frame, with all the world time since its previous tick: the dt it gets add up to the world time, and
// nothing is lost or doubled. The remainder lives in the Npc itself (Npc::simTickPending), so NPCs added,
// removed or re-sorted by scripts never read another one's time. It is not saved: a load starts at zero.
//
// Frames per period: as many frames as make the time since the previous tick nearest to the period at the
// current frame step (8 ms frames, 33 ms period: every 4th frame; 30 ms frames: every frame). A phase clock of
// the world advances one period per that many frames; an NPC ticks in the frame where the clock plus its own
// phase crosses a whole period. The phase comes from the NPC's creation number (as the animation LOD's, never
// its address: the frames chosen change the world and must replay), so the NPCs spread evenly over the frames,
// and a long frame that ticks everyone does not bunch them up afterwards. At a frame step not below the period
// every NPC ticks every frame with the frame's dt, exactly as without the rule (the day stand steps 33 ms).
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

// The NPC's place in the period, from its creation number.
uint32_t phase(const Npc& npc);

// The phase clock of one world.
class Clock final {
  public:
    // A new world tick with frame step dt.
    void advance(uint64_t dt, const Config& cfg);
    // Every NPC ticks this frame: the rule is off or the frame step is not below the period.
    bool everyone() const { return all; }
    // This frame is the NPC's turn.
    bool due(const Npc& npc) const;

  private:
    uint64_t now = 0, before = 0;
    bool     all = true;
  };

}
