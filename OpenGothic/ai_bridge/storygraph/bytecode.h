#pragma once

#include <cstdint>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace daedalus {
  class DaedalusScript;
  class DaedalusSymbol;
  }

namespace storygraph {

// What a script function touches, as seen in its own bytecode. Script functions it calls are listed,
// not followed. Writes are "may write": the bytecode is read as a whole, branches are not resolved.
struct FnSummary final {
  std::set<std::string> externals;   // engine functions called (NPC_KNOWSINFO, AI_OUTPUT, ...)
  std::set<std::string> calls;       // script functions called (B_GIVEPLAYERXP, C_BEKLAUEN, ...)
  std::set<std::string> reads;       // global variables read; constants are left out
  std::set<std::string> writes;      // global variables written
  std::set<std::string> members;     // class members used (C_NPC.AIVAR, C_NPC.GUILD, ...)
  std::set<std::string> memberWrites; // class members written (C_NPC.EXP, ...)
  std::set<std::string> instances;   // instances pushed (HERO, SELF, VLK_4201_WIRT, ...), dialogs left out
  std::set<std::string> infos;       // dialogs pushed, i.e. the arguments of Npc_KnowsInfo
  std::vector<uint32_t> choices;     // functions passed to Info_AddChoice
  uint32_t              outputs = 0; // AI_Output calls
  };

class Bytecode final {
  public:
    explicit Bytecode(const daedalus::DaedalusScript& script);

    FnSummary summarize(uint32_t fnSymbol) const;
    bool      isInfo(const daedalus::DaedalusSymbol& sym) const;
    // prints the instructions of a function to stdout; false if there is no such function
    bool      print(const std::string& fnName) const;

    // where the code block starting at `address` ends: the start of the next block
    uint32_t  codeEnd(uint32_t address) const;
    static bool hasCode(const daedalus::DaedalusSymbol& sym);
    // the script function whose code starts at `address`, nullptr if there is none
    const daedalus::DaedalusSymbol* functionAt(uint32_t address) const;

  private:

    const daedalus::DaedalusScript&          script;
    std::vector<uint32_t>                  starts;     // sorted start addresses of all code blocks
    std::unordered_map<uint32_t, uint32_t> fnByAddress;
    uint32_t                               infoClass = uint32_t(-1);
  };

}
