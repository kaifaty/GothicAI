#pragma once

#include <string>
#include <string_view>

namespace ProgramData { struct Index; struct Source; }

struct BaseProgramSelection {
  const ProgramData::Source& source;
  std::string_view           branch;
  };

BaseProgramSelection selectBaseProgram(const ProgramData::Index& index, std::string_view request, int language);
