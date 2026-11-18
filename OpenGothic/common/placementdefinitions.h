#pragma once

#include "asset/placementdefinitions.h"

#include <filesystem>
#include <string_view>

namespace PlacementDefinitions {
// Reads definitions/<program SHA>/placement.json without a script VM.
Library read(const std::filesystem::path& base, std::string_view programSha256);
}
