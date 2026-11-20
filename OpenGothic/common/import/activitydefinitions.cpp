#include "baseimport.h"
#include "npcsnapshot.h"
#include "definitionjson.h"
#include "asset/activitydefinitions.h"
#include "daedalus/classes.h"
#include "daedalus/vm.h"
#include "programdata.h"

#include <rapidjson/document.h>
#include <rapidjson/prettywriter.h>
#include <rapidjson/stringbuffer.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>

// Step D4.1: recognise the template "activity at a world object" in the three script phases of
// every human routine state. The phases run in a VM whose engine is a stub answering from one
// situation; the template walk answers from the same situation. A state is captured only when
// both call streams and the Loop result are equal in every situation of the template's branches.
namespace fs = std::filesystem;
namespace {
using namespace daedalus;
using namespace ActivityDefinitions;
using Type = DaedalusDataType;
using Json = rapidjson::Value;
using Allocator = rapidjson::Document::AllocatorType;

void require(bool ok,const std::string& why) {
  if(!ok) throw std::runtime_error(why);
  }
struct Stop : std::runtime_error {
  std::string reason,detail;
  Stop(std::string reason,std::string detail):std::runtime_error(reason+": "+detail),reason(std::move(reason)),detail(std::move(detail)) {}
  };

// Answers of the stubbed engine. The VM run and the template walk read the same values.
struct Situation {
  std::string waypoint = "ROUTINE_WP"; // self.wp
  bool        lookAt = false;
  std::string nearest = "OTHER_WP";
  int32_t     distance = std::numeric_limits<int32_t>::max();
  bool        available = true;
  int32_t     body = 0;
  bool        onPoint = false;
  int32_t     stage = 0, seconds = 0, roll = 0;
  std::string text() const {
    return "wp="+Recorder::text(waypoint)+" look_at="+std::to_string(lookAt)+" nearest="+Recorder::text(nearest)+
           " distance="+std::to_string(distance)+" available="+std::to_string(available)+" body="+std::to_string(body)+
           " on_point="+std::to_string(onPoint)+" stage="+std::to_string(stage)+" seconds="+std::to_string(seconds)+" roll="+std::to_string(roll);
    }
  };

class StubHost final : public Host {
  public:
    explicit StubHost(const Situation& s):s(s) {}
    void        deferred(std::string_view) override {}
    const void* lookAtTarget() override { return s.lookAt ? &token : nullptr; }
    bool        isValidNpc(const void* npc) override { return npc!=nullptr; }
    void        stopLookAt() override {}
    void        removeWeapon() override {}
    void        setWalkMode(int32_t) override {}
    std::string waypoint() override { return s.waypoint; }
    std::string nearestWaypoint() override { return s.nearest; }
    bool        stringsEqual(std::string_view a, std::string_view b) override { return a==b; }
    int32_t     distanceToWaypoint(std::string_view) override { return s.distance; }
    void        gotoWaypoint(std::string_view) override {}
    bool        isMobAvailable(std::string_view) override { return s.available; }
    int32_t     bodyState() override { return s.body; }
    void        useMob(std::string_view, int32_t) override {}
  private:
    const Situation& s;
    int              token = 0;
  };

struct Phases {
  const DaedalusSymbol* init = nullptr;
  const DaedalusSymbol* loop = nullptr;
  const DaedalusSymbol* end  = nullptr;
  };
// One observed engine call with its raw arguments, kept for parameter extraction.
struct Event {
  std::string name, text;
  std::vector<std::variant<int32_t,std::string>> args;
  };
struct Run {
  std::vector<Event> events;
  int result = 0;
  std::optional<int32_t> busy; // first C_BODYSTATECONTAINS body state argument
  std::vector<std::string> texts() const {
    std::vector<std::string> out;
    for(const auto& event:events) out.push_back(event.text);
    return out;
    }
  };

const DaedalusSymbol* localOwner(const DaedalusSymbol& symbol,const DaedalusVm& vm) {
  const auto* parent=vm.find_symbol_by_index(symbol.parent());
  if(parent && parent->type()==Type::FUNCTION) return parent;
  const auto dot=symbol.name().find('.');
  const auto* owner=dot==std::string::npos ? nullptr : vm.find_symbol_by_name(symbol.name().substr(0,dot));
  return owner && owner->type()==Type::FUNCTION ? owner : nullptr;
  }

// Runs the phases of one state in a VM with the stub engine. The VM is reused between runs;
// a stop leaves it unusable, and the state then stays on the VM path anyway.
class ScriptRun {
  public:
    ScriptRun(const NpcSnapshot::Program& program,bool points=false):vm(load(program)),points(points) {
      self = vm.global_self();
      require(self!=nullptr && vm.global_other()!=nullptr,"activity definitions: program has no SELF or OTHER");
      selfNpc = vm.allocate_instance<INpc>(self);
      auto previous = vm.global_other()->get_instance();
      lookAtNpc = vm.allocate_instance<INpc>(vm.global_other());
      vm.global_other()->set_instance(previous);
      for(const auto& symbol:vm.symbols())
        if(symbol.type()==Type::FUNCTION && !symbol.is_external() && symbol.name().starts_with("PERCEPTION_SET_") &&
           symbol.count()==0 && !symbol.has_return()) {
          deferred.insert(symbol.index());
          vm.override_function_custom(symbol,[this,name=symbol.name()](DaedalusVm&) {
            current->events.push_back({"deferred","deferred "+name,{}});
            });
          }
      vm.register_exception_handler([](DaedalusVm&,const DaedalusScriptError& error,const DaedalusInstruction&)
                                      -> DaedalusVmExceptionStrategy {
        throw Stop("script_error",error.what());
        });
      vm.observe_calls([this](const DaedalusSymbol& function) {
        // An overridden helper returns without bytecode, so it has no RSR to close a frame.
        if(!deferred.contains(function.index())) frames.push_back({&function,{}});
        if(function.name()=="C_BODYSTATECONTAINS" && !current->busy) {
          const auto& frame=vm.stack_frame(0);
          current->busy = frame.reference ? std::get<DaedalusSymbol*>(frame.value)->get_int(frame.index,frame.context.get())
                                          : std::get<int32_t>(frame.value);
          }
        });
      vm.observe_instructions([this](const DaedalusInstruction& instruction) {
        if(++instructions>100000) throw Stop("execution_limit",phaseName);
        if(instruction.op==DaedalusOpcode::RSR && !frames.empty()) frames.pop_back();
        });
      vm.observe_reads([this](const DaedalusSymbol& symbol) {
        // SELF is the NPC; generated strings are the VM's scratch for pushed values.
        if(&symbol==self || symbol.is_const() || symbol.is_member() || (symbol.is_generated() && symbol.type()==Type::STRING))
          return;
        const auto* owner=localOwner(symbol,vm);
        if(owner==nullptr) throw Stop("game_state",symbol.name());
        const auto frame=std::find_if(frames.rbegin(),frames.rend(),[&](const auto& f) { return f.function==owner; });
        if(frame==frames.rend() || !frame->assigned.contains(symbol.index()))
          throw Stop("local_state",symbol.name()+" before assignment");
        });
      vm.observe_member_reads([this](const DaedalusSymbol& symbol,uint16_t index,const DaedalusInstance* instance) {
        if(this->points && symbol.name()=="C_NPC.AIVAR" && index==19 && instance==selfNpc.get()) {
          current->events.push_back({"stage_read","aivar[19]->"+std::to_string(selfNpc->aivar[19]),{}});
          return;
          }
        if(symbol.name()!="C_NPC.WP" || instance!=selfNpc.get()) throw Stop("engine_state_read",symbol.name());
        });
      vm.observe_writes([this](const DaedalusSymbol& symbol) {
        if(this->points && symbol.is_member()) return; // checked with index and instance below
        if(symbol.is_member()) throw Stop("engine_state_write",symbol.name());
        const auto* owner=localOwner(symbol,vm);
        if(owner==nullptr) throw Stop("game_state_write",symbol.name());
        const auto frame=std::find_if(frames.rbegin(),frames.rend(),[&](const auto& f) { return f.function==owner; });
        if(frame==frames.rend()) throw Stop("local_state_write",symbol.name());
        frame->assigned.insert(symbol.index());
        });
      vm.observe_member_writes([this](const DaedalusSymbol& symbol,uint16_t index,const DaedalusInstance* instance) {
        if(!this->points || symbol.name()!="C_NPC.AIVAR" || index!=19 || instance!=selfNpc.get())
          throw Stop("engine_state_write",symbol.name());
        current->events.push_back({"stage_write","aivar[19]="+std::to_string(selfNpc->aivar[19]),{}});
        });
      vm.register_default_external_custom([this](DaedalusVm&,DaedalusSymbol& function) { external(function); });
      }

    Run run(const DaedalusSymbol& function,const Situation& situation) {
      Run result;
      current = &result;
      this->situation = &situation;
      phaseName = function.name();
      frames.clear();
      instructions = 0;
      selfNpc->wp = situation.waypoint;
      selfNpc->aivar[19] = situation.stage;
      self->set_instance(selfNpc);
      if(function.has_return()) result.result = vm.call_function<int>(&function);
      else vm.call_function<void>(&function);
      current = nullptr;
      return result;
      }
    const DaedalusVm& script() const { return vm; }

  private:
    static DaedalusVm load(const NpcSnapshot::Program& program) {
      auto script=DaedalusScript::from_program(program.data);
      if(script.find_symbol_by_name("C_NPC")) INpc::register_(script);
      if(script.find_symbol_by_name("C_ITEM")) IItem::register_(script);
      return DaedalusVm(std::move(script));
      }
    std::string npcText(const std::shared_ptr<DaedalusInstance>& npc) const {
      if(npc==nullptr) return "null";
      if(npc==selfNpc) return "self";
      if(npc==lookAtNpc) return "npc";
      throw Stop("unknown_instance",phaseName);
      }
    // The engine calls the template knows. Every other external keeps the state in the VM.
    void external(DaedalusSymbol& function) {
      static const std::set<std::string> known={
        "NPC_GETLOOKATTARGET","HLP_ISVALIDNPC","AI_STOPLOOKAT","AI_REMOVEWEAPON","AI_SETWALKMODE","NPC_GETNEARESTWP",
        "HLP_STRCMP","NPC_GETDISTTOWP","AI_GOTOWP","WLD_ISMOBAVAILABLE","NPC_GETBODYSTATE","AI_USEMOB"};
      const auto& name=function.name();
      static const std::set<std::string> pointCalls={"NPC_ISONFP","WLD_ISFPAVAILABLE","AI_GOTOFP","AI_STANDUP",
        "AI_ALIGNTOFP","AI_ALIGNTOWP","AI_PLAYANI","NPC_GETSTATETIME","NPC_SETSTATETIME","HLP_RANDOM"};
      if(!known.contains(name) && !(points && pointCalls.contains(name))) throw Stop("unsupported_external",name);
      const auto parameters=vm.find_parameters_for_function(&function);
      Event event{name,name+"(",{}};
      std::vector<std::string> texts(parameters.size());
      std::vector<std::shared_ptr<DaedalusInstance>> instances(parameters.size());
      for(size_t i=parameters.size();i>0;--i) {
        const auto type=parameters[i-1].type();
        if(type==Type::INSTANCE) { instances[i-1]=vm.pop_instance(); texts[i-1]=npcText(instances[i-1]); }
        else if(type==Type::STRING) { auto value=vm.pop_string(); texts[i-1]=Recorder::text(value); event.args.insert(event.args.begin(),value); }
        else if(type==Type::INT) { auto value=vm.pop_int(); texts[i-1]=std::to_string(value); event.args.insert(event.args.begin(),value); }
        else throw Stop("unsupported_parameter",name);
        }
      for(size_t i=0;i<texts.size();++i) event.text+=(i ? "," : "")+texts[i];
      event.text+=")";
      const auto& s=*situation;
      std::string result;
      if(name=="NPC_GETLOOKATTARGET") {
        vm.push_instance(s.lookAt ? lookAtNpc : nullptr);
        result=s.lookAt ? "npc" : "null";
        }
      else if(name=="HLP_ISVALIDNPC") {
        const bool valid=instances.at(0)!=nullptr;
        vm.push_int(valid); result=std::to_string(valid);
        }
      else if(name=="NPC_GETNEARESTWP") { vm.push_string(s.nearest); result=Recorder::text(s.nearest); }
      else if(name=="HLP_STRCMP") {
        const bool equal=std::get<std::string>(event.args.at(0))==std::get<std::string>(event.args.at(1));
        vm.push_int(equal); result=std::to_string(equal);
        }
      else if(name=="NPC_GETDISTTOWP") { vm.push_int(s.distance); result=std::to_string(s.distance); }
      else if(name=="WLD_ISMOBAVAILABLE") { vm.push_int(s.available); result=std::to_string(s.available); }
      else if(name=="NPC_GETBODYSTATE") { vm.push_int(s.body); result=std::to_string(s.body); }
      else if(name=="NPC_ISONFP") { vm.push_int(s.onPoint); result=std::to_string(s.onPoint); }
      else if(name=="WLD_ISFPAVAILABLE") { vm.push_int(s.available); result=std::to_string(s.available); }
      else if(name=="NPC_GETSTATETIME") { vm.push_int(s.seconds); result=std::to_string(s.seconds); }
      else if(name=="HLP_RANDOM") { vm.push_int(s.roll); result=std::to_string(s.roll); }
      else if(function.has_return()) {
        if(function.rtype()!=Type::INT) throw Stop("unsupported_external",name);
        vm.push_int(0); result="0";
        }
      if(function.has_return()) event.text+="->"+result;
      current->events.push_back(std::move(event));
      }

    struct Frame {
      const DaedalusSymbol* function;
      std::set<uint32_t>    assigned;
      };
    DaedalusVm                vm;
    DaedalusSymbol*           self = nullptr;
    std::shared_ptr<INpc>     selfNpc, lookAtNpc;
    Run*                      current = nullptr;
    const Situation*          situation = nullptr;
    std::string               phaseName;
    std::vector<Frame>        frames;
    size_t                    instructions = 0;
    std::set<uint32_t>        deferred;
    bool                     points = false;
  };

struct Capture {
  MobActivity activity;
  std::optional<PointActivity> point;
  std::string reason, detail;
  size_t situations = 0;
  };

std::vector<std::string> walk(const MobActivity& activity,Phase phase,const Situation& situation,int& result) {
  std::vector<std::string> events;
  StubHost stub(situation);
  Recorder recorder(stub,[&](std::string event) { events.push_back(std::move(event)); });
  result=run(activity,phase,recorder);
  return events;
  }

const Event* firstEvent(const Run& run,std::string_view name) {
  for(const auto& event:run.events)
    if(event.name==name) return &event;
  return nullptr;
  }

class Recognizer {
  public:
    Recognizer(const NpcSnapshot::Program& program,const std::string& state):program(program),state(state) {}

    Capture capture() {
      Capture result;
      try {
        ScriptRun script(program);
        const auto phases=phasesOf(script.script());
        result.activity=extract(script,phases);
        result.situations=compare(script,phases,result.activity);
        }
      catch(const Stop& stop) {
        result.reason=stop.reason;
        result.detail=stop.detail;
        }
      if(!result.reason.empty()) {
        try {
          ScriptRun script(program,true);
          const auto phases=phasesOf(script.script());
          result.point=extractPoint(script,phases);
          result.situations=comparePoint(script,phases,*result.point);
          result.reason.clear(); result.detail.clear();
          }
        catch(const Stop& stop) {
          if(result.point) { result.reason=stop.reason; result.detail=stop.detail; }
          else result.detail+="; point: "+stop.reason+": "+stop.detail;
          result.point.reset();
          }
        }
      return result;
      }

  private:
    PointActivity extractPoint(ScriptRun& script,const Phases& phases) const {
      PointActivity a;
      Situation s;
      const auto init=script.run(*phases.init,s);
      if(init.events.empty() || init.events.front().name!="deferred") throw Stop("no_perception_set",state);
      a.perceptions=init.events.front().text.substr(std::string("deferred ").size());
      s.onPoint=true; s.stage=a.arrivedStage;
      const auto loop=script.run(*phases.loop,s);
      const auto* point=firstEvent(loop,"NPC_ISONFP"),*enter=firstEvent(loop,"AI_PLAYANI");
      const auto end=script.run(*phases.end,s);
      const auto* leave=firstEvent(end,"AI_PLAYANI");
      if(!point || !enter || !leave) throw Stop("no_point_pose",state);
      a.point=std::get<std::string>(point->args.at(0));
      a.enterAnimation=std::get<std::string>(enter->args.at(0));
      a.leaveAnimation=std::get<std::string>(leave->args.at(0));
      // This first form has a five-second timer and a ten-way draw. Other timer/draw
      // forms stay in the VM until their branch contract is defined, regardless of name.
      s.stage=a.poseStage; s.seconds=a.timerSeconds+1;
      for(s.roll=0;s.roll<a.rollRange;++s.roll) {
        const auto gesture=script.run(*phases.loop,s);
        const auto* animation=firstEvent(gesture,"AI_PLAYANI");
        a.gestures.push_back({animation ? std::get<std::string>(animation->args.at(0)) : "",true});
        }
      return a;
      }

    std::vector<std::string> walkPoint(const PointActivity& a,Phase phase,const Situation& s,int& result) const {
      std::vector<std::string> events;
      auto call=[&](std::string text) { events.push_back(std::move(text)); };
      auto stage=s.stage;
      auto read=[&] { call("aivar[19]->"+std::to_string(stage)); return stage; };
      auto write=[&](int32_t value) { stage=value; call("aivar[19]="+std::to_string(value)); };
      auto play=[&](const std::string& animation) { call("AI_PLAYANI(self,"+Recorder::text(animation)+")"); };
      result=0;
      if(phase==Phase::Init) {
        MobActivity approach;
        approach.perceptions=a.perceptions; approach.walkMode=a.walkMode;
        approach.approach=Approach::Distance; approach.approachDistance=a.approachDistance;
        events=walk(approach,phase,s,result);
        write(a.initialStage);
        }
      else if(phase==Phase::End) play(a.leaveAnimation);
      else {
        call("NPC_ISONFP(self,"+Recorder::text(a.point)+")->"+std::to_string(s.onPoint));
        if(s.onPoint) {
          call("AI_ALIGNTOFP(self)");
          if(read()==a.walkingStage) write(a.arrivedStage);
          }
        else {
          call("WLD_ISFPAVAILABLE(self,"+Recorder::text(a.point)+")->"+std::to_string(s.available));
          if(s.available) {
            call("AI_GOTOFP(self,"+Recorder::text(a.point)+")");
            call("AI_STANDUP(self)"); call("AI_ALIGNTOFP(self)"); write(a.walkingStage);
            }
          else {
            call("AI_ALIGNTOWP(self)");
            if(read()==a.walkingStage) write(a.arrivedStage);
            }
          }
        if(read()==a.arrivedStage) { play(a.enterAnimation); write(a.poseStage); }
        const bool posed=read()==a.poseStage;
        call("NPC_GETSTATETIME(self)->"+std::to_string(s.seconds));
        if(posed && s.seconds>a.timerSeconds) {
          call("HLP_RANDOM("+std::to_string(a.rollRange)+")->"+std::to_string(s.roll));
          const auto& gesture=a.gestures.at(size_t(s.roll));
          if(!gesture.animation.empty()) play(gesture.animation);
          if(gesture.resetTimer) call("NPC_SETSTATETIME(self,0)");
          }
        result=a.loopResult;
        }
      return events;
      }

    size_t comparePoint(ScriptRun& script,const Phases& phases,const PointActivity& a) const {
      size_t count=0;
      auto compare=[&](Phase phase,const Situation& s) {
        const auto* fn=phase==Phase::Init ? phases.init : phase==Phase::Loop ? phases.loop : phases.end;
        const auto actual=script.run(*fn,s);
        int result=0;
        const auto expected=walkPoint(a,phase,s,result);
        if(actual.texts()!=expected || actual.result!=result)
          throw Stop("point_template_mismatch",fn->name()+" with "+s.text());
        ++count;
        };
      for(bool lookAt:{false,true})
        for(const char* wp:{"ROUTINE_WP",""})
          for(int32_t distance:{0,499,500,501,std::numeric_limits<int32_t>::max()}) {
            Situation s; s.lookAt=lookAt; s.waypoint=wp; s.distance=distance;
            compare(Phase::Init,s);
            }
      for(int32_t stage:{-1,0,1,2,3,std::numeric_limits<int32_t>::max()}) {
        Situation s; s.stage=stage;
        compare(Phase::End,s);
        for(bool on:{false,true})
          for(bool available:{false,true})
            for(int32_t seconds:{0,4,5,6,std::numeric_limits<int32_t>::max()})
              for(int32_t roll=0;roll<a.rollRange;++roll) {
                s.onPoint=on; s.available=available; s.seconds=seconds; s.roll=roll;
                compare(Phase::Loop,s);
                }
        }
      return count;
      }

    Phases phasesOf(const DaedalusVm& vm) const {
      Phases phases;
      auto phase=[&](const std::string& name,bool result) {
        const auto* symbol=vm.find_symbol_by_name(name);
        if(symbol==nullptr || symbol->type()!=Type::FUNCTION || symbol->is_external())
          throw Stop("missing_phase",name);
        if(symbol->count()!=0 || symbol->has_return()!=result || (result && symbol->rtype()!=Type::INT))
          throw Stop("unsupported_signature",name);
        return symbol;
        };
      phases.init=phase(state,false);
      phases.loop=phase(state+"_LOOP",true);
      phases.end=phase(state+"_END",false);
      return phases;
      }

    // Parameters come from the arguments the script passes in a probe situation. The comparison
    // below decides whether they describe the state; extraction alone proves nothing.
    MobActivity extract(ScriptRun& script,const Phases& phases) const {
      MobActivity activity;
      const Situation probe;
      const auto init=script.run(*phases.init,probe);
      if(init.events.empty() || init.events.front().name!="deferred")
        throw Stop("no_perception_set",state);
      activity.perceptions=init.events.front().text.substr(std::string("deferred ").size());
      const auto* mode=firstEvent(init,"AI_SETWALKMODE");
      if(mode==nullptr || mode->args.size()!=1) throw Stop("no_walk_mode",state);
      activity.walkMode=std::get<int32_t>(mode->args[0]);
      if(firstEvent(init,"NPC_GETNEARESTWP")) activity.approach=Approach::NearestWaypoint;
      else if(firstEvent(init,"NPC_GETDISTTOWP")) {
        activity.approach=Approach::Distance;
        activity.approachDistance=threshold(script,*phases.init);
        }
      else throw Stop("no_approach",state);

      Situation idle=probe;
      const auto loop=script.run(*phases.loop,idle);
      if(!loop.busy) throw Stop("no_busy_body_state",state);
      activity.busyBodyState=*loop.busy;
      idle.body=~activity.busyBodyState;
      const auto use=script.run(*phases.loop,idle);
      const auto* available=firstEvent(use,"WLD_ISMOBAVAILABLE");
      const auto* mob=firstEvent(use,"AI_USEMOB");
      if(available==nullptr || mob==nullptr || mob->args.size()!=2) throw Stop("no_mob_use",state);
      activity.scheme=std::get<std::string>(available->args.at(0));
      activity.useState=std::get<int32_t>(mob->args.at(1));
      activity.loopResult=use.result;
      const auto end=script.run(*phases.end,probe);
      const auto* leave=firstEvent(end,"AI_USEMOB");
      if(leave==nullptr || leave->args.size()!=2) throw Stop("no_mob_leave",state);
      activity.leaveState=std::get<int32_t>(leave->args.at(1));
      return activity;
      }

    // The script walks to its point when farther than a limit: the smallest distance with AI_GOTOWP, minus one.
    int32_t threshold(ScriptRun& script,const DaedalusSymbol& init) const {
      auto walks=[&](int32_t distance) {
        Situation situation;
        situation.distance=distance;
        return firstEvent(script.run(init,situation),"AI_GOTOWP")!=nullptr;
        };
      int64_t low=0,high=std::numeric_limits<int32_t>::max();
      if(walks(0)) return -1;
      if(!walks(int32_t(high))) return int32_t(high);
      while(high-low>1) {
        const auto middle=low+(high-low)/2;
        if(walks(int32_t(middle))) high=middle; else low=middle;
        }
      return int32_t(low);
      }

    // Every branch of the template: look-at target or not, at or away from the point (by name
    // or around the distance limit), object free or not, each body-state bit of the condition.
    size_t compare(ScriptRun& script,const Phases& phases,const MobActivity& activity) const {
      std::vector<std::pair<Phase,Situation>> situations;
      std::vector<int32_t> distances={0,std::numeric_limits<int32_t>::max()};
      for(int64_t value:{int64_t(activity.approachDistance)-1,int64_t(activity.approachDistance),int64_t(activity.approachDistance)+1})
        if(value>=0 && value<=std::numeric_limits<int32_t>::max()) distances.push_back(int32_t(value));
      for(const char* waypoint:{"ROUTINE_WP",""})
        for(bool lookAt:{false,true})
          for(const char* nearest:{"ROUTINE_WP","OTHER_WP",""})
            for(auto distance:distances) {
              Situation s;
              s.waypoint=waypoint; s.lookAt=lookAt; s.nearest=nearest; s.distance=distance;
              situations.emplace_back(Phase::Init,s);
              }
      const auto busy=activity.busyBodyState;
      for(bool available:{false,true})
        for(int32_t body:{0,busy,busy^0x10000,busy^0x8000,busy^0x10,busy^0x1,busy|0x40000,busy|0x40000000,
                          std::numeric_limits<int32_t>::max()}) {
          Situation s;
          s.available=available; s.body=body;
          situations.emplace_back(Phase::Loop,s);
          }
      situations.emplace_back(Phase::End,Situation{});
      for(const auto& [phase,situation]:situations) {
        const auto* function=phase==Phase::Init ? phases.init : phase==Phase::Loop ? phases.loop : phases.end;
        const auto vm=script.run(*function,situation);
        int result=0;
        const auto expected=vm.texts(),actual=walk(activity,phase,situation,result);
        size_t i=0;
        while(i<expected.size() && i<actual.size() && expected[i]==actual[i]) ++i;
        const auto where=function->name()+" with "+situation.text()+": ";
        if(i<expected.size() || i<actual.size())
          throw Stop("template_mismatch",where+"call "+std::to_string(i+1)+" VM "+(i<expected.size() ? expected[i] : "<none>")+
                     ", template "+(i<actual.size() ? actual[i] : "<none>"));
        if(vm.result!=result)
          throw Stop("template_mismatch",where+"result VM "+std::to_string(vm.result)+", template "+std::to_string(result));
        }
      return situations.size();
      }

    const NpcSnapshot::Program& program;
    const std::string&          state;
  };

// The human routine states: every state of a captured row of routines.json, by name. The file
// must belong to the program; a row without a state or an uncaptured routine with rows fails.
std::vector<std::string> corpus(const NpcSnapshot::Program& program,const fs::path& routines) {
  std::ifstream input(routines,std::ios::binary);
  require(bool(input),"cannot read routine definitions: "+routines.string());
  const std::string text{std::istreambuf_iterator<char>(input),{}};
  rapidjson::Document json;
  json.Parse(text.data(),text.size());
  const auto where="routine definitions "+routines.string()+": ";
  require(!json.HasParseError() && json.IsObject() && json.HasMember("records") && json["records"].IsArray(),where+"invalid JSON");
  require(json.HasMember("program_sha256") && json["program_sha256"].IsString() &&
          json["program_sha256"].GetString()==program.sha256,where+"program SHA differs");
  std::set<std::string> states;
  for(const auto& record:json["records"].GetArray()) {
    require(record.IsObject() && record.HasMember("status") && record["status"].IsString() &&
            record.HasMember("rows") && record["rows"].IsArray(),where+"record without status or rows");
    const std::string_view status=record["status"].GetString();
    require(status=="captured" || (status=="uncaptured" && record["rows"].Empty()),where+"unsupported routine record");
    for(const auto& row:record["rows"].GetArray()) {
      require(row.IsObject() && row.HasMember("state") && row["state"].IsString() && row["state"].GetStringLength()>0,
              where+"row without a state");
      states.insert(row["state"].GetString());
      }
    }
  return {states.begin(),states.end()};
  }
using DefinitionJson::text;
using DefinitionJson::put;
using DefinitionJson::encode;
Json recordJson(const std::string& symbol,const Capture& capture,Allocator& allocator) {
  Json json(rapidjson::kObjectType);
  put(json,"symbol",text(symbol,allocator),allocator);
  put(json,"status",text(capture.reason.empty() ? "captured" : "uncaptured",allocator),allocator);
  if(!capture.reason.empty()) {
    put(json,"reason",text(capture.reason,allocator),allocator);
    put(json,"detail",text(encode(capture.detail),allocator),allocator);
    return json;
    }
  if(capture.point) {
    const auto& a=*capture.point;
    Json parameters(rapidjson::kObjectType),stages(rapidjson::kObjectType),gestures(rapidjson::kArrayType);
    put(parameters,"perceptions",text(a.perceptions,allocator),allocator);
    put(parameters,"walk_mode",Json(a.walkMode),allocator);
    put(parameters,"approach",text("distance",allocator),allocator);
    put(parameters,"approach_distance",Json(a.approachDistance),allocator);
    put(parameters,"point",text(encode(a.point),allocator),allocator);
    put(parameters,"occupied",text(a.occupied,allocator),allocator);
    put(parameters,"enter_animation",text(encode(a.enterAnimation),allocator),allocator);
    put(parameters,"leave_animation",text(encode(a.leaveAnimation),allocator),allocator);
    put(parameters,"timer_seconds",Json(a.timerSeconds),allocator);
    put(parameters,"roll_range",Json(a.rollRange),allocator);
    put(parameters,"loop_result",Json(a.loopResult),allocator);
    put(stages,"initial",Json(a.initialStage),allocator);
    put(stages,"walking",Json(a.walkingStage),allocator);
    put(stages,"arrived",Json(a.arrivedStage),allocator);
    put(stages,"pose",Json(a.poseStage),allocator);
    put(parameters,"aivar_19",std::move(stages),allocator);
    for(const auto& gesture:a.gestures) {
      Json entry(rapidjson::kObjectType);
      put(entry,"animation",text(encode(gesture.animation),allocator),allocator);
      put(entry,"reset_timer",Json(gesture.resetTimer),allocator);
      gestures.PushBack(std::move(entry),allocator);
      }
    put(parameters,"gestures",std::move(gestures),allocator);
    put(json,"template",text("point",allocator),allocator);
    put(json,"parameters",std::move(parameters),allocator);
    return json;
    }
  const auto& a=capture.activity;
  Json perceptions(rapidjson::kObjectType),parameters(rapidjson::kObjectType);
  put(perceptions,"function",text(a.perceptions,allocator),allocator);
  put(perceptions,"deferred",Json(true),allocator);
  put(parameters,"perceptions",std::move(perceptions),allocator);
  put(parameters,"walk_mode",Json(a.walkMode),allocator);
  put(parameters,"approach",text(a.approach==Approach::NearestWaypoint ? "nearest_waypoint" : "distance",allocator),allocator);
  put(parameters,"approach_distance",Json(a.approachDistance),allocator);
  put(parameters,"scheme",text(encode(a.scheme),allocator),allocator);
  put(parameters,"busy_body_state",Json(a.busyBodyState),allocator);
  put(parameters,"use_state",Json(a.useState),allocator);
  put(parameters,"leave_state",Json(a.leaveState),allocator);
  put(parameters,"loop_result",Json(a.loopResult),allocator);
  put(json,"template",text("mob",allocator),allocator);
  put(json,"parameters",std::move(parameters),allocator);
  return json;
  }
Json envelope(const NpcSnapshot::Program& program,Allocator& allocator) {
  Json json(rapidjson::kObjectType);
  put(json,"format",text("gothic-activity-definitions",allocator),allocator);
  put(json,"version",Json(2),allocator);
  put(json,"program_name",text(program.name,allocator),allocator);
  put(json,"program_sha256",text(program.sha256,allocator),allocator);
  put(json,"string_encoding",text("unicode-byte-mapping",allocator),allocator);
  return json;
  }
using DefinitionJson::difference;
}

NpcSnapshot::ActivityCounts NpcSnapshot::writeActivities(const Program& program,const fs::path& file,const fs::path& routines) {
  fs::create_directories(file.parent_path());
  rapidjson::Document storage;
  auto& allocator=storage.GetAllocator();
  auto json=envelope(program,allocator);
  Json records(rapidjson::kArrayType);
  ActivityCounts counts;
  for(const auto& state:corpus(program,routines)) {
    const auto capture=Recognizer(program,state).capture();
    ++counts.total;
    if(capture.reason.empty()) { ++counts.captured; counts.situations+=capture.situations; }
    std::cout<<"activity "<<state<<": "<<(capture.reason.empty() ? "captured" : "uncaptured "+capture.reason+": "+capture.detail)<<'\n';
    records.PushBack(recordJson(state,capture,allocator),allocator);
    }
  put(json,"records",std::move(records),allocator);
  rapidjson::StringBuffer buffer;
  rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buffer);
  require(json.Accept(writer),"cannot encode activity definitions: "+file.string());
  require(!fs::is_symlink(file),"activity definitions output is a symlink: "+file.string());
  std::ofstream output(file,std::ios::binary);
  output.write(buffer.GetString(),std::streamsize(buffer.GetSize())); output.put('\n');
  require(bool(output),"cannot write activity definitions: "+file.string());
  return counts;
  }

int NpcSnapshot::verifyActivities(const Program& program,const fs::path& file,const fs::path& routines) {
  std::ifstream input(file,std::ios::binary);
  require(bool(input),"cannot read activity definitions: "+file.string());
  const std::string bytes{std::istreambuf_iterator<char>(input),{}};
  rapidjson::Document json;
  json.Parse<rapidjson::kParseFullPrecisionFlag | rapidjson::kParseValidateEncodingFlag>(bytes.data(),bytes.size());
  require(!json.HasParseError() && json.IsObject(),"invalid activity definitions JSON: "+file.string());
  auto& allocator=json.GetAllocator();
  const auto expected=envelope(program,allocator);
  int errors=0;
  for(auto it=expected.MemberBegin();it!=expected.MemberEnd();++it) {
    require(json.HasMember(it->name),file.string()+": missing "+it->name.GetString());
    difference(it->value,json[it->name],file.string()+"."+it->name.GetString(),errors,true);
    }
  require(json.HasMember("records") && json["records"].IsArray(),file.string()+": expected records array");
  const auto& records=json["records"];
  const auto states=corpus(program,routines);
  require(records.Size()==states.size(),file.string()+": activity record count differs");
  for(size_t i=0;i<states.size();++i)
    difference(recordJson(states[i],Recognizer(program,states[i]).capture(),allocator),records[rapidjson::SizeType(i)],
               "activity."+states[i],errors,true);
  return errors;
  }

int BaseImport::convertActivityDefinitions(const fs::path& base) {
  const auto start=std::chrono::steady_clock::now();
  const auto index=ProgramData::readIndex(base/"programs");
  size_t total=0,captured=0,programs=0,situations=0;
  for(const auto& [name,sha]:index.programs) {
    const auto routines=base/"definitions"/sha/"routines.json";
    if(!fs::exists(routines)) continue;
    NpcSnapshot::Program program;
    program.name=name;
    program.data=ProgramData::readProgram(index.dir/(name+".json"),&program.sha256);
    if(program.sha256!=sha) throw std::runtime_error("activity definitions: program SHA differs: "+name);
    ++programs;
    const auto file=base/"definitions"/sha/"activities.json";
    const auto counts=NpcSnapshot::writeActivities(program,file,routines);
    total+=counts.total; captured+=counts.captured; situations+=counts.situations;
    if(NpcSnapshot::verifyActivities(program,file,routines)!=0)
      throw std::runtime_error("activity definitions: fresh VM comparison failed: "+name);
    }
  const auto ms=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-start).count();
  std::cout<<"written and verified "<<programs<<" activity programs; "<<total<<" states, "<<captured<<" captured, "
           <<(total-captured)<<" uncaptured, "<<situations<<" compared situations; 0 failed ("<<ms<<" ms)\n";
  return 0;
  }
