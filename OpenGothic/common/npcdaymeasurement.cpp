#include "npcdaymeasurement.h"
#include "npcdayjournal.h"

#include "game/gamerules.h"
#include "game/gamescript_daystats.h"
#include "game/definitions/basescripts.h"
#include "game/definitions/gameplaydefinitions.h"
#include "game/definitions/nativebasecatalog.h"
#include "resources.h"
#include "utils/jsonescape.h"
#include "world/mobreservations.h"
#include "world/npcactivityprobe.h"
#include "world/objects/interactive.h"
#include "world/objects/item.h"
#include "world/objects/npc.h"
#include "world/waypoint.h"
#include "world/world.h"

#ifndef GOTHIC_NATIVE_V3
#include "game/definitions/baseprogram.h"
#include "game/gamescript.h"
#include "gothic.h"
#include "programdata.h"
#include "settings.h"
#endif

#include <cstdio>
#include <memory>
#include <stdexcept>

namespace {
std::string_view stageName(ActivityEvents::Stage stage) {
  using ActivityEvents::Stage;
  switch(stage) {
    case Stage::Off:         return "off";
    case Stage::Approach:    return "approach";
    case Stage::Attach:      return "attach";
    case Stage::WalkToPoint: return "walk_to_point";
    case Stage::Wait:        return "wait";
    case Stage::Hold:        return "hold";
    }
  return {};
  }

class Measurement final : public NpcActivityProbe {
  public:
    World& world;
    std::FILE* output=nullptr;
    std::FILE* trace=nullptr;
    std::shared_ptr<DayScriptStats> stats;
#ifndef GOTHIC_NATIVE_V3
    daedalus::DaedalusVm* observedVm=nullptr;
#endif
    std::vector<std::string> selected;
    std::string header;
    struct Context { const Npc* npc; std::string symbol; };
    std::vector<Context> phases;
    std::map<uint32_t,uint32_t> bodies;

    explicit Measurement(World& w):world(w) {}
    ~Measurement() {
      if(output) std::fclose(output);
      if(trace) std::fclose(trace);
      }
    std::string symbol(const Npc* npc) {
      return npc ? std::string(world.rules().symbolName(npc->instanceSymbol())) : "";
      }
    bool watches(const Npc* npc) {
      if(!trace || !npc) return false;
      const auto id=std::to_string(world.npcId(npc));
      const auto name=symbol(npc);
      for(const auto& value:selected)
        if(value==id || value==name || value==npc->displayName()) return true;
      return false;
      }
    void emit(const Npc* npc, std::string_view kind, const std::string& fields) {
      if(!watches(npc)) return;
      const auto context=phases.empty() ? "" : phases.back().symbol;
      std::fprintf(trace,"{\"tick\":%llu,\"world_ms\":%lld,\"npc\":%u,\"symbol\":%s,\"phase_symbol\":%s,\"kind\":%s%s}\n",
                   (unsigned long long)world.tickCount(),(long long)world.time().toInt(),world.npcId(npc),
                   jsonEscape(symbol(npc)).c_str(),jsonEscape(context).c_str(),jsonEscape(kind).c_str(),fields.c_str());
      }
    void body(const Npc& npc) {
      if(!watches(&npc)) return;
      const auto id=world.npcId(&npc), value=uint32_t(npc.bodyStateMasked());
      const auto found=bodies.find(id);
      if(found!=bodies.end() && found->second==value) return;
      const auto from=found==bodies.end() ? -1ll : (long long)found->second;
      bodies[id]=value;
      emit(&npc,"body",",\"from\":"+std::to_string(from)+",\"to\":"+std::to_string(value));
      }
    void phaseBegin(const Npc* npc, std::string_view name) override {
      phases.push_back({npc,std::string(name)});
      if(stats) stats->begin(name);
      if(npc) body(*npc);
      const auto [state,phase]=DayScriptStats::statePhase(name);
      emit(npc,"phase_begin",",\"state\":"+jsonEscape(state)+",\"phase\":"+jsonEscape(phase));
      }
    void phaseEnd(int result) override {
      const auto* npc=phases.back().npc;
      if(npc) body(*npc);
      emit(npc,"phase_end",",\"result\":"+std::to_string(result));
      if(stats) stats->end();
      phases.pop_back();
      }
    void phaseRecord() override {
      if(stats) stats->record();
      }
    void random(const Npc* npc, uint32_t bound, uint32_t result, bool script) override {
      if(stats) stats->random(script,npc!=nullptr);
      if(!watches(npc)) return;
      const auto source=stats ? stats->source(script,npc!=nullptr) : (script ? "script" : "engine");
      emit(npc,"random",",\"bound\":"+std::to_string(bound)+",\"result\":"+std::to_string(result)+",\"source\":"+jsonEscape(source));
      }
    void queued(const Npc* npc, const AiQueue::AiAction& a) override {
      if(!watches(npc)) return;
      const auto number=[](int value) {return std::to_string(value);};
      std::string item;
      if(a.item) item=world.rules().symbolName(a.item->clsId());
      else if(a.act==AI_UseItem || a.act==AI_UseItemToState || a.act==AI_EquipArmor)
        item=world.rules().symbolName(size_t(a.i0));
      emit(npc,"queue",",\"action\":"+number(int(a.act))+",\"action_name\":"+jsonEscape(AiQueue::actionName(uint32_t(a.act)))+",\"i0\":"+number(a.i0)+",\"i1\":"+number(a.i1)+
           ",\"i2\":"+number(a.i2)+",\"s0\":"+jsonEscape(a.s0)+",\"s1\":"+jsonEscape(a.s1)+
           ",\"point\":"+jsonEscape(a.point ? a.point->name : "")+",\"target\":"+jsonEscape(symbol(a.target))+
           ",\"victim\":"+jsonEscape(symbol(a.victim))+",\"function\":"+jsonEscape(world.rules().symbolName(a.func.ptr))+
           ",\"item\":"+jsonEscape(item));
      }
    void point(const Npc& npc, const WayPoint* from, const WayPoint* to) override {
      if(from) emit(&npc,"point_release",",\"name\":"+jsonEscape(from->name));
      if(to) emit(&npc,"point_occupy",",\"name\":"+jsonEscape(to->name));
      }
    void mob(const Npc& npc, const Interactive& object, bool occupied) override {
      emit(&npc,occupied ? "mob_occupy" : "mob_release",",\"name\":"+jsonEscape(object.vobNameStr())+
           ",\"zen_id\":"+std::to_string(object.zenId())+",\"scheme\":"+jsonEscape(object.schemeName()));
      body(npc);
      }
    void activityTransition(const Npc& npc, uint16_t wakes, ActivityEvents::Stage before) override {
      if(!watches(&npc)) return;
      std::string reasons = "[";
      for(size_t i=0;i<ActivityEvents::WakeCount;++i) {
        if((wakes & (1u<<i))==0) continue;
        if(reasons.size()>1) reasons+=",";
        reasons+=jsonEscape(ActivityEvents::wakeName(i));
        }
      reasons+="]";

      const auto* point=npc.currentWayPoint();
      const auto* object=npc.interactive();
      if(!object) object=world.mobReservations().reserved(npc);
      const auto* head=npc.aiQueueFront();
      const bool owns=object || (point && point->isFreePoint() && npc.lockedWayPoint()==point);
      emit(&npc,"activity_transition",
           ",\"state\":"+jsonEscape(world.rules().symbolName(npc.currentState().ptr))+
           ",\"reasons\":"+reasons+",\"from\":"+jsonEscape(stageName(before))+
           ",\"to\":"+jsonEscape(stageName(npc.activityStage()))+
           ",\"executor\":\"engine\",\"place\":"+jsonEscape(object ? object->vobNameStr() : point ? point->name : "")+
           ",\"owner\":"+(owns ? std::to_string(world.npcId(&npc)) : "null")+
           ",\"queue_head\":"+jsonEscape(head ? AiQueue::actionName(uint32_t(head->act)) : ""));
      }
    void physicsNoise(const Npc& npc, const Npc* culprit, bool vmCallback) override {
      if(!watches(&npc)) return;
      emit(&npc,"physics_noise",",\"culprit\":"+jsonEscape(symbol(culprit))+
           ",\"executor\":"+jsonEscape(vmCallback ? "vm" : "engine"));
      }
  };
std::unique_ptr<Measurement> measurement;
}

bool NpcDayMeasurement::start(World& w, const NpcDayJournal::Options& options, std::string& error) {
  if(NpcActivityProbe::instance!=nullptr) {
    error="an activity observation is already running";
    return false;
    }
  try {
    auto m=std::make_unique<Measurement>(w);
    const auto& directory=options.directory;
    if(options.scriptProbe) m->output=std::fopen((directory+"/npc-day-script.json").c_str(),"w");
    if(!options.traceNpcs.empty()) m->trace=std::fopen((directory+"/npc-day-activity.jsonl").c_str(),"w");
    if((options.scriptProbe && !m->output) || (!options.traceNpcs.empty() && !m->trace))
      throw std::runtime_error("cannot write day measurements in "+directory);
    m->selected=options.traceNpcs;
    m->header="\"seed\":"+std::to_string(options.seed)+",\"stream\":"+jsonEscape(options.isolate ? "npc" : "shared")+
              ",\"start_world_ms\":"+std::to_string(w.time().toInt());
    if(options.scriptProbe) {
      if(const auto* catalog=baseNativeCatalog()) {
        const auto routines=RoutineDefinitions::read(Resources::baseDir(),GameplayDefinitions::stockProgram);
        m->stats=std::make_shared<DayScriptStats>(routines,DayScriptStats::Metric::NativePhases);
        for(const auto& state:catalog->states)
          m->stats->registerBeastState(state.name);
        } else {
#ifndef GOTHIC_NATIVE_V3
        // Use the same program selection as GameScript, regardless of the routine playback switch.
        const auto program=selectBaseProgram(baseProgramIndex(),Gothic::inst().defaultGameDatFile(),
                                             Settings::getI("GAME","language")).source.recordSha256;
        const auto routines=RoutineDefinitions::read(Resources::baseDir(),program);
        m->observedVm=&w.script().getVm();
        m->stats=DayScriptStats::attach(*m->observedVm,routines);
#else
        throw std::runtime_error("NPC day measurements require a native game catalog");
#endif
        }
      }
    if(m->trace) std::fprintf(m->trace,"{\"kind\":\"header\",\"trace_version\":2,%s}\n",m->header.c_str());
    measurement=std::move(m);
    NpcActivityProbe::instance=measurement.get();
    tick(w);
    return true;
    }
  catch(const std::exception& e) { error=e.what(); return false; }
  }

void NpcDayMeasurement::tick(World& w) {
  if(!measurement || !measurement->trace) return;
  for(uint32_t i=0;i<w.npcCount();++i) measurement->body(*w.npcById(i));
  std::fflush(measurement->trace);
  }

void NpcDayMeasurement::stop(World& w) {
  if(!measurement) return;
  NpcActivityProbe::instance=nullptr;
  if(measurement->stats) {
#ifndef GOTHIC_NATIVE_V3
    if(auto* vm=measurement->observedVm) measurement->stats->stop(*vm);
#endif
    std::fprintf(measurement->output,"{%s,\"end_world_ms\":%lld,\"stats\":%s}\n",measurement->header.c_str(),
                 (long long)w.time().toInt(),measurement->stats->json().c_str());
    }
  measurement.reset();
  }
