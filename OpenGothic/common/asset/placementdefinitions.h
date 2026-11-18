#pragma once

#include "npcdefinitions.h"

namespace PlacementDefinitions {
using Value = NpcDefinitions::Value;
using Call = NpcDefinitions::Call;
struct Definition {
  std::vector<Call> steps;
  bool operator==(const Definition&) const = default;
  };
struct Library {
  std::map<std::string,Definition> placements;
  std::map<std::string,std::string> uncaptured;
  };
}
