#include "program.h"

#include "bytecode.h"
#include "model.h"
#include "textcodec.h"
#include "vocab.h"

#include "daedalus/script.h"

#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <memory>
#include <unordered_map>

using namespace storygraph;
using daedalus::DaedalusDataType;
using daedalus::DaedalusOpcode;

namespace {

const char* binaryOp(DaedalusOpcode op) {
  switch(op) {
    case DaedalusOpcode::ADD:  return "+";
    case DaedalusOpcode::SUB:  return "-";
    case DaedalusOpcode::MUL:  return "*";
    case DaedalusOpcode::DIV:  return "/";
    case DaedalusOpcode::MOD:  return "%";
    case DaedalusOpcode::OR:   return "|";
    case DaedalusOpcode::ANDB: return "&";
    case DaedalusOpcode::LT:   return "<";
    case DaedalusOpcode::GT:   return ">";
    case DaedalusOpcode::ORR:  return "||";
    case DaedalusOpcode::AND:  return "&&";
    case DaedalusOpcode::LSL:  return "<<";
    case DaedalusOpcode::LSR:  return ">>";
    case DaedalusOpcode::LTE:  return "<=";
    case DaedalusOpcode::EQ:   return "==";
    case DaedalusOpcode::NEQ:  return "!=";
    case DaedalusOpcode::GTE:  return ">=";
    default:                   return nullptr;
    }
  }

// stores; `compound` is the operator of `x += v` and the like
bool storeOp(DaedalusOpcode op, const char*& compound) {
  compound = nullptr;
  switch(op) {
    case DaedalusOpcode::ADDMOVI: compound = "+"; return true;
    case DaedalusOpcode::SUBMOVI: compound = "-"; return true;
    case DaedalusOpcode::MULMOVI: compound = "*"; return true;
    case DaedalusOpcode::DIVMOVI: compound = "/"; return true;
    case DaedalusOpcode::MOVI:
    case DaedalusOpcode::MOVS:
    case DaedalusOpcode::MOVSS:
    case DaedalusOpcode::MOVVF:
    case DaedalusOpcode::MOVF:
    case DaedalusOpcode::MOVVI:
      return true;
    default:
      return false;
    }
  }

// script parameters declared `var int` that take an instance: B_GIVEINVITEMS.ITEMINSTANCE
bool instanceParam(const daedalus::DaedalusSymbol& par) {
  auto dot = par.name().find('.');
  return par.type()==DaedalusDataType::INT && dot!=std::string::npos &&
         par.name().find("INSTANCE", dot)!=std::string::npos;
  }

std::string memberName(const std::string& full) {
  auto dot = full.find('.');
  return dot==std::string::npos ? full : full.substr(dot+1);
  }

bool storable(const ExprP& e) {
  return e->kind==Expr::Var || e->kind==Expr::Field || e->kind==Expr::Inst;
  }

// A value on the stack. The result of a call is a step of its own unless something uses it: then `used` is set
// and the call is computed inside the expression that uses it, where the VM pops it.
struct Val final {
  ExprP                 e;
  std::shared_ptr<bool> used;
  };

struct Draft final {
  Step                  step;
  std::shared_ptr<bool> used;  // a call whose result an expression took
  std::vector<Draft>    then, otherwise;
  };

class Builder final {
  public:
    Builder(const daedalus::DaedalusScript& script, const Bytecode& code, const daedalus::DaedalusSymbol& fn, uint32_t end)
      :script(script), code(code), fn(fn) {
      for(uint32_t pc=fn.address(); pc<end;) {
        auto i = script.instruction_at(pc);
        at[pc] = ins.size();
        ins.push_back(i);
        pcs.push_back(pc);
        pc += i.size;
        }
      at[end] = ins.size();
      indexNames();
      }

    Program run() {
      Program p;
      p.returns = fn.has_return();
      std::vector<Val> st;
      // the prologue pops the arguments into the parameters
      for(uint32_t i=0; i<fn.count(); ++i)
        st.push_back({ex::param(int32_t(i)), nullptr});
      std::vector<Draft> top;
      bool returned = false;
      if(!block(0, ins.size(), st, nullptr, top, returned, true)) {
        p.error = error;
        return p;
        }
      if(!returned) {
        // fell off the end without RSR: as if there was one
        Draft d;
        d.step.kind  = Step::Return;
        d.step.pc    = ins.empty() ? fn.address() : pcs.back();
        if(fn.has_return())
          d.step.value = ex::integer(0);
        top.push_back(std::move(d));
        }
      p.steps = finish(top);
      return p;
      }

  private:
    bool fail(std::string why) {
      if(error.empty())
        error = std::move(why);
      return false;
      }

    ExprP pop(std::vector<Val>& st) {
      if(st.size()<=floor) {
        // structured code computes what it uses in its own block
        fail(st.empty() ? "empty stack" : "uses a value from outside its block");
        return ex::unknown("empty stack");
        }
      auto v = std::move(st.back());
      st.pop_back();
      if(v.used)
        *v.used = true;
      return v.e;
      }

    // An operator's value. If it has calls inside and nothing takes it (`x == f();`, a typo for `=`), the calls
    // still run: it stays a step, computed where the VM computes it.
    void push(std::vector<Val>& st, ExprP e, uint32_t pc, std::vector<Draft>& out) {
      bool calls = false;
      ex::walk(e, [&calls](const Expr& x) { calls |= (x.kind==Expr::Call || x.kind==Expr::Fn); });
      if(!calls) {
        st.push_back({std::move(e), nullptr});
        return;
        }
      Draft d;
      d.step.kind = Step::Call;
      d.step.pc   = pc;
      d.step.call = e;
      d.used      = std::make_shared<bool>(false);
      st.push_back({std::move(e), d.used});
      out.push_back(std::move(d));
      }

    void indexNames() {
      static const std::pair<const char*,const char*> prefixes[] = {
        {"AIVAR","AIV_"}, {"ATTRIBUTE","ATR_"}, {"HITCHANCE","NPC_TALENT_"}, {"PROTECTION","PROT_"},
        };
      for(auto& s : script.symbols()) {
        if(!s.is_const() || s.type()!=DaedalusDataType::INT || s.count()!=1)
          continue;
        for(auto& [member,prefix] : prefixes)
          if(s.name().rfind(prefix,0)==0)
            names[member].emplace(s.get_int(0), s.name());
        }
      }

    ExprP pushVar(const daedalus::DaedalusSymbol& sym, int32_t index, const ExprP& inst) {
      const uint16_t i = uint16_t(std::max(index,0));
      if(sym.is_const()) {
        switch(sym.type()) {
          case DaedalusDataType::INT:
            return ex::integer(sym.get_int(i), index>=0 ? sym.name() + "[" + std::to_string(index) + "]" : sym.name());
          case DaedalusDataType::STRING:
            return ex::str(sym.get_string(i), sym.name());
          case DaedalusDataType::FUNCTION:
            return ex::sym(sym.name());
          case DaedalusDataType::INSTANCE:
            return ex::inst(sym.name());
          default:
            fail("constant " + sym.name() + " of type " + std::to_string(int(sym.type())));
            return ex::unknown("constant " + sym.name());
          }
        }
      if(sym.is_member()) {
        if(inst==nullptr) {
          fail("member " + sym.name() + " without an instance");
          return ex::unknown("no instance");
          }
        auto        name = memberName(sym.name());
        std::string idxName;
        auto        n = names.find(name);
        if(index>=0 && n!=names.end()) {
          auto it = n->second.find(index);
          if(it!=n->second.end())
            idxName = it->second;
          }
        return ex::field(inst, name, index, idxName);
        }
      if(sym.type()==DaedalusDataType::INSTANCE)
        return ex::inst(sym.name());
      if(sym.type()==DaedalusDataType::FLOAT)
        fail("float variable " + sym.name());
      return ex::var(sym.name(), index);  // a global, or a static local FN.NAME
      }

    bool target(size_t from, uint32_t address, size_t& to) {
      auto it = at.find(address);
      if(it==at.end())
        return fail("jump outside the function");
      if(it->second<=from)
        return fail("backward jump");
      to = it->second;
      return true;
      }

    // [begin, end) as steps; `returned`: the block ends in a return. A block uses only what it pushed itself
    bool block(size_t begin, size_t end, std::vector<Val> st, ExprP inst, std::vector<Draft>& out, bool& returned,
               bool whole = false) {
      returned = false;
      struct Floor final {
        size_t& f;
        size_t  was;
        Floor(size_t& f, size_t now) : f(f), was(f) { f = now; }
        ~Floor() { f = was; }
        } scope(floor, whole ? 0 : st.size());  // the whole function pops its arguments
      for(size_t i=begin; i<end;) {
        auto& op  = ins[i];
        auto* sym = script.find_symbol_by_index(op.symbol);

        const char* compound = nullptr;
        if(storeOp(op.op, compound)) {
          auto t = pop(st);
          auto v = pop(st);
          if(!storable(t))
            return fail("store to " + t->str());
          if(compound!=nullptr)
            v = ex::exact(compound, {t, v});
          Draft d;
          d.step.kind   = Step::Store;
          d.step.pc     = pcs[i];
          d.step.target = t;
          d.step.value  = v;
          out.push_back(std::move(d));
          ++i;
          continue;
          }
        if(auto bop = binaryOp(op.op)) {
          auto a = pop(st);
          auto b = pop(st);
          push(st, ex::exact(bop, {a, b}), pcs[i], out);
          ++i;
          continue;
          }

        switch(op.op) {
          case DaedalusOpcode::PLUS:
            break;
          case DaedalusOpcode::NEGATE:
            push(st, ex::exact("neg", {pop(st)}), pcs[i], out);
            break;
          case DaedalusOpcode::NOT:
            push(st, ex::exact("!", {pop(st)}), pcs[i], out);
            break;
          case DaedalusOpcode::CMPL:
            push(st, ex::exact("~", {pop(st)}), pcs[i], out);
            break;
          case DaedalusOpcode::PUSHI:
            st.push_back({ex::integer(op.immediate), nullptr});
            break;
          case DaedalusOpcode::PUSHV:
          case DaedalusOpcode::PUSHVV:
            if(sym==nullptr)
              return fail("bad symbol");
            st.push_back({pushVar(*sym, op.op==DaedalusOpcode::PUSHVV ? int32_t(op.index) : -1, inst), nullptr});
            break;
          case DaedalusOpcode::PUSHVI:
            if(sym==nullptr)
              return fail("bad symbol");
            st.push_back({ex::inst(sym->name()), nullptr});
            break;
          case DaedalusOpcode::GMOVI:
            if(sym==nullptr)
              return fail("bad symbol");
            inst = ex::inst(sym->name());
            break;
          case DaedalusOpcode::BE:
          case DaedalusOpcode::BL: {
            const daedalus::DaedalusSymbol* callee = nullptr;
            if(op.op==DaedalusOpcode::BE)
              callee = sym;
            else
              callee = code.functionAt(op.address);
            if(callee==nullptr)
              return fail("bad call");
            std::vector<ExprP> args(callee->count());
            for(size_t k=args.size(); k>0; --k)
              args[k-1] = pop(st);
            for(uint32_t k=0; k<args.size(); ++k) {
              auto& a   = args[k];
              auto* par = script.find_symbol_by_index(callee->index()+1+k);
              bool  ref = par!=nullptr && (par->type()==DaedalusDataType::INSTANCE ||
                                           par->type()==DaedalusDataType::FUNCTION || instanceParam(*par));
              // an instance or a function passed as PUSHI <symbol index>: by name, it is the same value
              if(a->kind==Expr::Int && a->name.empty() && (ref || vocab::takesInstance(callee->name(), k)))
                if(auto* r = script.find_symbol_by_index(uint32_t(a->value)))
                  a = ex::sym(r->name());
              }
            auto call = op.op==DaedalusOpcode::BE ? ex::call(callee->name(), std::move(args))
                                                  : ex::fn  (callee->name(), std::move(args));
            Draft d;
            d.step.kind = Step::Call;
            d.step.pc   = pcs[i];
            d.step.call = call;
            if(callee->has_return()) {
              d.used = std::make_shared<bool>(false);
              st.push_back({call, d.used});
              }
            out.push_back(std::move(d));
            break;
            }
          case DaedalusOpcode::BZ: {
            auto   c = pop(st);
            size_t t = 0;
            if(!target(i, op.address, t))
              return false;
            if(t>end)
              return fail("branch out of its block");
            // `if c {A} else {B}`: BZ over A, and A ends with a jump over B. A jump to right after itself is
            // an empty else and stays inside A.
            size_t thenEnd = t, elseEnd = t;
            if(t-1>i && ins[t-1].op==DaedalusOpcode::B) {
              size_t e = 0;
              if(!target(t-1, ins[t-1].address, e))
                return false;
              if(e>end)
                return fail("else out of its block");
              if(e>t) {
                thenEnd = t-1;
                elseEnd = e;
                }
              }
            Draft d;
            d.step.kind = Step::If;
            d.step.pc   = pcs[i];
            d.step.cond = c;
            bool r1 = false, r2 = false;
            if(!block(i+1, thenEnd, st, inst, d.then, r1))
              return false;
            if(!block(t, elseEnd, st, inst, d.otherwise, r2))
              return false;
            out.push_back(std::move(d));
            i = elseEnd;
            continue;
            }
          case DaedalusOpcode::B: {
            size_t t = 0;
            if(!target(i, op.address, t))
              return false;
            if(t!=i+1)
              return fail("jump that is not an else");
            break;
            }
          case DaedalusOpcode::RSR: {
            Draft d;
            d.step.kind = Step::Return;
            d.step.pc   = pcs[i];
            if(fn.has_return()) {
              // the caller takes the top of the stack, even a result left there before this block
              if(st.size()>floor) {
                d.step.value = pop(st);
                } else if(!st.empty()) {
                d.step.value = st.back().e;
                if(st.back().used)
                  *st.back().used = true;
                } else {
                d.step.value = ex::integer(0);
                }
              }
            out.push_back(std::move(d));
            returned = true;
            return true;  // what follows in this block can't run
            }
          default:
            return fail("opcode " + std::to_string(int(op.op)));
          }
        if(!error.empty())
          return false;
        ++i;
        }
      return error.empty();
      }

    static std::vector<Step> finish(std::vector<Draft>& ds) {
      std::vector<Step> r;
      for(auto& d : ds) {
        if(d.used && *d.used)
          continue;
        Step s = std::move(d.step);
        s.then      = finish(d.then);
        s.otherwise = finish(d.otherwise);
        r.push_back(std::move(s));
        }
      return r;
      }

    const daedalus::DaedalusScript&               script;
    const Bytecode&                             code;
    const daedalus::DaedalusSymbol&               fn;
    std::vector<daedalus::DaedalusInstruction>    ins;
    std::vector<uint32_t>                       pcs;
    std::unordered_map<uint32_t,size_t>         at;
    std::unordered_map<std::string,std::unordered_map<int32_t,std::string>> names;
    std::string                                 error;
    size_t                                      floor = 0;  // values below belong to an enclosing block
  };

void describe(const std::vector<Step>& steps, const std::string& indent, std::string& out) {
  for(auto& s : steps) {
    switch(s.kind) {
      case Step::Call:
        out += indent + s.call->str() + "\n";
        break;
      case Step::Store:
        out += indent + s.target->str() + " = " + s.value->str() + "\n";
        break;
      case Step::Return:
        out += indent + "return" + (s.value ? " " + s.value->str() : std::string()) + "\n";
        break;
      case Step::If:
        out += indent + "if " + s.cond->str() + "\n";
        describe(s.then, indent + "  ", out);
        if(!s.otherwise.empty()) {
          out += indent + "else\n";
          describe(s.otherwise, indent + "  ", out);
          }
        break;
      case Step::Say:
        out += indent + (s.hero ? "other" : "self") + " says " +
               (s.line.empty() ? "\"" + TextCodec::toUtf8(s.text) + "\"" : s.line) + "\n";
        break;
      case Step::Choice:
        out += indent + "choice " + s.id + " \"" + TextCodec::toUtf8(s.text) + "\"\n";
        describe(s.then, indent + "  ", out);
        break;
      case Step::ClearChoices:
        out += indent + "clear choices\n";
        break;
      }
    }
  }

template<class W>
void writeExprRaw(W& w, const ExprP& e) {
  auto text = toJson(e);
  w.RawValue(text.c_str(), text.size(), rapidjson::kArrayType);
  }

template<class W>
void writeSteps(W& w, const std::vector<Step>& steps) {
  w.StartArray();
  for(auto& s : steps) {
    w.StartObject();
    switch(s.kind) {
      case Step::Call:
        w.Key("do"); w.String("call");
        w.Key("call"); writeExprRaw(w, s.call);
        break;
      case Step::Store:
        w.Key("do"); w.String("set");
        w.Key("target"); writeExprRaw(w, s.target);
        w.Key("value");  writeExprRaw(w, s.value);
        break;
      case Step::Return:
        w.Key("do"); w.String("return");
        if(s.value) {
          w.Key("value"); writeExprRaw(w, s.value);
          }
        break;
      case Step::If:
        w.Key("do"); w.String("if");
        w.Key("cond"); writeExprRaw(w, s.cond);
        w.Key("then"); writeSteps(w, s.then);
        if(!s.otherwise.empty()) {
          w.Key("else"); writeSteps(w, s.otherwise);
          }
        break;
      case Step::Say: {
        w.Key("do");      w.String("say");
        w.Key("speaker"); w.String(s.hero ? "other" : "self");
        auto text = s.line.empty() ? TextCodec::toUtf8(s.text) : s.line;
        w.Key(s.line.empty() ? "text" : "line");
        w.String(text.c_str(), rapidjson::SizeType(text.size()));
        break;
        }
      case Step::Choice: {
        w.Key("do"); w.String("choice");
        if(!s.id.empty()) {
          w.Key("id"); w.String(s.id.c_str(), rapidjson::SizeType(s.id.size()));
          }
        auto text = TextCodec::toUtf8(s.text);
        w.Key("text");  w.String(text.c_str(), rapidjson::SizeType(text.size()));
        w.Key("steps"); writeSteps(w, s.then);
        break;
        }
      case Step::ClearChoices:
        w.Key("do"); w.String("clear_choices");
        break;
      }
    w.EndObject();
    }
  w.EndArray();
  }

}

Program storygraph::liftProgram(const daedalus::DaedalusScript& script, const Bytecode& code, uint32_t fnSymbol) {
  auto* fn = script.find_symbol_by_index(fnSymbol);
  if(fn==nullptr || !Bytecode::hasCode(*fn)) {
    Program p;
    p.error = "no code";
    return p;
    }
  Builder b(script, code, *fn, code.codeEnd(fn->address()));
  return b.run();
  }

std::string storygraph::describe(const Program& p) {
  if(!p.error.empty())
    return "not a program: " + p.error + "\n";
  std::string out;
  ::describe(p.steps, "", out);
  return out;
  }

std::string storygraph::toJson(const Program& p) {
  rapidjson::StringBuffer buf;
  rapidjson::Writer<rapidjson::StringBuffer> w(buf);
  writeSteps(w, p.steps);
  return std::string(buf.GetString(), buf.GetSize());
  }
