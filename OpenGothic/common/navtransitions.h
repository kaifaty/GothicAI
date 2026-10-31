#pragma once

#include <string>
#include <vector>

class World;
class Npc;
class NavMesh;

// Native movement experiments belong to the body runner, never normal gameplay.
namespace NavTransitions {
bool command(const std::vector<std::string>& args, World& world, Npc& player,
             NavMesh& mesh, const std::string& output);
void tick(World& world);
bool running();
bool controls(const Npc& npc);
bool supported(World& world, Npc& npc);
void reset();
}
