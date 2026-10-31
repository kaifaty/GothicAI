#pragma once

#include "game/definitions/definitiontypes.h"

#include <unordered_map>

class SoundDefinitions final {
  public:
    SoundDefinitions();

    const Def::SoundEffect& operator [](std::string_view name) const;

  private:
    std::unordered_map<std::string, std::shared_ptr<Def::SoundEffect>> sfx;
  };

