#pragma once

#include "game/gametime.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>

namespace ActivityDefinitions { struct MobActivity; }

// Routine activities at world objects run by events instead of a polled Loop (step E1,
// reports/engine/2026-11-14-npc-data/d4-event-executor.md). Init stays the record phase (the
// perception set in the VM, B_ResetAll, the walk to the routine point); then a small automaton
// walks approach -> reserve and use -> hold, and its code runs only when one of the wakes below
// arrives. End stays the record phase. The places of the objects are MobReservations.
namespace ActivityEvents {
enum class Stage : uint8_t {
  Off,      // the state polls its Loop: no record, events off, or not the state's automaton
  Approach, // Init's commands are queued: wait for the queue to empty (arrival at the point)
  Attach,   // AI_UseMob is queued: wait for the attach, its failure or the queue
  Wait,     // every object of the scheme in reach is taken: wait for a freed place
  Hold,     // at the object: nothing runs until the end of the row or a lost object
  };

// Why the automaton ran; one step may carry several.
enum Wake : uint16_t {
  Entered      = 1<<0, // Init ran: the state started or the NPC came back to it
  Loaded       = 1<<1, // the first step of the state after a load
  QueueDone    = 1<<2, // the queue emptied after the automaton's commands
  Attached     = 1<<3, // the NPC attached to an object
  AttachFailed = 1<<4, // the use was dropped, the queue was reset, or the reservation was lost
  Detached     = 1<<5, // the NPC left its object while holding it
  Freed        = 1<<6, // a place of the scheme was freed near the waiting NPC
  RowEnd       = 1<<7, // the routine row ended (eTime)
  Interrupted  = 1<<8, // the state ended otherwise: another state, death, a reset, removal
  Watchdog     = 1<<9, // a wait outlived WatchdogMs: the activity restarts from the place check
  };
inline constexpr size_t WakeCount = 10;
std::string_view wakeName(size_t bit);

// Every wait of the automaton ends by the world clock: a lost event restarts the activity.
inline constexpr uint64_t WatchdogMs    = 60*60*1000;
// A reservation its owner never turned into a use expires: the place is free again.
inline constexpr uint64_t ReservationMs = 10*60*1000;

struct State final {
  const ActivityDefinitions::MobActivity* activity = nullptr; // null: the state polls its Loop
  size_t   init       = size_t(-1);         // the Init symbol this automaton was set up for
  Stage    stage      = Stage::Off;
  uint16_t pending    = 0;                  // Wake bits not yet handled
  bool     awaitQueue = false;              // QueueDone when the queue is empty next time
  gtime    deadline   = gtime::endOfTime(); // the watchdog of the current wait
  };

// Day counters (bench npcday: npc-day.json "activity_events"). Reset by the journal. The calls of each
// phase, Loop among them, are the probe's (npc-day-script.json).
struct StateStats final {
  uint64_t wakes[WakeCount] = {};
  uint64_t steps = 0;                // place checks: each runs the activity's code once
  };
struct Stats final {
  std::map<std::string,StateStats,std::less<>> states;
  uint64_t watchdog  = 0;            // waits that outlived WatchdogMs
  uint64_t reserved  = 0;            // reservations made
  uint64_t expired   = 0;            // reservations whose owner never attached in ReservationMs
  uint64_t ownerless = 0;            // reservations whose owner is dead, gone or no longer holds them
  uint64_t doubled   = 0;            // two NPCs on one place
  uint64_t bypassed  = 0;            // a reserved place taken by another NPC without a reservation
  };
Stats&      stats();
void        reset();
void        countWakes(std::string_view state, uint16_t wakes, bool step);
std::string json();
}
