#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <utility>
#include <vector>

namespace NpcWorldData {

struct Globals {
  int32_t unsetChapter = 0;
  int32_t defaultChapter = 0;
  std::vector<std::pair<int32_t,uint32_t>> bindings;
  };

struct AttitudeRule {
  int32_t from = 0;
  int32_t attitude = 0;
  int32_t to = 0;
  };

std::optional<Globals> loadGlobals(const std::filesystem::path& base);
std::optional<std::vector<AttitudeRule>> loadAttitudes(const std::filesystem::path& base);
void write(const std::filesystem::path& base, const Globals& globals, const std::vector<AttitudeRule>& attitudes);

}
