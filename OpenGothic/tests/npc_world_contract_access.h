#pragma once

#include "storygraph/exec.h"
#include "game/gamescript.h"
#include "game/gamescript_programs.h"
#include "game/serialize.h"
#include "graphics/mesh/skeleton.h"
#include "world/objects/interactive.h"
#include "world/objects/item.h"
#include "world/objects/npc.h"
#include "world/waypoint.h"
#include "world/world.h"

#include <Tempest/MemReader>
#include <Tempest/MemWriter>
#include <stdexcept>
#include <string>
#include <vector>
void npcPetzCrimeParity(World& world,Npc& witness,Npc& hero,bool& enabled);

struct StockOracleActors {
  daedalus::DaedalusVm& vm;
  std::shared_ptr<daedalus::DaedalusInstance> self, other;

  StockOracleActors(daedalus::DaedalusVm& vm, Npc* npc, Npc* player)
    : vm(vm), self(vm.global_self()->get_instance()), other(vm.global_other()->get_instance()) {
    vm.global_self()->set_instance(npc ? std::static_pointer_cast<daedalus::INpc>(npc->handlePtr()) : nullptr);
    vm.global_other()->set_instance(player ? std::static_pointer_cast<daedalus::INpc>(player->handlePtr()) : nullptr);
    }

  ~StockOracleActors() {
    vm.global_self()->set_instance(self);
    vm.global_other()->set_instance(other);
    }
  };

#if defined(GOTHIC_BENNET_LOOKUP_AUDIT)
namespace BennetLookupAudit {
  inline thread_local size_t depth = 0;
  inline thread_local size_t scopes = 0;
  inline thread_local size_t lookups = 0;

  struct Rejected : std::runtime_error {
    using std::runtime_error::runtime_error;
    };

  inline void check(const char* method) {
    if(depth==0)
      return;
    ++lookups;
    throw Rejected(std::string("StoryGraph queried the full symbol table: ")+method);
    }
}
#endif

#if defined(GOTHIC_BENNET_LOOKUP_WRAPPERS)
// The linker redirects references from the game objects to these out-of-line lookups.
#define GOTHIC_WRAP_LOOKUP(abi, script_type, key_type, symbol_type, method) \
  extern "C" symbol_type* __real_##abi(script_type*, key_type); \
  extern "C" symbol_type* __wrap_##abi(script_type* script, key_type key) { \
    BennetLookupAudit::check(#method); \
    return __real_##abi(script,key); \
    }

GOTHIC_WRAP_LOOKUP(_ZN8daedalus14DaedalusScript19find_symbol_by_nameESt17basic_string_viewIcSt11char_traitsIcEE,
                   daedalus::DaedalusScript, std::string_view, daedalus::DaedalusSymbol, find_symbol_by_name)
GOTHIC_WRAP_LOOKUP(_ZNK8daedalus14DaedalusScript19find_symbol_by_nameESt17basic_string_viewIcSt11char_traitsIcEE,
                   const daedalus::DaedalusScript, std::string_view, const daedalus::DaedalusSymbol, find_symbol_by_name)
GOTHIC_WRAP_LOOKUP(_ZN8daedalus14DaedalusScript20find_symbol_by_indexEj,
                   daedalus::DaedalusScript, uint32_t, daedalus::DaedalusSymbol, find_symbol_by_index)
GOTHIC_WRAP_LOOKUP(_ZNK8daedalus14DaedalusScript20find_symbol_by_indexEj,
                   const daedalus::DaedalusScript, uint32_t, const daedalus::DaedalusSymbol, find_symbol_by_index)
GOTHIC_WRAP_LOOKUP(_ZN8daedalus14DaedalusScript22find_symbol_by_addressEj,
                   daedalus::DaedalusScript, uint32_t, daedalus::DaedalusSymbol, find_symbol_by_address)
GOTHIC_WRAP_LOOKUP(_ZNK8daedalus14DaedalusScript22find_symbol_by_addressEj,
                   const daedalus::DaedalusScript, uint32_t, const daedalus::DaedalusSymbol, find_symbol_by_address)

#undef GOTHIC_WRAP_LOOKUP
#endif

// Test-only access keeps the production creation entry point unchanged.
class GameStoryExternals final : public storygraph::Externals {
  public:
    GameStoryExternals(GameScript& script, bool effects);

    storygraph::Value call(const daedalus::DaedalusSymbol& fn, const std::vector<storygraph::Value>& args) override;

  private:
    GameScript&               script;
    storygraph::VmExternals   fallback;
    bool                      effects;
  };

inline GameStoryExternals::GameStoryExternals(GameScript& script, bool effects)
  : script(script), fallback(script.getVm()), effects(effects) {
  }

inline storygraph::Value GameStoryExternals::call(const daedalus::DaedalusSymbol& fn, const std::vector<storygraph::Value>& args) {
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

struct NpcDefinitionContract {
#if defined(GOTHIC_BENNET_LOOKUP_AUDIT)
  static void watchBennetLookups(GameScript& script) {
    BennetLookupAudit::depth = 0;
    BennetLookupAudit::scopes = 0;
    BennetLookupAudit::lookups = 0;
    script.bennetLookupScope=[](bool enter) {
      if(enter) {
        ++BennetLookupAudit::depth;
        ++BennetLookupAudit::scopes;
        } else {
        --BennetLookupAudit::depth;
        }
      };
    }

  static size_t bennetLookupCount() { return BennetLookupAudit::lookups; }
  static size_t bennetLookupScopeCount() { return BennetLookupAudit::scopes; }
  static bool isBennetOreDialog(const GameScript& script, const daedalus::IInfo& info) {
    return script.bennetOreDialog(info)!=nullptr;
    }
  static auto& bennetAnswer(GameScript& script) { return script.bennetOre->dialogs.back().answer; }
  static auto& bennetTransferObserver(GameScript& script) { return script.bennetTransferObserver; }

  static void checkBennetLookupProbe(GameScript& script) {
    auto& writable = static_cast<daedalus::DaedalusScript&>(script.getVm());
    const auto& readable = writable;
    const auto rejects=[](auto lookup) {
      struct Scope {
        Scope() { ++BennetLookupAudit::depth; }
        ~Scope() { --BennetLookupAudit::depth; }
        } scope;
      try {
        lookup();
        }
      catch(const BennetLookupAudit::Rejected&) { return; }
      throw std::runtime_error("Bennet ore lookup audit missed an intentional symbol lookup");
      };
    rejects([&] { (void)writable.find_symbol_by_name("DIA_BENNET_BRINGORE"); });
    rejects([&] { (void)readable.find_symbol_by_name("DIA_BENNET_BRINGORE"); });
    rejects([&] { (void)writable.find_symbol_by_index(23822); });
    rejects([&] { (void)readable.find_symbol_by_index(23822); });
    rejects([&] { (void)writable.find_symbol_by_address(0); });
    rejects([&] { (void)readable.find_symbol_by_address(0); });
    }
#endif

  static auto& definitions(GameScript& script) { return script.npcDefinitions; }
  static void runRoutine(GameScript& script, Npc& npc, uint32_t function) {
    npc.clearRoutine();
    script.invokeState(&npc,npc.currentOther,npc.currentVictim,ScriptFn(function));
    }
  static auto& placements(GameScript& script) { return script.placementDefinitions; }
  static int32_t& storyChapter(GameScript& script) { return script.storyChapter; }
  static int32_t currentStoryWorld(GameScript& script) { return script.currentStoryWorld; }
  static const auto& worldEntryLatches(GameScript& script) { return script.worldEntryLatches; }
  static const auto& deathProgress(GameScript& script) { return script.deathProgress; }
  static void saveChapter(GameScript& script, Serialize& out) { script.saveSym(out,*script.findSymbol("KAPITEL")); }
  static auto& crimeAbsolutionLevels(GameScript& script) { return script.crimeAbsolutionLevels; }
  static auto& crimeCounters(GameScript& script) { return script.petzCrimeCounters; }
  static auto& questLog(GameScript& script) { return script.quests; }
  static void saveCrimeGlobal(GameScript& script, Serialize& out, daedalus::DaedalusSymbol& symbol) { script.saveSym(out,symbol); }
  static auto& activities(GameScript& script) { return script.activityDefinitions; }
  static const auto& pointGestureRolls(const GameScript& script) { return script.pointGestureRolls; }
  static bool pointGuardRollMatches(GameScript& script, int32_t actual, int32_t expected) {
    return actual==expected && (!script.usesVerifiedNpcProgram() || pointGestureRolls(script).guard==expected);
    }
  static bool pointAndQuakeRollsMatch(GameScript& script, bool folded, const std::array<int32_t,3>& actual,
                                      const std::array<int32_t,3>& expected) {
    if(actual!=expected)
      return false;
    if(!script.usesVerifiedNpcProgram())
      return true;
    const auto& owned=pointGestureRolls(script);
    return std::array{folded ? owned.crossedArms : owned.waypoint,owned.portal,owned.earthquake}==expected;
    }
  static void checkDirectorNpc(GameScript& script, const daedalus::DaedalusSymbol* owner, const std::string& error) {
    if(owner==nullptr)
      throw std::runtime_error("create NPC record template fixture: "+error);
    if(script.findSymbolIndex("dir_npc_record_contract")!=owner->index() ||
       script.symbolName(owner->index())!=owner->name() ||
       script.storySymbolKind("dir_npc_record_contract")!=GameScript::SymbolKind::Npc)
      throw std::runtime_error("director-defined NPC metadata must remain visible alongside the stock catalog");
    }
  static auto& aiState(Npc& npc) { return npc.aiState; }
  static int32_t& lastHitSpell(Npc& npc) { return npc.lastHitSpell; }
  static int32_t activeSpellInfo(const Npc& npc) { return npc.spellInfo; }
  static ScriptFn perception(const Npc& npc, PercType type) { return npc.perception[type].func; }
  static auto& activityEvents(Npc& npc) { return npc.activityEvents; }
  static bool& activityEventsOn(GameScript& script) { return script.activityEvents; }
  static const auto& dialogInfos(const GameScript& script) { return script.dialogsInfo; }
  static const auto& transferVariables(const GameScript& script) { return script.inventoryTransferVariables; }
  static int32_t dialogCondition(GameScript& script,daedalus::DaedalusSymbol* fn) { return script.dialogFunction(fn->index()); }
  static bool hasPreparedStockProgram(const GameScript& script, const daedalus::DaedalusSymbol& fn) {
    if(!script.stockPrograms || !script.stockPrograms->prepared)
      return false;
    const auto& programs=script.stockPrograms->prepared->programs;
    const auto entry=programs.find(fn.index());
    return entry!=programs.end() && entry->second.name==fn.name();
    }
  static auto& previousState(Npc& npc) { return npc.aiPrevState; }
  static void lastHitSpell(Npc& npc, int32_t spell) { npc.lastHitSpell=spell; }
  static ScriptFn perceptionCallback(const Npc& npc, PercType type) { return npc.perception[type].func; }
  static void tickRoutine(Npc& npc) { npc.aiState.loopNextTime=npc.world().tickCount(); npc.tickRoutine(); }
  static void tickScheduledRoutine(Npc& npc) { npc.tickRoutine(); }
  // The engine's next queued action, as the NPC's tick takes it.
  static void nextAiAction(Npc& npc) { npc.nextAiAction(npc.aiQueue,0); }
  static size_t pendingItemUse(const Npc& npc) { return npc.itemUse.item; }
  static void finishItemUseAnimation(Npc& npc) {
    Animation::EvCount events;
    events.timed.push_back({asset::MdsEventType::ITEM_DESTROY});
    npc.tickTimedEvt(events);
    npc.visual.pose().interruptItemUse(); // Finish the clip before the next fixture case.
    }
  static const auto& foodPool(const Npc& npc) { return npc.foodPool; }
  static void tickActivityDeadline(Npc& npc) { npc.tickActivityDeadline(); }
  static const AiQueue& queue(const Npc& npc) { return npc.aiQueue; }
  static AiQueue& queue(Npc& npc) { return npc.aiQueue; }
  static uint64_t& refusalDeadline(Npc& npc) { return npc.refuseTalkMilis; }
  static auto& perceptions(Npc& npc) { return npc.perception; }
  static uint64_t perceptionTime(const Npc& npc) { return npc.perceptionTime; }
  static const AiQueue& overlayQueue(const Npc& npc) { return npc.aiQueueOverlay; }
  static bool walking(const Npc& npc) { return !npc.go2.empty(); }
  static const auto& physicsPosition(const Npc& npc) { return npc.physic.position(); }
  // An action at the head of the queue, as an interruption ahead of the queued use.
  static void pushFront(Npc& npc, AiQueue::AiAction&& a) { npc.aiQueue.pushFront(std::move(a)); }
  static const auto& items(GameScript& script) { return script.itemDefinitions; }
  static const auto& routines(GameScript& script) { return script.routineDefinitions; }
  static auto& random(GameScript& script) { return script.randGen; }
  static auto& guildAttitudes(GameScript& script) { return script.gilAttitudes; }
  static int& outputOrder(GameScript& script) { return script.aiOutOrderId; }
  static auto& passivePerceptions(World& world) { return world.wobj.sndPerc; }
  // the views of items (graphics/itemview): the sword and the bow on a body, a lying item's
  static const ItemView& sword(const Npc& npc) { return npc.visual.sword.view; }
  static const ItemView& bow(const Npc& npc) { return npc.visual.bow.view; }
  static const ItemView* lying(const Item& item) {
    const auto& v=item.visual;
    return v.type==ObjVisual::M_Mesh && v.mesh.worldItem ? &v.mesh.item : nullptr;
    }
  static StreamedVisual& visual(Item& item) { return item.visual; }
  static const ItemView& stuckArrow(const Npc& npc, size_t i) { return npc.visual.stuckArrows[i].view; }
  // where a stuck arrow is by its own bone and offset: the bone's rendered matrix times the pin's local one
  static Tempest::Vec3 pinAt(const Npc& npc, size_t i) {
    const auto& pin=npc.visual.stuckArrows[i];
    const auto m=npc.visual.renderedBone(pin.bone)*pin.local;
    return {m.at(3,0),m.at(3,1),m.at(3,2)};
    }
  // a stuck arrow's record in main's own bytes (mdlvisual_quiver.cpp, APR1 version 1), read by the current loader
  static bool loadMainArrow(Npc& npc, World& world, size_t i) {
    const auto& pin=npc.visual.stuckArrows[i];
    const std::string skeleton(npc.visual.skeleton->name()),bone(npc.visual.skeleton->nodes[pin.bone].name),visual=pin.visual;
    const auto local=pin.local;
    std::vector<uint8_t> raw;
    {
      Tempest::MemWriter writer(raw);
      Serialize out(writer);
      out.setEntry("contract/arrows");
      const auto name=[&](const std::string& v) {
        const uint32_t n=uint32_t(v.size());
        out.writeBytes(&n,sizeof(n));
        out.writeBytes(v.data(),v.size());
        };
      const uint32_t magic=0x31525041,count=1;
      const uint16_t version=1;
      out.writeBytes(&magic,sizeof(magic));
      out.writeBytes(&version,sizeof(version));
      name(skeleton);
      out.writeBytes(&count,sizeof(count));
      name(visual);
      name(bone);
      out.writeBytes(&local,sizeof(local));
    }
    Tempest::MemReader reader(raw);
    Serialize in(reader);
    return in.setEntry("contract/arrows") && npc.visual.loadStuckArrows(in,world);
    }
  static auto& isolatedRandom(GameScript& script) { return script.isolatedRandom; }
  // an object's model and its effects of the animations' events (MdlVisual::startEffect)
  static const MdlVisual& model(const Interactive& mob) { return mob.objVisual().mdl.view; }
  static const auto& effects(const MdlVisual& view) { return view.effects; }
  static const auto& effects(const Npc& npc) { return effects(npc.visual); }
  static size_t bone(const MdlVisual& view, std::string_view name) { return view.skeleton->findNode(name); }
  static const AnimationSolver& solver(const MdlVisual& view) { return view.solver; }
  };

namespace NpcWorldFixture {

inline void require(bool condition, const char* message) {
  if(!condition)
    throw std::runtime_error(message);
  }

inline std::vector<uint8_t> savedNpc(Npc& npc) {
  std::vector<uint8_t> bytes;
  {
    Tempest::MemWriter writer(bytes);
    Serialize out(writer);
    out.setContext(&npc.world());
    npc.save(out,0,"/npc/");
    }
  return bytes;
  }

inline void loadSavedNpc(Npc& npc,const std::vector<uint8_t>& bytes) {
  Tempest::MemReader reader(bytes);
  Serialize in(reader);
  in.setContext(&npc.world());
  npc.load(in,0,"/npc/");
  if(npc.world().player()==&npc)
    npc.world().script().setInstanceNPC("HERO",npc);
  npc.postValidate();
  npc.setSimTier(NpcSimTier::Full);
  }

inline Npc* activityNpc(World& world,const char* name) {
  const auto symbol=world.script().findSymbolIndex(name);
  for(uint32_t i=0;i<world.npcCount();++i)
    if(world.npcById(i)->instanceSymbol()==symbol)
      return world.npcById(i);
  return nullptr;
  }

// The NPC's queue as text, front first, without taking it.
inline std::vector<std::string> activityQueue(const Npc& npc) {
  auto queue=NpcDefinitionContract::queue(npc);
  std::vector<std::string> actions;
  while(queue.size()>0) {
    const auto a=queue.pop();
    actions.push_back(std::string(AiQueue::actionName(uint32_t(a.act)))+" "+std::to_string(a.i0)+" "+std::to_string(a.i1)+" "+
                      a.s0+" "+(a.point ? a.point->name : "")+" "+(a.target ? std::string(a.target->displayName()) : "")+" "+
                      std::to_string(a.func.ptr));
    }
  return actions;
  }

void oldWorldNpcCases(World& world,Npc& context);
void dragonIslandChapterWorld(World& world);
void prepareRoutineWitnessWorld(World& world, bool events);
void gregAtDexter(World& world);
void companionPlayerPerceptions(World& world,Npc& npc,Npc& hero);
void fightSoundPerceptions(World& world,Npc& npc,Npc& hero);

} // namespace NpcWorldFixture
