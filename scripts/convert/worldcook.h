#pragma once

// Cooked binary chunks (.wcb, OpenGothic/common/worldchunkbinary.h), the set's index (chunks.wci,
// worldchunkindex.h) and its world cloth bundle (cloth.wcl, physics/worldclothbundle.h) from the converter:
//   cook-world <world dir> [--content <override world dir>] [--out <dir>]
//   verify-cooked-world <world dir> [--content <dir>] [--out <dir>]
//   time-world <world dir> [--content <dir>] [--out <dir>]
// The game's per-cell reader takes a cell from its .wcb beside the GLB (worldchunkcookedsource.h); time-world
// times it next to the GLB read of the same Near cells, the world cloth island search next to reading and
// applying the bundle, and static collision's input from the chunk records (physics/worldcollisionmesh.h: read,
// check and build). verify-cooked-world also compares the bundle with a fresh island search. The start of the
// game still reads every GLB (docs/landscape-plan.md, step Д3).

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
// A world of one city banner across a cell border written to dir and cooked beside its GLBs: the world cloth
// bundle holds the banner, and extract takes it with the island search's result. Throws on a difference.
void clothRoundTrip(const std::filesystem::path& dir);

}
