#pragma once

#include <cstdlib>
#include <filesystem>

inline std::filesystem::path userHome() {
#ifdef _WIN32
  const auto* home=_wgetenv(L"USERPROFILE");
#else
  const auto* home=std::getenv("HOME");
#endif
  return home ? std::filesystem::path(home) : std::filesystem::path();
  }
