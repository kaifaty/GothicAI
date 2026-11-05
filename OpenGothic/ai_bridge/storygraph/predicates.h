#pragma once

#include "expr.h"

#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>

namespace daedalus {
  class DaedalusScript;
  }

namespace storygraph {

class Bytecode;
class Lifter;

// A script function used by conditions (C_BEKLAUEN, C_NPCISDOWN...), lifted into an expression over its parameters.
struct Helper final {
  std::string name;
  uint32_t    params = 0;
  ExprP       body;
  std::string notLifted;
  };

// Dialog conditions and the script functions they call, lifted on demand and kept. The converter lifts
// them all at once; the bridge lifts each one the first time a dialog menu needs it.
class Predicates final {
  public:
    Predicates(const daedalus::DaedalusScript& script, const Lifter& lifter);

    // a condition function as one boolean expression
    const ExprP&  condition(uint32_t fnSymbol);
    // why the expression is not fully known (an unknown part, an engine call with effects, a helper that
    // was not lifted); empty if it is
    std::string   whyNotLifted(const ExprP& e);
    const Helper& helper(const std::string& name);

    const std::map<std::string,Helper>& helpers() const { return lifted; }

  private:
    const daedalus::DaedalusScript&        script;
    const Lifter&                        lifter;
    std::unordered_map<uint32_t,ExprP>   conditions;
    std::map<std::string,Helper>         lifted;
  };

}
