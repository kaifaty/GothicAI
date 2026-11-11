#include <Tempest/File>
#include <Tempest/MemReader>
#include <Tempest/MemWriter>
#include <Tempest/Pixmap>

#include "commandline.h"
#include "gothic.h"
#include "gamemusic.h"
#include "resources.h"
#include "game/bodyclock.h"
#include "game/bodycontroller.h"
#include "game/bodyobservations.h"
#include "game/bodyenginepolicy.h"
#include "game/gamescript.h"
#include "game/playercontrol.h"
#include "game/serialize.h"
#include "game/simulationstep.h"
#include "physics/dynamicworld.h"
#include "testbench.h"
#include "utils/workers.h"
#include "world/objects/npc.h"
#include "world/objects/item.h"
#include "world/waypoint.h"
#include "world/world.h"

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
    BodyClock::setManual(BodyClock::time_point(std::chrono::seconds(1)));
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
  BodyClock::advance(std::chrono::milliseconds(500-20));
  uint64_t accepted=0;
  bool providerLost=false;
  bool watchdogExpired=false;
  restored=false;
  for(unsigned i=0;i<100 && BodyController::owns(actor);++i) {
    if(accepted && !watchdogExpired) {
      BodyClock::advance(std::chrono::milliseconds(40)); // 500 + 20 ms since acquire.
      watchdogExpired=true;
      }
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
    }
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
  expect(call(command(2),true)["accepted"].GetBool(),"realtime accepts the sent observation while a newer row is only sampled");
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
  BodyClock::advance(std::chrono::milliseconds(2000-20));
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
  expect(action(actor,FightAlgo::PolicyAction::AttackFront)["accepted"].GetBool(),"stale far fixture attack accepted");
  step(player);
  expect(actor.bodyObservationState().segmentRemainingMs>33,"stale far fixture starts a protected attack");
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
  Gothic::settingsSetI("GAME","melee_blade_shadow",1);
  Gothic::settingsSetI("GAME","melee_physics_hits",1);
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
    static void meleeAi(Npc& actor,Npc& target) {
      if(!actor.hasAnim("T_1HDASHB") || !actor.hasAnim("T_1HRIPOSTE") || !target.hasAnim("T_RIPOSTEVICTIM")) {
        std::cerr << "melee AI fixture skipped: needs OPENGOTHIC_CONTENT with anim-strikes\n";
        return;
        }
      const std::array<std::pair<const char*,int>,7> settings{{
        {"melee_dodge",1},{"melee_ai_dodge",1},{"melee_riposte",1},{"melee_ai_riposte",1},
        {"melee_stagger",1},{"melee_stamina",0},{"melee_parry_ai",0},
        }};
      std::array<int,settings.size()> saved{};
      for(size_t i=0;i<settings.size();++i) {
        saved[i]=Gothic::settingsGetI("GAME",settings[i].first);
        Gothic::settingsSetI("GAME",settings[i].first,settings[i].second);
        }
      const auto talent=actor.hitChance(TALENT_1H);
      const auto prepare=[&](float distance=150.f,float yaw=0.f) {
        for(auto* npc:{&actor,&target}) {
          npc->endMeleeStun("fixture"); npc->meleeRiposte.reset();
          npc->meleeRiposteHoldUntil=0; npc->meleeRiposteAiHeld=false; npc->meleeAiRiposteRolls.clear();
          }
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
      expect(actor.fghAlgo.queuedAction()==FightAlgo::MV_ATTACKR && actor.meleeAiRiposteRolls.empty(),
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
      Gothic::settingsSetI("GAME","melee_stamina",1);
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
      Gothic::settingsSetI("GAME","melee_stamina",0);

      ++cases;
      prepare();
      actor.setExternalControl(false);
      initialWatch(actor,699);
      expect(acquire(actor)["accepted"].GetBool(),"exhausted P30 fixture acquires its body lease");
      advance();
      BodyObservations::sampleAll(true);
      expect(action(actor,FightAlgo::PolicyAction::JumpBack)["accepted"].GetBool(),"exhausted P30 jump back is accepted");
      Gothic::settingsSetI("GAME","melee_stamina",1);
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
      Gothic::settingsSetI("GAME","melee_stamina",0);

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

      ++cases;
      prepare();
      actor.handle().hitchance[TALENT_1H]=100;
      target.startMeleeStun(actor,"posture");
      expect(target.isMeleeStunned(),"P30 riposte fixture has a stunned target");
      actor.implAttack(tickMs,true,FightAlgo::MV_ATTACK);
      expect(actor.isMeleeRiposting() && target.isMeleeRiposteHeld() && !actor.meleeAiRiposteRolls.empty(),
             "P30 direct attack on a stunned target reaches the real riposte roll and paired execution");
      prepare(300);
      actor.handle().hitchance[TALENT_1H]=talent;
      actor.meleeStamina={};
      actor.setExternalControl(false);
      for(size_t i=0;i<settings.size();++i)
        Gothic::settingsSetI("GAME",settings[i].first,saved[i]);
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
    heroTargetRelease(a,*gothic.player(),player,farFailures);
    staleFarRelease(a,b,*gothic.player(),player);
    for(const auto& failure:farFailures) std::cerr << "body-watch-test failure: " << failure << '\n';
    expect(farFailures.empty(),"far release/watchdog fixtures restore native ownership by the next engine tick");
    expect(!Resources::hasGraphicsDevice() && !gothic.gameSession()->hasAudioDevice(),"fixture creates no graphics/audio device");
    BodyController::reset();
    BodyObservations::disable();
    meleeRemoval(b);
    meleeShadowLoopChanges(b);
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
