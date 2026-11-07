#include "storyruntime.h"

#include <Tempest/Log>

#include "gothic.h"
#include "world/objects/npc.h"
#include "world/world.h"

#include "bytecode.h"
#include "eval.h"
#include "exec.h"
#include "lifter.h"
#include "predicates.h"

using namespace Tempest;
using storygraph::ExprP;
using storygraph::Expr;
using daedalus::DaedalusDataType;

StoryRuntime::StoryRuntime(std::function<std::string(const std::string&,const std::string&)> ownLine)
  :ownLine(std::move(ownLine)) {
  }

StoryRuntime::~StoryRuntime() = default;

daedalus::DaedalusVm* StoryRuntime::vm() const {
  World* w = Gothic::inst().world();
  return w!=nullptr ? &w->script().getVm() : nullptr;
  }

GameScript* StoryRuntime::script() const {
  World* w = Gothic::inst().world();
  return w!=nullptr ? &w->script() : nullptr;
  }

void StoryRuntime::clear() {
  vars.clear();
  varsMade.clear();
  varsFor = nullptr;
  reported.clear();
  // the next game session has its own VM
  lifted.reset();
  lifter.reset();
  code.reset();
  liftedFor = nullptr;
  interp.reset();
  engine.reset();
  runFor = nullptr;
  }

std::string StoryRuntime::addVar(const std::string& name, int32_t first) {
  if(name.rfind(OwnSymbols::Prefix,0)!=0)
    return name + ": a variable of the director's is named DIR_...";
  if(auto* sc = script()) {
    if(auto err = whyNotVar(*sc,name); !err.empty())
      return err;
    }
  vars.emplace(name,first);
  ensureVars();
  return {};
  }

void StoryRuntime::ensureVars() {
  auto* sc = script();
  auto* v  = vm();
  if(v==nullptr || (v==varsFor && varsMade.size()==vars.size()))
    return;
  if(v!=varsFor) {
    varsMade.clear();
    varsFor = v;
    }
  auto& own = sc->ownSymbols();
  for(auto& [name,value] : vars) {
    if(varsMade.count(name))
      continue;
    varsMade.insert(name);
    if(auto err = whyNotVar(*sc,name); !err.empty()) {
      Log::e("[story] vars: ",err);
      continue;
      }
    if(own.find(name)!=nullptr)
      continue;  // a save brought it back with its value
    std::string why;
    if(auto* s = own.addInt(name,why))
      s->set_int(value); else
      Log::e("[story] vars: ",why);
    }
  }

std::string StoryRuntime::whyNotVar(GameScript& script, const std::string& name) {
  if(name.rfind(OwnSymbols::Prefix,0)!=0)
    return name + ": a variable of the director's is named DIR_...";
  auto* own = script.ownSymbols().find(name);
  if(own!=nullptr ? own->kind!=OwnSymbols::Kind::Int : script.getVm().find_symbol_by_name(name)!=nullptr)
    return name + " is taken";
  return {};
  }

std::optional<int32_t> StoryRuntime::intOf(const std::string& name) {
  ensureVars();
  auto* v = vm();
  auto* s = v!=nullptr ? v->find_symbol_by_name(name) : nullptr;
  if(s==nullptr || s->is_member() || s->type()!=DaedalusDataType::INT)
    return std::nullopt;
  return s->get_int();
  }

bool StoryRuntime::setInt(const std::string& name, int32_t value) {
  ensureVars();
  auto* v = vm();
  auto* s = v!=nullptr ? v->find_symbol_by_name(name) : nullptr;
  if(s==nullptr || s->is_member() || s->is_const() || s->type()!=DaedalusDataType::INT)
    return false;
  s->set_int(value);
  return true;
  }

void StoryRuntime::ensureLifter() {
  ensureVars();
  auto* v = vm();
  if(v==nullptr || v==liftedFor)
    return;
  lifted.reset();
  lifter.reset();
  code      = std::make_unique<storygraph::Bytecode>(*v);
  lifter    = std::make_unique<storygraph::Lifter>(*v,*code);
  lifted    = std::make_unique<storygraph::Predicates>(*v,*lifter);
  liftedFor = v;
  }

storygraph::Bytecode& StoryRuntime::bytecode() {
  return *code;
  }

storygraph::Predicates& StoryRuntime::predicates() {
  return *lifted;
  }

storygraph::Interpreter& StoryRuntime::interpreter(daedalus::DaedalusVm& v) {
  ensureVars();
  if(runFor==&v && interp!=nullptr)
    return *interp;
  interp.reset();
  engine = std::make_unique<storygraph::VmExternals>(v);  // the engine's own functions, as the VM calls them
  interp = std::make_unique<storygraph::Interpreter>(v,*engine);
  interp->ownLine = [this](const std::string& npc, const std::string& text) { return ownLine(npc,text); };
  interp->choices = [this](const storygraph::Step& s) { return choices && choices(s); };
  runFor = &v;
  return *interp;
  }

std::string StoryRuntime::checkExpr(const ExprP& e) {
  auto* v = vm();
  if(v==nullptr)
    return {};  // no world yet: names are checked when the condition is computed
  auto err = checkNames(*v,e);
  if(!err.empty())
    return err;
  ensureLifter();
  return lifted->whyNotLifted(e);
  }

std::string StoryRuntime::checkNames(daedalus::DaedalusVm& vm, const ExprP& e) const {
  auto* v = &vm;
  std::string err;
  storygraph::ex::walk(e, [&](const Expr& x) {
    if(!err.empty())
      return;
    auto* s = (x.kind==Expr::Var || x.kind==Expr::Inst || x.kind==Expr::Sym || x.kind==Expr::Call ||
               x.kind==Expr::Fn) ? v->find_symbol_by_name(x.name) : nullptr;
    switch(x.kind) {
      case Expr::Var:
        if(s==nullptr || s->is_member())
          err = "no variable " + x.name;
        break;
      case Expr::Inst:
        if(s==nullptr || s->type()!=DaedalusDataType::INSTANCE)
          err = "no instance " + x.name;
        break;
      case Expr::Sym:
        if(s==nullptr)
          err = "no symbol " + x.name;
        break;
      case Expr::Call:
        if(s==nullptr || !s->is_external())
          err = "no engine function " + x.name;
        else if(v->find_parameters_for_function(s).size()!=x.args.size())
          err = x.name + " takes " + std::to_string(v->find_parameters_for_function(s).size()) + " arguments";
        break;
      case Expr::Fn:
        if(s==nullptr || s->type()!=DaedalusDataType::FUNCTION || s->is_external())
          err = "no script function " + x.name;
        break;
      default:
        break;
      }
    });
  return err;
  }

std::string StoryRuntime::checkProgram(daedalus::DaedalusVm& v, const storygraph::Program& p,
                                       const daedalus::DaedalusSymbol* fn, bool choices) const {
  const size_t params = fn!=nullptr ? v.find_parameters_for_function(fn).size() : 0;
  auto member = [&v](const std::string& name) {
    for(auto* cls : {"C_NPC.", "C_ITEM.", "C_INFO.", "C_FOCUS.", "C_MENU_ITEM."})
      if(auto* m = v.find_symbol_by_name(cls + name); m!=nullptr && m->is_member())
        return true;
    return false;
    };
  auto expr = [&](const ExprP& e) {
    auto err = checkNames(v,e);
    storygraph::ex::walk(e, [&](const Expr& x) {
      if(!err.empty())
        return;
      if(x.kind==Expr::Fn) {
        // the VM runs it: it takes what it takes
        auto* s = v.find_symbol_by_name(x.name);
        if(s!=nullptr && v.find_parameters_for_function(s).size()!=x.args.size())
          err = x.name + " takes " + std::to_string(v.find_parameters_for_function(s).size()) + " arguments";
        }
      else if(x.kind==Expr::Param && (x.index<0 || size_t(x.index)>=params))
        err = "no argument " + std::to_string(x.index) + (fn!=nullptr ? " of " + fn->name() : std::string());
      else if(x.kind==Expr::Field && !member(x.name))
        err = "no member " + x.name;
      });
    return err;
    };
  std::function<std::string(const std::vector<storygraph::Step>&)> steps = [&](const std::vector<storygraph::Step>& ss) {
    for(auto& st : ss) {
      std::string err;
      switch(st.kind) {
        case storygraph::Step::Call:
          err = expr(st.call);
          break;
        case storygraph::Step::Store: {
          err = expr(st.target);
          if(err.empty())
            err = expr(st.value);
          auto* t = st.target->kind!=Expr::Field ? v.find_symbol_by_name(st.target->name) : nullptr;
          if(err.empty() && t!=nullptr && t->is_const())
            err = "store to the constant " + st.target->name;
          break;
          }
        case storygraph::Step::If:
          err = expr(st.cond);
          if(err.empty())
            err = steps(st.then);
          if(err.empty())
            err = steps(st.otherwise);
          break;
        case storygraph::Step::Return:
          if(st.value!=nullptr)
            err = expr(st.value);
          break;
        case storygraph::Step::Say:
          if(!st.line.empty() && Gothic::inst().messageByName(st.line).empty())
            err = "no original line " + st.line;
          break;
        case storygraph::Step::Choice:
        case storygraph::Step::ClearChoices:
          if(fn!=nullptr)
            err = "a dialog of the scripts offers its choices through INFO_ADDCHOICE";
          else if(!choices)
            err = "choices are offered in a dialog of the director's only";
          else
            err = steps(st.then);
          break;
        }
      if(!err.empty())
        return err;
      }
    return std::string();
    };
  return steps(p.steps);
  }

std::optional<bool> StoryRuntime::test(const ExprP& e, std::string_view what) {
  auto* v = vm();
  if(v==nullptr)
    return std::nullopt;
  ensureLifter();
  lifted->whyNotLifted(e);  // lifts the script functions it calls
  storygraph::VmExternals engine(*v);
  storygraph::Evaluator   ev(*v,lifted->helpers(),engine);
  auto r = ev.eval(e);
  if(!r || r->type!=storygraph::Value::Int) {
    if(once(std::string("eval:") + std::string(what)))
      Log::e("[story] condition of ",what," not computed: ",r ? "not an int" : ev.why());
    return std::nullopt;
    }
  return r->i!=0;
  }

StoryRuntime::Bind::Bind(daedalus::DaedalusVm& v, Npc* s, Npc* o)
  :self(v.global_self()), other(v.global_other()) {
  if(self!=nullptr) {
    prevSelf = self->get_instance();
    self->set_instance(s!=nullptr ? std::shared_ptr<daedalus::DaedalusInstance>(s->handlePtr()) : nullptr);
    }
  if(other!=nullptr) {
    prevOther = other->get_instance();
    other->set_instance(o!=nullptr ? std::shared_ptr<daedalus::DaedalusInstance>(o->handlePtr()) : nullptr);
    }
  }

StoryRuntime::Bind::~Bind() {
  if(self!=nullptr)
    self->set_instance(prevSelf);
  if(other!=nullptr)
    other->set_instance(prevOther);
  }
