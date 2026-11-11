#pragma once

#include "eval.h"
#include "program.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace daedalus {
  class DaedalusVm;
  }

namespace storygraph {

// Runs programs (program.h) the way the VM runs their functions: variables and members are read and written in
// the VM's symbols, engine functions go to `ext`, script functions a program calls run in the VM. self, other
// and hero are what the caller set, as for the VM.
//
// Like the VM, a variable is read when its value is popped, after the operands pushed later are computed. A
// script function that returns a variable returns it this way too: `f() + f()` with `return local;` reads the
// local twice after both calls (B_GetLearnCostTalent: the original game prices "all thief talents" so).
class Interpreter final {
  public:
    Interpreter(daedalus::DaedalusVm& vm, Externals& ext) : vm(vm), ext(ext) {}

    struct Result final {
      bool        ok = true;
      std::string error;     // why it stopped; the steps before it are done
      Value       returned;  // a function that returns a value: what it returned
      };
    Result run(const Program& p, const std::vector<Value>& args = {});

    // a line of its own (Step::Say with text): the speaker's symbol and the text in the game codepage -> the id
    // of a line AI_Output plays; empty if it can't be had. Original lines need nothing.
    std::function<std::string(const std::string& npc, const std::string& text)> ownLine;
    // a Choice or ClearChoices step: the dialog that runs offers it or clears its choices; false if there is no
    // dialog of the director to take it
    std::function<bool(const Step& s)> choices;

  private:
    enum class Flow : uint8_t { Next, Return, Fail };
    // A value on the VM's stack: a computed value, or a variable or member it reads when the value is popped.
    struct Item final {
      Value                                      value;
      daedalus::DaedalusSymbol*                    ref = nullptr;
      uint16_t                                   index = 0;
      std::shared_ptr<daedalus::DaedalusInstance>  context;  // a member's instance, taken when it was pushed
      const Expr*                                field = nullptr;  // a member of no instance: read as 0 or ""
      };

    Flow                 steps(const std::vector<Step>& s);
    bool                 store(const Step& s);
    bool                 say(const Step& s);
    std::optional<Value> eval(const Expr& e);
    std::optional<Item>  item(const Expr& e);
    std::optional<Value> read(const Item& it);
    std::optional<Item>  call(const Expr& e);
    std::optional<Item>  op(const Expr& e);
    std::optional<Value> fail(std::string why);
    std::nullopt_t       failed(std::string why) { fail(std::move(why)); return std::nullopt; }

    daedalus::DaedalusVm&       vm;
    Externals&                ext;
    const std::vector<Value>* params = nullptr;
    Value                     returned;
    std::string               error;
  };

}
