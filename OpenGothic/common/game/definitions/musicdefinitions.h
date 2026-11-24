#pragma once

#include "musicdefinitiondata.h"

class MusicDefinitions final {
  public:
    MusicDefinitions();

    const Def::MusicTheme* operator[](std::string_view name) const;

  private:
    MusicDefinitionData::Themes themes;
  };
