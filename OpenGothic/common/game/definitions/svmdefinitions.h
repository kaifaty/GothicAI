#pragma once

#include "world/records/recordinstance.h"

#include <memory>
#include <string_view>
#include <vector>

namespace daedalus {
class DaedalusVm;
}
class GameplayDefinitions;

class SvmDefinitions final {
  public:
    explicit SvmDefinitions(daedalus::DaedalusVm& vm);
    explicit SvmDefinitions(std::shared_ptr<const GameplayDefinitions> records);
    ~SvmDefinitions();

    std::string_view find(std::string_view speech, int id);

  private:
    daedalus::DaedalusVm*                                     vm = nullptr;
    std::shared_ptr<const GameplayDefinitions>                records;
    std::vector<std::shared_ptr<daedalus::DaedalusInstance>>   svm;
  };
