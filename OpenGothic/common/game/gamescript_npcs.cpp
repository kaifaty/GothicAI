#include "gamescript.h"

void GameScript::initializeNpcDefinition(const std::shared_ptr<daedalus::INpc>& npc,
                                         daedalus::DaedalusSymbol* symbol) {
  // A director NPC keeps its own symbol while using its template's definition.
  auto* like=own.likeOf(symbol->index());
  NpcDefinitionRuntime::initialize(vm,npc,symbol,npcDefinitions,like);
  }
