#include "activitydefinitionruntime.h"

#include "game/npcactivityhost.h"
#include "world/objects/npc.h"

#include <array>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

namespace {
using Type = daedalus::DaedalusDataType;

// Accepted states: their day parity with the VM is proven, each on its own trace (D4.1 the anvil,
// D4.2 the other six). A table in code, so accepting another captured state changes the game, not
// the base. ZS_ROAST_SCAVENGER is captured but never runs in a NEWWORLD day: no parity, not accepted.
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
        if(record.point && phase==ActivityDefinitions::Phase::Loop)
          continue; // explicit calls and the events-off fallback still use the script Loop
        definitions.phases.emplace(symbol->index(),Phase{record,phase,helper->index()});
        }
      }
    catch(const std::exception& e) { throw std::runtime_error("activity definitions: "+name+": "+e.what()); }
    };
  for(const auto& [name,activity]:library.activities) {
    if(mode==Mode::Accepted && !accepted(name))
      continue;
    const auto& stored = definitions.activities.emplace(name,activity).first->second;
    addPhases(name,{&stored,nullptr},stored.perceptions);
    }
  const auto guard = library.points.find("ZS_STAND_GUARDING");
  if(guard!=library.points.end() && guard->second.form==ActivityDefinitions::PointActivity::Form::Guard &&
     guard->second.loopResult==LOOP_CONTINUE) {
    definitions.guard = std::make_unique<ActivityDefinitions::PointActivity>(guard->second);
    addPhases(guard->first,{nullptr,definitions.guard.get()},guard->second.perceptions);
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
    if(entry.phase==ActivityDefinitions::Phase::End) {
      npc.aiPush(AiQueue::aiPlayAnim(point->leaveAnimation));
      } else {
      // The importer verified the same distance-based Init as a mob, followed by the stage write.
      ActivityDefinitions::MobActivity approach;
      approach.perceptions = point->perceptions;
      approach.walkMode = point->walkMode;
      approach.approach = ActivityDefinitions::Approach::Distance;
      approach.approachDistance = point->approachDistance;
      ActivityDefinitions::run(approach,entry.phase,host);
      npc.handle().aivar[19] = point->initialStage;
      }
    return 0;
    }
  if(!observer)
    return ActivityDefinitions::run(*entry.record.mob,entry.phase,host);
  ActivityDefinitions::Recorder recorder(host,[&npc](std::string event) { observer(npc,event); });
  return ActivityDefinitions::run(*entry.record.mob,entry.phase,recorder);
  }
