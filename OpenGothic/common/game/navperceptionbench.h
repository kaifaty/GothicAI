#pragma once

#include <string>
#include <vector>

class World;
class Npc;

// Bounded, evaluator-only replay of saved poses. It neither runs a teacher nor
// records demonstrations. Truth is queried only after eye/memory readout.
namespace NavPerceptionBench {
void command(const std::vector<std::string>& args, World& world, Npc& player,
             const std::string& outputDirectory);
}
