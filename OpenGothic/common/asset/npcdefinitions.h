#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <variant>
#include <vector>

namespace NpcDefinitions {
struct Value {
  enum class Kind { Int, Float, String, Symbol, Self, Null };
  Kind kind = Kind::Int;
  int32_t integer = 0;
  float real = 0;
  std::string text;
  bool operator==(const Value&) const = default;
  };
struct Field {
  std::string binding;
  uint16_t index = 0;
  Value value;
  bool operator==(const Field&) const = default;
  };
struct Call {
  std::string function;
  std::vector<Value> args;
  bool deferred = false;
  bool operator==(const Call&) const = default;
  };
using Step = std::variant<Field,Call>;
struct Definition {
  std::vector<Step> steps;
  bool operator==(const Definition&) const = default;
  };
struct Library {
  std::map<std::string,Definition> npcs;
  std::map<std::string,std::string> uncaptured;
  };
}
