#include "exec.h"

#include "daedalus/vm.h"

#include <cstring>

using namespace storygraph;
using daedalus::DaedalusDataType;

namespace {

// what the VM pushes for a function of this return type
Value asReturned(DaedalusDataType t, const Value& v) {
  switch(t) {
    case DaedalusDataType::FLOAT:    return Value::real(v.type==Value::Float ? v.f : 0.f);
    case DaedalusDataType::STRING:   return Value::str(v.type==Value::Str ? v.s : std::string());
    case DaedalusDataType::INSTANCE: return Value::instance(v.type==Value::Inst ? v.inst : nullptr);
    default:                         return Value::integer(v.type==Value::Int ? v.i : 0);
    }
  }

// a value as a parameter of this type pops it: a float literal is pushed as its bits (PUSHI)
std::optional<Value> asParameter(DaedalusDataType t, const Value& v) {
  switch(t) {
    case DaedalusDataType::FLOAT:
      if(v.type==Value::Float)
        return v;
      if(v.type==Value::Int) {
        float f = 0;
        std::memcpy(&f, &v.i, sizeof(f));
        return Value::real(f);
        }
      return std::nullopt;
    case DaedalusDataType::STRING:
      return v.type==Value::Str ? std::optional<Value>(v) : std::nullopt;
    case DaedalusDataType::INSTANCE:
      return v.type==Value::Inst ? std::optional<Value>(v) : std::nullopt;
    default:
      return v.type==Value::Int ? std::optional<Value>(v) : std::nullopt;
    }
  }

bool isInt(const std::optional<Value>& v) { return v && v->type==Value::Int; }

}

Interpreter::Result Interpreter::run(const Program& p, const std::vector<Value>& args) {
  Result r;
  if(!p.error.empty()) {
    r.ok    = false;
    r.error = "not a program: " + p.error;
    return r;
    }
  // an engine function a program calls may run another program (Npc_CheckInfo checks the conditions)
  auto outerParams   = params;
  auto outerReturned = std::move(returned);
  auto outerError    = std::move(error);
  params   = &args;
  returned = Value::integer(0);
  error.clear();
  Flow f = Flow::Fail;
  try {
    f = steps(p.steps);
    }
  catch(const std::exception& e) {
    fail(std::string("vm: ") + e.what());
    }
  if(f==Flow::Fail) {
    r.ok    = false;
    r.error = error.empty() ? std::string("failed") : error;
    } else {
    r.returned = returned;
    }
  params   = outerParams;
  returned = std::move(outerReturned);
  error    = std::move(outerError);
  return r;
  }

std::optional<Value> Interpreter::fail(std::string why) {
  if(error.empty())
    error = std::move(why);
  return std::nullopt;
  }

Interpreter::Flow Interpreter::steps(const std::vector<Step>& s) {
  for(auto& st : s) {
    switch(st.kind) {
      case Step::Call:
        if(!eval(*st.call))
          return Flow::Fail;
        break;
      case Step::Store:
        if(!store(st))
          return Flow::Fail;
        break;
      case Step::If: {
        auto c = eval(*st.cond);
        if(!isInt(c)) {
          if(c)
            fail("condition is not an int: " + st.cond->str());
          return Flow::Fail;
          }
        auto f = steps(c->i!=0 ? st.then : st.otherwise);
        if(f!=Flow::Next)
          return f;
        break;
        }
      case Step::Return:
        if(st.value!=nullptr) {
          auto v = eval(*st.value);
          if(!v)
            return Flow::Fail;
          returned = *v;
          }
        return Flow::Return;
      case Step::Say:
        if(!say(st))
          return Flow::Fail;
        break;
      case Step::Choice:
      case Step::ClearChoices:
        if(!choices || !choices(st)) {
          fail("choices of its own are for the director's dialogs");
          return Flow::Fail;
          }
        break;
      }
    }
  return Flow::Next;
  }

bool Interpreter::say(const Step& s) {
  // as a script says it: AI_Output(self, other, line), or the other way round for the hero
  auto* out   = vm.find_symbol_by_name("AI_OUTPUT");
  auto* self  = vm.global_self();
  auto* other = vm.global_other();
  if(out==nullptr || !out->is_external() || self==nullptr || other==nullptr)
    return bool(fail("say: the scripts have no AI_Output"));
  auto speaker  = s.hero ? other->get_instance() : self->get_instance();
  auto listener = s.hero ? self->get_instance() : other->get_instance();
  if(speaker==nullptr || listener==nullptr)
    return bool(fail("say: no one to say it"));
  std::string id = s.line;
  if(id.empty()) {
    auto* who = vm.find_symbol_by_index(speaker->symbol_index());
    id = ownLine ? ownLine(who!=nullptr ? who->name() : std::string(), s.text) : std::string();
    if(id.empty())
      return bool(fail("say: a line of its own can't be said here"));
    }
  ext.call(*out, {Value::instance(speaker), Value::instance(listener), Value::str(id)});
  return true;
  }

bool Interpreter::store(const Step& s) {
  auto v = eval(*s.value);
  if(!v)
    return false;
  auto& t = *s.target;
  if(t.kind==Expr::Inst) {
    auto* sym = vm.find_symbol_by_name(t.name);
    if(sym==nullptr || sym->type()!=DaedalusDataType::INSTANCE)
      return bool(fail("no instance variable " + ident(t.name)));
    if(v->type!=Value::Inst)
      return bool(fail("not an instance for " + t.str()));
    sym->set_instance(v->inst);
    return true;
    }

  daedalus::DaedalusSymbol*                    sym = nullptr;
  std::shared_ptr<daedalus::DaedalusInstance>  of;
  if(t.kind==Expr::Var) {
    sym = vm.find_symbol_by_name(t.name);
    if(sym==nullptr)
      return bool(fail("no variable " + ident(t.name)));
    } else if(t.kind==Expr::Field) {
    auto inst = eval(*t.args[0]);
    if(!inst)
      return false;
    if(inst->type!=Value::Inst)
      return bool(fail("member of a non-instance: " + t.str()));
    if(inst->inst==nullptr)
      return true;  // the VM drops a store to a member of no instance (ALLOW_NULL_INSTANCE_ACCESS)
    of  = inst->inst;
    sym = member(vm, *of, t.name);
    if(sym==nullptr)
      return bool(fail("no member " + t.str()));
    } else {
    return bool(fail("store to " + t.str()));
    }
  if(sym->is_const())
    return bool(fail("store to the constant " + ident(sym->name())));
  auto i = uint16_t(std::max(t.index, 0));
  if(i>=sym->count())
    return bool(fail("index out of range: " + t.str()));
  switch(sym->type()) {
    case DaedalusDataType::INT:
    case DaedalusDataType::FUNCTION: {
      if(v->type!=Value::Int)
        return bool(fail("not an int for " + t.str()));
      sym->set_int(v->i, i, of.get());
      return true;
      }
    case DaedalusDataType::FLOAT: {
      auto f = asParameter(DaedalusDataType::FLOAT, *v);
      if(!f)
        return bool(fail("not a float for " + t.str()));
      sym->set_float(f->f, i, of.get());
      return true;
      }
    case DaedalusDataType::STRING:
      if(v->type!=Value::Str)
        return bool(fail("not a string for " + t.str()));
      sym->set_string(v->s, i, of.get());
      return true;
    default:
      return bool(fail("store to a variable of an odd type: " + t.str()));
    }
  }

static std::optional<Value> readMember(daedalus::DaedalusVm& vm, const Expr& e,
                                       const std::shared_ptr<daedalus::DaedalusInstance>& inst, std::string& error) {
  if(inst==nullptr) {
    // members of no instance read as 0 or "" (ALLOW_NULL_INSTANCE_ACCESS); the class is not known, so the
    // member's type is looked up by name
    for(auto* cls : {"C_NPC.", "C_ITEM.", "C_INFO.", "C_FOCUS.", "C_MENU_ITEM."})
      if(auto* m = vm.find_symbol_by_name(cls + e.name); m!=nullptr && m->is_member())
        return m->type()==DaedalusDataType::STRING ? Value::str("") : Value::integer(0);
    return Value::integer(0);
    }
  auto* sym = member(vm, *inst, e.name);
  if(sym==nullptr) {
    error = "no member " + e.str();
    return std::nullopt;
    }
  auto i = uint16_t(std::max(e.index, 0));
  if(i>=sym->count()) {
    error = "index out of range: " + e.str();
    return std::nullopt;
    }
  switch(sym->type()) {
    case DaedalusDataType::INT:
    case DaedalusDataType::FUNCTION: return Value::integer(sym->get_int(i, inst.get()));
    case DaedalusDataType::FLOAT:    return Value::real(sym->get_float(i, inst.get()));
    case DaedalusDataType::STRING:   return Value::str(sym->get_string(i, inst.get()));
    default:
      error = "member of an odd type: " + e.str();
      return std::nullopt;
    }
  }

std::optional<Value> Interpreter::eval(const Expr& e) {
  auto it = item(e);
  if(!it)
    return std::nullopt;
  return read(*it);
  }

std::optional<Value> Interpreter::read(const Item& it) {
  if(it.field!=nullptr) {
    std::string why;
    auto v = readMember(vm, *it.field, it.context, why);
    return v ? v : fail(why);
    }
  auto* sym = it.ref;
  if(sym==nullptr)
    return it.value;
  if(sym->type()==DaedalusDataType::INSTANCE)
    return Value::instance(sym->get_instance());
  if(it.index>=sym->count())
    return fail("index out of range: " + ident(sym->name()));
  if(sym->is_member() && it.context==nullptr)
    return sym->type()==DaedalusDataType::STRING ? Value::str("") : Value::integer(0);
  auto* ctx = sym->is_member() ? it.context.get() : nullptr;
  switch(sym->type()) {
    case DaedalusDataType::INT:
    case DaedalusDataType::FUNCTION: return Value::integer(sym->get_int(it.index, ctx));
    case DaedalusDataType::FLOAT:    return Value::real(sym->get_float(it.index, ctx));
    case DaedalusDataType::STRING:   return Value::str(sym->get_string(it.index, ctx));
    default:                         return fail("variable of an odd type: " + ident(sym->name()));
    }
  }

std::optional<Interpreter::Item> Interpreter::item(const Expr& e) {
  Item it;
  switch(e.kind) {
    case Expr::Int:
      it.value = Value::integer(e.value);
      return it;
    case Expr::Str:
      it.value = Value::str(e.text);
      return it;
    case Expr::Var:
    case Expr::Inst: {
      auto* sym = vm.find_symbol_by_name(e.name);
      if(sym==nullptr || (e.kind==Expr::Inst && sym->type()!=DaedalusDataType::INSTANCE))
        return failed("no variable " + ident(e.name));
      it.ref   = sym;
      it.index = uint16_t(std::max(e.index, 0));
      return it;
      }
    case Expr::Field: {
      auto inst = eval(*e.args[0]);
      if(!inst)
        return std::nullopt;
      if(inst->type!=Value::Inst)
        return failed("member of a non-instance: " + e.str());
      it.context = inst->inst;
      it.field   = &e;
      return it;
      }
    case Expr::Sym: {
      auto* sym = vm.find_symbol_by_name(e.name);
      if(sym==nullptr)
        return failed("no symbol " + ident(e.name));
      it.value = Value::integer(int32_t(sym->index()));
      return it;
      }
    case Expr::Param:
      if(params==nullptr || e.index<0 || size_t(e.index)>=params->size())
        return failed("no argument " + std::to_string(e.index));
      it.value = (*params)[size_t(e.index)];
      return it;
    case Expr::Call:
    case Expr::Fn:
      return call(e);
    case Expr::Op:
      return op(e);
    case Expr::Local:
    case Expr::Cases:
    case Expr::Unknown:
      break;
    }
  return failed("not in a program: " + e.str());
  }

std::optional<Interpreter::Item> Interpreter::call(const Expr& e) {
  auto* fn = vm.find_symbol_by_name(e.name);
  if(fn==nullptr || fn->type()!=DaedalusDataType::FUNCTION)
    return failed("no function " + ident(e.name));
  auto types = vm.find_parameters_for_function(fn);
  if(types.size()!=e.args.size())
    return failed("wrong argument count: " + e.str());

  // pushed first to last, read when the call pops them
  std::vector<Item> pushed;
  for(auto& a : e.args) {
    auto it = item(*a);
    if(!it)
      return std::nullopt;
    pushed.push_back(std::move(*it));
    }
  std::vector<Value> args(pushed.size());
  for(size_t k=0; k<pushed.size(); ++k) {
    auto v = read(pushed[k]);
    if(!v)
      return std::nullopt;
    auto p = asParameter(types[k].type(), *v);
    if(!p)
      return failed("argument " + std::to_string(k) + " of " + ident(e.name) + " has the wrong type");
    args[k] = std::move(*p);
    }

  Item r;
  if(fn->is_external()) {
    r.value = asReturned(fn->rtype(), ext.call(*fn, args));
    return r;
    }
  // a script function runs in the VM; it may return a variable, read when the value is popped
  for(auto& a : args)
    switch(a.type) {
      case Value::Int:   vm.push_int(a.i);         break;
      case Value::Float: vm.push_float(a.f);       break;
      case Value::Str:   vm.push_string(a.s);      break;
      case Value::Inst:  vm.push_instance(a.inst); break;
      }
  vm.unsafe_call_bl(fn);  // as BL: an engine override runs instead of the script
  if(!fn->has_return()) {
    r.value = Value::integer(0);
    return r;
    }
  if(vm.top_is_reference()) {
    auto [sym, index, context] = vm.pop_reference();
    r.ref     = sym;
    r.index   = index;
    r.context = context;
    return r;
    }
  switch(fn->rtype()) {
    case DaedalusDataType::FLOAT:    r.value = Value::real(vm.pop_float());        break;
    case DaedalusDataType::STRING:   r.value = Value::str(vm.pop_string());        break;
    case DaedalusDataType::INSTANCE: r.value = Value::instance(vm.pop_instance()); break;
    default:                         r.value = Value::integer(vm.pop_int());       break;
    }
  return r;
  }

std::optional<Interpreter::Item> Interpreter::op(const Expr& e) {
  // the VM pushes the right operand first: operands are computed last to first, then read
  const size_t n = e.args.size();
  std::vector<Item> pushed(n);
  for(size_t k=n; k>0; --k) {
    auto it = item(*e.args[k-1]);
    if(!it)
      return std::nullopt;
    pushed[k-1] = std::move(*it);
    }
  std::vector<int32_t> v(n);
  for(size_t k=0; k<n; ++k) {
    auto x = read(pushed[k]);
    if(!x)
      return std::nullopt;
    if(x->type!=Value::Int)
      return failed("arithmetic on a non-int: " + e.str());
    v[k] = x->i;
    }

  Item r;
  auto& o = e.name;
  auto  integer = [&r](int32_t x) { r.value = Value::integer(x); return r; };
  if(n==1) {
    if(o=="!")   return integer(v[0]==0);
    if(o=="neg") return integer(int32_t(0u-uint32_t(v[0])));
    if(o=="~")   return integer(~v[0]);
    return failed("unary " + o);
    }
  if(n!=2)
    return failed("operator " + o + " with " + std::to_string(n) + " operands");
  // like the VM: the left operand is args[0], arithmetic wraps, && and || compute both sides
  const int32_t a = v[0], b = v[1];
  const auto ua = uint32_t(a), ub = uint32_t(b);
  if(o=="&&") return integer(a && b);
  if(o=="||") return integer(a || b);
  if(o=="==") return integer(a==b);
  if(o=="!=") return integer(a!=b);
  if(o=="<")  return integer(a<b);
  if(o==">")  return integer(a>b);
  if(o=="<=") return integer(a<=b);
  if(o==">=") return integer(a>=b);
  if(o=="+")  return integer(int32_t(ua+ub));
  if(o=="-")  return integer(int32_t(ua-ub));
  if(o=="*")  return integer(int32_t(ua*ub));
  if(o=="&")  return integer(a & b);
  if(o=="|")  return integer(a | b);
  if(o=="<<") return integer(int32_t(ua << (ub & 31)));
  if(o==">>") return integer(a >> (b & 31));
  if(o=="/" || o=="%") {
    if(b==0)
      return failed("vm: division by zero");
    return integer(o=="/" ? a/b : a%b);
    }
  return failed("operator " + o);
  }
