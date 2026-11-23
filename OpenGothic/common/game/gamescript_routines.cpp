#include "gamescript.h"

int GameScript::initializeRoutine(const std::shared_ptr<daedalus::INpc>& npc,
                                   daedalus::DaedalusSymbol* function) {
  return RoutineDefinitionRuntime::initialize(vm,function,routineDefinitions,[&](const auto& row) {
    ta_min(npc,row.startHour,row.startMinute,row.endHour,row.endMinute,int(row.state),row.waypoint);
    });
  }

void GameScript::loadNativeRoutineExchange() {
  if(!nativeRoutineProgram)
    return;

  using Handle=std::shared_ptr<daedalus::INpc>;
  vm.override_function("B_STARTOTHERROUTINE",std::function<void(Handle,std::string_view)>{
    [this](Handle npc,std::string_view routine) {
      auto* actor=findSymbol("B_STARTOTHERROUTINE.SLF");
      auto* name=findSymbol("B_STARTOTHERROUTINE.NEWROUTINE");
      name->set_string(routine);
      actor->set_instance(npc);

      // Keep the existing engine boundaries, including the placement barrier.
      const auto call=[&](std::string_view external) {
        vm.push_reference(actor);
        vm.unsafe_call_external(findSymbol(external));
        };
      const auto instanceId=[&](daedalus::DaedalusSymbol* symbol) {
        vm.push_reference(symbol);
        vm.unsafe_call_external(findSymbol("HLP_GETINSTANCEID"));
        return vm.pop_int();
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
  }
