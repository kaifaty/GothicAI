#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>

#include "game/gamescript.h"

#include "expr.h"
#include "program.h"

class Npc;

namespace storygraph {
  class Bytecode;
  class Externals;
  class Interpreter;
  class Lifter;
  class Predicates;
  }

// What the director's programs and conditions run on (docs/director.md, stage 3): the VM of the game session,
// its engine functions, the lifted script functions, the director's story variables, and the checks a program
// or a condition passes when the server sends it. The dialogs of the story graph (dialoggraph.h) and the
// triggers (storytriggers.h) run theirs here alike: in a dialog with the self and other the engine set, outside
// one with those the caller binds (Bind).
class StoryRuntime final {
  public:
    // ownLine: registers a runtime line (npc symbol, text in the game codepage) and returns its id
    explicit StoryRuntime(std::function<std::string(const std::string&,const std::string&)> ownLine);
    ~StoryRuntime();

    daedalus::DaedalusVm* vm() const;
    GameScript*         script() const;

    // the director's story variable `name` (DIR_..., op "vars"): made with `first` in the VM if it is missing,
    // one a save brought back keeps its value. Empty, or why it can't be
    std::string         addVar(const std::string& name, int32_t first);
    // why `name` can't be a variable of the director's in this VM; empty: it can
    static std::string  whyNotVar(GameScript& script, const std::string& name);
    size_t              varCount() const { return vars.size(); }
    // a global integer's value now; nullopt without a world or such a variable
    std::optional<int32_t> intOf(const std::string& name);
    bool                setInt(const std::string& name, int32_t value);

    // the expression may be evaluated: names exist, script functions it calls can be lifted
    std::string         checkExpr(const storygraph::ExprP& e);
    // the names an expression uses exist; empty: they do
    std::string         checkNames(daedalus::DaedalusVm& v, const storygraph::ExprP& e) const;
    // the program may run in place of `fn` (nullptr: a program of the director's, no parameters): names exist,
    // functions get as many arguments as they take, stores go into variables. `choices`: Choice and
    // ClearChoices steps may be offered (a dialog of the director's), else they are rejected
    std::string         checkProgram(daedalus::DaedalusVm& v, const storygraph::Program& p, const daedalus::DaedalusSymbol* fn,
                                     bool choices = true) const;

    // a condition, with self and other as they are now; nullopt: it could not be computed (logged once as `what`)
    std::optional<bool> test(const storygraph::ExprP& e, std::string_view what);
    // the interpreter of the VM's programs, made again for a new VM
    storygraph::Interpreter& interpreter(daedalus::DaedalusVm& v);
    // a Choice or ClearChoices step ran: the dialog of the director that runs takes it (dialoggraph.h); false: none
    std::function<bool(const storygraph::Step&)> choices;

    // lifting: for conditions, and for the script functions the dialogs run as programs
    void                    ensureLifter();
    storygraph::Bytecode&   bytecode();    // after ensureLifter
    storygraph::Predicates& predicates();  // after ensureLifter

    // self and other bound for a program or a condition outside a dialog; as they were afterwards. nullptr: none
    class Bind final {
      public:
        Bind(daedalus::DaedalusVm& v, Npc* self, Npc* other);
        Bind(const Bind&) = delete;
        ~Bind();

      private:
        daedalus::DaedalusSymbol*                   self  = nullptr;
        daedalus::DaedalusSymbol*                   other = nullptr;
        std::shared_ptr<daedalus::DaedalusInstance> prevSelf, prevOther;
      };

    // true the first time for `key`: what is logged once
    bool                once(const std::string& key) { return reported.insert(key).second; }
    // a new session or the server's patches anew: nothing of the old VM is kept
    void                clear();

  private:
    // the director's variables in this VM: made with their first value if missing
    void                ensureVars();

    std::function<std::string(const std::string&,const std::string&)> ownLine;

    std::map<std::string,int32_t>                vars;      // the director's, with their first values
    std::set<std::string>                        varsMade;  // of those, the ones in varsFor
    daedalus::DaedalusVm*                          varsFor = nullptr;

    daedalus::DaedalusVm*                          liftedFor = nullptr;
    std::unique_ptr<storygraph::Bytecode>        code;
    std::unique_ptr<storygraph::Lifter>          lifter;
    std::unique_ptr<storygraph::Predicates>      lifted;

    daedalus::DaedalusVm*                          runFor = nullptr;
    std::unique_ptr<storygraph::Externals>       engine;
    std::unique_ptr<storygraph::Interpreter>     interp;

    std::set<std::string>                        reported;
  };
