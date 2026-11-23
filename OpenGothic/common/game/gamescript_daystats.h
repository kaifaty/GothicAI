#pragma once

#include "daedalus/vm.h"
#include "routinedefinitions.h"
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

// The deterministic counters are independent of world data, for the offline contract.
class DayScriptStats {
  public:
    struct Phase {
      uint64_t engine=0, records=0, nested=0, instructions=0, random=0;
      std::map<std::string,uint64_t> externals;
      };
    struct State {
      uint64_t rows=0;
      std::map<std::string,Phase> phases;
      };
    explicit DayScriptStats(const RoutineDefinitions::Library& routines);
    ~DayScriptStats();
    static std::shared_ptr<DayScriptStats> attach(daedalus::DaedalusVm& vm, const RoutineDefinitions::Library& routines);
    static void trace(daedalus::DaedalusVm& vm, const char* program);
    void stop(daedalus::DaedalusVm& vm);
    void begin(std::string_view symbol);
    void end();
    void record();
    void random(bool script, bool actor);
    std::string json() const;
    std::string source(bool script, bool actor) const;
    static std::pair<std::string,std::string> statePhase(std::string_view symbol);

  private:
    enum class RootCall { Pending, Observed, Record };
    struct Context { std::string state, phase; RootCall root=RootCall::Pending; };
    struct Frame { std::string name; uint32_t address; uint64_t* instructions=nullptr; };
    void call(const daedalus::DaedalusSymbol& symbol, const daedalus::DaedalusVm& vm);
    void instruction(const daedalus::DaedalusInstruction& instruction, uint32_t pc);
    void external(const daedalus::DaedalusSymbol& symbol);
    Phase* phase();
    std::string family(std::string_view name) const;
    bool active=true;
    uint64_t callObserver=0;
    uint64_t calls=0, rolls=0, unattributedInstructions=0;
    std::map<std::string,uint64_t> symbols, sources, instructionsBySymbol;
    std::map<std::string,State> states;
    std::map<std::string,std::string> creationKinds;
    std::set<std::string> perceptions, dialogs;
    std::vector<Context> contexts;
    std::vector<Frame> stack;
    std::string traceProgram;
  };
