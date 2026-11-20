#pragma once

#include "npcdefinitions.h"
#include "npchelperruntime.h"
#include "daedalus/classes.h"
#include "daedalus/vm.h"

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
using Step = std::variant<Field,Call>;
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
// The game and its contract share this choice of definition or VM constructor.
// Engine calls use the VM's registered external, exactly as a script BE instruction does.
bool initialize(daedalus::DaedalusVm& vm, const std::shared_ptr<daedalus::INpc>& npc,
                daedalus::DaedalusSymbol* symbol, const Definitions& definitions,
                daedalus::DaedalusSymbol* definitionSymbol = nullptr);
}
