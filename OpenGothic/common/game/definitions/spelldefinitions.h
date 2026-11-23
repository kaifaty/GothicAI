#pragma once

#include "daedalus/vm.h"
#include "daedalus/classes.h"

class SpellDefinitions final {
  public:
    SpellDefinitions(daedalus::DaedalusVm &vm);
    ~SpellDefinitions();

    const daedalus::ISpell& find(std::string_view instanceName) const;

  private:
    daedalus::DaedalusVm&                          vm;
    std::vector<std::shared_ptr<daedalus::ISpell>> spl;
  };
