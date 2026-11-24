#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace NativeBaseCatalog {

inline constexpr int projectionVersion = 3;
inline constexpr int catalogVersion = 1;

struct State {
  uint32_t id = 0;
  std::string name;
  std::optional<uint32_t> loop, end;
  bool operator==(const State&) const = default;
  };

struct Selection {
  std::string fight;
  int language = -1;
  std::vector<State> states;
  };

std::filesystem::path section(const std::filesystem::path& base);
Selection read(const std::filesystem::path& base);
void write(const std::filesystem::path& base, const Selection& selection);

}
