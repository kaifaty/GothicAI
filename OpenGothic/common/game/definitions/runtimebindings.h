#pragma once

#include "asset/program.h"

#include <filesystem>
#include <span>
#include <string_view>

namespace RuntimeBindings {

std::filesystem::path path(const std::filesystem::path& base, std::string_view program);
std::string_view recipe();
asset::ProgramRecord read(const std::filesystem::path& base, std::string_view program);
void validate(const std::filesystem::path& base, std::string_view stockProgram);

// Build only the slots reachable by the native runtime and preserve their numeric identities.
std::vector<uint32_t> collect(const std::filesystem::path& base, const asset::ProgramRecord& source);
void write(const std::filesystem::path& base, std::string_view program,
           const asset::ProgramRecord& source, std::span<const uint32_t> ids);

}
