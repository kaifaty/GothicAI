#pragma once

#include "storygraph/program.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace StockStoryPrograms {

inline constexpr std::string_view stockProgram = "a1918ecb7239b6f48cb98dda5e6ff7eb7ab8099085b3d7adec603c1431de641c";

struct Entry {
  uint32_t id = 0;
  std::string name;
  uint32_t returnType = 0;
  std::vector<uint32_t> parameters;
  std::vector<uint32_t> calls;
  storygraph::Program program;
  };

struct Catalog {
  std::map<uint32_t,Entry> programs;
  std::map<std::string,uint32_t,std::less<>> symbols;
  std::map<std::string,int32_t,std::less<>> integers;
  std::map<std::string,std::string,std::less<>> strings;
  };

std::filesystem::path section(const std::filesystem::path& base);
Catalog read(const std::filesystem::path& base);
void write(const std::filesystem::path& base, const Catalog& catalog);

}
