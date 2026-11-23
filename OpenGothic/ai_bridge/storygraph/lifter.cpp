#include "lifter.h"

#include "bytecode.h"
#include "vocab.h"

#include "daedalus/script.h"

#include <algorithm>
#include <map>
#include <set>

using namespace storygraph;
using daedalus::DaedalusDataType;
using daedalus::DaedalusOpcode;

// ---- Dnf

Dnf Dnf::always() {
  Dnf d;
  d.terms.emplace_back();
  return d;
  }

Dnf Dnf::unknown() {
  Dnf d;
  d.unknownFlag = true;
  return d;
  }

static bool contains(const std::vector<ExprP>& conj, const std::string& key) {
  auto it = std::lower_bound(conj.begin(), conj.end(), key, [](const ExprP& e, const std::string& k){ return e->key<k; });
  return it!=conj.end() && (*it)->key==key;
  }

Dnf Dnf::andLiteral(const ExprP& lit) const {
  if(unknownFlag)
    return *this;
  if(lit->kind==Expr::Int)
    return lit->isTrue() ? *this : never();
  const std::string neg = ex::lnot(lit)->key;
  Dnf r;
  for(auto& c : terms) {
    if(contains(c, neg))
      continue;  // x && !x
    Conj n = c;
    if(!contains(n, lit->key)) {
      n.push_back(lit);
      std::sort(n.begin(), n.end(), [](const ExprP& a, const ExprP& b){ return a->key<b->key; });
      }
    r.terms.push_back(std::move(n));
    }
  return r;
  }

Dnf Dnf::operator&&(const ExprP& cond) const {
  if(unknownFlag)
    return *this;
  if(cond->kind==Expr::Op) {
    auto& o = cond->name;
    if(o=="&&") {
      Dnf r = *this;
      for(auto& t : cond->args)
        r = r && t;
      return r;
      }
    if(o=="||") {
      Dnf r;
      for(auto& t : cond->args)
        r = r || (*this && t);
      return r;
      }
    if(o=="!" && cond->args[0]->kind==Expr::Op) {
      auto& in = cond->args[0];
      if(in->name=="&&") {
        Dnf r;
        for(auto& t : in->args)
          r = r || (*this && ex::lnot(t));
        return r;
        }
      if(in->name=="||") {
        Dnf r = *this;
        for(auto& t : in->args)
          r = r && ex::lnot(t);
        return r;
        }
      }
    }
  return andLiteral(cond);
  }

Dnf Dnf::operator||(const Dnf& other) const {
  if(unknownFlag || other.unknownFlag)
    return unknown();
  Dnf r = *this;
  r.terms.insert(r.terms.end(), other.terms.begin(), other.terms.end());
  r.simplify();
  return r;
  }

static bool subsetOf(const std::vector<ExprP>& a, const std::vector<ExprP>& b) {
  if(a.size()>b.size())
    return false;
  for(auto& x : a)
    if(!contains(b, x->key))
      return false;
  return true;
  }

void Dnf::simplify() {
  for(bool changed=true; changed;) {
    changed = false;
    // absorption: (a) || (a && b) -> (a); also removes duplicates
    for(size_t i=0; i<terms.size() && !changed; ++i)
      for(size_t j=0; j<terms.size() && !changed; ++j)
        if(i!=j && subsetOf(terms[i], terms[j])) {
          terms.erase(terms.begin()+ptrdiff_t(j));
          changed = true;
          }
    // (c && x) || (c && !x) -> c
    for(size_t i=0; i<terms.size() && !changed; ++i) {
      for(size_t j=i+1; j<terms.size() && !changed; ++j) {
        auto& a = terms[i];
        auto& b = terms[j];
        if(a.size()!=b.size())
          continue;
        size_t diffA = a.size(), diffs = 0;
        for(size_t k=0; k<a.size() && diffs<2; ++k)
          if(!contains(b, a[k]->key)) {
            diffA = k;
            diffs++;
            }
        if(diffs!=1)
          continue;
        auto neg = ex::lnot(a[diffA])->key;
        if(!contains(b, neg))
          continue;
        Conj merged = a;
        merged.erase(merged.begin()+ptrdiff_t(diffA));
        terms.erase(terms.begin()+ptrdiff_t(j));
        terms[i] = std::move(merged);
        changed = true;
        }
      }
    }
  if(terms.size()>MaxTerms) {
    terms.clear();
    unknownFlag = true;
    }
  }

ExprP Dnf::expr() const {
  if(unknownFlag)
    return ex::unknown("guard too complex");
  std::vector<ExprP> ors;
  for(auto& c : terms)
    ors.push_back(ex::land(c));
  return ex::lor(std::move(ors));
  }

// ---- Lifter

struct Lifter::State final {
  Dnf                guard;
  std::vector<ExprP> stack;
  // values stored on this path, by the key of the target (a local, global, member or instance variable):
  // a read after a store sees the stored value, like `self.aivar[AIV_PASSGATE]` set and then checked
  std::map<std::string, std::pair<ExprP,ExprP>> stored;  // key -> target, value
  // globals and members a script function called on this path may have written: read afterwards, their
  // value is not known (B_GivePlayerXP raises hero.exp, then the dialog reads it)
  std::map<std::string, std::string> clobberedVars, clobberedMembers;  // name -> the function
  ExprP              inst;  // instance for member access (GMOVI)
  };

static const char* binaryOp(DaedalusOpcode op) {
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

// compound assignments: the operator applied to the old value
static const char* storeOp(DaedalusOpcode op, bool& isStore) {
  isStore = true;
  switch(op) {
    case DaedalusOpcode::ADDMOVI: return "+";
    case DaedalusOpcode::SUBMOVI: return "-";
    case DaedalusOpcode::MULMOVI: return "*";
    case DaedalusOpcode::DIVMOVI: return "/";
    case DaedalusOpcode::MOVI:
    case DaedalusOpcode::MOVS:
    case DaedalusOpcode::MOVSS:
    case DaedalusOpcode::MOVVF:
    case DaedalusOpcode::MOVF:
    case DaedalusOpcode::MOVVI:
      return nullptr;
    default:
      isStore = false;
      return nullptr;
    }
  }

// script parameters declared `var int` that take an instance: B_GIVEINVITEMS.ITEMINSTANCE
static bool instanceParam(const daedalus::DaedalusSymbol& par) {
  auto dot = par.name().find('.');
  return par.type()==DaedalusDataType::INT && dot!=std::string::npos &&
         par.name().find("INSTANCE", dot)!=std::string::npos;
  }

static bool isLocal(const daedalus::DaedalusSymbol& s) {
  return !s.is_member() && s.name().find('.')!=std::string::npos;
  }

static std::string memberName(const std::string& full) {
  auto dot = full.find('.');
  return dot==std::string::npos ? full : full.substr(dot+1);
  }

Lifter::Lifter(const daedalus::DaedalusScript& script, const Bytecode& code)
  :script(script), code(code) {
  // names for array members' indices: aivar[AIV_*], attribute[ATR_*], hitchance[NPC_TALENT_*]
  static const std::pair<const char*,const char*> prefixes[] = {
    {"AIVAR","AIV_"}, {"ATTRIBUTE","ATR_"}, {"HITCHANCE","NPC_TALENT_"}, {"PROTECTION","PROT_"},
    };
  for(auto& s : script.symbols()) {
    if(!s.is_const() || s.type()!=DaedalusDataType::INT || s.count()!=1)
      continue;
    for(auto& [member,prefix] : prefixes)
      if(s.name().rfind(prefix,0)==0)
        indexNames[member].emplace(s.get_int(0), s.name());
    }
  }

static bool storable(const ExprP& e) {
  return e->kind==Expr::Local || e->kind==Expr::Var || e->kind==Expr::Field || e->kind==Expr::Inst;
  }

// A variable not stored on this path. Daedalus locals are static: they keep their value between calls,
// like globals with a function's prefix, so an unassigned local holds whatever was stored last.
static ExprP unstored(const ExprP& e) {
  return e->kind==Expr::Local ? ex::var(e->name) : e;
  }

// A variable read as a value: what this path stored in it, or its value before the function ran.
ExprP Lifter::resolve(const State& s, const ExprP& e) const {
  if(!storable(e))
    return e;
  auto it = s.stored.find(e->key);
  if(it!=s.stored.end())
    return it->second.second;
  auto& clobbered = e->kind==Expr::Field ? s.clobberedMembers : s.clobberedVars;
  if(e->kind==Expr::Var || e->kind==Expr::Field || e->kind==Expr::Inst) {
    auto c = clobbered.find(e->name);
    if(c!=clobbered.end())
      return ex::unknown(e->str() + " after " + c->second);
    }
  return unstored(e);
  }

const Lifter::Clobbers& Lifter::clobbers(const daedalus::DaedalusSymbol& fn) const {
  auto it = clobberCache.find(fn.index());
  if(it!=clobberCache.end())
    return it->second;
  Clobbers c;
  if(clobberBusy.insert(fn.index()).second) {
    auto f = code.summarize(fn.index());
    c.vars = f.writes;
    for(auto& m : f.memberWrites)
      c.members.insert(memberName(m));
    for(auto& name : f.calls)
      if(auto* callee = script.find_symbol_by_name(name)) {
        auto& sub = clobbers(*callee);
        c.vars.insert(sub.vars.begin(), sub.vars.end());
        c.members.insert(sub.members.begin(), sub.members.end());
        }
    clobberBusy.erase(fn.index());
    }
  return clobberCache[fn.index()] = std::move(c);
  }

ExprP Lifter::pushVar(const State& s, const daedalus::DaedalusSymbol& sym, int32_t index) const {
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
        return ex::unknown("constant " + sym.name());
      }
    }
  if(sym.is_member()) {
    auto inst = s.inst ? s.inst : ex::unknown("no instance");
    auto name = memberName(sym.name());
    std::string idxName;
    auto names = indexNames.find(name);
    if(index>=0 && names!=indexNames.end()) {
      auto it = names->second.find(index);
      if(it!=names->second.end())
        idxName = it->second;
      }
    return ex::field(inst, name, index, idxName);
    }
  if(isLocal(sym))
    return ex::local(index>=0 ? sym.name() + "[" + std::to_string(index) + "]" : sym.name());
  if(sym.type()==DaedalusDataType::INSTANCE)
    return ex::inst(sym.name());
  return ex::var(sym.name(), index);
  }

void Lifter::merge(State& a, State&& b) const {
  const Dnf ga = a.guard;
  a.guard = a.guard || b.guard;
  size_t n = std::min(a.stack.size(), b.stack.size());
  std::vector<ExprP> st(n);
  for(size_t i=0; i<n; ++i) {
    auto& x = a.stack[i];
    auto& y = b.stack[i];
    st[i] = x->key==y->key ? x : ex::unknown("value differs between branches");
    }
  a.stack = std::move(st);
  // a variable stored differently on the two branches: the value of the branch that ran.
  // Not stored on a branch, it holds what it held before.
  std::set<std::string> keys;
  for(auto& [k,v] : a.stored) keys.insert(k);
  for(auto& [k,v] : b.stored) keys.insert(k);
  for(auto& k : keys) {
    auto ia = a.stored.find(k);
    auto ib = b.stored.find(k);
    auto t  = ia!=a.stored.end() ? ia->second.first : ib->second.first;
    auto va = ia!=a.stored.end() ? ia->second.second : unstored(t);
    auto vb = ib!=b.stored.end() ? ib->second.second : unstored(t);
    if(va->key==vb->key)
      continue;
    a.stored[k] = {t, mergeValues(ga, va, b.guard, vb, t->str())};
    }
  a.clobberedVars.insert(b.clobberedVars.begin(), b.clobberedVars.end());
  a.clobberedMembers.insert(b.clobberedMembers.begin(), b.clobberedMembers.end());
  if(a.inst==nullptr || b.inst==nullptr || a.inst->key!=b.inst->key)
    a.inst = (a.inst==nullptr && b.inst==nullptr) ? nullptr : ex::unknown("instance differs between branches");
  }

// The value of a variable after two branches meet. A counter (`x = x + 1` on one branch) stays linear:
// x + (case guard -> 1), instead of doubling the tree at every if.
ExprP Lifter::mergeValues(const Dnf& ga, const ExprP& va, const Dnf& gb, const ExprP& vb, const std::string& k) const {
  auto step = [](const ExprP& base, const ExprP& v) -> ExprP {
    if(v->kind!=Expr::Op || v->name!="+" || v->args.size()!=2)
      return nullptr;
    if(v->args[0]->key==base->key) return v->args[1];
    if(v->args[1]->key==base->key) return v->args[0];
    return nullptr;
    };
  ExprP v;
  if(auto d = step(va, vb))
    v = ex::op("+", {va, ex::cases({gb.expr(), d})});
  else if(auto d = step(vb, va))
    v = ex::op("+", {vb, ex::cases({ga.expr(), d})});
  else
    v = ex::cases({ga.expr(), va, gb.expr(), vb});
  // every merge can double the tree (a value that differs under five ifs has 2^5 leaves)
  return v->size<=MaxLocalSize ? v : ex::unknown(k + " too complex");
  }

Lifted Lifter::lift(uint32_t fnSymbol) const {
  Lifted out;
  auto* fn = script.find_symbol_by_index(fnSymbol);
  if(fn==nullptr || !Bytecode::hasCode(*fn)) {
    out.error = "no code";
    return out;
    }
  State entry;
  entry.guard = Dnf::always();
  // the prologue pops the arguments into the parameter symbols
  for(uint32_t i=0; i<fn->count(); ++i)
    entry.stack.push_back(ex::param(int32_t(i)));
  run(*fn, std::move(entry), out, 0, nullptr);
  return out;
  }

bool Lifter::inlines(const daedalus::DaedalusSymbol& fn, uint32_t depth) const {
  if(depth>MaxInline || fn.is_external() || fn.has_return() || !Bytecode::hasCode(fn))
    return false;
  // the model reads these by name: a chapter change, experience, a journal entry, teaching...
  if(fn.name()=="B_KAPITELWECHSEL" || vocab::StandardHelpers.count(fn.name()) || vocab::PredicateHelpers.count(fn.name()))
    return false;
  auto it = inlineCache.find(fn.index());
  if(it!=inlineCache.end())
    return it->second;
  bool quest = false;
  for(auto& v : clobbers(fn).vars)
    quest |= v.rfind("MIS_",0)==0;
  inlineCache[fn.index()] = false;  // while it is lifted
  return inlineCache[fn.index()] = quest && lift(fn.index()).error.empty();
  }

bool Lifter::run(const daedalus::DaedalusSymbol& func, State entry, Lifted& out, uint32_t depth, State* exit) const {
  auto* fn = &func;
  std::vector<daedalus::DaedalusInstruction> ins;
  std::unordered_map<uint32_t, size_t>     at;
  for(uint32_t pc=fn->address(), end=code.codeEnd(pc); pc<end;) {
    auto i = script.instruction_at(pc);
    at[pc] = ins.size();
    ins.push_back(i);
    pc += i.size;
    }
  std::vector<uint32_t> pcs(ins.size());
  for(auto& [pc,i] : at)
    pcs[i] = pc;
  // put in place of a call, its parameters are the caller's arguments: not stores of its own
  std::set<std::string> params;
  if(depth>0)
    for(uint32_t k=0; k<fn->count(); ++k)
      if(auto* par = script.find_symbol_by_index(fn->index()+1+k))
        params.insert(par->name());
  std::vector<State> exits;

  std::vector<State> in(ins.size());
  std::vector<bool>  has(ins.size(), false);
  if(ins.empty()) {
    if(exit!=nullptr)
      *exit = std::move(entry);
    return true;
    }
  in[0]  = std::move(entry);
  has[0] = true;

  auto send = [&](size_t to, State s) {
    if(s.guard.isNever())
      return;
    if(!has[to]) {
      in[to]  = std::move(s);
      has[to] = true;
      } else {
      merge(in[to], std::move(s));
      }
    };
  auto jumpTarget = [&](size_t from, uint32_t address, size_t& to) {
    auto it = at.find(address);
    if(it==at.end()) {
      out.error = "jump outside the function";
      return false;
      }
    if(it->second<=from) {
      out.error = "backward jump";
      return false;
      }
    to = it->second;
    return true;
    };
  auto leave = [&](State&& s, ExprP ret) {
    if(depth==0)
      out.returns.emplace_back(s.guard, std::move(ret));
    else
      exits.push_back(std::move(s));
    };

  for(size_t i=0; i<ins.size(); ++i) {
    if(!has[i])
      continue;
    State s  = std::move(in[i]);
    auto& op = ins[i];
    auto* sym = script.find_symbol_by_index(op.symbol);
    auto  pop = [&s]() {
      if(s.stack.empty())
        return ex::unknown("empty stack");
      auto e = s.stack.back();
      s.stack.pop_back();
      return e;
      };
    bool  last = (i+1==ins.size());

    bool        store = false;
    const char* sop   = storeOp(op.op, store);
    if(store) {
      auto target = pop();
      auto v      = resolve(s, pop());
      if(sop!=nullptr)
        v = ex::op(sop, {resolve(s, target), v});
      if(storable(target))
        s.stored[target->key] = {target, v};
      if(!(target->kind==Expr::Local && params.count(target->name))) {
        Event e;
        e.kind   = Event::Store;
        e.pc     = pcs[i];
        e.target = target;
        e.value  = v;
        e.guard  = s.guard;
        out.events.push_back(std::move(e));
        }
      }
    else if(auto bop = binaryOp(op.op)) {
      auto a = resolve(s, pop());
      auto b = resolve(s, pop());
      s.stack.push_back(ex::op(bop, {a, b}));
      }
    else switch(op.op) {
      case DaedalusOpcode::PLUS:
        break;
      case DaedalusOpcode::NEGATE:
        s.stack.push_back(ex::op("neg", {resolve(s, pop())}));
        break;
      case DaedalusOpcode::NOT:
        s.stack.push_back(ex::lnot(resolve(s, pop())));
        break;
      case DaedalusOpcode::CMPL:
        s.stack.push_back(ex::op("~", {resolve(s, pop())}));
        break;
      case DaedalusOpcode::PUSHI:
        s.stack.push_back(ex::integer(op.immediate));
        break;
      case DaedalusOpcode::PUSHV:
        s.stack.push_back(sym ? pushVar(s, *sym, -1) : ex::unknown("bad symbol"));
        break;
      case DaedalusOpcode::PUSHVV:
        s.stack.push_back(sym ? pushVar(s, *sym, op.index) : ex::unknown("bad symbol"));
        break;
      case DaedalusOpcode::PUSHVI:
        if(sym==nullptr)
          s.stack.push_back(ex::unknown("bad symbol"));
        else if(isLocal(*sym))
          s.stack.push_back(ex::local(sym->name()));
        else
          s.stack.push_back(ex::inst(sym->name()));
        break;
      case DaedalusOpcode::GMOVI:
        if(sym==nullptr)
          s.inst = ex::unknown("bad symbol");
        else
          s.inst = resolve(s, isLocal(*sym) ? ex::local(sym->name()) : ex::inst(sym->name()));
        break;
      case DaedalusOpcode::BE:
      case DaedalusOpcode::BL: {
        auto* callee = op.op==DaedalusOpcode::BE ? sym : code.functionAt(op.address);
        if(callee==nullptr) {
          s.stack.push_back(ex::unknown("bad call"));
          break;
          }
        std::vector<ExprP> args(callee->count());
        for(size_t k=args.size(); k>0; --k)
          args[k-1] = resolve(s, pop());
        for(uint32_t k=0; k<args.size(); ++k) {
          auto& a   = args[k];
          auto* par = script.find_symbol_by_index(callee->index()+1+k);
          bool  ref = par!=nullptr && (par->type()==DaedalusDataType::INSTANCE ||
                                       par->type()==DaedalusDataType::FUNCTION || instanceParam(*par));
          if(a->kind==Expr::Int && a->name.empty() && (ref || vocab::takesInstance(callee->name(), k))) {
            if(auto* r = script.find_symbol_by_index(uint32_t(a->value)))
              a = ex::sym(r->name());
            }
          }
        if(op.op==DaedalusOpcode::BL && inlines(*callee, depth+1)) {
          // its events in place of the call, from this state; the caller goes on from where it returns
          State  inner = s;
          inner.stack  = args;  // its prologue pops them into its parameters
          Lifted mine;
          State  after;
          if(run(*callee, std::move(inner), mine, depth+1, &after)) {
            for(auto& ev : mine.events)
              out.events.push_back(std::move(ev));
            after.stack = std::move(s.stack);
            s = std::move(after);
            break;
            }
          }
        Event e;
        e.kind   = Event::Call;
        e.pc     = pcs[i];
        e.callee = callee;
        e.args   = args;
        e.guard  = s.guard;
        out.events.push_back(std::move(e));
        if(op.op==DaedalusOpcode::BL) {
          // what it may write is no longer what this path stored, nor what was there before
          auto& c = clobbers(*callee);
          for(auto& v : c.vars)
            s.clobberedVars.emplace(v, callee->name());
          for(auto& m : c.members)
            s.clobberedMembers.emplace(m, callee->name());
          for(auto it=s.stored.begin(); it!=s.stored.end();) {
            auto& t = *it->second.first;
            bool  w = ((t.kind==Expr::Var || t.kind==Expr::Inst) && c.vars.count(t.name)) ||
                      (t.kind==Expr::Field && c.members.count(t.name));
            it = w ? s.stored.erase(it) : std::next(it);
            }
          }
        if(callee->has_return())
          s.stack.push_back(op.op==DaedalusOpcode::BE ? ex::call(callee->name(), std::move(args))
                                                      : ex::fn  (callee->name(), std::move(args)));
        break;
        }
      case DaedalusOpcode::BZ: {
        auto   c = resolve(s, pop());
        if(c->size>MaxCondSize)
          c = ex::unknown("condition too complex");  // one object, so both branches still merge back
        size_t to = 0;
        if(!jumpTarget(i, op.address, to))
          return false;
        State jump = s;
        jump.guard = s.guard && ex::lnot(c);
        s.guard    = s.guard && c;
        send(to, std::move(jump));
        if(!last)
          send(i+1, std::move(s));
        continue;
        }
      case DaedalusOpcode::B: {
        size_t to = 0;
        if(!jumpTarget(i, op.address, to))
          return false;
        send(to, std::move(s));
        continue;
        }
      case DaedalusOpcode::RSR: {
        ExprP ret;
        if(fn->has_return())
          ret = s.stack.empty() ? ex::integer(0) : resolve(s, s.stack.back());
        leave(std::move(s), ret);
        continue;
        }
      default:
        break;
      }

    if(last) {
      // fell off the end without RSR
      leave(std::move(s), fn->has_return() ? ex::integer(0) : nullptr);
      } else {
      send(i+1, std::move(s));
      }
    }
    if(exit!=nullptr) {
    if(exits.empty()) {
      *exit       = State();
      exit->guard = Dnf::never();  // it never returns: what follows the call does not run
      } else {
      *exit = std::move(exits[0]);
      for(size_t k=1; k<exits.size(); ++k)
        merge(*exit, std::move(exits[k]));
      }
    }
  return true;
  }

ExprP Lifter::condition(const Lifted& l) {
  if(!l.error.empty())
    return ex::unknown(l.error);
  Dnf r;
  for(auto& [g,ret] : l.returns) {
    if(ret==nullptr)
      continue;
    if(ret->kind==Expr::Int) {
      if(ret->isTrue())
        r = r || g;
      continue;
      }
    r = r || (g && ret);
    }
  return r.expr();
  }

ExprP Lifter::value(const Lifted& l) {
  if(!l.error.empty())
    return ex::unknown(l.error);
  bool boolean = true;
  for(auto& [g,ret] : l.returns)
    if(ret!=nullptr && !(ret->isInt(0) || ret->isInt(1) || ret->isBool()))
      boolean = false;
  if(boolean)
    return condition(l);
  std::vector<ExprP> gv;
  for(auto& [g,ret] : l.returns) {
    if(ret==nullptr || ret->isFalse())
      continue;
    gv.push_back(g.expr());
    gv.push_back(ret);
    }
  return ex::cases(std::move(gv));
  }
