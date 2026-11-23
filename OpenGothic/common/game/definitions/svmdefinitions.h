#pragma once

#include "daedalus/vm.h"
#include "daedalus/classes.h"

#include <memory>

class GameplayDefinitions;

class SvmDefinitions final {
  public:
    SvmDefinitions(daedalus::DaedalusVm& vm, std::shared_ptr<const GameplayDefinitions> records = {});
    ~SvmDefinitions();

    std::string_view find(std::string_view speech, int id);

  private:
    daedalus::DaedalusVm&                                    vm;
    std::shared_ptr<const GameplayDefinitions> records;
    std::vector<std::shared_ptr<daedalus::DaedalusInstance>> svm;
  };

