#pragma once

#include "records/definitions.h"

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace SfxDefinitionData {
inline constexpr std::string_view stockProgram = "3aa84956cff4df65b5a51693ae8f30ba706dfbe34f3d0d782ed4c7251f5acd78";
inline constexpr std::size_t stockPresetCount = 1770;
using Presets = std::vector<std::pair<std::string,Def::SoundEffect>>;

std::filesystem::path section(const std::filesystem::path& base, std::string_view program);
Presets read(const std::filesystem::path& base, std::string_view program);
void write(const std::filesystem::path& base, std::string_view program, const Presets& presets);
}
