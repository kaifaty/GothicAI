#include "benchhooks.h"
#include "objects/npc.h"
#include "world.h"

std::string_view Bench::who(const Npc* npc) {
  if(npc==nullptr)
    return "none";
  auto name = npc->world().rules().symbolName(size_t(npc->instanceSymbol()));
  return name.empty() ? std::string_view("npc") : name;
  }

std::string_view World::npcSymbolName(const Npc* npc) const { return Bench::who(npc); }
