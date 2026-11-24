#pragma once

#include <cstddef>
#include <string>

struct AiState final {
  size_t funcIni = 0;
  size_t funcLoop = 0;
  size_t funcEnd = 0;
  std::string mname;

  const char* name() const { return mname.c_str(); }
  };
