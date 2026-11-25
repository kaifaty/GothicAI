#include "predicates.h"

#include "vocab.h"

#include <vector>

using namespace storygraph;

std::string storygraph::checkPredicate(const ExprP& e, const FindHelper& findHelper) {
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
    const auto* h = findHelper(f);
    if(h==nullptr)
      return "helper " + f + ": not in the prepared story graph";
    if(!h->notLifted.empty())
      return "helper " + f + ": " + h->notLifted;
    }
  return {};
  }
