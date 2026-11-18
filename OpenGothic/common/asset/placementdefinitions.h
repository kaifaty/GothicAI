#pragma once

#include "npcdefinitions.h"

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
struct Library {
  std::map<std::string,Definition> placements;
  std::map<std::string,std::string> uncaptured;
  };
}
