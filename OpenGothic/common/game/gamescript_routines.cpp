#include "gamescript.h"

#include <stdexcept>

int GameScript::initializeRoutine(const std::shared_ptr<daedalus::INpc>& npc,
                                   daedalus::DaedalusSymbol* function) {
  return RoutineDefinitionRuntime::initialize(vm,function,routineDefinitions,[&](const auto& row) {
    ta_min(npc,row.startHour,row.startMinute,row.endHour,row.endMinute,int(row.state),row.waypoint);
    });
  }

void GameScript::loadNativeRoutineExchange() {
  if(!nativeRoutineProgram)
    return;

  const auto instanceId=[this](daedalus::DaedalusSymbol* symbol) {
    vm.push_reference(symbol);
    vm.unsafe_call_external(findSymbol("HLP_GETINSTANCEID"));
    return vm.pop_int();
    };
  using Handle=std::shared_ptr<daedalus::INpc>;
  vm.override_function("B_STARTOTHERROUTINE",std::function<void(Handle,std::string_view)>{
    [this,instanceId](Handle npc,std::string_view routine) {
      auto* actor=findSymbol("B_STARTOTHERROUTINE.SLF");
      auto* name=findSymbol("B_STARTOTHERROUTINE.NEWROUTINE");
      name->set_string(routine);
      actor->set_instance(npc);

      // Keep the existing engine boundaries, including the placement barrier.
      const auto call=[&](std::string_view external) {
        vm.push_reference(actor);
        vm.unsafe_call_external(findSymbol(external));
        };
      call("AI_STANDUP");
      const auto selected=instanceId(actor);
      const auto self=instanceId(vm.global_self());
      const bool sameInstance=selected==self;
      if(!sameInstance) {
        call("NPC_ISDEAD");
        const bool dead=vm.pop_int()!=0;
        call("HLP_ISVALIDNPC");
        const bool valid=vm.pop_int()!=0;
        if(dead || !valid)
          return;
        }
      vm.push_reference(actor);
      vm.push_reference(name);
      vm.unsafe_call_external(findSymbol("NPC_EXCHANGEROUTINE"));
      if(!sameInstance)
        call("AI_CONTINUEROUTINE");
      }},RoutineDefinitionRuntime::enabled);

  vm.override_function("B_GREG_COMESTODEXTER",std::function<void()>{[this,instanceId] {
    auto* dexter=findSymbol("DEXTER");
    const auto expected=instanceId(dexter);
    const auto actual=instanceId(vm.global_self());
    if(expected!=actual)
      return;
    auto* complete=findSymbol("B_GREG_COMESTODEXTER_ONETIME");
    const auto no=findSymbol("FALSE")->get_int();
    if(complete->get_int()!=no)
      return;

    constexpr uint16_t enemyOverride=61; // AIV_ENEMYOVERRIDE in the verified program.
    const auto context=dexter->get_instance();
    vm.unsafe_set_gi(context);
    if(context)
      findSymbol("C_NPC.AIVAR")->set_int(no,enemyOverride,context.get());
    findSymbol("GREGLOCATION")->set_int(findSymbol("GREG_DEXTER")->get_int());
    vm.push_reference(findSymbol("GREG_NW"));
    vm.push_reference(findSymbol("$12928"));
    vm.unsafe_call_bl(findSymbol("B_STARTOTHERROUTINE"));
    complete->set_int(findSymbol("TRUE")->get_int());
    }},RoutineDefinitionRuntime::enabled);
  }

void GameScript::loadNativeAiStates() {
  using Type = daedalus::DaedalusDataType;
  for(const auto& fn : vm.symbols()) {
    if(fn.type()==Type::FUNCTION && fn.is_const() && !fn.is_external())
      aiStates.emplace(fn.index(),AiState{fn.index(),size_t(-1),size_t(-1),fn.name()});
    }
  for(const auto& phase : vm.symbols()) {
    const std::string_view name = phase.name();
    const bool loop = name.ends_with("_LOOP");
    if(!loop && !name.ends_with("_END"))
      continue;
    const auto* init = vm.find_symbol_by_name(name.substr(0,name.size()-(loop ? 5 : 4)));
    if(init==nullptr)
      continue;
    if(const auto it = aiStates.find(init->index()); it!=aiStates.end()) {
      if(loop)
        it->second.funcLoop=phase.index();
      else
        it->second.funcEnd=phase.index();
      }
    }
  }

const AiState& GameScript::aiState(ScriptFn id) {
  return aiStates.try_emplace(id.ptr,AiState{id.ptr,size_t(-1),size_t(-1),{}}).first->second;
  }

void GameScript::runGregRoutineTransition(const daedalus::INpc& self) {
  const auto& dexter = findSymbol("DEXTER")->get_instance();
  if(dexter!=nullptr && dexter->symbol_index()==self.symbol_index() &&
     findSymbol("B_GREG_COMESTODEXTER_ONETIME")->get_int()==0) {
    int32_t gregResult = 0;
    if(!invokeStockProgram(*findSymbol("B_GREG_COMESTODEXTER"),gregResult,nullptr,true))
      throw std::runtime_error("Prepared Greg routine transition is unavailable");
    }
  }
