#pragma once

#include "daedalus/vm.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

class Serialize;

// Script symbols of our own, not in the compiled scripts: the AI director's story variables (DIR_Q1) and its
// npc's (DIR_NPC_1: an instance of a script npc's class, made by its code, with a symbol of its own). The VM
// has them as the scripts' own ones, so conditions, programs, Hlp_GetNpc and saves see no difference.
//
// The engine saves an npc with the index of its symbol, and a save's world loads before the variables. So a
// save keeps them in an entry of their own, and a GameSession made from a save makes them again in the same
// order before its world loads: every one gets the index it had, and the npc's of the save find theirs.
class OwnSymbols final {
  public:
    enum class Kind : uint8_t {
      Int = 0,  // a global integer
      Npc = 1,  // an npc instance made like a script's one
      };

    struct Entry final {
      Kind        kind  = Kind::Int;
      std::string name;
      std::string like;       // Npc: the script's instance it is made like
      uint32_t    index = 0;  // its symbol
      };

    static constexpr std::string_view SaveEntry = "game/symbols";
    static constexpr std::string_view Prefix    = "DIR_";  // every name of ours

    explicit OwnSymbols(daedalus::DaedalusVm& vm);

    // a story variable, its value 0; nullptr and why not
    daedalus::DaedalusSymbol* addInt(std::string_view name, std::string& why);
    // an npc instance made like the script's npc `like`, to be put in the world by its index; nullptr and why not
    daedalus::DaedalusSymbol* addNpc(std::string_view name, std::string_view like, std::string& why);

    const std::vector<Entry>& entries() const { return list; }
    const Entry*              find(std::string_view name) const;
    // the script's instance an npc of ours is made like; nullptr for any other symbol
    daedalus::DaedalusSymbol*   likeOf(uint32_t index);

    void save(Serialize& fout) const;
    // makes them again in the order of the save: false if one did not get the index it had (the scripts are
    // not those of the save, its npc's would take wrong symbols)
    bool load(Serialize& fin);

  private:
    std::string             whyNot(const std::string& name) const;
    daedalus::DaedalusSymbol* add(const Entry& e, std::string& why);

    daedalus::DaedalusVm& vm;
    std::vector<Entry>  list;
  };
