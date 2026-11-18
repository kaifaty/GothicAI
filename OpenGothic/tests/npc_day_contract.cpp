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
  unsigned commands=0;
  void phaseBegin(const Npc*,std::string_view) override {}
  void phaseEnd(int) override {}
  void random(const Npc*,uint32_t,uint32_t,bool) override {}
  void queued(const Npc*,const AiQueue::AiAction& a) override {
    ++commands;
    check(a.act==AI_UseMob && a.s0=="SMITH_ANVIL" && a.i0==1,"queue retains action arguments");
    }
  void point(const Npc&,const WayPoint*,const WayPoint*) override {}
  void mob(const Npc&,const Interactive&,bool) override {}
  };
std::string run() {
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
  daedalus::DaedalusVm vm(daedalus::DaedalusScript::from_program(std::move(program)));
  unsigned previousCalls=0;
  vm.observe_calls([&](const auto&) {++previousCalls;});
  RoutineDefinitions::Library catalog;
  catalog.routines["RTN_TEST"]={{8,0,20,0,"ZS_A","WP"},{20,0,8,0,"ZS_B","WP"}};
  auto stats=DayScriptStats::attach(vm,catalog);
  std::vector<std::string> sources;
  vm.register_external("HLP_RANDOM",[&] {sources.push_back(stats->source(true,true)); stats->random(true,true);});
  for(const auto name:{"ZS_A_LOOP","ZS_B_LOOP"}) {
    stats->begin(name);
    vm.call_function<void>(name);
    stats->end();
    }
  vm.call_function<void>("OUTSIDE");
  stats->random(false,true);
  stats->random(false,false);
  check(sources==std::vector<std::string>{"HLP_RANDOM/ZS_A/loop","HLP_RANDOM/ZS_B/loop","HLP_RANDOM/outside/OUTSIDE"},"random ownership follows the executing phase and restores outside top-level");
  const auto bytes=stats->json();
  rapidjson::Document j;
  j.Parse(bytes.c_str());
  check(!j.HasParseError(),"valid counter JSON");
  check(j["calls"].GetUint64()==6 && previousCalls==6,"all nested script calls and existing observer included");
  check(j["states"]["ZS_A"]["phases"]["loop"]["engine"]==1,"engine phase counted once");
  check(j["states"]["ZS_A"]["phases"]["loop"]["nested"]==1,"nested call belongs to phase");
  check(j["states"]["ZS_A"]["phases"]["loop"]["instructions"]==4,"inclusive VM instruction count");
  check(j["states"]["ZS_B"]["phases"]["loop"]["externals"]["HLP_RANDOM"]==1,"external attributed by name and state");
  uint64_t total=0;
  for(const auto& value:j["families"].GetObject()) total+=value.value.GetUint64();
  check(total==j["calls"].GetUint64(),"family counts converge");
  total=0;
  for(const auto& value:j["random_sources"].GetObject()) total+=value.value.GetUint64();
  check(total==5 && total==j["random"].GetUint64(),"all random sources converge");
  stats->stop(vm);
  vm.call_function<void>("OUTSIDE");
  check(stats->json()==bytes && previousCalls==8,"stopped interval is inert, original observer retained");
  return bytes;
  }
}
int main() {
  try {
    check(NpcActivityProbe::instance==nullptr,"engine hooks disabled by default");
    check(run()==run(),"repeated programs produce identical counter bytes");
    Sink sink;
    int actorToken=0;
    auto* actor=reinterpret_cast<const Npc*>(&actorToken);
    AiQueue queue;
    NpcActivityProbe::instance=&sink;
    queue.pushBack(AiQueue::aiUseMob("SMITH_ANVIL",1),actor);
    NpcActivityProbe::instance=nullptr;
    queue.pushBack(AiQueue::aiUseMob("SMITH_ANVIL",1),actor);
    check(sink.commands==1 && queue.size()==2,"engine enqueue trace observes every enabled command without changing queue");
    std::cout<<"NPC day contract passed: "<<assertions<<" assertions\n";
    return 0;
    }
  catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
  }
