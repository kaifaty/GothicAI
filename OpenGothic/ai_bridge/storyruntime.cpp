#include "storyruntime.h"

#include <Tempest/Log>

#include "definitionjson.h"
#include "gothic.h"
#include "game/definitions/basescripts.h"
#include "game/gamescript_programs.h"
#include "programdata.h"
#include "world/objects/npc.h"
#include "world/world.h"

#include "bytecode.h"
#include "eval.h"
#include "exec.h"
#include "exprjson.h"
#include "lifter.h"
#include "predicates.h"

using namespace Tempest;
using storygraph::ExprP;
using storygraph::Expr;
using daedalus::DaedalusDataType;

namespace {

struct PreparedQueries {
  std::map<std::string,storygraph::Helper> helpers;
  std::map<std::string,ExprP,std::less<>> conditions;
  };

PreparedQueries readPreparedQueries() {
  using namespace DefinitionJson;
  const auto path = baseProgramIndex().dir.parent_path()/"storygraph"/"story.json";
  if(!std::filesystem::exists(path))
    return {};

  const auto graph = DefinitionJson::read(path);
  const auto& sources = at(graph,"sources");
  require(integer(at(graph,"version"))==1 &&
          string(at(sources,"program_native_sha256"))==StockStoryPrograms::stockProgram,
          "prepared story graph differs from the active stock program");

  const auto& functions = at(graph,"functions");
  const auto& nodes = at(graph,"nodes");
  require(functions.IsObject() && nodes.IsArray(),"prepared story graph is missing queries");
  PreparedQueries queries;
  for(auto it = functions.MemberBegin(); it!=functions.MemberEnd(); ++it) {
    auto name = storygraph::symbolName(string(it->name));
    const auto& params = at(it->value,"params");
    const auto& lifted = at(it->value,"lifted");
    require(!name.empty() && params.IsUint() && lifted.IsBool(),"invalid prepared story helper");
    storygraph::Helper helper;
    helper.name = name;
    helper.params = params.GetUint();
    if(lifted.GetBool())
      helper.body = storygraph::readExpr(at(it->value,"body"),helper.notLifted);
    else
      helper.notLifted = string(at(it->value,"not_lifted"));
    require(queries.helpers.emplace(std::move(name),std::move(helper)).second,"duplicate prepared story helper");
    }
  for(const auto& node : nodes.GetArray()) {
    if(string(at(node,"kind"))!="dialog")
      continue;
    const auto& lifted = at(node,"lifted");
    require(lifted.IsBool(),"invalid prepared story condition");
    if(!lifted.GetBool())
      continue;
    auto name = storygraph::symbolName(string(at(node,"id")));
    std::string error;
    auto condition = storygraph::readExpr(at(node,"condition"),error);
    if(!name.empty() && condition)
      require(queries.conditions.emplace(std::move(name),std::move(condition)).second,"duplicate prepared story condition");
    }
  return queries;
  }

}

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
  preparedConditions.clear();
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
    if(own.addInt(*v,name,why))
      own.setInt(name,value); else
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
  if(auto* sc = script())
    return sc->globalIntSymbol(name);
  return std::nullopt;
  }

bool StoryRuntime::setInt(const std::string& name, int32_t value) {
  ensureVars();
  auto* sc = script();
  return sc!=nullptr && sc->setGlobalIntSymbol(name,value);
  }

void StoryRuntime::ensurePredicates() {
  ensureVars();
  auto* sc = script();
  if(sc==nullptr)
    return;
  auto& vm = sc->getVm();
  const bool legacy = vm.has_legacy_dispatcher();
  if(liftedFor==&vm && lifted!=nullptr && (code!=nullptr)==legacy)
    return;
  lifted.reset();
  lifter.reset();
  code.reset();
  preparedConditions.clear();
  if(legacy) {
    code   = std::make_unique<storygraph::Bytecode>(vm);
    lifter = std::make_unique<storygraph::Lifter>(vm,*code);
    lifted = std::make_unique<storygraph::Predicates>(vm,*lifter);
    } else {
    PreparedQueries queries;
    try {
      if(sc->usesVerifiedNpcProgram())
        queries = readPreparedQueries();
      }
    catch(const std::exception& e) {
      Log::e("[story] prepared queries unavailable: ",e.what());
      }
    preparedConditions = std::move(queries.conditions);
    lifted = std::make_unique<storygraph::Predicates>(std::move(queries.helpers));
    }
  liftedFor = &vm;
  }

storygraph::Program StoryRuntime::originalProgram(const daedalus::DaedalusSymbol& fn) {
  if(auto* sc = script()) {
    if(const auto* program = sc->preparedStockProgram(fn))
      return *program;
    auto& vm = sc->getVm();
    if(vm.has_legacy_dispatcher()) {
      ensurePredicates();
      return storygraph::liftProgram(vm,*code,fn.index());
      }
    }
  storygraph::Program missing;
  missing.error = "function is missing from the prepared stock catalog";
  return missing;
  }

ExprP StoryRuntime::originalCondition(uint32_t fn, std::string_view dialog) {
  ensurePredicates();
  if(lifted==nullptr)
    return {};
  if(code!=nullptr) {
    const auto& condition = lifted->condition(fn);
    return lifted->whyNotLifted(condition).empty() ? condition : nullptr;
    }
  const auto entry = preparedConditions.find(dialog);
  if(entry==preparedConditions.end() || !lifted->whyNotLifted(entry->second).empty())
    return {};
  return entry->second;
  }

storygraph::Interpreter& StoryRuntime::interpreter(daedalus::DaedalusVm& v) {
  ensureVars();
  const bool legacy = v.has_legacy_dispatcher();
  if(runFor==&v && interp!=nullptr && interp->allowUnresolvedScriptCalls==legacy)
    return *interp;
  interp.reset();
  if(auto* sc=script(); sc!=nullptr && &sc->getVm()==&v)
    engine = std::make_unique<GameStoryExternals>(*sc,true);
  else
    engine = std::make_unique<storygraph::VmExternals>(v);
  interp = std::make_unique<storygraph::Interpreter>(v,*engine);
  interp->allowUnresolvedScriptCalls = legacy;
  if(!legacy) {
    if(auto* sc = script(); sc!=nullptr && &sc->getVm()==&v)
      interp->resolveProgram = [sc](const daedalus::DaedalusSymbol& fn) { return sc->preparedStockProgram(fn); };
    }
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
  ensurePredicates();
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
  auto* sc = v.has_legacy_dispatcher() ? nullptr : script();
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
        auto* s = v.find_symbol_by_name(x.name);
        if(s!=nullptr && v.find_parameters_for_function(s).size()!=x.args.size())
          err = x.name + " takes " + std::to_string(v.find_parameters_for_function(s).size()) + " arguments";
        else if(s!=nullptr && !v.has_legacy_dispatcher() && !v.has_active_override(*s) &&
                (sc==nullptr || &sc->getVm()!=&v || sc->preparedStockProgram(*s,true)==nullptr))
          err = "script function is unavailable without bytecode: " + x.name;
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

std::optional<storygraph::Value> StoryRuntime::evaluate(const ExprP& e, std::string& error, storygraph::Externals* externals) {
  auto* sc=script();
  if(sc==nullptr) {
    error = "no world";
    return std::nullopt;
    }
  ensurePredicates();
  auto why = lifted->whyNotLifted(e);
  if(code==nullptr && !why.empty()) {
    error = std::move(why);
    return std::nullopt;
    }
  GameStoryExternals engine(*sc,false);
  storygraph::Evaluator ev(sc->getVm(),lifted->helpers(),externals!=nullptr ? *externals : engine);
  auto r=ev.eval(e);
  error = ev.why();
  return r;
  }

std::optional<bool> StoryRuntime::test(const ExprP& e, std::string_view what) {
  if(vm()==nullptr)
    return std::nullopt;
  std::string error;
  auto r=evaluate(e,error);
  if(!r || r->type!=storygraph::Value::Int) {
    if(once(std::string("eval:") + std::string(what)))
      Log::e("[story] condition of ",what," not computed: ",r ? "not an int" : error);
    return std::nullopt;
    }
  return r->i!=0;
  }

StoryRuntime::Bind::Bind(daedalus::DaedalusVm& v, Npc* s, Npc* o)
  :self(v.global_self()), other(v.global_other()) {
  if(self!=nullptr) {
    prevSelf = self->get_instance();
    self->set_instance(s!=nullptr ? std::static_pointer_cast<daedalus::INpc>(s->handlePtr()) : nullptr);
    }
  if(other!=nullptr) {
    prevOther = other->get_instance();
    other->set_instance(o!=nullptr ? std::static_pointer_cast<daedalus::INpc>(o->handlePtr()) : nullptr);
    }
  }

StoryRuntime::Bind::~Bind() {
  if(self!=nullptr)
    self->set_instance(prevSelf);
  if(other!=nullptr)
    other->set_instance(prevOther);
  }
