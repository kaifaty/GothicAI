#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <filesystem>

namespace NativeBaseCatalog { struct Selection; }

namespace NativeWorldPlacement {

struct Spawn {
  uint32_t instance;
  std::string point;
  bool npc;
  };

struct SetFlag { std::string name; int32_t value; };
struct SetTime { int32_t hour, minute; };
struct PlayVideo { std::string name; };
struct KillNpc { uint32_t instance; };
struct InitLanguage {};
using StartupStep = std::variant<Spawn,SetFlag,SetTime,PlayVideo,KillNpc,InitLanguage>;

struct Room {
  std::string label;
  int32_t guild;
  };

struct RoomChoice {
  std::string flag;
  int32_t expected;
  Room equal, otherwise;
  };

struct MobRoutine {
  int32_t hour, minute;
  std::string name;
  int32_t state;
  };

struct RoomInitializer {
  std::vector<MobRoutine> mobs;
  std::vector<std::variant<Room,RoomChoice>> rooms;
  };

struct Definitions {
  std::map<std::string,std::vector<StartupStep>,std::less<>> startup;
  std::vector<Spawn> dragonIsland, addonChapter4;
  std::map<std::string,RoomInitializer,std::less<>> rooms;
  std::string dragonArrival;

  static std::filesystem::path section(const std::filesystem::path& base,std::string_view programSha);
  static Definitions load(const std::filesystem::path& base,std::string_view programSha,
                          const NativeBaseCatalog::Selection& catalog);
  };

}
