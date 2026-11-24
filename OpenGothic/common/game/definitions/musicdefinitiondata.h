#pragma once

#include "records/definitions.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace MusicDefinitionData {
inline constexpr std::string_view stockProgram = "21c45bc3c692a6abb32aade2769c1723fa2eaf9e28665592a47f33a2be5b3678";
inline constexpr std::size_t stockThemeCount = 74;

struct Theme {
  uint32_t id;
  std::string name;
  Def::MusicTheme value;
  };
using Themes = std::vector<Theme>;

std::filesystem::path section(const std::filesystem::path& base, std::string_view program);
Themes read(const std::filesystem::path& base, std::string_view program);
void write(const std::filesystem::path& base, std::string_view program, const Themes& themes);
}
