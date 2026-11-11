#pragma once

#include "daedalus/vm.h"
#include "records/definitions.h"

#include <memory>

class MusicDefinitions final {
  public:
    MusicDefinitions();
    ~MusicDefinitions();

    const Def::MusicTheme* operator[](std::string_view name) const;

  private:
    std::unique_ptr<daedalus::DaedalusVm>  vm;
    std::vector<std::pair<uint32_t,Def::MusicTheme>> themes;
  };
