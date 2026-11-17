#pragma once

#include "asset/program.h"
#include <filesystem>
#include <string>

namespace NpcSnapshot {
struct Program {
  asset::ProgramRecord data;
  std::string name, sha256;
  };
Program load(const std::filesystem::path& base);
// No installation, world or base writes. Incomplete entries are retained with their reason.
void write(const Program& program, const std::filesystem::path& output);
// Reads records, restores native instances, and compares with fresh constructor executions.
// Returns the number of differences and prints their JSON paths to stderr.
int verify(const Program& program, const std::filesystem::path& input);
struct ItemCounts {
  size_t total = 0, captured = 0;
  };
// The item-only definitions section used by base import, independent of the D1 corpus.
ItemCounts writeItems(const Program& program, const std::filesystem::path& file);
int verifyItems(const Program& program, const std::filesystem::path& file);
}
