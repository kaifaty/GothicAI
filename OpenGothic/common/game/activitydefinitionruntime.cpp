#include "activitydefinitionruntime.h"

#include "game/npcactivityhost.h"
#include "world/objects/npc.h"

#include <array>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

namespace {
using Type = daedalus::DaedalusDataType;

// Accepted untimed states. ZS_ROAST_SCAVENGER is captured but never runs in a NEWWORLD day.
constexpr std::array<std::string_view,7> acceptedStates = {
  "ZS_SMITH_ANVIL",
  "ZS_SMITH_COOL",
  "ZS_SMITH_FIRE",
  "ZS_SMITH_SHARP",
  "ZS_READ_BOOKSTAND",
  "ZS_STOMP_HERB",
  "ZS_SMOKE_WATERPIPE",
  };

const daedalus::DaedalusSymbol* function(const daedalus::DaedalusScript& script, const std::string& name, bool result) {
  const auto* symbol = script.find_symbol_by_name(name);
  if(symbol==nullptr || symbol->type()!=Type::FUNCTION || symbol->is_external() || symbol->count()!=0 ||
     symbol->has_return()!=result || (result && symbol->rtype()!=Type::INT))
    throw std::runtime_error("invalid activity function "+name);
  return symbol;
  }
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
  if(ActivityDefinitions::seatedState(state))
    return true;
  for(auto name:acceptedStates)
    if(name==state)
      return true;
  return false;
  }

ActivityDefinitionRuntime::Definitions ActivityDefinitionRuntime::resolve(
    const ActivityDefinitions::Library& library, const daedalus::DaedalusScript& script, Mode mode) {
  Definitions definitions;
  if(mode==Mode::Off)
    return definitions;
  auto addPhases = [&](const std::string& name, ActivityEvents::Record record, const std::string& perceptions) {
    try {
      const auto* helper = function(script,perceptions,false);
      const std::array phases = {
        std::pair{function(script,name,false),ActivityDefinitions::Phase::Init},
        std::pair{function(script,name+"_LOOP",true),ActivityDefinitions::Phase::Loop},
        std::pair{function(script,name+"_END",false),ActivityDefinitions::Phase::End},
        };
      for(const auto& [symbol,phase]:phases) {
        if(phase==ActivityDefinitions::Phase::Loop && (record.point || record.mob->seated))
          continue; // explicit calls and the events-off fallback still use the script Loop
        definitions.phases.emplace(symbol->index(),Phase{record,phase,helper->index()});
        }
      }
    catch(const std::exception& e) { throw std::runtime_error("activity definitions: "+name+": "+e.what()); }
    };
  for(const auto& [name,activity]:library.activities) {
    if(ActivityDefinitions::seatedState(name)!=activity.seated.has_value())
      continue; // old records for a different seated script stay on the VM
    if(mode==Mode::Accepted && !accepted(name))
      continue;
    const auto& stored = definitions.activities.emplace(name,activity).first->second;
    addPhases(name,{&stored,nullptr},stored.perceptions);
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
    const auto& stored = definitions.points.emplace(*it).first->second;
    addPhases(it->first,{nullptr,&stored},stored.perceptions);
    }
  return definitions;
  }

ActivityDefinitionRuntime::Definitions ActivityDefinitionRuntime::load(
    const std::filesystem::path& base, const daedalus::DaedalusScript& program, std::string_view sha256) {
  const auto current = mode();
  if(current==Mode::Off)
    return Definitions{};
  return resolve(ActivityDefinitions::read(base,sha256),program,current);
  }

const ActivityDefinitionRuntime::Phase* ActivityDefinitionRuntime::find(const Definitions& definitions, uint32_t function) {
  const auto found = definitions.phases.find(function);
  return found==definitions.phases.end() ? nullptr : &found->second;
  }

int ActivityDefinitionRuntime::run(daedalus::DaedalusVm& vm, World& world, Npc& npc, const Phase& entry) {
  NpcActivityHost host(world,npc,[&vm,&entry](std::string_view) {
    vm.call_function<void>(vm.find_symbol_by_index(entry.helper));
    });
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
