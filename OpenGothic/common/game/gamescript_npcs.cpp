#include "gamescript.h"

#include "world/objects/npc.h"

void GameScript::initializeNpcDefinition(const std::shared_ptr<daedalus::INpc>& npc,
                                         daedalus::DaedalusSymbol* symbol, bool useRecords) {
  if(npc->user_ptr)
    static_cast<Npc*>(npc->user_ptr)->npcInstanceSymbol=symbol->index();
  // A director NPC keeps its own symbol while using its template's definition.
  auto* like=own.likeOf(symbol->index());
  const NpcDefinitionRuntime::Definitions empty;
  NpcDefinitionRuntime::initialize(vm,npc,symbol,useRecords ? npcDefinitions : empty,like);
  }
