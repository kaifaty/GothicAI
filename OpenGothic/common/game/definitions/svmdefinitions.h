#pragma once

#include "daedalus/vm.h"
#include "daedalus/classes.h"

#include <memory>

class SvmDefinitions final {
  public:
    SvmDefinitions(daedalus::DaedalusVm& vm);
    ~SvmDefinitions();

    std::string_view find(std::string_view speech, int id);

  private:
    daedalus::DaedalusVm&                                    vm;
    std::vector<std::shared_ptr<daedalus::DaedalusInstance>> svm;
  };

