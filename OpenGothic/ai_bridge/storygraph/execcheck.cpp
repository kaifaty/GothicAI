#include "execcheck.h"

#include "bytecode.h"
#include "exprjson.h"
#include "model.h"
#include "snapshot.h"

#include <zenkit/DaedalusVm.hh>
#include <zenkit/addon/daedalus.hh>

#include <algorithm>
#include <cstdio>
#include <cstring>

using namespace storygraph;
using zenkit::DaedalusDataType;

// ---- Ledger

int32_t Ledger::has(const std::vector<Value>& npcAndItem) {
  if(hasItemsFn==nullptr)
    return 0;
  auto base = world.call(*hasItemsFn, npcAndItem);
  auto it   = items.find({npcAndItem[0].inst.get(), npcAndItem[1].i});
  return std::max(0, (base.type==Value::Int ? base.i : 0) + (it==items.end() ? 0 : it->second));
  }

Value Ledger::call(const zenkit::DaedalusSymbol& fn, const std::vector<Value>& args) {
  auto& n = fn.name();
  const bool create = (n=="CREATEINVITEMS" || n=="CREATEINVITEM");
  const bool remove = (n=="NPC_REMOVEINVITEMS" || n=="NPC_REMOVEINVITEM");
  if((create || remove) && args.size()>=2 && args[0].type==Value::Inst && args[0].inst!=nullptr &&
     args[1].type==Value::Int) {
    int32_t amount = args.size()>=3 && args[2].type==Value::Int ? args[2].i : 1;
    auto    key    = std::make_pair(static_cast<const zenkit::DaedalusInstance*>(args[0].inst.get()), args[1].i);
    if(create) {
      items[key] += amount;
      } else {
      // the engine takes what there is
      int32_t now = has({args[0], args[1]});
      items[key] -= std::min(std::max(amount, 0), now);
      }
    return world.call(fn, args);
    }
  if(n=="NPC_HASITEMS" && args.size()==2 && args[0].type==Value::Inst && args[0].inst!=nullptr && args[1].type==Value::Int)
    return Value::integer(has(args));
  return world.call(fn, args);
  }

// ---- ExecCheck

class ExecCheck::Recorder final : public Externals {
  public:
    explicit Recorder(Externals& next) : next(next) {}

    // calls made inside another one are the world's own (it builds an npc the first time it is asked for)
    Value call(const zenkit::DaedalusSymbol& fn, const std::vector<Value>& args) override {
      depth++;
      auto r = next.call(fn, args);
      depth--;
      if(on && depth==0)
        calls.push_back({fn.name(), args, r});
      return r;
      }

    bool              on = false;
    std::vector<Call> calls;

  private:
    Externals& next;
    uint32_t   depth = 0;
  };

struct ExecCheck::State final {
  explicit State(zenkit::DaedalusVm& vm) : globals(vm) {}
  Snapshot                                                                     globals;
  std::vector<std::pair<std::shared_ptr<zenkit::DaedalusInstance>,std::vector<Value>>> insts;
  std::map<std::pair<const zenkit::DaedalusInstance*,int32_t>,int32_t>        items;
  };

namespace {

bool same(const Value& a, const Value& b) {
  if(a.type!=b.type)
    return false;
  switch(a.type) {
    case Value::Int:   return a.i==b.i;
    case Value::Float: return std::memcmp(&a.f, &b.f, sizeof(float))==0;
    case Value::Str:   return a.s==b.s;
    case Value::Inst:  return a.inst==b.inst;
    }
  return false;
  }

}

ExecCheck::ExecCheck(zenkit::DaedalusVm& vm, Externals& world)
  :vm(vm), book(std::make_unique<Ledger>(world, vm.find_symbol_by_name("NPC_HASITEMS"))),
   recorder(std::make_unique<Recorder>(*book)), code(std::make_unique<Bytecode>(vm)),
   interp(std::make_unique<Interpreter>(vm, *recorder)),
   before(std::make_unique<State>(vm)), afterVm(std::make_unique<State>(vm)), afterProgram(std::make_unique<State>(vm)) {
  }

ExecCheck::~ExecCheck() = default;

Externals& ExecCheck::engine() {
  return *recorder;
  }

const Program& ExecCheck::program(uint32_t fnSymbol) {
  auto it = programs.find(fnSymbol);
  if(it!=programs.end())
    return it->second;
  auto p = liftProgram(vm, *code, fnSymbol);
  if(viaJson && p.error.empty()) {
    rapidjson::Document doc;
    doc.Parse(toJson(p).c_str());
    auto back = readProgram(doc, p.returns);
    if(back.error.empty() && describe(back)!=describe(p))
      back.error = "reads back otherwise";
    if(!back.error.empty())
      back.error = "json: " + back.error;
    p = std::move(back);
    }
  return programs[fnSymbol] = std::move(p);
  }

// instances the function and the script functions it calls name
const std::set<std::string>& ExecCheck::reach(uint32_t fnSymbol) {
  auto it = reaches.find(fnSymbol);
  if(it!=reaches.end())
    return it->second;
  std::set<std::string> r;
  if(busy.insert(fnSymbol).second) {
    auto f = code->summarize(fnSymbol);
    r = f.instances;
    for(auto& c : f.calls)
      if(auto* callee = vm.find_symbol_by_name(c)) {
        auto& sub = reach(callee->index());
        r.insert(sub.begin(), sub.end());
        }
    busy.erase(fnSymbol);
    }
  return reaches[fnSymbol] = std::move(r);
  }

std::vector<std::shared_ptr<zenkit::DaedalusInstance>> ExecCheck::watched(uint32_t fnSymbol,
                                                                         const std::shared_ptr<zenkit::INpc>& self,
                                                                         const std::shared_ptr<zenkit::INpc>& hero) {
  std::vector<std::shared_ptr<zenkit::DaedalusInstance>> r;
  auto add = [&r](std::shared_ptr<zenkit::DaedalusInstance> i) {
    if(i!=nullptr && std::find(r.begin(), r.end(), i)==r.end())
      r.push_back(std::move(i));
    };
  add(self);
  add(hero);
  for(auto& name : reach(fnSymbol)) {
    auto* s = vm.find_symbol_by_name(name);
    if(s==nullptr || s->type()!=DaedalusDataType::INSTANCE)
      continue;
    if(!s->is_const())
      add(s->get_instance());  // an instance variable: what it holds
    else if(npcOf)
      add(npcOf(s->index()));  // an npc of the world
    }
  return r;
  }

const std::vector<zenkit::DaedalusSymbol*>& ExecCheck::membersOf(const zenkit::DaedalusInstance& inst) {
  auto* cls = vm.find_symbol_by_index(inst.symbol_index());
  for(int i=0; i<8 && cls!=nullptr && cls->type()!=DaedalusDataType::CLASS; ++i)
    cls = vm.find_symbol_by_index(cls->parent());
  static const std::vector<zenkit::DaedalusSymbol*> none;
  if(cls==nullptr)
    return none;
  auto it = members.find(cls->index());
  if(it!=members.end())
    return it->second;
  auto& m = members[cls->index()];
  const std::string prefix = cls->name() + ".";
  for(uint32_t i=0; i<vm.symbols().size(); ++i) {
    auto* s = vm.find_symbol_by_index(i);
    if(s==nullptr || !s->is_member() || s->name().rfind(prefix, 0)!=0)
      continue;
    auto t = s->type();
    if(t==DaedalusDataType::INT || t==DaedalusDataType::FLOAT || t==DaedalusDataType::STRING)
      m.push_back(s);
    }
  return m;
  }

void ExecCheck::take(State& s, const std::vector<std::shared_ptr<zenkit::DaedalusInstance>>& insts) {
  s.globals.take();
  s.insts.clear();
  for(auto& inst : insts) {
    std::vector<Value> v;
    for(auto* m : membersOf(*inst))
      for(uint16_t i=0; i<m->count(); ++i)
        switch(m->type()) {
          case DaedalusDataType::FLOAT:  v.push_back(Value::real(m->get_float(i, inst.get()))); break;
          case DaedalusDataType::STRING: v.push_back(Value::str(m->get_string(i, inst.get()))); break;
          default:                       v.push_back(Value::integer(m->get_int(i, inst.get()))); break;
          }
    s.insts.emplace_back(inst, std::move(v));
    }
  s.items = book->items;
  }

void ExecCheck::restore(State& s) {
  s.globals.restore();
  for(auto& [inst, v] : s.insts) {
    size_t k = 0;
    for(auto* m : membersOf(*inst))
      for(uint16_t i=0; i<m->count(); ++i, ++k)
        switch(m->type()) {
          case DaedalusDataType::FLOAT:  m->set_float(v[k].f, i, inst.get());  break;
          case DaedalusDataType::STRING: m->set_string(v[k].s, i, inst.get()); break;
          default:                       m->set_int(v[k].i, i, inst.get());    break;
          }
    }
  book->items = s.items;
  }

std::string ExecCheck::text(const Call& c) const {
  std::string r = ident(c.fn) + "(";
  for(size_t i=0; i<c.args.size(); ++i) {
    auto t = c.args[i].text(vm);
    if(t.size()>60)
      t = t.substr(0, 60) + "...";
    r += (i ? ", " : "") + t;
    }
  r += ")";
  auto* fn = vm.find_symbol_by_name(c.fn);
  if(fn!=nullptr && fn->has_return())
    r += " = " + c.ret.text(vm);
  return r;
  }

ExecCheck::Result ExecCheck::run(uint32_t fnSymbol, const std::shared_ptr<zenkit::INpc>& self,
                                 const std::shared_ptr<zenkit::INpc>& hero) {
  Result r;
  auto*  fn = vm.find_symbol_by_index(fnSymbol);
  auto&  p  = program(fnSymbol);
  if(fn==nullptr || !p.error.empty()) {
    r.outcome = Outcome::NotProgram;
    r.detail  = fn==nullptr ? std::string("no function") : p.error;
    return r;
    }
  auto setGlobals = [&]() {
    vm.global_self()->set_instance(self);
    vm.global_other()->set_instance(hero);
    vm.global_hero()->set_instance(hero);
    };
  setGlobals();
  auto insts = watched(fnSymbol, self, hero);
  book->reset();
  take(*before, insts);

  // the VM runs the function
  if(vmFault)
    vmFault->clear();
  recorder->calls.clear();
  recorder->on = true;
  Value       vmRet;
  std::string vmError;
  try {
    std::string why;
    auto v = callFunction(vm, *fn, {}, why);
    if(v)
      vmRet = *v;
    else
      vmError = why;
    }
  catch(const std::exception& e) {
    vmError = e.what();
    }
  recorder->on = false;
  if(vmError.empty() && vmFault && !vmFault->empty())
    vmError = *vmFault;
  auto vmCalls = std::move(recorder->calls);
  take(*afterVm, insts);
  restore(*before);
  setGlobals();

  // the Interpreter runs the program
  if(vmFault)
    vmFault->clear();
  recorder->calls.clear();
  recorder->on = true;
  auto res = interp->run(p);
  recorder->on = false;
  std::string progError = res.ok ? std::string() : res.error;
  if(progError.empty() && vmFault && !vmFault->empty())
    progError = "a script function failed in the VM: " + *vmFault;
  auto progCalls = std::move(recorder->calls);
  take(*afterProgram, insts);
  restore(*before);
  setGlobals();

  if(trace) {
    trace(ident(fn->name()) + ", vm:");
    for(auto& c : vmCalls)
      trace("  " + text(c));
    trace("program:");
    for(auto& c : progCalls)
      trace("  " + text(c));
    }

  if(!vmError.empty()) {
    r.outcome = Outcome::VmFailed;
    r.detail  = vmError;
    return r;
    }
  auto differ = [&r](std::string why) {
    r.outcome = Outcome::Differ;
    r.detail  = std::move(why);
    return r;
    };
  if(!progError.empty())
    return differ("program: " + progError);

  // the engine calls, in order
  r.calls = std::min(vmCalls.size(), progCalls.size());
  for(size_t i=0; i<std::max(vmCalls.size(), progCalls.size()); ++i) {
    if(i>=progCalls.size())
      return differ("call " + std::to_string(i) + ": vm " + text(vmCalls[i]) + ", program nothing more");
    if(i>=vmCalls.size())
      return differ("call " + std::to_string(i) + ": program " + text(progCalls[i]) + ", vm nothing more");
    auto& a = vmCalls[i];
    auto& b = progCalls[i];
    bool  ok = a.fn==b.fn && a.args.size()==b.args.size() && same(a.ret, b.ret);
    for(size_t k=0; ok && k<a.args.size(); ++k)
      ok = same(a.args[k], b.args[k]);
    if(!ok)
      return differ("call " + std::to_string(i) + ": vm " + text(a) + ", program " + text(b));
    }
  if(fn->has_return() && !same(vmRet, res.returned))
    return differ("returned: vm " + vmRet.text(vm) + ", program " + res.returned.text(vm));
  if(auto d = afterVm->globals.differs(afterProgram->globals); !d.empty())
    return differ("variable " + ident(d) + " (vm, program)");
  for(size_t k=0; k<afterVm->insts.size(); ++k) {
    auto& [inst, a] = afterVm->insts[k];
    auto& b         = afterProgram->insts[k].second;
    for(size_t i=0; i<a.size(); ++i)
      if(!same(a[i], b[i])) {
        auto* s = vm.find_symbol_by_index(inst->symbol_index());
        return differ("member " + std::to_string(i) + " of " + (s ? ident(s->name()) : std::string("?")) + ": vm " +
                      a[i].text(vm) + ", program " + b[i].text(vm));
        }
    }
  if(afterVm->items!=afterProgram->items)
    return differ("inventory after the run");
  return r;
  }

// ---- tallies

void ExecTotals::add(const std::string& fn, uint32_t fnSymbol, const ExecCheck::Result& r) {
  runs++;
  calls += r.calls;
  switch(r.outcome) {
    case ExecCheck::Outcome::Agree:  agree++; break;
    case ExecCheck::Outcome::Differ: if(differ++<1000) diffs.emplace_back(fn, r.detail); break;
    case ExecCheck::Outcome::VmFailed: vmFailed++; break;
    case ExecCheck::Outcome::NotProgram:
      notProgram++;
      if(notPrograms.insert(fnSymbol).second)
        notProgramWhy[r.detail]++;
      break;
    }
  }

void ExecTotals::merge(const ExecTotals& o) {
  runs += o.runs; calls += o.calls; agree += o.agree; differ += o.differ; notProgram += o.notProgram;
  vmFailed += o.vmFailed;
  notPrograms.insert(o.notPrograms.begin(), o.notPrograms.end());
  // the same functions on every save: the larger count
  for(auto& [k,v] : o.notProgramWhy)
    notProgramWhy[k] = std::max(notProgramWhy[k], v);
  diffs.insert(diffs.end(), o.diffs.begin(), o.diffs.end());
  }

void ExecTotals::print(const char* what, bool verbose) const {
  std::printf("   programs (%s): %zu runs, %zu engine calls compared; agree %zu, DIFFER %zu; VM failed %zu; "
              "not a program %zu (%zu functions)\n", what, runs, calls, agree, differ, vmFailed, notProgram,
              notPrograms.size());
  for(auto& [why,c] : notProgramWhy)
    std::printf("     not a program: %zu x %s\n", c, why.c_str());
  for(size_t i=0; i<diffs.size() && (verbose || i<20); ++i)
    std::printf("   PROGRAM DIFF %s: %s\n", ident(diffs[i].first).c_str(), diffs[i].second.c_str());
  if(diffs.size()>20 && !verbose)
    std::printf("   ... %zu more (--verbose)\n", diffs.size()-20);
  }

std::vector<uint32_t> storygraph::nodeFunctions(zenkit::DaedalusVm& vm, const Node& n) {
  std::vector<uint32_t> r;
  auto* sym = vm.find_symbol_by_name(n.id);
  if(sym==nullptr)
    return r;
  if(n.choice) {
    if(sym->type()==DaedalusDataType::FUNCTION)
      r.push_back(sym->index());
    return r;
    }
  if(sym->type()!=DaedalusDataType::INSTANCE)
    return r;
  auto info = vm.init_instance<zenkit::IInfo>(sym);
  if(info->condition!=0)
    r.push_back(uint32_t(info->condition));
  if(info->information!=0)
    r.push_back(uint32_t(info->information));
  return r;
  }
