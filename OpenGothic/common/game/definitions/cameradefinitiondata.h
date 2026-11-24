#pragma once

#include "records/definitions.h"

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace CameraDefinitionData {
inline constexpr std::string_view stockProgram = "c7ad6c0a9b9aa48b509a783102b89a0cff7c716c3aa0a010bf7e7b33b00b5d18";
inline constexpr std::size_t stockPresetCount = 33;
using Presets = std::vector<std::pair<std::string,Def::Camera>>;

std::filesystem::path section(const std::filesystem::path& base, std::string_view program);
Presets read(const std::filesystem::path& base, std::string_view program);
void write(const std::filesystem::path& base, std::string_view program, const Presets& presets);
}
