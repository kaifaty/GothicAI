#include "gamescript.h"

#include "game/definitions/basescripts.h"
#include "game/definitions/nativebasecatalog.h"
#include "loadprobe.h"
#include "world/objects/npc.h"

#include <stdexcept>

void GameScript::addPreparedRoutine(Npc* npc, const RoutineDefinitionRuntime::Routine& routine) {
  if(npc==nullptr)
    return;
  for(const auto& row : routine.rows) {
    LoadProbe::Startup::Detail::Scope detail("ta_min",[&]{return row.waypoint;});
    npc->addRoutine(gtime(row.startHour,row.startMinute),gtime(row.endHour,row.endMinute),row.state,row.waypoint);
    }
  }

void GameScript::loadNativeRoutineExchange() {
  using Actor=std::shared_ptr<daedalus::INpc>;
  nativeExternals.insert_or_assign("B_STARTOTHERROUTINE",storygraph::makeNativeInternal(
    std::function<void(Actor,std::string_view)>{[this](Actor actor,std::string_view routine) {
      startOtherRoutine(actor,routine);
      }}));
  nativeExternals.insert_or_assign("B_GREG_COMESTODEXTER",storygraph::makeNativeInternal(
    std::function<void()>{[this] {
      if(const auto self = nativeNpc("SELF"))
        runGregRoutineTransition(*self);
      }}));
  }

void GameScript::startOtherRoutine(const std::shared_ptr<daedalus::INpc>& actor,std::string_view routine) {
  setNpcRuleString("B_STARTOTHERROUTINE.NEWROUTINE",routine);
  storyActorSlot("B_STARTOTHERROUTINE.SLF") = actor;
  ai_standup(actor);
  const bool sameInstance = hlp_getinstanceid(actor)==hlp_getinstanceid(nativeNpc("SELF"));
  if(!sameInstance) {
    const bool dead = npc_isdead(actor);
    if(!hlp_isvalidnpc(actor) || dead)
      return;
    }
  npc_exchangeroutine(actor,npcRuleString("B_STARTOTHERROUTINE.NEWROUTINE"));
  if(!sameInstance)
    ai_continueroutine(actor);
  }

void GameScript::loadNativeAiStates() {
  const auto& catalog = *baseNativeCatalog();
  const auto phase = [](std::optional<uint32_t> id) { return id ? size_t(*id) : size_t(-1); };
  for(const auto& state : catalog.states)
    aiStates.emplace(state.id,AiState{state.id,phase(state.loop),phase(state.end),state.name});
  }

const AiState& GameScript::aiState(ScriptFn id) {
  return aiStates.try_emplace(id.ptr,AiState{id.ptr,size_t(-1),size_t(-1),{}}).first->second;
  }

void GameScript::runGregRoutineTransition(const daedalus::INpc& self) {
  const auto dexter = nativeNpc("DEXTER");
  if(dexter==nullptr || dexter->symbol_index()!=self.symbol_index() ||
     npcRuleInt("B_GREG_COMESTODEXTER_ONETIME")!=0)
    return;

  constexpr uint16_t enemyOverride=61; // AIV_ENEMYOVERRIDE in the verified program.
  dexter->aivar[enemyOverride] = 0;
  setNpcRuleInt("GREGLOCATION",npcRuleInt("GREG_DEXTER"));
  startOtherRoutine(nativeNpc("GREG_NW"),"Dexter");
  setNpcRuleInt("B_GREG_COMESTODEXTER_ONETIME",1);
  }
