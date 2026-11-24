#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

class SaveArchive;

namespace ScriptVars {

// The values and reference kinds in the game/daedalus entry.
enum class ReferenceKind : uint8_t {
  None          = 0,
  Npc           = 1,
  WorldItem     = 2,
  InventoryItem = 3,
  };

struct Reference {
  ReferenceKind kind   = ReferenceKind::None;
  uint32_t      object = uint32_t(-1);
  uint32_t      item   = uint32_t(-1);
  };

using Value = std::variant<std::monostate,std::vector<int32_t>,std::vector<float>,std::vector<std::string>,Reference>;

struct Row {
  std::string name;
  Value       value;
  };

Row  readRow(SaveArchive& in);
void writeRow(SaveArchive& out, const Row& row);

// The game/symbols entry is loaded before world objects resolve their symbol IDs.
inline constexpr std::string_view SymbolEntry = "game/symbols";
inline constexpr uint8_t          SymbolFormat = 1;

enum class SymbolKind : uint8_t {
  Int = 0,
  Npc = 1,
  };

struct SymbolDefinition {
  SymbolKind  kind  = SymbolKind::Int;
  std::string name;
  std::string like;       // The base NPC instance for SymbolKind::Npc.
  uint32_t    index = 0;
  };

struct SymbolHeader {
  uint8_t  format = 0;
  uint32_t count  = 0;
  };

SymbolHeader     readSymbolHeader(SaveArchive& in);
SymbolDefinition readSymbolDefinition(SaveArchive& in);
void             writeSymbolDefinitions(SaveArchive& out, std::span<const SymbolDefinition> symbols);

} // namespace ScriptVars
