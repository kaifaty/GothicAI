#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

class SaveArchive;
namespace daedalus { class DaedalusInstance; class DaedalusSymbol; }
namespace NativeBaseCatalog { struct InitialRow; }

namespace ScriptVars {

inline constexpr std::string_view StateEntry = "game/runtime";

// Values and object references for the prepared program state.
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
  uint32_t id;
  Value    value;
  };

Row  readRow(SaveArchive& in);
void writeRow(SaveArchive& out, const Row& row);

using Object = std::shared_ptr<daedalus::DaedalusInstance>;
template<class T>
struct Array {
  std::unique_ptr<T[]> storage;
  std::span<T>         values;
  };
using Slot = std::variant<Array<int32_t>,Array<float>,Array<std::string>,Object>;
struct SymbolDefinition;

class State final {
  public:
    void loadInitial(std::span<const NativeBaseCatalog::InitialRow> rows);
    bool declareDirector(const SymbolDefinition& definition);
    void bind(daedalus::DaedalusSymbol& symbol);
    void bind(std::string_view name, int32_t& value);
    void bind(std::string_view name, std::string& value);
    std::optional<uint32_t> id(std::string_view name) const;
    Slot* find(uint32_t id);
    const Slot* find(uint32_t id) const;
    Slot* find(std::string_view name);
    const Slot* find(std::string_view name) const;
    auto& slots() { return values; }
    const auto& slots() const { return values; }

  private:
    // The VM compatibility views may keep pointers to these cells for the session lifetime.
    std::map<uint32_t,Slot> values;
    std::map<std::string,uint32_t,std::less<>> names;
  };

// Director declarations are loaded before the world creates their NPCs.
inline constexpr std::string_view SymbolEntry = "game/director";
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
