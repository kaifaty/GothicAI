#include <Tempest/File>
#include <Tempest/MemReader>
#include <Tempest/MemWriter>
#include <Tempest/Pixmap>

#include "commandline.h"
#include "gothic.h"
#include "settings.h"
#include "gamemusic.h"
#include "resources.h"
#include "game/bodyclock.h"
#include "game/bodycontroller.h"
#include "game/bodyobservations.h"
#include "game/bodyenginepolicy.h"
#include "game/gamescript.h"
#include "game/definitions/fightaidefinitions.h"
#include "game/playercontrol.h"
#include "game/serialize.h"
#include "game/simulationstep.h"
#include "physics/dynamicworld.h"
#include "testbench.h"
#include "utils/workers.h"
#include "world/objects/npc.h"
#include "world/objects/item.h"
#include "world/bullet.h"
#include "world/waypoint.h"
#include "world/world.h"
#include "world/worldobjects.h"
#include "world/animationtail.h"
#include "world/npcsimtick.h"
#include "game/bodyarena.h"

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#include <unistd.h>
#include <sys/wait.h>

namespace {
using Json = rapidjson::Document;
size_t assertions=0, cases=0;

struct ManualBodyClock final {
  ManualBodyClock() {
    BodyClock::useManualTime();
    active=true;
    }
  ~ManualBodyClock() {
    active=false;
    BodyClock::useRealTime();
    }
  static void stallBeforeStep() {
    if(!active) return;
    static const auto delay=[] {
      const auto value=std::getenv("BODY_WATCH_TEST_STALL_MS");
      return std::chrono::milliseconds(value ? std::stoll(value) : 0);
      }();
    if(delay.count()>0) std::this_thread::sleep_for(delay);
    }
  void pumpWatchdogs() const {
    stallBeforeStep();
    BodyController::pumpWatchdogs();
    }
  inline static bool active=false;
  };

void expect(bool condition,std::string_view message) {
  ++assertions;
  if(!condition) {
    std::cerr << "body-watch-test failure: " << message << '\n';
    throw std::runtime_error(std::string(message));
    }
  }

void clockSwitching() {
  ++cases;
  const auto before=BodyClock::now();
  BodyClock::useManualTime();
  const auto frozen=BodyClock::now();
  expect(frozen>=before,"freezing the body clock preserves its monotonic epoch");
  BodyClock::advance(std::chrono::milliseconds(20));
  const auto advanced=BodyClock::now();
  expect(advanced-frozen==std::chrono::milliseconds(20),"manual body time advances by exactly the requested interval");
  BodyClock::useRealTime();
  expect(BodyClock::now()>=advanced,"resuming the real body clock never goes behind its last manual value");
  }

Json parse(const std::string& encoded) {
  Json d;
  d.Parse(encoded.c_str());
  if(d.HasParseError() || !d.IsObject()) throw std::runtime_error("invalid body JSON");
  return d;
  }

void number(Json& d,const char* key,uint64_t value) {
  d.AddMember(rapidjson::Value(key,d.GetAllocator()),rapidjson::Value(value),d.GetAllocator());
  }

Json status(const Npc& actor) {
  auto d=parse(BodyController::statusJson(&actor,true));
  Json row;
  row.CopyFrom(d["actors"][0],row.GetAllocator());
  return row;
  }

Json request(const char* type,const Npc& actor) {
  auto row=status(actor);
  Json d(rapidjson::kObjectType);
  d.AddMember("type",rapidjson::Value(type,d.GetAllocator()),d.GetAllocator());
  d.AddMember("provider","body-watch-test",d.GetAllocator());
  for(const char* key:{"session_epoch","world_epoch","entity_id","control_generation"})
    number(d,key,row[key].GetUint64());
  return d;
  }

Json call(const Json& request,bool sentObservations=false) {
  return parse(BodyController::handleJson(request,sentObservations));
  }

Json action(Npc& actor,FightAlgo::PolicyAction policy) {
  auto d=request("body_action",actor);
  const auto current=status(actor);
  for(const char* key:{"goal_version","target_entity_id","observation_seq"})
    number(d,key,current[key].GetUint64());
  number(d,"issued_game_ms",current["observed_game_ms"].GetUint64());
  number(d,"command_id",1); number(d,"action_id",uint64_t(policy)); number(d,"ttl_ms",1000);
  return call(d);
  }

Json acquire(Npc& actor,uint64_t awaitMs=0,const char* mode="lockstep",uint64_t timeoutMs=5000,const char* policy=nullptr) {
  auto d=request("body_acquire",actor);
  d.AddMember("body_control",true,d.GetAllocator());
  if(policy) d.AddMember("policy",rapidjson::Value(policy,d.GetAllocator()),d.GetAllocator());
  d.AddMember("mode",rapidjson::Value(mode,d.GetAllocator()),d.GetAllocator());
  number(d,"timeout_ms",timeoutMs);
  number(d,"await_boundary_ms",awaitMs);
  number(d,"target_entity_id",BodyObservations::entityId(*actor.target()));
  return call(d);
  }

void release(Npc& actor) {
  auto result=call(request("body_release",actor));
  expect(result["accepted"].GetBool(),"release accepted");
  expect(!BodyController::owns(actor),"idle lease released at safe boundary");
  }

using Identity = std::array<uint64_t,4>;
Identity identity(const rapidjson::Value& row) {
  return {row["session_epoch"].GetUint64(),row["world_epoch"].GetUint64(),
          row["entity_id"].GetUint64(),row["control_generation"].GetUint64()};
  }

void step(PlayerControl& player,unsigned count=1) {
  for(unsigned i=0;i<count;++i) {
    ManualBodyClock::stallBeforeStep();
    SimulationStep::worldPlayerStep(33,player);
    SimulationStep::finishStep(33,player);
    }
  }

void initialWatch(Npc& actor,uint64_t generation) {
  BodyObservations::reset(generation);
  BodyObservations::configure(BodyObservations::Config{});
  BodyObservations::watch({&actor});
  BodyController::setActors({&actor});
  BodyObservations::sampleAll();
  }

#ifdef GOTHIC_BODY_ENGINE_NET
void missingEngineNetwork(Npc& actor) {
  ++cases;
  const auto network=parse(BodyEnginePolicy::netStatusJson());
  expect(!network["loaded"].GetBool() && network["checkpoint_sha256"].IsNull() && network["error"].IsString(),
         "missing network hello status includes the load error");
  initialWatch(actor,700);
  const auto response=acquire(actor,0,"lockstep",5000,"engine");
  expect(!response["accepted"].GetBool() && std::string_view(response["reason"].GetString())=="body_net_unavailable",
         "body_acquire without weights returns body_net_unavailable");
  expect(!BodyController::owns(actor),"missing network does not grant ownership");
  expect(acquire(actor)["accepted"].GetBool(),"ordinary acquire still succeeds without network weights");
  release(actor);
  BodyController::reset("missing_network_cleanup");
  BodyObservations::disable();
  }

void enginePolicy(Npc& actor,Npc& target,PlayerControl& player) {
  const ManualBodyClock clock;
  ++cases;
  BodyController::fixturePrepare(actor,target,150,0);
  actor.setExternalControl(false);
  initialWatch(actor,700);
  const auto initialGame=BodyObservations::latestGameMs(actor);
  const auto acquired=acquire(actor,0,"lockstep",5000,"engine");
  expect(acquired["accepted"].GetBool(),"engine policy acquires a real NPC");
  const BodyNet weights(GOTHIC_BODY_ENGINE_NET);
  BodyPolicyRuntime direct(weights,100,100,true);
  direct.beginEpisode(BodyObservations::sessionEpoch(),BodyObservations::worldEpoch(),initialGame);
  uint64_t sequence=0;
  size_t started=0;
  std::map<uint64_t,int> expected,applied;
  std::string latest;
  BodyObservations::setSink([&](std::string_view encoded) {
    auto row=parse(std::string(encoded));
    if(!row["periodic"].GetBool() || row["control_generation"].GetUint64()!=acquired["control_generation"].GetUint64()) return;
    latest=encoded;
    const bool eligible=row["can_decide"].GetBool() && row["action_mask_valid"].GetBool();
    Json process(rapidjson::kObjectType);
    auto& a=process.GetAllocator();
    process.AddMember("observation",rapidjson::Value(row,a),a);
    process.AddMember("stream_seq",++sequence,a);
    process.AddMember("now_game_ms",row["t_game_ms"].GetUint64(),a);
    process.AddMember("command_enabled",eligible,a);
    process.AddMember("memory_only",!eligible,a);
    const auto result=direct.process(process);
    expect(result.status!="fallback","live periodic row is accepted by the direct runtime");
    if(eligible) {
      const auto action=direct.arbitrate(row,sequence);
      expect(action.status=="decision" && action.actionId.has_value(),"live row has a direct policy decision");
      expected.emplace(row["observation_seq"].GetUint64(),*action.actionId);
      }
    });
  BodyController::drainJson();
  for(unsigned i=0;i<90 && expected.size()<4 && BodyController::owns(actor);++i) {
    expect(call(request("body_heartbeat",actor))["accepted"].GetBool(),"owner heartbeat keeps the engine lease alive");
    step(player);
    const auto events=parse(BodyController::drainJson());
    for(const auto& event:events["events"].GetArray()) {
      if(std::string_view(event["phase"].GetString())=="started") ++started;
      if(std::string_view(event["phase"].GetString())=="accepted") {
        applied.emplace(event["observation_seq"].GetUint64(),int(event["action_id"].GetUint64()));
        expect(event["ttl_ms"].GetUint64()==100,"engine actions use the ordinary executor with ttl 100");
        }
      }
    }
  BodyObservations::setSink({});
  expect(expected.size()==4 && applied==expected,"real NPC applies exactly the direct policy sequence on the emitted rows");
  expect(started>0,"engine policy physically starts an action through the native executor");
  expect(BodyController::owns(actor) && !latest.empty(),"live engine lease survives the policy sequence");
  auto refusal=parse(latest);
  refusal["observation_seq"].SetUint64(refusal["observation_seq"].GetUint64()+1);
  refusal["sample_period_ms"].SetUint64(100);
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  refusal.Accept(writer);
  BodyController::onObservation(actor,{buffer.GetString(),buffer.GetSize()});
  expect(BodyController::owns(actor),"policy fallback defers native restoration until the next simulation boundary");
  auto reports=BodyController::drainPolicyJson();
  bool measured=false,finalReport=false;
  for(const auto& report:reports) {
    const auto metrics=parse(report);
    if(metrics["final"].GetBool()) {
      expect(metrics["fallbacks"]["unsupported_sampling_rate"].GetUint64()==1,"live fallback telemetry retains the policy reason");
      finalReport=true;
      }
    if(metrics["decisions"].GetUint64()>0) {
      expect(metrics["p50_us"].GetDouble()>0 && metrics["p99_us"].GetDouble()>=metrics["p50_us"].GetDouble(),
             "live body_policy contains actual inference timings");
      measured=true;
      }
    std::cerr << "engine-policy-fixture body_policy=" << report << '\n';
    }
  expect(measured && finalReport,"policy fallback immediately emits a body_policy event with measured network steps");
  for(unsigned i=0;i<100 && BodyController::owns(actor);++i) step(player);
  const auto released=parse(BodyController::drainJson());
  bool restored=false;
  for(const auto& event:released["events"].GetArray())
    if(std::string_view(event["reason"].GetString())=="native_restored" &&
       std::string_view(event["release_reason"].GetString())=="policy_fallback:unsupported_sampling_rate") restored=true;
  expect(restored && !BodyController::owns(actor),"policy refusal returns the actual NPC to native AI at its safe boundary");
  BodyController::reset("engine_fixture_cleanup");

  ++cases;
  BodyController::fixturePrepare(actor,target,150,0);
  actor.setExternalControl(false);
  initialWatch(actor,701);
  expect(acquire(actor,0,"realtime",500,"engine")["accepted"].GetBool(),"watchdog fixture acquires an engine lease");
  const auto ownership=parse(BodyController::drainJson());
  uint64_t ownerHeartbeat=0;
  for(const auto& event:ownership["events"].GetArray())
    if(std::string_view(event["reason"].GetString())=="acquired") ownerHeartbeat=event["last_heartbeat_wall_ms"].GetUint64();
  expect(ownerHeartbeat>0,"watchdog fixture records the owner's acquisition heartbeat");
  BodyClock::advance(std::chrono::milliseconds(20));
  uint64_t accepted=0;
  bool providerLost=false;
  bool checkedBeforeDeadline=false;
  restored=false;
  for(unsigned i=0;i<100 && BodyController::owns(actor);++i) {
    BodyClock::advance(std::chrono::milliseconds(10));
    step(player);
    const auto events=parse(BodyController::drainJson());
    for(const auto& event:events["events"].GetArray()) {
      const std::string_view reason=event["reason"].GetString();
      if(std::string_view(event["phase"].GetString())=="accepted") {
        ++accepted;
        expect(event["last_heartbeat_wall_ms"].GetUint64()==ownerHeartbeat,
               "local engine decisions do not refresh the owner's heartbeat");
        }
      if(reason=="provider_lost" && std::string_view(event["release_reason"].GetString())=="watchdog_timeout") providerLost=true;
      if(reason=="native_restored" && std::string_view(event["release_reason"].GetString())=="watchdog_timeout") restored=true;
      }
    if(20+10*(i+1)==490) {
      expect(accepted>0 && BodyController::owns(actor) && !providerLost,
             "engine decisions run while the owner watchdog is still 10 ms before its deadline");
      checkedBeforeDeadline=true;
      }
    }
  expect(checkedBeforeDeadline,"engine lease reaches the 490 ms watchdog check");
  expect(accepted,"engine accepts a decision before the owner watchdog expires");
  expect(providerLost && restored && !BodyController::owns(actor),"engine decisions cannot prevent watchdog_timeout and native restoration");
  uint64_t reported=0,resets=0,finals=0;
  for(const auto& report:BodyController::drainPolicyJson()) {
    const auto metrics=parse(report);
    reported+=metrics["decisions"].GetUint64();
    resets+=metrics["hidden_reset"].GetUint64();
    if(metrics["final"].GetBool()) {
      ++finals;
      expect(std::string_view(metrics["reason"].GetString())=="watchdog_timeout",
             "ordinary release reports its final interval with the release reason");
      }
    }
  expect(reported==accepted && resets==1 && finals==1,
         "ordinary release retains all decisions and the initial memory reset exactly once");
  BodyController::reset("engine_watchdog_cleanup");
  BodyController::fixturePrepare(actor,target,300,0);
  actor.setExternalControl(false);
  }
#endif

void liveLease(Npc& a,Npc& b,PlayerControl& player) {
  ++cases;
  initialWatch(a,100);
  expect(acquire(a)["accepted"].GetBool(),"acquire A");
  step(player,2);
  const auto before=status(a);
  const auto stable=identity(before);
  const auto sequence=BodyObservations::latestSequence(a);
  expect(sequence>1,"A has established observation stream");
  BodyObservations::drainJson();

  expect(std::string_view(BodyController::updateWatch({&a,&b},50))=="allowed","grow watch with lease A");
  expect(identity(status(a))==stable,"A identity survives grow");
  expect(BodyObservations::latestSequence(a)==sequence,"grow preserves A observation sequence");
  expect(BodyObservations::latestSequence(b)==0,"new B awaits next tick");
  step(player,2);
  expect(BodyObservations::latestSequence(a)>sequence,"A observation sequence continues growing");
  expect(BodyObservations::latestSequence(b)>0,"B receives observations");
  auto rows=parse(BodyObservations::drainJson());
  size_t aRows=0,bRows=0;
  uint64_t previous=sequence;
  for(const auto& row:rows["observations"].GetArray()) {
    if(row["entity_id"].GetUint64()==stable[2]) {
      ++aRows;
      expect(identity(row)==stable,"A stream identity survives grow");
      expect(row["observation_seq"].GetUint64()>previous,"A rows continue without sequence reset");
      previous=row["observation_seq"].GetUint64();
      }
    if(row["entity_id"].GetUint64()==BodyObservations::entityId(b)) ++bRows;
    }
  expect(aRows>0 && bRows>0,"both actors emit real observation rows");

  auto action=request("body_action",a);
  const auto current=status(a);
  for(const char* key:{"goal_version","target_entity_id","observation_seq"})
    number(action,key,current[key].GetUint64());
  number(action,"issued_game_ms",current["observed_game_ms"].GetUint64());
  number(action,"command_id",1); number(action,"action_id",0); number(action,"ttl_ms",1000);
  expect(call(action)["accepted"].GetBool(),"action on original A lease accepted after grow");
  b.setExternalControl(false);
  expect(acquire(b)["accepted"].GetBool(),"newly watched B can acquire lease");
  const auto bIdentity=identity(status(b));
  for(const auto& watch:std::array<std::vector<Npc*>,2>{{{&b},{}}}) {
    expect(std::string_view(BodyController::updateWatch(watch,50))!="allowed","cannot omit leased A");
    expect(identity(status(a))==stable && identity(status(b))==bIdentity,"refused watch leaves both leases intact");
    expect(BodyObservations::watchedCount()==2,"refused watch leaves subscription intact");
    }
  expect(std::string_view(BodyController::updateWatch({&a,&b},100))!="allowed","cannot change rate under lease");
  expect(BodyObservations::samplePeriodMs()==50,"refused rate leaves period intact");
  step(player,2);
  const auto cursor=BodyObservations::queuedTotal();
  release(b);
  BodyObservations::sampleAll();
  expect(!BodyObservations::queuedSince(cursor).empty(),"B release queues a row after the cursor");
  b.setExternalControl(true);
  expect(std::string_view(BodyController::updateWatch({&a},50))=="allowed","remove free B while A remains leased");
  expect(identity(status(a))==stable,"A identity survives shrink");
  for(const auto& row:BodyObservations::queuedSince(cursor))
    expect(row.enqueueIndex>cursor && row.entityId==stable[2],"queue cursor skips removed B without returning old A rows");
  rows=parse(BodyObservations::drainJson());
  expect(!rows["observations"].Empty(),"shrink retains already queued A rows");
  for(const auto& row:rows["observations"].GetArray())
    expect(row["entity_id"].GetUint64()==stable[2],"shrink drops already queued free B rows");
  step(player,2);
  rows=parse(BodyObservations::drainJson());
  expect(!rows["observations"].Empty(),"A stream continues after shrink");
  for(const auto& row:rows["observations"].GetArray())
    expect(row["entity_id"].GetUint64()==stable[2],"removed B emits no new rows");
  release(a);
  }

void realtimeSentObservation(Npc& actor,Npc& target,PlayerControl& player) {
  const ManualBodyClock clock;
  ++cases;
  BodyController::fixturePrepare(actor,target,300,0);
  actor.setExternalControl(false);
  initialWatch(actor,600);
  expect(acquire(actor,0,"realtime")["accepted"].GetBool(),"sent observation fixture acquired realtime lease");
  step(player,2);
  const auto sent=status(actor);
  const auto sequence=sent["observation_seq"].GetUint64();
  const auto gameMs=sent["observed_game_ms"].GetUint64();
  BodyObservations::drainJson();
  BodyObservations::markDrainedSent();
  expect(BodyObservations::latestSent(actor).sequence==sequence,"successful send records the drained observation");
  const auto command=[&](uint64_t id,uint64_t ttl=1000) {
    auto d=request("body_action",actor);
    for(const char* key:{"goal_version","target_entity_id","observation_seq"})
      number(d,key,sent[key].GetUint64());
    number(d,"issued_game_ms",gameMs);
    number(d,"command_id",id); number(d,"action_id",uint64_t(FightAlgo::PolicyAction::Continue));
    number(d,"ttl_ms",ttl);
    return d;
    };
  const auto rejected=[&](const Json& response,const char* reason,std::string_view message) {
    expect(!response["accepted"].GetBool() && std::string_view(response["reason"].GetString())==reason,message);
    };

  BodyClock::advance(std::chrono::milliseconds(250-20));
  step(player,2);
  const auto nextSequence=BodyObservations::latestSequence(actor);
  expect(nextSequence>sequence,"next frame samples a newer observation");
  rejected(call(command(1)),"stale_observation","runner still rejects an older sampled observation");
  expect(call(command(2,250),true)["accepted"].GetBool(),"realtime accepts the sent observation while a newer row is only sampled");
  BodyObservations::drainJson();
  expect(call(command(3),true)["accepted"].GetBool(),"draining without a successful send does not stale the observation");
  auto mismatched=command(4);
  mismatched["issued_game_ms"].SetUint64(gameMs+1);
  rejected(call(mismatched,true),"stale_observation","realtime still checks the sent observation timestamp");
  const auto gameAge=actor.world().tickCount()-gameMs;
  expect(gameAge>0 && gameAge<250,"fixture advances less game time than its wall TTL");
  rejected(call(command(5,gameAge),true),"expired","a newer unsent sample does not refresh the original game TTL");

  BodyClock::advance(std::chrono::milliseconds(40)); // 250 + 20 ms since the sent observation.
  step(player);
  BodyObservations::sampleAll(true);
  expect(BodyObservations::latestSequence(actor)>nextSequence,"wall TTL fixture has a fresh unsent sample");
  expect(actor.world().tickCount()-gameMs<250,"original observation remains within the game TTL");
  rejected(call(command(6,250),true),"expired_wall","a newer unsent sample does not refresh the original wall TTL");
  BodyObservations::markDrainedSent();
  expect(BodyObservations::latestSent(actor).sequence==nextSequence,"successful send marks only the last drained rows");
  rejected(call(command(7),true),"stale_observation","sending the newer row makes the previous observation stale");
  release(actor);
  expect(std::string_view(BodyController::updateWatch({},50))=="allowed","sent observation fixture can leave watch after release");
  expect(BodyObservations::latestSent(actor).sequence==0,"removing an actor clears its sent observation");
  }

void pendingAcquire(Npc& a,Npc& b,PlayerControl& player) {
  const ManualBodyClock clock;
  ++cases;
  // Start the real script state once, then clean its initial work with the same fixture used
  // by the body API suite. fixtureLock installs an actual FAI game-time wait.
  const auto attack=a.world().script().findSymbolIndex("ZS_ATTACK");
  expect(attack!=size_t(-1),"native combat script exists");
  expect(a.startState(ScriptFn(attack),""),"start native combat state");
  a.setOther(&b);
  for(unsigned i=0;i<60 && !a.isStateStarted();++i) step(player);
  expect(a.isStateStarted() && a.currentState()==ScriptFn(attack),"native combat state started");
  BodyController::fixturePrepare(a,b,300,0);
  a.setExternalControl(false);
  initialWatch(a,101);
  BodyController::fixtureLock(a,200);
  const auto pending=acquire(a,2000);
  expect(pending["accepted"].GetBool() && std::string_view(pending["reason"].GetString())=="acquire_pending","A waits for real locked boundary");
  const auto requestId=pending["acquire_request_id"].GetUint64();
  const auto stable=identity(status(a));
  const auto sequence=BodyObservations::latestSequence(a);
  expect(BodyController::hasOwners(),"pending acquire counts as body owner");
  expect(std::string_view(BodyController::updateWatch({&b},50))!="allowed","cannot omit pending A");
  expect(std::string_view(BodyController::updateWatch({},50))!="allowed","cannot clear pending A");
  expect(std::string_view(BodyController::updateWatch({&a,&b},50))=="allowed","grow watch with pending A");
  auto current=status(a);
  expect(current["acquire_pending"].GetBool() && current["acquire_request_id"].GetUint64()==requestId,"pending request survives with same ID");
  expect(identity(current)==stable && BodyObservations::latestSequence(a)==sequence,"pending identity and sequence preserved");
  for(unsigned i=0;i<20 && !BodyController::owns(a);++i) step(player);
  expect(BodyController::owns(a),"pending A grants at natural boundary");
  current=status(a);
  expect(current["acquire_request_id"].GetUint64()==requestId,"granted lease retains original pending request ID");
  expect(current["session_epoch"].GetUint64()==stable[0] && current["world_epoch"].GetUint64()==stable[1] && current["entity_id"].GetUint64()==stable[2],"grant retains stream identity");
  expect(BodyObservations::latestSequence(a)>sequence,"pending A stream sequence keeps growing");
  release(a);
  }

void movementRelease(Npc& actor,Npc& target,PlayerControl& player) {
  struct Case final { FightAlgo::PolicyAction action; float distance,yaw; };
  const std::array<Case,7> fixtures{{
    {FightAlgo::PolicyAction::Approach,800,0},
    {FightAlgo::PolicyAction::TurnToTarget,300,30},
    {FightAlgo::PolicyAction::StrafeLeft,300,0},
    {FightAlgo::PolicyAction::StrafeRight,300,0},
    {FightAlgo::PolicyAction::JumpBack,150,0},
    {FightAlgo::PolicyAction::AttackFront,150,0},
    {FightAlgo::PolicyAction::Block,150,0},
    }};
  for(const auto& fixture:fixtures) {
    ++cases;
    BodyController::fixturePrepare(actor,target,fixture.distance,fixture.yaw);
    actor.setExternalControl(false);
    initialWatch(actor,200+uint64_t(fixture.action));
    expect(acquire(actor)["accepted"].GetBool(),"release fixture acquired");
    step(player,2);
    const auto prepared=status(actor);
    std::cerr << "release-fixture prepare action=" << uint64_t(fixture.action)
              << " pose=" << actor.animNames() << " can_decide=" << prepared["can_decide"].GetBool()
              << " mask=" << prepared["action_mask"][rapidjson::SizeType(fixture.action)].GetBool() << '\n';
    const auto accepted=action(actor,fixture.action);
    if(!accepted["accepted"].GetBool())
      std::cerr << "release-fixture rejected reason=" << accepted["reason"].GetString() << '\n';
    expect(accepted["accepted"].GetBool(),"release fixture action accepted");
    step(player);
    const auto before=actor.bodyObservationState();
    const auto pose=actor.animNames();
    const auto startedEvents=parse(BodyController::drainJson());
    bool started=false;
    for(const auto& event:startedEvents["events"].GetArray())
      if(std::string_view(event["phase"].GetString())=="started" &&
         event["action_id"].GetUint64()==uint64_t(fixture.action)) started=true;
    if(!started) {
      std::cerr << "release-fixture did not start action=" << uint64_t(fixture.action) << " pose=" << pose << '\n';
      for(const auto& event:startedEvents["events"].GetArray())
        std::cerr << "release-fixture event phase=" << event["phase"].GetString()
                  << " reason=" << event["reason"].GetString() << " action=" << event["action_id"].GetUint64() << '\n';
      }
    expect(started,"release fixture physically starts its requested action");
    const auto start=actor.world().tickCount();
    // Target replacement is deferred until the same safe handoff used when a live
    // opponent disappears. The explicit release must not let locomotion keep its lease.
    const auto releaseRequest=request("body_release",actor);
    bool deferredTargetCleared=false;
    actor.setTarget(nullptr);
    // Native perception may choose this enemy again on its next tick. Observe the
    // deferred mutation itself, immediately after setTarget and before native AI runs.
    expect(BodyController::deferNative(actor,[&] { deferredTargetCleared=actor.target()==nullptr; }),
           "release fixture queues target-loss verification before native handoff");
    const auto released=call(releaseRequest);
    expect(released["accepted"].GetBool() ||
           (std::string_view(released["reason"].GetString())=="not_owned" && !BodyController::owns(actor)),
           "moving release accepted or already returned to native after target loss");
    step(player);
    const bool ownedNextTick=BodyController::owns(actor);
    for(unsigned i=0;i<100 && BodyController::owns(actor);++i) step(player);
    const auto events=parse(BodyController::drainJson());
    bool restored=false;
    uint64_t elapsed=actor.world().tickCount()-start;
    for(const auto& event:events["events"].GetArray())
      if(std::string_view(event["reason"].GetString())=="native_restored") {
        restored=true; elapsed=event["t_game_ms"].GetUint64()-start;
        }
    std::cerr << "release-fixture action=" << uint64_t(fixture.action) << " pose=" << pose
              << " segment_ms=" << before.segmentRemainingMs << " fai_ms=" << before.faiWaitMs
              << " ani_ms=" << before.aniWaitMs << " ai_ms=" << before.aiWaitMs
              << " output_ms=" << before.outputWaitMs << " interruptible=" << before.interruptible
              << " owned_next_tick=" << ownedNextTick << " elapsed_game_ms=" << elapsed << '\n';
    expect(!BodyController::owns(actor),"release fixture returns native authority within 3.3 game seconds");
    expect(restored,"release fixture emits confirmed native_restored");
    expect(deferredTargetCleared,"release applies deferred target loss before native AI resumes");
    if(before.segmentRemainingMs==0)
      expect(!ownedNextTick,"unprotected movement returns native authority on the next engine tick");
    else {
      expect(elapsed>=before.segmentRemainingMs,"release waits for the whole protected combat segment");
      if(before.segmentRemainingMs>33)
        expect(ownedNextTick,"release preserves a protected combat segment");
      }
    }
  }

void farRelease(Npc& actor,Npc& target,Npc& hero,PlayerControl& player,std::vector<std::string>& failures) {
  const ManualBodyClock clock;
  using Clock=std::chrono::steady_clock;
  const auto heroStart=hero.position();
  const auto check=[&](bool condition,std::string_view message) {
    ++assertions;
    if(!condition) failures.emplace_back(message);
    };
  const auto printState=[&](const char* phase,const char* cause,FightAlgo::PolicyAction policy) {
    auto row=status(actor);
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    row.Accept(writer);
    const auto p=actor.bodyObservationState();
    std::cerr << "far-release-fixture phase=" << phase << " cause=" << cause
              << " action=" << uint64_t(policy) << " pose=" << actor.animNames()
              << " segment_ms=" << p.segmentRemainingMs << " fai_ms=" << p.faiWaitMs
              << " ani_ms=" << p.aniWaitMs << " ai_ms=" << p.aiWaitMs
              << " output_ms=" << p.outputWaitMs << " interruptible=" << p.interruptible
              << " status=" << buffer.GetString() << '\n';
    };
  for(bool watchdog:{false,true}) {
    for(const auto policy:{FightAlgo::PolicyAction::AttackFront,FightAlgo::PolicyAction::Approach}) {
      ++cases;
      const char* cause=watchdog ? "watchdog_timeout" : "released";
      actor.setExternalControl(true);
      hero.setPosition(actor.position()+Tempest::Vec3(0,0,800)); hero.updateTransform();
      step(player);
      expect(actor.processPolicy()==NpcProcessPolicy::AiNormal,"far fixture starts with full nearby simulation");
      BodyController::fixturePrepare(actor,target,policy==FightAlgo::PolicyAction::Approach ? 800.f : 150.f,0);
      actor.setExternalControl(false);
      initialWatch(actor,300+uint64_t(watchdog)*20+uint64_t(policy));
      expect(acquire(actor,0,"realtime",watchdog ? 500 : 5000)["accepted"].GetBool(),"far fixture acquired realtime lease");
      step(player,2);
      expect(action(actor,policy)["accepted"].GetBool(),"far fixture action accepted");
      step(player);
      auto events=parse(BodyController::drainJson());
      bool started=false;
      for(const auto& event:events["events"].GetArray())
        if(std::string_view(event["phase"].GetString())=="started" &&
           event["action_id"].GetUint64()==uint64_t(policy)) started=true;
      expect(started,"far fixture physically starts requested action");
      if(policy==FightAlgo::PolicyAction::AttackFront)
        expect(actor.bodyObservationState().segmentRemainingMs>33,"far fixture starts a protected attack segment");
      printState("started",cause,policy);
      // WorldObjects switches out of AiNormal at 3000 cm and into AiFar2 at 6000 cm.
      // Keep the actual enemy nearby: teleporting the target would request a different
      // release through target_visibility_lost before the explicit/watchdog request.
      hero.setPosition(actor.position()+Tempest::Vec3(9000,0,0)); hero.updateTransform();
      step(player);
      expect(actor.processPolicy()==NpcProcessPolicy::AiFar2,"far fixture crosses both native simulation radii");
      expect(BodyController::owns(actor),"far fixture retains its lease before requested handoff");
      printState("far_before_request",cause,policy);
      BodyController::drainJson();
      bool queueEmptyAtRestore=false,poseCleanAtRestore=false;
      struct ResetSink final { ~ResetSink() { BodyController::setSink({}); } } resetSink;
      BodyController::setSink([&](std::string_view encoded) {
        const auto event=parse(std::string(encoded));
        if(std::string_view(event["reason"].GetString())=="native_restored") {
          // FightAlgo uses a fixed array: MV_NULL at its head means no instructions.
          const auto row=status(actor);
          queueEmptyAtRestore=row["purity_state"]["queue"][0].GetUint64()==uint64_t(FightAlgo::MV_NULL);
          poseCleanAtRestore=row["segment_remaining_ms"].GetUint64()==0 &&
                             !row["fai_gate"].GetBool() && !row["ani_gate"].GetBool() && row["interruptible"].GetBool();
          }
        });
      const auto startGame=actor.world().tickCount();
      const auto startWall=Clock::now();
      if(watchdog) {
        expect(call(request("body_heartbeat",actor))["accepted"].GetBool(),"far watchdog fixture starts a fresh heartbeat deadline");
        BodyClock::advance(std::chrono::milliseconds(500-20));
        clock.pumpWatchdogs();
        expect(BodyController::owns(actor),"far watchdog fixture retains ownership before the deadline");
        BodyClock::advance(std::chrono::milliseconds(40)); // 500 + 20 ms since heartbeat.
        clock.pumpWatchdogs();
        } else {
        expect(call(request("body_release",actor))["accepted"].GetBool(),"far explicit release accepted");
        }
      step(player);
      const bool ownedNextTick=BodyController::owns(actor);
      // Keep collecting all four combinations on a broken engine, including the state
      // beyond both reported two- and five-second release deadlines.
      for(unsigned i=0;i<152 && BodyController::owns(actor);++i) {
        step(player);
        if(i==60) printState("after_2s",cause,policy);
        }
      events=parse(BodyController::drainJson());
      bool cancelled=false,restored=false,reasonCorrect=false;
      uint64_t elapsed=actor.world().tickCount()-startGame;
      for(const auto& event:events["events"].GetArray()) {
        if(std::string_view(event["phase"].GetString())=="cancelled") cancelled=true;
        if(std::string_view(event["reason"].GetString())=="native_restored") {
          restored=true; elapsed=event["t_game_ms"].GetUint64()-startGame;
          reasonCorrect=std::string_view(event["release_reason"].GetString())==cause;
          }
        }
      const auto elapsedWall=std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-startWall).count();
      printState("after_handoff",cause,policy);
      std::cerr << "far-release-fixture result cause=" << cause << " action=" << uint64_t(policy)
                << " owned_next_tick=" << ownedNextTick << " elapsed_game_ms=" << elapsed
                << " elapsed_wall_ms=" << elapsedWall << " native_restored=" << restored << '\n';
      check(!ownedNextTick && elapsed<=33,"far release returns native authority by the next engine tick");
      check(restored,"far release emits native_restored");
      check(reasonCorrect,"far native_restored retains the release cause");
      check(cancelled,"far release cancels its active command");
      const auto after=status(actor);
      check(!BodyController::owns(actor) && after["command_id"].GetUint64()==0,"far release clears active execution");
      check(queueEmptyAtRestore,"far release clears the body primitive queue before native AI resumes");
      check(poseCleanAtRestore,"far release clears its protected pose and combat waits before native AI resumes");
      check(std::string_view(BodyController::updateWatch({},50))=="allowed","far release permits removing the former owner from watch");
      // Reset only after recording a stuck lease, so later combinations remain runnable.
      BodyController::reset("far_fixture_cleanup");
      }
    }
  hero.setPosition(heroStart); hero.updateTransform();
  }

void heroTargetRelease(Npc& actor,Npc& hero,PlayerControl& player,std::vector<std::string>& failures) {
  const ManualBodyClock clock;
  using Clock=std::chrono::steady_clock;
  const auto heroStart=hero.position();
  const auto heroDirection=hero.rotation();
  const bool heroExternal=hero.isExternalControl();
  const auto heroHp=hero.attribute(ATR_HITPOINTS);
  struct Case final { FightAlgo::PolicyAction action; float distance,yaw; bool watchdog=false; };
  const std::array<Case,8> fixtures{{
    {FightAlgo::PolicyAction::Approach,800,0},
    {FightAlgo::PolicyAction::TurnToTarget,300,30},
    {FightAlgo::PolicyAction::StrafeLeft,300,0},
    {FightAlgo::PolicyAction::StrafeRight,300,0},
    {FightAlgo::PolicyAction::JumpBack,150,0},
    {FightAlgo::PolicyAction::AttackFront,150,0},
    {FightAlgo::PolicyAction::Block,150,0},
    {FightAlgo::PolicyAction::AttackFront,150,0,true},
    }};
  const auto check=[&](bool condition,std::string_view message) {
    ++assertions;
    if(!condition) failures.emplace_back(message);
    };
  const auto printState=[&](const char* phase,const char* cause,FightAlgo::PolicyAction policy) {
    auto row=status(actor);
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    row.Accept(writer);
    std::cerr << "hero-target-release-fixture phase=" << phase << " cause=" << cause
              << " action=" << uint64_t(policy) << " pose=" << actor.animNames()
              << " hero_distance_sq=" << actor.qDistTo(hero) << " status=" << buffer.GetString() << '\n';
    };
  for(const auto& fixture:fixtures) {
    ++cases;
    const char* cause=fixture.watchdog ? "watchdog_timeout" : "released";
    actor.setExternalControl(true);
    // fixturePrepare changes the target's transient pose/queues/control, not its
    // equipment or attributes. Here the real player is the actual leased enemy.
    BodyController::fixturePrepare(actor,hero,fixture.distance,fixture.yaw);
    step(player);
    expect(actor.processPolicy()==NpcProcessPolicy::AiNormal,"hero target fixture starts nearby");
    actor.setExternalControl(false);
    initialWatch(actor,400+uint64_t(fixture.watchdog)*20+uint64_t(fixture.action));
    expect(acquire(actor,0,"realtime",fixture.watchdog ? 500 : 5000)["accepted"].GetBool(),"hero target fixture acquired realtime lease");
    step(player,2);
    expect(actor.target()==&hero,"hero is the actual leased target");
    expect(action(actor,fixture.action)["accepted"].GetBool(),"hero target fixture action accepted");
    step(player);
    auto events=parse(BodyController::drainJson());
    bool started=false;
    for(const auto& event:events["events"].GetArray())
      if(std::string_view(event["phase"].GetString())=="started" &&
         event["action_id"].GetUint64()==uint64_t(fixture.action)) started=true;
    expect(started,"hero target fixture physically starts its action");
    printState("started",cause,fixture.action);
    BodyController::drainJson();
    const auto startGame=actor.world().tickCount();
    const auto startWall=Clock::now();
    // Release before the next controller tick, so target_visibility_lost cannot
    // replace the explicit release/watchdog cause while the real enemy escapes.
    hero.setPosition(actor.position()+Tempest::Vec3(9000,0,0)); hero.updateTransform();
    printState("far_before_request",cause,fixture.action);
    if(fixture.watchdog) {
      expect(call(request("body_heartbeat",actor))["accepted"].GetBool(),"hero target watchdog fixture starts a fresh deadline");
      BodyClock::advance(std::chrono::milliseconds(500-20));
      clock.pumpWatchdogs();
      expect(BodyController::owns(actor),"hero target watchdog fixture retains ownership before the deadline");
      BodyClock::advance(std::chrono::milliseconds(40)); // 500 + 20 ms since heartbeat.
      clock.pumpWatchdogs();
      } else {
      expect(call(request("body_release",actor))["accepted"].GetBool(),"hero target release accepted");
      }
    step(player);
    const bool ownedNextTick=BodyController::owns(actor);
    for(unsigned i=0;i<152 && BodyController::owns(actor);++i) {
      step(player);
      if(i==60) printState("after_2s",cause,fixture.action);
      }
    events=parse(BodyController::drainJson());
    bool restored=false,reasonCorrect=false;
    uint64_t elapsed=actor.world().tickCount()-startGame;
    for(const auto& event:events["events"].GetArray())
      if(std::string_view(event["reason"].GetString())=="native_restored") {
        restored=true; elapsed=event["t_game_ms"].GetUint64()-startGame;
        reasonCorrect=std::string_view(event["release_reason"].GetString())==cause;
        }
    printState("after_handoff",cause,fixture.action);
    std::cerr << "hero-target-release-fixture result cause=" << cause << " action=" << uint64_t(fixture.action)
              << " owned_next_tick=" << ownedNextTick << " elapsed_game_ms=" << elapsed
              << " elapsed_wall_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-startWall).count()
              << " native_restored=" << restored << '\n';
    check(actor.processPolicy()==NpcProcessPolicy::AiFar2,"escaping hero crosses both native simulation radii");
    check(!ownedNextTick && elapsed<=33,"hero target release returns native authority by the next engine tick");
    check(restored && reasonCorrect,"hero target release emits native_restored with its original cause");
    check(std::string_view(BodyController::updateWatch({},50))=="allowed","hero target release permits removing its former body owner");
    BodyController::reset("hero_target_fixture_cleanup");
    }
  hero.setPosition(heroStart); hero.setDirection(heroDirection); hero.updateTransform();
  hero.setExternalControl(heroExternal);
  hero.changeAttribute(ATR_HITPOINTS,heroHp-hero.attribute(ATR_HITPOINTS),false);
  }

void staleFarRelease(Npc& actor,Npc& target,Npc& hero,PlayerControl& player) {
  const ManualBodyClock clock;
  ++cases;
  const auto heroStart=hero.position();
  actor.setExternalControl(true);
  hero.setPosition(actor.position()+Tempest::Vec3(0,0,800)); hero.updateTransform();
  step(player);
  BodyController::fixturePrepare(actor,target,150,0);
  actor.setExternalControl(false);
  initialWatch(actor,500);
  expect(acquire(actor,0,"realtime")["accepted"].GetBool(),"stale far fixture acquired");
  step(player,2);
  BodyController::drainJson();
  const auto attack=action(actor,FightAlgo::PolicyAction::AttackFront);
  step(player);
  const auto attackEvents=parse(BodyController::drainJson());
  bool segmentStarted=false;
  for(const auto& event:attackEvents["events"].GetArray()) {
    std::cerr << "stale-far-release-fixture command=" << event["command_id"].GetUint64()
              << " phase=" << event["phase"].GetString() << " reason=" << event["reason"].GetString() << '\n';
    if(std::string_view(event["phase"].GetString())=="started" &&
       std::string_view(event["reason"].GetString())=="segment_started") segmentStarted=true;
    }
  expect(attack["accepted"].GetBool(),"stale far fixture attack accepted");
  expect(actor.bodyObservationState().segmentRemainingMs>33,"stale far fixture starts a protected attack");
  expect(segmentStarted,"stale far fixture command starts an actual combat segment");
  const auto nearHero=hero.position();
  hero.setPosition(actor.position()+Tempest::Vec3(9000,0,0)); hero.updateTransform();
  step(player);
  expect(actor.processPolicy()==NpcProcessPolicy::AiFar2 && BodyController::owns(actor),"stale far fixture enters far policy under lease");
  hero.setPosition(nearHero); hero.updateTransform();
  const auto remaining=actor.bodyObservationState().segmentRemainingMs;
  expect(remaining>33 && actor.qDistTo(hero)<3000.f*3000.f,"hero returns near before protected attack ends");
  const auto start=actor.world().tickCount();
  BodyController::drainJson();
  expect(call(request("body_release",actor))["accepted"].GetBool(),"stale far release accepted");
  expect(BodyController::owns(actor),"stale far policy cannot interrupt a protected attack beside the hero");
  for(unsigned i=0;i<46 && BodyController::owns(actor);++i) step(player);
  const auto events=parse(BodyController::drainJson());
  bool restored=false;
  uint64_t elapsed=actor.world().tickCount()-start;
  for(const auto& event:events["events"].GetArray())
    if(std::string_view(event["reason"].GetString())=="native_restored") {
      restored=true; elapsed=event["t_game_ms"].GetUint64()-start;
      }
  std::cerr << "stale-far-release-fixture segment_ms=" << remaining
            << " elapsed_game_ms=" << elapsed << " native_restored=" << restored << '\n';
  expect(restored && !BodyController::owns(actor),"stale far release confirms native authority");
  expect(elapsed>=remaining && elapsed<=1500,"stale far release preserves the nearby segment and finishes within 1.5 game seconds");
  expect(std::string_view(BodyController::updateWatch({},50))=="allowed","stale far release permits removing its former body owner");
  hero.setPosition(heroStart); hero.updateTransform();
  }

void meleeRemoval(Npc& victim) {
  auto& gothic=Gothic::inst();
  auto& world=victim.world();
  Settings::setI("GAME","melee_blade_shadow",1);
  Settings::setI("GAME","melee_physics_hits",1);
  const auto attackerSymbol=world.script().findSymbolIndex("BDT_1003_BANDIT_M");
  const auto sword=world.script().findSymbolIndex("ItMw_1h_Common_01");
  const auto perception=world.script().findSymbolIndex("ZS_ATTACK");
  expect(attackerSymbol!=size_t(-1) && sword!=size_t(-1) && perception!=size_t(-1),"melee removal stock symbols exist");
  for(unsigned phase=0;phase<3;++phase) {
    ++cases;
    victim.clearAiQueue(); victim.clearState(true); victim.clearGoTo(); victim.stopWalking();
    victim.closeWeapon(true); victim.setExternalControl(false);
    victim.handle().attribute[ATR_HITPOINTS]=victim.handle().attribute[ATR_HITPOINTSMAX]=5000;
    victim.setPerceptionEnable(PERC_ASSESSDAMAGE,perception);
    auto* attacker=world.addNpc(attackerSymbol,victim.position()+Tempest::Vec3(150,0,0));
    expect(attacker!=nullptr,"melee removal creates a real attacker");
    attacker->clearAiQueue(); attacker->clearState(true); attacker->clearGoTo(); attacker->stopWalking();
    attacker->setExternalControl(true); attacker->setToFightMode(sword); attacker->setTarget(&victim);
    expect(attacker->playAnimByName("S_1HATTACK",BS_HIT)!=nullptr,"melee removal starts a stock attack");
    attacker->updateAnimation(0,true); attacker->updateMeleeShadow();
    if(phase==2) {
      // Retain the old pending window while the existing fixture starts a
      // different attack; sync must not resurrect it after callback removal.
      BodyController::fixturePrepare(*attacker,victim,150,0);
      victim.setExternalControl(false);
      expect(attacker->playAnimByName("T_1HATTACKL",BS_HIT)!=nullptr,"melee removal starts the next stock attack");
      }
    // Use the production contact queue, with geometry above the map so this
    // lifecycle regression does not depend on combat positioning or scenery.
    const uint64_t now=world.tickCount();
    world.physic()->updateShadowBody(&victim,{{{10,100000,0},{10,100020,0},1.f,0,1}});
    world.physic()->updateShadowBlade(attacker,1,{{{0,100000,0},{0,100020,0},{0,0,1},1.f}},now,0,double(now+34));
    world.physic()->updateShadowBlade(attacker,1,{{{20,100000,0},{20,100020,0},{0,0,1},1.f}},now+33,0,double(now+34));
    world.physic()->flushShadowBlades();
    const auto hp=victim.attribute(ATR_HITPOINTS);
    unsigned callbacks=0;
    gothic.interceptPerception=[&](Npc& self,Npc& other,Npc*,PercType type,const HeroDeed*) {
      if(&self==&victim && &other==attacker && type==PERC_ASSESSDAMAGE) {
        ++callbacks;
        world.removeNpc(*attacker);
        }
      return true;
      };
    if(phase==1)
      attacker->resolveMeleeContacts();
    else if(phase==2)
      attacker->updateMeleeShadow();
    else
      world.removeNpc(*attacker);
    gothic.interceptPerception={};
    expect(world.npcId(attacker)==uint32_t(-1),"melee callback/removal takes exactly the attacking NPC out of the world");
    expect(callbacks==(phase==0 ? 0u : 1u),"only live contact resolution can run the damage perception");
    attacker->resolveMeleeContacts();
    attacker->finishMeleeShadowSwing();
    expect(world.physic()->takeShadowBladeContacts(attacker).empty(),"removed attacker retains no blade contacts");
    std::ifstream journal("melee-blade-shadow.jsonl");
    std::string line,last;
    while(std::getline(journal,line)) if(!line.empty()) last=line;
    const auto row=parse(last);
    expect(row["physical"]["targets"].Size()==1,"removal journals its contact once without opening another window");
    if(phase==0) {
      expect(victim.attribute(ATR_HITPOINTS)==hp,"removing an attacker must not apply its pending blade damage");
      expect(std::string_view(row["physical"]["targets"][0]["result"].GetString())=="discarded",
             "removal must journal its pending body contact as discarded");
      }
    }
  }
}

namespace {
// The melee shadow body of npc posed now, the way the shadow loop always posed it in place.
MeleeShadowBody posedShadowBody(const Npc& npc) {
  std::vector<HitZones::Posed> zones;
  npc.hitZonesPosed(zones);
  MeleeShadowBody body;
  for(const auto& capsule:zones)
    body.capsules.push_back({capsule.a,capsule.b,capsule.r,capsule.bone,uint8_t(capsule.zone)});
  DynamicWorld::boundShadowBody(body);
  return body;
  }

bool sameShadowBody(const MeleeShadowBody& a, const MeleeShadowBody& b) {
  const auto bits=[](const Tempest::Vec3& v) { return std::array<float,3>{v.x,v.y,v.z}; };
  if(a.capsules.size()!=b.capsules.size() ||
     std::memcmp(bits(a.boundsMin).data(),bits(b.boundsMin).data(),sizeof(float)*3)!=0 ||
     std::memcmp(bits(a.boundsMax).data(),bits(b.boundsMax).data(),sizeof(float)*3)!=0)
    return false;
  for(size_t i=0;i<a.capsules.size();++i) {
    const auto& x=a.capsules[i]; const auto& y=b.capsules[i];
    if(std::memcmp(bits(x.from).data(),bits(y.from).data(),sizeof(float)*3)!=0 ||
       std::memcmp(bits(x.to).data(),bits(y.to).data(),sizeof(float)*3)!=0 ||
       std::memcmp(&x.radiusCm,&y.radiusCm,sizeof(float))!=0 || x.bone!=y.bone || x.zone!=y.zone)
      return false;
    }
  return true;
  }

// A damage script inside the melee shadow loop of WorldObjects::updateAnimation spawns
// an NPC, removes the attacker (published this frame) and an NPC behind it (not yet
// published) and moves another one. Bodies are posed in the parallel pass; the loop must
// publish what posing in place gives, and removed owners must leave the published set.
void meleeShadowLoopChanges(Npc& victim) {
  ++cases;
  auto& gothic=Gothic::inst();
  auto& world=victim.world();
  const auto bandit=world.script().findSymbolIndex("BDT_1003_BANDIT_M");
  const auto sword=world.script().findSymbolIndex("ItMw_1h_Common_01");
  const auto perception=world.script().findSymbolIndex("ZS_ATTACK");
  expect(bandit!=size_t(-1) && sword!=size_t(-1) && perception!=size_t(-1),"melee loop stock symbols exist");
  victim.clearAiQueue(); victim.clearState(true); victim.clearGoTo(); victim.stopWalking();
  victim.closeWeapon(true); victim.setExternalControl(false);
  victim.handle().attribute[ATR_HITPOINTS]=victim.handle().attribute[ATR_HITPOINTSMAX]=5000;
  victim.setPerceptionEnable(PERC_ASSESSDAMAGE,perception);
  // Fighters with a target are animated every frame, outside the animation LOD.
  auto fighter=[&](const Tempest::Vec3& offset) {
    auto* npc=world.addNpc(bandit,victim.position()+offset);
    expect(npc!=nullptr,"melee loop creates a real NPC");
    npc->clearAiQueue(); npc->clearState(true); npc->clearGoTo(); npc->stopWalking();
    npc->setExternalControl(true); npc->setToFightMode(sword); npc->setTarget(&victim);
    return npc;
    };
  auto* attacker=fighter({150,0,0});
  expect(attacker->playAnimByName("S_1HATTACK",BS_HIT)!=nullptr,"melee loop starts a stock attack");
  attacker->updateAnimation(0,true); attacker->updateMeleeShadow();
  BodyController::fixturePrepare(*attacker,victim,150,0);
  victim.setExternalControl(false);
  expect(attacker->playAnimByName("T_1HATTACKL",BS_HIT)!=nullptr,"melee loop starts the next stock attack");
  auto* bystander=fighter({-150,0,0});
  auto* mover=fighter({0,0,150});
  for(auto* npc:{bystander,mover}) {
    npc->updateAnimation(0,true); npc->updateMeleeShadow();
    }
  expect(world.npcId(attacker)<world.npcId(bystander) && world.npcId(bystander)<world.npcId(mover),
         "the removed and the moved NPC come after the attacker in the shadow loop");
  // The pending contact the attacker's next shadow update retires, as in meleeRemoval.
  const uint64_t now=world.tickCount();
  world.physic()->updateShadowBody(&victim,{{{10,100000,0},{10,100020,0},1.f,0,1}});
  world.physic()->updateShadowBlade(attacker,1,{{{0,100000,0},{0,100020,0},{0,0,1},1.f}},now,0,double(now+34));
  world.physic()->updateShadowBlade(attacker,1,{{{20,100000,0},{20,100020,0},{0,0,1},1.f}},now+33,0,double(now+34));
  world.physic()->flushShadowBlades();
  Npc* newcomer=nullptr;
  MeleeShadowBody beforeMove;
  unsigned callbacks=0;
  gothic.interceptPerception=[&](Npc& self,Npc& other,Npc*,PercType type,const HeroDeed*) {
    if(&self==&victim && &other==attacker && type==PERC_ASSESSDAMAGE && callbacks++==0) {
      beforeMove=posedShadowBody(*mover);
      mover->setPosition(mover->position()+Tempest::Vec3(40,0,0));
      mover->updateTransform();
      world.removeNpc(*bystander);
      world.removeNpc(*attacker);
      newcomer=world.addNpc(bandit,victim.position()+Tempest::Vec3(0,0,-150));
      }
    return true;
    };
  Gothic::inst().gameSession()->updateAnimation(33);
  gothic.interceptPerception={};
  expect(callbacks==1 && newcomer!=nullptr,"the damage script ran once inside the shadow loop");
  MeleeShadowBody published;
  expect(!world.physic()->shadowBody(attacker,published) && !world.physic()->shadowBody(bystander,published),
         "NPCs removed inside the shadow loop leave the published bodies");
  expect(world.physic()->shadowBody(newcomer,published) && sameShadowBody(published,posedShadowBody(*newcomer)),
         "an NPC spawned inside the shadow loop publishes its body posed in place");
  expect(world.physic()->shadowBody(mover,published) && sameShadowBody(published,posedShadowBody(*mover)) &&
         !sameShadowBody(published,beforeMove),
         "an NPC moved inside the shadow loop publishes its moved body, not the parallel pass's");
  world.removeNpc(*newcomer);
  world.removeNpc(*mover);
  }

// A frozen world (OPENGOTHIC_FREEZE_WORLD) through the production rules on real NPCs: the animation pass and the
// shadow loop keep the hero and the arena actors and pass any other NPC over; a passed-over NPC's tail plan has
// no shadow step, a kept one asked for every body has it (world/animationtail.h).
void frozenArenaTail(Npc& bystander) {
  ++cases;
  auto& world=bystander.world();
  auto* hero=Gothic::inst().player();
  expect(hero!=nullptr,"frozen arena pin has a hero");
  BodyArena::configure(BodyArena::Config{},1);
  BodyArena::setup(world,*hero);
  const auto actors=BodyArena::actors();
  expect(actors.size()==2,"frozen arena pin sets up two real actors");
  for(auto* actor:actors) {
    expect(BodyArena::isActor(*actor),"an arena actor is an actor");
    expect(!WorldObjects::animationPassSkips(*actor,hero,false,true),"a frozen world keeps an arena actor in the pass");
    expect((actor->animationTailPlan(false,true)&AnimationTail::Shadow)!=0,"a kept actor's plan has its shadow step");
    }
  expect(!WorldObjects::animationPassSkips(*hero,hero,false,true),"a frozen world keeps the hero in the pass");
  expect(!BodyArena::isActor(bystander) && WorldObjects::animationPassSkips(bystander,hero,false,true),
         "a frozen world passes a bystander over");
  expect(!WorldObjects::animationPassSkips(bystander,hero,false,false),"a live world keeps the bystander");
  expect((bystander.animationTailPlan(true,true)&AnimationTail::Shadow)==0,"a passed-over NPC's plan has no shadow step");
  for(auto* actor:actors)
    world.removeNpc(*actor);
  BodyArena::disable();
  }
}

// Uses a fresh full session, so unsaved Pose speed overrides cannot survive in
// the object being tested. The restored native body is the canonical reference.
class NpcLogicalStateContract final {
  static constexpr uint64_t tickMs=33;

  static void advance(uint64_t dt=tickMs) {
    auto& session=*Gothic::inst().gameSession();
    session.tick(dt);
    session.updateAnimation(dt);
    }

  static uint64_t predictedEnd(const Pose::CombatLayer& layer,uint16_t combo) {
    if(layer.sequence->animCls==Animation::Loop) return 0;
    const double duration=std::min(layer.sequence->atkTotalTime(combo),layer.sequence->totalTime());
    return layer.start+uint64_t(std::ceil((std::floor(duration)+1.)/double(layer.speed)));
    }

  static void compare(const Npc& npc) {
    const auto now=npc.world().tickCount();
    const auto& logical=npc.logicalBodyState();
    expect(logical.body==npc.bodyState(),"restored logical body equals the canonical native body");
    expect(logical.weapon==npc.weaponState(),"restored logical weapon equals the canonical native weapon");
    expect(logical.bodyStateMasked()==npc.bodyStateMasked(),"restored logical body mask equals the native mask");
    const auto real=npc.visual.pose().combatLayers(now);
    expect(logical.layers.size()==real.size(),"restored logical clock covers every native layer");
    for(const auto& layer:real) {
      const auto found=std::find_if(logical.layers.begin(),logical.layers.end(),[&](const auto& shadow) {
        return shadow.sequence==layer.sequence && shadow.origin==layer.origin;
        });
      expect(found!=logical.layers.end(),"restored logical layer retains canonical sequence and origin");
      expect(found->start==layer.start && found->speed==layer.speed && found->state==layer.state,
             "restored logical layer retains canonical start, speed and state");
      expect(found->expectedEnd==predictedEnd(layer,npc.visual.pose().comboLength()),
             "restored logical layer deadline comes from its own native sequence metadata");
      expect(std::abs(logical.animProgress(layer.sequence->name,now)-npc.animProgress(layer.sequence->name))<.00001f,
             "restored logical progress equals the native progress");
      expect(logical.hasState(layer.state)==npc.hasState(layer.state),"restored logical hasState equals native hasState");
      }
    }

  public:
    static void preTickOrder(Npc& anchor) {
      ++cases;
      auto& world=anchor.world();
      auto& gothic=Gothic::inst();
      const auto kind=world.script().findSymbolIndex("BDT_1003_BANDIT_M");
      const auto hero=world.player();
      auto make=[&](NpcSimTier tier,const Tempest::Vec3& offset) {
        auto* npc=world.addNpc(kind,hero->position()+offset);
        npc->clearAiQueue(); npc->clearState(true); npc->clearGoTo(); npc->stopWalking();
        npc->closeWeapon(true); npc->setTarget(nullptr); npc->setExternalControl(true);
        npc->visual.pose().stopAllAnim(); npc->setAnim(Npc::Anim::Idle);
        npc->setSimTierOverride(tier);
        npc->setSimTier(tier); npc->updateSimpleMovement();
        // The pre-tick pass must replace this deliberately wrong tier before either actor ticks.
        npc->setSimTier(tier==NpcSimTier::Full ? NpcSimTier::Simplified : NpcSimTier::Full);
        npc->waitTime=npc->aniWaitTime=npc->outWaitTime=npc->faiWaitTime=0;
        return npc;
        };
      auto* first=make(NpcSimTier::Simplified,{100,0,0});
      auto* fighter=make(NpcSimTier::Full,{200,0,0});
      first->handle().id=std::numeric_limits<int32_t>::min()+1;
      fighter->handle().id=std::numeric_limits<int32_t>::min()+2;
      expect(world.npcById(0)!=first,"order fixture starts with an unsorted NPC array");
      fighter->setToFightMode(world.script().findSymbolIndex("ItMw_1h_Common_01")); fighter->setTarget(first);
      expect(fighter->startState(world.script().findFunction("ZS_ATTACK"),""),"order fixture enters the native attack state");
      expect(fighter->playAnimByName("S_1HATTACK",BS_HIT)!=nullptr && fighter->isAttack(),"order fixture has a real attack");
      first->meleeFighter={}; fighter->meleeFighter={};
      // Graph locomotion keeps the observer out of native gravity before its queued output callback.
      first->setExternalControl(false);
      first->handle().voice=14;
      first->aiPush(AiQueue::aiOutputSvm(*hero,"$NOTNOW",0));
      unsigned outputs=0; bool marked=false, tiers=false, sorted=false;
      gothic.interceptSvm=[&](Npc& self,std::string_view,std::string_view,Npc*,bool) {
        if(&self==first) {
          ++outputs;
          marked=first->isMeleeFighter() && fighter->isMeleeFighter();
          tiers=first->simTier()==NpcSimTier::Simplified && fighter->simTier()==NpcSimTier::Full;
          sorted=world.npcById(0)==first && world.npcById(1)==fighter;
          }
        return true;
        };
      world.tick(33);
      gothic.interceptSvm={};
      expect(outputs==1 && marked,"all melee fighters are refreshed before the first NPC tick callback: outputs="+
             std::to_string(outputs)+" marked="+std::to_string(marked));
      expect(tiers && sorted,"simulation tiers and handle order are established before the first NPC tick callback");
      world.removeNpc(*first); world.removeNpc(*fighter);
      }

    // Frame steps of the far tick cases: random 7/8 or 6..10 ms from a fixed seed, the same on every platform.
    static uint64_t farTickStep(uint64_t seed, uint64_t frame, uint64_t from, uint64_t count) {
      uint64_t z=seed+frame+0x9e3779b97f4a7c15ull;
      z=(z^(z>>30))*0xbf58476d1ce4e5b9ull;
      z=(z^(z>>27))*0x94d049bb133111ebull;
      return from+(z^(z>>31))%count;
      }

    // The far tick rule itself on 728 NPC states, without game objects: at most 40 % of them tick in a frame
    // at 8 ms, after a 33 ms frame and at random 7/8 and 6..10 ms steps, and the ticks hand over all the time.
    static void farTickRule() {
      ++cases;
      constexpr uint64_t Period=33;
      constexpr size_t   States=728;
      std::vector<NpcSimTick::State> state(States);
      std::vector<uint64_t> phase(States), given(States,0);
      for(size_t i=0;i<States;++i) phase[i]=NpcSimTick::phase(uint32_t(i),Period);
      uint64_t clock=0;
      const auto frame=[&](uint64_t dt) {
        clock+=dt;
        size_t ticked=0;
        for(size_t i=0;i<States;++i) {
          const auto owed=state[i].pending+dt;
          if(NpcSimTick::due(clock,dt,owed,phase[i],Period)) { given[i]+=owed; state[i].pending=0; ++ticked; }
          else state[i].pending=owed;
          }
        return ticked;
        };
      const std::array<std::pair<const char*,std::function<uint64_t(uint64_t)>>,4> blocks{{
        {"8 ms",[](uint64_t) { return uint64_t(8); }},
        {"8 ms after a 33 ms frame",[](uint64_t f) { return f==0 ? Period : 8; }},
        {"random 7/8 ms",[](uint64_t f) { return farTickStep(1,f,7,2); }},
        {"random 6..10 ms",[](uint64_t f) { return farTickStep(2,f,6,5); }},
        }};
      for(const auto& [name,step]:blocks) {
        size_t most=0;
        for(uint64_t f=0;f<2001;++f) {
          const auto dt=step(f);
          const auto ticked=frame(dt);
          if(dt<Period) most=std::max(most,ticked);
          }
        expect(most*10<=States*4,std::string("the far tick rule spreads 728 NPCs at ")+name+": at most 40 % in a frame, got "+
               std::to_string(most));
        }
      for(size_t i=0;i<States;++i)
        expect(given[i]+state[i].pending==clock,"the far tick rule hands every NPC the world time");
      }

    // Far NPC tick (world/npcsimtick.h) on real standing NPCs through WorldObjects::tick. What a tick really got
    // is measured by its effect: regeneration of 1000 hit points a second gives one hit point per ms of every
    // Npc::tick (Npc::tickRegen); the time still owed is Npc::simTick().pending.
    static void farTick(Npc& anchor) {
      auto& world=anchor.world();
      const auto kind=world.script().findSymbolIndex("BDT_1003_BANDIT_M");
      const auto restore=NpcSimTick::config();
      // 96: with fewer, the Fibonacci hash of consecutive numbers is not even enough for a third at 10 ms steps.
      constexpr size_t Count=96;
      constexpr uint64_t Period=33;
      std::vector<Npc*> npcs;
      for(size_t i=0;i<Count;++i) {
        // 45 m from the hero: no perception and no living body. Made in a row: consecutive creation numbers.
        auto* npc=world.addNpc(kind,world.player()->position()+Tempest::Vec3(float(i%16)*150.f,0,4500+float(i/16)*150.f));
        npc->clearAiQueue(); npc->clearState(true); npc->clearGoTo(); npc->stopWalking();
        npc->closeWeapon(true); npc->setTarget(nullptr); npc->setExternalControl(true);
        npc->visual.pose().stopAllAnim(); npc->setAnim(Npc::Anim::Idle);
        npc->setSimTierOverride(NpcSimTier::Simplified);
        npc->handle().attribute[ATR_HITPOINTSMAX]=1000000;
        npc->handle().attribute[ATR_HITPOINTS]=1;
        npc->handle().attribute[ATR_REGENERATEHP]=1000;
        npcs.push_back(npc);
        }
      struct Track { std::vector<uint64_t> frame, dt; };
      std::vector<Track> track(Count);
      uint64_t frames=0, worldMs=0;
      const auto hp=[](const Npc* npc) { return uint64_t(npc->attribute(ATR_HITPOINTS)); };
      // One world tick: how many of the NPCs ticked; each tick must get, once, exactly the time its NPC waited.
      const auto frame=[&](uint64_t dt) {
        std::vector<uint64_t> waited(Count), before(Count);
        for(size_t i=0;i<Count;++i) { waited[i]=npcs[i]->simTick().pending; before[i]=hp(npcs[i]); }
        world.tick(dt);
        ++frames; worldMs+=dt;
        size_t ticked=0;
        for(size_t i=0;i<Count;++i) {
          const auto got=hp(npcs[i])-before[i];
          if(npcs[i]->simTick().pending>0) {
            expect(npcs[i]->simTick().pending==waited[i]+dt && got==0,
                   "a waiting NPC gets no time and keeps the frame's for its next tick");
            continue;
            }
          expect(got==waited[i]+dt && npcs[i]->logicalBodyState().worldStep==got,
                 "a far tick gets, once, all the world time since the NPC's previous tick: got "+std::to_string(got)+
                 ", owed "+std::to_string(waited[i]+dt));
          track[i].frame.push_back(frames); track[i].dt.push_back(got);
          ++ticked;
          }
        return ticked;
        };
      const auto clear=[&] { for(auto& t:track) { t.frame.clear(); t.dt.clear(); } };
      const auto everyFrame=[&](uint64_t step,unsigned count,const char* what) {
        for(unsigned f=0;f<count;++f) {
          clear();
          expect(frame(step)==Count,std::string(what)+" ticks every far NPC every frame");
          for(const auto& t:track) expect(t.dt.size()==1 && t.dt[0]==step,std::string(what)+" gives a far NPC the frame's step");
          }
        };

      // Constant 8 ms: after the first tick steps of 32 or 40 ms, 303 +- 1 ticks in 10 s, a third at most in a frame.
      ++cases;
      NpcSimTick::setConfig({Period,NpcSimTick::Scope::Graph});
      size_t most=0;
      std::vector<size_t> inTenSeconds(Count);
      for(unsigned f=0;f<2000;++f) {
        most=std::max(most,frame(8));
        if(f==0)
          for(auto* npc:npcs)
            expect(NpcSimTick::qualifies(*npc,NpcSimTick::Scope::Graph),"the far tick fixture stands in graph movement");
        if(frames==1250)
          for(size_t i=0;i<Count;++i) inTenSeconds[i]=track[i].dt.size();
        }
      expect(most*3<=Count,"8 ms frames spread the far ticks: at most a third of the NPCs in a frame, got "+
             std::to_string(most)+" of "+std::to_string(Count));
      for(size_t i=0;i<Count;++i) {
        const auto& t=track[i];
        expect(inTenSeconds[i]>=302 && inTenSeconds[i]<=304,"a far NPC ticks 303 +- 1 times in 10 s of 8 ms frames, got "+
               std::to_string(inTenSeconds[i]));
        for(size_t k=1;k<t.dt.size();++k)
          expect(t.dt[k]==32 || t.dt[k]==40,"8 ms frames give a far NPC steps of 32 or 40 ms: "+std::to_string(t.dt[k]));
        }

      // The spread holds after a long frame and at random steps: the nodes of the grid do not move.
      const std::array<std::pair<const char*,std::function<uint64_t(uint64_t)>>,3> blocks{{
        {"8 ms after a 33 ms frame",[](uint64_t f) { return f==0 ? Period : 8; }},
        {"random 7/8 ms",[](uint64_t f) { return farTickStep(1,f,7,2); }},
        {"random 6..10 ms",[](uint64_t f) { return farTickStep(2,f,6,5); }},
        }};
      for(const auto& [name,step]:blocks) {
        ++cases;
        clear();
        most=0;
        for(uint64_t f=0;f<2001;++f) {
          const auto dt=step(f);
          const auto ticked=frame(dt);
          if(dt<Period) most=std::max(most,ticked);
          else expect(ticked==Count,"a 33 ms frame ticks every far NPC");
          }
        expect(most*3<=Count,std::string(name)+" spreads the far ticks: at most a third of the NPCs in a frame, got "+
               std::to_string(most)+" of "+std::to_string(Count));
        }

      // 25 and 30 ms frames, below the period: every frame by the 1.5-period limit, 25 ms at its edge (one more
      // frame would give 50 > 49.5 ms); at 24 ms an NPC off its node waits. Then 33, 34 and 50 ms.
      ++cases;
      expect(frame(25)==Count,"a 25 ms frame ticks every far NPC");
      everyFrame(25,10,"a 25 ms frame");
      everyFrame(30,10,"a 30 ms frame");
      size_t fewest=Count;
      for(unsigned f=0;f<10;++f) fewest=std::min(fewest,frame(24));
      expect(fewest<Count,"24 ms frames let a far NPC off its node wait");
      ++cases;
      expect(frame(Period)==Count,"a 33 ms frame ticks every far NPC");
      for(const uint64_t step:{uint64_t(33),uint64_t(34),uint64_t(50)})
        everyFrame(step,3,"a frame step not below the period");

      // An NPC that stops qualifying between its ticks ticks in that frame with all it waited, then every frame.
      ++cases;
      for(unsigned f=0;f<8;++f) frame(8); // back on the grid after the long frames
      Npc* fighter=nullptr;
      for(unsigned f=0;f<8 && fighter==nullptr;++f) {
        frame(8);
        for(auto* npc:npcs)
          if(npc->simTick().pending==8) fighter=npc; // ticked at its node one frame ago: the next is 33 ms on
        }
      expect(fighter!=nullptr,"a far NPC waits one frame after its tick");
      fighter->setToFistMode();
      expect(!NpcSimTick::qualifies(*fighter,NpcSimTick::Scope::Graph),"native combat movement leaves graph scope");
      frame(8);
      expect(fighter->simTick().pending==0 && fighter->logicalBodyState().worldStep==16,
             "an NPC that stops qualifying ticks in the same frame with the time it waited");
      for(unsigned f=0;f<3;++f) {
        frame(8);
        expect(fighter->simTick().pending==0 && fighter->logicalBodyState().worldStep==8,"an NPC that does not qualify ticks every frame");
        }
      fighter->closeWeapon(true);

      // The frames come from the creation number, not from the place in npcArr: at 8 ms the grid repeats every
      // 33 frames (264 ms), so a re-sorted npcArr ticks every NPC in the same frames of the next 33.
      ++cases;
      for(unsigned f=0;f<8;++f) frame(8);
      const auto window=[&] {
        clear();
        for(unsigned f=0;f<33;++f) frame(8);
        std::vector<std::vector<uint64_t>> at(Count);
        for(size_t i=0;i<Count;++i)
          for(const auto t:track[i].frame) at[i].push_back(t-(frames-33));
        return at;
        };
      const auto before=window();
      const auto place=world.npcId(npcs[0]);
      // Reversed and lowest: the pre-tick pass re-sorts npcArr before the next ticks.
      for(size_t i=0;i<Count;++i) npcs[i]->handle().id=std::numeric_limits<int32_t>::min()+int32_t(Count-i);
      const auto after=window();
      expect(world.npcId(npcs[0])!=place && world.npcId(npcs[0])>world.npcId(npcs[1]),"the phase fixture re-sorts npcArr");
      for(size_t i=0;i<Count;++i)
        expect(after[i]==before[i],"a far NPC ticks in the frames of its own phase, not of its place in npcArr");

      // Tier scope also takes native movement: a standing NPC in Fist without a target or a physics body, as the
      // monsters at rest, waits for its turn; graph scope ticks it every frame.
      ++cases;
      NpcSimTick::setConfig({Period,NpcSimTick::Scope::Tier});
      auto* native=npcs[0];
      const auto* ground=world.findWayPoint(native->position());
      expect(ground!=nullptr,"the tier fixture stands at a waypoint");
      native->setPosition(ground->groundPos); native->updateTransform(); // native movement stands on the ground
      native->setToFistMode();
      for(unsigned f=0;f<4;++f) frame(8);
      expect(native->weaponState()==WeaponState::Fist && native->target()==nullptr && !native->usesSimplifiedMovement() &&
             !native->hasRagdoll() && native->livingBody()==nullptr && NpcSimTick::qualifies(*native,NpcSimTick::Scope::Tier),
             "tier scope takes a standing Simplified NPC in Fist on its native path");
      bool waited=false;
      for(unsigned f=0;f<33;++f) { frame(8); waited|=native->simTick().pending>0; }
      expect(waited,"tier scope defers the ticks of a standing NPC in Fist");
      native->closeWeapon(true);

      // Period 0: every NPC every frame, as without the rule.
      ++cases;
      NpcSimTick::setConfig({0,NpcSimTick::Scope::Graph});
      frame(8);
      everyFrame(8,8,"period 0");
      for(auto* npc:npcs)
        expect(hp(npc)-1==worldMs,"the far ticks hand every NPC the world time, each ms once");
      NpcSimTick::setConfig(restore);
      for(auto* npc:npcs) world.removeNpc(*npc);
      }

    static void restoreStamina(Npc& actor) {
      MeleeStamina::reset(actor.meleeStamina,actor.staminaMax(),actor.world().tickCount());
      }

    static void meleeAi(Npc& actor,Npc& target) {
      if(!actor.hasAnim("T_1HDASHB")) {
        std::cerr << "melee AI fixture skipped: needs OPENGOTHIC_CONTENT with anim-strikes\n";
        return;
        }
      const std::array<std::pair<const char*,int>,5> settings{{
        {"melee_dodge",1},{"melee_ai_dodge",1},
        {"melee_stagger",1},{"melee_stamina",0},{"melee_parry_ai",0},
        }};
      std::array<int,settings.size()> saved{};
      for(size_t i=0;i<settings.size();++i) {
        saved[i]=Settings::getI("GAME",settings[i].first);
        Settings::setI("GAME",settings[i].first,settings[i].second);
        }
      const auto talent=actor.hitChance(TALENT_1H);
      const auto prepare=[&](float distance=150.f,float yaw=0.f) {
        for(auto* npc:{&actor,&target})
          npc->endMeleeStun("fixture");
        actor.meleeAiDodgeTiming.reset(); actor.meleeAiDodgeStreak.reset();
        BodyController::fixturePrepare(actor,target,distance,yaw);
        actor.setExternalControl(true);
        target.setTarget(&actor);
        target.setDirection(actor.position()-target.position()); target.updateTransform();
        };
      const auto queue=[&](FightAlgo::Action first,FightAlgo::Action second=FightAlgo::MV_NULL) {
        std::vector<uint8_t> bytes;
        {
          Tempest::MemWriter writer(bytes);
          Serialize out(writer);
          out.setEntry("fight");
          out.write(int32_t(0));
          for(int i=0;i<FightAlgo::MV_MAX;++i)
            out.write(uint8_t(i==0 ? first : i==1 ? second : FightAlgo::MV_NULL));
          out.write(false);
          }
        Tempest::MemReader reader(bytes);
        Serialize in(reader);
        expect(in.setEntry("fight"),"native primitive fixture archive entry exists");
        actor.fghAlgo.load(in);
        expect(actor.fghAlgo.queuedAction()==first,"native primitive fixture loaded the requested action");
        };
      prepare();

      ++cases;
      actor.handle().hitchance[TALENT_1H]=0;
      target.startMeleeStun(actor,"posture");
      expect(target.isMeleeStunned(),"native combo fixture has a stunned target");
      queue(FightAlgo::MV_ATTACKL,FightAlgo::MV_ATTACKR);
      actor.implAttack(tickMs);
      expect(actor.fghAlgo.queuedAction()==FightAlgo::MV_ATTACKR,
             "parry AI off preserves the native combo against a stunned target");

      for(const bool parade:{false,true}) {
        ++cases;
        prepare(300,parade ? 0.f : 90.f);
        if(parade) {
          expect(actor.blockSword() && actor.bodyStateMasked()==BS_PARADE,"jump-back guard fixture starts in parade");
          }
        queue(FightAlgo::MV_JUMPBACK);
        const auto at=actor.position();
        actor.implAttack(tickMs);
        expect(actor.fghAlgo.queuedAction()==FightAlgo::MV_NULL && !actor.isMeleeDodging() &&
               (actor.position()-at).quadLength()<.001f,
               "parade and off-focus jump backs are consumed without a dodge or movement");
        }

      ++cases;
      prepare(300);
      Settings::setI("GAME","melee_stamina",1);
      const auto now=actor.world().tickCount();
      actor.meleeStamina={0,now,now,true,true};
      queue(FightAlgo::MV_JUMPBACK);
      actor.implAttack(tickMs);
      for(unsigned i=0;i<34;++i) advance();
      actor.visual.pose().stopAllAnim(); actor.setAnim(Npc::Anim::Idle); actor.faiWaitTime=0;
      actor.implAttack(tickMs);
      expect(actor.fghAlgo.queuedAction()==FightAlgo::MV_JUMPBACK && actor.meleeAiDodgeTiming &&
             actor.meleeAiDodgeTiming->began==0 && !actor.isMeleeDodging(),
             "a no-swing dodge keeps waiting for stamina beyond 1000 ms without spending its primitive");
      Settings::setI("GAME","melee_stamina",0);

      ++cases;
      prepare();
      actor.setExternalControl(false);
      initialWatch(actor,699);
      expect(acquire(actor)["accepted"].GetBool(),"exhausted P30 fixture acquires its body lease");
      advance();
      BodyObservations::sampleAll(true);
      expect(action(actor,FightAlgo::PolicyAction::JumpBack)["accepted"].GetBool(),"exhausted P30 jump back is accepted");
      Settings::setI("GAME","melee_stamina",1);
      const auto exhaustedAt=actor.world().tickCount();
      actor.meleeStamina={0,exhaustedAt,exhaustedAt,true,true};
      BodyController::tick(actor,tickMs);
      bool jumpBack=false;
      for(const auto& layer:actor.visual.pose().combatLayers(actor.world().tickCount()))
        jumpBack |= MeleeDodge::isJumpBackAnimation(layer.sequence->name);
      expect(jumpBack && !actor.isMeleeDodging() && !actor.meleeAiDodgeTiming,
             "P30 at zero stamina starts its original MoveBack immediately");
      bool consumed=false;
      const auto events=parse(BodyController::drainJson());
      for(const auto& event:events["events"].GetArray())
        if(std::string_view(event["phase"].GetString())=="started")
          consumed |= event["primitive_consumed"].GetBool();
      expect(consumed,"P30 MoveBack consumes the primitive when the segment starts");
      BodyController::reset("melee_ai_fixture");
      BodyObservations::disable();
      Settings::setI("GAME","melee_stamina",0);

      for(unsigned attempt=0;attempt<3;++attempt) {
        ++cases;
        prepare();
        target.swingSword();
        const auto swing=target.visual.pose().nextMeleeContact(actor.world().tickCount());
        expect(swing.has_value(),"P30 dodge fixture has a real visible incoming swing");
        actor.meleeAiDodgeTiming=Npc::MeleeAiDodgeTiming{&target,swing->sequence,swing->origin,
            actor.world().tickCount(),swing->contact,0,{MeleeDodge::Kind::Dash,MeleeDodge::Stance::OneHand,MeleeDodge::Direction::Left}};
        actor.meleeAiDodgeStreak={2,actor.world().tickCount()};
        actor.implAttack(tickMs,true,FightAlgo::MV_JUMPBACK);
        expect(actor.isMeleeDodging() && !actor.meleeAiDodgeTiming && actor.meleeAiDodgeStreak.count==2,
               "P30 jump back starts immediately despite a swing and a full native dodge streak");
        bool backward=false;
        for(const auto& layer:actor.visual.pose().combatLayers(actor.world().tickCount()))
          if(const auto move=MeleeDodge::parseAnimation(layer.sequence->name)) {
            expect(move->direction==MeleeDodge::Direction::Back,"P30 jump back never inherits a native side dodge");
            backward=true;
            }
        expect(backward,"P30 jump back starts an actual backward dodge animation");
        }

      prepare(300);
      actor.handle().hitchance[TALENT_1H]=talent;
      actor.meleeStamina={};
      actor.setExternalControl(false);
      for(size_t i=0;i<settings.size();++i)
        Settings::setI("GAME",settings[i].first,saved[i]);
      }

    static void run(uint32_t id,const std::string& scenario,const std::filesystem::path& output) {
      auto& gothic=Gothic::inst();
      // Isolate canonical action clocks from unrelated ambient transitions.
      // The complete-world audit remains part of npc-day-parity.
      auto* subject=gothic.world()->npcById(id);
      TestBench::configure(scenario,output.string(),17);
      for(uint32_t i=gothic.world()->npcCount();i>0;--i)
        if(auto* npc=gothic.world()->npcById(i-1);npc!=nullptr && npc!=subject && !npc->isPlayer())
          gothic.world()->removeNpc(*npc);
      id=gothic.world()->npcId(subject);
      for(const auto tier:{NpcSimTier::Full,NpcSimTier::Simplified})
      for(const auto action:{AI_PlayAnim,AI_StandUp,AI_DrawWeaponMelee}) {
        ++cases;
        auto* npc=gothic.world()->npcById(id);
        expect(npc!=nullptr,"logical save/load fixture actor survives the preceding session reload");
        npc->setSimTierOverride(tier);
        npc->setSimTier(tier);
        npc->setExternalControl(true);
        npc->clearState(true);
        npc->clearAiQueue();
        npc->outWaitTime=0;
        npc->clearGoTo();
        npc->setTarget(nullptr);
        npc->clearSpeed();
        npc->handle().attribute[ATR_HITPOINTS]=npc->attribute(ATR_HITPOINTSMAX);
        npc->setInteraction(nullptr,true);
        npc->closeWeapon(true);
        npc->visual.pose().stopAllAnim();
        expect(npc->setAnim(Npc::Anim::Idle),"logical save/load fixture starts from native idle");
        for(unsigned i=0;i<5;++i) advance();
        npc->clearAiQueue();
        if(action==AI_PlayAnim) {
          expect(npc->hasAnim("T_STAND_2_PEE"),"logical save/load PlayAni uses a real stock transition");
          npc->aiPush(AiQueue::aiPlayAnim("T_STAND_2_PEE"));
          }
        else if(action==AI_StandUp) {
          npc->visual.pose().stopAllAnim();
          expect(npc->playAnimByName("S_WOUNDED",BS_LIE)!=nullptr,"logical save/load stand-up starts lying");
          npc->aiPush(AiQueue::aiStandup());
          }
        else {
          expect(npc->currentMeleeWeapon()!=nullptr,"logical save/load weapon draw has an equipped native weapon");
          npc->aiPush(AiQueue::aiReadyMeleeWeapon());
          }

        std::optional<Pose::CombatLayer> playing;
        for(unsigned i=0;i<90 && !playing;++i) {
          advance();
          for(const auto& layer:npc->visual.pose().combatLayers(npc->world().tickCount())) {
            if(layer.sequence->animCls!=Animation::Transition || layer.sequence->totalTime()<300.f)
              continue;
            if(action==AI_PlayAnim && layer.sequence->name!="T_STAND_2_PEE")
              continue;
            if(action==AI_StandUp && layer.sequence->name.find("WOUNDED_2_")==std::string::npos)
              continue;
            if(action==AI_DrawWeaponMelee &&
               std::none_of(layer.sequence->data->events.begin(),layer.sequence->data->events.end(),[](const auto& event) {
                 return event.type==asset::MdsEventType::SET_FIGHT_MODE;
                 }))
              continue;
            playing=layer;
            break;
            }
          }
        expect(playing.has_value(),"logical save/load admits the requested native body action");
        if(npc->usesSimplifiedMovement()) {
          const auto native=npc->visual.pose().combatLayers(npc->world().tickCount());
          expect(!npc->prepareTurn(),"a native body action retains ownership against the graph turn puppet");
          expect(!npc->simplifiedBodyState(),"a native body action retains its own logical layer state");
          const auto after=npc->visual.pose().combatLayers(npc->world().tickCount());
          expect(native.size()==after.size(),"the graph turn puppet leaves every native action layer in place");
          for(size_t i=0;i<native.size();++i)
            expect(native[i].sequence==after[i].sequence && native[i].start==after[i].start &&
                   native[i].origin==after[i].origin && native[i].state==after[i].state,
                   "the graph turn puppet preserves native action sequence, clock and state");
          }
        if(action==AI_PlayAnim) {
          // Existing saves normalize this transient speed. It is deliberately
          // not a before/after equality assertion.
          advance();
          npc->visual.pose().setLayerSpeed(playing->sequence,.5f,npc->world().tickCount());
          }
        if(action==AI_DrawWeaponMelee) {
          uint64_t fightAt=UINT64_MAX;
          for(size_t i=0;i<playing->sequence->data->events.size();++i) {
            if(playing->sequence->data->events[i].type!=asset::MdsEventType::SET_FIGHT_MODE) continue;
            const auto local=playing->sequence->logicTag(i).time;
            fightAt=std::min(fightAt,playing->start+uint64_t(std::ceil(double(local)/double(playing->speed))));
            }
          const auto now=npc->world().tickCount();
          expect(fightAt!=UINT64_MAX && fightAt>now+1,"weapon draw fixture is admitted before its fight-mode event");
          // Save after positive progress while the weapon event remains pending.
          advance(std::min(tickMs,(fightAt-now)/2));
          expect(npc->world().tickCount()<fightAt && npc->weaponState()==WeaponState::NoWeapon,
                 "weapon draw fixture saves before the real weapon-state change");
          }
        else {
          for(unsigned i=0;i<4;++i) advance();
          }
        const auto before=npc->logicalBodyState();
        const auto sequence=playing->sequence;
        const std::string name=sequence->name;
        const float progress=npc->animProgress(name);
        expect(progress>0.f && progress<.9f,"logical save/load saves strictly inside the native action");
        expect(before.action.kind==action,"logical save/load records the originating action before save");
        const auto beforeLayer=std::find_if(before.layers.begin(),before.layers.end(),[&](const auto& l) {
          return l.sequence->name==name;
          });
        expect(beforeLayer!=before.layers.end(),"logical save/load has a pre-save body clock for comparison");
        const auto beforeSpeed=beforeLayer->speed;
        std::vector<uint8_t> bytes;
        {
          Tempest::MemWriter writer(bytes);
          Serialize out(writer);
          Tempest::Pixmap preview(1,1,Tempest::TextureFormat::RGBA8);
          std::memset(preview.data(),0,4);
          gothic.gameSession()->save(out,"logical body mid-action",preview);
        }
        const auto savedAt=gothic.world()->tickCount();
        NpcLogicAudit::reset();
        gothic.clearGame().reset();
        {
          Tempest::MemReader reader(bytes);
          Serialize in(reader);
          gothic.setGame(std::make_unique<GameSession>(in));
        }
        npc=gothic.world()->npcById(id);
        expect(npc!=nullptr && gothic.world()->tickCount()==savedAt,"logical save/load restores the actor and exact world time");
        npc->setSimTierOverride(tier);
        npc->setSimTier(tier);
        compare(*npc); // Before any positive simulation step or explicit sync.
        npc->logicalBody.auditTick(*npc);
        const auto restored=npc->logicalBodyState();
        auto stale=restored;
        stale.body=restored.body==BS_NONE ? BS_STAND : BS_NONE;
        NpcLogicAudit::reset();
        stale.auditTick(*npc);
        expect(NpcLogicAudit::stats().bodyMismatch==1 && NpcLogicAudit::stats().weaponMismatch==0,
               "body audit detects stale logical input before any native resynchronization");
        stale=restored;
        stale.weapon=restored.weapon==WeaponState::NoWeapon ? WeaponState::Fist : WeaponState::NoWeapon;
        NpcLogicAudit::reset();
        stale.auditTick(*npc);
        expect(NpcLogicAudit::stats().bodyMismatch==0 && NpcLogicAudit::stats().weaponMismatch==1,
               "weapon audit detects stale logical input before any native resynchronization");
        NpcLogicAudit::reset();
        npc->logicalBody.auditTick(*npc);
        const auto layer=std::find_if(restored.layers.begin(),restored.layers.end(),[&](const auto& l) {
          return l.sequence->name==name;
          });
        expect(layer!=restored.layers.end() && layer->expectedEnd>savedAt,
               "logical save/load rebuilds an active canonical action clock");
        const auto canonicalAction=action==AI_DrawWeaponMelee ? AI_DrawWeapon : action;
        expect(restored.action.kind==canonicalAction,
               "logical save/load recovers the canonical action kind from the saved body");
        expect(restored.action.sequence==layer->sequence,
               "logical save/load binds the action to its own sequence rather than an unrelated layer");
        const auto expectedEnd=layer->expectedEnd;
        const auto origin=layer->origin;
        const auto canonicalStart=layer->start;
        const auto canonicalSpeed=layer->speed;
        const auto actionEnd=restored.action.expectedEnd;
        const auto nativeWaitEnd=npc->aniWaitTime;
        uint32_t bodyAfterSequence=BS_NONE;
        for(const auto& clock:restored.layers) {
          auto state=clock.state;
          if(clock.sequence==layer->sequence && clock.origin==origin) {
            if(clock.sequence->nextPtr==nullptr) continue;
            if((state&BS_MAX)==BS_STUMBLE || (state&BS_MAX)==BS_PARADE) state=BS_NONE;
            }
          bodyAfterSequence=std::max(bodyAfterSequence,uint32_t(state&(BS_MAX|BS_FLAG_MASK)));
          }
        if(restored.interactiveState) bodyAfterSequence=uint32_t(*restored.interactiveState);
        if(restored.externalState) bodyAfterSequence=uint32_t(*restored.externalState);
        expect(restored.action.predictable && actionEnd>savedAt,"logical save/load rebuilds the pending action deadline");
        expect(actionEnd==expectedEnd,"restored action deadline follows its own canonical layer rather than the saved AI wait");
        if(action==AI_PlayAnim)
          expect(beforeSpeed==.5f && canonicalSpeed==1.f,"fresh load applies the native save speed normalization");
        // This copy receives no further Pose inputs. Its layer, body and
        // weapon changes must follow the retained sequence clock alone.
        auto detached=restored;
        detached.advance(expectedEnd-1);
        expect(std::any_of(detached.layers.begin(),detached.layers.end(),[&](const auto& l) {
          return l.sequence->name==name && l.origin==origin && !l.finished;
          }),"detached logical clock keeps its own layer until the metadata deadline");
        detached.advance(expectedEnd);
        expect(std::none_of(detached.layers.begin(),detached.layers.end(),[&](const auto& l) {
          return l.sequence->name==name && l.origin==origin && !l.finished;
          }),"detached logical clock finishes its own layer without a Pose resynchronization");
        expect(uint32_t(detached.body)==bodyAfterSequence,
               "detached logical body follows the sequence successor metadata without executor inputs");
        if(action==AI_DrawWeaponMelee) {
          expect(std::any_of(layer->events.begin(),layer->events.end(),[&](const auto& event) {
            return event.source.def==asset::MdsEventType::SET_FIGHT_MODE && event.expected>savedAt;
            }),"weapon save fixture precedes a pending native fight-mode event");
          expect(detached.weapon!=restored.weapon,
                 "detached logical weapon advances at its sequence fight-mode event without native input");
          }
        detached=restored;
        npc->setExternalControl(true);
        bool active=true;
        uint64_t endedAt=0, nativeWaitReleasedAt=0;
        BodyState nativeBodyAtRemoval=BS_NONE, detachedBodyAtRemoval=BS_NONE;
        const bool pendingNativeWait=nativeWaitEnd>=savedAt;
        for(unsigned i=0;i<400 && (active || (pendingNativeWait && nativeWaitReleasedAt==0));++i) {
          advance();
          compare(*npc);
          detached.advance(npc->world().tickCount());
          active=false;
          for(const auto& real:npc->visual.pose().combatLayers(npc->world().tickCount()))
            active |= real.sequence->name==name && real.origin==origin;
          if(active)
            expect(detached.body==npc->bodyState(),"detached logical body follows the real body while its own layer is active");
          if(!active && endedAt==0) endedAt=npc->world().tickCount();
          if(!active && endedAt==npc->world().tickCount()) {
            expect(std::none_of(detached.layers.begin(),detached.layers.end(),[&](const auto& l) {
              return l.sequence->name==name && l.origin==origin && !l.finished;
              }),"detached logical clock finishes its own layer when the native layer is removed");
            expect(uint32_t(detached.body)==bodyAfterSequence,
                   "detached logical body follows its metadata after the native layer is removed");
            // Npc::tick can start Idle explicitly after a sequence with no
            // successor. A detached clock has not received that executor input.
            nativeBodyAtRemoval=npc->bodyState();
            detachedBodyAtRemoval=detached.body;
            expect(detached.weapon==npc->weaponState(),"detached logical weapon follows the real event result without Pose inputs");
            }
          if(pendingNativeWait && nativeWaitReleasedAt==0 && npc->world().tickCount()>nativeWaitEnd)
            nativeWaitReleasedAt=npc->world().tickCount();
          }
        expect(!active,"logical save/load observes the real canonical animation end");
        expect(!pendingNativeWait || nativeWaitReleasedAt!=0,"logical save/load separately observes the native AI wait release");
        const auto error=int64_t(endedAt)-int64_t(expectedEnd);
        const auto actionError=int64_t(endedAt)-int64_t(actionEnd);
        const auto nativeWaitError=pendingNativeWait ? int64_t(nativeWaitReleasedAt)-int64_t(actionEnd) : 0;
        const auto& audit=NpcLogicAudit::stats();
        std::cerr << "npc-logic-save {\"action\":\"" << NpcLogicAudit::actionName(action)
                  << "\",\"animation\":\"" << name << "\",\"archive_bytes\":" << bytes.size()
                  << ",\"saved_at\":" << savedAt << ",\"pre_save_start\":" << before.action.start
                  << ",\"canonical_start\":" << canonicalStart << ",\"pre_save_end\":" << before.action.expectedEnd
                  << ",\"pre_save_speed\":" << beforeSpeed << ",\"canonical_speed\":" << canonicalSpeed
                  << ",\"canonical_action\":\"" << NpcLogicAudit::actionName(restored.action.kind) << '"'
                  << ",\"pre_save_layer_start\":" << beforeLayer->start << ",\"pre_save_layer_end\":" << beforeLayer->expectedEnd
                  << ",\"canonical_action_start\":" << restored.action.start << ",\"canonical_action_end\":" << actionEnd
                  << ",\"actual_action_end\":" << endedAt << ",\"action_end_error_ms\":" << actionError
                  << ",\"native_wait_end\":" << nativeWaitEnd << ",\"native_wait_released_at\":" << nativeWaitReleasedAt
                  << ",\"native_wait_error_ms\":" << nativeWaitError
                  << ",\"canonical_end\":" << expectedEnd << ",\"actual_end\":" << endedAt
                  << ",\"end_error_ms\":" << error << ",\"world_tick_ms\":" << tickMs
                  << ",\"detached_body_at_removal\":" << uint32_t(detachedBodyAtRemoval)
                  << ",\"native_body_at_removal\":" << uint32_t(nativeBodyAtRemoval)
                  << ",\"removal_body_difference\":\"" << (detachedBodyAtRemoval==nativeBodyAtRemoval ? "none" : "executor_idle_reanchor") << '"'
                  << ",\"audit_samples\":" << audit.ticks << ",\"body_mismatch\":" << audit.bodyMismatch
                  << ",\"weapon_mismatch\":" << audit.weaponMismatch << "}\n";
        expect(audit.ticks>0 && audit.bodyMismatch==0 && audit.weaponMismatch==0,
               "logical save/load shadow audit has samples and no body or weapon mismatches");
        expect(std::abs(error)<=int64_t(tickMs),"logical save/load canonical sequence end is predicted within one world tick");
        expect(std::abs(actionError)<=int64_t(tickMs),"logical save/load canonical action end is predicted within one world tick");
        }
      {
        ++cases;
        auto& world=*gothic.world();
        auto* moving=world.npcById(id);
        const auto* first=world.findPoint("FP_ROAM_CITY_TO_FOREST_15",false);
        const WayPoint* second=nullptr;
        world.forEachWayPoint([&](const WayPoint& point) {
          if(first!=nullptr && point.isFreePoint() && point.name==first->name && &point!=first)
            second=&point;
          });
        expect(first!=nullptr && second!=nullptr,"FP save/load fixture uses duplicate authored freepoint names");
        moving->clearAiQueue(); moving->clearState(true); moving->clearGoTo();
        moving->setTarget(nullptr);
        moving->setExternalControl(true);
        moving->setPosition(second->groundPos);
        moving->attachToPoint(second);
        moving->go2.set(second);
        moving->wayPath.add(*second);
        moving->aiPush(AiQueue::aiGoToPoint(*second));
        std::vector<uint8_t> bytes;
        {
          Tempest::MemWriter writer(bytes);
          Serialize out(writer);
          out.setContext(&world);
          moving->save(out,id,"/npc/");
        }
        const auto load=[&](NpcSimTier tier) {
          moving->currentFpLock=FpLock();
          Tempest::MemReader reader(bytes);
          Serialize in(reader);
          in.setContext(&world);
          moving->load(in,id,"/npc/");
          moving->postValidate();
          moving->setSimTier(tier);
          };
        load(NpcSimTier::Full);
        expect(moving->currentFp==first && moving->currentFpLock.point()==first &&
               moving->go2.wp==first && moving->wayPath.last()==first && moving->aiQueue.front()->point==first,
               "Full save/load keeps native first-match FP references");
        const auto count=world.npcSimTierStats().fpLoadResolutions;
        load(NpcSimTier::Simplified);
        expect(moving->currentFp==second && moving->currentFpLock.point()==second &&
               moving->go2.wp==second && moving->wayPath.last()==second && moving->aiQueue.front()->point==second &&
               world.npcSimTierStats().fpLoadResolutions==count+5,
               "Simplified save/load resolves every ambiguous saved reference against the saved NPC position");
        moving->setSimTier(NpcSimTier::Full);
        expect(moving->currentFp==second && moving->currentFpLock.point()==second &&
               moving->go2.wp==second && moving->wayPath.last()==second && moving->aiQueue.front()->point==second &&
               world.npcSimTierStats().fpLoadResolutions==count+5,
               "a later Full tier continues the resolved FP action without another lookup");
        moving->clearAiQueue(); moving->clearGoTo(); moving->attachToPoint(nullptr);
        }
      for(const bool itemGoal:{false,true}) {
        ++cases;
        auto* moving=gothic.world()->npcById(id);
        const auto* start=gothic.world()->findWayPoint("NW_XARDAS_TOWER_VALLEY_091");
        const auto* goal=gothic.world()->findWayPoint("NW_FARM1_PATH_CITY_05");
        expect(moving!=nullptr && start!=nullptr && goal!=nullptr,"graph save/load uses authored route endpoints");
        moving->clearAiQueue(); moving->clearState(true); moving->clearGoTo();
        moving->closeWeapon(true);
        moving->setTarget(nullptr);
        moving->setExternalControl(true);
        if(!itemGoal) {
          // Native SetWalkMode need not replace the preceding idle loop. A
          // handoff during its turn must accept that old locomotion idle.
          moving->setSimTierOverride(NpcSimTier::Full);
          moving->setSimTier(NpcSimTier::Full);
          moving->setWalkMode(WalkBit::WM_Run);
          moving->visual.pose().stopAllAnim();
          expect(moving->setAnim(Npc::Anim::Idle),"mid-turn handoff starts from a native run idle");
          const auto toward=gothic.player()->position()-moving->position();
          moving->setDirection(Tempest::Vec3(toward.z,0,-toward.x));
          moving->setWalkMode(WalkBit::WM_Walk);
          moving->aiPush(AiQueue::aiTurnToNpc(gothic.player()));
          advance();
          expect(moving->aiQueue.front()!=nullptr && moving->aiQueue.front()->act==AI_TurnToNpc &&
                 moving->visual.pose().logicalLayerCount()>1,
                 "mid-turn handoff retains the old idle alongside its native turn layer");
          const auto angle=moving->rotation();
          moving->setSimTierOverride(NpcSimTier::Simplified);
          moving->setSimTier(NpcSimTier::Simplified);
          advance();
          const bool turnProgress=moving->usesSimplifiedMovement() &&
                                  std::abs(std::remainder(moving->rotation()-angle,360.f))>0.001f;
          expect(turnProgress,
                 "a mode change before a native turn does not stall its graph handoff");
          for(unsigned i=0;i<90 && moving->aiQueue.size()>0;++i)
            advance();
          expect(moving->aiQueue.size()==0,"the graph handoff finishes the same mode-changed turn");
          }
        moving->setSimTierOverride(NpcSimTier::Simplified);
        moving->setSimTier(NpcSimTier::Simplified);
        moving->setWalkMode(WalkBit::WM_Walk);
        moving->setPosition(start->groundPos);
        Item* item=nullptr;
        uint32_t itemId=uint32_t(-1);
        if(itemGoal) {
          item=gothic.world()->addItem(gothic.world()->script().findSymbolIndex("ItMi_Gold"),goal->groundPos);
          expect(item!=nullptr,"graph item save/load creates a real world target");
          itemId=gothic.world()->itmId(&item->handle());
          moving->aiPush(AiQueue::aiGotoItem(*item));
          } else {
          moving->aiPush(AiQueue::aiGoToPoint(*goal));
          }
        moving->aiPush(AiQueue::aiWait(5000));
        bool midEdge=false;
        for(unsigned i=0;i<600;++i) {
          advance();
          const auto* next=moving->currentGoToPoint();
          if(next!=nullptr && next!=start && (moving->position()-start->groundPos).quadLength()>100.f*100.f &&
             (moving->position()-next->groundPos).quadLength()>100.f*100.f) {
            midEdge=true;
            break;
            }
          }
        const auto pathNames=[](const Npc& actor) {
          std::vector<std::string> names;
          auto path=actor.navigationPath();
          while(const auto* point=path.pop()) names.push_back(point->name);
          return names;
          };
        expect(midEdge && moving->usesSimplifiedMovement() && moving->currentGoToPoint()!=nullptr && !pathNames(*moving).empty(),
               "graph save/load saves an active edge with a remaining native path");
        const auto position=moving->position();
        const auto angle=moving->rotation();
        const auto path=pathNames(*moving);
        const auto endpoint=moving->currentGoToPoint()->name;
        const auto hint=moving->moveHint();
        const auto state=moving->currentState().ptr;
        const auto queueSize=moving->aiQueue.size();
        expect(queueSize==(itemGoal ? 2u : 1u) &&
               moving->aiQueue.front()->act==(itemGoal ? AI_GotoItem : AI_Wait),
               "graph save/load retains its terminal target and an action queued after arrival");
        const float tickTravel=moving->simplifiedMoveSpeed()*float(tickMs);
        std::vector<uint8_t> bytes;
        {
          Tempest::MemWriter writer(bytes);
          Serialize out(writer);
          Tempest::Pixmap preview(1,1,Tempest::TextureFormat::RGBA8);
          std::memset(preview.data(),0,4);
          gothic.gameSession()->save(out,"simplified NPC mid-edge",preview);
        }
        advance();
        const auto nextPosition=moving->position();
        gothic.clearGame().reset();
        {
          Tempest::MemReader reader(bytes);
          Serialize in(reader);
          gothic.setGame(std::make_unique<GameSession>(in));
        }
        moving=gothic.world()->npcById(id);
        expect(moving!=nullptr,"graph save/load restores the same NPC identity");
        moving->setExternalControl(true);
        moving->setSimTierOverride(NpcSimTier::Simplified);
        moving->setSimTier(NpcSimTier::Simplified);
        expect((moving->position()-position).quadLength()<.01f && moving->rotation()==angle,
               "graph save/load preserves the exact mid-edge transform before its first tick");
        expect(moving->currentGoToPoint()!=nullptr && moving->currentGoToPoint()->name==endpoint &&
               pathNames(*moving)==path && moving->moveHint()==hint && moving->currentState().ptr==state &&
               moving->aiQueue.size()==queueSize && moving->aiQueue.front()->act==(itemGoal ? AI_GotoItem : AI_Wait),
               "graph save/load preserves the current edge, remaining path, destination, ZS and queued action");
        if(itemGoal) {
          item=gothic.world()->itmById(itemId);
          expect(item!=nullptr && moving->aiQueue.front()->item==item && moving->navigationGoal().kind=="item",
                 "graph save/load restores the exact live world item without changing the archive layout");
          }
        advance();
        expect((moving->position()-nextPosition).quadLength()<=(tickTravel+.01f)*(tickTravel+.01f),
               "graph save/load continues within one tick of uninterrupted travel");
        if(itemGoal) {
          const auto movedItem=item->position()+Tempest::Vec3(20,0,0);
          item->setPosition(movedItem.x,movedItem.y,movedItem.z);
          expect((moving->navigationGoal().position-item->position()).quadLength()<.01f,
                 "restored graph item movement follows the live target position");
          }
        for(unsigned i=0;i<10;++i) advance();
        expect((moving->position()-position).quadLength()>tickTravel*tickTravel,
               "restored graph movement keeps advancing on the saved route");
        moving->clearAiQueue(); moving->clearGoTo();
        moving->setSimTierOverride(NpcSimTier::Full);
        moving->setSimTier(NpcSimTier::Full);
        moving->updateSimpleMovement();
      }
      {
        ++cases;
        auto* moving=gothic.world()->npcById(id);
        auto* hero=gothic.player();
        const auto* start=gothic.world()->findWayPoint("NW_CITY_MERCHANT_PATH_23");
        const auto* corner=gothic.world()->findWayPoint("NW_CITY_MERCHANT_PATH_18");
        const auto* goal=gothic.world()->findWayPoint("NW_CITY_MERCHANT_TAVERN01_BACKROOM");
        expect(moving!=nullptr && hero!=nullptr && start!=nullptr && corner!=nullptr && goal!=nullptr,
               "graph corner fixture uses the authored tavern route");
        auto route=gothic.world()->wayTo(*start,*goal);
        expect(route.pop()==start && route.pop()==corner && route.pop()==goal && route.first()==nullptr,
               "the native planner routes the tavern walk through PATH18");
        const auto* heroPoint=hero->currentWayPoint();
        hero->attachToPoint(corner);
        moving->clearAiQueue(); moving->clearState(true); moving->clearGoTo();
        moving->closeWeapon(true); moving->setTarget(nullptr); moving->setExternalControl(true);
        moving->setSimTierOverride(NpcSimTier::Simplified);
        moving->setSimTier(NpcSimTier::Simplified);
        moving->setWalkMode(WalkBit::WM_Walk);
        moving->setPosition(start->groundPos);
        moving->setDirection(corner->groundPos-start->groundPos);
        moving->attachToPoint(start);
        moving->visual.pose().stopAllAnim();
        expect(moving->setAnim(Npc::Anim::Idle),"graph corner fixture starts from the native walk idle");
        moving->aiPush(AiQueue::aiGoToPoint(*goal));
        moving->aiPush(AiQueue::aiWait(5000));
        const auto graphFailures=gothic.world()->npcSimTierStats().graphFallbackGround+
                                 gothic.world()->npcSimTierStats().graphFallbackSegment;
        bool started=false,occupied=false,arrived=false,waitPreserved=true;
        float closest=std::numeric_limits<float>::max();
        float tickTravel=0;
        for(unsigned step=0;step<1200;++step) {
          advance();
          const auto delta=moving->position()-corner->groundPos;
          closest=std::min(closest,std::hypot(delta.x,delta.z));
          tickTravel=std::max(tickTravel,moving->simplifiedMoveSpeed()*float(tickMs));
          if(moving->currentGoToPoint()==corner)
            occupied|=corner->useCounter()>1;
          if(moving->moveHint()!=Npc::GT_No) {
            started=true;
            const auto* head=moving->aiQueue.front();
            waitPreserved&=head!=nullptr && head->act==AI_Wait;
            }
          else if(started) {
            arrived=true;
            break;
            }
          }
        const float finalDistance=MoveAlgo::closeToPointThreshold*(moving->mvAlgo.checkLastBounce() ? .5f : 1.5f);
        expect(occupied && closest<=tickTravel+.01f,
               "an occupied internal vertex is visited within one tick instead of cutting the tavern corner");
        expect(arrived && moving->usesSimplifiedMovement() && MoveAlgo::isClose(*moving,*goal,finalDistance),
               "the graph route arrives using the native terminal distance");
        expect(graphFailures==gothic.world()->npcSimTierStats().graphFallbackGround+
                              gothic.world()->npcSimTierStats().graphFallbackSegment,
               "the graph corner contract never substitutes native movement for a failed segment");
        const auto* head=moving->aiQueue.front();
        expect(waitPreserved && ((head!=nullptr && head->act==AI_Wait) || moving->waitTime>moving->world().tickCount()),
               "the occupied-corner route preserves and releases its following wait");
        hero->attachToPoint(heroPoint);
        moving->clearAiQueue(); moving->clearGoTo();
        moving->visual.pose().stopAllAnim();
        expect(moving->setAnim(Npc::Anim::Idle) && !moving->nativeBodyAction(),
               "unsupported-ground fixture admits graph movement from a clean native idle");
        moving->setPosition(start->groundPos+Tempest::Vec3(0,500,0));
        moving->go2.set(goal);
        moving->simpleMovement.edgeTo=nullptr;
        const auto groundFailures=gothic.world()->npcSimTierStats().graphFallbackGround;
        expect(moving->implGoTo(tickMs) && moving->graphMovementFallback() &&
               gothic.world()->npcSimTierStats().graphFallbackGround==groundFailures+1 &&
               moving->currentGoToPoint()==goal,
               "unsupported graph ground hands the same remaining action to native movement without throwing");
        moving->setSimTierOverride(NpcSimTier::Full);
        moving->setSimTier(NpcSimTier::Full);
        moving->updateSimpleMovement();
        expect(moving->graphMovementFallback() && moving->currentGoToPoint()==goal,
               "a graph fallback keeps its native action through a tier switch");
        moving->clearGoTo();
        expect(!moving->graphMovementFallback(),"native fallback ends with its action");
      }
      // Native float frame rounding can fire a loop event one millisecond
      // before its integer schedule. The audit must match the same cycle.
      ++cases;
      auto* npc=gothic.world()->npcById(id);
      npc->closeWeapon(true);
      npc->visual.pose().stopAllAnim();
      const auto* sequence=npc->playAnimByName("S_FISTWALKL",BS_WALK);
      expect(sequence!=nullptr && sequence->animCls==Animation::Loop,"loop clock fixture uses the stock fist walk");
      npc->syncLogicalBodyState();
      auto clock=npc->logicalBodyState();
      const auto layer=std::find_if(clock.layers.begin(),clock.layers.end(),[&](const auto& l) { return l.sequence==sequence; });
      expect(layer!=clock.layers.end() && !layer->events.empty(),"loop clock fixture has native event metadata");
      struct Probe {
        NpcLogicalState& clock;
        const NpcLogicalState::Layer& layer;
        const Animation::LogicEvent* target=nullptr;
        uint64_t now=0;
        bool fired=false;
        } probe{clock,*layer};
      Animation::LogicEventObserver observer{&probe,[](void* opaque,const Animation::LogicEvent& event) {
        auto& p=*static_cast<Probe*>(opaque);
        if(event.kind!=p.target->kind || event.index!=p.target->index || event.frame!=p.target->frame) return;
        p.fired=true;
        p.clock.eventFired(p.layer.sequence,p.layer.origin,event,p.now);
        }};
      NpcLogicAudit::reset();
      for(uint64_t cycle=1;cycle<=65536 && !probe.fired;++cycle) {
        for(const auto& event:layer->events) {
          if(!event.source.delivered) continue;
          probe.target=&event.source;
          probe.now=event.expected+uint64_t(double(cycle)*sequence->totalTime())-1;
          Animation::EvCount events;
          sequence->processEvents(probe.now-1,layer->start,probe.now,events,&observer);
          if(probe.fired) break;
          }
        }
      expect(probe.fired,"loop clock fixture observes a native event before its integer deadline");
      const auto& errors=NpcLogicAudit::stats().byAnimation.at(sequence->name).eventErrorMs;
      expect(errors.size()==1 && errors.begin()->first==1,"an early native loop event has a one millisecond audit error");
      expect(NpcLogicAudit::stats().eventMismatch==0,"an event inside the world step is not a mismatch");
      NpcLogicAudit::reset();
      const auto& event=layer->events.front();
      clock.eventFired(sequence,layer->origin,event.source,event.expected+clock.worldStep+1);
      expect(NpcLogicAudit::stats().eventMismatch==1,"an event beyond the world step is a mismatch");

      ++cases;
      NpcLogicAudit::reset();
      clock.auditTick(*npc);
      clock.interactiveState=BS_SIT;
      clock.updateBody();
      clock.auditTick(*npc);
      clock.externalState=BS_DEAD; // External state takes precedence over mobsi.
      clock.updateBody();
      clock.auditTick(*npc);
      const auto& samples=NpcLogicAudit::stats();
      expect(samples.bodySamples==1 && samples.bodyInteractiveSamples==1 && samples.bodyExternalSamples==1,
             "body audit separates informative, interactive and external samples without double counting");
      expect(samples.ticks==3 && samples.bodyMismatch==0,
             "body overrides cannot turn hidden layer forecasts into body comparisons");

      {
        ++cases;
        auto& world=*gothic.world();
        const auto originalKind=static_cast<const daedalus::INpc&>(npc->handle()).symbol_index();
        const auto sheep=world.script().findSymbolIndex("SHEEP");
        expect(sheep!=size_t(-1) && sheep!=originalKind,"transformation fixture uses a different stock NPC kind");
        expect(npc->transformInto(sheep),"transformation fixture changes the real NPC instance");
        std::vector<uint8_t> bytes;
        {
          Tempest::MemWriter writer(bytes);
          Serialize out(writer);
          out.setContext(&world);
          out.setEntry("transform-kind");
          npc->saveTrState(out);
          npc->transformBack();
          npc->save(out,id,"/npc/");
          }
        Tempest::MemReader reader(bytes);
        Serialize in(reader);
        in.setContext(&world);
        expect(in.setEntry("transform-kind"),"transformation snapshot entry exists");
        bool transformed=false;
        uint32_t kind=uint32_t(-1);
        in.read(transformed,kind);
        expect(transformed && kind==originalKind,"transformation snapshot stores the original NPC kind");
        expect(in.setEntry("worlds/",in.worldName(),"/npc/",id,"/data"),"restored NPC save entry exists");
        in.read(kind);
        expect(kind==originalKind && npc->instanceSymbol()==originalKind,"restored NPC save stores the original kind");
        }

      ++cases;
      auto retained=npc->handlePtr(); // Keep the invalid actor eligible for revival.
      const auto* waypoint=npc->world().findWayPoint(npc->position());
      expect(waypoint!=nullptr,"inactive logical-state fixture has a valid return point");
      const auto returnPoint=waypoint->name;
      npc->clearRoutine();
      npc->handle().wp="__BODY_WATCH_INVALID_POINT";
      npc->world().resetPositionToTA();
      expect(npc->world().npcId(npc)==uint32_t(-1) && !npc->logicalBody.initialized && npc->visual.pose().logicalInputs().empty(),
             "retiring an invalid routine discards queued inputs and invalidates its logical shadow");
      npc->visual.pose().stopAllAnim();
      expect(npc->playAnimByName("S_FISTWALKL",BS_WALK)!=nullptr && npc->visual.pose().logicalInputs().empty(),
             "inactive NPC animation mutations do not accumulate logical inputs");
      npc->handle().wp=returnPoint;
      npc->world().resetPositionToTA();
      expect(npc->world().npcId(npc)!=uint32_t(-1) && npc->logicalBody.initialized && npc->visual.pose().logicalInputs().empty(),
             "routine revival rebuilds the current body without replaying inactive inputs");
      compare(*npc);
      npc->visual.pose().stopAllAnim();
      expect(npc->playAnimByName("S_FISTWALKL",BS_WALK)!=nullptr && !npc->visual.pose().logicalInputs().empty(),
             "revived NPC resumes collecting live logical inputs");
      npc->world().removeNpc(*npc);
      expect(!npc->logicalBody.initialized && npc->visual.pose().logicalInputs().empty(),
             "explicit NPC removal discards live logical inputs");
      npc->visual.pose().stopAllAnim();
      expect(npc->playAnimByName("S_FISTWALKL",BS_WALK)!=nullptr && npc->visual.pose().logicalInputs().empty(),
             "removed NPC no longer collects logical inputs");
      }
  };

int main(int argc,const char** argv) {
  std::FILE* result=fdopen(dup(STDOUT_FILENO),"w");
  dup2(STDERR_FILENO,STDOUT_FILENO);
  const auto workingDirectory=std::filesystem::current_path();
  try {
    std::string save,scenario;
    for(int i=1;i<argc;++i) {
      const std::string_view arg=argv[i];
      if(arg=="--save" || arg=="--scenario") {
        if(++i>=argc) throw std::runtime_error("missing fixture option value");
        (arg=="--save" ? save : scenario)=std::filesystem::absolute(argv[i]).string();
        }
      }
    if(scenario.empty()) throw std::runtime_error("--scenario scripts/bench/body-api.bench is required");
    clockSwitching();
    NpcLogicalStateContract::farTickRule();
    bool missingNetwork=false;
#ifdef GOTHIC_BODY_ENGINE_NET
    // A fresh process exercises the one-time startup load with absent weights.
    const auto child=fork();
    if(child<0) throw std::runtime_error("cannot fork missing-network fixture");
    missingNetwork=child==0;
    if(child>0) {
      int status=0;
      expect(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0,
             "missing-network body_acquire fixture passes in a fresh process");
      }
    setenv("OPENGOTHIC_BODY_NET",missingNetwork ? "/nonexistent/gothic-body-watch-test.bodynet" : GOTHIC_BODY_ENGINE_NET,1);
#endif
    setenv("OPENGOTHIC_PHYSICS_THREADS","0",0);
    setenv("OPENGOTHIC_NPC_LOGIC_AUDIT","1",1);
    // Canonical body fixtures keep the background native; individual cases
    // explicitly opt their subject into Simplified.
    setenv("OPENGOTHIC_NPC_SIM_TIER","full",1);
    // Every NPC ticks every frame in the other fixtures; the far tick case sets its own rate.
    setenv("OPENGOTHIC_NPC_SIM_TICK_MS","0",1);
    Workers::setSerialExecution(true);
    CommandLine commandLine(argc,argv);
    Resources resources;
    Gothic gothic;
    gothic.isNpcInDialogFn=[](const Npc*) { return false; };
    gothic.setupGlobalScripts();
    GameMusic music;
    const auto output=std::filesystem::temp_directory_path()/("gothic-body-watch-test-"+std::to_string(getpid()));
    std::filesystem::create_directories(output);
    // The melee journal opens on the first swing, including earlier fixture cases.
    std::filesystem::current_path(output);
    TestBench::configure(scenario,output.string(),17);
    if(save.empty()) {
      gothic.setGame(std::make_unique<GameSession>(std::string(gothic.defaultWorld())));
      } else {
      Tempest::RFile file(save);
      Serialize serialized(file);
      gothic.setGame(std::make_unique<GameSession>(serialized));
      }
    expect(gothic.world()!=nullptr && gothic.player()!=nullptr,"playable headless world");
    PlayerControl player;
    player.clearInput();
    gothic.gameSession()->setTimeMultiplyer(1.f);
    gothic.camera()->setViewport(640,480);
    player.clearFocus();
    SimulationStep::finishStep(0,player);
    for(unsigned i=0;i<300 && TestBench::bodyObservationActors().size()<2;++i) step(player);
    auto actors=TestBench::bodyObservationActors();
    expect(actors.size()==2,"body-api fixture created two real NPCs");
    auto& a=*actors[0]; auto& b=*actors[1];
    BodyController::fixturePrepare(b,a,300,0);
    BodyController::fixturePrepare(a,b,300,0);
    b.setDirection(a.position()-b.position()); b.updateTransform();
    a.setExternalControl(false);
    const auto wireStatus=parse(BodyController::statusJson(&a));
    for(const char* field:{"process_policy","segment_remaining_ms","fai_gate","ani_gate","ai_gate","output_gate","interruptible"})
      expect(!wireStatus["actors"][0].HasMember(field),"wire status keeps the strict server schema; boundary diagnostics are opt-in");
    if(!missingNetwork) {
      ++cases;
      auto& script=gothic.world()->script();
      auto& vm=script.getVm();
      const auto current=std::static_pointer_cast<daedalus::INpc>(gothic.player()->handlePtr());
      const auto copied=std::static_pointer_cast<daedalus::INpc>(script.copyNpc(gothic.player()->handlePtr()));
      expect(copied!=current,"dialog fixture uses a copied NPC instance");
      const auto previousOther=vm.global_other()->get_instance();
      auto previousExtra=std::move(gothic.dialogExtraFn);
      bool receivedCurrent=false;
      vm.global_other()->set_instance(current);
      gothic.dialogExtraFn=[&](const daedalus::INpc& npc,bool,std::vector<GameScript::DlgChoice>&) {
        receivedCurrent=&npc==current.get();
        };
      vm.push_instance(copied);
      vm.push_int(1);
      expect(vm.unsafe_call_external(vm.find_symbol_by_name("NPC_CHECKINFO")),"NPC_CHECKINFO external is registered");
      const auto offered=vm.pop_int();
      vm.global_other()->set_instance(previousOther);
      gothic.dialogExtraFn=std::move(previousExtra);
      expect(receivedCurrent && offered==0,"NPC_CHECKINFO extra dialogs receive the current NPC instance");
      }
    if(!missingNetwork) NpcLogicalStateContract::preTickOrder(b);
    if(!missingNetwork) NpcLogicalStateContract::farTick(b);
    if(!missingNetwork) NpcLogicalStateContract::meleeAi(a,b);
#ifdef GOTHIC_BODY_ENGINE_NET
    if(missingNetwork) {
      missingEngineNetwork(a);
      } else {
      enginePolicy(a,b,player);
#endif
    liveLease(a,b,player);
    realtimeSentObservation(a,b,player);
    movementRelease(a,b,player);
    pendingAcquire(a,b,player);
    std::vector<std::string> farFailures;
    farRelease(a,b,*gothic.player(),player,farFailures);
    NpcLogicalStateContract::restoreStamina(a);
    heroTargetRelease(a,*gothic.player(),player,farFailures);
    NpcLogicalStateContract::restoreStamina(a);
    staleFarRelease(a,b,*gothic.player(),player);
    for(const auto& failure:farFailures) std::cerr << "body-watch-test failure: " << failure << '\n';
    expect(farFailures.empty(),"far release/watchdog fixtures restore native ownership by the next engine tick");
    expect(!Resources::hasGraphicsDevice() && !gothic.gameSession()->hasAudioDevice(),"fixture creates no graphics/audio device");
    BodyController::reset();
    BodyObservations::disable();
    meleeRemoval(b);
    meleeShadowLoopChanges(b);
    frozenArenaTail(b);
    NpcLogicalStateContract::run(gothic.world()->npcId(&b),scenario,output);
    {
      const auto statsScenario=output/"melee-stats.bench";
      {
        std::ofstream commands(statsScenario);
        commands << "stats 1h 100 2h 100 talent 2\nwait 33\n"
                    "stats bow 100 talent 0\nwait 33\n"
                    "stats 1h 0 talent auto\nwait 33\n"
                    "stats 2h 0 talent auto\nwait 33\n";
        }
      TestBench::configure(statsScenario.string(),output.string(),17);
      player.clearInput(); player.clearFocus();
      SimulationStep::finishStep(0,player);
      const std::array<std::pair<int,int>,4> levels{{{2,2},{2,2},{0,2},{0,0}}};
      for(const auto& expected:levels) {
        ++cases;
        step(player);
        expect(gothic.player()->talentSkill(TALENT_1H)==expected.first &&
               gothic.player()->talentSkill(TALENT_2H)==expected.second,
               "stats changes melee animation levels only for explicitly supplied melee skills");
        }
    }
#ifdef GOTHIC_BODY_ENGINE_NET
      }
#endif
    {
      auto& script=gothic.world()->script();
      auto& vm=script.getVm();
      GameRules& rules=script;
      auto* integer=vm.add_int_symbol("RULES_INT_CONTRACT");
      expect(integer!=nullptr,"rules fixture adds an integer symbol");
      integer->set_int(314);
      expect(rules.intSymbol("RULES_INT_CONTRACT",19)==314,"rules read the integer value rather than its index");
      expect(rules.intSymbol("RULES_MISSING_CONTRACT",19)==19 && rules.intSymbol("ZS_DEAD",23)==23,
             "rules preserve missing and non-integer fallbacks");
      expect(rules.findSymbolIndex("PC_HERO")==11471 && rules.findSymbolIndex("RULES_MISSING_CONTRACT")==size_t(-1),
             "rules preserve the Gothic II NPC kind index and missing-symbol sentinel");
      expect(rules.symbolName(rules.findSymbolIndex("RULES_INT_CONTRACT"))=="RULES_INT_CONTRACT" &&
             rules.symbolName(size_t(-1)).empty(),"rules resolve the requested symbol and reject an invalid index");
      expect(rules.findFunction("ZS_DEAD").ptr==13035 && rules.symbolName(rules.findFunction("ZS_DEAD").ptr)=="ZS_DEAD" &&
             !rules.findFunction("RULES_INT_CONTRACT").isValid() && !rules.findFunction("RULES_MISSING_CONTRACT").isValid(),
             "rules return a function handle only for a function symbol");
      expect(rules.goldItemId()==6929 && rules.symbolName(rules.goldItemId())=="ITMI_GOLD","rules identify the currency item");
      const auto& state=rules.aiState(ScriptFn(13063));
      expect(state.funcIni==13063 && state.funcLoop==13064 && state.funcEnd==13065 &&
             std::string_view(state.name())=="ZS_UNCONSCIOUS","rules preserve the state callbacks and their name");
      const auto npcId=gothic.player()->instanceSymbol();
      expect(rules.isNpcInstance(npcId) && !rules.isNpcInstance(rules.goldItemId()) && !rules.isNpcInstance(size_t(-1)),
             "rules distinguish the NPC class from item and missing symbols");
      expect(rules.npcRecord(npcId).get()==gothic.player()->handlePtr().get(),
             "rules expose the current NPC instance rather than a copy");
      bool foundString=false;
      for(const auto& symbol:vm.symbols()) {
        if(symbol.type()!=daedalus::DaedalusDataType::STRING || symbol.is_member() || symbol.count()==0)
          continue;
        auto* text=vm.find_symbol_by_index(symbol.index());
        const auto saved=text->get_string();
        text->set_string("Rules string contract");
        const auto actual=std::string(rules.stringSymbol(symbol.name()));
        text->set_string(saved);
        expect(actual=="Rules string contract","rules read the requested string value");
        foundString=true;
        break;
        }
      expect(foundString,"rules fixture exercises a string symbol");
      const char* names[]={"NPCTYPE_MAIN","NPCTYPE_OCMAIN","NPCTYPE_BL_MAIN"};
      const int32_t values[]={31,43,59};
      for(size_t i=0;i<3;++i) {
        auto* symbol=vm.find_symbol_by_name(names[i]);
        if(symbol==nullptr) symbol=vm.add_int_symbol(names[i]);
        expect(symbol!=nullptr,"rules fixture has a story type integer");
        symbol->set_int(values[i]);
        }
      expect(rules.storyNpcTypes()==std::array<int32_t,3>{31,43,59},"rules read all three story types from the VM");
      auto* duration=vm.find_symbol_by_name("SPL_TIME_WHIRLWIND");
      if(duration==nullptr) duration=vm.add_int_symbol("SPL_TIME_WHIRLWIND");
      expect(duration!=nullptr,"rules fixture has the whirlwind duration");
      duration->set_int(17);
      expect(rules.whirlwindDuration()==17,"rules use the scripted whirlwind duration");
      duration->set_int(-4);
      expect(rules.whirlwindDuration()==1,"rules preserve the lower whirlwind duration clamp");
      duration->set_int(91);
      expect(rules.whirlwindDuration()==60,"rules preserve the upper whirlwind duration clamp");
      expect(rules.scriptVmMemorySize()>10000000,"rules include the symbol table in the VM memory probe");
      ++cases;
      auto& source=*gothic.player();
      auto* target=gothic.world()->addNpc(rules.findSymbolIndex("BDT_1003_BANDIT_M"),source.position()+Tempest::Vec3(150,0,0));
      expect(target!=nullptr,"damage rules fixture has a distinct victim");
      const auto savedSource=source.handle();
      const auto savedTarget=target->handle();
      auto* weapon=source.activeWeapon();
      const auto savedWeaponMask=weapon!=nullptr ? weapon->handle().damage_type : 0;
      source.handle().damage_type=64;
      if(weapon!=nullptr) weapon->handle().damage_type=64;
      source.handle().attribute[ATR_DEXTERITY]=17;
      std::fill(std::begin(source.handle().damage),std::end(source.handle().damage),0);
      std::fill(std::begin(source.handle().protection),std::end(source.handle().protection),0);
      source.handle().damage[DamageType::POINT]=33;
      source.handle().damage[DamageType::EDGE]=9;
      target->handle().attribute[ATR_DEXTERITY]=91;
      target->handle().damage_type=4;
      expect(rules.damageTypeMask(source)==64,"damage rules read the attacker's damage mask");
      const auto ranged=rules.rangeDamageValue(source);
      expect(ranged.val[DamageType::POINT]==50 && ranged.val[DamageType::EDGE]==0,
             "ranged damage uses the attacker's dexterity and damage mask");
      target->handle().protection[DamageType::POINT]=17;
      target->handle().protection[DamageType::EDGE]=23;
      Damage spellDamage;
      spellDamage[DamageType::POINT]=59;
      spellDamage[DamageType::EDGE]=83;
      const auto spellHit=rules.damageValue(source,*target,nullptr,true,spellDamage,COLL_APPLYDOUBLEDAMAGE);
      expect(spellHit.value==244 && spellHit.hasHit && !spellHit.invincible && !spellHit.critical,
             "damage rules retain the victim, spell damage and collision multiplier");
      source.handle().attribute[ATR_STRENGTH]=31;
      std::fill(std::begin(source.handle().hitchance),std::end(source.handle().hitchance),100);
      Item ammunition(source.world(),rules.findSymbolIndex("ITRW_ARROW"),Item::T_Inventory);
      Bullet arrow(source.world(),ammunition,source.position());
      Damage arrowDamage;
      arrowDamage[DamageType::POINT]=71;
      arrow.setDamage(arrowDamage);
      arrow.setPrecise(1.f);
      const auto arrowHit=rules.damageValue(source,*target,&arrow,false,{},COLL_APPLYDAMAGE);
      const auto meleeHit=rules.damageValue(source,*target,nullptr,false,{},COLL_APPLYDAMAGE);
      expect(arrowHit.value==54 && arrowHit.hasHit && !arrowHit.invincible && !arrowHit.critical,
             "damage rules preserve the physical arrow and its damage");
      expect(meleeHit.value==47 && meleeHit.hasHit && !meleeHit.invincible && meleeHit.critical,
             "damage rules distinguish an arrow from the melee fallback");
      auto& guild=const_cast<GuildRecord&>(rules.guildVal());
      const auto sourceGuild=source.guild();
      const auto savedFallHeight=guild.falldown_height[sourceGuild];
      const auto savedFallDamage=guild.falldown_damage[sourceGuild];
      guild.falldown_height[sourceGuild]=-300;
      guild.falldown_damage[sourceGuild]=7;
      source.handle().protection[DamageType::FALL]=4;
      const auto standingFall=rules.damageFall(source,0.f);
      const auto movingFall=rules.damageFall(source,1.f);
      guild.falldown_height[sourceGuild]=savedFallHeight;
      guild.falldown_damage[sourceGuild]=savedFallDamage;
      expect(standingFall.value==17 && standingFall.hasHit && movingFall.value==52 && movingFall.hasHit,
             "fall damage rules retain the selected NPC, guild and speed");
      source.handle()=savedSource;
      target->handle()=savedTarget;
      if(weapon!=nullptr) weapon->handle().damage_type=savedWeaponMask;
      expect(std::abs(rules.meleeMultiplier(200.f,100.f,1.f,90.f,1)-.9f)<.00001f,
             "melee multiplier rules preserve speed, blade geometry and target index");
      expect(rules.vanillaChance(60,0.f)==100 && rules.vanillaChance(60,750.f)==80 &&
             rules.vanillaChance(60,1501.f)==39 && rules.vanillaChance(60,4500.f)==0,
             "arrow chance rules retain talent, distance and Gothic's distance step");
      auto& tacticOne=const_cast<FightTactic&>(Gothic::fai()[1]);
      auto& tacticTwo=const_cast<FightTactic&>(Gothic::fai()[2]);
      expect(&tacticOne!=&tacticTwo,"fight rules fixture has two distinct tactic tables");
      const auto savedOne=tacticOne.my_w_focus.move[0];
      const auto savedTwo=tacticTwo.my_w_focus.move[0];
      tacticOne.my_w_focus.move[0]=Def::FightMove(7);
      tacticTwo.my_w_focus.move[0]=Def::FightMove(11);
      const auto actualOne=int(rules.fightAi(1).my_w_focus.move[0]);
      const auto actualTwo=int(rules.fightAi(2).my_w_focus.move[0]);
      tacticOne.my_w_focus.move[0]=savedOne;
      tacticTwo.my_w_focus.move[0]=savedTwo;
      expect(actualOne==7 && actualTwo==11,"fight rules select the requested tactic table");
      auto* finishDistance=vm.find_symbol_by_name("NPC_ATTACK_FINISH_DISTANCE");
      expect(finishDistance!=nullptr,"fight rules fixture has the finishing distance");
      if(finishDistance->type()==daedalus::DaedalusDataType::FLOAT) {
        const auto saved=finishDistance->get_float();
        finishDistance->set_float(247.5f);
        const auto actual=rules.attackFinishDistance();
        finishDistance->set_float(saved);
        expect(actual==247.5f,"fight rules retain a fractional scripted finishing distance");
        } else {
        expect(finishDistance->type()==daedalus::DaedalusDataType::INT,"fight rules fixture has a numeric finishing distance");
        const auto saved=finishDistance->get_int();
        finishDistance->set_int(247);
        const auto actual=rules.attackFinishDistance();
        finishDistance->set_int(saved);
        expect(actual==247.f,"fight rules retain an integer scripted finishing distance");
        }
      }
    {
      ++cases;
      GameHost& host=gothic;
      expect(!host.isDialogActive(),"dialog activity defaults to false without a menu");
      auto previous=std::move(gothic.isDialogActiveFn);
      bool active=false;
      gothic.isDialogActiveFn=[&]() { return active; };
      expect(!host.isDialogActive() && !host.isInDialog(),"dialog activity preserves an inactive menu response");
      active=true;
      expect(host.isDialogActive() && !host.isInDialog(),
             "dialog activity preserves pending text even when no NPC dialog is open");
      active=false;
      expect(!host.isDialogActive(),"dialog activity reflects the menu becoming inactive again");
      gothic.isDialogActiveFn=std::move(previous);
      }
    TestBench::flushOutput();
    gothic.clearGame().reset();
    std::filesystem::current_path(workingDirectory);
    std::filesystem::remove_all(output);
    std::fprintf(result,"{\"type\":\"%s\",\"passed\":true,\"cases\":%zu,\"assertions\":%zu}\n",
                 missingNetwork ? "body_watch_missing_net_test" : "body_watch_test",cases,assertions);
    std::fclose(result);
    return 0;
    } catch(const std::exception& error) {
    std::filesystem::current_path(workingDirectory);
    Json d(rapidjson::kObjectType);
    d.AddMember("type","body_watch_test",d.GetAllocator());
    d.AddMember("passed",false,d.GetAllocator());
    number(d,"cases",cases); number(d,"assertions",assertions);
    d.AddMember("failure",rapidjson::Value(error.what(),d.GetAllocator()),d.GetAllocator());
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    d.Accept(writer);
    std::fprintf(result,"%s\n",buffer.GetString());
    std::fclose(result);
    return 1;
    }
  }
