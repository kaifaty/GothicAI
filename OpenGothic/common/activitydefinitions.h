#pragma once

#include "asset/activitydefinitions.h"

#include <rapidjson/fwd.h>

#include <filesystem>
#include <string_view>

namespace ActivityDefinitions {
// Reads definitions/<program SHA>/activities.json without a script VM.
Library read(const std::filesystem::path& base, std::string_view programSha256);
Library readJson(const rapidjson::Value& doc, const std::filesystem::path& file, std::string_view programSha256);
}
