#pragma once

#include "game/definitions/scriptvars.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace daedalus { class DaedalusVm; class DaedalusSymbol; }

// Script symbols of our own, not in the compiled scripts: the AI director's story variables (DIR_Q1) and its
// npc's (DIR_NPC_1: an instance of a script npc's class, made by its code, with a symbol of its own). The VM
// has them as the scripts' own ones, so conditions, programs, Hlp_GetNpc and saves see no difference.
//
// The engine saves an npc with the index of its symbol, and a save's world loads before the variables. So a
// save keeps them in an entry of their own, and a GameSession made from a save makes them again in the same
// order before its world loads: every one gets the index it had, and the npc's of the save find theirs.
class OwnSymbols final {
  public:
    using Kind  = ScriptVars::SymbolKind;
    using Entry = ScriptVars::SymbolDefinition;

    static constexpr std::string_view SaveEntry = ScriptVars::SymbolEntry;
    static constexpr std::string_view Prefix    = "DIR_";  // every name of ours

    OwnSymbols() = default;
    OwnSymbols(const OwnSymbols&) = delete;
    OwnSymbols& operator=(const OwnSymbols&) = delete;

    // a story variable, its value 0; nullptr and why not
    daedalus::DaedalusSymbol* addInt(daedalus::DaedalusVm& vm, std::string_view name, std::string& why);
    // an npc instance made like the script's npc `like`, to be put in the world by its index; nullptr and why not
    daedalus::DaedalusSymbol* addNpc(daedalus::DaedalusVm& vm, std::string_view name, std::string_view like, std::string& why);

    const std::vector<Entry>& entries() const { return list; }
    const Entry*              find(std::string_view name) const;
    const Entry*              find(uint32_t index) const;
    std::optional<int32_t>    intValue(std::string_view name) const;
    bool                      setInt(std::string_view name, int32_t value);
    // the script's instance an npc of ours is made like; nullptr for any other symbol
    daedalus::DaedalusSymbol*   likeOf(daedalus::DaedalusVm& vm, uint32_t index);

    void save(SaveArchive& fout) const;
    // makes them again in the order of the save: false if one did not get the index it had (the scripts are
    // not those of the save, its npc's would take wrong symbols)
    bool load(daedalus::DaedalusVm& vm, SaveArchive& fin);

  private:
    std::string             whyNot(const daedalus::DaedalusVm& vm, const std::string& name) const;
    daedalus::DaedalusSymbol* add(daedalus::DaedalusVm& vm, const Entry& e, std::string& why);

    std::vector<Entry> list;
    // A symbol keeps a reference to its value across additions to this table.
    std::map<std::string,int32_t,std::less<>> integers;
  };
