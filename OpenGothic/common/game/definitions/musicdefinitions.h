#pragma once

#include "musicdefinitiondata.h"

#include <memory>

namespace daedalus {
class DaedalusVm;
}

class MusicDefinitions final {
  public:
    MusicDefinitions();
    ~MusicDefinitions();

    const Def::MusicTheme* operator[](std::string_view name) const;

  private:
    std::unique_ptr<daedalus::DaedalusVm> vm;
    MusicDefinitionData::Themes          themes;
  };
