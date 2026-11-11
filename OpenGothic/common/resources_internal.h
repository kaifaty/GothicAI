#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace ResourcesInternal {
std::string modelKey(std::string_view name);
std::string censusSourcePath(const std::filesystem::path& source);
const char* scriptDirectory(int language);
}
