#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>

namespace DeathTrophyDefinitions {
inline constexpr uint16_t given = 11; // AIV_DEATHINVGIVEN
inline constexpr uint16_t species = 43; // AIV_MM_REAL_ID

enum class Match : uint8_t { Instance, ActorSpecies, SelfSpecies };

struct Grant {
  int32_t item = 0, amount = 0;
  bool operator==(const Grant&) const = default;
  };

struct Chance {
  int32_t threshold = 0;
  bool exact = false;
  bool operator==(const Chance&) const = default;
  };

struct Trophy {
  Match match = Match::Instance;
  uint32_t symbol = 0;
  std::optional<Chance> chance;
  std::vector<Grant> grants;
  bool operator==(const Trophy&) const = default;
  };
using Trophies = std::vector<Trophy>;

struct Talent {
  uint16_t index = 0;
  Trophies trophies;
  bool operator==(const Talent&) const = default;
  };

struct Draw {
  int32_t bound = 0;
  uint32_t result = 0;
  std::vector<Trophies> choices;
  bool operator==(const Draw&) const = default;
  };

struct Catalog {
  std::array<Talent,14> taught;
  Trophies unconditional;
  std::array<Draw,3> randomized;
  bool operator==(const Catalog&) const = default;
  };

std::optional<Catalog> load(const std::filesystem::path& base);
void write(const std::filesystem::path& base, const Catalog& catalog);
}
