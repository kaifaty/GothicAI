#pragma once

#include "world/records/spellrecord.h"

#include <map>
#include <memory>
#include <string>
#include <string_view>

namespace daedalus { class DaedalusVm; }
class GameplayDefinitions;

class SpellDefinitions final {
  public:
    explicit SpellDefinitions(daedalus::DaedalusVm& vm);
    explicit SpellDefinitions(const GameplayDefinitions& records);

    const SpellRecord& find(std::string_view instanceName) const;

  private:
    std::map<std::string,std::shared_ptr<const SpellRecord>,std::less<>> spl;
  };
