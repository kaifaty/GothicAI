#pragma once

// The journal of a run and its 5-minute marks (docs/society-sim.md, «Журнал и отметки»).
//
// Journal, binary, little-endian. Header:
//   8   magic "GSOCJRNL"
//   u32 format version (1)
//   u32 bytes per record (24)
//   u64 the export's hash (FNV-1a 64 of its bytes, World::exportHash)
//   u64 seed
//   i64 start time (game seconds since day 0, 00:00)
//   u32 NPCs, u32 points (the export's npcs[] and waypoints[]: ids in the records are their indices)
//   u32 states, then each state name: u16 length and its bytes (the index in the records is the place here)
// Then the events, 24 bytes each:
//   u32 time (game seconds since day 0, 00:00), u32 npc, u8 kind (Society::EventKind), u8 reason,
//   u16 routine entry in force (0xFFFF: none), u32 a, u32 b, u32 c (the arrival time of travel_started)
// Version 2 (the rules measured against the engine): the record is the same; activity_started carries a reason
// (Society::ActivityReason) and comes StartDelay after routine_entry, before the walk, as the engine's ZS starts.
// Version 3 (needs, only with a config that has them; without, the journal stays version 2 byte for byte): the
// header goes on after the state names with
//   u64 the config's hash (FNV-1a 64 of its bytes), u32 needs, then each need's id: u16 length and its bytes
// and the records add kind 6 need_threshold: reason 0 down (to the threshold or below) / 1 up (above it),
// a = need (the index in that list), b = threshold (1 low, 2 critical), c = the state being done (0xFFFF none).
//
// Marks: CSV, one row per NPC every 5 game minutes from the start, NPCs in the export's order:
//   t_min,key,routine,state,moving,point,to,arrive_min,x,y,z
// t_min and arrive_min — game minutes since the start; routine — "index hh:mm-hh:mm ZS waypoint" as in the
// engine's npc-day.csv, or "-"; state — the ZS being done, also on the way (as the engine's zs), "-" for none;
// moving 1: on the way from `point` to `to`, arriving at arrive_min; moving 0: at `point`, `to` and arrive_min "-";
// x, y, z — the position in cm: the point's, or on the way the place along the walk's points by the time gone.
// With needs (journal v3) every row goes on with a column per need, `need_<id>`: a person's level at the mark with
// four decimals, "-" for an NPC that is not a person.

#include "society.h"

#include <string>

namespace Society {
namespace Journal {

constexpr uint32_t Version     = 2;
constexpr uint32_t NeedsVersion = 3; // with needs
constexpr uint32_t RecordBytes = 24;
constexpr char     Magic[8]    = {'G','S','O','C','J','R','N','L'};

std::string header(const World& world, uint64_t seed);
// Appends the records; throws when a time does not fit the u32 field.
void        append(std::string& out, std::span<const Event> events);

}

namespace Marks {

constexpr Time Period = 5*60;

std::string header(const World& world);
// The rows of every NPC at the sim's current time; the walk's points come from (and fill) the sim's path cache.
void        append(std::string& out, Sim& sim);

}
}
