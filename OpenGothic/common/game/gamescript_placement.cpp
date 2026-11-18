#include "gamescript.h"

void GameScript::runStartup(std::string_view fn) {
  PlacementDefinitionRuntime::execute(vm,vm.find_symbol_by_name(fn),placementDefinitions);
  }
