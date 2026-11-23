#pragma once

#include "daedalus/vm.h"
#include "game/fighttactic.h"

class FightAi final {
  public:
    using FA = FightTactic;

    FightAi();

    const FA& operator[](size_t i) const;

  private:
    auto loadAi(daedalus::DaedalusVm &vm, std::string_view name) -> Def::FightAi;
    FA   loadAi(daedalus::DaedalusVm &vm, size_t id);

    std::vector<FA> fAi;
  };
