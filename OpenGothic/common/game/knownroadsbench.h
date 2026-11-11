#pragma once

#include <string>
#include <vector>
#include "knownroads.h"

class World;
class Npc;
class NavigationMemory;

// Stand commands only; roads neither move NPCs nor alter their daily routines.
namespace KnownRoadsBench {
  // Read the walking actor's enabled knowledge; never grants additional roads.
  KnownRoads::Path readPath(World& world, const Npc& npc, const Tempest::Vec3& goal,
                            Tempest::Vec3& next);
  // passage: the walker's own memory verdict on a straight passage (catch-up
  // and the start of a rebuilt route); empty keeps the window-only rule.
  void readGuidance(World& world, const Npc& npc, const Tempest::Vec3& position,
                    const Tempest::Vec3& goal, KnownRoads::Guidance& guidance,
                    const KnownRoads::PassageCheck& passage = {});
  // Stand episodes only: attaches the offline eye atlas through this actor's
  // road/place knowledge. A stranger never receives a familiar mask.
  void attachFamiliar(World& world, const Npc& npc, NavigationMemory& memory);
  // Build population knowledge at each saved actor's home, before any teleport.
  void writePackage(World& world, const std::string& places,
                    const std::vector<std::string>& selectors, const std::string& directory);
  void reset();
  void tick(World* world);
  std::string command(const std::vector<std::string>& args, World& world,
                      const std::string& outputDirectory);
}
