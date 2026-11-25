#pragma once

#include "game/definitions/scriptvars.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace NativeBaseCatalog { struct Selection; }

// Director variables and NPC declarations. A save restores their IDs before it loads the world and values.
class OwnSymbols final {
  public:
    using Kind  = ScriptVars::SymbolKind;
    using Entry = ScriptVars::SymbolDefinition;

    static constexpr std::string_view SaveEntry = ScriptVars::SymbolEntry;
    static constexpr std::string_view Prefix    = "DIR_";  // every name of ours

    explicit OwnSymbols(ScriptVars::State& state) : state(state) {}
    OwnSymbols(const OwnSymbols&) = delete;
    OwnSymbols& operator=(const OwnSymbols&) = delete;

    void useNativeCatalog(const NativeBaseCatalog::Selection& source) { catalog = &source; }
    bool addInt(std::string_view name, std::string& why);
    std::optional<uint32_t> addNpc(std::string_view name, std::string_view like, std::string& why);
    ScriptVars::Object* nativeNpc(uint32_t index);

    const std::vector<Entry>& entries() const { return list; }
    const Entry*              find(std::string_view name) const;
    const Entry*              find(uint32_t index) const;
    std::optional<int32_t>    intValue(std::string_view name) const;
    bool                      setInt(std::string_view name, int32_t value);
    void save(SaveArchive& fout) const;
    bool load(SaveArchive& fin);

  private:
    const Entry* add(Entry entry, std::string& why);

    ScriptVars::State& state;
    const NativeBaseCatalog::Selection* catalog = nullptr;
    std::vector<Entry> list;
  };
