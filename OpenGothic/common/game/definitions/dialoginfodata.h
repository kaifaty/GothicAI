#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace DialogInfoData {

struct Lesson {
  int32_t talent = 0;
  int32_t skill = 0;
  std::string title;
  };

struct Write {
  uint32_t member = 0;
  std::variant<int32_t,std::string,Lesson> value;
  };

using Rows = std::map<uint32_t,std::vector<Write>>;

std::filesystem::path section(const std::filesystem::path& base);
std::optional<Rows> load(const std::filesystem::path& base);
void write(const std::filesystem::path& base, const Rows& rows);

}
