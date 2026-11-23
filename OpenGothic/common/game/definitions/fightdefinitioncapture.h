#pragma once

#include "daedalus/vm.h"
#include "fightdefinitiondata.h"

#include <algorithm>
#include <string>

namespace FightDefinitions {
inline Def::FightAi readMoves(daedalus::DaedalusVm& vm, const daedalus::DaedalusSymbol* move, const std::string& name) {
  auto* id = vm.find_symbol_by_name(name);
  if(id==nullptr || move==nullptr)
    return {};

  try {
    auto instance = vm.init_opaque_instance(id);
    Def::FightAi result{};
    for(uint16_t m=0; m<Def::FightAi::move_count && m<move->count(); ++m)
      result.move[m] = Def::FightMove(move->get_int(m,instance.get()));
    return result;
    }
  catch(const daedalus::DaedalusScriptError&) {
    return {};  // An invalid scripted tactic used the empty row in the game.
    }
  }

inline std::vector<FightTactic> capture(daedalus::DaedalusVm& vm) {
  const auto* max = vm.find_symbol_by_name("MAX_FIGHTAI");
  const auto* move = vm.find_symbol_by_name("C_FIGHTAI.MOVE");
  const auto count = max==nullptr ? 0 : std::max(0,max->get_int());
  std::vector<FightTactic> tactics(static_cast<size_t>(count));
  for(size_t i=1; i<tactics.size(); ++i)
    for(const auto& [prefix, field] : FightDefinitions::slots)
      tactics[i].*field = readMoves(vm,move,std::string(prefix)+std::to_string(i));
  return tactics;
  }
}
