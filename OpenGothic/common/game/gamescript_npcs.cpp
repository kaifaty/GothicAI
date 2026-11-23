#include "gamescript.h"

#include <stdexcept>
#include <utility>
#include <vector>

#include "world/objects/npc.h"

void GameScript::initializeNpcDefinition(const std::shared_ptr<daedalus::INpc>& npc,
                                         daedalus::DaedalusSymbol* symbol, bool useRecords) {
  if(npc->user_ptr)
    static_cast<Npc*>(npc->user_ptr)->npcInstanceSymbol=symbol->index();
  // A director NPC keeps its own symbol while using its template's definition.
  auto* like=own.likeOf(symbol->index());
  const NpcDefinitionRuntime::Definitions empty;
  NpcDefinitionRuntime::initialize(vm,npc,symbol,useRecords ? npcDefinitions : empty,like);
  }

void GameScript::refreshNpcAtInsert(Npc& npc) {
  auto& record = npc.handle();
  if(record.attribute[ATR_HITPOINTS]<=0)
    return;

  // Retain the saved local and compare instance identities, as the stock script
  // does even when the currently controlled character is not PC_HERO.
  const auto hero = hlp_getnpc(int(findSymbolIndex("PC_HERO")));
  findSymbol("B_REFRESHATINSERT.HER")->set_instance(hero);
  const auto self = std::static_pointer_cast<daedalus::INpc>(npc.handlePtr());
  if(hlp_getinstanceid(self)==hlp_getinstanceid(hero) || record.guild>=int32_t(GIL_SEPERATOR_HUM))
    return;

  record.attribute[ATR_HITPOINTS] = record.attribute[ATR_HITPOINTSMAX];
  if(npc_hasequippedweapon(self))
    return;

  switch(record.guild) {
    case GIL_MIL:
    case GIL_PAL:
    case GIL_SLD:
    case GIL_BAU:
    case GIL_NOV:
      createinvitems(self,int(findSymbolIndex("ITMW_1H_BAU_MACE")),1);
      break;
    }
  }

void GameScript::initPerceptions() {
  // New sessions can call this before a world is attached.
  if(!nativeRoutineProgram || !NpcDefinitionRuntime::enabled()) {
    if(hasSymbolName("initPerceptions"))
      vm.call_function("initPerceptions");
    return;
    }

  perc_setrange(PERC_ASSESSDAMAGE,       9999);
  perc_setrange(PERC_ASSESSOTHERSDAMAGE, 1000);
  perc_setrange(PERC_ASSESSDEFEAT,       2000);
  perc_setrange(PERC_ASSESSMURDER,       2000);
  perc_setrange(PERC_ASSESSTHREAT,       1000);
  perc_setrange(PERC_DRAWWEAPON,          500);
  perc_setrange(PERC_ASSESSFIGHTSOUND,   3000);
  perc_setrange(PERC_ASSESSQUIETSOUND,   1000);
  perc_setrange(PERC_ASSESSTHEFT,        1000);
  perc_setrange(PERC_ASSESSUSEMOB,       1000);
  perc_setrange(PERC_ASSESSENTERROOM,    1000);
  perc_setrange(PERC_ASSESSMAGIC,        9999);
  perc_setrange(PERC_ASSESSSTOPMAGIC,    9999);
  perc_setrange(PERC_ASSESSTALK,          500);
  perc_setrange(PERC_ASSESSWARN,         1000);
  perc_setrange(PERC_MOVEMOB,             500);
  perc_setrange(PERC_ASSESSSURPRISE,     3500);
  perc_setrange(PERC_OBSERVEINTRUDER,     100);
  perc_setrange(PERC_ASSESSREMOVEWEAPON,  100);
  perc_setrange(PERC_CATCHTHIEF,          100);
  perc_setrange(PERC_ASSESSCALL,          100);
  perc_setrange(PERC_MOVENPC,             100);
  perc_setrange(PERC_ASSESSCASTER,        100);
  perc_setrange(PERC_NPCCOMMAND,          100);
  perc_setrange(PERC_OBSERVESUSPECT,      100);
  }

void GameScript::loadNativeKillNpc() {
  if(!nativeRoutineProgram)
    return;
  vm.override_function("B_KILLNPC",std::function<void(int32_t)>{[this](int32_t symbol) { killNpc(symbol); }},
                       NpcDefinitionRuntime::enabled);
  }

void GameScript::invokeKillNpc(int32_t symbol) {
  if(!nativeRoutineProgram) {
    vm.call_function<void>("B_KILLNPC",symbol);
    return;
    }
  // Use the same call frame and gated dispatch as a script BL instruction.
  vm.push_int(symbol);
  vm.unsafe_call_bl(findSymbol("B_KILLNPC"));
  }

void GameScript::killNpc(int32_t symbol) {
  findSymbol("B_KILLNPC.NPCINSTANCE")->set_int(symbol);
  auto* local=findSymbol("B_KILLNPC.NPC");
  // Use the registered engine calls so a death during world init still runs
  // the startup floor barrier for the affected NPC and its neighbours.
  vm.push_int(symbol);
  vm.unsafe_call_external(findSymbol("HLP_GETNPC"));
  const auto npc=std::static_pointer_cast<daedalus::INpc>(vm.pop_instance());
  local->set_instance(npc);
  vm.push_instance(npc);
  vm.unsafe_call_external(findSymbol("NPC_ISDEAD"));
  const bool dead=vm.pop_int()!=0;
  vm.push_instance(npc);
  vm.unsafe_call_external(findSymbol("HLP_ISVALIDNPC"));
  const bool valid=vm.pop_int()!=0;
  if(dead || !valid)
    return;

  // The dispatch frame restores the member context on return.
  vm.unsafe_set_gi(npc);
  npc->flags=NpcFlag::NONE;
  vm.push_instance(npc);
  vm.push_int(int32_t(findSymbolIndex("ITMI_OLDCOIN")));
  vm.unsafe_call_external(findSymbol("CREATEINVITEM"));

  // The script reads its instance local and current HP after creating the item.
  const auto target=std::static_pointer_cast<daedalus::INpc>(local->get_instance());
  vm.unsafe_set_gi(target);
  vm.push_instance(target);
  vm.push_int(ATR_HITPOINTS);
  vm.push_int(target ? -target->attribute[ATR_HITPOINTS] : 0);
  vm.unsafe_call_external(findSymbol("NPC_CHANGEATTRIBUTE"));
  }

void GameScript::loadNativeNpcGlobals() {
  if(!nativeRoutineProgram)
    return;

  // Capture a typed chapter default and ordered alias table once. World entry
  // resolves the source NPCs anew against the current world.
  using Op=daedalus::DaedalusOpcode;
  auto pc=findSymbol("B_INITNPCGLOBALS")->address();
  const auto take=[&](Op expected) {
    const auto instruction=vm.instruction_at(pc);
    if(instruction.op!=expected)
      throw std::runtime_error("Invalid stock NPC global binding at "+std::to_string(pc));
    pc+=instruction.size;
    return instruction;
    };
  const auto unsetChapter=take(Op::PUSHI).immediate;
  const auto chapter=take(Op::PUSHV).symbol;
  take(Op::EQ);
  const auto bindStart=take(Op::BZ).address;
  const auto defaultChapter=take(Op::PUSHI).immediate;
  const auto destination=take(Op::PUSHV).symbol;
  take(Op::MOVI);
  if(pc!=bindStart || chapter!=destination || chapter!=findSymbolIndex("KAPITEL"))
    throw std::runtime_error("Invalid stock NPC global chapter default");

  const auto lookup=findSymbol("HLP_GETNPC")->index();
  std::vector<std::pair<int32_t,uint32_t>> bindings;
  while(vm.instruction_at(pc).op!=Op::RSR) {
    const auto source=take(Op::PUSHI).immediate;
    const auto external=take(Op::BE).symbol;
    const auto target=take(Op::PUSHVI).symbol;
    take(Op::MOVVI);
    if(external!=lookup)
      throw std::runtime_error("Invalid stock NPC global lookup");
    bindings.emplace_back(source,target);
    }

  vm.override_function("B_INITNPCGLOBALS",std::function<void()>{[this, chapter, unsetChapter, defaultChapter,
                                                                lookup, bindings=std::move(bindings)] {
    auto* chapterState=findSymbol(chapter);
    if(chapterState->get_int()==unsetChapter)
      chapterState->set_int(defaultChapter);
    for(const auto& [source,target]:bindings) {
      vm.push_int(source);
      vm.unsafe_call_external(findSymbol(lookup));
      findSymbol(target)->set_instance(vm.pop_instance());
      }
    }},NpcDefinitionRuntime::enabled);
  }
