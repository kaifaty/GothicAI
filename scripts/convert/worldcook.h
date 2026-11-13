#pragma once

// Cooked binary chunks (.wcb, OpenGothic/common/worldchunkbinary.h) and the set's index (chunks.wci,
// worldchunkindex.h) from the converter:
//   cook-world <world dir> [--content <override world dir>] [--out <dir>]
//   verify-cooked-world <world dir> [--content <dir>] [--out <dir>]
//   time-world <world dir> [--content <dir>] [--out <dir>]
// Nothing in the game reads .wcb yet (docs/landscape-plan.md, step Д3).

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace WorldCook {

// The exit code, or nothing for other commands.
std::optional<int> run(const std::vector<std::string>& args);

extern const char* const usage;

// Cook dir (with overrideDir) into outDir, assemble and compare with readWorldChunks. Throws on a difference.
void roundTrip(const std::filesystem::path& dir, const std::filesystem::path& overrideDir,
               const std::filesystem::path& outDir);

}
