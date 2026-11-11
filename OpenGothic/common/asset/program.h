#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace asset {

// A symbol before class registration or execution. Data types retain the DAT codes:
// void=0, float=1, int=2, string=3, class=4, function=5, prototype=6, instance=7.
struct ProgramSymbol {
  std::string name;
  bool generated = false;
  uint32_t type = 0;
  uint32_t count = 0;
  uint32_t flags = 0;
  int32_t address = -1;
  int32_t parent = -1;
  int32_t class_offset = -1;
  uint32_t member_offset = uint32_t(-1);
  uint32_t class_size = uint32_t(-1);
  uint32_t return_type = 0;
  uint32_t file_index = 0;
  uint32_t line_start = 0;
  uint32_t line_count = 0;
  uint32_t char_start = 0;
  uint32_t char_count = 0;

  // Only nonmember INT, FLOAT and STRING symbols have source array values.
  std::vector<int32_t> int_values;
  std::vector<uint32_t> float_bits;
  std::vector<std::string> string_values;
  };

// Owned program data; each symbol's position is its original VM index.
struct ProgramRecord {
  uint8_t version = 0;
  std::vector<ProgramSymbol> symbols;
  std::vector<std::byte> bytecode;
  };

}
