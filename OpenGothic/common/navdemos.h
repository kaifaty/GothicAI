#pragma once

#include <cstdint>
#include <string>
#include <vector>

class World;
class Npc;
namespace NavDemos {
void reset();
bool running();
// Stand population actors and opt-in CV episodes retain full simulation at any distance.
bool fullSimulation(const Npc& npc);
// Explicit stand physics mode; ordinary NPC controller radius rules stay unchanged.
bool forceCharacterVirtual(const Npc& npc);
void command(const std::vector<std::string>& args, World& world, Npc& player,
             const std::string& output);
void tick(World& world, uint64_t dt);
}
