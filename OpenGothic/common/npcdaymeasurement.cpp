#include "npcdaymeasurement.h"

#include "game/gamescript.h"
#include "game/gamescript_daystats.h"
#include "game/definitions/baseprogram.h"
#include "game/definitions/basescripts.h"
#include "gothic.h"
#include "programdata.h"
#include "settings.h"
#include "utils/jsonescape.h"
#include "world/npcactivityprobe.h"
#include "world/objects/interactive.h"
#include "world/objects/item.h"
#include "world/objects/npc.h"
#include "world/waypoint.h"
#include "world/world.h"

#include <cstdio>
#include <memory>
#include <stdexcept>

namespace {
class Measurement final : public NpcActivityProbe {
  public:
    World& world;
    std::FILE* output=nullptr;
    std::FILE* trace=nullptr;
    std::shared_ptr<DayScriptStats> stats;
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
      stats->begin(name);
      if(npc) body(*npc);
      const auto [state,phase]=DayScriptStats::statePhase(name);
      emit(npc,"phase_begin",",\"state\":"+jsonEscape(state)+",\"phase\":"+jsonEscape(phase));
      }
    void phaseEnd(int result) override {
      const auto* npc=phases.back().npc;
      if(npc) body(*npc);
      emit(npc,"phase_end",",\"result\":"+std::to_string(result));
      stats->end();
      phases.pop_back();
      }
    void phaseRecord() override {
      stats->record();
      }
    void random(const Npc* npc, uint32_t bound, uint32_t result, bool script) override {
      const auto source=stats->source(script,npc!=nullptr);
      stats->random(script,npc!=nullptr);
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
  };
std::unique_ptr<Measurement> measurement;
}

bool NpcDayMeasurement::start(World& w, const std::string& directory, uint32_t seed, bool isolate,
                              const std::vector<std::string>& selected, std::string& error) {
  try {
    auto m=std::make_unique<Measurement>(w);
    m->output=std::fopen((directory+"/npc-day-script.json").c_str(),"w");
    if(!selected.empty()) m->trace=std::fopen((directory+"/npc-day-activity.jsonl").c_str(),"w");
    if(!m->output || (!selected.empty() && !m->trace)) throw std::runtime_error("cannot write day measurements in "+directory);
    // Read the same verified program selection as GameScript, regardless of the routine playback switch.
    std::string sha;
    loadBaseProgram(Gothic::inst().defaultGameDatFile(),Settings::getI("GAME","language"),&sha);
    const auto routines=RoutineDefinitions::read(baseProgramIndex().dir.parent_path(),sha);
    m->selected=selected;
    m->header="\"seed\":"+std::to_string(seed)+",\"stream\":"+jsonEscape(isolate ? "npc" : "shared")+
              ",\"start_world_ms\":"+std::to_string(w.time().toInt());
    m->stats=DayScriptStats::attach(w.script().getVm(),routines);
    if(m->trace) std::fprintf(m->trace,"{\"kind\":\"header\",%s}\n",m->header.c_str());
    measurement=std::move(m);
    NpcActivityProbe::instance=measurement.get();
    tick(w);
    return true;
    }
  catch(const std::exception& e) { error=e.what(); return false; }
  }

void NpcDayMeasurement::tick(World& w) {
  if(!measurement) return;
  for(uint32_t i=0;i<w.npcCount();++i) measurement->body(*w.npcById(i));
  if(measurement->trace) std::fflush(measurement->trace);
  }

void NpcDayMeasurement::stop(World& w) {
  if(!measurement) return;
  NpcActivityProbe::instance=nullptr;
  measurement->stats->stop(w.script().getVm());
  std::fprintf(measurement->output,"{%s,\"end_world_ms\":%lld,\"stats\":%s}\n",measurement->header.c_str(),
               (long long)w.time().toInt(),measurement->stats->json().c_str());
  measurement.reset();
  }
