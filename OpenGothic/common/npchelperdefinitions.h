#pragma once

#include "asset/npchelperdefinitions.h"

#include <filesystem>

namespace NpcHelperDefinitions {
// The game and import verifier use the same strict reader, without a script VM.
Library read(const std::filesystem::path& base, std::string_view programSha256);
Library readFile(const std::filesystem::path& file, std::string_view programSha256);
void write(const Library& library, const std::filesystem::path& file,
           std::string_view programName, std::string_view programSha256);
}
