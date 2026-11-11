#pragma once

// The rules' internals shared by sim.cpp (the routine and the needs) and decide.cpp (the menu, the masks, the
// activities chosen and the policies). Not part of the interface: society.h is.

#include "society.h"

namespace Society {
namespace detail {

// sim.cpp
uint64_t push(State& s, Time t, uint32_t npc, Pending::Kind kind);
void     pushBody(State& s, Time t, uint32_t npc, Pending::Kind kind);
// An event of the journal; one that can change what a person does ends its needs' segment (thresholds first).
void     emit(Sim& sim, EventKind kind, uint32_t npc, uint8_t reason, uint32_t a, uint32_t b, Time c = 0);
void     setState(Sim& sim, uint32_t npc, uint16_t state, ActivityReason reason, uint32_t at);
// A standing NPC goes where it should be: its chosen activity's place, or its routine entry's point.
void     follow(Sim& sim, uint32_t npc);
bool     near(const World& w, uint32_t a, uint32_t b);
uint32_t entryPoint(const Sim& sim, uint32_t npc, uint16_t e);
uint16_t entryState(const Sim& sim, uint32_t npc, uint16_t e);
inline bool deciding(const Sim& sim) { return !sim.s.mode.empty(); }
inline bool moving(const State& s, uint32_t npc) { return (s.flags[npc] & Moving)!=0; }

// reach.cpp: what the decisions section gives a world at load.
void   applyReach(World& w, const std::unordered_map<std::string,std::string>& placeOf);
bool   fills(const World& w, uint16_t state, size_t need);        // the state raises the need
bool   guildMay(const World& w, uint32_t npc, uint16_t kind);     // the kind's guilds let the NPC use it
Access access(const World& w, uint32_t npc, uint32_t ownerNpc, int32_t ownerGuild);
// Who an option's place is to the NPC by its kind's rule: outdoors and places are public, own rooms only own or
// the guild's; Access::Other is closed.
Access targetAccess(const World& w, uint32_t npc, uint32_t target, uint16_t kind);

// decide.cpp
void   sizeDecisions(Sim& sim);
void   resetDecisions(Sim& sim);
// At the start: a decider in a free entry (or with a critical need in an anchor one) is asked.
void   startDecisions(Sim& sim, uint32_t npc);
// The state's end of a decider has passed: the routine's next entry begins under the rules of decisions.
void   beginEntry(Sim& sim, uint32_t npc);
// A decider's needs began a segment: in a free entry a need forecast to be critical in the next anchor entry asks.
void   onSegment(Sim& sim, uint32_t npc);
// A need of a decider fell to `zone` (1 low, 2 critical).
void   onDown(Sim& sim, uint32_t npc, uint8_t zone);
// The place the NPC's routine uses, now (after its state, walk or standing changes): standing at its entry's point in
// Routine mode doing a kind's state (or the one its into-bed passes to), it holds the nearest place of that kind with
// room among World::spots — a mobsi of the point, a freepoint within FreePointCm; otherwise it lets the one it held go.
// When every such place is full, a chosen activity at the nearest one gives way (Evicted). Only with decisions.
void   routinePlace(Sim& sim, uint32_t npc);
// A decider stands at its chosen activity's place: the activity begins.
void   arrivedChosen(Sim& sim, uint32_t npc);
void   onActivity(Sim& sim, uint32_t npc);
// The rows of the NPCs asked at this second; the ones whose only allowed option is the routine are answered here.
size_t buildBatch(Sim& sim, DecisionBatch& out);
void   applyChoices(Sim& sim, std::span<const Choice> choices);

}
}
