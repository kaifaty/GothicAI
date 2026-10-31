#include <Tempest/File>
#include <Tempest/Log>
#include <zenkit/Logger.hh>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include "commandline.h"
#include "gothic.h"
#include "gamemusic.h"
#include "resources.h"
#include "basestore.h"
#include "game/playercontrol.h"
#include "game/serialize.h"
#include "game/simulationstep.h"
#include "game/bodyobservations.h"
#include "game/bodycontroller.h"
#include "game/bodyarena.h"
#include "game/bodyarenageometry.h"
#include "game/bodytrajectory.h"
#include "testbench.h"
#include "stucklogger.h"
#include "world/objects/npc.h"
#include "world/world.h"
#include "world/waypoint.h"
#include "utils/workers.h"
#include "../ai_bridge/npccontroller.h"
#include "../ai_bridge/npcobserver.h"

#include <chrono>
#include <algorithm>
#include <cmath>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <fcntl.h>
#include <iostream>
#include <optional>
#include <poll.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <stdexcept>
#include <thread>
#include <unistd.h>

namespace {
using Clock = std::chrono::steady_clock;
using Json = rapidjson::Document;
using Allocator = rapidjson::Document::AllocatorType;

struct Options {
  std::string save, scenario, output = ".body-runner", mode = "lockstep";
  bool stdio = false, compactReplies = false, schedulingDiagnostics = false, serialWorkers = false, forkReset = false;
  bool forkPrefix = false;
};

Options options(int argc, const char** argv) {
  Options result;
  for(int i=1; i<argc; ++i) {
    std::string arg = argv[i];
    auto value = [&]() -> std::string {
      if(++i>=argc)
        throw std::runtime_error("missing value for " + arg);
      return argv[i];
    };
    if(arg=="--save") result.save = value();
    else if(arg=="--scenario") result.scenario = value();
    else if(arg=="--output") result.output = value();
    else if(arg=="--mode") result.mode = value();
    else if(arg=="--stdio") result.stdio = true;
    else if(arg=="--compact-replies") result.compactReplies = true;
    else if(arg=="--scheduling-diagnostics") result.schedulingDiagnostics = true;
    else if(arg=="--serial-workers") result.serialWorkers = true;
    else if(arg=="--fork-reset") result.forkReset = true;
    else if(arg=="--fork-prefix") result.forkPrefix = true;
  }
  if(result.save.empty() || result.scenario.empty())
    throw std::runtime_error("--save and --scenario are required");
  if(result.mode!="lockstep" && result.mode!="realtime")
    throw std::runtime_error("mode must be lockstep or realtime");
  if(result.forkReset && (!result.serialWorkers || !result.stdio))
    throw std::runtime_error("--fork-reset requires --serial-workers and --stdio");
  if(result.forkPrefix && !result.forkReset)
    throw std::runtime_error("--fork-prefix requires --fork-reset");
  result.save = std::filesystem::absolute(result.save).string();
  result.scenario = std::filesystem::absolute(result.scenario).string();
  result.output = std::filesystem::absolute(result.output).string();
  return result;
}

void string(Json& json, const char* key, const std::string& value) {
  auto& a = json.GetAllocator();
  json.AddMember(rapidjson::Value(key,a), rapidjson::Value(value.c_str(),a),a);
}

std::string encode(const Json& json) {
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  json.Accept(writer);
  return {buffer.GetString(),buffer.GetSize()};
}

std::string optionalString(const Json& command, const char* key, std::string fallback) {
  if(!command.HasMember(key))
    return fallback;
  if(!command[key].IsString())
    throw std::runtime_error(std::string(key) + " must be a string");
  return command[key].GetString();
}

bool priorityWrite(const Json& request) {
  if(request.HasParseError() || !request.IsObject() || !request.HasMember("cmd") || !request["cmd"].IsString())
    return false;
  const std::string_view command(request["cmd"].GetString(),request["cmd"].GetStringLength());
  return command=="body_action" || command=="body_goal" || command=="body_acquire" ||
         command=="body_heartbeat" || command=="body_release" || command=="body_disconnect" ||
         command=="pause" || command=="resume" || command=="close";
  }

class Runner final {
public:
  Runner(Gothic& gothic, Options options) : gothic(gothic), opts(std::move(options)) {}

  struct ResetPlan final {
    std::string scenario, mode, output;
    int64_t seed = -1;
    std::optional<BodyArena::Config> arena;
  };

  ResetPlan plan(const Json& command) const {
    const auto scenario = optionalString(command,"scenario",opts.scenario);
    const auto mode = optionalString(command,"mode",opts.mode);
    const auto output = optionalString(command,"output_dir",opts.output);
    if(mode!="lockstep" && mode!="realtime")
      throw std::runtime_error("mode must be lockstep or realtime");
    if(!std::filesystem::is_regular_file(scenario))
      throw std::runtime_error("scenario file is missing");
    int64_t seed = -1;
    if(command.HasMember("seed")) {
      if(!command["seed"].IsUint())
        throw std::runtime_error("seed must be uint32");
      seed = command["seed"].GetUint();
    }
    std::optional<BodyArena::Config> arenaConfig;
    if(command.HasMember("arena")) {
      const auto& arena=command["arena"];
      if(!arena.IsObject()) throw std::runtime_error("arena must be an object");
      arenaConfig.emplace();
      const auto textField=[&](const char* key,const std::string& fallback) {
        if(!arena.HasMember(key)) return fallback;
        if(!arena[key].IsString()) throw std::runtime_error(std::string("arena ")+key+" must be string");
        return std::string(arena[key].GetString());
      };
      const auto unsignedField=[&](const char* key,uint64_t fallback) {
        if(!arena.HasMember(key)) return fallback;
        if(!arena[key].IsUint()) throw std::runtime_error(std::string("arena ")+key+" must be uint32");
        return uint64_t(arena[key].GetUint());
      };
      arenaConfig->scenario=textField("scenario","native_duel");
      arenaConfig->weapon=textField("weapon","1h");
      arenaConfig->seed=uint32_t(unsignedField("seed",seed>=0 ? uint64_t(seed) : 17));
      arenaConfig->side=unsigned(unsignedField("side",0));
      arenaConfig->timeoutMs=unsignedField("timeout_ms",60000);
      if(arena.HasMember("distance_cm")) {
        if(!arena["distance_cm"].IsNumber()) throw std::runtime_error("arena distance_cm must be numeric");
        arenaConfig->distanceCm=arena["distance_cm"].GetFloat();
      }
      const auto numberField=[&](const char* key,float fallback) {
        if(!arena.HasMember(key)) return fallback;
        if(!arena[key].IsNumber()) throw std::runtime_error(std::string("arena ")+key+" must be numeric");
        return arena[key].GetFloat();
      };
      arenaConfig->yawOffsetDeg=numberField("yaw_offset_deg",0);
      arenaConfig->opponentYawOffsetDeg=numberField("opponent_yaw_offset_deg",0);
      arenaConfig->separationYawDeg=numberField("separation_yaw_deg",0);
      arenaConfig->geometry=textField("geometry","open");
      if(arena.HasMember("wall_clearance_cm")) {
        if(!arena["wall_clearance_cm"].IsNumber()) throw std::runtime_error("arena wall_clearance_cm must be numeric");
        arenaConfig->wallClearanceCm=arena["wall_clearance_cm"].GetFloat();
      }
      if(arena.HasMember("wall_tangent_sign")) {
        if(!arena["wall_tangent_sign"].IsInt()) throw std::runtime_error("arena wall_tangent_sign must be integer +1 or -1");
        arenaConfig->wallTangentSign=arena["wall_tangent_sign"].GetInt();
        }
      if(arena.HasMember("load_fixture")) {
        if(!arena["load_fixture"].IsBool()) throw std::runtime_error("arena load_fixture must be boolean");
        arenaConfig->loadFixture=arena["load_fixture"].GetBool();
        }
      if(arena.HasMember("opponent_owned")) {
        if(!arena["opponent_owned"].IsBool()) throw std::runtime_error("arena opponent_owned must be boolean");
        arenaConfig->opponentOwned=arena["opponent_owned"].GetBool();
        }
      if(arena.HasMember("reaction_hold")) {
        if(!arena["reaction_hold"].IsBool()) throw std::runtime_error("arena reaction_hold must be boolean");
        arenaConfig->reactionHold=arena["reaction_hold"].GetBool();
        }
      if(arena.HasMember("center_offset_cm")) {
        const auto& offset=arena["center_offset_cm"];
        if(!offset.IsArray() || offset.Size()!=2 || !offset[0].IsNumber() || !offset[1].IsNumber())
          throw std::runtime_error("arena center_offset_cm must be [forward,lateral] numeric");
        arenaConfig->centerOffsetCm={offset[0].GetFloat(),offset[1].GetFloat()};
      }
      seed=arenaConfig->seed;
      if((arenaConfig->weapon!="1h" && arenaConfig->weapon!="2h") ||
         (arenaConfig->weapon=="2h" && arenaConfig->scenario!="native_duel") ||
         arenaConfig->side>1 || !std::isfinite(arenaConfig->distanceCm) || arenaConfig->distanceCm<80 ||
         arenaConfig->distanceCm>2000 || arenaConfig->timeoutMs<33 || arenaConfig->timeoutMs>60000 ||
         !std::isfinite(arenaConfig->yawOffsetDeg) || std::abs(arenaConfig->yawOffsetDeg)>45 ||
         !std::isfinite(arenaConfig->opponentYawOffsetDeg) || std::abs(arenaConfig->opponentYawOffsetDeg)>45 ||
         !std::isfinite(arenaConfig->separationYawDeg) || std::abs(arenaConfig->separationYawDeg)>180 ||
         !std::isfinite(arenaConfig->centerOffsetCm[0]) || std::abs(arenaConfig->centerOffsetCm[0])>1000 ||
         !std::isfinite(arenaConfig->centerOffsetCm[1]) || std::abs(arenaConfig->centerOffsetCm[1])>1000 ||
         (arenaConfig->geometry!="open" && arenaConfig->geometry!="near_wall") ||
         (arenaConfig->geometry=="near_wall" && arenaConfig->separationYawDeg!=0) ||
         (arenaConfig->wallClearanceCm && (arenaConfig->geometry!="near_wall" || !std::isfinite(*arenaConfig->wallClearanceCm) ||
                                          *arenaConfig->wallClearanceCm<60.f || *arenaConfig->wallClearanceCm>180.f)) ||
         !BodyArenaGeometry::validTangentSelection(arenaConfig->wallTangentSign,arenaConfig->geometry=="near_wall",bool(arenaConfig->wallClearanceCm)) ||
         (arenaConfig->reactionHold && (!BodyArena::acceptsReactionScenario(arenaConfig->scenario) || arenaConfig->loadFixture || mode!="lockstep")) ||
         (arenaConfig->loadFixture && (arenaConfig->scenario!="native_duel" || arenaConfig->geometry!="open" ||
           arenaConfig->distanceCm>400 || arenaConfig->yawOffsetDeg!=0 || arenaConfig->opponentYawOffsetDeg!=0 ||
           arenaConfig->separationYawDeg!=0 || arenaConfig->centerOffsetCm[0]!=0 || arenaConfig->centerOffsetCm[1]!=0)) ||
         (arenaConfig->scenario!="distance" && arenaConfig->scenario!="attack" && arenaConfig->scenario!="block" &&
          arenaConfig->scenario!="native_duel" && arenaConfig->scenario!="reset_stress"))
        throw std::runtime_error("invalid bounded arena configuration");
    }
    return {scenario,mode,output,seed,arenaConfig};
  }

  void reset(const Json& command) {
    const auto p = plan(command);
    teardown();
    configureEpisode(p);
    // A process begins with C rand seed 1. Reconstruct that state for each independent episode,
    // separately from the script RNG reseeded by bodyduel. No wall-clock-derived randomness.
    std::srand(1);
    const auto from = Clock::now();
    loadWorld();
    beginEpisode(p,from);
  }

  // --fork-reset: the save is loaded once into a template process that never simulates; each
  // episode runs in a fork of it. Loading consults neither the bench nor the arena config, so
  // configuring them after the load reaches the same state as a full reset (checked by tests).
  void loadTemplate() {
    teardown();
    std::srand(1);
    loadWorld();
    templateReady = true;
  }
  bool hasTemplate() const { return templateReady; }
  // --fork-prefix: the scenario's opening (time of day, hero placement, waits, clearing) is the same
  // for every arena episode, and setting the time of day re-plans every routine in the world. The
  // template plays it once, stops right before `bodyarena` and forks there; see resetFromTemplate.
  bool wantsPrefix() const { return opts.forkPrefix && !prefixReady; }
  void preparePrefix(const Json& command) {
    const auto p = plan(command);
    if(!p.arena) throw std::runtime_error("--fork-prefix serves arena episodes only");
    const auto from = Clock::now();
    BodyObservations::reset(++observationGeneration);
    const auto output = std::filesystem::path(opts.output)/"fork-prefix";
    std::filesystem::create_directories(output);
    TestBench::configure(p.scenario,output.string(),p.seed);
    TestBench::holdBefore("bodyarena");
    SimulationStep::finishStep(0,*player);
    modeNow = "lockstep";
    outputNow = output.string();
    steps = 0;
    elapsedSimMs = 0;
    started = Clock::now();
    due = started;
    loaded = true;
    paused = false;
    for(unsigned step=0;step<300 && !TestBench::held() && !TestBench::done();++step)
      oneStep();
    if(!TestBench::held()) throw std::runtime_error("fork prefix did not reach bodyarena within 300 substeps");
    TestBench::flushOutput();
    prefixScenario = p.scenario;
    prefixReady = true;
    prefixWallMs = elapsed(from);
  }
  // In the template after each fork: the next episode gets new identities, as a full reset would.
  void noteForked() { ++generation; ++observationGeneration; }
  void resetFromTemplate(const Json& command) {
    const auto p = plan(command);
    const auto from = Clock::now();
    if(prefixReady) {
      // The template's opening already ran: this episode starts at its held `bodyarena` step, with
      // the step counters it would have had, a fresh observation generation and its own outputs.
      if(p.scenario!=prefixScenario || !p.arena) throw std::runtime_error("fork prefix serves only its scenario's arena episodes");
      const auto step = BodyObservations::currentStep();
      BodyObservations::reset(++observationGeneration);
      BodyObservations::beginStep(step);
      std::filesystem::create_directories(p.output);
      BodyArena::configure(*p.arena,generation+1);
      TestBench::retarget(p.output);
      beginEpisode(p,from,true);
      return;
    }
    BodyObservations::reset(++observationGeneration);
    configureEpisode(p);
    beginEpisode(p,from);
  }
  // A fork ending its episode: flush its own journals without tearing down the shared world.
  void flushForExit() {
    finishMeleeShadowSwings();
    saveSummary();
    observationFile.close(); timingFile.close(); controlFile.close(); arenaFile.close();
  }

private:
  void finishMeleeShadowSwings() {
    if(auto* world=gothic.world()) {
      // finishStep publishes animation after World::tick has run physics. Query
      // that final pose interval before any NPC drains/removes its shadow blade.
      world->physic()->flushShadowBlades();
      for(uint32_t i=0;i<world->npcCount();++i)
        world->npcById(i)->finishMeleeShadowSwing();
      }
  }

  void teardown() {
    BodyArena::interrupt("reset_requested");
    BodyArena::disable();
    BodyTrajectory::reset("reset");
    BodyController::reset("reset");
    BodyController::setSink({});
    BodyObservations::reset(++observationGeneration);
    observationFile.close(); timingFile.close(); controlFile.close(); arenaFile.close();
    watchedActors.clear(); watchRequested = false;
    arenaSetupWatch=false;
    player.reset();
    gothic.clearGame().reset();
    loaded = false;
  }

  void configureEpisode(const ResetPlan& p) {
    std::filesystem::create_directories(p.output);
    if(p.arena) BodyArena::configure(*p.arena,generation+1);
    TestBench::configure(p.scenario,p.output,p.seed);
  }

  void loadWorld() {
    Tempest::RFile file(opts.save);
    Serialize save(file);
    gothic.setGame(std::make_unique<GameSession>(save));
    if(gothic.world()==nullptr || gothic.player()==nullptr || gothic.world()->physic()==nullptr)
      throw std::runtime_error("save did not restore a playable world");
    player = std::make_unique<PlayerControl>();
    player->clearInput();
    gothic.gameSession()->setTimeMultiplyer(1.f);
    gothic.camera()->setViewport(640,480);
    player->clearFocus();
  }

  void beginEpisode(const ResetPlan& p,Clock::time_point from,bool fromPrefix=false) {
    const auto& mode=p.mode; const auto& output=p.output; const auto& arenaConfig=p.arena;
    (void)output;
    // Match the rendered load-finalize frame: animation/camera/bench initialize at dt=0.
    // A prefix fork already ran it, and its opening steps, in the template.
    if(!fromPrefix)
      SimulationStep::finishStep(0,*player);
    modeNow = mode;
    outputNow = output;
    generation++;
    if(!fromPrefix) {
      steps = 0;
      elapsedSimMs = 0;
    }
    resetWallMs = elapsed(from);
    started = Clock::now();
    due = started;
    loaded = true;
    paused = false;
    maxLagMs = 0;
    schedulingRows = schedulingDropped = schedulingTick = 0;
    observationFile.open(std::filesystem::path(outputNow)/"observations.jsonl",std::ios::trunc);
    timingFile.open(std::filesystem::path(outputNow)/"observer-timing.jsonl",std::ios::trunc);
    if(!observationFile || !timingFile)
      throw std::runtime_error("cannot create observation/timing journals");
    controlFile.open(std::filesystem::path(outputNow)/"body-control-events.jsonl",std::ios::trunc);
    if(!controlFile)
      throw std::runtime_error("cannot create body control journal");
    BodyController::setSink([this](std::string_view row) {
      controlFile.write(row.data(),std::streamsize(row.size()));
      controlFile.put('\n');
      controlFile.flush();
      if(!controlFile)
        throw std::runtime_error("body control journal write failed");
      BodyArena::noteControl(row);
      BodyTrajectory::noteControl(row);
    });
    BodyObservations::setSink([this](std::string_view row) {
      observationFile.write(row.data(),std::streamsize(row.size()));
      observationFile.put('\n');
      if(!observationFile)
        throw std::runtime_error("observation journal write failed");
    });
    if(arenaConfig) {
      arenaFile.open(std::filesystem::path(outputNow)/"arena-events.jsonl",std::ios::trunc);
      if(!arenaFile) throw std::runtime_error("cannot create arena journal");
      BodyArena::setSink([this](std::string_view row) {
        arenaFile.write(row.data(),std::streamsize(row.size())); arenaFile.put('\n'); arenaFile.flush();
        if(!arenaFile) throw std::runtime_error("arena journal write failed");
      });
      // The held `bodyarena` line runs where the template stopped: in the same step's bench pass.
      if(fromPrefix)
        TestBench::resumeHeld();
      for(unsigned warmup=0;warmup<300 && !BodyArena::ready() && !TestBench::done();++warmup)
        oneStep();
      if(!BodyArena::ready()) throw std::runtime_error("arena setup did not reach readiness within 300 substeps");
      Json watch(rapidjson::kObjectType);
      watch.AddMember("enabled",true,watch.GetAllocator());
      watch.AddMember("rate_hz",20,watch.GetAllocator());
      watch.AddMember("max_actors",arenaConfig->loadFixture ? 4 : 2,watch.GetAllocator());
      arenaSetupWatch=true; bodyWatch(watch); arenaSetupWatch=false;
      // The final bootstrap step completed world movement and animation before installing the
      // participants. Their canonical reset state is now stable; no NPC AI runs before acquisition.
      BodyObservations::sampleAll();
      BodyArena::sealInitialState();
      observationFile.flush();
      resetWallMs=elapsed(from);
      started=Clock::now()-std::chrono::milliseconds(elapsedSimMs); due=Clock::now();
    }
  }

public:
  void oneStep() {
    if(!loaded || paused || complete())
      return;
    if(BodyArena::enabled() && BodyArena::ready() && !BodyArena::started())
      return;
    if(BodyArena::reactionArmed()) return;
    if(modeNow=="realtime")
      maxLagMs = std::max(maxLagMs,elapsed(due));
    const auto tickDue=due;
    ++schedulingTick;
    schedulingEvent("tick_begin",Clock::now(),tickDue);
    const uint64_t dt = TestBench::frameDt(33);
    SimulationStep::worldPlayerStep(dt,*player);
    SimulationStep::finishStep(dt,*player);
    BodyArena::postStep();
    if(watchRequested) {
      const auto watchFrom = Clock::now();
      const auto actors = TestBench::bodyObservationActors();
      if(actors!=watchedActors) {
        const auto currentStep = BodyObservations::currentStep();
        BodyController::reset("actor_set_changed");
        BodyObservations::reset(++observationGeneration);
        BodyObservations::beginStep(currentStep);
        BodyObservations::watch(actors);
        watchedActors = actors;
        BodyController::setActors(actors);
      }
      const auto builderCost = BodyObservations::lastStepCostUs();
      const auto cost = builderCost+uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now()-watchFrom).count());
      timingFile << "{\"sim_step\":" << BodyObservations::currentStep()
                 << ",\"game_ms\":" << gothic.gameSession()->tickCount()
                 << ",\"wall_sample_us\":" << uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count())
                 << ",\"kind\":\"sample_step\",\"watch_enabled\":true,\"actors\":" << BodyObservations::watchedCount()
                 << ",\"builder_wall_us\":" << builderCost << ",\"total_observer_wall_us\":" << cost << "}\n";
      if(!timingFile)
        throw std::runtime_error("observer timing journal write failed");
    }
    steps++;
    elapsedSimMs += dt;
    due = started + std::chrono::milliseconds(elapsedSimMs);
    schedulingEvent("tick_end",Clock::now(),tickDue);
    if(TestBench::done() || BodyArena::complete())
      saveSummary();
  }

  void pace() {
    if(modeNow!="realtime")
      return;
    const auto now = Clock::now();
    if(now<due)
      std::this_thread::sleep_until(due);
    else
      maxLagMs = std::max(maxLagMs,elapsed(due));
  }

  Json status() const {
    Json json(rapidjson::kObjectType);
    auto& a = json.GetAllocator();
    json.AddMember("schema_version",1,a);
    json.AddMember("ok",true,a);
    json.AddMember("generation",generation,a);
    json.AddMember("game_ms",loaded ? gothic.gameSession()->tickCount() : uint64_t(0),a);
    json.AddMember("sim_step",steps,a);
    json.AddMember("done",loaded && (TestBench::done() || BodyArena::complete()),a);
    rapidjson::Value workerExecution(rapidjson::kObjectType);
    workerExecution.AddMember("serial",Workers::serialExecution(),a);
    workerExecution.AddMember("mode",rapidjson::Value(Workers::serialExecution() ? "caller_serial" : "default_parallel",a),a);
    workerExecution.AddMember("worker_pool_created",Workers::workerPoolCreated(),a);
    rapidjson::Value taskCap;
    if(Workers::serialExecution()) taskCap.SetUint(1);
    workerExecution.AddMember("native_task_parallelism_cap",taskCap,a);
    json.AddMember("worker_execution",workerExecution,a);
    // Read-only scheduling hint in the response's monotonic clock domain. A
    // held, paused, terminal or lockstep world has no autonomous next tick.
    if(realtime())
      json.AddMember("next_tick_wall_monotonic_ns",uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(due.time_since_epoch()).count()),a);
    if(opts.schedulingDiagnostics) {
      rapidjson::Value diagnostic(rapidjson::kObjectType);
      diagnostic.AddMember("enabled",true,a);
      diagnostic.AddMember("rows_emitted",schedulingRows,a);
      diagnostic.AddMember("dropped_total",schedulingDropped,a);
      diagnostic.AddMember("capacity",uint64_t(65536),a);
      diagnostic.AddMember("oracle_only",true,a);
      diagnostic.AddMember("journal",rapidjson::Value("observer-timing.jsonl",a),a);
      json.AddMember("scheduling_diagnostics",diagnostic,a);
      }
    json.AddMember("paused",paused,a);
    json.AddMember("infrastructure_failure",BodyController::infrastructureFailure(),a);
    string(json,"infrastructure_reason",BodyController::infrastructureReason());
    json.AddMember("loaded",loaded,a);
    json.AddMember("elapsed_wall_ms",loaded ? elapsed(started) : uint64_t(0),a);
    json.AddMember("reset_wall_ms",resetWallMs,a);
    json.AddMember("simulated_ms",elapsedSimMs,a);
    json.AddMember("max_realtime_lag_ms",maxLagMs,a);
    json.AddMember("native_choices",TestBench::nativeChoices(),a);
    json.AddMember("graphics_device_created",Resources::hasGraphicsDevice(),a);
    json.AddMember("window_created",false,a); // No Window/Application is constructed in this entry point.
    json.AddMember("audio_device_created",loaded && gothic.gameSession()->hasAudioDevice(),a);
    Json observationStats;
    observationStats.Parse(BodyObservations::statsJson().c_str());
    if(observationStats.HasParseError())
      throw std::runtime_error("invalid native observation statistics");
    json.AddMember("body_observation",rapidjson::Value(observationStats,a),a);
    Json controlStats;
    controlStats.Parse(BodyController::statusJson().c_str());
    if(controlStats.HasParseError())
      throw std::runtime_error("invalid native control statistics");
    json.AddMember("body_control",rapidjson::Value(controlStats,a),a);
    Json trajectoryStats;
    trajectoryStats.Parse(BodyTrajectory::statsJson().c_str());
    if(trajectoryStats.HasParseError())
      throw std::runtime_error("invalid native trajectory statistics");
    json.AddMember("body_trajectory",rapidjson::Value(trajectoryStats,a),a);
    if(BodyArena::enabled()) {
      Json arena; arena.Parse(BodyArena::statusJson().c_str());
      if(arena.HasParseError()) throw std::runtime_error("invalid arena status");
      json.AddMember("arena",rapidjson::Value(arena,a),a);
    }
    string(json,"mode",modeNow);
    string(json,"output_dir",outputNow);
    return json;
  }

  void bodyWatch(const Json& command) {
    if(!loaded)
      throw std::runtime_error("reset before body_watch");
    if(BodyArena::enabled() && !arenaSetupWatch)
      throw std::runtime_error("arena observation subscription is fixed by reset");
    if(!command.HasMember("enabled") || !command["enabled"].IsBool())
      throw std::runtime_error("body_watch enabled must be boolean");
    if(anyBodyOwner())
      throw std::runtime_error("release body owner at a safe boundary before changing body_watch");
    if(!command["enabled"].GetBool()) {
      BodyObservations::disable(); watchRequested = false; watchedActors.clear();
      observationFile.flush(); timingFile.flush();
      return;
    }
    const auto integer = [&](const char* key,unsigned fallback) {
      if(!command.HasMember(key)) return fallback;
      if(!command[key].IsUint()) throw std::runtime_error(std::string(key)+" must be uint32");
      return command[key].GetUint();
    };
    const auto rate = integer("rate_hz",20);
    if(rate!=10 && rate!=20)
      throw std::runtime_error("rate_hz must be 10 or 20");
    BodyObservations::Config config;
    config.periodMs = 1000/rate;
    config.maxActors = integer("max_actors",4);
    config.queueCapacity = integer("queue_capacity",256);
    if(command.HasMember("ray_range_cm")) {
      if(!command["ray_range_cm"].IsNumber()) throw std::runtime_error("ray_range_cm must be finite numeric");
      config.rayRangeCm = command["ray_range_cm"].GetFloat();
    }
    if(config.maxActors==0 || config.maxActors>4 || config.queueCapacity==0 || config.queueCapacity>256 ||
       !std::isfinite(config.rayRangeCm) || config.rayRangeCm<=0 || config.rayRangeCm>2000)
      throw std::runtime_error("invalid bounded body observation configuration");
    const auto currentStep = BodyObservations::currentStep();
    BodyController::reset("observation_subscription_changed");
    BodyObservations::reset(++observationGeneration);
    BodyObservations::beginStep(currentStep);
    BodyObservations::configure(config);
    watchRequested = true;
    watchedActors = TestBench::bodyObservationActors();
    BodyObservations::watch(watchedActors);
    BodyController::setActors(watchedActors);
  }

  Json bodyControl(const Json& request) {
    if(!loaded)
      throw std::runtime_error("reset before body control");
    Json json;
    Json wire;
    wire.CopyFrom(request,wire.GetAllocator());
    const auto operation = optionalString(request,"cmd","");
    if(BodyArena::reactionConfigured() && !BodyArena::complete() &&
       (BodyArena::reactionHeld() || (BodyArena::ready() && !BodyArena::started()))) {
      Json arena; arena.Parse(BodyArena::statusJson().c_str());
      const bool reaction=arena.HasMember("reaction") && arena["reaction"]["enabled"].GetBool();
      if(reaction && request.HasMember("entity_id") && request["entity_id"].IsUint64()) {
        const auto entity=request["entity_id"].GetUint64();
        const auto primary=arena["primary_entity_id"].GetUint64(), opponent=arena["opponent_entity_id"].GetUint64();
        if(entity==primary && (operation=="body_action" || operation=="body_goal" || operation=="body_release" || operation=="body_disconnect"))
          throw std::runtime_error("reaction defender commands require arena_reaction_activate");
        if(entity==opponent && operation=="body_action" &&
           (BodyArena::reactionArmed() || !request.HasMember("action_id") || !request["action_id"].IsUint64() || request["action_id"].GetUint64()!=6))
          throw std::runtime_error("reaction preparation permits only the opponent's real front attack");
        }
      }
    if(!BodyArena::complete() && operation=="body_acquire" && optionalString(request,"mode","")!=modeNow)
      throw std::runtime_error("body acquire mode must match runner mode");
    if(wire.HasMember("type")) {
      if(!wire["type"].IsString() || operation!=wire["type"].GetString())
        throw std::runtime_error("body control cmd/type mismatch");
    } else {
      string(wire,"type",operation);
    }
    std::string result;
    if(BodyArena::complete()) {
      // A late poll/heartbeat must observe the sealed state, not run a wall watchdog
      // that could cancel an unfinished segment or replace its pose after terminal.
      if(operation=="body_control_status") result=BodyController::statusJson();
      else if(operation=="body_events") result=BodyController::drainJson();
      else if(operation=="body_self_test") result=BodyController::selfTestJson();
      else if(operation=="body_acquire" || operation=="body_action" || operation=="body_goal" || operation=="body_heartbeat") {
        Json current; current.Parse(BodyController::statusJson().c_str());
        if(current.HasParseError() || !current.HasMember("actors") || !current["actors"].IsArray())
          throw std::runtime_error("invalid native terminal control status");
        const rapidjson::Value* actor=nullptr;
        if(request.HasMember("entity_id") && request["entity_id"].IsUint64())
          for(const auto& candidate:current["actors"].GetArray())
            if(candidate["entity_id"].GetUint64()==request["entity_id"].GetUint64()) { actor=&candidate; break; }
        Json refused(rapidjson::kObjectType); auto& allocator=refused.GetAllocator();
        string(refused,"type","body_control_result");
        for(const char* key:{"session_epoch","world_epoch"})
          refused.AddMember(rapidjson::Value(key,allocator),rapidjson::Value(current[key],allocator),allocator);
        for(const char* key:{"entity_id","control_generation","goal_version","target_entity_id","command_id","execution_id"}) {
          rapidjson::Value value(uint64_t(0));
          if(actor) value.CopyFrom((*actor)[key],allocator);
          refused.AddMember(rapidjson::Value(key,allocator),value,allocator);
        }
        if(request.HasMember("command_id") && request["command_id"].IsUint64())
          refused["command_id"].SetUint64(request["command_id"].GetUint64());
        string(refused,"owner",actor ? (*actor)["owner"].GetString() : "native");
        refused.AddMember("release_pending",actor && (*actor)["release_pending"].GetBool(),allocator);
        refused.AddMember("accepted",false,allocator); refused.AddMember("duplicate",false,allocator);
        string(refused,"reason","arena_terminal"); result=encode(refused);
      } else if(operation=="body_release" || operation=="body_disconnect") {
        result=BodyController::handleJson(wire); // explicit cleanup, rather than a read-only poll
      } else throw std::runtime_error("arena terminal state permits only status/events and explicit cleanup");
    } else result=BodyController::handleJson(wire);
    json.Parse(result.c_str());
    if(json.HasParseError() || !json.IsObject())
      throw std::runtime_error("invalid native control reply");
    // Preserve the exact common encoder value separately from the runner's status envelope.
    // This also lets validation feed native output directly into the ordinary typed bridge codec.
    Json nativeReply;
    nativeReply.CopyFrom(json,nativeReply.GetAllocator());
    json.AddMember("body_reply",rapidjson::Value(nativeReply,json.GetAllocator()),json.GetAllocator());
    auto current = status();
    for(auto it=current.MemberBegin();it!=current.MemberEnd();++it)
      if(!json.HasMember(it->name))
        json.AddMember(rapidjson::Value(it->name,json.GetAllocator()),rapidjson::Value(it->value,json.GetAllocator()),json.GetAllocator());
    return json;
  }

  void setPaused(bool value) {
    if(!loaded)
      throw std::runtime_error("reset before pause");
    if(BodyArena::complete())
      throw std::runtime_error("arena terminal state cannot pause or resume");
    paused = value;
    BodyController::pause(value);
    // Resuming must not catch up wall time spent paused in a burst of simulation ticks.
    started = Clock::now()-std::chrono::milliseconds(elapsedSimMs);
    due = Clock::now();
  }

  Json fixture(const Json& request) {
    // This entry point exists only in the bench runner. It is never exposed by AiBridge.
    if(BodyArena::enabled()) throw std::runtime_error("body fixtures are disabled for arena episodes");
    if(!loaded || !watchRequested)
      throw std::runtime_error("reset and body_watch before body_fixture");
    const auto id = [&](const char* field) {
      if(!request.HasMember(field) || !request[field].IsUint64() || request[field].GetUint64()==0)
        throw std::runtime_error(std::string(field)+" must be a positive entity ID");
      auto* npc = BodyObservations::findEntity(request[field].GetUint64());
      if(npc==nullptr)
        throw std::runtime_error(std::string("unknown fixture ")+field);
      return npc;
    };
    Npc* actor = id("entity_id");
    const auto kind = optionalString(request,"kind","prepare");
    std::vector<std::string> legacyReplies;
    Json geometry(rapidjson::kObjectType);
    const auto number = [&](const char* field,float fallback) {
      if(!request.HasMember(field))
        return fallback;
      if(!request[field].IsNumber())
        throw std::runtime_error(std::string(field)+" must be finite numeric");
      const auto value = request[field].GetFloat();
      if(!std::isfinite(value) || std::abs(value)>10000000.f)
        throw std::runtime_error(std::string(field)+" outside fixture bounds");
      return value;
    };
    if(kind=="prepare") {
      Npc* target = id("target_entity_id");
      if(actor==target)
        throw std::runtime_error("fixture target must differ from actor");
      unsigned action = 0;
      if(request.HasMember("action_id")) {
        if(!request["action_id"].IsUint() || request["action_id"].GetUint()>11)
          throw std::runtime_error("fixture action_id must be in [0,11]");
        action = request["action_id"].GetUint();
      }
      const float distance = number("distance_cm",action==1 ? 500.f : 150.f);
      const float yaw = number("yaw_offset_deg",action==2 ? 30.f : 0.f);
      if(distance<80.f || distance>2000.f)
        throw std::runtime_error("fixture distance_cm must be in [80,2000]");
      BodyController::fixturePrepare(*actor,*target,distance,yaw);
    } else if(kind=="hp") {
      if(!request.HasMember("hp") || !request["hp"].IsInt() || request["hp"].GetInt()<0 || request["hp"].GetInt()>1000000)
        throw std::runtime_error("fixture hp must be in [0,1000000]");
      actor->changeAttribute(ATR_HITPOINTS,request["hp"].GetInt()-actor->attribute(ATR_HITPOINTS),false);
    } else if(kind=="position") {
      actor->setPosition(number("x_cm",actor->position().x),number("y_cm",actor->position().y),number("z_cm",actor->position().z));
      actor->setDirection(number("yaw_deg",actor->rotation()));
      BodyObservations::discontinuity(*actor);
      actor->updateTransform();
    } else if(kind=="occluded") {
      Npc* target = request.HasMember("target_entity_id") ? id("target_entity_id") : actor->target();
      if(target==nullptr || actor==target)
        throw std::runtime_error("occlusion fixture requires a distinct current target");
      auto& world = actor->world();
      const auto oldActor = actor->position(), oldTarget = target->position();
      const auto oldYaw = actor->rotation(), oldTargetYaw = target->rotation();
      const auto place = [&](const Tempest::Vec3& first,const Tempest::Vec3& second) {
        actor->setPosition(first); target->setPosition(second);
        actor->setDirection(second-first); target->setDirection(first-second);
        actor->updateTransform(); target->updateTransform();
      };
      const auto ground = [&](const Tempest::Vec3& point) {
        auto hit = world.physic()->ray({point.x,point.y+150,point.z},{point.x,point.y-300,point.z});
        return hit.hasCol ? hit.v.y : NAN;
      };
      std::vector<const WayPoint*> points;
      world.findWayPoint(oldActor,[&](const WayPoint& point) { points.push_back(&point); return false; });
      std::sort(points.begin(),points.end(),[&](const WayPoint* first,const WayPoint* second) {
        const auto a=(first->pos-oldActor).quadLength(), b=(second->pos-oldActor).quadLength();
        return a!=b ? a<b : first->name<second->name;
      });
      bool found=false;
      size_t attempts=0;
      for(const auto* first:points) {
        if(found || attempts>=8192) break;
        const auto y0=ground(first->pos);
        if(!std::isfinite(y0) || std::abs(y0-first->pos.y)>120.f) continue;
        for(const auto* second:points) {
          const auto distance=(second->pos-first->pos).length();
          if(distance<200.f || distance>std::min(float(actor->handle().senses_range)-250.f,2500.f)) continue;
          const auto y1=ground(second->pos);
          if(!std::isfinite(y1) || std::abs(y1-second->pos.y)>120.f) continue;
          ++attempts;
          place({first->pos.x,y0,first->pos.z},{second->pos.x,y1,second->pos.z});
          const auto center=world.physic()->ray(actor->mapHeadBone(),target->collosionCenter());
          const auto head=world.physic()->ray(actor->mapHeadBone(),target->mapHeadBone());
          const auto melee=world.physic()->ray(actor->collosionCenter(),target->collosionCenter());
          if(center.hasCol && head.hasCol && melee.hasCol && center.hitFraction>0.01f &&
             center.hitFraction<0.99f && head.hitFraction>0.01f && head.hitFraction<0.99f &&
             !actor->canSeeNpc(*target,false)) {
            geometry.AddMember("center_ray_hit",true,geometry.GetAllocator());
            geometry.AddMember("head_ray_hit",true,geometry.GetAllocator());
            geometry.AddMember("melee_line_hit",true,geometry.GetAllocator());
            geometry.AddMember("target_visible",false,geometry.GetAllocator());
            geometry.AddMember("search_attempts",uint64_t(attempts),geometry.GetAllocator());
            found=true; break;
          }
          if(attempts>=8192) break;
        }
      }
      if(!found) {
        place(oldActor,oldTarget);
        actor->setDirection(oldYaw); target->setDirection(oldTargetYaw);
        throw std::runtime_error("bounded fixture search found no real occlusion");
      }
      BodyObservations::discontinuity(*actor); BodyObservations::discontinuity(*target);
    } else if(kind=="target_gone") {
      actor->setTarget(nullptr);
    } else if(kind=="weapon_undrawn") {
      actor->closeWeapon(true);
    } else if(kind=="lock_wait") {
      if(!request.HasMember("duration_ms") || !request["duration_ms"].IsUint() ||
         request["duration_ms"].GetUint()==0 || request["duration_ms"].GetUint()>1000)
        throw std::runtime_error("fixture duration_ms must be in [1,1000]");
      BodyController::fixtureLock(*actor,request["duration_ms"].GetUint());
    } else if(kind=="script_takeover") {
      const auto state = actor->world().script().findSymbolIndex("ZS_ATTACK");
      if(state==size_t(-1))
        throw std::runtime_error("fixture native attack state is missing");
      actor->startState(ScriptFn(uint32_t(state)),"");
    } else if(kind=="soft_action" || kind=="attach_control") {
      auto& world = actor->world();
      if(world.findNpcByInstance(actor->instanceSymbol())!=actor)
        throw std::runtime_error("legacy fixture needs an unambiguous script instance");
      const auto symbol = world.script().findSymbol(actor->instanceSymbol())->name();
      const auto emit = [&](std::string&& row) { legacyReplies.push_back(std::move(row)); };
      if(kind=="soft_action") {
        NpcObserver observer(emit,[](std::string_view) { return std::string_view{}; });
        Json message(rapidjson::kObjectType);
        string(message,"id","fixture-soft-action");
        string(message,"npc",symbol);
        string(message,"action","turn_to");
        observer.act(world,message);
      } else {
        NpcController controller(emit);
        std::string error;
        if(controller.attach(world,symbol,error)) {
          // This is a rejected-conflict probe, so never retain an unexpected second owner.
          controller.detach(world,symbol,"fixture_cleanup",error);
          throw std::runtime_error("legacy attach unexpectedly acquired the body");
        }
        legacyReplies.push_back(error);
      }
    } else if(kind=="dialog_control") {
      if(gothic.player()==nullptr)
        throw std::runtime_error("fixture dialogue player is missing");
      actor->startDialog(*gothic.player());
    } else if(kind=="restore_native") {
      actor->setExternalControl(false);
      if(request.HasMember("target_entity_id"))
        id("target_entity_id")->setExternalControl(false);
    } else {
      throw std::runtime_error("unknown body fixture kind");
    }
    auto result = status();
    string(result,"fixture_kind",kind);
    result.AddMember("fixture_entity_id",BodyObservations::entityId(*actor),result.GetAllocator());
    result.AddMember("fixture_test_only",true,result.GetAllocator());
    rapidjson::Value replies(rapidjson::kArrayType);
    for(const auto& row:legacyReplies)
      replies.PushBack(rapidjson::Value(row.c_str(),result.GetAllocator()),result.GetAllocator());
    result.AddMember("legacy_replies",replies,result.GetAllocator());
    if(!geometry.ObjectEmpty())
      result.AddMember("fixture_geometry",rapidjson::Value(geometry,result.GetAllocator()),result.GetAllocator());
    return result;
  }

  void pumpWatchdogs() {
    // The real post-step terminal capture is sealed. Idle transport polls must not
    // later expire a pending lease or replace its final pose; explicit cleanup owns that.
    if(BodyArena::complete()) return;
    BodyController::pumpWatchdogs(); BodyArena::postStep();
  }

  void providerLost() {
    for(auto* actor:watchedActors) {
      auto state = BodyController::snapshot(*actor);
      if(state.owned)
        BodyController::disconnect(state.provider);
    }
    // On a realtime pipe disconnect, finish the current legal lock before returning native.
    // Lockstep records infrastructure failure and never resumes the episode as a normal duel.
    while(realtime() && anyBodyOwner()) {
      pace();
      oneStep();
      pumpWatchdogs();
    }
    saveSummary();
  }

  bool anyBodyOwner() const {
    for(auto* actor:watchedActors)
      if(BodyController::owns(*actor))
        return true;
    return false;
  }

  Json observations(const Json& request) {
    if(request.HasMember("max") && (!request["max"].IsUint() || request["max"].GetUint()!=256))
      throw std::runtime_error("max must be 256; the configured bounded queue is drained in full");
    Json json;
    const auto packet = BodyObservations::drainJson();
    json.Parse(packet.c_str());
    if(json.HasParseError() || !json.IsObject())
      throw std::runtime_error("invalid native observation packet");
    auto current = status();
    for(auto it=current.MemberBegin();it!=current.MemberEnd();++it)
      if(!json.HasMember(it->name))
        json.AddMember(rapidjson::Value(it->name,json.GetAllocator()),rapidjson::Value(it->value,json.GetAllocator()),json.GetAllocator());
    return json;
  }

  void observationTransportCost(uint64_t wallUs) {
    if(!watchRequested)
      return;
    timingFile << "{\"kind\":\"packet_transport\",\"sim_step\":" << BodyObservations::currentStep()
               << ",\"game_ms\":" << gothic.gameSession()->tickCount()
               << ",\"watch_enabled\":true,\"actors\":" << BodyObservations::watchedCount()
               << ",\"builder_wall_us\":0,\"total_observer_wall_us\":" << wallUs << "}\n";
  }

  void trajectoryWatch(const Json& request) {
    if(!loaded) throw std::runtime_error("reset before body_trajectory_watch");
    if(!request.HasMember("enabled") || !request["enabled"].IsBool())
      throw std::runtime_error("body_trajectory_watch enabled must be boolean");
    const bool enabled=request["enabled"].GetBool();
    if(enabled && (!BodyObservations::enabled() || watchedActors.empty()))
      throw std::runtime_error("watch numeric actors before enabling trajectories");
    if(enabled && BodyArena::enabled() && BodyArena::started())
      throw std::runtime_error("enable trajectories before arena_start");
    size_t capacity=4096;
    if(request.HasMember("queue_capacity")) {
      if(!request["queue_capacity"].IsUint())
        throw std::runtime_error("trajectory queue_capacity must be uint32");
      capacity=request["queue_capacity"].GetUint();
    }
    BodyTrajectory::configure(enabled,capacity);
  }

  Json trajectoryEvents(const Json& request) const {
    if(!loaded) throw std::runtime_error("reset before body_trajectory_events");
    size_t maximum=256;
    if(request.HasMember("max")) {
      if(!request["max"].IsUint() || request["max"].GetUint()==0 || request["max"].GetUint()>256)
        throw std::runtime_error("trajectory max must be in [1,256]");
      maximum=request["max"].GetUint();
    }
    Json result;
    result.Parse(BodyTrajectory::drainJson(maximum).c_str());
    if(result.HasParseError() || !result.IsObject())
      throw std::runtime_error("invalid native trajectory packet");
    if(!result.HasMember("schema_version")) result.AddMember("schema_version",1,result.GetAllocator());
    if(!result.HasMember("ok")) result.AddMember("ok",true,result.GetAllocator());
    return result;
  }

  void saveSummary() const {
    if(!loaded)
      return;
    std::ofstream out(std::filesystem::path(outputNow)/"runner-summary.json");
    out << encode(status()) << '\n';
  }

  void close() {
    finishMeleeShadowSwings();
    saveSummary();
    BodyArena::interrupt("close_requested"); BodyArena::disable();
    BodyTrajectory::reset("close");
    BodyController::reset("close"); BodyController::setSink({});
    BodyObservations::disable(); BodyObservations::setSink({});
    observationFile.close(); timingFile.close(); controlFile.close(); arenaFile.close();
    player.reset();
    gothic.clearGame().reset();
    loaded = false;
  }

  bool realtime() const {
    // Reset bootstraps synchronously, then the ready world remains still while the
    // caller allocates policies/acquires leases. It must not busy-poll an old due tick.
    return loaded && !paused && modeNow=="realtime" && !complete() &&
           (!BodyArena::enabled() || BodyArena::started()) && !BodyArena::reactionArmed();
  }
  bool complete() const { return loaded && (TestBench::done() || BodyController::infrastructureFailure() || BodyArena::complete()); }
  void arenaStart(const Json& request) {
    if(!loaded || !BodyArena::enabled()) throw std::runtime_error("reset with arena before arena_start");
    BodyArena::start(optionalString(request,"provider","scripted"));
    if(modeNow=="realtime") {
      // Rebase only after a successful start. Model allocation/pre-start wall time
      // is not simulation debt, and must not produce a burst of catch-up AI ticks.
      const auto now=Clock::now();
      started=now-std::chrono::milliseconds(elapsedSimMs); due=now; maxLagMs=0;
    }
  }
  void arenaCheck() { BodyArena::postStep(); }
  void arenaReactionActivate(const Json& command) {
    if(!loaded || !BodyArena::enabled() || modeNow!="lockstep" || paused)
      throw std::runtime_error("reaction activation requires an unpaused lockstep arena");
    BodyArena::ReactionRequest request;
    const auto integer=[&](const char* key) {
      if(!command.HasMember(key) || !command[key].IsUint64()) throw std::runtime_error(std::string("reaction ")+key+" must be uint64");
      return command[key].GetUint64();
      };
    request.session=integer("session_epoch"); request.world=integer("world_epoch");
    request.primary=integer("primary_entity_id"); request.opponent=integer("opponent_entity_id");
    request.primaryGeneration=integer("primary_control_generation"); request.opponentGeneration=integer("opponent_control_generation");
    request.observation=integer("observation_seq"); request.game=integer("issued_game_ms");
    request.provider=optionalString(command,"provider","scripted"); request.mode=optionalString(command,"mode","lockstep");
    BodyArena::activateReaction(request);
    modeNow=request.mode;
    if(modeNow=="realtime") {
      const auto now=Clock::now(); started=now-std::chrono::milliseconds(elapsedSimMs); due=now; maxLagMs=0;
      }
    observationFile.flush(); controlFile.flush();
    }
  void arenaSnapshot() {
    if(!loaded || !BodyArena::enabled()) throw std::runtime_error("reset with arena before arena_status");
    BodyArena::journal("snapshot");
  }
  Json arenaStep(const Json& request) {
    if(!loaded || !BodyArena::enabled() || !BodyArena::ready() || !BodyArena::started())
      throw std::runtime_error("reset with arena before arena_step");
    if(modeNow!="lockstep" || paused) throw std::runtime_error("arena_step requires unpaused lockstep");
    if(!request.HasMember("substeps") || !request["substeps"].IsUint() || request["substeps"].GetUint()==0 || request["substeps"].GetUint()>2000)
      throw std::runtime_error("arena_step substeps must be 1..2000");
    const auto before=steps;
    for(unsigned i=0;i<request["substeps"].GetUint() && !complete() && !BodyArena::reactionArmed();++i) oneStep();
    auto result=status(); result.AddMember("advanced_substeps",steps-before,result.GetAllocator());
    observationFile.flush(); timingFile.flush();
    return result;
  }
  // Lockstep training transport: advance tick by tick until a watched actor's newest row
  // since the previous check would let a per-tick client decide, then return every row and
  // control event in the same reply. One round trip per decision instead of three per tick.
  // After each tick the control watchdogs are pumped exactly as that client's body_events poll
  // would; the stop predicate mirrors its eligibility check (lease, can_decide, valid mask).
  Json arenaAdvance(const Json& request) {
    if(!loaded || !BodyArena::enabled() || !BodyArena::ready() || !BodyArena::started())
      throw std::runtime_error("reset with arena before arena_advance");
    if(modeNow!="lockstep" || paused) throw std::runtime_error("arena_advance requires unpaused lockstep");
    if(!request.HasMember("max_substeps") || !request["max_substeps"].IsUint() ||
       request["max_substeps"].GetUint()==0 || request["max_substeps"].GetUint()>2000)
      throw std::runtime_error("arena_advance max_substeps must be 1..2000");
    if(!request.HasMember("until") || !request["until"].IsArray() || request["until"].Size()>4)
      throw std::runtime_error("arena_advance until must be an array of at most four watches");
    struct Watch final { uint64_t entity=0, session=0, world=0, generation=0; bool periodic=false; std::vector<unsigned> actions; };
    std::vector<Watch> watches;
    for(const auto& item:request["until"].GetArray()) {
      if(!item.IsObject()) throw std::runtime_error("arena_advance watch must be an object");
      Watch watch;
      const auto field=[&](const char* key) {
        if(!item.HasMember(key) || !item[key].IsUint64()) throw std::runtime_error(std::string("arena_advance watch ")+key+" must be uint64");
        return item[key].GetUint64();
        };
      watch.entity=field("entity_id"); watch.session=field("session_epoch");
      watch.world=field("world_epoch"); watch.generation=field("control_generation");
      if(!item.HasMember("periodic") || !item["periodic"].IsBool()) throw std::runtime_error("arena_advance watch periodic must be boolean");
      watch.periodic=item["periodic"].GetBool();
      if(item.HasMember("actions")) {
        if(!item["actions"].IsArray()) throw std::runtime_error("arena_advance watch actions must be an array");
        for(const auto& action:item["actions"].GetArray()) {
          if(!action.IsUint() || action.GetUint()>11) throw std::runtime_error("arena_advance watch action must be 0..11");
          watch.actions.push_back(action.GetUint());
          }
        }
      watches.push_back(std::move(watch));
      }
    // Rows queued since the client's last drain (e.g. by its commands) count for the first check.
    uint64_t mark=BodyObservations::queuedTotal()-BodyObservations::queuedCount();
    const auto before=steps;
    std::string reason="max_substeps";
    uint64_t decisionEntity=0;
    for(unsigned i=0;i<request["max_substeps"].GetUint();++i) {
      if(complete() || BodyArena::reactionArmed()) break;
      oneStep();
      if(!BodyArena::complete()) BodyController::pumpWatchdogs();
      const auto rows=BodyObservations::queuedSince(mark);
      mark=BodyObservations::queuedTotal();
      for(const auto& watch:watches) {
        const BodyObservations::QueuedRow* newest=nullptr;
        for(const auto& row:rows)
          if(row.entityId==watch.entity && row.sessionEpoch==watch.session &&
             row.worldEpoch==watch.world && row.controlGeneration==watch.generation)
            newest=&row;
        if(newest==nullptr || (watch.periodic && !newest->periodic) || !newest->canDecide || !newest->actionMaskValid)
          continue;
        if(!watch.actions.empty() &&
           std::none_of(watch.actions.begin(),watch.actions.end(),[&](unsigned action) { return newest->actionMask[action]; }))
          continue;
        decisionEntity=watch.entity;
        break;
        }
      if(decisionEntity!=0) { reason="decision"; break; }
      if(BodyObservations::queuedCount()*4>=BodyObservations::queueCapacity()*3) { reason="queue_bound"; break; }
      }
    if(complete()) reason="terminal";
    auto result=status(); auto& allocator=result.GetAllocator();
    result.AddMember("advanced_substeps",steps-before,allocator);
    result.AddMember("stop_reason",rapidjson::Value(reason.c_str(),allocator),allocator);
    result.AddMember("decision_entity_id",decisionEntity,allocator);
    Json observations; observations.Parse(BodyObservations::drainJson().c_str());
    Json events; events.Parse(BodyController::drainJson().c_str());
    if(observations.HasParseError() || !observations.IsObject() || events.HasParseError() || !events.IsObject())
      throw std::runtime_error("invalid native advance packet");
    for(const char* key:{"dropped_total","dropped_since_drain","event_dropped_total"})
      result.AddMember(rapidjson::Value((std::string("observation_")+key).c_str(),allocator),rapidjson::Value(observations[key],allocator),allocator);
    result.AddMember("observations",rapidjson::Value(observations["observations"],allocator),allocator);
    result.AddMember("events",rapidjson::Value(events["events"],allocator),allocator);
    result.AddMember("events_dropped_total",rapidjson::Value(events["dropped_total"],allocator),allocator);
    observationFile.flush(); timingFile.flush();
    return result;
  }
  bool isPaused() const { return paused; }
  bool isLoaded() const { return loaded; }
  Clock::time_point deadline() const { return due; }
  uint64_t step() const { return steps; }

  void schedulingInputRead(const Json& request,Clock::time_point readAt,bool beforeDue) {
    schedulingEvent("input_read",readAt,due,&request,beforeDue);
    }

  void compactReply(Json& response,std::string_view command) const {
    if(!opts.compactReplies || !loaded || !BodyArena::started() || complete() || BodyArena::reactionHeld()) return;
    // Initial/activation, terminal, error and fixture replies retain the full evidence.
    // Only recurring active RPC/status envelopes omit redundant diagnostic strings.
    if(command!="status" && command!="arena_status" && command!="body_observations" &&
       command!="body_control_status" && command!="body_events" && command!="body_acquire" &&
       command!="body_action" && command!="body_goal" && command!="body_heartbeat" &&
       command!="body_release" && command!="body_disconnect") return;
    const auto stripRng=[](rapidjson::Value& status) {
      if(!status.IsObject() || !status.HasMember("actors") || !status["actors"].IsArray()) return;
      for(auto& actor:status["actors"].GetArray()) {
        if(!actor.IsObject() || !actor.HasMember("purity_state") || !actor["purity_state"].IsObject()) continue;
        auto& purity=actor["purity_state"];
        if(purity.HasMember("rng_state") && purity["rng_state"].IsString()) purity.RemoveMember("rng_state");
        }
      };
    stripRng(response);
    for(const char* key:{"body_control","body_reply"})
      if(response.HasMember(key)) stripRng(response[key]);
    if(response.HasMember("arena") && response["arena"].IsObject())
      response["arena"].RemoveMember("initial_state");
    }

private:
  void schedulingEvent(std::string_view phase,Clock::time_point capturedAt,Clock::time_point tickDue,
                       const Json* request=nullptr,bool beforeDue=false) {
    if(!opts.schedulingDiagnostics || !loaded || !timingFile.is_open()) return;
    if(schedulingRows>=65536) { ++schedulingDropped; return; }
    const auto ns=[](Clock::time_point value) {
      return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(value.time_since_epoch()).count());
      };
    Json row(rapidjson::kObjectType); auto& a=row.GetAllocator();
    row.AddMember("schema_version",1,a); string(row,"kind","runner_scheduling");
    string(row,"phase",std::string(phase)); row.AddMember("oracle_only",true,a);
    row.AddMember("seq",++schedulingRows,a); row.AddMember("tick_id",schedulingTick,a);
    row.AddMember("game_ms",gothic.gameSession()->tickCount(),a); row.AddMember("sim_step",steps,a);
    row.AddMember("runner_generation",generation,a); row.AddMember("wall_monotonic_ns",ns(capturedAt),a);
    row.AddMember("tick_due_wall_monotonic_ns",ns(tickDue),a);
#ifdef __linux__
    string(row,"clock_domain","linux-clock-monotonic");
#else
    string(row,"clock_domain","cxx-steady-clock");
#endif
    if(request && request->IsObject()) {
      if(request->HasMember("id")) {
        const auto& id=(*request)["id"];
        if(id.IsInt64() || id.IsUint64() || (id.IsString() && id.GetStringLength()<=128))
          row.AddMember("request_id",rapidjson::Value(id,a),a);
        else row.AddMember("request_id_omitted",true,a);
        }
      if(request->HasMember("cmd") && (*request)["cmd"].IsString()) {
        if((*request)["cmd"].GetStringLength()<=128)
          row.AddMember("command",rapidjson::Value((*request)["cmd"],a),a);
        else row.AddMember("command_omitted",true,a);
        }
      row.AddMember("admitted_before_due_tick",beforeDue,a);
      row.AddMember("max_ready_commands_per_due_tick",1,a);
      }
    timingFile << encode(row) << '\n';
    if(!timingFile) throw std::runtime_error("runner scheduling diagnostic journal write failed");
    }

  static uint64_t elapsed(Clock::time_point from) {
    return uint64_t(std::max<int64_t>(0,std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-from).count()));
  }
  Gothic& gothic;
  std::unique_ptr<PlayerControl> player;
  Options opts;
  std::string modeNow = "lockstep", outputNow;
  uint64_t generation = 0, steps = 0, elapsedSimMs = 0, resetWallMs = 0, maxLagMs = 0;
  uint64_t observationGeneration = 0;
  uint64_t schedulingRows = 0, schedulingDropped = 0, schedulingTick = 0;
  Clock::time_point started{}, due{};
  bool loaded = false;
  bool templateReady = false;
  bool prefixReady = false;
  std::string prefixScenario;
  uint64_t prefixWallMs = 0;
  bool paused = false;
  bool watchRequested = false;
  bool arenaSetupWatch = false;
  std::vector<Npc*> watchedActors;
  std::ofstream observationFile, timingFile, controlFile, arenaFile;
};

struct Advance {
  Json request;
  uint64_t startStep = 0, targetStep = 0, choices = 0;
  bool untilDecision = false;
};

class CommandInput final {
public:
  enum class State { Waiting, Line, End };
  struct Read {
    State state = State::Waiting;
    std::string line;
  };

  CommandInput() {
    originalFlags = fcntl(STDIN_FILENO,F_GETFL);
    if(originalFlags<0 || fcntl(STDIN_FILENO,F_SETFL,originalFlags | O_NONBLOCK)<0)
      throw std::runtime_error("cannot configure nonblocking command input");
  }

  ~CommandInput() {
    if(originalFlags>=0)
      fcntl(STDIN_FILENO,F_SETFL,originalFlags);
  }

  Read next(int timeoutMs) {
    if(auto line = bufferedLine())
      return {State::Line,std::move(*line)};
    if(ended)
      return end();

    pollfd input{STDIN_FILENO,POLLIN,0};
    const int ready = poll(&input,1,timeoutMs);
    if(ready<0) {
      if(errno==EINTR)
        return {};
      throw std::runtime_error("command input poll failed");
    }
    if(ready==0)
      return {};
    if(input.revents & (POLLERR | POLLNVAL))
      throw std::runtime_error("command input descriptor failed");

    char chunk[65536];
    const auto count = read(STDIN_FILENO,chunk,sizeof(chunk));
    if(count<0) {
      if(errno==EAGAIN || errno==EWOULDBLOCK || errno==EINTR)
        return {};
      throw std::runtime_error("command input read failed");
    }
    if(count==0) {
      ended = true;
      return end();
    }
    buffer.append(chunk,size_t(count));
    if(auto line = bufferedLine())
      return {State::Line,std::move(*line)};
    return {};
  }

private:
  std::optional<std::string> bufferedLine() {
    const auto newline = buffer.find('\n');
    if((newline==std::string::npos && buffer.size()>MaxFrameBytes) ||
       (newline!=std::string::npos && newline>MaxFrameBytes))
      throw std::runtime_error("JSONL command frame exceeds 1 MiB");
    if(newline==std::string::npos)
      return std::nullopt;
    auto line = buffer.substr(0,newline);
    buffer.erase(0,newline+1);
    return line;
  }

  Read end() const {
    if(!buffer.empty())
      throw std::runtime_error("truncated JSONL command at EOF: newline is required");
    return {State::End,{}};
  }

  static constexpr size_t MaxFrameBytes = 1024*1024;
  int originalFlags = -1;
  bool ended = false;
  std::string buffer;
};

void echoId(Json& response, const Json& request) {
  if(request.HasMember("id"))
    response.AddMember("id",rapidjson::Value(request["id"],response.GetAllocator()),response.GetAllocator());
}

void send(std::FILE* output, Json& response) {
  const auto ns=uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count());
  if(response.HasMember("wall_monotonic_ns")) response["wall_monotonic_ns"].SetUint64(ns);
  else response.AddMember("wall_monotonic_ns",ns,response.GetAllocator());
#ifdef __linux__
  constexpr const char* domain="linux-clock-monotonic";
#else
  constexpr const char* domain="cxx-steady-clock";
#endif
  if(response.HasMember("clock_domain")) response["clock_domain"].SetString(domain,response.GetAllocator());
  else response.AddMember("clock_domain",rapidjson::Value(domain,response.GetAllocator()),response.GetAllocator());
  const auto json = encode(response) + "\n";
  std::fwrite(json.data(),1,json.size(),output);
  std::fflush(output);
}

Advance advance(const Json& request, const Runner& runner) {
  if(!runner.isLoaded())
    throw std::runtime_error("reset before advance");
  if(runner.isPaused())
    throw std::runtime_error("resume before advance");
  if(BodyArena::enabled() && !BodyArena::started())
    throw std::runtime_error("arena_start before advance");
  if(BodyArena::reactionHeld()) throw std::runtime_error("use arena_step for bounded reaction preparation");
  if(!request.HasMember("substeps") || !request["substeps"].IsUint64() ||
     request["substeps"].GetUint64()==0 || request["substeps"].GetUint64()>100000)
    throw std::runtime_error("substeps must be an integer in [1,100000]");
  if(request.HasMember("until_decision") && !request["until_decision"].IsBool())
    throw std::runtime_error("until_decision must be boolean");
  Advance pending;
  pending.request.CopyFrom(request,pending.request.GetAllocator());
  pending.startStep = runner.step();
  pending.targetStep = pending.startStep + request["substeps"].GetUint64();
  pending.choices = TestBench::nativeChoices();
  pending.untilDecision = request.HasMember("until_decision") && request["until_decision"].GetBool();
  return pending;
}

bool finished(const Advance& pending, const Runner& runner) {
  return runner.complete() || runner.step()>=pending.targetStep ||
         (pending.untilDecision && TestBench::nativeChoices()!=pending.choices);
}

void advanceReply(std::FILE* output, const Advance& pending, const Runner& runner) {
  auto response = runner.status();
  response.AddMember("steps_advanced",runner.step()-pending.startStep,response.GetAllocator());
  string(response,"stopped_on",BodyController::infrastructureFailure() ? "infrastructure_failure" : runner.complete() ? "completed" :
         (pending.untilDecision && TestBench::nativeChoices()!=pending.choices ? "native_decision" : "budget"));
  echoId(response,pending.request);
  send(output,response);
}
}

size_t threadCount() {
  size_t count = 0;
  for(const auto& entry:std::filesystem::directory_iterator("/proc/self/task")) { (void)entry; ++count; }
  return count;
}

// Template side of --fork-reset. Returns an exit code once the template is done. In a fork it
// returns -1, with `line` holding the reset that fork must perform and answer.
int serveForks(Runner& runner, std::FILE* protocol, std::string& line, int& forkPipe) {
  for(;;) {
    Json request;
    request.Parse(line.data(),line.size());
    const auto cmd = request.HasParseError() || !request.IsObject() ? std::string() : optionalString(request,"cmd","");
    if(cmd=="close") {
      auto response = runner.status();
      echoId(response,request);
      send(protocol,response);
      return 0;
    }
    if(cmd!="reset")
      throw std::runtime_error("fork-reset template accepts only reset or close");
    if(!runner.hasTemplate())
      runner.loadTemplate();
    if(runner.wantsPrefix())
      runner.preparePrefix(request);
    if(threadCount()!=1)
      throw std::runtime_error("fork-reset requires a single-threaded template");
    std::fflush(protocol); std::fflush(stdout); std::fflush(stderr);
    int fds[2];
    if(pipe(fds)!=0)
      throw std::runtime_error("fork-reset pipe failed");
    const pid_t pid = fork();
    if(pid<0)
      throw std::runtime_error("fork-reset fork failed");
    if(pid==0) {
      close(fds[0]);
      forkPipe = fds[1];
      return -1;
    }
    close(fds[1]);
    runner.noteForked();
    // The fork forwards the next reset/close line and exits; an empty pipe means it ended.
    std::string next;
    char chunk[65536];
    for(;;) {
      const auto count = read(fds[0],chunk,sizeof(chunk));
      if(count>0) { next.append(chunk,size_t(count)); continue; }
      if(count<0 && errno==EINTR) continue;
      break;
    }
    close(fds[0]);
    int status = 0;
    while(waitpid(pid,&status,0)<0 && errno==EINTR) {}
    if(next.empty())
      return WIFEXITED(status) && WEXITSTATUS(status)==0 ? 0 : 3;
    line = std::move(next);
  }
}

[[noreturn]] void forwardAndExit(int forkPipe, const std::string& line) {
  size_t offset = 0;
  while(offset<line.size()) {
    const auto count = write(forkPipe,line.data()+offset,line.size()-offset);
    if(count<0) {
      if(errno==EINTR) continue;
      _exit(4);
    }
    offset += size_t(count);
  }
  close(forkPipe);
  _exit(0);
}

int main(int argc, const char** argv) {
  // Reserve protocol stdout before redirecting all engine/library logging (including C stdio).
  std::FILE* protocol = fdopen(dup(STDOUT_FILENO),"w");
  dup2(STDERR_FILENO,STDOUT_FILENO);
  try {
    const auto opts = options(argc,argv);
    Workers::setSerialExecution(opts.serialWorkers);
    // Jolt steps on this thread unless asked otherwise: one schedule for lockstep and realtime runs,
    // and a single-threaded template for --fork-reset.
    setenv("OPENGOTHIC_PHYSICS_THREADS","0",0);
    // No background copy of the base to the store: --fork-reset needs a single thread, and the
    // copy would serialize every parallel runner on the store's lock.
    BaseStore::setPublication(false);
    std::filesystem::create_directories(opts.output);
    CommandLine commandLine(argc,argv);
    zenkit::Logger::set(zenkit::LogLevel::WARNING,Resources::zenkitLog);
    Resources resources;
    Gothic gothic;
    auto stuckLogger = StuckLogger::fromEnvironment();
    gothic.isNpcInDialogFn = [](const Npc*) { return false; };
    gothic.setupGlobalScripts();
    GameMusic music;
    Runner runner(gothic,opts);
    if(!opts.stdio) {
      Json reset(rapidjson::kObjectType);
      runner.reset(reset);
      while(!runner.complete()) {
        runner.pace();
        runner.oneStep();
      }
      runner.saveSummary();
      auto response=runner.status(); send(protocol,response);
      runner.close();
      return 0;
    }
    CommandInput input;
    int forkPipe = -1;
    std::optional<Advance> pending;
    std::optional<Clock::time_point> admittedForDue;
    for(;;) {
      runner.pumpWatchdogs();
      if(pending && finished(*pending,runner)) {
        advanceReply(protocol,*pending,runner);
        pending.reset();
      }
      CommandInput::Read frame;
      bool admittedBeforeDue=false;
      Clock::time_point inputReadAt{};
      Json request;
      bool requestParsed=false;
      if(runner.realtime() && Clock::now()>=runner.deadline()) {
        const auto tickDue=runner.deadline();
        if(!admittedForDue || *admittedForDue!=tickDue) {
          // At a safe step boundary, admit one already complete body/state write
          // before a catch-up tick can invalidate its observation. Never wait.
          frame=input.next(0); inputReadAt=Clock::now();
          if(frame.state==CommandInput::State::Line) {
            request.Parse(frame.line.data(),frame.line.size()); requestParsed=true;
            admittedBeforeDue=priorityWrite(request);
            // A queued read must observe the due post-step state. In particular,
            // returning old numeric rows before a forced catch-up tick creates
            // a stale anchor during the caller's inference. Preserve its true
            // read timestamp/state even though the query executes after the tick.
            runner.schedulingInputRead(request,inputReadAt,admittedBeforeDue);
            }
          else if(frame.state==CommandInput::State::End) admittedBeforeDue=true;
          if(admittedBeforeDue) admittedForDue=tickDue;
          }
        // The next pass must service this same due tick even under an endless
        // command queue. No clock debt is discarded and a step is indivisible.
        if(!admittedBeforeDue) runner.oneStep();
        }
      if(pending && finished(*pending,runner)) {
        advanceReply(protocol,*pending,runner);
        pending.reset();
      }
      // A finite idle poll also runs the wall watchdog in lockstep and while paused.
      int timeoutMs = 20;
      if(runner.realtime()) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(runner.deadline()-Clock::now()).count();
        timeoutMs = int(std::min<int64_t>(20,std::max<int64_t>(0,remaining)));
      }
      if(frame.state==CommandInput::State::Waiting) {
        frame=input.next(timeoutMs); inputReadAt=Clock::now();
        }
      if(frame.state==CommandInput::State::Waiting)
        continue;
      if(frame.state==CommandInput::State::End) {
        runner.providerLost();
        break;
      }
      if(!requestParsed) {
        request.Parse(frame.line.data(),frame.line.size());
        const bool dueOnRead=runner.realtime() && inputReadAt>=runner.deadline();
        if(dueOnRead) {
          // A slow preceding step or the idle poll can cross the next deadline.
          // Classify this complete frame against that same due tick as well.
          const auto tickDue=runner.deadline();
          admittedBeforeDue=(!admittedForDue || *admittedForDue!=tickDue) && priorityWrite(request);
          if(admittedBeforeDue) admittedForDue=tickDue;
          }
        runner.schedulingInputRead(request,inputReadAt,admittedBeforeDue);
        if(dueOnRead && !admittedBeforeDue) {
          runner.oneStep();
          if(pending && finished(*pending,runner)) {
            advanceReply(protocol,*pending,runner);
            pending.reset();
            }
          }
        }
      try {
        if(request.HasParseError() || !request.IsObject())
          throw std::runtime_error("expected a JSON command object");
        const auto cmd = optionalString(request,"cmd","");
        const bool controlCommand = cmd=="body_acquire" || cmd=="body_goal" || cmd=="body_action" ||
                                    cmd=="body_heartbeat" || cmd=="body_release" || cmd=="body_disconnect" ||
                                    cmd=="body_events" || cmd=="body_control_status" || cmd=="body_self_test" || cmd=="body_visibility_watch";
        if(pending && cmd!="status" && cmd!="close" && cmd!="body_observations" && cmd!="body_trajectory_events" && !controlCommand && cmd!="pause" && cmd!="resume")
          throw std::runtime_error("advance is pending");
        if(cmd=="advance") {
          pending.emplace(advance(request,runner));
          if(!runner.realtime()) {
            while(!finished(*pending,runner))
              runner.oneStep();
            advanceReply(protocol,*pending,runner);
            pending.reset();
          }
          continue;
        }
        if(cmd=="reset") {
          if(!opts.forkReset) {
            runner.reset(request);
          } else if(forkPipe>=0) {
            // This fork's episode is over: the template forks the next one from the pristine world.
            runner.flushForExit();
            forwardAndExit(forkPipe,frame.line);
          } else {
            std::string line = frame.line;
            const int code = serveForks(runner,protocol,line,forkPipe);
            if(code>=0)
              return code;
            request.Parse(line.data(),line.size());
            runner.resetFromTemplate(request);
          }
        }
        else if(cmd=="body_watch") runner.bodyWatch(request);
        else if(cmd=="body_trajectory_watch") runner.trajectoryWatch(request);
        else if(cmd=="pause" || cmd=="resume") runner.setPaused(cmd=="pause");
        else if(cmd=="arena_start") runner.arenaStart(request);
        else if(cmd=="arena_reaction_activate") runner.arenaReactionActivate(request);
        else if(cmd=="arena_status") runner.arenaSnapshot();
        else if(cmd=="arena_step") {
          auto response=runner.arenaStep(request);
          echoId(response,request); send(protocol,response); continue;
        }
        else if(cmd=="arena_advance") {
          auto response=runner.arenaAdvance(request);
          echoId(response,request); send(protocol,response); continue;
        }
        else if(cmd=="body_fixture") {
          auto response = runner.fixture(request);
          echoId(response,request);
          send(protocol,response);
          continue;
        }
        else if(controlCommand) {
          auto response = runner.bodyControl(request);
          echoId(response,request);
          runner.compactReply(response,cmd);
          send(protocol,response);
          continue;
        }
        else if(cmd=="body_observations") {
          const auto from = Clock::now();
          auto response = runner.observations(request);
          echoId(response,request);
          runner.compactReply(response,cmd);
          send(protocol,response);
          runner.observationTransportCost(uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now()-from).count()));
          continue;
        }
        else if(cmd=="body_trajectory_events") {
          auto response=runner.trajectoryEvents(request);
          echoId(response,request); send(protocol,response); continue;
        }
        else if(cmd!="status" && cmd!="close") throw std::runtime_error("unknown command");
        auto response = runner.status();
        echoId(response,request);
        runner.compactReply(response,cmd);
        // The caller may terminate the template as soon as it receives this
        // acknowledgement. All episode journals must be complete first.
        if(cmd=="close") {
          if(forkPipe>=0) runner.flushForExit(); else runner.close();
        }
        send(protocol,response);
        if(cmd=="close") {
          if(forkPipe>=0) _exit(0);
          return 0;
        }
      } catch(const std::exception& e) {
        if(request.IsObject() && request.HasMember("cmd") && request["cmd"].IsString() && std::string_view(request["cmd"].GetString())=="reset")
          BodyArena::setupFailed(e.what());
        auto response = runner.status();
        response["ok"].SetBool(false);
        string(response,"error",e.what());
        if(request.IsObject()) echoId(response,request);
        send(protocol,response);
      }
    }
    if(forkPipe>=0) { runner.flushForExit(); _exit(0); }
    runner.close();
    return 0;
  } catch(const std::exception& e) {
    Json response(rapidjson::kObjectType);
    response.AddMember("schema_version",1,response.GetAllocator());
    response.AddMember("ok",false,response.GetAllocator());
    string(response,"error",e.what());
    send(protocol,response);
    return 2;
  }
}
