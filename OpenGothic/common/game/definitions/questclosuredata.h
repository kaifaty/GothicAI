#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string_view>
#include <variant>
#include <vector>

namespace QuestClosureData {

using Number = std::variant<int32_t,uint32_t>;

struct Row {
  uint32_t topic = 0;
  std::array<Number,3> values;
  bool operator==(const Row&) const = default;
  };

struct GuildRule {
  uint32_t before = 0;
  uint32_t topic = 0;
  uint32_t status = 0;
  std::array<uint32_t,3> excluded;
  bool operator==(const GuildRule&) const = default;
  };

struct Group {
  std::vector<Row> rows;
  std::optional<GuildRule> guildRule;
  bool operator==(const Group&) const = default;
  };

inline constexpr std::array names{
  "B_CLOSETOPICS_MATTES", "B_CLOSETOPICS_HOSHI", "B_CLOSETOPICS_YOLY"};
inline constexpr std::array counts{101u,18u,87u};
using Groups = std::array<Group,names.size()>;

std::filesystem::path section(const std::filesystem::path& base);
Groups read(const std::filesystem::path& base);
void write(const std::filesystem::path& base, const Groups& groups);

}
