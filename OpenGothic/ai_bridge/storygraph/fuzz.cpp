// Random states for the check of stage 2 (docs/story-graph.md). A save shows each dialog in one state, so
// most branches of its condition never run. Here every condition is computed in many random states: the
// variables and members it reads and the answers of the engine are drawn from the constants it compares
// against and their neighbours. The VM and the model see the same draws, so a difference is again an
// error of the lifting. What is drawn comes from the bytecode, not from the model: an input the lifting
// lost must still vary.

#include "bytecode.h"
#include "check.h"
#include "effects.h"
#include "eval.h"
#include "execcheck.h"
#include "exprjson.h"
#include "model.h"
#include "vocab.h"

#include "daedalus/vm.h"
#include <zenkit/Logger.hh>
#include "daedalus/classes.h"

#include <cstdio>
#include <functional>
#include <memory>
#include <random>
#include <set>

using namespace storygraph;
using daedalus::DaedalusDataType;

namespace {

// What the draws are made of, for one condition and the script functions it calls.
struct Domain final {
  std::vector<int32_t>     ints;
  std::vector<std::string> strs;
  std::set<std::string>    vars;     // globals and static locals
  std::set<std::string>    members;  // C_NPC members, all elements are drawn
  std::set<std::string>    insts;    // instances and instance variables
  };

void collect(daedalus::DaedalusVm& vm, const Bytecode& code, uint32_t fnSymbol, Domain& d, std::set<uint32_t>& seen,
             std::set<int32_t>& ints, std::set<std::string>& strs) {
  auto* fn = vm.find_symbol_by_index(fnSymbol);
  if(fn==nullptr || !Bytecode::hasCode(*fn) || !seen.insert(fnSymbol).second)
    return;
  auto f = code.summarize(fnSymbol);
  d.vars.insert(f.reads.begin(), f.reads.end());
  d.insts.insert(f.instances.begin(), f.instances.end());
  for(auto& m : f.members)
    d.members.insert(m.substr(m.find('.')+1));
  // static locals: a condition may read what an earlier call left there
  for(auto& l : vm.find_locals_for_function(fn))
    d.vars.insert(l.name());
  // the constants it compares against, and their neighbours
  for(uint32_t pc=fn->address(), end=code.codeEnd(pc); pc<end;) {
    auto  in = vm.instruction_at(pc);
    auto* s  = vm.find_symbol_by_index(in.symbol);
    if(in.op==daedalus::DaedalusOpcode::PUSHI) {
      for(int32_t k : {-1, 0, 1})
        ints.insert(int32_t(uint32_t(in.immediate) + uint32_t(k)));
      }
    else if((in.op==daedalus::DaedalusOpcode::PUSHV || in.op==daedalus::DaedalusOpcode::PUSHVV) && s && s->is_const()) {
      auto i = uint16_t(in.op==daedalus::DaedalusOpcode::PUSHVV ? in.index : 0);
      if(s->type()==DaedalusDataType::INT && i<s->count())
        for(int32_t k : {-1, 0, 1})
          ints.insert(int32_t(uint32_t(s->get_int(i)) + uint32_t(k)));
      else if(s->type()==DaedalusDataType::STRING && i<s->count())
        strs.insert(s->get_string(i));
      }
    pc += in.size;
    }
  for(auto& c : f.calls)
    if(auto* callee = vm.find_symbol_by_name(c))
      collect(vm, code, callee->index(), d, seen, ints, strs);
  }

Domain domainOf(daedalus::DaedalusVm& vm, const Bytecode& code, uint32_t condition) {
  Domain                d;
  std::set<uint32_t>    seen;
  std::set<int32_t>     ints = {-1, 0, 1, 2};
  std::set<std::string> strs = {"", "X"};
  collect(vm, code, condition, d, seen, ints, strs);
  d.ints.assign(ints.begin(), ints.end());
  d.strs.assign(strs.begin(), strs.end());
  return d;
  }

// The engine answers at random, the same answer for the same question within a round.
class FuzzWorld final : public Externals {
  public:
    explicit FuzzWorld(daedalus::DaedalusVm& vm) : vm(vm) {}

    Value call(const daedalus::DaedalusSymbol& fn, const std::vector<Value>& args) override {
      if(!fn.has_return())
        return {};
      if(setup || domain==nullptr)
        return fn.name()=="HLP_GETNPC" ? Value::instance(npc(uint32_t(args[0].i))) : Value();
      std::string key = fn.name() + "(";
      for(size_t i=0; i<args.size(); ++i) {
        auto& a = args[i];
        key += i ? "," : "";
        switch(a.type) {
          case Value::Int:   key += std::to_string(a.i); break;
          case Value::Float: key += std::to_string(a.f); break;
          case Value::Str:   key += "\"" + a.s + "\""; break;
          case Value::Inst:  key += a.inst ? "#" + std::to_string(a.inst->symbol_index()) : "null"; break;
          }
        }
      key += ")";
      auto it = memo.find(key);
      if(it!=memo.end())
        return it->second;
      Value v;
      switch(fn.rtype()) {
        case DaedalusDataType::STRING:
          v = Value::str(str());
          break;
        case DaedalusDataType::INSTANCE:
          if(fn.name()=="HLP_GETNPC")
            v = Value::instance(rng()%4==0 ? nullptr : npc(uint32_t(args[0].i)));
          else
            v = Value::instance(rng()%3==0 ? nullptr : rng()%2 ? hero : self);
          break;
        case DaedalusDataType::FLOAT:
          v = Value::real(0);
          break;
        default:
          // predicates answer TRUE or FALSE: `pred == TRUE` is folded to `pred` on that promise
          v = Value::integer(vocab::BoolExternals.count(fn.name()) ? int32_t(rng()%2) : num());
          break;
        }
      return memo[key] = v;
      }

    // an npc built from its instance, one per instance
    std::shared_ptr<daedalus::INpc> npc(uint32_t symbol) {
      auto* sym = vm.find_symbol_by_index(symbol);
      if(sym!=nullptr && sym->type()==DaedalusDataType::INSTANCE && !sym->is_const()) {
        // an instance variable (Hlp_GetNpc(Attila)): the npc it holds. Its "constructor" address is junk:
        // building an instance from it runs whatever bytes are there
        return std::dynamic_pointer_cast<daedalus::INpc>(sym->get_instance());
        }
      auto it = npcs.find(symbol);
      if(it!=npcs.end())
        return it->second;
      npcs[symbol] = nullptr;  // an instance function asking for itself
      std::shared_ptr<daedalus::INpc> r;
      if(sym!=nullptr && sym->type()==DaedalusDataType::INSTANCE && isNpc(*sym)) {
        bool was = setup;
        setup = true;
        try {
          r = vm.init_instance<daedalus::INpc>(sym);
          }
        catch(const std::exception&) {
          r = nullptr;
          }
        setup = was;
        }
      return npcs[symbol] = r;
      }

    // an instance of C_NPC (an item or a dialog built as an npc would write past its object)
    bool isNpc(const daedalus::DaedalusSymbol& sym) const {
      auto* s = &sym;
      for(int i=0; i<8 && s!=nullptr && s->type()!=DaedalusDataType::CLASS; ++i)
        s = vm.find_symbol_by_index(s->parent());
      return s!=nullptr && s->name()=="C_NPC";
      }

    // flags are TRUE or FALSE most of the time, so 0 and 1 come up often; the rest is the constants
    int32_t num() {
      auto r = rng()%10;
      return r<4 ? 0 : r<6 ? 1 : domain->ints[rng()%domain->ints.size()];
      }
    std::string str() {
      return rng()%2 ? std::string() : domain->strs[rng()%domain->strs.size()];
      }

    daedalus::DaedalusVm&                                         vm;
    std::mt19937                                                rng;
    const Domain*                                               domain = nullptr;
    std::map<std::string,Value>                                 memo;
    std::shared_ptr<daedalus::INpc>                               hero, self;
    bool                                                        setup = true;

  private:
    std::unordered_map<uint32_t,std::shared_ptr<daedalus::INpc>>  npcs;
  };

struct Mismatch final {
  const Node* node;
  int32_t     vm, model;
  std::string state;
  };


// The draws of one round: the variables the function reads, and the members of the instances it reads.
void draw(daedalus::DaedalusVm& vm, FuzzWorld& world, const Domain& d,
          const std::vector<std::shared_ptr<daedalus::DaedalusInstance>>& owners) {
  world.memo.clear();
  for(auto& v : d.vars) {
    auto* s = vm.find_symbol_by_name(v);
    if(s==nullptr || s->is_const() || s->is_member())
      continue;
    for(uint16_t i=0; i<s->count(); ++i) {
      if(s->type()==DaedalusDataType::INT)
        s->set_int(world.num(), i);
      else if(s->type()==DaedalusDataType::STRING)
        s->set_string(world.str(), i);
      }
    }
  for(auto& o : owners) {
    if(o==nullptr)
      continue;
    for(auto& name : d.members) {
      auto* mem = member(vm, *o, name);
      if(mem==nullptr)
        continue;
      for(uint16_t i=0; i<mem->count(); ++i) {
        if(mem->type()==DaedalusDataType::INT)
          mem->set_int(world.num(), i, o.get());
        else if(mem->type()==DaedalusDataType::STRING)
          mem->set_string(world.str(), i, o.get());
        }
      }
    }
  }

// instances whose members are drawn: the npc, the hero and the instance variables the function reads
std::vector<std::shared_ptr<daedalus::DaedalusInstance>> ownersOf(daedalus::DaedalusVm& vm, const FuzzWorld& world,
                                                                const Domain& d) {
  std::vector<std::shared_ptr<daedalus::DaedalusInstance>> owners = {world.self, world.hero};
  for(auto& name : d.insts)
    if(auto* s = vm.find_symbol_by_name(name); s && s->type()==DaedalusDataType::INSTANCE && s->get_instance() &&
       !s->is_const())
      owners.push_back(s->get_instance());
  return owners;
  }

// The VM over FuzzWorld, with the hero and the npc globals set up as at the start of a game.
struct Fuzz final {
  std::unique_ptr<daedalus::DaedalusVm> vm;
  std::unique_ptr<FuzzWorld>          world;
  std::string                         vmFault;

  bool open(const char* datPath, const std::function<void(daedalus::DaedalusVm&, FuzzWorld&)>& bind) {
    zenkit::Logger::use_logger([](zenkit::LogLevel, const std::string&){});
    vm = loadVm(datPath);
    if(vm==nullptr) {
      std::fprintf(stderr, "can't open %s\n", datPath);
      return false;
      }
    world = std::make_unique<FuzzWorld>(*vm);
    bind(*vm, *world);
    vm->register_exception_handler([this](daedalus::DaedalusVm&, const daedalus::DaedalusScriptError& e,
                                          const daedalus::DaedalusInstruction&) {
      if(vmFault.empty())
        vmFault = e.what();
      return daedalus::DaedalusVmExceptionStrategy::RETURN;
      });
    if(auto* h = vm->find_symbol_by_name("PC_HERO"))
      world->hero = world->npc(h->index());
    vm->global_hero()->set_instance(world->hero);
    if(auto* init = vm->find_symbol_by_name("B_INITNPCGLOBALS"))
      vm->call_function(init);
    world->setup = false;
    return true;
    }
  };

}

namespace storygraph {

int fuzzConditions(const char* datPath, const Model& m, uint32_t rounds, const char* only, bool verbose) {
  Fuzz fz;
  if(!fz.open(datPath, [](daedalus::DaedalusVm& v, FuzzWorld& w){ bindExternals(v, w); }))
    return 1;
  auto&        vm      = fz.vm;
  FuzzWorld&   world   = *fz.world;
  std::string& vmFault = fz.vmFault;

  // conditions go to the bridge as JSON: they must read back as they were
  size_t roundTrips = 0, roundTripErrors = 0;
  for(auto& n : m.nodes) {
    if(n.choice || !n.notLifted.empty())
      continue;
    rapidjson::Document doc;
    auto        text = toJson(n.condition);
    std::string error;
    doc.Parse(text.c_str(), text.size());
    auto back = doc.HasParseError() ? nullptr : readExpr(doc, error);
    roundTrips++;
    if(back==nullptr || back->key!=n.condition->key) {
      if(roundTripErrors++<5)
        std::printf("JSON round trip of %s: %s\n", ident(n.id).c_str(), back ? "differs" : error.c_str());
      }
    }
  std::printf("JSON round trip: %zu conditions, %zu differ\n", roundTrips, roundTripErrors);

  // the model asks the engine through the VM, as the bridge does in the game
  Bytecode              code(*vm);
  VmExternals           engine(*vm);
  Evaluator             ev(*vm, m.helpers, engine);
  std::vector<Mismatch> found;
  size_t dialogs = 0, runs = 0, vmFaults = 0, modelFails = 0, bothTrue = 0, bothFalse = 0;
  std::map<std::string,size_t> failWhy;

  for(auto& n : m.nodes) {
    world.domain = nullptr;  // the previous dialog's is gone
    if(n.choice || !n.notLifted.empty() || (only!=nullptr && n.id!=only))
      continue;
    auto* infoSym = vm->find_symbol_by_name(n.id);
    if(infoSym==nullptr)
      continue;
    auto info = vm->init_instance<daedalus::IInfo>(infoSym);
    if(info->condition==0)
      continue;
    auto* cond = vm->find_symbol_by_index(uint32_t(info->condition));
    dialogs++;

    Domain d = domainOf(*vm, code, uint32_t(info->condition));
    world.domain = &d;
    world.self   = world.npc(uint32_t(info->npc));
    world.rng.seed(uint32_t(std::hash<std::string>()(n.id)));

    auto owners = ownersOf(*vm, world, d);
    for(uint32_t r=0; r<rounds; ++r) {
      draw(*vm, world, d, owners);
      vm->global_self()->set_instance(world.self);
      vm->global_other()->set_instance(world.hero);
      vm->global_hero()->set_instance(world.hero);

      auto model = ev.eval(n.condition);
      vmFault.clear();
      int32_t vmValue = 0;
      try {
        vmValue = vm->call_function<int32_t>(cond);
        }
      catch(const std::exception& e) {
        vmFault = e.what();
        }
      runs++;
      if(!vmFault.empty()) {
        vmFaults++;
        continue;
        }
      if(!model || model->type!=Value::Int) {
        modelFails++;
        failWhy[model ? "not an int" : ev.why()]++;
        continue;
        }
      if((vmValue!=0)==(model->i!=0)) {
        (vmValue!=0 ? bothTrue : bothFalse)++;
        continue;
        }
      // the draws the condition saw: globals it reads, members of the npc and the hero it reads, engine answers
      std::string state;
      for(auto& v : d.vars)
        if(auto* s = vm->find_symbol_by_name(v); s && s->type()==DaedalusDataType::INT && s->count()==1 &&
           v.find('.')==std::string::npos)
          state += " " + ident(v) + "=" + std::to_string(s->get_int(0));
      ex::walk(n.condition, [&](const Expr& x) {
        if(x.kind!=Expr::Field || x.args[0]->kind!=Expr::Inst)
          return;
        auto inst = vm->find_symbol_by_name(x.args[0]->name);
        auto* mem = inst && inst->get_instance() ? member(*vm, *inst->get_instance(), x.name) : nullptr;
        auto  i   = uint16_t(std::max(x.index, 0));
        if(mem && mem->type()==DaedalusDataType::INT && i<mem->count())
          state += " " + x.str() + "=" + std::to_string(mem->get_int(i, inst->get_instance().get()));
        });
      for(auto& [k,v] : world.memo)
        state += " " + ident(k) + "=" + v.text(*vm);
      found.push_back({&n, vmValue, model->i, state});
      break;  // one is enough to look at
      }
    }

  std::printf("random states: %zu dialogs x %u rounds = %zu runs; agree true %zu, false %zu; DIFFER in %zu dialogs\n",
              dialogs, rounds, runs, bothTrue, bothFalse, found.size());
  std::printf("not compared: VM failed %zu (division by zero and such), model could not evaluate %zu\n",
              vmFaults, modelFails);
  for(auto& [why,c] : failWhy)
    std::printf("   model: %zu x %s\n", c, why.c_str());
  for(size_t i=0; i<found.size() && (verbose || i<20); ++i) {
    auto& f = found[i];
    std::printf("DIFF %s (%s): vm %d, model %d\n   if %s\n   state:%s\n", ident(f.node->id).c_str(),
                ident(f.node->npc).c_str(), f.vm, f.model, f.node->condition->str().c_str(), f.state.c_str());
    }
  if(found.size()>20 && !verbose)
    std::printf("... %zu more (--verbose)\n", found.size()-20);
  return found.empty() && modelFails==0 && roundTripErrors==0 ? 0 : 1;
  }

int fuzzEffects(const char* datPath, const Model& m, uint32_t rounds, const char* only, bool verbose) {
  std::unique_ptr<EffectCheck> fx;
  Fuzz fz;
  bool ok = fz.open(datPath, [&fx](daedalus::DaedalusVm& v, FuzzWorld& w) {
    fx = std::make_unique<EffectCheck>(v, w);
    bindExternals(v, fx->engine());
    });
  if(!ok)
    return 1;
  auto&       vm    = fz.vm;
  FuzzWorld&  world = *fz.world;
  Bytecode    code(*vm);
  VmExternals engine(*vm);
  Evaluator   ev(*vm, m.helpers, engine);
  EffectTotals t;
  size_t nodes = 0;
  for(auto& n : m.nodes) {
    world.domain = nullptr;
    if(only!=nullptr && n.id!=only)
      continue;
    auto* fn     = fx->function(n);
    auto* npcSym = vm->find_symbol_by_name(n.npc);
    if(fn==nullptr || npcSym==nullptr)
      continue;
    nodes++;
    Domain d = domainOf(*vm, code, fn->index());
    world.domain = &d;
    world.self   = world.npc(npcSym->index());
    world.rng.seed(uint32_t(std::hash<std::string>()(n.id)));
    if(only!=nullptr)
      fx->trace = [](const std::string& s){ std::printf("   %s\n", s.c_str()); };
    auto owners = ownersOf(*vm, world, d);
    for(uint32_t r=0; r<rounds; ++r) {
      draw(*vm, world, d, owners);
      fz.vmFault.clear();
      auto res = fx->run(n, world.self, world.hero, ev);
      if(res.outcome==EffectCheck::Outcome::Agree && !fz.vmFault.empty()) {
        res.outcome = EffectCheck::Outcome::VmFailed;
        res.detail  = fz.vmFault;
        }
      t.add(n, res);
      if(res.outcome==EffectCheck::Outcome::Differ)
        break;  // one is enough to look at
      }
    }
  std::printf("random states: %zu dialogs and choices x %u rounds\n", nodes, rounds);
  t.print("random states", verbose);
  return t.differ==0 ? 0 : 1;
  }

int fuzzExec(const char* datPath, const Model& m, uint32_t rounds, const char* only, bool verbose) {
  std::unique_ptr<ExecCheck> xc;
  Fuzz fz;
  bool ok = fz.open(datPath, [&xc](daedalus::DaedalusVm& v, FuzzWorld& w) {
    xc = std::make_unique<ExecCheck>(v, w);
    bindExternals(v, xc->engine());
    });
  if(!ok)
    return 1;
  auto&       vm    = fz.vm;
  FuzzWorld&  world = *fz.world;
  Bytecode    code(*vm);
  xc->vmFault = &fz.vmFault;
  xc->npcOf   = [&world](uint32_t s) -> std::shared_ptr<daedalus::DaedalusInstance> { return world.npc(s); };
  // every npc is built now: built during a run, its constructor would change the state of that run only
  for(uint32_t i=0; i<vm->symbols().size(); ++i)
    if(auto* s = vm->find_symbol_by_index(i); s && s->type()==DaedalusDataType::INSTANCE && s->is_const() && world.isNpc(*s))
      world.npc(i);
  ExecTotals t;
  size_t nodes = 0;
  for(auto& n : m.nodes) {
    world.domain = nullptr;
    if(only!=nullptr && n.id!=only)
      continue;
    auto* npcSym = vm->find_symbol_by_name(n.npc);
    if(npcSym==nullptr)
      continue;
    nodes++;
    if(only!=nullptr)
      xc->trace = [](const std::string& s){ std::printf("   %s\n", s.c_str()); };
    for(auto fn : nodeFunctions(*vm, n)) {
      Domain d = domainOf(*vm, code, fn);
      world.domain = &d;
      world.self   = world.npc(npcSym->index());
      world.rng.seed(uint32_t(std::hash<std::string>()(n.id)) + fn);
      auto  owners = ownersOf(*vm, world, d);
      auto* sym    = vm->find_symbol_by_index(fn);
      for(uint32_t r=0; r<rounds; ++r) {
        draw(*vm, world, d, owners);
        auto res = xc->run(fn, world.self, world.hero);
        t.add(sym ? sym->name() : n.id, fn, res);
        if(res.outcome==ExecCheck::Outcome::Differ || res.outcome==ExecCheck::Outcome::NotProgram)
          break;  // one is enough to look at
        }
      world.domain = nullptr;
      }
    }
  std::printf("random states: %zu dialogs and choices x %u rounds\n", nodes, rounds);
  t.print("random states", verbose);
  return t.differ==0 ? 0 : 1;
  }

}
