#pragma once

#include "asset/activitydefinitions.h"

#include <filesystem>
#include <string_view>

namespace ActivityDefinitions {
// Reads definitions/<program SHA>/activities.json without a script VM.
Library read(const std::filesystem::path& base, std::string_view programSha256);
}
