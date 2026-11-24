#pragma once

#include "npcdefinitions.h"

#include <array>
#include <optional>
#include <variant>

namespace PlacementDefinitions {
using Value = NpcDefinitions::Value;
using Call = NpcDefinitions::Call;
struct GlobalWrite {
  std::string symbol;
  uint16_t index = 0;
  Value value;
  bool operator==(const GlobalWrite&) const = default;
  };
using Step = std::variant<Call,GlobalWrite>;
struct Definition {
  std::vector<Step> steps;
  bool operator==(const Definition&) const = default;
  };
struct LiteralInsert {
  uint32_t instance;
  uint32_t point;
  uint32_t external;
  };
struct KhorinisEarlyChapters {
  static constexpr size_t chapter2Count = 50;
  static constexpr std::array<size_t,9> chapter3Sizes{1,1,1,3,4,61,8,2,4};
  std::vector<LiteralInsert> chapter2;
  std::array<std::vector<LiteralInsert>,chapter3Sizes.size()> chapter3;
  };
struct KhorinisLateChapters {
  static constexpr std::array<size_t,6> chapter4Sizes{15,61,6,90,23,1};
  static constexpr std::array<size_t,7> chapter5Sizes{1,1,17,2,26,9,1};
  std::array<std::vector<LiteralInsert>,chapter4Sizes.size()> chapter4;
  std::array<std::vector<LiteralInsert>,chapter5Sizes.size()> chapter5;
  };
struct OldWorldChapters {
  static constexpr std::array<size_t,3> chapter3Sizes{44,1,1};
  static constexpr std::array<size_t,8> chapter4Sizes{11,5,1,1,1,1,235,1};
  std::array<std::vector<LiteralInsert>,chapter3Sizes.size()> chapter3;
  std::array<std::vector<LiteralInsert>,chapter4Sizes.size()> chapter4;
  };
struct RoomAssignment {
  uint32_t label;
  std::optional<uint32_t> guild;
  };
struct RoomChoice {
  uint32_t expected, flag;
  RoomAssignment equal, otherwise;
  };
struct MobRoutine {
  int32_t hour, minute;
  uint32_t name;
  int32_t state;
  };
struct RoomInitializer {
  std::vector<MobRoutine> mobs;
  std::vector<std::variant<RoomAssignment,RoomChoice>> rooms;
  };
inline constexpr struct RoomInitializerShape {
  const char* name;
  size_t rooms, choices = 0, mobs = 0;
  } roomInitializerShapes[] = {
  {"INIT_SUB_ADDON_PART_BANDITSCAMP_01",6}, {"INIT_SUB_NEWWORLD_PART_MONASTERY_01",13}, {"INIT_SUB_OLDCAMP",25},
  {"INIT_SUB_NEWWORLD_PART_CITY_01",44,4,3}, {"INIT_SUB_NEWWORLD_PART_GREATPEASANT_01",13,2},
  };
struct FirstEntries {
  static constexpr size_t oldWorldCount = 18;
  static constexpr size_t addonWorldCount = 19;
  std::vector<uint32_t> oldWorld, addonWorld;
  };
struct Library {
  static constexpr size_t addonChapter4Count = 137;
  std::map<std::string,Definition> placements;
  std::map<std::string,std::string> uncaptured;
  std::optional<KhorinisLateChapters> khorinisLateChapters;
  std::vector<LiteralInsert> addonChapter4;
  std::optional<std::vector<uint32_t>> startupFunctions;
  std::optional<OldWorldChapters> oldWorldChapters;
  std::vector<LiteralInsert> dragonIsland;
  std::map<std::string,RoomInitializer> roomInitializers;
  std::optional<FirstEntries> firstEntries;
  std::optional<KhorinisEarlyChapters> khorinisEarlyChapters;
  };
}
