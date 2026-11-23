#pragma once

#include "menurecord.h"

#include <filesystem>
#include <map>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace MenuDefinitions {

inline constexpr std::string_view stockProgram = "a0f9f1552dbdd5e83587e5c1031b3ae5f734bb438dcd2bbb9aa7e45c045ce8a7";

struct Action {
  enum class Kind { Video, ApplyVideoOptions, ApplyPerformanceOptions };

  Kind        kind;
  std::string video;

  bool operator==(const Action&) const = default;
};

struct Catalog {
  std::optional<std::pair<int32_t,int32_t>>    infoPosition;
  std::map<std::string,Menu,std::less<>>        menus;
  std::map<std::string,MenuItem,std::less<>>    items;
  std::map<int32_t,std::vector<Action>>        actions;

  bool operator==(const Catalog&) const = default;
};

std::filesystem::path section(const std::filesystem::path& base, std::string_view program);
Catalog read(const std::filesystem::path& base, std::string_view program);
void write(const std::filesystem::path& base, std::string_view program, const Catalog& catalog);

}
