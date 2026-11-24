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
struct KhorinisLateChapters {
  static constexpr std::array<size_t,6> chapter4Sizes{15,61,6,90,23,1};
  static constexpr std::array<size_t,7> chapter5Sizes{1,1,17,2,26,9,1};
  std::array<std::vector<LiteralInsert>,chapter4Sizes.size()> chapter4;
  std::array<std::vector<LiteralInsert>,chapter5Sizes.size()> chapter5;
  };
struct Library {
  std::map<std::string,Definition> placements;
  std::map<std::string,std::string> uncaptured;
  std::optional<KhorinisLateChapters> khorinisLateChapters;
  };
}
