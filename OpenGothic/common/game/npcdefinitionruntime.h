#pragma once

#include "npcdefinitions.h"
#include "npchelperruntime.h"
#include "daedalus/classes.h"
#include "daedalus/vm.h"

#include <functional>
#include <unordered_map>

namespace NpcDefinitionRuntime {
struct Field {
  uint32_t binding = 0;
  uint16_t index = 0;
  NpcDefinitions::Value value;
  };
struct Argument {
  NpcDefinitions::Value value;
  daedalus::DaedalusDataType type = daedalus::DaedalusDataType::INT;
  uint32_t literal = 0;
  uint8_t literalIndex = 0;
  };
struct Call {
  uint32_t function = 0;
  std::vector<Argument> args;
  bool deferred = false;
  };
struct GlobalIntWrite {
  uint32_t binding = 0;
  uint16_t index = 0;
  int32_t value = 0;
  };
using Step = std::variant<Field,Call,GlobalIntWrite>;
struct Definitions {
  std::unordered_map<uint32_t,std::vector<Step>> npcs;
  NpcHelperRuntime::Definitions helpers;
  };

bool enabled();
Definitions resolve(const NpcDefinitions::Library& library, const daedalus::DaedalusScript& script);
Definitions load(const std::filesystem::path& base, const daedalus::DaedalusScript& program,
                 std::string_view sha256);
void observeCalls(daedalus::DaedalusVm& vm, const Definitions& definitions,
                  const std::filesystem::path& base, std::string_view sha256);
// The observer, when present, receives the original VM arguments before the engine operation.
using ApplyCall = std::function<bool(const daedalus::DaedalusSymbol&, const Call&,
                                         const std::function<void()>& observe)>;
bool initialize(daedalus::DaedalusVm& vm, const std::shared_ptr<daedalus::INpc>& npc,
                daedalus::DaedalusSymbol* symbol, const Definitions& definitions,
                daedalus::DaedalusSymbol* definitionSymbol = nullptr, const ApplyCall& applyCall = {});
}
