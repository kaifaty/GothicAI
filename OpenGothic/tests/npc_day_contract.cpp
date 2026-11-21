#include "game/gamescript_daystats.h"
#include "programdata.h"
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
daedalus::DaedalusVm makeVm() {
  asset::ProgramRecord program;
  program.version=50;
  program.bytecode.push_back(std::byte(Op::RSR));
  auto function=[&](const char* name,bool external=false) {
    asset::ProgramSymbol symbol;
    symbol.name=name;
    symbol.type=uint32_t(Type::FUNCTION);
    symbol.flags=daedalus::DaedalusSymbolFlag::CONST|(external ? daedalus::DaedalusSymbolFlag::EXTERNAL : 0);
    symbol.return_type=uint32_t(Type::VOID);
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
  vm.call_function<void>("OUTSIDE");
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
  }

void run() {
  auto vm=makeVm();
  RoutineDefinitions::Library catalog;
  catalog.routines["RTN_TEST"]={{8,0,20,0,"ZS_A","WP"},{20,0,8,0,"ZS_B","WP"}};
  unsigned earlyCalls=0,lateCalls=0;
  vm.observe_calls([&](const auto&) {++earlyCalls;});
  std::shared_ptr<DayScriptStats> stats;
  std::vector<std::string> sources;
  vm.register_external("HLP_RANDOM",[&] {sources.push_back(stats->source(true,true)); stats->random(true,true);});
  std::string first;
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
