#pragma once

#include "game/fighttactic.h"

#include <cstddef>
#include <vector>

class FightAi final {
  public:
    using FA = FightTactic;

    FightAi();

    const FA& operator[](size_t i) const;

  private:
    std::vector<FA> fAi;
  };
