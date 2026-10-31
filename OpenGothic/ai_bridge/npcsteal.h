#pragma once

#include <rapidjson/document.h>
#include <cstddef>
#include <string>

class Npc;
class World;
class Item;
class Interactive;

// Server-only theft uses native movement/interaction queues. The resolver is
// shared by storyguard preflight and execution so ownership cannot drift silently.
namespace NpcSteal {
struct Target final {
  Interactive* chest = nullptr;
  Item* item = nullptr;
  size_t instance = size_t(-1), count = 0;
  std::string owner;
  bool mission = false;
  };
struct Active final {
  bool active = false, opened = false, relock = false;
  Target target;
  size_t before = 0;
  std::string expectedOwner;
  };
enum class Progress { Running, Done, Failed };

Target resolve(World& world, Npc& npc, const rapidjson::Value& args, std::string& error);
std::string preview(World& world, const rapidjson::Value& args);
bool begin(World& world, Npc& npc, const rapidjson::Value& args, Active& active, std::string& error);
Progress tick(World& world, Npc& npc, Active& active, std::string& detail);
void finish(Npc& npc, Active& active);
}
