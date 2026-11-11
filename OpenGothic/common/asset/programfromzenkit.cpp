#include "fromzenkit.h"

#include <zenkit/DaedalusScript.hh>

asset::ProgramRecord asset::fromZenKit(const zenkit::DaedalusProgramRecord& source) {
  ProgramRecord result;
  result.version = source.version;
  result.symbols.reserve(source.symbols.size());
  for(const auto& symbol:source.symbols) {
    auto& out = result.symbols.emplace_back();
    out.name = symbol.name;
    out.generated = symbol.generated;
    out.type = uint32_t(symbol.type);
    out.count = symbol.count;
    out.flags = symbol.flags;
    out.address = symbol.address;
    out.parent = symbol.parent;
    out.class_offset = symbol.class_offset;
    out.member_offset = symbol.member_offset;
    out.class_size = symbol.class_size;
    out.return_type = uint32_t(symbol.return_type);
    out.file_index = symbol.file_index;
    out.line_start = symbol.line_start;
    out.line_count = symbol.line_count;
    out.char_start = symbol.char_start;
    out.char_count = symbol.char_count;
    out.int_values = symbol.int_values;
    out.float_bits = symbol.float_bits;
    out.string_values = symbol.string_values;
    }
  result.bytecode = source.bytecode;
  return result;
  }
