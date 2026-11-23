#pragma once

#include "daedalus/vm.h"
#include "daedalus/classes.h"

class GameplayDefinitions;

class SpellDefinitions final {
  public:
    SpellDefinitions(daedalus::DaedalusVm& vm, const GameplayDefinitions* records = nullptr);
    ~SpellDefinitions();

    const daedalus::ISpell& find(std::string_view instanceName) const;

  private:
    daedalus::DaedalusVm&                          vm;
    std::vector<std::shared_ptr<daedalus::ISpell>> spl;
  };
