#pragma once

#include <string>
#include <vector>

class Npc;
class World;

// Runner fixture of scripts/bench/blood-stains*.bench (physics stage 8, step 4c).
// Lays blood stains near the hero (splashes, an indoor splash, a pool under a dead
// person), moves the clock, removes the body, saves the session and samples the
// stains and the `blood` traces of World::physicsTraces. A failed check throws; the
// runner turns it into blood_stains_error.
namespace BloodStainsBench {
  void reset();
  std::string command(const std::vector<std::string>& args, World& world, Npc& player,
                      const std::string& outputDirectory);
}
