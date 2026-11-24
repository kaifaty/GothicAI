#pragma once

#include "expr.h"

#include <cstdint>
#include <functional>
#include <string>

namespace storygraph {

// A function used by conditions, represented as an expression over its parameters.
struct Helper final {
  std::string name;
  uint32_t    params = 0;
  ExprP       body;
  std::string notLifted;
  };

using FindHelper = std::function<const Helper*(const std::string&)>;

// An unknown value, an engine call with effects, or an unavailable helper makes a predicate unsafe to evaluate.
std::string checkPredicate(const ExprP& e, const FindHelper& findHelper);

}
