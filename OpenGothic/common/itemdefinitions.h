#pragma once

#include "world/records/itemrecord.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace ItemDefinitions {
struct Definition {
  ItemRecord record{};
  // One entry per function/owner field; empty names retain the record's 0/-1 sentinel.
  std::map<std::string,std::vector<std::string>> symbols;
  };
using DocumentArgument = std::variant<int32_t,std::string>;
struct DocumentCall {
  std::string function;
  std::vector<DocumentArgument> arguments; // the current document handle is implicit
  };
struct Document {
  std::string handle;
  bool map = false;
  std::vector<DocumentCall> calls;
  };
struct Library {
  std::map<std::string,Definition> items;
  std::map<std::string,std::string> uncaptured;
  std::map<std::string,Document> documents;
  };

// Reads definitions/<program SHA>/items.json. Errors name the file, item and field.
// This module knows records and symbol names, without a script VM.
Library read(const std::filesystem::path& base, std::string_view programSha256);
}
