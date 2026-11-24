#include "storyruntime.h"

#include <Tempest/Log>

#include <algorithm>
#include <charconv>

#include "definitionjson.h"
#include "execcore.h"
#include "exprjson.h"
#include "game/definitions/basescripts.h"
#include "game/definitions/stockstoryprograms.h"
#include "game/gamescript.h"
#include "gothic.h"
#include "predicates.h"
#include "programdata.h"
#include "world/world.h"

using namespace Tempest;
using storygraph::ExprP;
using storygraph::Expr;

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

storygraph::NativeSlot querySlot(const Expr& e) {
  auto name = std::string_view(e.name);
  int32_t index = e.index;
  if(e.kind==Expr::Var) {
    // The lifter encodes an unassigned static local array as FUNCTION.LOCAL[n].
    const auto bracket = name.find('[');
    if(bracket!=std::string_view::npos && name.back()==']') {
      const auto* end = name.data()+name.size()-1;
      auto parsed = std::from_chars(name.data()+bracket+1,end,index);
      if(parsed.ec==std::errc() && parsed.ptr==end)
        name = name.substr(0,bracket);
      }
    }
  using Slot = storygraph::NativeSlot;
  return {e.kind==Expr::Inst ? Slot::Instance : Slot::Variable,name,uint16_t(std::max(index,0)),0};
  }

// Prepared queries use guarded cases and short-circuit operators. The host supplies only game data
// and read-only operations, while the expression keeps its query semantics.
class Query final {
  public:
    using Value = storygraph::NativeValue;
    using Slot  = storygraph::NativeSlot;

    Query(storygraph::NativeExecutionHost& host, const std::map<std::string,storygraph::Helper>& helpers)
      :host(host), helpers(helpers) {
      }

    std::optional<Value> read(const ExprP& e, std::string& error) {
      auto result = e ? eval(*e,{},0) : Value::integer(1);
      error = std::move(why);
      return result;
      }

  private:
    std::optional<Value> fail(std::string error) {
      if(why.empty())
        why = std::move(error);
      return std::nullopt;
      }

    std::optional<Value> eval(const Expr& e, const std::vector<Value>& params, unsigned depth) {
      const auto index = uint16_t(std::max(e.index,0));
      switch(e.kind) {
        case Expr::Int:
          return Value::integer(e.value);
        case Expr::Str:
          return Value::str(e.text);
        case Expr::Var:
        case Expr::Inst:
          return host.read(querySlot(e),why);
        case Expr::Field: {
          auto inst = eval(*e.args[0],params,depth);
          if(!inst)
            return std::nullopt;
          if(inst->type!=Value::Inst)
            return fail("member of a non-instance: " + e.str());
          return host.read({Slot::Member,e.name,index,inst->inst},why);
          }
        case Expr::Sym:
          if(auto id = host.symbolIndex(e.name))
            return Value::integer(*id);
          return fail("no symbol " + e.name);
        case Expr::Param:
          if(e.index<0 || size_t(e.index)>=params.size())
            return fail("no parameter " + std::to_string(e.index));
          return params[size_t(e.index)];
        case Expr::Call:
        case Expr::Fn:
          return call(e,params,depth);
        case Expr::Op:
          return op(e,params,depth);
        case Expr::Cases:
          for(size_t i=0; i+1<e.args.size(); i+=2) {
            auto guard = eval(*e.args[i],params,depth);
            if(!guard)
              return std::nullopt;
            if(guard->type!=Value::Int)
              return fail("guard is not an int: " + e.args[i]->str());
            if(guard->i!=0)
              return eval(*e.args[i+1],params,depth);
            }
          return Value::integer(0);
        case Expr::Local:
        case Expr::Unknown:
          return fail("query expression unavailable: " + e.str());
        }
      return fail("bad expression");
      }

    std::optional<Value> call(const Expr& e, const std::vector<Value>& params, unsigned depth) {
      const storygraph::Helper* helper = nullptr;
      std::optional<storygraph::NativeFunction> fn;
      if(e.kind==Expr::Fn) {
        if(depth>=64)
          return fail("script functions nested too deep");
        const auto entry = helpers.find(e.name);
        if(entry==helpers.end() || !entry->second.body || !entry->second.notLifted.empty())
          return fail("script function not lifted: " + e.name);
        helper = &entry->second;
        }
      else {
        fn = host.findFunction(e.name);
        if(!fn || !host.acceptsCall(e.name,true))
          return fail("no engine function " + e.name);
        if(fn->parameters.size()!=e.args.size())
          return fail("wrong argument count: " + e.str());
        }

      std::vector<Value> args;
      args.reserve(e.args.size());
      for(size_t i=0; i<e.args.size(); ++i) {
        auto value = eval(*e.args[i],params,depth);
        if(!value)
          return std::nullopt;
        if(fn && value->type!=fn->parameters[i])
          return fail("argument " + std::to_string(i) + " of " + e.name + " has the wrong type");
        args.push_back(std::move(*value));
        }
      return helper!=nullptr ? eval(*helper->body,args,depth+1) : host.invoke(e.name,args,why);
      }

    std::optional<Value> op(const Expr& e, const std::vector<Value>& params, unsigned depth) {
      if(e.name=="&&" || e.name=="||") {
        const bool isAnd = e.name=="&&";
        bool unknown = false;
        for(const auto& arg : e.args) {
          auto value = eval(*arg,params,depth);
          if(!value || value->type!=Value::Int) {
            if(value)
              fail("logic on a non-int: " + arg->str());
            unknown = true;
            continue;
            }
          if((value->i!=0)!=isAnd) {
            why.clear();
            return Value::integer(!isAnd);
            }
          }
        return unknown ? std::optional<Value>() : Value::integer(isAnd);
        }
      std::vector<int32_t> operands;
      operands.reserve(e.args.size());
      for(const auto& arg : e.args) {
        auto value = eval(*arg,params,depth);
        if(!value)
          return std::nullopt;
        if(value->type!=Value::Int)
          return fail("arithmetic on a non-int: " + e.str());
        operands.push_back(value->i);
        }
      auto result = storygraph::execution::integerOperator(e.name,operands,why);
      if(!result)
        return std::nullopt;
      return Value::integer(*result);
      }

    storygraph::NativeExecutionHost&              host;
    const std::map<std::string,storygraph::Helper>& helpers;
    std::string                                   why;
  };

}

StoryRuntime::StoryRuntime(std::function<std::string(const std::string&,const std::string&)> ownLine)
  :ownLine(std::move(ownLine)) {
  }

StoryRuntime::~StoryRuntime() = default;

GameScript* StoryRuntime::script() const {
  World* w = Gothic::inst().world();
  return w!=nullptr ? &w->script() : nullptr;
  }

void StoryRuntime::clear() {
  vars.clear();
  varsMade.clear();
  varsFor = nullptr;
  reported.clear();
  lifted.reset();
  preparedConditions.clear();
  preparedFor = nullptr;
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
  if(sc==nullptr || (sc==varsFor && varsMade.size()==vars.size()))
    return;
  if(sc!=varsFor) {
    varsMade.clear();
    varsFor = sc;
    }
  for(const auto& [name,value] : vars) {
    if(!varsMade.insert(name).second)
      continue;
    if(auto err = whyNotVar(*sc,name); !err.empty()) {
      Log::e("[story] vars: ",err);
      continue;
      }
    if(auto err = sc->addStoryVariable(name,value); !err.empty())
      Log::e("[story] vars: ",err);
    }
  }

std::string StoryRuntime::whyNotVar(GameScript& script, const std::string& name) {
  if(name.rfind(OwnSymbols::Prefix,0)!=0)
    return name + ": a variable of the director's is named DIR_...";
  auto* own = script.ownSymbols().find(name);
  if(own!=nullptr ? own->kind!=OwnSymbols::Kind::Int : script.storySymbolIndex(name).has_value())
    return name + " is taken";
  return {};
  }

std::optional<int32_t> StoryRuntime::intOf(const std::string& name) {
  ensureVars();
  if(auto* sc = script())
    return sc->ownSymbols().intValue(name);
  return std::nullopt;
  }

bool StoryRuntime::setInt(const std::string& name, int32_t value) {
  ensureVars();
  auto* sc = script();
  return sc!=nullptr && sc->ownSymbols().setInt(name,value);
  }

void StoryRuntime::ensurePredicates() {
  ensureVars();
  auto* sc = script();
  if(sc==nullptr || preparedFor==sc)
    return;
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
  preparedFor = sc;
  }

storygraph::Program StoryRuntime::originalProgram(uint32_t fn) {
  if(auto* sc = script())
    if(const auto* program = sc->preparedStockProgram(fn))
      return *program;
  storygraph::Program missing;
  missing.error = "function is missing from the prepared stock catalog";
  return missing;
  }

ExprP StoryRuntime::originalCondition(std::string_view dialog) {
  ensurePredicates();
  const auto entry = preparedConditions.find(dialog);
  if(entry==preparedConditions.end() || !lifted->whyNotLifted(entry->second).empty())
    return {};
  return entry->second;
  }

std::string StoryRuntime::checkExpr(const ExprP& e) {
  auto* sc = script();
  if(sc==nullptr)
    return {}; // Deferred patch validation.
  ensurePredicates();
  if(auto err = lifted->whyNotLifted(e); !err.empty())
    return err;
  auto host = sc->makeStoryHost(nullptr,nullptr);
  std::string error;
  storygraph::ex::walk(e,[&](const Expr& x) {
    if(!error.empty())
      return;
    switch(x.kind) {
      case Expr::Var:
      case Expr::Inst:
        if(!host->hasVariable(querySlot(x).name,x.kind==Expr::Inst))
          error = "no variable " + x.name;
        break;
      case Expr::Sym:
        if(!host->symbolIndex(x.name))
          error = "no symbol " + x.name;
        break;
      case Expr::Call:
        if(const auto fn = host->findFunction(x.name); !fn || !host->acceptsCall(x.name,true))
          error = "no engine function " + x.name;
        else if(fn->parameters.size()!=x.args.size())
          error = x.name + " takes " + std::to_string(fn->parameters.size()) + " arguments";
        break;
      case Expr::Fn:
        if(const auto& helper = lifted->helper(x.name); helper.params!=x.args.size())
          error = x.name + " takes " + std::to_string(helper.params) + " arguments";
        break;
      default:
        break;
      }
    });
  return error;
  }

std::string StoryRuntime::checkProgram(const storygraph::Program& p, size_t arguments, bool allowChoices) {
  auto* sc = script();
  if(sc==nullptr)
    return {}; // Deferred patch validation.
  ensureVars();
  auto error = sc->checkStoryProgram(p,arguments,allowChoices);
  if(!error.empty())
    return error;
  std::function<void(const std::vector<storygraph::Step>&)> checkContents = [&](const auto& steps) {
    for(const auto& step : steps) {
      if(!error.empty())
        return;
      if(step.kind==storygraph::Step::Say && !step.line.empty() && Gothic::inst().messageByName(step.line).empty()) {
        error = "no original line " + step.line;
        return;
        }
      if(step.kind==storygraph::Step::Store && step.target->kind!=Expr::Field &&
         !sc->storySlotWritable(step.target->name)) {
        error = "store to the constant " + step.target->name;
        return;
        }
      checkContents(step.then);
      checkContents(step.otherwise);
      }
    };
  checkContents(p.steps);
  return error;
  }

std::optional<storygraph::NativeValue> StoryRuntime::evaluate(const ExprP& e, std::string& error, Npc* self,
                                                             Npc* other, std::string* instanceName,
                                                             std::pair<Npc*,Npc*>* resultingActors) {
  auto* sc = script();
  if(sc==nullptr) {
    error = "no world";
    return std::nullopt;
    }
  ensurePredicates();
  error = lifted->whyNotLifted(e);
  if(!error.empty())
    return std::nullopt;
  auto host = sc->makeStoryHost(self,other);
  auto result = Query(*host,lifted->helpers()).read(e,error);
  if(resultingActors!=nullptr)
    *resultingActors = sc->storyActors();
  if(instanceName!=nullptr && result && result->type==storygraph::NativeValue::Inst && result->inst!=0) {
    *instanceName = host->instanceName(result->inst);
    if(instanceName->empty())
      *instanceName = "?";
    }
  return result;
  }

std::optional<bool> StoryRuntime::test(const ExprP& e, std::string_view what, Npc* self, Npc* other,
                                       std::pair<Npc*,Npc*>* resultingActors) {
  std::string error;
  auto result = evaluate(e,error,self,other,nullptr,resultingActors);
  if(!result || result->type!=storygraph::NativeValue::Int) {
    if(reported.insert("eval:" + std::string(what)).second)
      Log::e("[story] condition of ",what," not computed: ",result ? "not an int" : error);
    return std::nullopt;
    }
  return result->i!=0;
  }

storygraph::NativeInterpreter::Result StoryRuntime::run(const storygraph::Program& p, Npc* self, Npc* other,
                                                         bool allowChoices, const std::vector<storygraph::NativeValue>& args) {
  if(auto* sc = script()) {
    ensureVars();
    return sc->runStoryProgram(p,self,other,true,ownLine,allowChoices ? choices : nullptr,args);
    }
  return {false,"no world",{},false};
  }
