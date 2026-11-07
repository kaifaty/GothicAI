#pragma once

#include "eval.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace daedalus {
  class DaedalusVm;
  class DaedalusSymbol;
  struct INpc;
  }

namespace storygraph {

class Bytecode;
class Lifter;
class Snapshot;
struct Lifted;
struct Model;
struct Node;

// The check of effects (docs/story-graph.md, after stage 2): what a dialog's function does when the VM runs
// it, against what the model says it does in the same state. Compared are
//   - the calls the function makes itself, in order, with their arguments: engine functions and script
//     functions alike (lines, items, journal, choices, B_* helpers, predicates in its conditions);
//   - the globals it leaves: the last store the model says runs, for each variable, and no write the model
//     does not explain (a store or a script function it calls that may write the variable).
// Both sides ask the same engine (`world`), so a difference is an error of the lifting.
class EffectCheck final {
  public:
    struct Call final {
      std::string        fn;
      std::vector<Value> args;
      };
    enum class Outcome : uint8_t { Agree, Differ, ModelUnknown, VmFailed, NoFunction };
    struct Result final {
      Outcome     outcome = Outcome::Agree;
      std::string detail;
      size_t      calls = 0;  // compared
      };

    // every script function of the VM is wrapped (calls are recorded at the dialog function's own level)
    EffectCheck(daedalus::DaedalusVm& vm, Externals& world);
    ~EffectCheck();

    // bind this to the VM in place of the world: it records the engine calls of the dialog's function
    Externals& engine();
    // runs the node's function with self = `self`, other = hero = `hero`, compares, and puts the script
    // variables and the members of both npc's back as they were
    Result     run(const Node& n, const std::shared_ptr<daedalus::INpc>& self, const std::shared_ptr<daedalus::INpc>& hero,
                   Evaluator& ev);
    // the node's script function (a dialog's information, or the choice's function), nullptr if none
    daedalus::DaedalusSymbol* function(const Node& n);

    // when set, run() reports what the model expects and what the VM did
    std::function<void(const std::string&)> trace;

  private:
    class Recorder;
    struct Store;

    void                          enter(daedalus::DaedalusVm& vm, daedalus::DaedalusSymbol& fn);
    const Lifted&                 lifted(uint32_t fn);
    const std::set<std::string>&  writes(const std::string& fn);
    std::string                   text(const Call& c) const;

    daedalus::DaedalusVm&                               vm;
    std::unique_ptr<Recorder>                         recorder;
    std::unique_ptr<Bytecode>                         code;
    std::unique_ptr<Lifter>                           lifter;
    std::unique_ptr<Snapshot>                         snap;
    std::unordered_map<uint32_t,std::unique_ptr<Lifted>> liftedFns;
    std::map<std::string,std::set<std::string>>       footprints;
    std::set<std::string>                             busy;
    std::unordered_map<std::string,daedalus::DaedalusSymbol*> functions;
    std::vector<daedalus::DaedalusSymbol*>              globals;   // int globals, compared after a run
    std::vector<daedalus::DaedalusSymbol*>              npcInts;   // int members of C_NPC, put back after a run

    bool                                              recording = false;
    uint32_t                                          level     = 0;  // script calls deep below the dialog function
    uint32_t                                          inlined   = 0;  // ... of those the lifter puts in its place
    std::vector<Call>                                 observed;
  };

}
