#pragma once

#include "asset/npcdefinitions.h"

#include <filesystem>
#include <string_view>

namespace NpcDefinitions {
// Reads definitions/<program SHA>/npcs.json without a script VM.
Library read(const std::filesystem::path& base, std::string_view programSha256);
}
