#pragma once

#include "placementdefinitions.h"
#include "daedalus/vm.h"

#include <unordered_map>

namespace PlacementDefinitionRuntime {
struct Argument {
  PlacementDefinitions::Value value;
  daedalus::DaedalusDataType type = daedalus::DaedalusDataType::INT;
  uint32_t literal = 0;
  uint8_t literalIndex = 0;
  };
struct Call {
  uint32_t function = 0;
  std::vector<Argument> args;
  bool deferred = false;
  };
struct GlobalWrite {
  uint32_t symbol = 0;
  uint16_t index = 0;
  PlacementDefinitions::Value value;
  };
using Step = std::variant<Call,GlobalWrite>;
struct Definitions {
  std::unordered_map<uint32_t,std::vector<Step>> placements;
  std::optional<PlacementDefinitions::KhorinisLateChapters> khorinisLateChapters;
  };

bool enabled();
Definitions resolve(const PlacementDefinitions::Library& library, const daedalus::DaedalusScript& script);
Definitions load(const std::filesystem::path& base, const daedalus::DaedalusScript& program,
                 std::string_view sha256);
void observeCalls(daedalus::DaedalusVm& vm, const std::filesystem::path& base, std::string_view sha256);
// The startup game entry point and the contract share the record/VM choice.
bool execute(daedalus::DaedalusVm& vm, const daedalus::DaedalusSymbol* function,
             const Definitions& definitions);
}
