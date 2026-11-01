#pragma once

#include <cstdint>
#include <string>

class World;

// Bench-only journal of every NPC over continuous game time, for the
// day-parity check of the NPC simulation tiers (world streaming, step 1,
// item 6; scripts/npc_day_parity.py). It reads the world and never changes
// it, except for the optional private random streams set up by start().
//
// Files in the directory:
//   npc-day.json         header (mode, seed, period, start) and, at the end, a summary
//   npc-day.csv          one row per NPC every `period` game minutes
//   npc-day-events.csv   ZS starts/ends, routine-entry changes, deaths, inserts, removals
//   npc-day-cost.csv     NPC tier counts and per-tier tick time at every sample
namespace NpcDayJournal {

struct Options {
  std::string directory;
  uint64_t    minutes = 24*60;   // continuous game minutes from the start
  uint32_t    period  = 5;       // game minutes between samples
  uint32_t    seed    = 0;
  // Give every NPC a private script random stream (GameScript::isolateRandom)
  // keyed by its journal key, so one NPC's extra draw does not shift every
  // later NPC's dice. Only the bench uses it; the game keeps one shared stream.
  bool        isolate = true;
  };

bool start(World& world, const Options& options, std::string& error);
// Every bench tick after the world step: event polling and due samples. It
// writes the final sample and the summary when the requested time is reached.
void tick(World& world);
bool running();
void stop(World& world, const char* reason);

}
