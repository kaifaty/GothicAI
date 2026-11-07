#pragma once

#include "eval.h"
#include "exec.h"
#include "program.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace daedalus {
  class DaedalusVm;
  class DaedalusInstance;
  struct INpc;
  }

namespace storygraph {

class Bytecode;
class Snapshot;
struct Node;

// An engine that remembers what was given: items created and removed change what Npc_HasItems answers, so a
// dialog that takes gold and then counts it sees the gold. The worlds of the checks only answer questions;
// everything but the inventory goes to `world` as it is.
class Ledger final : public Externals {
  public:
    // `hasItems`: the engine's Npc_HasItems, to ask the world what an npc has
    Ledger(Externals& world, const daedalus::DaedalusSymbol* hasItems) : world(world), hasItemsFn(hasItems) {}
    Value call(const daedalus::DaedalusSymbol& fn, const std::vector<Value>& args) override;

    void  reset() { items.clear(); }
    // npc, item instance -> how much more (or less) it has than the world says
    std::map<std::pair<const daedalus::DaedalusInstance*,int32_t>,int32_t> items;

  private:
    int32_t has(const std::vector<Value>& npcAndItem);

    Externals&                    world;
    const daedalus::DaedalusSymbol* hasItemsFn = nullptr;
  };

// Step 1 of «Исполнение без VM» (docs/story-graph.md): a function's program (program.h) run by the Interpreter
// against the VM running the function, over the same engine and from the same state. Compared are every engine
// call at any depth (its arguments and answer, in order), the returned value, all script variables after the
// run and the members of the instances the function can reach. Script functions the program calls run in the
// VM on both sides, so a difference is an error of the program or of the Interpreter.
class ExecCheck final {
  public:
    enum class Outcome : uint8_t { Agree, Differ, NotProgram, VmFailed };
    struct Result final {
      Outcome     outcome = Outcome::Agree;
      std::string detail;
      size_t      calls = 0;  // engine calls compared
      };

    // the VM is bound to engine() by the caller, before any instance is built
    ExecCheck(daedalus::DaedalusVm& vm, Externals& world);
    ~ExecCheck();

    Externals&     engine();
    Ledger&        ledger() { return *book; }
    const Program& program(uint32_t fnSymbol);

    // the npc of an instance symbol in the world (its members are watched), nullptr if there is none
    std::function<std::shared_ptr<daedalus::DaedalusInstance>(uint32_t)> npcOf;
    // where the driver's VM exception handler leaves what went wrong; nullptr: exceptions are thrown
    std::string*   vmFault = nullptr;
    // when set, run() reports both sides' engine calls
    std::function<void(const std::string&)> trace;
    // --via-json: every program is written as JSON and read back before it runs, as the director's programs
    // reach the game (readProgram); one that reads back otherwise is not a program
    static inline bool viaJson = false;

    Result         run(uint32_t fnSymbol, const std::shared_ptr<daedalus::INpc>& self,
                       const std::shared_ptr<daedalus::INpc>& hero);

  private:
    struct Call final {
      std::string        fn;
      std::vector<Value> args;
      Value              ret;
      };
    class Recorder;
    struct State;

    const std::set<std::string>& reach(uint32_t fnSymbol);
    std::vector<std::shared_ptr<daedalus::DaedalusInstance>> watched(uint32_t fnSymbol,
                                                                   const std::shared_ptr<daedalus::INpc>& self,
                                                                   const std::shared_ptr<daedalus::INpc>& hero);
    const std::vector<daedalus::DaedalusSymbol*>& membersOf(const daedalus::DaedalusInstance& inst);
    void           take(State& s, const std::vector<std::shared_ptr<daedalus::DaedalusInstance>>& insts);
    void           restore(State& s);
    std::string    text(const Call& c) const;

    daedalus::DaedalusVm&                                       vm;
    std::unique_ptr<Ledger>                                   book;
    std::unique_ptr<Recorder>                                 recorder;
    std::unique_ptr<Bytecode>                                 code;
    std::unique_ptr<Interpreter>                              interp;
    std::unordered_map<uint32_t,Program>                      programs;
    std::unordered_map<uint32_t,std::set<std::string>>        reaches;
    std::set<uint32_t>                                        busy;
    std::unordered_map<uint32_t,std::vector<daedalus::DaedalusSymbol*>> members;  // class symbol -> members
    std::unique_ptr<State>                                    before, afterVm, afterProgram;
  };

// tallies of the check, per function run
struct ExecTotals final {
  size_t runs = 0, calls = 0, agree = 0, differ = 0, notProgram = 0, vmFailed = 0;
  std::map<std::string,size_t>                     notProgramWhy;  // per function, not per run
  std::set<uint32_t>                               notPrograms;
  std::vector<std::pair<std::string,std::string>>  diffs;          // function, what differs

  void add(const std::string& fn, uint32_t fnSymbol, const ExecCheck::Result& r);
  void merge(const ExecTotals& o);
  void print(const char* what, bool verbose) const;
  };

// the functions a node runs: a dialog's condition and information, or the choice's function
std::vector<uint32_t> nodeFunctions(daedalus::DaedalusVm& vm, const Node& n);

}
