#include "gamescript_programs.h"

#include <Tempest/Log>

#include "gothic.h"
#include "world/objects/npc.h"

#include <cstdlib>

GameStoryExternals::GameStoryExternals(GameScript& script, bool effects)
  : script(script), fallback(script.getVm()), effects(effects) {
  }

storygraph::Value GameStoryExternals::call(const daedalus::DaedalusSymbol& fn, const std::vector<storygraph::Value>& args) {
  using storygraph::Value;
  const std::string_view name=fn.name();
  if(effects && !fn.has_return() && args.size()==2 && args[0].type==Value::Str) {
    if(name=="LOG_CREATETOPIC" && args[1].type==Value::Int)
      script.log_createtopic(args[0].s,args[1].i);
    else if(name=="LOG_SETTOPICSTATUS" && args[1].type==Value::Int)
      script.log_settopicstatus(args[0].s,args[1].i);
    else if(name=="LOG_ADDENTRY" && args[1].type==Value::Str)
      script.log_addentry(args[0].s,args[1].s);
    else
      return fallback.call(fn,args);
    return Value::integer(0);
    }

  const bool deadQuery=name=="NPC_ISDEAD";
  const bool query=fn.has_return() && fn.rtype()==daedalus::DaedalusDataType::INT &&
                   (deadQuery || name=="NPC_KNOWSINFO" || name=="NPC_HASITEMS" ||
                    name=="NPC_ISINSTATE" || name=="NPC_WASINSTATE" || name=="NPC_ISINROUTINE");
  const bool effect=effects && (name=="NPC_SETREFUSETALK" || name=="NPC_SETATTITUDE") && !fn.has_return();
  if((!query && !effect) || args.size()!=(deadQuery ? 1u : 2u) || args[0].type!=Value::Inst ||
     (!deadQuery && args[1].type!=Value::Int))
    return fallback.call(fn,args);

  auto* record=dynamic_cast<daedalus::INpc*>(args[0].inst.get());
  auto* npc=script.findNpc(static_cast<NpcRecord*>(record));
  if(args[0].inst && !npc)
    return fallback.call(fn,args);
  if(deadQuery)
    return Value::integer(npc==nullptr || script.isDead(*npc));
  if(!npc)
    return Value::integer(0);

  const auto value=args[1].i;
  if(name=="NPC_KNOWSINFO")
    return Value::integer(script.doesNpcKnowInfo(npc->handle(),uint32_t(value)));
  if(name=="NPC_HASITEMS")
    return Value::integer(int32_t(npc->itemCount(uint32_t(value))));
  if(name=="NPC_ISINSTATE")
    return Value::integer(npc->isInState(uint32_t(value)));
  if(name=="NPC_WASINSTATE")
    return Value::integer(npc->wasInState(uint32_t(value)));
  if(name=="NPC_ISINROUTINE")
    return Value::integer(npc->isInRoutine(uint32_t(value)));
  if(name=="NPC_SETREFUSETALK")
    npc->setRefuseTalkSeconds(value);
  else
    npc->setAttitude(Attitude(value));
  return Value::integer(0);
  }

GameScript::StockPrograms::StockPrograms(GameScript& script)
  : vm(script.getVm()), code(vm), engine(script,true), interpreter(vm,engine) {
  interpreter.allowUnresolvedScriptCalls = false;
  interpreter.resolveProgram = [this](const daedalus::DaedalusSymbol& fn) {
    return &entry(fn.index()).program;
    };
  }

const GameScript::StockPrograms::Entry& GameScript::StockPrograms::entry(uint32_t id) {
  const auto [it,inserted] = programs.try_emplace(id);
  if(inserted) {
    it->second.program = storygraph::liftProgram(vm,code,id);
    if(it->second.program.error.empty())
      it->second.calls = code.summarize(id).calls;
    }
  return it->second;
  }

bool GameScript::StockPrograms::isSupported(const daedalus::DaedalusSymbol& fn, std::unordered_set<uint32_t>& checked) {
  if(!checked.insert(fn.index()).second)
    return true;
  const auto& program = entry(fn.index());
  if(!program.program.error.empty())
    return false;
  for(const auto& name : program.calls) {
    const auto* child = vm.find_symbol_by_name(name);
    if(child==nullptr)
      return false;
    if(vm.has_active_override(*child))
      continue;
    if(child->has_local_variables_enabled() || !isSupported(*child,checked))
      return false;
    }
  return true;
  }

bool GameScript::StockPrograms::run(const daedalus::DaedalusSymbol& fn, int32_t& result, bool* failed) {
  if(fn.type()!=daedalus::DaedalusDataType::FUNCTION || fn.is_external() || fn.has_local_variables_enabled() ||
     !vm.find_parameters_for_function(&fn).empty())
    return false;
  // Preflight every nested function before the first effect. The interpreter also
  // rejects an unresolved call if an active override changes during execution.
  std::unordered_set<uint32_t> checked;
  if(!isSupported(fn,checked))
    return false;
  auto run = interpreter.run(entry(fn.index()).program);
  if(failed!=nullptr)
    *failed = !run.ok;
  if(!run.ok)
    Tempest::Log::e("[story] program ",fn.name(),": ",run.error);
  result = run.returned.type==storygraph::Value::Int ? run.returned.i : 0;
  return true;
  }

bool GameScript::invokeStockProgram(const daedalus::DaedalusSymbol& fn, int32_t& result, bool* failed) {
  if(!nativeRoutineProgram)
    return false;
  const auto* enabled = std::getenv("GOTHIC_AI_STORYEXEC");
  if(enabled!=nullptr && std::string_view(enabled)=="0")
    return false;
  if(stockPrograms==nullptr)
    stockPrograms = std::make_unique<StockPrograms>(*this);
  return stockPrograms->run(fn,result,failed);
  }

int32_t GameScript::dialogFunction(daedalus::DaedalusSymbol* fn, bool returns) {
  int32_t result = 0;
  if(fn!=nullptr) {
    auto& overlay = Gothic::inst().dialogScriptFn;
    if(overlay && overlay(*fn,result))
      return result;
    if(invokeStockProgram(*fn,result))
      return result;
    }
  if(returns)
    return vm.call_function<int32_t>(fn);
  vm.call_function(fn);
  return 0;
  }
