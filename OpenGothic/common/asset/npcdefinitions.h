#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace NpcDefinitions {
inline constexpr auto TalentWriteProgram =
    std::string_view("a1918ecb7239b6f48cb98dda5e6ff7eb7ab8099085b3d7adec603c1431de641c");

inline bool writesPlayerTalents(std::string_view npc) {
  return npc=="PC_BANDIT" || npc=="PC_E3MAGE" || npc=="PC_E3PALADIN" || npc=="PC_ITEMFELLER" ||
         npc=="PC_MAGETEST" || npc=="PC_LEVELINSPEKTOR" || npc=="PC_ROCKEFELLER";
  }

inline bool isPlayerTalentArray(std::string_view symbol) {
  return symbol=="PLAYER_TALENT_SMITH" || symbol=="PLAYER_TALENT_ALCHEMY" ||
         symbol=="PLAYER_TALENT_TAKEANIMALTROPHY" || symbol=="PLAYER_TALENT_RUNES";
  }

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
struct GlobalIntWrite {
  std::string binding;
  uint16_t index = 0;
  int32_t value = 0;
  bool operator==(const GlobalIntWrite&) const = default;
  };
using Step = std::variant<Field,Call,GlobalIntWrite>;
struct Definition {
  std::vector<Step> steps;
  bool operator==(const Definition&) const = default;
  };
struct Library {
  std::map<std::string,Definition> npcs;
  std::map<std::string,std::string> uncaptured;
  std::string programSha256;
  };
}
