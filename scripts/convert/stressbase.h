#pragma once

// A heavier copy of a ready base for stress measurements
// (reports/performance/2026-11-10-content-stress/README.md): every world
// triangle split in four at its edge midpoints, the look unchanged.
// Chunks are read by the game's reader and written by the importer's writer;
// the base identity stays, only base-files.json and its binding are rebuilt.

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "asset/worldmesh.h"

namespace StressBase {

// One level: triangle t becomes 4t..4t+3 with the material, flags, sector and
// lightmap of t. A midpoint is shared by every triangle that uses the same
// vertex (feature) pair; its attributes are the average of the two ends.
asset::WorldMesh subdivide(const asset::WorldMesh& mesh);

// stress-base / verify-stress-base: the exit code, or nothing for other commands.
std::optional<int> run(const std::vector<std::string>& args);

extern const char* const usage;

// Synthetic split: two triangles with a shared edge. Throws on failure.
void selfTest(const std::filesystem::path& dir);

}
