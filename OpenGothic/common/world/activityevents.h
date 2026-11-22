#pragma once

#include "game/gametime.h"

#include <cstddef>
#include <cstdint>
#include <vector>
#include <map>
#include <string>
#include <string_view>

class WayPoint;
namespace ActivityDefinitions { struct MobActivity; struct PointActivity; }

// Recorded routines keep Init and End. Between them, events drive the object's reservation
// and use (E1), or a point activity's arrival, pose and timed gesture (E2).
namespace ActivityEvents {
struct Record {
  const ActivityDefinitions::MobActivity*   mob   = nullptr;
  const ActivityDefinitions::PointActivity* point = nullptr;
  };

enum class Stage : uint8_t {
  Off,      // the state polls its Loop: no record, events off, or not the state's automaton
  Approach, // Init's commands are queued: wait for the queue to empty (arrival at the point)
  Attach,   // AI_UseMob is queued: wait for the attach, its failure or the queue
  WalkToPoint, // the chosen free point is acquired when the queued walk starts
  Wait,     // waiting for a place in reach
  Hold,     // using the object or holding the pose
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
  Gesture      = 1<<10, // the state clock reached the next gesture draw
  Visibility   = 1<<11, // recheck a free post previously hidden by an obstacle
  };
inline constexpr size_t WakeCount = 12;

// Recover a stalled approach or object wait on the world clock.
inline constexpr uint64_t WatchdogMs    = 60*60*1000;
// A reservation its owner never turned into a use expires: the place is free again.
inline constexpr uint64_t ReservationMs = 10*60*1000;

struct State final {
  const Record* activity = nullptr; // null: the state polls its Loop
  size_t   init       = size_t(-1);         // the Init symbol this automaton was set up for
  Stage    stage      = Stage::Off;
  uint16_t pending    = 0;                  // Wake bits not yet handled
  bool     awaitQueue = false;              // QueueDone when the queue is empty next time
  bool     restart    = false;              // the walk to the point was reset: start over with Init
  bool     retry      = false;              // the watchdog fired and was counted: look for the place again
  std::vector<const WayPoint*> waitPoints; // locked candidates within reach of this NPC
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
  uint64_t expired   = 0;            // reservations whose owner never attached in ReservationMs
  uint64_t ownerless = 0;            // reservations whose owner is dead, gone or no longer holds them
  uint64_t doubled   = 0;            // two NPCs on one place
  bool     counting  = false;        // the wakes are counted only while a journal or a contract reads them
  };
Stats&      stats();
void        reset(); // zero counters, and count the wakes from now on
void        countWakes(std::string_view state, uint16_t wakes, bool step);
std::string json();
}
