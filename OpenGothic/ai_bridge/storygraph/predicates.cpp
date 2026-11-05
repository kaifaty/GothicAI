#include "predicates.h"

#include "lifter.h"
#include "vocab.h"

#include "daedalus/script.h"

#include <vector>

using namespace storygraph;

Predicates::Predicates(const daedalus::DaedalusScript& script, const Lifter& lifter)
  :script(script), lifter(lifter) {
  }

const ExprP& Predicates::condition(uint32_t fnSymbol) {
  auto it = conditions.find(fnSymbol);
  if(it!=conditions.end())
    return it->second;
  auto& c = conditions[fnSymbol];
  c = Lifter::condition(lifter.lift(fnSymbol));
  whyNotLifted(c);  // lifts the helpers it calls
  return c;
  }

const Helper& Predicates::helper(const std::string& fn) {
  auto it = lifted.find(fn);
  if(it!=lifted.end())
    return it->second;
  Helper h;
  h.name = fn;
  auto* sym = script.find_symbol_by_name(fn);
  if(sym==nullptr) {
    h.body      = ex::unknown("no function " + fn);
    h.notLifted = "no such function";
    return lifted[fn] = h;
    }
  h.params = sym->count();
  // a placeholder while the body is lifted, so recursion ends
  lifted[fn]  = Helper{fn, h.params, ex::unknown("recursive"), "recursive"};
  h.body      = Lifter::value(lifter.lift(sym->index()));
  h.notLifted = whyNotLifted(h.body);
  return lifted[fn] = h;
  }

std::string Predicates::whyNotLifted(const ExprP& e) {
  std::string r;
  std::vector<std::string> fns;
  ex::walk(e, [&](const Expr& x) {
    if(!r.empty())
      return;
    if(x.kind==Expr::Unknown)
      r = "unknown value (" + x.text + ")";
    else if(x.kind==Expr::Local)
      r = "local variable";
    else if(x.kind==Expr::Call && !vocab::PredicateExternals.count(x.name) && !vocab::PureExternals.count(x.name))
      r = "engine call " + x.name;
    else if(x.kind==Expr::Fn)
      fns.push_back(x.name);
    });
  if(!r.empty())
    return r;
  for(auto& f : fns) {
    auto& h = helper(f);
    if(!h.notLifted.empty())
      return "helper " + f + ": " + h.notLifted;
    }
  return {};
  }
