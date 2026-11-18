#include "baseimport.h"
#include "npcsnapshot.h"
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
  std::string text() const {
    return "wp="+Recorder::text(waypoint)+" look_at="+std::to_string(lookAt)+" nearest="+Recorder::text(nearest)+
           " distance="+std::to_string(distance)+" available="+std::to_string(available)+" body="+std::to_string(body);
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
    ScriptRun(const NpcSnapshot::Program& program):vm(load(program)) {
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
      vm.observe_member_reads([this](const DaedalusSymbol& symbol,uint16_t,const DaedalusInstance* instance) {
        if(symbol.name()!="C_NPC.WP" || instance!=selfNpc.get()) throw Stop("engine_state_read",symbol.name());
        });
      vm.observe_writes([this](const DaedalusSymbol& symbol) {
        if(symbol.is_member()) throw Stop("engine_state_write",symbol.name());
        const auto* owner=localOwner(symbol,vm);
        if(owner==nullptr) throw Stop("game_state_write",symbol.name());
        const auto frame=std::find_if(frames.rbegin(),frames.rend(),[&](const auto& f) { return f.function==owner; });
        if(frame==frames.rend()) throw Stop("local_state_write",symbol.name());
        frame->assigned.insert(symbol.index());
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
      if(!known.contains(name)) throw Stop("unsupported_external",name);
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
  };

struct Capture {
  MobActivity activity;
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
      return result;
      }

  private:
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

// The human routine states: every state of a captured row of routines.json, by name.
std::vector<std::string> corpus(const fs::path& routines) {
  std::ifstream input(routines,std::ios::binary);
  require(bool(input),"cannot read routine definitions: "+routines.string());
  const std::string text{std::istreambuf_iterator<char>(input),{}};
  rapidjson::Document json;
  json.Parse(text.data(),text.size());
  require(!json.HasParseError() && json.IsObject() && json.HasMember("records") && json["records"].IsArray(),
          "invalid routine definitions: "+routines.string());
  std::set<std::string> states;
  for(const auto& record:json["records"].GetArray())
    if(record.HasMember("rows") && record["rows"].IsArray())
      for(const auto& row:record["rows"].GetArray())
        if(row.HasMember("state") && row["state"].IsString()) states.insert(row["state"].GetString());
  return {states.begin(),states.end()};
  }
Json text(std::string_view value,Allocator& allocator) { return Json(value.data(),rapidjson::SizeType(value.size()),allocator); }
void put(Json& json,const char* name,Json value,Allocator& allocator) { json.AddMember(text(name,allocator),value,allocator); }
std::string bytes(std::string_view value) {
  std::string encoded;
  for(auto byte:value) {
    const auto character=uint8_t(byte);
    if(character<128) encoded+=char(character);
    else { encoded+=char(0xc0|(character>>6)); encoded+=char(0x80|(character&63)); }
    }
  return encoded;
  }
Json recordJson(const std::string& symbol,const Capture& capture,Allocator& allocator) {
  Json json(rapidjson::kObjectType);
  put(json,"symbol",text(symbol,allocator),allocator);
  put(json,"status",text(capture.reason.empty() ? "captured" : "uncaptured",allocator),allocator);
  if(!capture.reason.empty()) {
    put(json,"reason",text(capture.reason,allocator),allocator);
    put(json,"detail",text(bytes(capture.detail),allocator),allocator);
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
  put(parameters,"scheme",text(bytes(a.scheme),allocator),allocator);
  put(parameters,"busy_body_state",Json(a.busyBodyState),allocator);
  put(parameters,"use_state",Json(a.useState),allocator);
  put(parameters,"leave_state",Json(a.leaveState),allocator);
  put(parameters,"loop_result",Json(a.loopResult),allocator);
  put(json,"template",text("mob",allocator),allocator);
  put(json,"parameters",std::move(parameters),allocator);
  put(json,"situations",Json(uint64_t(capture.situations)),allocator);
  return json;
  }
Json envelope(const NpcSnapshot::Program& program,Allocator& allocator) {
  Json json(rapidjson::kObjectType);
  put(json,"format",text("gothic-activity-definitions",allocator),allocator);
  put(json,"version",Json(1),allocator);
  put(json,"program_name",text(program.name,allocator),allocator);
  put(json,"program_sha256",text(program.sha256,allocator),allocator);
  put(json,"string_encoding",text("unicode-byte-mapping",allocator),allocator);
  return json;
  }
void difference(const Json& expected,const Json& actual,const std::string& path,int& errors) {
  if(expected==actual) return;
  if(expected.IsObject() && actual.IsObject()) {
    for(auto it=expected.MemberBegin();it!=expected.MemberEnd();++it) {
      const auto found=actual.FindMember(it->name);
      if(found==actual.MemberEnd()) { ++errors; std::cerr<<path<<'.'<<it->name.GetString()<<": missing field\n"; }
      else difference(it->value,found->value,path+"."+it->name.GetString(),errors);
      }
    for(auto it=actual.MemberBegin();it!=actual.MemberEnd();++it)
      if(!expected.HasMember(it->name)) { ++errors; std::cerr<<path<<'.'<<it->name.GetString()<<": unexpected field\n"; }
    }
  else if(expected.IsArray() && actual.IsArray()) {
    if(expected.Size()!=actual.Size()) { ++errors; std::cerr<<path<<": count differs\n"; }
    for(rapidjson::SizeType i=0;i<std::min(expected.Size(),actual.Size());++i)
      difference(expected[i],actual[i],path+"["+std::to_string(i+1)+"]",errors);
    }
  else { ++errors; std::cerr<<path<<": value differs from fresh VM\n"; }
  }
}

NpcSnapshot::ActivityCounts NpcSnapshot::writeActivities(const Program& program,const fs::path& file,const fs::path& routines) {
  fs::create_directories(file.parent_path());
  rapidjson::Document storage;
  auto& allocator=storage.GetAllocator();
  auto json=envelope(program,allocator);
  Json records(rapidjson::kArrayType);
  ActivityCounts counts;
  for(const auto& state:corpus(routines)) {
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
    difference(it->value,json[it->name],file.string()+"."+it->name.GetString(),errors);
    }
  require(json.HasMember("records") && json["records"].IsArray(),file.string()+": expected records array");
  const auto& records=json["records"];
  const auto states=corpus(routines);
  require(records.Size()==states.size(),file.string()+": activity record count differs");
  for(size_t i=0;i<states.size();++i)
    difference(recordJson(states[i],Recognizer(program,states[i]).capture(),allocator),records[rapidjson::SizeType(i)],
               "activity."+states[i],errors);
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
