#pragma once

#include <cstddef>

struct AiState final {
  size_t funcIni = 0;
  size_t funcLoop = 0;
  size_t funcEnd = 0;
  const char* mname = "";

  const char* name() const { return mname; }
  };
