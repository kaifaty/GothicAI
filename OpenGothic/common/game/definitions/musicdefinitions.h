#pragma once

#include <zenkit/DaedalusVm.hh>
#include "game/definitions/definitiontypes.h"

#include <memory>

class MusicDefinitions final {
  public:
    MusicDefinitions();
    ~MusicDefinitions();

    const Def::MusicTheme* operator[](std::string_view name) const;

  private:
    std::unique_ptr<zenkit::DaedalusVm>  vm;
    std::vector<std::shared_ptr<Def::MusicTheme>> themes;
  };
