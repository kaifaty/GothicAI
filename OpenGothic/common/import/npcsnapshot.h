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
struct NpcCounts {
  size_t total = 0, captured = 0;
  };
// Assigned NPC member elements and ordered engine/deferred-script calls.
NpcCounts writeNpcs(const Program& program, const std::filesystem::path& file);
int verifyNpcs(const Program& program, const std::filesystem::path& file);
struct HelperCounts {
  size_t total = 0, captured = 0, situations = 0, deferred = 0;
  };
HelperCounts writeHelpers(const Program& program, const std::filesystem::path& file,
                         const std::filesystem::path& npcs);
int verifyHelpers(const Program& program, const std::filesystem::path& file,
                  const std::filesystem::path& npcs);
struct PlacementCounts {
  size_t total = 0, captured = 0, steps = 0, deferred = 0;
  };
// Roots are STARTUP_GLOBAL and STARTUP_<directory name> from worldsDirectory.
// Engine calls and constant global writes stay ordered; state-dependent helpers remain in the VM.
PlacementCounts writePlacements(const Program& program, const std::filesystem::path& file,
                               const std::filesystem::path& worldsDirectory);
int verifyPlacements(const Program& program, const std::filesystem::path& file,
                     const std::filesystem::path& worldsDirectory);
struct ActivityCounts {
  size_t total = 0, captured = 0, situations = 0;
  };
// States of the rows in `routines`; each captured when the template walk equals its three VM phases.
ActivityCounts writeActivities(const Program& program, const std::filesystem::path& file,
                               const std::filesystem::path& routines);
int verifyActivities(const Program& program, const std::filesystem::path& file,
                     const std::filesystem::path& routines);
struct RoutineCounts {
  size_t total = 0, captured = 0;
  };
// TA_Min rows in call order, verified independently of the D1 snapshot files.
RoutineCounts writeRoutines(const Program& program, const std::filesystem::path& file);
int verifyRoutines(const Program& program, const std::filesystem::path& file);
}
