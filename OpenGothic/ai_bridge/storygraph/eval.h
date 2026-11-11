#pragma once

#include "expr.h"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace daedalus {
  class DaedalusVm;
  class DaedalusSymbol;
  class DaedalusInstance;
  }

namespace storygraph {

struct Helper;

// A value of the Daedalus VM.
struct Value final {
  enum Type : uint8_t { Int, Float, Str, Inst };
  Type                                       type = Int;
  int32_t                                    i    = 0;
  float                                      f    = 0;
  std::string                                s;
  std::shared_ptr<daedalus::DaedalusInstance>  inst;

  static Value integer(int32_t v);
  static Value real(float v);
  static Value str(std::string v);
  static Value instance(std::shared_ptr<daedalus::DaedalusInstance> v);

  // readable, for traces: 5, 1.5, "text", BAU_950_LOBART, null
  std::string text(const daedalus::DaedalusVm& vm) const;
  };

// Engine functions. Arguments come converted to the parameter types of `fn`: int and func as Int,
// instances as Inst (maybe null), strings as Str.
class Externals {
  public:
    virtual ~Externals() = default;
    virtual Value call(const daedalus::DaedalusSymbol& fn, const std::vector<Value>& args) = 0;
  };

// Sends every engine function the VM calls to `ext`, so the VM and the Evaluator see the same engine.
void bindExternals(daedalus::DaedalusVm& vm, Externals& ext);

// Calls a script function or an engine function with these arguments, the way a script does: the values must
// fit the parameter types (int and func: Int, instance: Inst, string: Str). nullopt and `error` if they don't.
std::optional<Value> callFunction(daedalus::DaedalusVm& vm, const daedalus::DaedalusSymbol& fn,
                                  const std::vector<Value>& args, std::string& error);

// The engine functions registered with the VM (in the game: GameScript's), for the Evaluator.
class VmExternals final : public Externals {
  public:
    explicit VmExternals(daedalus::DaedalusVm& vm) : vm(vm) {}
    Value call(const daedalus::DaedalusSymbol& fn, const std::vector<Value>& args) override;

  private:
    daedalus::DaedalusVm& vm;
  };

// The member symbol of an instance's class (C_NPC.AIVAR for an npc and "AIVAR"), nullptr if there is none.
daedalus::DaedalusSymbol* member(daedalus::DaedalusVm& vm, const daedalus::DaedalusInstance& inst, const std::string& name);

// Evaluates lifted expressions against the state of a VM: globals and instance members are read from its
// symbols, engine functions go to `ext`, script functions are evaluated through their lifted bodies.
// Nothing is written: the effects of conditions are not replayed.
class Evaluator final {
  public:
    Evaluator(daedalus::DaedalusVm& vm, const std::map<std::string,Helper>& helpers, Externals& ext);

    // nullopt: the value is not known (an unknown part, a helper that was not lifted, a bad access); see why()
    std::optional<Value> eval(const ExprP& e);
    const std::string&   why() const { return error; }

  private:
    std::optional<Value> eval(const Expr& e, const std::vector<Value>& params, uint32_t depth);
    std::optional<Value> fail(std::string why);
    std::optional<Value> var(const Expr& e);
    std::optional<Value> field(const Expr& e, const Value& inst);
    std::optional<Value> call(const Expr& e, const std::vector<Value>& params, uint32_t depth);
    std::optional<Value> helper(const Expr& e, const std::vector<Value>& params, uint32_t depth);
    std::optional<Value> op(const Expr& e, const std::vector<Value>& params, uint32_t depth);

    daedalus::DaedalusVm&                  vm;
    const std::map<std::string,Helper>&  helpers;
    Externals&                           ext;
    std::string                          error;
  };

}
