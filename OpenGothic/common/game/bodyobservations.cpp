#include "bodyobservations.h"
#include "bodycontroller.h"
#include "bodytrajectory.h"

#include "world/objects/npc.h"
#include "world/world.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <deque>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace {
using Tempest::Vec3;
using Clock = std::chrono::steady_clock;
constexpr size_t MaxEvents = 32;
constexpr size_t MaxEntityIds = 4096;
constexpr double Pi = 3.14159265358979323846;
constexpr const char* Phase = "post_simulation_post_animation";
constexpr const char* CapturePhase = "pre_native_primitive";

uint64_t processEpoch() {
  // Identity only: never used for velocity, TTL or comparisons with a remote host's clock.
  // Reading a clock once also preserves the native game's random sequence.
  static const uint64_t epoch = [] {
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::system_clock::now().time_since_epoch()).count();
    return ns>0 ? uint64_t(ns) : uint64_t(1);
    }();
  return epoch;
  }

uint64_t micros(Clock::time_point start) {
  return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now()-start).count());
  }

// Only fixed, source-owned field names and enum strings are emitted. All numeric fields are
// checked here, so neither Bullet's no-hit -inf sentinel nor corrupted native data becomes NaN JSON.
struct Json final {
  std::string s;
  Json() { s.reserve(5500); }
  void key(const char* name) { s += '"'; s += name; s += "\":"; }
  void boolean(bool v) { s += v ? "true" : "false"; }
  void integer(uint64_t v) { s += std::to_string(v); }
  void signedInteger(int64_t v) { s += std::to_string(v); }
  void number(double v) {
    if(!std::isfinite(v))
      throw std::runtime_error("body observation contains non-finite native number");
    char buf[48];
    std::snprintf(buf,sizeof(buf),"%.17g",v);
    s += buf;
    }
  void vector(const Vec3& v) { s += '['; number(v.x); s += ','; number(v.y); s += ','; number(v.z); s += ']'; }
  void null() { s += "null"; }
  void b(const char* name, bool v) { key(name); boolean(v); s += ','; }
  void u(const char* name, uint64_t v) { key(name); integer(v); s += ','; }
  void i(const char* name, int64_t v) { key(name); signedInteger(v); s += ','; }
  void f(const char* name, double v) { key(name); number(v); s += ','; }
  void v(const char* name, const Vec3& value) { key(name); vector(value); s += ','; }
  void nullableVector(const char* name, bool valid, const Vec3& value) { key(name); if(valid) vector(value); else null(); s += ','; }
  void optionalFloat(const char* name, bool valid, double value) { key(name); if(valid) number(value); else null(); s += ','; }
  void optionalInt(const char* name, bool valid, int64_t value) { key(name); if(valid) signedInteger(value); else null(); s += ','; }
  void optionalBool(const char* name, bool valid, bool value) { key(name); if(valid) boolean(value); else null(); s += ','; }
  void finishObject() { if(s.back()==',') s.pop_back(); s += '}'; }
  };

struct Event final {
  uint64_t id = 0, source = 0, time = 0, step = 0;
  int code = 0;
  bool self = true;
  };

struct Actor final {
  Npc* npc = nullptr;
  bool sampled = false, seen = false, seenVelocity = false, lastSampleVisible = false;
  bool motionDiscontinuity = false;
  bool lastPeriodic = false;
  uint64_t lastWallMs = 0, lastSequence = 0, lastTime = 0, lastBucket = 0, lastSeen = 0, targetId = 0, goalVersion = 0;
  BodyObservations::SentObservation sent, drained;
  Vec3 lastPosition = {}, lastSeenPosition = {}, lastSeenVelocity = {};
  std::array<uint64_t,18> fingerprint = {};
  std::deque<Event> events;
  Event lastResult;
  };

struct State final {
  BodyObservations::Config config;
  uint64_t generation = 0, step = 0, nextId = 0, sequence = 0, eventSequence = 0;
  bool enabled = false;
  std::vector<Actor> actors;
  std::unordered_map<const Npc*,uint64_t> ids;
  std::deque<std::string> queue;
  std::deque<BodyObservations::QueuedRow> queuedRows; // parallel to queue
  uint64_t queuedTotal = 0;
  std::function<void(std::string_view)> sink;
  uint64_t emitted = 0, dropped = 0, lastDrainDropped = 0, eventDropped = 0;
  uint64_t invalid = 0, missedPeriods = 0, calls = 0, totalCost = 0, stepCost = 0;
  uint64_t drainCost = 0, maxCost = 0;
  size_t stepActors = 0, maxQueue = 0;
  Actor* captureActor = nullptr;
  std::string* captureOutput = nullptr;
  uint64_t captureSequence = 0;
  };

// NPC destructors may run after other engine globals during shutdown. The process-lifetime
// holder avoids accessing destroyed maps from forget(); its bounded contents are cleared on reset.
State& state() { static State* s = new State; return *s; }

uint64_t idFor(const Npc& npc) {
  auto& s = state();
  auto it = s.ids.find(&npc);
  if(it==s.ids.end()) {
    if(s.ids.size()==MaxEntityIds)
      throw std::runtime_error("body observation entity registry limit exceeded");
    it = s.ids.emplace(&npc,++s.nextId).first;
    }
  return it->second;
  }

Actor* tracked(const Npc* npc) {
  for(auto& actor:state().actors)
    if(actor.npc==npc)
      return &actor;
  return nullptr;
  }

void clearActors() {
  auto& s = state();
  for(const auto& actor:s.actors)
    s.eventDropped += actor.events.size();
  s.actors.clear();
  }

void validateActors(const std::vector<Npc*>& actors) {
  if(actors.size()>state().config.maxActors)
    throw std::invalid_argument("body observation actor limit exceeded");
  for(size_t i=0;i<actors.size();++i) {
    auto end = actors.begin()+std::ptrdiff_t(i);
    if(actors[i]==nullptr || std::find(actors.begin(),end,actors[i])!=end)
      throw std::invalid_argument("null or duplicate body observation actor");
    }
  }

bool visible(const Npc& observer, const Npc& target) {
  return (observer.handlePtr()->senses & int32_t(SensesBit::SENSE_SEE))!=0 && observer.canSeeNpc(target,false);
  }

int eventCode(std::string_view event) {
  if(event=="animation_accepted") return 1;
  if(event=="animation_rejected") return 2;
  if(event=="applied") return 3;
  if(event=="out_of_range") return 4;
  if(event=="out_of_focus") return 5;
  if(event=="no_target") return 6;
  if(event=="blocked") return 7;
  if(event=="evaded") return 8;
  if(event=="damage_received") return 9;
  if(event=="damage_no_loss") return 10;
  if(event=="dead") return 11;
  if(event=="unconscious") return 12;
  return 0;
  }

void appendEvent(Actor& actor, const Event& event) {
  auto& s = state();
  if(actor.events.size()==MaxEvents) {
    actor.events.pop_front();
    ++s.eventDropped;
    }
  actor.events.push_back(event);
  if(event.self)
    actor.lastResult = event;
  }

void common(Json& j, const State& s, const char* phase = Phase) {
  j.u("schema_version",1);
  j.u("generation",s.generation);
  j.u("session_epoch",processEpoch());
  j.u("world_epoch",s.generation);
  j.u("control_generation",0);
  j.key("phase"); j.s += '"'; j.s += phase; j.s += "\",";
  j.u("sample_period_ms",s.config.periodMs);
  }

void poseFlags(Json& j, const Npc::BodyObservationState& p) {
  j.b("attack_anim",p.attackAnim);
  j.b("prehit",p.prehit);
  j.b("defence_active",p.defence);
  j.b("jumpback_active",p.jumpBack);
  j.b("def_window",p.defWindow);
  j.b("par_window",p.parWindow);
  j.b("interruptible",p.interruptible);
  }

void accountCost(uint64_t cost) {
  auto& s = state();
  s.totalCost += cost;
  s.stepCost += cost;
  s.maxCost = std::max(s.maxCost,s.stepCost);
  }
}

void BodyObservations::reset(uint64_t generation) {
  BodyTrajectory::reset("observation_reset");
  BodyController::reset("observation_reset");
  auto sink = std::move(state().sink);
  auto config = state().config;
  state() = State{};
  state().generation = generation;
  state().sink = std::move(sink);
  state().config = config;
  }

void BodyObservations::configure(const Config& config) {
  if((config.periodMs!=50 && config.periodMs!=100) || config.maxActors==0 || config.maxActors>4 ||
     config.queueCapacity==0 || config.queueCapacity>256 || !std::isfinite(config.rayRangeCm) ||
     config.rayRangeCm<=0 || config.rayRangeCm>2000)
    throw std::invalid_argument("invalid bounded body observation configuration");
  auto& s = state();
  if(s.enabled)
    throw std::logic_error("disable body observations before reconfiguration");
  s.config = config;
  while(s.queue.size()>config.queueCapacity) {
    s.queue.pop_front();
    s.queuedRows.pop_front();
    ++s.dropped;
    }
  }

void BodyObservations::watch(const std::vector<Npc*>& actors) {
  // Initial configuration deliberately starts every actor's history from scratch.
  validateActors(actors);
  clearActors();
  auto& s = state();
  for(auto* npc:actors) {
    Actor actor;
    actor.npc = npc;
    s.actors.push_back(actor);
    idFor(*npc);
    }
  s.enabled = !s.actors.empty();
  }

void BodyObservations::updateWatch(const std::vector<Npc*>& actors) {
  validateActors(actors);
  auto& s = state();
  std::vector<Actor> updated;
  updated.reserve(actors.size());
  for(auto* npc:actors) {
    if(auto actor=tracked(npc)) {
      updated.push_back(*actor);
      } else {
      Actor added;
      added.npc = npc;
      updated.push_back(std::move(added));
      }
    idFor(*npc);
    }
  for(const auto& actor:s.actors)
    if(std::find(actors.begin(),actors.end(),actor.npc)==actors.end())
      s.eventDropped += actor.events.size();
  // The bridge acknowledges watch before draining. Do not send removed actors afterwards,
  // including rows waiting behind socket backpressure; retained rows keep their original IDs.
  for(size_t i=s.queuedRows.size();i>0;--i) {
    const auto entity=s.queuedRows[i-1].entityId;
    if(std::any_of(actors.begin(),actors.end(),[&](const Npc* npc) { return s.ids.at(npc)==entity; }))
      continue;
    s.queue.erase(s.queue.begin()+std::ptrdiff_t(i-1));
    s.queuedRows.erase(s.queuedRows.begin()+std::ptrdiff_t(i-1));
    ++s.dropped;
    }
  s.actors = std::move(updated);
  s.enabled = !s.actors.empty();
  }

void BodyObservations::disable() { state().enabled = false; clearActors(); }
bool BodyObservations::enabled() { return state().enabled; }
size_t BodyObservations::watchedCount() { return state().actors.size(); }
size_t BodyObservations::queuedCount() { return state().queue.size(); }
size_t BodyObservations::queueCapacity() { return state().config.queueCapacity; }
uint64_t BodyObservations::samplePeriodMs() { return state().config.periodMs; }
void BodyObservations::setSink(std::function<void(std::string_view)> sink) { state().sink = std::move(sink); }
uint64_t BodyObservations::lastStepCostUs() { return state().stepCost; }
size_t BodyObservations::lastStepActorCount() { return state().stepActors; }
uint64_t BodyObservations::currentStep() { return state().step; }
uint64_t BodyObservations::entityId(const Npc& npc) { return idFor(npc); }
uint64_t BodyObservations::sessionEpoch() { return processEpoch(); }
uint64_t BodyObservations::worldEpoch() { return state().generation; }
Npc* BodyObservations::findEntity(uint64_t id) {
  for(const auto& pair:state().ids)
    if(pair.second==id)
      return const_cast<Npc*>(pair.first);
  return nullptr;
  }
bool BodyObservations::watched(const Npc& actor) { return tracked(&actor)!=nullptr; }
std::string BodyObservations::captureDecision(Npc& npc) {
  auto& s=state();
  const auto actor=tracked(&npc);
  if(!s.enabled || actor==nullptr) return {};
  if(s.captureActor!=nullptr) throw std::logic_error("recursive native body capture");
  Actor isolated=*actor;
  std::string result;
  s.captureActor=&isolated; s.captureOutput=&result;
  try { sample(npc,npc.world().tickCount(),true); }
  catch(...) { s.captureActor=nullptr; s.captureOutput=nullptr; throw; }
  s.captureActor=nullptr; s.captureOutput=nullptr;
  return result;
  }

uint64_t BodyObservations::latestSequence(const Npc& npc) { const auto a=tracked(&npc); return a ? a->lastSequence : 0; }
uint64_t BodyObservations::latestGameMs(const Npc& npc) { const auto a=tracked(&npc); return a ? a->lastTime : 0; }
bool BodyObservations::latestPeriodic(const Npc& npc) { const auto a=tracked(&npc); return a && a->lastPeriodic; }
uint64_t BodyObservations::latestWallMs(const Npc& npc) { const auto a=tracked(&npc); return a ? a->lastWallMs : 0; }
BodyObservations::SentObservation BodyObservations::latestSent(const Npc& npc) {
  const auto a=tracked(&npc);
  return a ? a->sent : SentObservation{};
  }

void BodyObservations::forget(const Npc* npc) {
  BodyTrajectory::forget(npc);
  BodyController::forget(npc);
  auto& s = state();
  s.ids.erase(npc);
  for(const auto& actor:s.actors)
    if(actor.npc==npc)
      s.eventDropped += actor.events.size();
  s.actors.erase(std::remove_if(s.actors.begin(),s.actors.end(),[npc](const Actor& a) { return a.npc==npc; }),s.actors.end());
  if(s.actors.empty())
    s.enabled = false;
  }

void BodyObservations::discontinuity(const Npc& npc) {
  for(auto& actor:state().actors) {
    if(actor.npc==&npc)
      actor.motionDiscontinuity = true;
    if(actor.npc->target()==&npc) {
      actor.lastSampleVisible = false;
      }
    }
  }

void BodyObservations::beginStep(uint64_t simStep) {
  state().step = simStep;
  state().stepCost = 0;
  state().stepActors = 0;
  }

void BodyObservations::noteEvent(const Npc& npc, const Npc* other, std::string_view event, uint64_t gameMs) {
  (void)other;
  auto& s = state();
  if(!s.enabled)
    return;
  const int code = eventCode(event);
  if(code==0)
    return;
  const auto start = Clock::now();
  // Never publish an unseen opponent's event. Own events convey no current target coordinates,
  // HP or pose; their code describes the observer's own attempted/experienced result.
  for(auto& actor:s.actors) {
    const bool self = actor.npc==&npc;
    if(!self && (actor.npc->target()!=&npc || !visible(*actor.npc,npc)))
      continue;
    appendEvent(actor,Event{++s.eventSequence,idFor(npc),gameMs,s.step,code,self});
    }
  accountCost(micros(start));
  }

void BodyObservations::sampleAll(bool force) {
  auto& s = state();
  if(!s.enabled)
    return;
  const auto start = Clock::now();
  ++s.calls;
  for(auto& actor:s.actors)
    sample(*actor.npc,actor.npc->world().tickCount(),force);
  accountCost(micros(start));
  }

void BodyObservations::sample(Npc& npc, uint64_t now, bool force) {
  auto& s = state();
  if(!s.enabled)
    return;
  const bool capture=s.captureActor!=nullptr;
  auto actor = capture ? s.captureActor : tracked(&npc);
  if(actor==nullptr)
    return;
  if(!capture && force && actor->sampled && actor->lastTime==now)
    return;
  const auto start = Clock::now();
  const auto own = npc.bodyObservationState();
  Npc* target = npc.target();
  const uint64_t targetId = target==nullptr ? 0 : idFor(*target);
  if(actor->targetId!=targetId) {
    actor->targetId = targetId;
    actor->seen = false;
    actor->seenVelocity = false;
    ++actor->goalVersion;
    }
  const bool seen = target!=nullptr && visible(npc,*target);
  Npc::BodyObservationState targetState;
  if(seen)
    targetState = target->bodyObservationState();
  auto control=BodyController::snapshot(npc);
  const std::array<uint64_t,18> fingerprint = {
    own.bodyState,uint64_t(npc.attribute(ATR_HITPOINTS)),own.weaponState,uint64_t(own.primitive),targetId,
    uint64_t(own.attackAnim),uint64_t(own.prehit),uint64_t(own.defence),uint64_t(own.jumpBack),
    uint64_t(own.faiGate)|(uint64_t(own.aniGate)<<1)|(uint64_t(own.aiGate)<<2)|(uint64_t(own.outputGate)<<3),
    uint64_t(own.interruptible)|(uint64_t(own.defWindow)<<1)|(uint64_t(own.parWindow)<<2),
    uint64_t(seen),seen ? targetState.bodyState : 0,
    uint64_t(seen && targetState.prehit),uint64_t(seen && targetState.attackAnim)|
    (uint64_t(seen && targetState.defence)<<1)|(uint64_t(seen && targetState.jumpBack)<<2)|
    (uint64_t(seen && targetState.defWindow)<<3)|(uint64_t(seen && targetState.parWindow)<<4),
    control.controlGeneration,control.commandId,control.goalVersion
    };
  const uint64_t bucket = now/s.config.periodMs;
  const bool periodic = !actor->sampled || bucket!=actor->lastBucket;
  const bool changed = !actor->sampled || actor->motionDiscontinuity || fingerprint!=actor->fingerprint;
  if(!force && !periodic && !changed && actor->events.empty())
    return;
  if(actor->sampled && now<actor->lastTime)
    throw std::logic_error("body observations require reset on game clock reversal");
  if(!capture && actor->sampled && bucket>actor->lastBucket+1)
    s.missedPeriods += bucket-actor->lastBucket-1;
  const uint64_t dt = actor->sampled ? now-actor->lastTime : 0;
  const Vec3 position = npc.position();
  const bool velocityValid = actor->sampled && dt>0 && !actor->motionDiscontinuity;
  const Vec3 velocity = velocityValid ? (position-actor->lastPosition)*(1000.f/float(dt)) : Vec3{};
  Vec3 targetPosition = {}, targetVelocity = {};
  bool targetVelocityValid = false;
  if(seen) {
    targetPosition = target->position();
    const bool previousSeen = actor->seen && actor->lastSampleVisible && actor->lastSeen<now;
    // A reappearing target has no current velocity until a second consecutive visible sample.
    if(previousSeen) {
      targetVelocity = (targetPosition-actor->lastSeenPosition)*(1000.f/float(now-actor->lastSeen));
      targetVelocityValid = true;
      }
    actor->lastSeenPosition = targetPosition;
    actor->lastSeenVelocity = targetVelocity;
    actor->seenVelocity = targetVelocityValid;
    actor->lastSeen = now;
    actor->seen = true;
    }
  const bool nativeEligible = own.nativeEligible && seen && !target->isDown();
  const auto captureResolver=capture ? BodyController::resolverState(npc) : FightAlgo::ResolverState{};
  const auto sharedMask=capture ? FightAlgo::actionMask(captureResolver) : std::array<bool,12>{};
  const bool captureEligible=captureResolver.eligible && captureResolver.targetValid;
  Npc::BodyObservationTarget targetProbe;
  if(seen)
    targetProbe = npc.bodyObservationTarget(*target);
  const std::array<bool,12> mask = {
    true,
    nativeEligible && own.hasMove,
    nativeEligible && npc.isRotationAllowed(),
    nativeEligible && own.hasLeft,
    nativeEligible && own.hasRight,
    nativeEligible && own.hasBack && targetProbe.focus30 && !npc.hasState(BS_PARADE),
    nativeEligible && own.hasAttack && targetProbe.focus5 && targetProbe.meleeLineClear,
    nativeEligible && own.hasAttackLeft && targetProbe.focus5 && targetProbe.inWRange && targetProbe.meleeLineClear,
    nativeEligible && own.hasAttackRight && targetProbe.focus5 && targetProbe.inWRange && targetProbe.meleeLineClear,
    nativeEligible && own.hasBlock && targetProbe.focus30 && !npc.hasState(BS_PARADE),
    nativeEligible && !targetProbe.inCloseup,
    nativeEligible
    };
  try {
    Json j;
    j.s = "{\"kind\":\"body_observation\",";
    common(j,s,capture ? CapturePhase : Phase);
    actor->lastWallMs = uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count());
    actor->lastSequence = capture ? ++s.captureSequence : ++s.sequence;
    control=BodyController::snapshot(npc);
    j.u("observation_seq",actor->lastSequence);
    j.u("entity_id",idFor(npc));
    if(control.owned) {
      const auto at=j.s.find("\"control_generation\":0,");
      if(at!=std::string::npos) j.s.replace(at,std::string_view("\"control_generation\":0,").size(),"\"control_generation\":"+std::to_string(control.controlGeneration)+",");
      }
    j.u("script_symbol",npc.instanceSymbol());
    j.u("t_game_ms",now); j.u("dt_game_ms",dt); j.u("sim_step",s.step);
    j.u("snapshot_wall_monotonic_ns",uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(start.time_since_epoch()).count()));
#ifdef __linux__
    j.s += "\"clock_domain\":\"linux-clock-monotonic\",";
#else
    j.s += "\"clock_domain\":\"cxx-steady-clock\",";
#endif
    j.b("periodic",periodic); j.b("event_only",!periodic); j.b("state_changed",changed);
    j.b("motion_discontinuity",actor->motionDiscontinuity);
    j.b("can_decide",capture ? captureEligible : control.canDecide); j.b("control_allowed",capture ? captureEligible : control.controlAllowed);
    j.b("native_decision_eligible",capture ? captureEligible : nativeEligible); j.b("action_mask_valid",capture || control.owned);
    j.s += (capture || control.owned) ? "\"mask_kind\":\"shared_melee_resolver\",\"availability_mask\":[" :
                           "\"mask_kind\":\"conservative_native_availability\",\"availability_mask\":[";
    const auto& publishedMask=capture ? sharedMask : control.owned ? control.mask : mask;
    for(size_t i=0;i<publishedMask.size();++i) { if(i>0) j.s += ','; j.boolean(publishedMask[i]); }
    if(capture || control.owned) {
      j.s += "],\"action_mask\":[";
      for(size_t i=0;i<publishedMask.size();++i) { if(i>0) j.s += ','; j.boolean(publishedMask[i]); }
      }

    j.s += "],\"self\":{";
    j.v("position_world_cm",position); j.nullableVector("linear_velocity_cm_s",velocityValid,velocity);
    j.b("velocity_valid",velocityValid); j.f("yaw_rad",npc.rotationRad());
    const int hp = npc.attribute(ATR_HITPOINTS), hpMax = npc.attribute(ATR_HITPOINTSMAX);
    j.i("hp",hp); j.i("hp_max",hpMax); j.f("hp_fraction",hpMax>0 ? double(hp)/hpMax : 0);
    j.u("weapon_state",own.weaponState); j.u("weapon_symbol",own.weaponSymbol);
    j.i("weapon_length_cm",own.weaponLength); j.i("skill",own.skill); j.u("skill_talent",own.skillTalent);
    j.b("skill_valid",own.skillTalent!=TALENT_UNKNOWN); j.i("fight_tactic",own.fightTactic);
    j.u("body_state_raw",own.bodyState); j.u("body_state_base",own.bodyState & uint32_t(BS_MAX));
    j.u("body_flags",own.bodyState & ~uint32_t(BS_MAX));
    j.b("dead",npc.isDead()); j.b("unconscious",npc.isUnconscious()); j.b("down",npc.isDown());
    j.b("in_air",npc.isInAir()); j.b("swim",npc.isSwim()); j.b("dive",npc.isDive());
    poseFlags(j,own);
    j.i("queued_primitive",own.primitive); j.u("choice_id",own.choiceId);
    j.s += "\"gates\":{";
    j.u("fai_wait_remaining_ms",own.faiWaitMs); j.u("ani_wait_remaining_ms",own.aniWaitMs);
    j.u("ai_wait_remaining_ms",own.aiWaitMs); j.u("output_wait_remaining_ms",own.outputWaitMs);
    j.b("fai_wait",own.faiGate); j.b("ani_wait",own.aniGate); j.b("ai_wait",own.aiGate); j.b("output_wait",own.outputGate);
    j.finishObject(); j.finishObject();
    j.s += ",\"target\":{";
    j.u("entity_id",targetId); j.b("present",target!=nullptr); j.b("visible",seen); j.b("current_valid",seen);
    j.nullableVector("position_world_cm",seen,targetPosition);
    j.nullableVector("relative_position_cm",seen,targetPosition-position);
    j.nullableVector("linear_velocity_cm_s",seen && targetVelocityValid,targetVelocity);
    j.nullableVector("relative_velocity_cm_s",seen && targetVelocityValid && velocityValid,targetVelocity-velocity);
    double yawError = 0;
    if(seen) {
      const auto delta = target->collosionCenter()-npc.collosionCenter();
      yawError = std::remainder(std::atan2(double(delta.z),double(delta.x))-npc.rotationRad(),2*Pi);
      }
    j.optionalFloat("yaw_error_rad",seen,yawError);
    j.optionalFloat("fight_distance_cm",seen,targetProbe.fightDistanceCm);
    j.optionalFloat("attack_range_cm",seen,targetProbe.attackRangeCm);
    j.optionalFloat("g_range_cm",seen,targetProbe.gRangeCm);
    j.optionalFloat("closeup_range_cm",seen,targetProbe.closeupRangeCm);
    j.optionalBool("in_w_range",seen,targetProbe.inWRange); j.optionalBool("in_g_range",seen,targetProbe.inGRange);
    j.optionalBool("in_closeup",seen,targetProbe.inCloseup); j.optionalBool("focus_30",seen,targetProbe.focus30);
    j.optionalBool("focus_5",seen,targetProbe.focus5); j.optionalBool("melee_line_clear",seen,targetProbe.meleeLineClear);
    const int targetHp = seen ? target->attribute(ATR_HITPOINTS) : 0;
    const int targetHpMax = seen ? target->attribute(ATR_HITPOINTSMAX) : 0;
    j.optionalInt("hp",seen,targetHp); j.optionalInt("hp_max",seen,targetHpMax);
    j.optionalFloat("hp_fraction",seen,targetHpMax>0 ? double(targetHp)/targetHpMax : 0);
    j.optionalInt("weapon_state",seen,targetState.weaponState); j.optionalInt("body_state_raw",seen,targetState.bodyState);
    j.optionalBool("attack_anim",seen,targetState.attackAnim); j.optionalBool("prehit",seen,targetState.prehit);
    j.optionalBool("defence_active",seen,targetState.defence); j.optionalBool("jumpback_active",seen,targetState.jumpBack);
    j.optionalBool("down",seen,seen && target->isDown());
    j.nullableVector("last_known_position_world_cm",actor->seen,actor->lastSeenPosition);
    j.nullableVector("last_known_velocity_cm_s",actor->seen && actor->seenVelocity,actor->lastSeenVelocity);
    j.optionalInt("last_seen_game_ms",actor->seen,int64_t(actor->lastSeen));
    j.optionalInt("last_known_age_ms",actor->seen,int64_t(now-actor->lastSeen));
    j.finishObject();
    j.s += ",\"goal\":{\"kind\":";
    j.s += target!=nullptr ? "\"native_combat\"," : "\"native_idle\",";
    j.u("version",control.owned ? control.goalVersion : actor->goalVersion); j.u("target_id",targetId); j.u("move_hint",uint8_t(npc.moveHint()));
    j.finishObject();
    j.s += ",\"last_result\":{";
    j.b("valid",actor->lastResult.code!=0); j.i("code",actor->lastResult.code);
    j.u("source_game_ms",actor->lastResult.time); j.u("source_sim_step",actor->lastResult.step); j.finishObject();
    j.s += ",\"local_obstacles\":{";
    j.f("ray_range_cm",s.config.rayRangeCm); j.s += "\"rays\":[";
    const Vec3 origin = npc.collosionCenter();
    for(size_t i=0;i<16;++i) {
      if(i>0) j.s += ',';
      const double angle = 2*Pi*double(i)/16;
      const double worldAngle = double(npc.rotationRad())+angle;
      const Vec3 end = origin+Vec3(float(std::cos(worldAngle)*s.config.rayRangeCm),0,float(std::sin(worldAngle)*s.config.rayRangeCm));
      const auto hit = npc.world().physic()->ray(origin,end);
      // Reading hit.v without hasCol would serialize the physics no-hit sentinel (-inf).
      const double distance = hit.hasCol ? std::clamp(double((hit.v-origin).length()),0.0,double(s.config.rayRangeCm)) : double(s.config.rayRangeCm);
      j.s += '{'; j.f("angle_rad",angle); j.f("distance_cm",distance); j.b("hit",hit.hasCol); j.finishObject();
      }
    j.s += "]},\"events\":[";
    for(size_t i=0;i<actor->events.size();++i) {
      if(i>0) j.s += ',';
      const auto& ev = actor->events[i];
      j.s += '{'; j.u("event_id",ev.id); j.u("source_entity_id",ev.source);
      j.u("source_game_ms",ev.time); j.u("source_sim_step",ev.step);
      j.key("perspective"); j.s += ev.self ? "\"self\"," : "\"visible_target\",";
      j.i("code",ev.code); j.finishObject();
      }
    j.s += "],";
    j.u("builder_wall_us",micros(start)); j.finishObject();
    if(capture) { *s.captureOutput=std::move(j.s); return; }
    // Journal receives exactly the same serialized row as the bounded IPC queue. Sink work is
    // included in sampleAll's simulation-thread cost, but IPC packet encoding is measured separately.
    if(s.sink)
      s.sink(j.s);
    BodyTrajectory::observe(j.s);
    if(s.queue.size()==s.config.queueCapacity) {
      s.queue.pop_front();
      s.queuedRows.pop_front();
      ++s.dropped;
      }
    s.queue.push_back(std::move(j.s));
    BodyObservations::QueuedRow queued;
    queued.entityId=idFor(npc); queued.sessionEpoch=processEpoch(); queued.worldEpoch=s.generation;
    queued.enqueueIndex=s.queuedTotal+1;
    queued.observation={actor->lastSequence,now,actor->lastWallMs};
    queued.controlGeneration=control.owned ? control.controlGeneration : 0;
    queued.periodic=periodic; queued.canDecide=control.canDecide; queued.actionMaskValid=control.owned;
    if(control.owned) queued.actionMask=control.mask;
    s.queuedRows.push_back(queued);
    ++s.queuedTotal;
    s.maxQueue = std::max(s.maxQueue,s.queue.size());
    ++s.emitted; ++s.stepActors;
    npc.traceVisibilityDiagnostic("post_observation");
    }
  catch(const std::runtime_error&) {
    if(!capture) { ++s.invalid; ++s.dropped; }
    throw;
    }
  actor->lastPosition = position;
  actor->lastTime = now;
  actor->lastBucket = bucket;
  actor->fingerprint = fingerprint;
  actor->sampled = true;
  actor->lastSampleVisible = seen;
  actor->lastPeriodic = periodic;
  actor->motionDiscontinuity = false;
  actor->events.clear();
  BodyController::onObservation(npc,s.queue.back());
  }

std::string BodyObservations::drainJson() {
  auto& s = state();
  const auto start = Clock::now();
  Json j;
  j.s = "{\"kind\":\"body_observations\",";
  common(j,s);
  j.u("queue_capacity",s.config.queueCapacity);
  j.u("dropped_total",s.dropped); j.u("dropped_since_drain",s.dropped-s.lastDrainDropped);
  j.u("event_dropped_total",s.eventDropped);
  j.s += "\"observations\":[";
  bool first = true;
  for(const auto& row:s.queue) {
    if(!first) j.s += ',';
    first = false;
    j.s += row;
    }
  j.s += "]}";
  for(auto& actor:s.actors) {
    actor.drained={};
    const auto entity=s.ids.at(actor.npc);
    for(const auto& row:s.queuedRows)
      if(row.entityId==entity) actor.drained=row.observation;
    }
  s.queue.clear();
  s.queuedRows.clear();
  s.lastDrainDropped = s.dropped;
  s.drainCost += micros(start);
  return std::move(j.s);
  }

void BodyObservations::markDrainedSent() {
  for(auto& actor:state().actors) {
    if(actor.drained.sequence>actor.sent.sequence) actor.sent=actor.drained;
    actor.drained={};
    }
  }

uint64_t BodyObservations::queuedTotal() { return state().queuedTotal; }

std::vector<BodyObservations::QueuedRow> BodyObservations::queuedSince(uint64_t total) {
  const auto& s = state();
  const auto first=std::find_if(s.queuedRows.begin(),s.queuedRows.end(),[total](const QueuedRow& row) {
    return row.enqueueIndex>total;
    });
  return std::vector<QueuedRow>(first,s.queuedRows.end());
  }

std::string BodyObservations::statsJson() {
  const auto& s = state();
  Json j;
  j.s = "{";
  j.b("enabled",s.enabled); j.u("generation",s.generation); j.u("session_epoch",processEpoch());
  j.u("world_epoch",s.generation); j.u("control_generation",0); j.u("sim_step",s.step);
  j.u("sample_period_ms",s.config.periodMs); j.u("watched_actors",s.actors.size());
  j.u("queue_capacity",s.config.queueCapacity); j.u("queue_size",s.queue.size()); j.u("max_queue_size",s.maxQueue);
  j.u("emitted",s.emitted); j.u("dropped_total",s.dropped); j.u("event_dropped_total",s.eventDropped);
  j.u("entity_registry_size",s.ids.size()); j.u("entity_registry_capacity",MaxEntityIds);
  j.u("invalid_numeric_rows",s.invalid); j.u("missed_period_buckets",s.missedPeriods); j.u("sample_all_calls",s.calls);
  j.u("last_cost_us",s.stepCost); j.u("current_step_wall_us",s.stepCost); j.u("last_step_actor_count",s.stepActors);
  j.u("total_wall_us",s.totalCost); j.u("max_step_wall_us",s.maxCost); j.u("drain_wall_us",s.drainCost);
  j.finishObject();
  return std::move(j.s);
  }
