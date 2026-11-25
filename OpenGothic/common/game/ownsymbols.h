#pragma once

#include "game/definitions/scriptvars.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace daedalus { class DaedalusVm; class DaedalusSymbol; }
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

    // a story variable, its value 0; nullptr and why not
    daedalus::DaedalusSymbol* addInt(daedalus::DaedalusVm& vm, std::string_view name, std::string& why);
    // an npc instance made like the script's npc `like`, to be put in the world by its index; nullptr and why not
    daedalus::DaedalusSymbol* addNpc(daedalus::DaedalusVm& vm, std::string_view name, std::string_view like, std::string& why);
    daedalus::DaedalusSymbol* addNpc(daedalus::DaedalusVm& vm, std::string_view name, const daedalus::DaedalusSymbol& prototype,
                                    std::string& why);

    const std::vector<Entry>& entries() const { return list; }
    const Entry*              find(std::string_view name) const;
    const Entry*              find(uint32_t index) const;
    daedalus::DaedalusSymbol* binding(std::string_view name) const;
    std::optional<int32_t>    intValue(std::string_view name) const;
    bool                      setInt(std::string_view name, int32_t value);
    // the script's instance an npc of ours is made like; nullptr for any other symbol
    daedalus::DaedalusSymbol*   likeOf(daedalus::DaedalusVm& vm, uint32_t index);

    void save(SaveArchive& fout) const;
    bool load(SaveArchive& fin);
    bool load(daedalus::DaedalusVm& vm, SaveArchive& fin);

  private:
    const Entry* addNative(Entry entry, std::string& why);
    bool loadDeclarations(SaveArchive& fin, daedalus::DaedalusVm* vm);
    daedalus::DaedalusSymbol* add(daedalus::DaedalusVm& vm, const Entry& e, std::string& why,
                                 const daedalus::DaedalusSymbol* prototype = nullptr);

    ScriptVars::State& state;
    const NativeBaseCatalog::Selection* catalog = nullptr;
    std::vector<Entry> list;
    std::map<std::string,daedalus::DaedalusSymbol*,std::less<>> bindings;
  };
