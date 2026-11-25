#include "activitydefinitionruntime.h"

#include "game/npcactivityhost.h"
#include "world/objects/npc.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

namespace {
// Accepted untimed states.
constexpr std::array<std::string_view,13> acceptedStates = {
  "ZS_SMITH_ANVIL",
  "ZS_SMITH_COOL",
  "ZS_SMITH_FIRE",
  "ZS_SMITH_SHARP",
  "ZS_READ_BOOKSTAND",
  "ZS_STOMP_HERB",
  "ZS_SMOKE_WATERPIPE",
  "ZS_ROAST_SCAVENGER",
  "ZS_COOK_CAULDRON",
  "ZS_COOK_PAN",
  "ZS_COOK_STOVE",
  "ZS_PICK_ORE",
  "ZS_POTION_ALCHEMY",
  };

#ifndef GOTHIC_NATIVE_V3
using Type = daedalus::DaedalusDataType;

const daedalus::DaedalusSymbol* function(const daedalus::DaedalusScript& script, const std::string& name, bool result) {
  const auto* symbol = script.find_symbol_by_name(name);
  if(symbol==nullptr || symbol->type()!=Type::FUNCTION || symbol->is_external() || symbol->count()!=0 ||
     symbol->has_return()!=result || (result && symbol->rtype()!=Type::INT))
    throw std::runtime_error("invalid activity function "+name);
  return symbol;
  }
#endif


}

ActivityDefinitionRuntime::Mode ActivityDefinitionRuntime::mode() {
  const auto* value = std::getenv("OPENGOTHIC_ACTIVITY_RECORDS");
  if(value!=nullptr && std::strcmp(value,"0")==0)
    return Mode::Off;
  if(value!=nullptr && std::strcmp(value,"all")==0)
    return Mode::All;
  return Mode::Accepted;
  }

bool ActivityDefinitionRuntime::events() {
  const auto* value = std::getenv("OPENGOTHIC_ACTIVITY_EVENTS");
  return value==nullptr || std::strcmp(value,"0")!=0;
  }

bool ActivityDefinitionRuntime::accepted(std::string_view state) {
  if(ActivityDefinitions::timedMobState(state) || ActivityDefinitions::mobInitChecksBody(state))
    return true;
  for(auto name:acceptedStates)
    if(name==state)
      return true;
  return false;
  }

namespace {
struct PhaseBindings {
  std::array<uint32_t,3> ids;
  std::optional<uint32_t> legacyHelper;
  };

ActivityDefinitionRuntime::Definitions resolveRecords(const ActivityDefinitions::Library& library,
    ActivityDefinitionRuntime::Mode mode,
    const std::function<PhaseBindings(const std::string&,const std::string&)>& bindings) {
  using namespace ActivityDefinitionRuntime;
  Definitions definitions;
  if(mode==Mode::Off)
    return definitions;

  const auto addPhases = [&](const std::string& name, ActivityEvents::Record record, const std::string& perceptions) {
    try {
      const auto bound = bindings(name,perceptions);
      constexpr std::array phases = {ActivityDefinitions::Phase::Init, ActivityDefinitions::Phase::Loop,
                                     ActivityDefinitions::Phase::End};
      for(size_t i=0; i<phases.size(); ++i) {
        const auto phase = phases[i];
        if(phase==ActivityDefinitions::Phase::Loop && (record.point || record.mob->timer))
          continue; // Events handle the timed and point Loops.
        if(!definitions.phases.emplace(bound.ids[i],Phase{record,phase,name,bound.legacyHelper}).second)
          throw std::runtime_error("duplicate phase ID");
        }
      }
    catch(const std::exception& e) { throw std::runtime_error("activity definitions: "+name+": "+e.what()); }
    };
  for(const auto& [name,activity]:library.activities) {
    if(ActivityDefinitions::timedMobState(name)!=activity.timer.has_value() ||
       ActivityDefinitions::mobInitChecksBody(name)!=activity.skipApproachWhenBusy)
      continue; // old records that lack a verified timer or body check stay on the VM
    if(mode==Mode::Accepted && !accepted(name))
      continue;
    const auto& [storedName,stored] = *definitions.activities.emplace(name,activity).first;
    addPhases(storedName,{&stored,nullptr},stored.perceptions);
    }
  using Form = ActivityDefinitions::PointActivity::Form;
  constexpr std::array pointStates = {
    std::pair{"ZS_STAND_GUARDING",Form::Guard}, std::pair{"ZS_SIT_CAMPFIRE",Form::Campfire},
    std::pair{"ZS_STAND_ARMSCROSSED",Form::ArmsCrossed}, std::pair{"ZS_STAND_WP",Form::Waypoint},
    std::pair{"ZS_PEE",Form::Pee},
    };
  for(const auto& [name,form]:pointStates) {
    const auto it = library.points.find(name);
    if(it==library.points.end() || it->second.form!=form || it->second.loopResult!=LOOP_CONTINUE)
      continue;
    const auto& [storedName,stored] = *definitions.points.emplace(*it).first;
    addPhases(storedName,{nullptr,&stored},stored.perceptions);
    }
  return definitions;
  }

}

#ifndef GOTHIC_NATIVE_V3
ActivityDefinitionRuntime::Definitions ActivityDefinitionRuntime::resolve(
    const ActivityDefinitions::Library& library, const daedalus::DaedalusScript& script, Mode mode) {
  return resolveRecords(library,mode,[&](const std::string& name,const std::string& perceptions) {
    const auto* helper = function(script,perceptions,false);
    return PhaseBindings{{function(script,name,false)->index(),
                          function(script,name+"_LOOP",true)->index(),
                          function(script,name+"_END",false)->index()},helper->index()};
    });
  }
#endif

ActivityDefinitionRuntime::Definitions ActivityDefinitionRuntime::resolve(
    const ActivityDefinitions::Library& library, const NativeBaseCatalog::Selection& catalog) {
  return resolveRecords(library,Mode::Accepted,[&](const std::string& name,const std::string& perceptions) {
    if(perceptions!="PERCEPTION_SET_NORMAL")
      throw std::runtime_error("unbound perception preset "+perceptions);
    const auto state = std::ranges::find(catalog.states,name,&NativeBaseCatalog::State::name);
    if(state==catalog.states.end() || !state->loop || !state->end)
      throw std::runtime_error("missing activity state phase");
    return PhaseBindings{{state->id,*state->loop,*state->end},std::nullopt};
    });
  }

#ifndef GOTHIC_NATIVE_V3
ActivityDefinitionRuntime::Definitions ActivityDefinitionRuntime::load(
    const std::filesystem::path& base, const daedalus::DaedalusScript& program, std::string_view sha256) {
  const auto current = mode();
  if(current==Mode::Off)
    return Definitions{};
  return resolve(ActivityDefinitions::read(base,sha256),program,current);
  }
#endif

ActivityDefinitionRuntime::Definitions ActivityDefinitionRuntime::load(
    const std::filesystem::path& base, const NativeBaseCatalog::Selection& catalog, std::string_view sha256) {
  return resolve(ActivityDefinitions::read(base,sha256),catalog);
  }

const ActivityDefinitionRuntime::Phase* ActivityDefinitionRuntime::find(const Definitions& definitions, uint32_t function) {
  const auto found = definitions.phases.find(function);
  return found==definitions.phases.end() ? nullptr : &found->second;
  }

int ActivityDefinitionRuntime::run(World& world, Npc& npc, const Phase& entry,
                                   const std::function<void(std::string_view)>& helper,
                                   const std::function<void(int32_t)>& createItem) {
  NpcActivityHost host(world,npc,helper,createItem);
  if(const auto* point=entry.record.point) {
    using Form = ActivityDefinitions::PointActivity::Form;
    if(entry.phase==ActivityDefinitions::Phase::End) {
      if(point->form==Form::Campfire)
        npc.aiPush(AiQueue::aiPlayAnimBs(point->leaveAnimation,BodyState(point->leaveBodyState)));
      else
        npc.aiPush(AiQueue::aiPlayAnim(point->leaveAnimation));
      return 0;
      }
    ActivityDefinitions::resetForActivity(host,point->perceptions);
    if(point->form==Form::Campfire && ActivityDefinitions::bodyStateContains(host.bodyState(),point->enterBodyState))
      return 0;
    host.setWalkMode(point->walkMode);
    const auto wp=host.waypoint();
    if(point->form==Form::Waypoint) {
      host.gotoWaypoint(wp);
      npc.aiPush(AiQueue::aiAlignToWp());
      }
    else if(host.distanceToWaypoint(wp)>point->approachDistance)
      host.gotoWaypoint(wp);
    if(point->form!=Form::Campfire)
      npc.handle().aivar[19] = point->initialStage;
    return 0;
    }
  if(!observer)
    return ActivityDefinitions::run(*entry.record.mob,entry.phase,host);
  ActivityDefinitions::Recorder recorder(host,[&npc](std::string event) { observer(npc,event); });
  return ActivityDefinitions::run(*entry.record.mob,entry.phase,recorder);
  }
