#include "eval.h"

#include "model.h"

#include "daedalus/vm.h"

using namespace storygraph;
using daedalus::DaedalusDataType;

static constexpr uint32_t MaxDepth = 64;  // script functions calling each other

Value Value::integer(int32_t v) {
  Value r;
  r.i = v;
  return r;
  }

Value Value::real(float v) {
  Value r;
  r.type = Float;
  r.f    = v;
  return r;
  }

Value Value::str(std::string v) {
  Value r;
  r.type = Str;
  r.s    = std::move(v);
  return r;
  }

Value Value::instance(std::shared_ptr<daedalus::DaedalusInstance> v) {
  Value r;
  r.type = Inst;
  r.inst = std::move(v);
  return r;
  }

std::string Value::text(const daedalus::DaedalusVm& vm) const {
  switch(type) {
    case Int:   return std::to_string(i);
    case Float: return std::to_string(f);
    case Str:   return "\"" + s + "\"";
    case Inst: {
      if(inst==nullptr)
        return "null";
      auto* sym = vm.find_symbol_by_index(inst->symbol_index());
      return sym ? ident(sym->name()) : "?";
      }
    }
  return "?";
  }

// what the VM pushes for a function of this return type
static Value asReturned(DaedalusDataType t, const Value& v) {
  switch(t) {
    case DaedalusDataType::FLOAT:    return Value::real(v.type==Value::Float ? v.f : 0.f);
    case DaedalusDataType::STRING:   return Value::str(v.type==Value::Str ? v.s : std::string());
    case DaedalusDataType::INSTANCE: return Value::instance(v.type==Value::Inst ? v.inst : nullptr);
    default:                         return Value::integer(v.type==Value::Int ? v.i : 0);
    }
  }

void storygraph::bindExternals(daedalus::DaedalusVm& vm, Externals& ext) {
  vm.register_default_external_custom([&ext](daedalus::DaedalusVm& v, daedalus::DaedalusSymbol& fn) {
    auto params = v.find_parameters_for_function(&fn);
    std::vector<Value> args(params.size());
    for(size_t k=params.size(); k>0; --k) {
      switch(params[k-1].type()) {
        case DaedalusDataType::FLOAT:    args[k-1] = Value::real(v.pop_float());       break;
        case DaedalusDataType::STRING:   args[k-1] = Value::str(v.pop_string());       break;
        case DaedalusDataType::INSTANCE: args[k-1] = Value::instance(v.pop_instance()); break;
        default:                         args[k-1] = Value::integer(v.pop_int());      break;
        }
      }
    auto r = asReturned(fn.rtype(), ext.call(fn, args));
    if(!fn.has_return())
      return;
    switch(r.type) {
      case Value::Float: v.push_float(r.f);     break;
      case Value::Str:   v.push_string(r.s);    break;
      case Value::Inst:  v.push_instance(r.inst); break;
      case Value::Int:   v.push_int(r.i);       break;
      }
    });
  }

std::optional<Value> storygraph::callFunction(daedalus::DaedalusVm& vm, const daedalus::DaedalusSymbol& fn,
                                              const std::vector<Value>& args, std::string& error) {
  if(fn.type()!=DaedalusDataType::FUNCTION) {
    error = ident(fn.name()) + " is not a function";
    return std::nullopt;
    }
  auto params = vm.find_parameters_for_function(&fn);
  if(params.size()!=args.size()) {
    error = ident(fn.name()) + " takes " + std::to_string(params.size()) + " arguments";
    return std::nullopt;
    }
  for(size_t i=0; i<args.size(); ++i) {
    auto t    = params[i].type();
    auto want = t==DaedalusDataType::FLOAT    ? Value::Float :
                t==DaedalusDataType::STRING   ? Value::Str   :
                t==DaedalusDataType::INSTANCE ? Value::Inst  : Value::Int;
    if(args[i].type!=want) {
      error = "argument " + std::to_string(i) + " of " + ident(fn.name()) + " has the wrong type";
      return std::nullopt;
      }
    }
  for(auto& a : args)
    switch(a.type) {
      case Value::Int:   vm.push_int(a.i);         break;
      case Value::Float: vm.push_float(a.f);       break;
      case Value::Str:   vm.push_string(a.s);      break;
      case Value::Inst:  vm.push_instance(a.inst); break;
      }
  if(fn.is_external()) {
    if(!vm.unsafe_call_external(&fn)) {
      error = "no engine function " + ident(fn.name());
      return std::nullopt;
      }
    } else {
    vm.unsafe_call(&fn);
    }
  if(!fn.has_return())
    return Value::integer(0);
  switch(fn.rtype()) {
    case DaedalusDataType::FLOAT:    return Value::real(vm.pop_float());
    case DaedalusDataType::STRING:   return Value::str(vm.pop_string());
    case DaedalusDataType::INSTANCE: return Value::instance(vm.pop_instance());
    default:                         return Value::integer(vm.pop_int());
    }
  }

Value VmExternals::call(const daedalus::DaedalusSymbol& fn, const std::vector<Value>& args) {
  std::string error;
  auto r = callFunction(vm, fn, args, error);
  return r ? *r : asReturned(fn.rtype(), Value());
  }

daedalus::DaedalusSymbol* storygraph::member(daedalus::DaedalusVm& vm, const daedalus::DaedalusInstance& inst,
                                           const std::string& name) {
  auto* cls = vm.find_symbol_by_index(inst.symbol_index());
  while(cls!=nullptr && cls->type()!=DaedalusDataType::CLASS)
    cls = vm.find_symbol_by_index(cls->parent());
  return cls ? vm.find_symbol_by_name(cls->name() + "." + name) : nullptr;
  }

Evaluator::Evaluator(daedalus::DaedalusVm& vm, const std::map<std::string,Helper>& helpers, Externals& ext)
  :vm(vm), helpers(helpers), ext(ext) {
  }

std::optional<Value> Evaluator::eval(const ExprP& e) {
  error.clear();
  if(e==nullptr)
    return Value::integer(1);
  return eval(*e, {}, 0);
  }

std::optional<Value> Evaluator::fail(std::string why) {
  if(error.empty())
    error = std::move(why);
  return std::nullopt;
  }

static bool isInt(const std::optional<Value>& v) { return v && v->type==Value::Int; }

std::optional<Value> Evaluator::eval(const Expr& e, const std::vector<Value>& params, uint32_t depth) {
  switch(e.kind) {
    case Expr::Int:
      return Value::integer(e.value);
    case Expr::Str:
      return Value::str(e.text);
    case Expr::Var:
      return var(e);
    case Expr::Field: {
      auto inst = eval(*e.args[0], params, depth);
      if(!inst)
        return std::nullopt;
      if(inst->type!=Value::Inst)
        return fail("member of a non-instance: " + e.str());
      return field(e, *inst);
      }
    case Expr::Inst: {
      auto* sym = vm.find_symbol_by_name(e.name);
      if(sym==nullptr || sym->type()!=DaedalusDataType::INSTANCE)
        return fail("no instance " + ident(e.name));
      return Value::instance(sym->get_instance());
      }
    case Expr::Sym: {
      auto* sym = vm.find_symbol_by_name(e.name);
      if(sym==nullptr)
        return fail("no symbol " + ident(e.name));
      return Value::integer(int32_t(sym->index()));
      }
    case Expr::Param:
      if(e.index<0 || size_t(e.index)>=params.size())
        return fail("no parameter " + std::to_string(e.index));
      return params[size_t(e.index)];
    case Expr::Local:
      return fail("local " + ident(e.name));
    case Expr::Call:
      return call(e, params, depth);
    case Expr::Fn:
      return helper(e, params, depth);
    case Expr::Op:
      return op(e, params, depth);
    case Expr::Cases:
      for(size_t i=0; i+1<e.args.size(); i+=2) {
        auto g = eval(*e.args[i], params, depth);
        if(!isInt(g))
          return g ? fail("guard is not an int: " + e.args[i]->str()) : g;
        if(g->i!=0)
          return eval(*e.args[i+1], params, depth);
        }
      return Value::integer(0);
    case Expr::Unknown:
      return fail("unknown: " + e.text);
    }
  return fail("bad expression");
  }

std::optional<Value> Evaluator::var(const Expr& e) {
  // a static local not assigned on the path reads as `FN.NAME` or `FN.NAME[i]`
  std::string name  = e.name;
  int32_t     index = e.index;
  auto br = name.find('[');
  if(br!=std::string::npos && name.back()==']') {
    index = std::atoi(name.c_str()+br+1);
    name.resize(br);
    }
  auto* sym = vm.find_symbol_by_name(name);
  if(sym==nullptr)
    return fail("no variable " + ident(name));
  auto i = uint16_t(std::max(index, 0));
  if(i>=sym->count() && sym->type()!=DaedalusDataType::INSTANCE)
    return fail("index out of range: " + e.str());
  switch(sym->type()) {
    case DaedalusDataType::INT:
    case DaedalusDataType::FUNCTION: return Value::integer(sym->get_int(i));
    case DaedalusDataType::FLOAT:    return Value::real(sym->get_float(i));
    case DaedalusDataType::STRING:   return Value::str(sym->get_string(i));
    case DaedalusDataType::INSTANCE: return Value::instance(sym->get_instance());
    default:                         return fail("variable of an odd type: " + e.str());
    }
  }

std::optional<Value> Evaluator::field(const Expr& e, const Value& inst) {
  if(inst.inst==nullptr) {
    // the VM reads members of a null instance as 0 or "" (ALLOW_NULL_INSTANCE_ACCESS): which one, the
    // member says (its class is not known, the classes are tried)
    for(auto* cls : {"C_NPC.", "C_ITEM.", "C_INFO.", "C_FOCUS.", "C_MENU_ITEM."})
      if(auto* m = vm.find_symbol_by_name(cls + e.name); m!=nullptr && m->is_member())
        return m->type()==DaedalusDataType::STRING ? Value::str("") : Value::integer(0);
    return Value::integer(0);
    }
  auto* sym = member(vm, *inst.inst, e.name);
  if(sym==nullptr)
    return fail("no member " + e.str());
  auto i = uint16_t(std::max(e.index, 0));
  if(i>=sym->count())
    return fail("index out of range: " + e.str());
  try {
    switch(sym->type()) {
      case DaedalusDataType::INT:
      case DaedalusDataType::FUNCTION: return Value::integer(sym->get_int(i, inst.inst.get()));
      case DaedalusDataType::FLOAT:    return Value::real(sym->get_float(i, inst.inst.get()));
      case DaedalusDataType::STRING:   return Value::str(sym->get_string(i, inst.inst.get()));
      default:                         return fail("member of an odd type: " + e.str());
      }
    }
  catch(const std::exception& x) {
    return fail(std::string("member access: ") + x.what());
    }
  }

std::optional<Value> Evaluator::call(const Expr& e, const std::vector<Value>& params, uint32_t depth) {
  auto* fn = vm.find_symbol_by_name(e.name);
  if(fn==nullptr || !fn->is_external())
    return fail("no engine function " + ident(e.name));
  auto types = vm.find_parameters_for_function(fn);
  if(types.size()!=e.args.size())
    return fail("wrong argument count: " + e.str());
  std::vector<Value> args;
  for(size_t k=0; k<e.args.size(); ++k) {
    auto v = eval(*e.args[k], params, depth);
    if(!v)
      return std::nullopt;
    // what the VM pops for this parameter type
    auto t    = types[k].type();
    bool want = t==DaedalusDataType::FLOAT    ? v->type==Value::Float :
                t==DaedalusDataType::STRING   ? v->type==Value::Str   :
                t==DaedalusDataType::INSTANCE ? v->type==Value::Inst  : v->type==Value::Int;
    if(!want)
      return fail("argument " + std::to_string(k) + " of " + ident(e.name) + " has the wrong type");
    args.push_back(std::move(*v));
    }
  return asReturned(fn->rtype(), ext.call(*fn, args));
  }

std::optional<Value> Evaluator::helper(const Expr& e, const std::vector<Value>& params, uint32_t depth) {
  if(depth>=MaxDepth)
    return fail("script functions nested too deep");
  auto it = helpers.find(e.name);
  if(it==helpers.end() || !it->second.notLifted.empty())
    return fail("script function not lifted: " + ident(e.name));
  std::vector<Value> args;
  for(auto& a : e.args) {
    auto v = eval(*a, params, depth);
    if(!v)
      return std::nullopt;
    args.push_back(std::move(*v));
    }
  return eval(*it->second.body, args, depth+1);
  }

static bool compare(const std::string& o, int32_t a, int32_t b, int32_t& r) {
  if(o=="==") r = a==b;
  else if(o=="!=") r = a!=b;
  else if(o=="<")  r = a<b;
  else if(o==">")  r = a>b;
  else if(o=="<=") r = a<=b;
  else if(o==">=") r = a>=b;
  else return false;
  return true;
  }

std::optional<Value> Evaluator::op(const Expr& e, const std::vector<Value>& params, uint32_t depth) {
  auto& o = e.name;
  if(o=="&&" || o=="||") {
    // the result is known as soon as one term decides it, even if another one is not
    const bool isAnd   = (o=="&&");
    bool       unknown = false;
    for(auto& a : e.args) {
      auto v = eval(*a, params, depth);
      if(!isInt(v)) {
        if(v)
          fail("logic on a non-int: " + a->str());
        unknown = true;
        continue;
        }
      if((v->i!=0)!=isAnd) {
        error.clear();
        return Value::integer(isAnd ? 0 : 1);
        }
      }
    if(unknown)
      return std::nullopt;
    return Value::integer(isAnd ? 1 : 0);
    }

  std::vector<int32_t> v;
  for(auto& a : e.args) {
    auto x = eval(*a, params, depth);
    if(!x)
      return std::nullopt;
    if(x->type!=Value::Int)
      return fail("arithmetic on a non-int: " + e.str());
    v.push_back(x->i);
    }
  if(v.size()==1) {
    if(o=="!")   return Value::integer(v[0]==0);
    if(o=="neg") return Value::integer(int32_t(0u-uint32_t(v[0])));
    if(o=="~")   return Value::integer(~v[0]);
    return fail("unary " + o);
    }
  if(v.size()!=2)
    return fail("operator " + o + " with " + std::to_string(v.size()) + " arguments");
  // like the VM: the left operand is args[0], arithmetic wraps
  const int32_t a = v[0], b = v[1];
  const auto ua = uint32_t(a), ub = uint32_t(b);
  int32_t r = 0;
  if(compare(o, a, b, r)) return Value::integer(r);
  if(o=="+")  return Value::integer(int32_t(ua+ub));
  if(o=="-")  return Value::integer(int32_t(ua-ub));
  if(o=="*")  return Value::integer(int32_t(ua*ub));
  if(o=="&")  return Value::integer(a & b);
  if(o=="|")  return Value::integer(a | b);
  if(o=="<<") return Value::integer(int32_t(ua << (ub & 31)));
  if(o==">>") return Value::integer(a >> (b & 31));
  if(o=="/" || o=="%") {
    if(b==0)
      return fail("division by zero: " + e.str());
    return Value::integer(o=="/" ? a/b : a%b);
    }
  return fail("operator " + o);
  }
