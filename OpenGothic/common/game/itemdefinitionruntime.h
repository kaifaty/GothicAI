#pragma once

#include "itemdefinitions.h"
#include "daedalus/classes.h"
#include "daedalus/vm.h"

#include <unordered_map>

namespace ItemDefinitionRuntime {
struct Definitions {
  std::unordered_map<uint32_t,ItemRecord> items;
  };
Definitions resolve(const ItemDefinitions::Library& library, const daedalus::DaedalusScript& script);
// Select the section by the verified program-record SHA, applying the game's environment switch.
Definitions load(const std::filesystem::path& base, const daedalus::DaedalusScript& program,
                 std::string_view sha256);
// Both the game and its contract use this complete choice of initialization path.
// Returns true when a definition supplied the fields, false after a VM constructor.
bool initialize(daedalus::DaedalusVm& vm, const std::shared_ptr<daedalus::IItem>& item,
                daedalus::DaedalusSymbol* symbol, const Definitions& definitions);
}
