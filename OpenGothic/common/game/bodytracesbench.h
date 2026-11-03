#pragma once

#include <string>
#include <vector>

class Npc;
class World;

// Runner fixture of scripts/bench/body-traces*.bench (physics stage 8, step 4b).
// Spawns and kills people and a wolf near the hero, reads the production npc_view
// through TestBench::npcView, moves the clock and the hero, and checks the body
// lifetime rule. A failed check throws; the runner turns it into body_traces_error.
namespace BodyTracesBench {
  void reset();
  std::string command(const std::vector<std::string>& args, World& world, Npc& player,
                      const std::string& outputDirectory);
}
