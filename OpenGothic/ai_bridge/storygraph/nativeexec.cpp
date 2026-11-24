#include "nativeexec.h"
#include "execcore.h"

#include <algorithm>
#include <array>
#include <bit>
#include <exception>
#include <unordered_set>
#include <utility>

using namespace storygraph;

NativeValue NativeValue::integer(int32_t v) {
  NativeValue result;
  result.i = v;
  return result;
  }

NativeValue NativeValue::real(float v) {
  NativeValue result;
  result.type = Float;
  result.f = v;
  return result;
  }

NativeValue NativeValue::str(std::string v) {
  NativeValue result;
  result.type = Str;
  result.s = std::move(v);
  return result;
  }

NativeValue NativeValue::instance(NativeInstance v) {
  NativeValue result;
  result.type = Inst;
  result.inst = v;
  return result;
  }

std::optional<int32_t> execution::integerOperator(std::string_view op, std::span<const int32_t> v, std::string& error) {
  if(v.size()==1) {
    if(op=="!")   return v[0]==0;
    if(op=="neg") return int32_t(0u-uint32_t(v[0]));
    if(op=="~")   return ~v[0];
    error = "unary " + std::string(op);
    return std::nullopt;
    }
  if(v.size()!=2) {
    error = "operator " + std::string(op) + " with " + std::to_string(v.size()) + " operands";
    return std::nullopt;
    }

  const int32_t a = v[0], b = v[1];
  const auto ua = uint32_t(a), ub = uint32_t(b);
  if(op=="&&") return a && b;
  if(op=="||") return a || b;
  if(op=="==") return a==b;
  if(op=="!=") return a!=b;
  if(op=="<")  return a<b;
  if(op==">")  return a>b;
  if(op=="<=") return a<=b;
  if(op==">=") return a>=b;
  if(op=="+")  return int32_t(ua+ub);
  if(op=="-")  return int32_t(ua-ub);
  if(op=="*")  return int32_t(ua*ub);
  if(op=="&")  return a & b;
  if(op=="|")  return a | b;
  if(op=="<<") return int32_t(ua << (ub & 31));
  if(op==">>") return a >> (b & 31);
  if(op=="/" || op=="%") {
    if(b==0) {
      error = "vm: division by zero";
      return std::nullopt;
      }
    return int32_t(op=="/" ? int64_t(a)/b : int64_t(a)%b);
    }
  error = "operator " + std::string(op);
  return std::nullopt;
  }

namespace {

std::optional<NativeValue> asParameter(NativeValue::Type type, NativeValue value) {
  if(type==value.type)
    return value;
  if(type==NativeValue::Float && value.type==NativeValue::Int)
    return NativeValue::real(std::bit_cast<float>(value.i));
  return std::nullopt;
  }

NativeValue asReturned(NativeValue::Type type, NativeValue value) {
  if(value.type==type)
    return value;
  value = {};
  value.type = type;
  return value;
  }

struct Preflight final {
  const NativeExecutionHost& host;
  bool ownChoices;
  std::unordered_set<const Program*> visited;
  unsigned depth = 0;

  std::string program(const Program& p, size_t arguments) {
    if(!p.error.empty())
      return "not a program: " + p.error;
    if(!visited.insert(&p).second)
      return {};
    if(depth==64)
      return "program call depth exceeded";
    ++depth;
    auto error = steps(p.steps, arguments);
    --depth;
    return error;
    }

  std::string expression(const ExprP& expr, size_t arguments) {
    if(!expr)
      return "missing expression";
    const auto& e = *expr;
    switch(e.kind) {
      case Expr::Int:
      case Expr::Str:
        break;
      case Expr::Var:
      case Expr::Inst:
        if(!host.hasVariable(e.name, e.kind==Expr::Inst))
          return "no variable " + ident(e.name);
        break;
      case Expr::Field:
        if(e.args.size()!=1)
          return "wrong member reference: " + e.str();
        break;
      case Expr::Sym:
        if(!host.symbolIndex(e.name))
          return "no symbol " + ident(e.name);
        break;
      case Expr::Param:
        if(e.index<0 || size_t(e.index)>=arguments)
          return "no argument " + std::to_string(e.index);
        break;
      case Expr::Call:
      case Expr::Fn: {
        const auto fn = host.findFunction(e.name);
        if(!fn)
          return "unresolved native program function: " + ident(e.name);
        if(fn->parameters.size()!=e.args.size())
          return "wrong argument count: " + e.str();
        if(fn->program!=nullptr) {
          auto error = program(*fn->program, e.args.size());
          if(!error.empty())
            return error;
          }
        break;
        }
      case Expr::Op: {
        const std::array<int32_t,2> sample = {1,1};
        if(e.args.size()>sample.size())
          return "wrong operator arity: " + e.str();
        std::string error;
        if(!execution::integerOperator(e.name, std::span(sample).first(e.args.size()), error))
          return error;
        break;
        }
      default:
        return "not in a native program: " + e.str();
      }
    for(const auto& argument : e.args) {
      auto error = expression(argument, arguments);
      if(!error.empty())
        return error;
      }
    return {};
    }

  std::string steps(const std::vector<Step>& list, size_t arguments) {
    for(const auto& s : list) {
      std::string error;
      switch(s.kind) {
        case Step::Call:
          error = expression(s.call, arguments);
          break;
        case Step::Store:
          if(!s.target || (s.target->kind!=Expr::Var && s.target->kind!=Expr::Inst && s.target->kind!=Expr::Field))
            return "invalid native store target";
          error = expression(s.target, arguments);
          if(error.empty())
            error = expression(s.value, arguments);
          break;
        case Step::If:
          error = expression(s.cond, arguments);
          if(error.empty())
            error = steps(s.then, arguments);
          if(error.empty())
            error = steps(s.otherwise, arguments);
          break;
        case Step::Return:
          if(s.value)
            error = expression(s.value, arguments);
          break;
        case Step::Say: {
          const auto out = host.findFunction("AI_OUTPUT");
          if(!out || out->program!=nullptr || out->parameters!=std::vector{NativeValue::Inst,NativeValue::Inst,NativeValue::Str} ||
             !host.hasVariable("SELF",true) || !host.hasVariable("OTHER",true))
            return "say: no native AI_Output";
          break;
          }
        case Step::Choice:
        case Step::ClearChoices:
          if(!ownChoices)
            return "choices of its own are for the director's dialogs";
          break;
        default:
          return "unknown native program step";
        }
      if(!error.empty())
        return error;
      }
    return {};
    }
  };

}

std::string NativeInterpreter::preflight(const Program& p, size_t arguments) const {
  return Preflight{host,bool(choices),{}}.program(p, arguments);
  }

NativeInterpreter::Result NativeInterpreter::run(const Program& p, const std::vector<NativeValue>& args) {
  try {
    auto error = preflight(p, args.size());
    if(!error.empty())
      return {false,std::move(error),{},false};
    }
  catch(const std::exception& e) {
    return {false,std::string("native program preflight: ")+e.what(),{},false};
    }

  auto state = execute(p, args, true);
  Result result;
  result.started = true;
  result.ok = state.error.empty();
  result.error = std::move(state.error);
  if(result.ok && state.returned)
    result.returned = std::move(state.returned->value);
  return result;
  }

NativeInterpreter::Frame NativeInterpreter::execute(const Program& p, const std::vector<NativeValue>& args, bool readReturn) {
  return execution::Core::execute(*this,p,args,readReturn,"native program failed","native program: ");
  }

std::nullopt_t NativeInterpreter::fail(std::string error) {
  if(frame->error.empty())
    frame->error = std::move(error);
  return std::nullopt;
  }

bool NativeInterpreter::store(const Step& s) {
  auto value = eval(*s.value);
  if(!value)
    return false;
  auto target = item(*s.target);
  if(!target || !target->ref)
    return false;
  if(target->ref->kind==NativeSlot::Member && target->ref->context==0)
    return true;

  std::string error;
  if(host.write(*target->ref, *value, error))
    return true;
  fail(error.empty() ? "cannot store " + s.target->str() : std::move(error));
  return false;
  }

bool NativeInterpreter::say(const Step& s) {
  Item speaker, listener;
  speaker.ref = NativeSlot{NativeSlot::Instance,s.hero ? "OTHER" : "SELF",0,0};
  listener.ref = NativeSlot{NativeSlot::Instance,s.hero ? "SELF" : "OTHER",0,0};
  const auto from = read(speaker);
  const auto to = read(listener);
  if(!from || !to || from->type!=NativeValue::Inst || to->type!=NativeValue::Inst || from->inst==0 || to->inst==0) {
    fail("say: no one to say it");
    return false;
    }
  auto line = s.line;
  if(line.empty() && ownLine)
    line = ownLine(host.instanceName(from->inst), s.text);
  if(line.empty()) {
    fail("say: a line of its own can't be said here");
    return false;
    }
  std::string error;
  if(host.invoke("AI_OUTPUT", {*from,*to,NativeValue::str(std::move(line))}, error))
    return true;
  fail(error.empty() ? "say: native AI_Output failed" : std::move(error));
  return false;
  }

std::optional<NativeValue> NativeInterpreter::eval(const Expr& e) {
  auto value = item(e);
  return value ? read(*value) : std::nullopt;
  }

std::optional<NativeValue> NativeInterpreter::read(const Item& it) {
  if(!it.ref)
    return it.value;
  std::string error;
  auto value = host.read(*it.ref, error);
  if(!value)
    return fail(error.empty() ? "cannot read " + ident(std::string(it.ref->name)) : std::move(error));
  return value;
  }

std::optional<NativeInterpreter::Item> NativeInterpreter::item(const Expr& e) {
  Item value;
  switch(e.kind) {
    case Expr::Int:
      value.value = NativeValue::integer(e.value);
      return value;
    case Expr::Str:
      value.value = NativeValue::str(e.text);
      return value;
    case Expr::Var:
    case Expr::Inst:
      value.ref = NativeSlot{e.kind==Expr::Inst ? NativeSlot::Instance : NativeSlot::Variable,
                             e.name,uint16_t(std::max(e.index,0)),0};
      return value;
    case Expr::Field: {
      auto instance = eval(*e.args[0]);
      if(!instance)
        return std::nullopt;
      if(instance->type!=NativeValue::Inst)
        return fail("member of a non-instance: " + e.str());
      value.ref = NativeSlot{NativeSlot::Member,e.name,uint16_t(std::max(e.index,0)),instance->inst};
      return value;
      }
    case Expr::Sym:
      if(const auto index = host.symbolIndex(e.name)) {
        value.value = NativeValue::integer(*index);
        return value;
        }
      return fail("no symbol " + ident(e.name));
    case Expr::Param:
      value.value = frame->args[size_t(e.index)];
      return value;
    case Expr::Call:
    case Expr::Fn:
      return call(e);
    case Expr::Op:
      return op(e);
    default:
      return fail("not in a native program: " + e.str());
    }
  }

std::optional<NativeInterpreter::Item> NativeInterpreter::call(const Expr& e) {
  const auto fn = host.findFunction(e.name);
  if(!fn || fn->parameters.size()!=e.args.size())
    return fail("unresolved native program function: " + ident(e.name));
  auto pushed = execution::Core::push(*this,e.args,execution::Core::Order::Arguments);
  if(!pushed)
    return std::nullopt;
  std::vector<NativeValue> args;
  args.reserve(pushed->size());
  for(size_t k=0; k<pushed->size(); ++k) {
    auto v = read((*pushed)[k]);
    if(!v)
      return std::nullopt;
    auto param = asParameter(fn->parameters[k], std::move(*v));
    if(!param)
      return fail("argument " + std::to_string(k) + " of " + ident(e.name) + " has the wrong type");
    args.push_back(std::move(*param));
    }

  if(fn->program==nullptr) {
    std::string error;
    auto result = host.invoke(e.name, args, error);
    if(!result)
      return fail(error.empty() ? "native function failed: " + ident(e.name) : std::move(error));
    return Item{fn->result ? asReturned(*fn->result, std::move(*result)) : NativeValue{}, {}};
    }

  auto nested = execute(*fn->program, args, false);
  if(!nested.error.empty())
    return fail(std::move(nested.error));
  if(!fn->result)
    return Item{};
  if(!nested.returned)
    return Item{asReturned(*fn->result, {}), {}};
  auto& returned = *nested.returned;
  if(!returned.ref) {
    auto value = asParameter(*fn->result, std::move(returned.value));
    if(!value)
      return fail("wrong return type: " + ident(e.name));
    returned.value = std::move(*value);
    }
  return std::move(returned);
  }

std::optional<NativeInterpreter::Item> NativeInterpreter::op(const Expr& e) {
  if(auto value = execution::Core::arithmetic(*this,e))
    return Item{NativeValue::integer(*value), {}};
  return std::nullopt;
  }
