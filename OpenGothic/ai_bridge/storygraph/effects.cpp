#include "effects.h"

#include "bytecode.h"
#include "lifter.h"
#include "model.h"
#include "snapshot.h"

#include "daedalus/vm.h"
#include "daedalus/classes.h"

#include <algorithm>
#include <cstring>
#include <optional>

using namespace storygraph;
using daedalus::DaedalusDataType;

// Engine calls of the dialog's function itself go into `observed`; the rest goes on to the world.
class EffectCheck::Recorder final : public Externals {
  public:
    Recorder(EffectCheck& owner, Externals& world) : owner(owner), world(world) {}

    Value call(const daedalus::DaedalusSymbol& fn, const std::vector<Value>& args) override {
      if(owner.recording && owner.level==0)
        owner.observed.push_back({fn.name(), args});
      return world.call(fn, args);
      }

  private:
    EffectCheck& owner;
    Externals&   world;
  };

struct EffectCheck::Store final {
  daedalus::DaedalusSymbol*                    sym = nullptr;
  std::shared_ptr<daedalus::DaedalusInstance>  of;     // a member store: the instance
  uint16_t                                   index = 0;
  Value                                      value;
  };

namespace {

bool same(const Value& want, const Value& got) {
  if(want.type==got.type) {
    switch(want.type) {
      case Value::Int:   return want.i==got.i;
      case Value::Float: return want.f==got.f;
      case Value::Str:   return want.s==got.s;
      case Value::Inst:  return want.inst==got.inst;
      }
    }
  // the model writes float constants as their bits (PUSHI)
  if(want.type==Value::Int && got.type==Value::Float) {
    int32_t bits = 0;
    std::memcpy(&bits, &got.f, sizeof(bits));
    return bits==want.i;
    }
  // an instance parameter given as its symbol index (PUSHI), as the model writes it
  if(want.type==Value::Int && got.type==Value::Inst)
    return got.inst!=nullptr && int32_t(got.inst->symbol_index())==want.i;
  if(want.type==Value::Inst && got.type==Value::Int)
    return want.inst!=nullptr && int32_t(want.inst->symbol_index())==got.i;
  return false;
  }

// what a guard comes to in the state before the run; nullopt: the model can't tell
std::optional<bool> fires(const Dnf& guard, Evaluator& ev) {
  if(guard.isAlways())
    return true;
  if(guard.isNever())
    return false;
  if(guard.isUnknown())
    return std::nullopt;
  auto v = ev.eval(guard.expr());
  if(!v || v->type!=Value::Int)
    return std::nullopt;
  return v->i!=0;
  }

}

EffectCheck::EffectCheck(daedalus::DaedalusVm& vm, Externals& world)
  :vm(vm), recorder(std::make_unique<Recorder>(*this, world)),
   code(std::make_unique<Bytecode>(vm)), lifter(std::make_unique<Lifter>(vm, *code)),
   snap(std::make_unique<Snapshot>(vm)) {
  for(uint32_t i=0; i<vm.symbols().size(); ++i) {
    auto* s = vm.find_symbol_by_index(i);
    if(s==nullptr)
      continue;
    if(s->type()==DaedalusDataType::FUNCTION && s->is_const() && !s->is_external() && Bytecode::hasCode(*s)) {
      functions[s->name()] = s;
      // the body still runs; only the depth is tracked and the call recorded at the dialog's own level
      vm.override_function(s->name(), [this, s](daedalus::DaedalusVm& v) -> daedalus::DaedalusNakedCall {
        enter(v, *s);
        return {};
        });
      }
    else if(s->type()==DaedalusDataType::INT && !s->is_member() && !s->is_const() &&
            s->name().find('.')==std::string::npos) {
      globals.push_back(s);
      }
    else if(s->type()==DaedalusDataType::INT && s->is_member() && s->name().rfind("C_NPC.", 0)==0) {
      npcInts.push_back(s);
      }
    }
  }

EffectCheck::~EffectCheck() = default;

Externals& EffectCheck::engine() {
  return *recorder;
  }

void EffectCheck::enter(daedalus::DaedalusVm& v, daedalus::DaedalusSymbol& fn) {
  struct Depth final {
    uint32_t& level;
    explicit Depth(uint32_t& l) : level(l) { ++level; }
    ~Depth() { --level; }
    };
  if(recording && level==0 && lifter->inlines(fn, inlined+1)) {
    // the model has its calls and stores in place of the call (Lifter::inlines): they are the dialog's own
    Depth in(inlined);
    v.unsafe_call(&fn);
    return;
    }
  if(recording && level==0) {
    // the arguments are on the stack: read them, then put them back for the function
    auto params = v.find_parameters_for_function(&fn);
    std::vector<Value> args(params.size());
    for(size_t k=params.size(); k>0; --k) {
      switch(params[k-1].type()) {
        case DaedalusDataType::FLOAT:    args[k-1] = Value::real(v.pop_float());        break;
        case DaedalusDataType::STRING:   args[k-1] = Value::str(v.pop_string());        break;
        case DaedalusDataType::INSTANCE: args[k-1] = Value::instance(v.pop_instance()); break;
        default:                         args[k-1] = Value::integer(v.pop_int());       break;
        }
      }
    for(auto& a : args)
      switch(a.type) {
        case Value::Int:   v.push_int(a.i);         break;
        case Value::Float: v.push_float(a.f);       break;
        case Value::Str:   v.push_string(a.s);      break;
        case Value::Inst:  v.push_instance(a.inst); break;
        }
    observed.push_back({fn.name(), std::move(args)});
    }
  Depth depth(level);
  v.unsafe_call(&fn);
  }

daedalus::DaedalusSymbol* EffectCheck::function(const Node& n) {
  auto* sym = vm.find_symbol_by_name(n.id);
  if(sym==nullptr)
    return nullptr;
  if(n.choice)
    return sym->type()==DaedalusDataType::FUNCTION ? sym : nullptr;
  if(sym->type()!=DaedalusDataType::INSTANCE)
    return nullptr;
  auto info = vm.init_instance<daedalus::IInfo>(sym);
  return info->information==0 ? nullptr : vm.find_symbol_by_index(uint32_t(info->information));
  }

const Lifted& EffectCheck::lifted(uint32_t fn) {
  auto& l = liftedFns[fn];
  if(l==nullptr)
    l = std::make_unique<Lifted>(lifter->lift(fn));
  return *l;
  }

const std::set<std::string>& EffectCheck::writes(const std::string& fn) {
  auto it = footprints.find(fn);
  if(it!=footprints.end())
    return it->second;
  auto& w = footprints[fn];
  auto* sym = vm.find_symbol_by_name(fn);
  if(sym==nullptr || busy.count(fn))
    return w;
  busy.insert(fn);
  auto s = code->summarize(sym->index());
  std::set<std::string> all = s.writes;
  for(auto& c : s.calls) {
    auto& sub = writes(c);
    all.insert(sub.begin(), sub.end());
    }
  busy.erase(fn);
  return footprints[fn] = std::move(all);
  }

std::string EffectCheck::text(const Call& c) const {
  std::string r = c.fn + "(";
  for(size_t i=0; i<c.args.size(); ++i) {
    auto t = c.args[i].text(vm);
    if(t.size()>60)
      t = t.substr(0, 60) + "...";
    r += (i ? ", " : "") + t;
    }
  return r + ")";
  }

EffectCheck::Result EffectCheck::run(const Node& n, const std::shared_ptr<daedalus::INpc>& self,
                                     const std::shared_ptr<daedalus::INpc>& hero, Evaluator& ev) {
  Result r;
  auto*  fn = function(n);
  if(fn==nullptr) {
    r.outcome = Outcome::NoFunction;
    return r;
    }
  vm.global_self()->set_instance(self);
  vm.global_other()->set_instance(hero);
  vm.global_hero()->set_instance(hero);

  // what the model says happens, from the state before the run
  auto& l = lifted(fn->index());
  if(!l.error.empty()) {
    r.outcome = Outcome::ModelUnknown;
    r.detail  = "not lifted: " + l.error;
    return r;
    }
  std::vector<Call>     want;
  std::vector<Store>    stores;
  std::set<std::string> calledWrites;  // what the script functions it calls may write
  auto unknown = [&r](std::string why) {
    r.outcome = Outcome::ModelUnknown;
    r.detail  = std::move(why);
    return r;
    };
  for(auto& e : l.events) {
    auto f = fires(e.guard, ev);
    if(!f)
      return unknown("guard: " + (e.guard.isUnknown() ? std::string("too complex") : ev.why()));
    if(!*f)
      continue;
    if(e.kind==Event::Call) {
      Call c{e.callee->name(), {}};
      for(auto& a : e.args) {
        auto v = ev.eval(a);
        if(!v)
          return unknown("argument of " + c.fn + ": " + ev.why());
        c.args.push_back(*v);
        }
      want.push_back(std::move(c));
      if(!e.callee->is_external())
        for(auto& w : writes(e.callee->name()))
          calledWrites.insert(w);
      continue;
      }
    auto& t = *e.target;
    if(t.kind!=Expr::Var && t.kind!=Expr::Field)
      continue;  // instance variables and locals: not compared
    auto v = ev.eval(e.value);
    if(!v)
      return unknown("value of " + t.str() + ": " + ev.why());
    Store s;
    s.index = uint16_t(std::max(t.index, 0));
    s.value = *v;
    if(t.kind==Expr::Var) {
      s.sym = vm.find_symbol_by_name(t.name);
      } else {
      auto of = ev.eval(t.args[0]);
      if(!of || of->type!=Value::Inst)
        return unknown("instance of " + t.str() + ": " + ev.why());
      s.of  = of->inst;
      s.sym = of->inst ? member(vm, *of->inst, t.name) : nullptr;
      }
    if(s.sym==nullptr || s.sym->type()!=DaedalusDataType::INT)
      continue;
    // the last store to a variable is the one that stays
    for(auto it=stores.begin(); it!=stores.end(); ++it)
      if(it->sym==s.sym && it->of==s.of && it->index==s.index) {
        stores.erase(it);
        break;
        }
    stores.push_back(std::move(s));
    }

  // the VM runs it
  snap->take();
  std::vector<int32_t> before;
  for(auto* g : globals)
    for(uint16_t i=0; i<g->count(); ++i)
      before.push_back(g->get_int(i));
  std::vector<std::pair<std::shared_ptr<daedalus::INpc>,std::vector<int32_t>>> npcs;
  for(auto& who : {self, hero}) {
    if(who==nullptr)
      continue;
    std::vector<int32_t> vals;
    for(auto* m : npcInts)
      for(uint16_t i=0; i<m->count(); ++i)
        vals.push_back(m->get_int(i, who.get()));
    npcs.emplace_back(who, std::move(vals));
    }
  auto restore = [&]() {
    snap->restore();
    for(auto& [who, vals] : npcs) {
      size_t k = 0;
      for(auto* m : npcInts)
        for(uint16_t i=0; i<m->count(); ++i)
          m->set_int(vals[k++], i, who.get());
      }
    };

  observed.clear();
  recording = true;
  std::string fault;
  try {
    vm.unsafe_call(fn);
    }
  catch(const std::exception& e) {
    fault = e.what();
    }
  recording = false;
  if(!fault.empty()) {
    restore();
    r.outcome = Outcome::VmFailed;
    r.detail  = fault;
    return r;
    }

  if(trace) {
    trace(ident(n.id) + ", model:");
    for(auto& c : want)
      trace("  " + text(c));
    trace("vm:");
    for(auto& c : observed)
      trace("  " + text(c));
    }

  // the calls, in order
  r.calls = std::min(want.size(), observed.size());
  for(size_t i=0; i<std::max(want.size(), observed.size()) && r.outcome==Outcome::Agree; ++i) {
    if(i>=want.size()) {
      r.outcome = Outcome::Differ;
      r.detail  = "call " + std::to_string(i) + ": vm " + text(observed[i]) + ", model nothing more";
      }
    else if(i>=observed.size()) {
      r.outcome = Outcome::Differ;
      r.detail  = "call " + std::to_string(i) + ": model " + text(want[i]) + ", vm nothing more";
      }
    else {
      bool ok = want[i].fn==observed[i].fn && want[i].args.size()==observed[i].args.size();
      for(size_t k=0; ok && k<want[i].args.size(); ++k)
        ok = same(want[i].args[k], observed[i].args[k]);
      if(!ok) {
        r.outcome = Outcome::Differ;
        r.detail  = "call " + std::to_string(i) + ": model " + text(want[i]) + ", vm " + text(observed[i]);
        }
      }
    }

  // the stores the model says stay, unless a script function it calls may write the same variable
  for(auto& s : stores) {
    if(r.outcome!=Outcome::Agree)
      break;
    if(s.of==nullptr && calledWrites.count(s.sym->name()))
      continue;
    const int32_t got = s.of ? s.sym->get_int(s.index, s.of.get()) : s.sym->get_int(s.index);
    if(!same(s.value, Value::integer(got))) {
      r.outcome = Outcome::Differ;
      r.detail  = "store " + ident(s.sym->name()) + (s.index ? "[" + std::to_string(s.index) + "]" : "") +
                  ": model " + s.value.text(vm) + ", vm " + std::to_string(got);
      }
    }
  // and no write the model does not explain
  size_t k = 0;
  for(auto* g : globals) {
    for(uint16_t i=0; i<g->count(); ++i, ++k) {
      if(r.outcome!=Outcome::Agree || g->get_int(i)==before[k] || calledWrites.count(g->name()))
        continue;
      bool stored = false;
      for(auto& s : stores)
        stored |= s.of==nullptr && s.sym==g && s.index==i;
      if(!stored) {
        r.outcome = Outcome::Differ;
        r.detail  = "vm wrote " + ident(g->name()) + (i ? "[" + std::to_string(i) + "]" : "") + " = " +
                    std::to_string(g->get_int(i)) + " (was " + std::to_string(before[k]) + "), the model no store";
        }
      }
    }
  restore();
  return r;
  }
