#pragma once

#include "daedalus/classes.h"
#include "daedalus/vm.h"
#include "game/itemdefinitionruntime.h"

namespace ItemDefinitionRuntime {
Definitions resolve(const ItemDefinitions::Library& library, const daedalus::DaedalusScript& script);
Definitions load(const std::filesystem::path& base, const daedalus::DaedalusScript& program,
                 std::string_view sha256);
// The offline contract compares native records with the original constructors.
bool initialize(daedalus::DaedalusVm& vm, const std::shared_ptr<daedalus::IItem>& item,
                daedalus::DaedalusSymbol* symbol, const Definitions& definitions);
}
