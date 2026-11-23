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

// Recognise mob and point activities by comparing the three script phases against their
// templates. The VM and the template see the same answers from the engine in each situation.
namespace fs = std::filesystem;
namespace {
using namespace daedalus;
using namespace ActivityDefinitions;
using Type = DaedalusDataType;
using Form = PointActivity::Form;
constexpr std::string_view PortalHelper = "B_EVENT_PORTAL_EARTHQUAKE";
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
  int32_t     portalRoll = 0, guild = 0, ravenDead = 0, world = 0;
  std::string text() const {
    return "wp="+Recorder::text(waypoint)+" look_at="+std::to_string(lookAt)+" nearest="+Recorder::text(nearest)+
           " distance="+std::to_string(distance)+" available="+std::to_string(available)+" body="+std::to_string(body)+
           " on_point="+std::to_string(onPoint)+" stage="+std::to_string(stage)+" seconds="+std::to_string(seconds)+" roll="+std::to_string(roll)+
           " portal_roll="+std::to_string(portalRoll)+" guild="+std::to_string(guild)+" raven_dead="+std::to_string(ravenDead)+" world="+std::to_string(world);
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
      if(points) {
        currentLevel=mutableInt("CURRENTLEVEL");
        ravenIsDead=mutableInt("RAVENISDEAD");
        }
      for(const auto& symbol:vm.symbols())
        if(symbol.type()==Type::FUNCTION && !symbol.is_external() &&
           (symbol.name().starts_with("PERCEPTION_SET_") || (points && symbol.name()==PortalHelper)) &&
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
        // The write observer fires before assignment. Read the value before the next instruction.
        if(drawWrite) {
          const auto& name=drawWrite->name();
          current->events.push_back({"draw_write",name.substr(name.rfind('.')+1)+"="+std::to_string(drawWrite->get_int()),{}});
          drawWrite=nullptr;
          }
        if(instruction.op==DaedalusOpcode::RSR && !frames.empty()) frames.pop_back();
        });
      vm.observe_reads([this](const DaedalusSymbol& symbol) {
        if(&symbol==currentLevel || &symbol==ravenIsDead) {
          current->events.push_back({symbol.name(),symbol.name()+"->"+std::to_string(symbol.get_int()),{}});
          return;
          }
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
        if(this->points && symbol.name()=="C_NPC.GUILD" && instance==selfNpc.get() && index==0) {
          current->events.push_back({"guild","self.guild->"+std::to_string(selfNpc->guild),{}});
          return;
          }
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
        if(&symbol==random || &symbol==eventRandy) drawWrite=&symbol;
        });
      vm.observe_member_writes([this](const DaedalusSymbol& symbol,uint16_t index,const DaedalusInstance* instance) {
        if(!this->points || symbol.name()!="C_NPC.AIVAR" || index!=19 || instance!=selfNpc.get())
          throw Stop("engine_state_write",symbol.name());
        current->events.push_back({"stage_write","aivar[19]="+std::to_string(selfNpc->aivar[19]),{selfNpc->aivar[19]}});
        });
      vm.register_default_external_custom([this](DaedalusVm&,DaedalusSymbol& function) { external(function); });
      }

    void watchRollWrites(const DaedalusSymbol& loop,std::string_view suffix=".RANDOM") {
      random=mutableInt(loop.name()+std::string(suffix));
      eventRandy=mutableInt(loop.name()+".EVENTRANDY");
      }
    void watchMobTimer(const DaedalusSymbol& loop,std::string_view suffix) {
      timedMob=true;
      watchRollWrites(loop,suffix);
      }

    Run run(const DaedalusSymbol& function,const Situation& situation) {
      Run result;
      current = &result;
      this->situation = &situation;
      phaseName = function.name();
      frames.clear();
      instructions = 0;
      draws = 0;
      drawWrite = nullptr;
      selfNpc->wp = situation.waypoint;
      selfNpc->aivar[19] = situation.stage;
      selfNpc->guild = situation.guild;
      if(currentLevel) currentLevel->set_int(situation.world);
      if(ravenIsDead) ravenIsDead->set_int(situation.ravenDead);
      self->set_instance(selfNpc);
      if(function.has_return()) result.result = vm.call_function<int>(&function);
      else vm.call_function<void>(&function);
      current = nullptr;
      return result;
      }
    const DaedalusVm& script() const { return vm; }

  private:
    DaedalusSymbol* mutableInt(std::string_view name) {
      auto* symbol=vm.find_symbol_by_name(name);
      return symbol && symbol->type()==Type::INT && symbol->count()==1 && !symbol->is_member() && !symbol->is_const() ? symbol : nullptr;
      }

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
        "AI_ALIGNTOFP","AI_ALIGNTOWP","AI_PLAYANI"};
      static const std::set<std::string> poseCalls={"AI_PLAYANIBS","NPC_GETSTATETIME","NPC_SETSTATETIME","HLP_RANDOM"};
      if(!known.contains(name) && !(points && pointCalls.contains(name)) &&
         !((points || timedMob) && poseCalls.contains(name))) throw Stop("unsupported_external",name);
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
      else if(name=="HLP_RANDOM") {
        const int32_t draw=draws++==0 ? s.roll : s.portalRoll;
        vm.push_int(draw); result=std::to_string(draw);
        }
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
    DaedalusSymbol*           currentLevel = nullptr;
    DaedalusSymbol*           ravenIsDead = nullptr;
    const DaedalusSymbol*      random = nullptr;
    const DaedalusSymbol*      eventRandy = nullptr;
    const DaedalusSymbol*      drawWrite = nullptr;
    std::shared_ptr<INpc>     selfNpc, lookAtNpc;
    Run*                      current = nullptr;
    const Situation*          situation = nullptr;
    std::string               phaseName;
    std::vector<Frame>        frames;
    size_t                    instructions = 0;
    size_t                    draws = 0;
    std::set<uint32_t>        deferred;
    bool                     points = false;
    bool                     timedMob = false;
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
  if(phase==Phase::Loop && activity.timer) {
    const auto& timer=*activity.timer;
    const auto readTime=[&] { events.push_back("NPC_GETSTATETIME(self)->"+std::to_string(situation.seconds)); };
    // The seat scripts read time before body state; the work scripts read body state first.
    if(activity.skipApproachWhenBusy)
      readTime();
    const bool busy=bodyStateContains(recorder.bodyState(),activity.busyBodyState);
    if(!activity.skipApproachWhenBusy)
      readTime();
    if(situation.seconds>timer.timerSeconds && busy) {
      if(timer.rollRange>0) {
        const auto roll=std::to_string(situation.roll);
        events.push_back("HLP_RANDOM("+std::to_string(timer.rollRange)+")->"+roll);
        events.push_back(timer.rollVariable.substr(1)+"="+roll);
        for(const auto& gesture:timer.gestures)
          if(situation.roll<=gesture.throughRoll)
            events.push_back("AI_PLAYANIBS(self,"+Recorder::text(gesture.animation)+","+std::to_string(activity.busyBodyState)+")");
        }
      events.push_back("NPC_SETSTATETIME(self,0)");
      }
    }
  return events;
  }

const Event* firstEvent(const Run& run,std::string_view name,size_t skip=0) {
  for(const auto& event:run.events)
    if(event.name==name) {
      if(skip==0) return &event;
      --skip;
      }
  return nullptr;
  }

void compareTrace(const Run& vm,const std::vector<std::string>& expected,int result,
                  const char* reason,const DaedalusSymbol& function,const Situation& s) {
  const auto actual=vm.texts();
  auto [left,right]=std::mismatch(actual.begin(),actual.end(),expected.begin(),expected.end());
  if(left!=actual.end() || right!=expected.end())
    throw Stop(reason,function.name()+" with "+s.text()+": call "+std::to_string(left-actual.begin()+1)+
               " VM "+(left==actual.end() ? "<none>" : *left)+", template "+(right==expected.end() ? "<none>" : *right));
  if(vm.result!=result)
    throw Stop(reason,function.name()+" with "+s.text()+": result VM "+std::to_string(vm.result)+", template "+std::to_string(result));
  }

class Recognizer {
  public:
    Recognizer(const NpcSnapshot::Program& program,const std::string& state):program(program),state(state) {}

    Capture capture() {
      Capture result;
      try {
        ScriptRun script(program);
        const auto phases=phasesOf(script.script());
        if(timedMobState(state))
          script.watchMobTimer(*phases.loop,mobRollVariable());
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
      if(init.events.empty() || !init.events.front().text.starts_with("deferred PERCEPTION_SET_"))
        throw Stop("no_perception_set",state);
      a.perceptions=init.events.front().text.substr(std::string("deferred ").size());
      const auto* initial=firstEvent(init,"stage_write");
      s.onPoint=true; s.stage=initial ? std::get<int32_t>(initial->args.at(0)) : 0;
      const auto loop=script.run(*phases.loop,s);
      const auto end=script.run(*phases.end,s);
      const auto* point=firstEvent(loop,"NPC_ISONFP");
      const bool sit=firstEvent(loop,"AI_PLAYANIBS")!=nullptr;
      const auto* enter=firstEvent(loop,sit ? "AI_PLAYANIBS" : "AI_PLAYANI");
      const auto* leave=firstEvent(end,sit ? "AI_PLAYANIBS" : "AI_PLAYANI");
      if(!enter || !leave) throw Stop("no_point_pose",state);
      a.enterAnimation=std::get<std::string>(enter->args.at(0));
      a.leaveAnimation=std::get<std::string>(leave->args.at(0));
      if(point) a.point=std::get<std::string>(point->args.at(0));
      if(sit) {
        a.form=Form::Campfire;
        a.enterBodyState=std::get<int32_t>(enter->args.at(1));
        a.leaveBodyState=std::get<int32_t>(leave->args.at(1));
        }
      else if(!point) a.form=Form::Waypoint;
      else if(firstEvent(loop,"NPC_ISONFP",1)) {
        // Washing has the same queue shape but targets a different kind of free point.
        if(a.point!="PEE") throw Stop("point_template_mismatch",phases.loop->name()+" uses FP "+a.point);
        a.form=Form::Pee;
        }
      else if(firstEvent(loop,"AI_STANDUP")) a.form=Form::ArmsCrossed;

      if(a.form!=Form::Guard) {
        const auto* mode=firstEvent(init,"AI_SETWALKMODE");
        if(!mode) throw Stop("no_walk_mode",state);
        a.walkMode=std::get<int32_t>(mode->args.at(0));
        a.approachDistance=a.form==Form::Waypoint ? 0 : threshold(script,*phases.init);
        if(!sit) {
          const auto* pose=firstEvent(loop,"stage_write");
          if(!initial || !pose) throw Stop("no_point_pose",state);
          a.initialStage=s.stage;
          a.arrivedStage=a.initialStage;
          a.poseStage=std::get<int32_t>(pose->args.at(0));
          if(a.form!=Form::Waypoint) {
            Situation free; free.stage=a.initialStage;
            const auto walking=script.run(*phases.loop,free);
            const auto* stage=firstEvent(walking,"stage_write");
            if(!stage) throw Stop("no_point_pose",state);
            a.walkingStage=std::get<int32_t>(stage->args.at(0));
            }
          }
        }
      if(a.form!=Form::Waypoint && a.point.empty()) throw Stop("no_point_pose",state);
      if(!a.timed()) {
        a.rollRange=0; a.timerSeconds=0;
        return a;
        }
      s.stage=a.poseStage;
      if(a.form!=Form::Guard) {
        a.timerSeconds=lastBefore([&](int32_t seconds) {
          s.seconds=seconds;
          return firstEvent(script.run(*phases.loop,s),"HLP_RANDOM")!=nullptr;
          });
        if(a.timerSeconds<0 || a.timerSeconds==std::numeric_limits<int32_t>::max())
          throw Stop("point_template_mismatch",phases.loop->name()+" gesture timer");
        }
      s.seconds=a.timerSeconds+1;
      const auto timed=script.run(*phases.loop,s);
      const auto* draw=firstEvent(timed,"HLP_RANDOM");
      if(!draw || std::get<int32_t>(draw->args.at(0))!=(a.form==Form::ArmsCrossed ? 7 : 10))
        throw Stop("point_template_mismatch",phases.loop->name()+" gesture draw");
      a.rollRange=std::get<int32_t>(draw->args.at(0));
      for(s.roll=0;s.roll<a.rollRange;++s.roll) {
        const auto gesture=script.run(*phases.loop,s);
        const auto* animation=firstEvent(gesture,"AI_PLAYANI");
        a.gestures.push_back({animation ? std::get<std::string>(animation->args.at(0)) : "",true});
        }
      if(a.form==Form::ArmsCrossed) {
        const auto* second=firstEvent(timed,"HLP_RANDOM",1);
        // Classic has neither the portal helper nor Raven's quest flag. In the addon a missing
        // second draw is a mismatch even when the rest of the loop resembles Classic.
        if(!second && !script.script().find_symbol_by_name(PortalHelper) && !script.script().find_symbol_by_name("RAVENISDEAD"))
          return a;
        if(!second || std::get<int32_t>(second->args.at(0))!=200)
          throw Stop("point_template_mismatch",phases.loop->name()+" portal draw");
        auto constant=[&](std::string_view name) {
          const auto* symbol=script.script().find_symbol_by_name(name);
          if(!symbol || symbol->type()!=Type::INT || !symbol->is_const() || symbol->count()!=1 || symbol->is_member())
            throw Stop("game_state",std::string(name));
          return symbol->get_int();
          };
        auto& portal=a.portal.emplace();
        portal.world=constant("ADDONWORLD_ZEN");
        portal.excludedGuild=constant("GIL_PIR");
        portal.ravenDead=constant("FALSE");
        portal.rollRange=std::get<int32_t>(second->args.at(0));
        s.world=portal.world; s.guild=portal.excludedGuild^1; s.ravenDead=portal.ravenDead; s.roll=0;
        for(s.portalRoll=0;s.portalRoll<portal.rollRange;++s.portalRoll) {
          const auto probe=script.run(*phases.loop,s);
          const auto* helper=firstEvent(probe,"deferred");
          if(!helper) continue;
          if(helper->text!="deferred "+std::string(PortalHelper) || !portal.function.empty())
            throw Stop("point_template_mismatch",phases.loop->name()+" portal helper with "+s.text());
          portal.function=PortalHelper; portal.triggerRoll=s.portalRoll;
          }
        if(portal.function.empty()) throw Stop("point_template_mismatch",phases.loop->name()+" missing portal call");
        }
      return a;
      }

    std::vector<std::string> walkPoint(const PointActivity& a,Phase phase,const Situation& s) const {
      std::vector<std::string> events;
      auto call=[&](std::string text) { events.push_back(std::move(text)); };
      StubHost stub(s);
      Recorder host(stub,call);
      auto stage=s.stage;
      auto read=[&] { call("aivar[19]->"+std::to_string(stage)); return stage; };
      auto write=[&](int32_t value) { stage=value; call("aivar[19]="+std::to_string(value)); };
      auto play=[&](const std::string& animation) { call("AI_PLAYANI(self,"+Recorder::text(animation)+")"); };
      auto playBody=[&](const std::string& animation,int32_t body) {
        call("AI_PLAYANIBS(self,"+Recorder::text(animation)+","+std::to_string(body)+")");
        };
      auto sitting=[&] { return bodyStateContains(host.bodyState(),a.enterBodyState); };
      auto onPoint=[&] {
        call("NPC_ISONFP(self,"+Recorder::text(a.point)+")->"+std::to_string(s.onPoint));
        return s.onPoint;
        };
      auto available=[&] {
        call("WLD_ISFPAVAILABLE(self,"+Recorder::text(a.point)+")->"+std::to_string(s.available));
        return s.available;
        };
      auto gotoPoint=[&] {
        call("AI_GOTOFP(self,"+Recorder::text(a.point)+")");
        call("AI_STANDUP(self)"); call("AI_ALIGNTOFP(self)");
        };
      if(phase==Phase::Init) {
        resetForActivity(host,a.perceptions);
        if(a.form==Form::Campfire && sitting()) return events;
        host.setWalkMode(a.walkMode);
        if(a.form==Form::Waypoint) {
          host.gotoWaypoint(s.waypoint);
          call("AI_ALIGNTOWP(self)");
          }
        else if(host.distanceToWaypoint(s.waypoint)>a.approachDistance) host.gotoWaypoint(s.waypoint);
        if(a.form!=Form::Campfire) write(a.initialStage);
        return events;
        }
      if(phase==Phase::End) {
        if(a.form==Form::Campfire) playBody(a.leaveAnimation,a.leaveBodyState);
        else play(a.leaveAnimation);
        return events;
        }
      if(a.form==Form::Campfire) {
        // Daedalus evaluates both operands, including the second body query in the else-if.
        const bool seated=sitting();
        if(onPoint() && !seated) {
          call("AI_ALIGNTOFP(self)");
          playBody(a.enterAnimation,a.enterBodyState);
          }
        else {
          const bool stillSeated=sitting();
          if(available() && !stillSeated) gotoPoint();
          }
        return events;
        }
      if(a.form!=Form::Waypoint) {
        if(onPoint()) {
          call("AI_ALIGNTOFP(self)");
          if(read()==a.walkingStage) write(a.arrivedStage);
          }
        else if(available()) { gotoPoint(); write(a.walkingStage); }
        else {
          call("AI_ALIGNTOWP(self)");
          if(read()==a.walkingStage) write(a.arrivedStage);
          }
        }
      const bool canEnter=a.form!=Form::Pee || onPoint();
      if(read()==(a.form==Form::Waypoint ? a.initialStage : a.arrivedStage) && canEnter) {
        if(a.form==Form::Pee) call("AI_ALIGNTOFP(self)");
        if(a.form==Form::Pee || a.form==Form::ArmsCrossed) call("AI_STANDUP(self)");
        play(a.enterAnimation); write(a.poseStage);
        }
      if(!a.timed()) return events;
      const bool posed=read()==a.poseStage;
      call("NPC_GETSTATETIME(self)->"+std::to_string(s.seconds));
      if(!posed || s.seconds<=a.timerSeconds) return events;
      call("HLP_RANDOM("+std::to_string(a.rollRange)+")->"+std::to_string(s.roll));
      if(a.form==Form::ArmsCrossed || a.form==Form::Waypoint) call("RANDOM="+std::to_string(s.roll));
      const auto& gesture=a.gestures.at(size_t(s.roll));
      if(!gesture.animation.empty()) play(gesture.animation);
      if(a.portal) {
        const auto& p=*a.portal;
        call("HLP_RANDOM("+std::to_string(p.rollRange)+")->"+std::to_string(s.portalRoll));
        call("EVENTRANDY="+std::to_string(s.portalRoll));
        call("self.guild->"+std::to_string(s.guild));
        call("RAVENISDEAD->"+std::to_string(s.ravenDead));
        call("CURRENTLEVEL->"+std::to_string(s.world));
        if(s.guild!=p.excludedGuild && s.ravenDead==p.ravenDead && s.world==p.world && s.portalRoll==p.triggerRoll)
          call("deferred "+p.function);
        }
      if(gesture.resetTimer) call("NPC_SETSTATETIME(self,0)");
      return events;
      }

    size_t comparePoint(ScriptRun& script,const Phases& phases,const PointActivity& a) const {
      if(a.form==Form::ArmsCrossed || a.form==Form::Waypoint) script.watchRollWrites(*phases.loop);
      size_t count=0;
      auto compare=[&](Phase phase,const Situation& s) {
        const auto* fn=phase==Phase::Init ? phases.init : phase==Phase::Loop ? phases.loop : phases.end;
        const auto actual=script.run(*fn,s);
        const auto expected=walkPoint(a,phase,s);
        compareTrace(actual,expected,phase==Phase::Loop ? a.loopResult : 0,"point_template_mismatch",*fn,s);
        ++count;
        };
      std::vector<int32_t> bodies={0};
      if(a.form==Form::Campfire) {
        const int32_t sit=a.enterBodyState;
        bodies={0,sit,sit^1,sit^0x8000,sit^0x10000,sit|0x40000};
        }
      const int32_t max=std::numeric_limits<int32_t>::max();
      std::set<int32_t> distances={0,max,a.approachDistance};
      if(a.approachDistance>0) distances.insert(a.approachDistance-1);
      if(a.approachDistance<max) distances.insert(a.approachDistance+1);
      for(bool lookAt:{false,true})
        for(const char* wp:{"ROUTINE_WP",""})
          for(int32_t distance:distances)
            for(auto body:bodies) {
              Situation s; s.lookAt=lookAt; s.waypoint=wp; s.distance=distance; s.body=body;
              compare(Phase::Init,s);
              if(a.form!=Form::Guard) {
                s.stage=a.initialStage; compare(Phase::Init,s);
                s.stage=a.walkingStage; compare(Phase::Init,s);
                }
              }
      const std::set<int32_t> stages={-1,0,1,2,3,max,a.initialStage,a.walkingStage,a.arrivedStage,a.poseStage};
      const std::set<int32_t> timers={0,a.timerSeconds-1,a.timerSeconds,a.timerSeconds+1,max};
      for(int32_t stage:stages) {
        Situation s; s.stage=stage;
        for(auto body:bodies) { s.body=body; compare(Phase::End,s); }
        for(bool on:{false,true})
          for(bool free:{false,true}) {
            s.onPoint=on; s.available=free;
            if(!a.timed()) {
              for(auto body:bodies) { s.body=body; compare(Phase::Loop,s); }
              continue;
              }
            if(a.portal) {
              s.world=a.portal->world; s.ravenDead=a.portal->ravenDead; s.guild=a.portal->excludedGuild^1;
              s.portalRoll=a.portal->triggerRoll;
              }
            for(int32_t seconds:timers)
              for(s.roll=0;s.roll<a.rollRange;++s.roll) {
                s.seconds=seconds;
                compare(Phase::Loop,s);
                }
            }
        }
      if(a.portal) {
        Situation s; s.stage=a.poseStage; s.seconds=a.timerSeconds+1; s.onPoint=true;
        const auto& p=*a.portal;
        s.world=p.world; s.guild=p.excludedGuild^1; s.ravenDead=p.ravenDead;
        for(s.roll=0;s.roll<a.rollRange;++s.roll)
          for(s.portalRoll=0;s.portalRoll<p.rollRange;++s.portalRoll) compare(Phase::Loop,s);
        s.portalRoll=p.triggerRoll;
        for(s.roll=0;s.roll<a.rollRange;++s.roll)
          for(auto guild:{p.excludedGuild,0,std::numeric_limits<int32_t>::max()})
            for(auto dead:{p.ravenDead,p.ravenDead^1})
              for(auto world:{p.world,0,1,2,3,max}) { // Newworld, Oldworld and Dragonisland are 1, 2 and 3 in the DAT.
                s.guild=guild; s.ravenDead=dead; s.world=world; compare(Phase::Loop,s);
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
      activity.skipApproachWhenBusy=mobInitChecksBody(state);
      if(timedMobState(state))
        activity.timer=extractMobTimer(script,*phases.loop,activity.busyBodyState);
      return activity;
      }

    std::string_view mobRollVariable() const {
      return state=="ZS_REPAIR_HUT" ? ".ZUFALL" : ".RANDOM";
      }

    MobActivity::Timer extractMobTimer(ScriptRun& script,const DaedalusSymbol& loop,int32_t body) const {
      MobActivity::Timer timer;
      Situation s;
      s.body=body;
      timer.timerSeconds=lastBefore([&](int32_t seconds) {
        s.seconds=seconds;
        return firstEvent(script.run(loop,s),"NPC_SETSTATETIME")!=nullptr;
        });
      if(timer.timerSeconds<0 || timer.timerSeconds==std::numeric_limits<int32_t>::max())
        throw Stop("template_mismatch",loop.name()+" missing mob timer");
      s.seconds=timer.timerSeconds+1;
      const auto timed=script.run(loop,s);
      const auto* draw=firstEvent(timed,"HLP_RANDOM");
      if(!draw)
        return timer;
      timer.rollVariable=mobRollVariable();
      timer.rollRange=std::get<int32_t>(draw->args.at(0));
      if(timer.rollRange<1 || timer.rollRange>100)
        throw Stop("template_mismatch",loop.name()+" mob timer roll range");
      for(const auto& event:timed.events)
        if(event.name=="AI_PLAYANIBS")
          timer.gestures.push_back({std::get<std::string>(event.args.at(0)),0});
      // All outcomes in these states play a suffix of the queue observed at draw zero.
      // The phase comparison checks each resulting animation and the order of all calls.
      for(s.roll=1;s.roll<timer.rollRange;++s.roll) {
        const auto rolled=script.run(loop,s);
        const auto count=size_t(std::count_if(rolled.events.begin(),rolled.events.end(),
                                [](const auto& e) { return e.name=="AI_PLAYANIBS"; }));
        if(count>timer.gestures.size())
          throw Stop("template_mismatch",loop.name()+" mob timer gesture queue");
        for(size_t i=timer.gestures.size()-count;i<timer.gestures.size();++i)
          timer.gestures[i].throughRoll=s.roll;
        }
      return timer;
      }

    // Last non-negative distance/time before the script issues a command.
    int32_t lastBefore(const auto& issuesCommand) const {
      int64_t low=0,high=std::numeric_limits<int32_t>::max();
      if(issuesCommand(0)) return -1;
      if(!issuesCommand(int32_t(high))) return int32_t(high);
      while(high-low>1) {
        const auto middle=low+(high-low)/2;
        if(issuesCommand(int32_t(middle))) high=middle; else low=middle;
        }
      return int32_t(low);
      }

    int32_t threshold(ScriptRun& script,const DaedalusSymbol& init) const {
      return lastBefore([&](int32_t distance) {
        Situation s; s.distance=distance;
        return firstEvent(script.run(init,s),"AI_GOTOWP")!=nullptr;
        });
      }

    // Every branch of the template: look-at target or not, at or away from the point (by name
    // or around the distance limit), object free or not, each body-state bit of the condition.
    size_t compare(ScriptRun& script,const Phases& phases,const MobActivity& activity) const {
      std::vector<std::pair<Phase,Situation>> situations;
      const auto busy=activity.busyBodyState;
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
              if(activity.skipApproachWhenBusy) {
                s.body=busy;
                situations.emplace_back(Phase::Init,s);
                }
              }
      for(bool available:{false,true})
        for(int32_t body:{0,busy,busy^0x10000,busy^0x8000,busy^0x10,busy^0x1,busy|0x40000,busy|0x40000000,
                          std::numeric_limits<int32_t>::max()}) {
          Situation s;
          s.available=available; s.body=body;
          situations.emplace_back(Phase::Loop,s);
          if(activity.timer) {
            const auto& timer=*activity.timer;
            for(auto seconds:{timer.timerSeconds,timer.timerSeconds+1,std::numeric_limits<int32_t>::max()}) {
              s.seconds=seconds;
              const auto rolls=seconds==timer.timerSeconds+1 && bodyStateContains(body,busy) ? std::max(1,timer.rollRange) : 1;
              for(s.roll=0;s.roll<rolls;++s.roll)
                situations.emplace_back(Phase::Loop,s);
              }
            }
          }
      situations.emplace_back(Phase::End,Situation{});
      for(const auto& [phase,situation]:situations) {
        const auto* function=phase==Phase::Init ? phases.init : phase==Phase::Loop ? phases.loop : phases.end;
        const auto vm=script.run(*function,situation);
        int result=0;
        const auto expected=walk(activity,phase,situation,result);
        compareTrace(vm,expected,result,"template_mismatch",*function,situation);
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
    Json parameters(rapidjson::kObjectType);
    put(parameters,"form",text(PointActivity::FormNames[size_t(a.form)],allocator),allocator);
    put(parameters,"perceptions",text(a.perceptions,allocator),allocator);
    put(parameters,"walk_mode",Json(a.walkMode),allocator);
    const bool waypoint=a.form==Form::Waypoint;
    put(parameters,"approach",text(waypoint ? "goto_align_waypoint" : "distance",allocator),allocator);
    if(!waypoint) {
      put(parameters,"approach_distance",Json(a.approachDistance),allocator);
      put(parameters,"point",text(encode(a.point),allocator),allocator);
      put(parameters,"occupied",text(a.occupied(),allocator),allocator);
      }
    put(parameters,"enter_animation",text(encode(a.enterAnimation),allocator),allocator);
    put(parameters,"leave_animation",text(encode(a.leaveAnimation),allocator),allocator);
    put(parameters,"loop_result",Json(a.loopResult),allocator);
    if(a.form==Form::Campfire) {
      put(parameters,"enter_body_state",Json(a.enterBodyState),allocator);
      put(parameters,"leave_body_state",Json(a.leaveBodyState),allocator);
      }
    else {
      Json stages(rapidjson::kObjectType);
      put(stages,"initial",Json(a.initialStage),allocator);
      if(!waypoint) {
        put(stages,"walking",Json(a.walkingStage),allocator);
        put(stages,"arrived",Json(a.arrivedStage),allocator);
        }
      put(stages,"pose",Json(a.poseStage),allocator);
      put(parameters,"aivar_19",std::move(stages),allocator);
      }
    if(a.timed()) {
      put(parameters,"timer_seconds",Json(a.timerSeconds),allocator);
      put(parameters,"roll_range",Json(a.rollRange),allocator);
      Json gestures(rapidjson::kArrayType);
      for(const auto& gesture:a.gestures) {
        Json entry(rapidjson::kObjectType);
        put(entry,"animation",text(encode(gesture.animation),allocator),allocator);
        put(entry,"reset_timer",Json(gesture.resetTimer),allocator);
        gestures.PushBack(std::move(entry),allocator);
        }
      put(parameters,"gestures",std::move(gestures),allocator);
      }
    if(a.form==Form::ArmsCrossed) {
      Json event;
      if(a.portal) {
        event.SetObject();
        put(event,"function",text(a.portal->function,allocator),allocator);
        put(event,"roll_range",Json(a.portal->rollRange),allocator);
        put(event,"trigger_roll",Json(a.portal->triggerRoll),allocator);
        put(event,"world",Json(a.portal->world),allocator);
        put(event,"raven_dead",Json(a.portal->ravenDead),allocator);
        put(event,"excluded_guild",Json(a.portal->excludedGuild),allocator);
        }
      put(parameters,"portal_event",std::move(event),allocator);
      }
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
  if(a.skipApproachWhenBusy)
    put(parameters,"skip_approach_when_busy",Json(true),allocator);
  if(a.timer) {
    Json timer(rapidjson::kObjectType),gestures(rapidjson::kArrayType);
    put(timer,"timer_seconds",Json(a.timer->timerSeconds),allocator);
    put(timer,"roll_range",Json(a.timer->rollRange),allocator);
    put(timer,"roll_variable",text(a.timer->rollVariable,allocator),allocator);
    for(const auto& entry:a.timer->gestures) {
      Json gesture(rapidjson::kObjectType);
      put(gesture,"animation",text(encode(entry.animation),allocator),allocator);
      put(gesture,"through_roll",Json(entry.throughRoll),allocator);
      gestures.PushBack(std::move(gesture),allocator);
      }
    put(timer,"gestures",std::move(gestures),allocator);
    put(parameters,"timer",std::move(timer),allocator);
    }
  put(json,"template",text("mob",allocator),allocator);
  put(json,"parameters",std::move(parameters),allocator);
  return json;
  }
Json envelope(const NpcSnapshot::Program& program,Allocator& allocator) {
  Json json(rapidjson::kObjectType);
  put(json,"format",text("gothic-activity-definitions",allocator),allocator);
  put(json,"version",Json(FormatVersion),allocator);
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
