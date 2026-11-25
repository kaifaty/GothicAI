#include "game/gamescript_daystats.h"
#include "daedalus/vm.h"
#include "programdata.h"
#include "storygraph/bytecode.h"
#include "storygraph/exec.h"
#include "world/npcactivityprobe.h"
#include <rapidjson/document.h>
#include <iostream>
#include <stdexcept>

namespace {
using Type=daedalus::DaedalusDataType;
using Op=daedalus::DaedalusOpcode;
unsigned assertions=0;
void check(bool value, const char* message) {
  ++assertions;
  if(!value) throw std::runtime_error(message);
  }
struct Sink final : NpcActivityProbe {
  std::vector<AiQueue::AiAction> commands;
  void phaseBegin(const Npc*,std::string_view) override {}
  void phaseEnd(int) override {}
  void random(const Npc*,uint32_t,uint32_t,bool) override {}
  void queued(const Npc*,const AiQueue::AiAction& a) override {
    commands.push_back(a);
    }
  void point(const Npc&,const WayPoint*,const WayPoint*) override {}
  void mob(const Npc&,const Interactive&,bool) override {}
  };
daedalus::DaedalusVm makeVm(bool legacy = true) {
  asset::ProgramRecord program;
  program.version=50;
  program.bytecode.push_back(std::byte(Op::RSR));
  auto function=[&](const char* name,bool external=false,Type result=Type::VOID,uint32_t args=0) {
    asset::ProgramSymbol symbol;
    symbol.name=name;
    symbol.type=uint32_t(Type::FUNCTION);
    symbol.flags=daedalus::DaedalusSymbolFlag::CONST|(external ? daedalus::DaedalusSymbolFlag::EXTERNAL : 0);
    if(result!=Type::VOID)
      symbol.flags|=daedalus::DaedalusSymbolFlag::RETURN;
    symbol.return_type=uint32_t(result);
    symbol.count=args;
    program.symbols.push_back(std::move(symbol));
    return uint32_t(program.symbols.size()-1);
    };
  auto op=[&](Op code) { program.bytecode.push_back(std::byte(code)); };
  auto call=[&](Op code,uint32_t argument) {
    op(code);
    for(unsigned i=0;i<4;++i) program.bytecode.push_back(std::byte((argument>>(8*i))&255));
    };
  auto begin=[&](uint32_t id) { program.symbols[id].address=int32_t(program.bytecode.size()); };
  const auto random=function("HLP_RANDOM",true);
  const auto helper=function("HELPER");begin(helper);call(Op::BE,random);op(Op::RSR);
  const auto a=function("ZS_A_LOOP");begin(a);call(Op::BL,uint32_t(program.symbols[helper].address));op(Op::RSR);
  const auto b=function("ZS_B_LOOP");begin(b);call(Op::BL,uint32_t(program.symbols[helper].address));op(Op::RSR);
  const auto outside=function("OUTSIDE");begin(outside);call(Op::BL,uint32_t(program.symbols[helper].address));op(Op::RSR);

  const auto remember=function("REMEMBER",false,Type::INT,2);
  const auto state=uint32_t(program.symbols.size());
  program.symbols.push_back({.name="REMEMBER.NUMBER",.type=uint32_t(Type::INT),.count=1,.int_values={0}});
  const auto label=uint32_t(program.symbols.size());
  program.symbols.push_back({.name="REMEMBER.LABEL",.type=uint32_t(Type::STRING),.count=1,.string_values={""}});
  const auto left=uint32_t(program.symbols.size());
  program.symbols.push_back({.name="LEFT_LABEL",.type=uint32_t(Type::STRING),.count=1,
                            .flags=daedalus::DaedalusSymbolFlag::CONST,.string_values={"left"}});
  const auto right=uint32_t(program.symbols.size());
  program.symbols.push_back({.name="RIGHT_LABEL",.type=uint32_t(Type::STRING),.count=1,
                            .flags=daedalus::DaedalusSymbolFlag::CONST,.string_values={"right"}});
  begin(remember);
  call(Op::PUSHV,label);op(Op::MOVS);
  call(Op::PUSHV,state);op(Op::MOVI);
  call(Op::BE,random);
  call(Op::PUSHV,state);op(Op::RSR);
  const auto story=function("STORY",false,Type::INT);begin(story);
  const auto rememberAddress=uint32_t(program.symbols[remember].address);
  call(Op::PUSHI,40);call(Op::PUSHV,right);call(Op::BL,rememberAddress);
  call(Op::PUSHI,2);call(Op::PUSHV,left);call(Op::BL,rememberAddress);
  op(Op::ADD);op(Op::RSR);
  if(!legacy) {
    program.bytecode.clear();
    return daedalus::DaedalusVm::without_legacy_dispatcher(daedalus::DaedalusScript::from_program(std::move(program)));
    }
  return daedalus::DaedalusVm(daedalus::DaedalusScript::from_program(std::move(program)));
  }
std::string measure(daedalus::DaedalusVm& vm, DayScriptStats& stats, const std::vector<std::string>& sources) {
  for(const auto name:{"ZS_A_LOOP","ZS_B_LOOP"}) {
    stats.begin(name);
    vm.call_function<void>(name);
    stats.end();
    }
  stats.begin("ZS_A_LOOP");
  for(const auto name:{"ZS_B_LOOP","OUTSIDE"}) {
    stats.begin(name);
    vm.call_function<void>(name);
    stats.end();
    }
  stats.end();
  vm.unsafe_call_bl(vm.find_symbol_by_name("OUTSIDE"));
  stats.random(false,true);
  stats.random(false,false);
  check(sources==std::vector<std::string>{"HLP_RANDOM/ZS_A/loop","HLP_RANDOM/ZS_B/loop","HLP_RANDOM/ZS_B/loop",
                                        "HLP_RANDOM/outside/OUTSIDE","HLP_RANDOM/outside/OUTSIDE"},
        "random ownership follows nested states and perceptions, then restores outside top-level");
  const auto bytes=stats.json();
  rapidjson::Document j;
  j.Parse(bytes.c_str());
  check(!j.HasParseError(),"valid counter JSON");
  check(j["calls"].GetUint64()==10,"all nested script calls included");
  const auto& instructions=j["instructions_by_symbol"];
  check(instructions["ZS_A_LOOP"]==2 && instructions["ZS_B_LOOP"]==4 && instructions["HELPER"]==10 &&
        instructions["OUTSIDE"]==4 && j["unattributed_instructions"]==0,"executed VM instructions belong to their actual body");
  check(j["states"]["ZS_A"]["phases"]["loop"]["engine"]==2,"engine phase entries counted separately from VM calls");
  check(j["states"]["ZS_A"]["phases"]["loop"]["nested"]==1,"nested call belongs to phase");
  check(j["states"]["ZS_A"]["phases"]["loop"]["instructions"]==4,"inclusive VM instruction count");
  check(j["states"]["ZS_B"]["phases"]["loop"]["externals"]["HLP_RANDOM"]==2,"external attributed to the innermost state");
  uint64_t total=0;
  for(const auto& value:j["families"].GetObject()) total+=value.value.GetUint64();
  check(total==j["calls"].GetUint64(),"family counts converge");
  total=0;
  for(const auto& value:j["random_sources"].GetObject()) total+=value.value.GetUint64();
  check(total==7 && total==j["random"].GetUint64(),"all random sources converge");
  return bytes;
  }

void checkOverrides() {
  auto vm=makeVm();
  check(vm.call_observer_count()==0,"VM starts without call observers");
  auto stats=DayScriptStats::attach(vm,{});
  const std::weak_ptr<DayScriptStats> lifetime=stats;
  check(vm.call_observer_count()==1,"the interval owns the first call observer");
  stats->stop(vm);
  check(vm.call_observer_count()==0,"stopping the sole interval restores the unobserved VM");
  stats.reset();
  check(lifetime.expired(),"VM releases its first collector after stop");

  unsigned overrides=0,notifications=0;
  vm.override_function("HELPER",[&] {++overrides;});
  vm.call_function<void>("OUTSIDE");
  check(overrides==1,"ordinary calls still execute with no observers after stop");
  vm.observe_calls([&](const auto& symbol) {if(symbol.name()=="HELPER") ++notifications;});
  vm.call_function<void>("OUTSIDE");
  check(overrides==2 && notifications==1,"bytecode BL reports an overridden call once");
  vm.unsafe_call_bl(vm.find_symbol_by_name("HELPER"));
  check(overrides==3 && notifications==2,"unsafe_call_bl reports an overridden call once");

  unsigned independentInstructions=0, offlineInstructions=0;
  const auto observer=vm.add_instruction_observer([&](const auto&,const auto&) { ++independentInstructions; });
  vm.observe_instructions([&](const auto&) { ++offlineInstructions; });
  stats=DayScriptStats::attach(vm,{});
  vm.call_function<void>("OUTSIDE");
  rapidjson::Document counters;
  counters.Parse(stats->json().c_str());
  check(counters["symbols"]["HELPER"]==1 && !counters["instructions_by_symbol"].HasMember("HELPER") &&
        counters["instructions_by_symbol"]["OUTSIDE"]==2 && counters["unattributed_instructions"]==0,
        "overridden engine calls leave no executed bytecode for the helper");
  check(independentInstructions==2 && offlineInstructions==2,"day probing coexists with other instruction observers");
  stats->stop(vm);
  vm.call_function<void>("OUTSIDE");
  check(independentInstructions==4 && offlineInstructions==4,"stopping the day probe preserves other instruction observers");
  vm.remove_instruction_observer(observer);
  vm.observe_instructions({});
  vm.call_function<void>("OUTSIDE");
  check(independentInstructions==4 && offlineInstructions==4,"instruction observers stop independently");

  vm.require_native_execution("synthetic");
  vm.unsafe_call_bl(vm.find_symbol_by_name("HELPER"));
  check(overrides==7,"strict instruction audit permits native overrides");
  }

void checkNestedStoryPrograms() {
  auto vm=makeVm();
  const auto* remember=vm.find_symbol_by_name("REMEMBER");
  storygraph::Bytecode code(vm);
  const auto root=storygraph::liftProgram(vm,code,vm.find_symbol_by_name("STORY")->index());
  const auto nested=storygraph::liftProgram(vm,code,remember->index());
  check(root.error.empty() && nested.error.empty(),"the dialog and its typed helper lift");
  storygraph::VmExternals external(vm);
  storygraph::Interpreter interpreter(vm,external);
  storygraph::Program reentrant;
  reentrant.steps.push_back({.kind=storygraph::Step::Return,.value=storygraph::ex::param(0)});
  unsigned externals=0,instructions=0;
  vm.register_external("HLP_RANDOM",[&] {
    ++externals;
    const auto value=interpreter.run(reentrant,{storygraph::Value::integer(99)});
    check(value.ok && value.returned.i==99,"an external can reenter StoryGraph with independent arguments");
    });
  vm.observe_instructions([&](const auto&) { ++instructions; });
  auto result=interpreter.run(root);
  check(result.ok && result.returned.i==4 && instructions>0 && externals==2,
        "without a resolver helpers take the existing VM route and return by reference");
  interpreter.resolveProgram=[&](const auto& fn) { return &fn==remember ? &nested : nullptr; };
  instructions=0;
  result=interpreter.run(root);
  check(result.ok && result.returned.i==4 && instructions==0 && externals==4 &&
        vm.find_symbol_by_name("REMEMBER.LABEL")->get_string()=="left",
        "resolved helpers preserve late reads, typed parameter storage and external order without VM instructions");

  bool enabled=true;
  unsigned overrides=0;
  vm.override_function("REMEMBER",std::function<int32_t(int32_t,std::string_view)>{
    [&](int32_t number,std::string_view) { ++overrides; return number+10; }},[&] { return enabled; });
  result=interpreter.run(root);
  check(result.ok && result.returned.i==62 && overrides==2 && externals==4 && instructions==0,
        "an enabled native override takes precedence over a resolved graph program");
  enabled=false;
  interpreter.resolveProgram=[&](const auto&) { return &root; };
  result=interpreter.run(root);
  check(!result.ok && result.error=="program call depth exceeded" && instructions==0 && externals==4,
        "a resolved call cycle stops without VM fallback after execution starts");
  interpreter.resolveProgram={};
  interpreter.allowUnresolvedScriptCalls=false;
  result=interpreter.run(root);
  check(!result.ok && result.error=="unresolved nested program: REMEMBER" && instructions==0 && externals==4,
        "a verified stock program rejects a missing nested program before it can enter the VM");
  interpreter.allowUnresolvedScriptCalls=true;
  result=interpreter.run(root);
  check(result.ok && result.returned.i==4 && externals==6,"the next dialog starts with a clean interpreter frame");

  auto native=makeVm(false);
  native.require_native_execution("synthetic",true);
  const auto* function=native.find_symbol_by_name("REMEMBER");
  const std::vector arguments={storygraph::Value::integer(5),storygraph::Value::str("direct")};
  std::string error;
  native.push_int(123);
  check(!storygraph::callFunction(native,*function,arguments,error) &&
        error=="script function is unavailable without bytecode: REMEMBER" && native.pop_int()==123,
        "a native-only direct call rejects a missing override before changing the stack");
  storygraph::VmExternals nativeExternals(native);
  storygraph::Interpreter prepared(native,nativeExternals);
  prepared.allowUnresolvedScriptCalls=false;
  unsigned preparedCalls=0;
  native.register_external("HLP_RANDOM",[&] { ++preparedCalls; });
  const auto preparedResult=prepared.run(nested,arguments);
  check(preparedResult.ok && preparedResult.returned.i==5 && preparedCalls==1 &&
        native.find_symbol_by_name("REMEMBER.LABEL")->get_string()=="direct",
        "a prepared root receives direct call arguments without a legacy dispatcher");
  native.override_function("REMEMBER",[&](int32_t number,std::string_view label) {
    check(label=="direct","native direct calls keep argument order");
    return number+10;
    });
  const auto returned=storygraph::callFunction(native,*function,arguments,error);
  check(returned && returned->type==storygraph::Value::Int && returned->i==15,
        "a native-only direct call uses the active override outside the legacy allowlist");
  }

void checkNativePhaseAccounting() {
  auto vm=makeVm();
  vm.register_external("HLP_RANDOM",[] {});
  auto stats=DayScriptStats::attach(vm,{});

  stats->begin("ZS_LATE_LOOP");
  vm.call_function<void>("HELPER");
  stats->begin("ZS_A_LOOP");
  vm.call_function<void>("ZS_A_LOOP");
  stats->end();
  stats->record();
  stats->end();

  stats->begin("ZS_EARLY_LOOP");
  stats->record();
  vm.call_function<void>("HELPER");
  stats->end();
  stats->begin("ZS_EMPTY_LOOP");
  stats->record();
  stats->end();
  stats->begin("ZS_B_LOOP");
  vm.call_function<void>("HELPER");
  vm.call_function<void>("ZS_B_LOOP");
  stats->end();

  rapidjson::Document counters;
  counters.Parse(stats->json().c_str());
  const auto phase=[&](const char* state) -> const rapidjson::Value& {
    return counters["states"][state]["phases"]["loop"];
    };
  check(phase("ZS_LATE")["records"]==1 && phase("ZS_LATE")["nested"]==1 &&
        phase("ZS_LATE")["instructions"]==2,"late native acceptance counts its first VM helper as nested");
  check(phase("ZS_EARLY")["records"]==1 && phase("ZS_EARLY")["nested"]==1 &&
        phase("ZS_EMPTY")["records"]==1 && phase("ZS_EMPTY")["nested"]==0,
        "early native acceptance counts only callbacks that ran");
  check(phase("ZS_A")["records"]==0 && phase("ZS_A")["nested"]==1 &&
        phase("ZS_B")["records"]==0 && phase("ZS_B")["nested"]==2,
        "the inner script phase and later fallback retain their own root calls");
  stats->stop(vm);

  DayScriptStats native({},DayScriptStats::Metric::NativePhases);
  native.registerBeastState("ZS_MM_IDLE");
  native.begin("ZS_A_LOOP");
  native.record();
  native.record();
  native.random(true,true);
  native.begin("B_ASSESSPLAYER");
  native.record();
  native.random(true,true);
  native.end();
  native.random(true,true);
  native.end();
  native.begin("ZS_MM_IDLE_LOOP");
  native.end();
  counters.Parse(native.json().c_str());
  check(!counters.HasParseError() && counters["metric"]=="native_phase_calls" && counters["calls"]==2 &&
        !counters.HasMember("instructions_by_symbol") && !counters.HasMember("unattributed_instructions"),
        "native phase counts identify their metric and omit VM costs");
  check(phase("ZS_A")["engine"]==1 && phase("ZS_A")["records"]==1 && phase("ZS_A")["random"]==2 &&
        !phase("ZS_A").HasMember("instructions") && !phase("ZS_A").HasMember("nested") &&
        !phase("ZS_A").HasMember("externals") && phase("ZS_MM_IDLE")["engine"]==1 && phase("ZS_MM_IDLE")["records"]==0,
        "native accounting counts accepted phases once and preserves observed phase entries");
  check(counters["random_sources"]["HLP_RANDOM/ZS_A/loop"]==2 &&
        counters["random_sources"]["HLP_RANDOM/outside/B_ASSESSPLAYER"]==1 &&
        counters["symbols"]["B_ASSESSPLAYER"]==1 && counters["families"]["perceptions"]==1,
        "native observations retain perception identity and return to the parent RNG context");
  }

void run() {
  auto vm=makeVm();
  RoutineDefinitions::Library catalog;
  catalog.routines["RTN_TEST"]={{8,0,20,0,"ZS_A","WP"},{20,0,8,0,"ZS_B","WP"}};
  unsigned earlyCalls=0,lateCalls=0;
  std::shared_ptr<DayScriptStats> stats;
  std::string first;
  vm.observe_calls([&](const auto&) {
    ++earlyCalls;
    if(first.empty() && earlyCalls==1) {
      rapidjson::Document counters;
      counters.Parse(stats->json().c_str());
      check(counters["calls"]==0,"startup observer runs before the first interval's collector");
      }
    });
  std::vector<std::string> sources;
  vm.register_external("HLP_RANDOM",[&] {sources.push_back(stats->source(true,true)); stats->random(true,true);});
  for(unsigned interval=0;interval<2;++interval) {
    stats=DayScriptStats::attach(vm,catalog);
    if(interval==0) vm.add_call_observer([&](const auto&) {
      ++lateCalls;
      check(earlyCalls==lateCalls,"startup observer runs before the later observer");
      if(lateCalls==1) {
        rapidjson::Document counters;
        counters.Parse(stats->json().c_str());
        check(counters["calls"]==(first.empty() ? 1 : 0),"collector and later observer run in registration order in both intervals");
        }
      });
    earlyCalls=lateCalls=0;
    sources.clear();
    const auto bytes=measure(vm,*stats,sources);
    if(interval==0) first=bytes;
    else check(bytes==first,"two intervals on the same VM produce identical counter bytes");
    stats->stop(vm);
    check(vm.call_observer_count()==2,"stop removes only the interval's call observer");
    vm.call_function<void>("OUTSIDE");
    check(stats->json()==bytes,"stopped counters stay unchanged");
    check(earlyCalls==12 && lateCalls==12,"both unrelated observers receive all calls, including after stop");
    const std::weak_ptr<DayScriptStats> lifetime=stats;
    stats.reset();
    check(lifetime.expired(),"VM releases the stopped collector");
    }
  }
}
int main() {
  try {
    check(NpcActivityProbe::instance==nullptr,"engine hooks disabled by default");
    run();
    checkOverrides();
    checkNestedStoryPrograms();
    checkNativePhaseAccounting();
    Sink sink;
    int actorToken=0;
    auto* actor=reinterpret_cast<const Npc*>(&actorToken);
    AiQueue queue;
    NpcActivityProbe::instance=&sink;
    queue.pushBack(AiQueue::aiUseMob("SMITH_ANVIL",1),actor);
    NpcActivityProbe::instance=nullptr;
    queue.pushBack(AiQueue::aiUseMob("SMITH_ANVIL",1),actor);
    check(sink.commands.size()==1 && queue.size()==2,"engine enqueue trace observes every enabled command without changing queue");
    check(sink.commands[0].act==AI_UseMob && sink.commands[0].s0=="SMITH_ANVIL" && sink.commands[0].i0==1,
          "queue retains action arguments");
    NpcActivityProbe::instance=&sink;
    queue.pushBack(AiQueue::aiLookAtNpc(nullptr),actor);
    queue.pushBack(AiQueue::aiLookAtNpc(const_cast<Npc*>(actor)),actor);
    NpcActivityProbe::instance=nullptr;
    check(sink.commands.size()==3 && queue.size()==3,"merged LookAtNpc commands are both traced");
    check(sink.commands[1].act==AI_LookAtNpc && sink.commands[1].target==nullptr &&
          sink.commands[2].act==AI_LookAtNpc && sink.commands[2].target==actor,
          "merged LookAtNpc trace retains both original targets");
    std::cout<<"NPC day contract passed: "<<assertions<<" assertions\n";
    return 0;
    }
  catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
  }
