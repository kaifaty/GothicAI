#include "bodycontroller.h"
#include "bodyobservations.h"
#include "bodyenginepolicy.h"
#include "world/objects/npc.h"
#include "world/objects/item.h"
#include "world/world.h"
#include "graphics/mesh/pose.h"
#include "testbench.h"
#include "frameprofile.h"

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
#include <algorithm>
#include <chrono>
#include <cmath>
#include <deque>
#include <memory>
#include <stdexcept>
#include <sstream>
#include <unordered_map>

namespace {
using Clock=std::chrono::steady_clock;
using PolicyAction=FightAlgo::PolicyAction;
using Json=rapidjson::Document;
constexpr size_t MaxEvents=512, MaxActors=4, MaxRetries=64;
// Bounded executor turn after a pose-cone loss. It stays below the 500 ms lease watchdog,
// so a collector waiting on the guard never outlives its own bound.
constexpr uint64_t VisibilityRecoveryMs=400;
struct Command final {
  uint64_t id=0, observation=0, goal=0, target=0, issued=0, ttl=0, execution=0;
  PolicyAction action=PolicyAction::Continue;
  FightAlgo::Action primitive=FightAlgo::MV_NULL;
  Clock::time_point deadline;
  bool started=false, consumed=false;
  uint8_t housekeepingCase=0; // telemetry only: trace changes of the real executor branch
  uint16_t comboLength=0;
  std::vector<Pose::CombatLayer> layers;
  std::string identity, beforeIdentity;
  };
struct Retry final { uint64_t id=0; std::string signature, result; };
struct Lease final {
  Npc* npc=nullptr;
  uint64_t entity=0, generation=0, goal=1, target=0, lastCommand=0, timeout=500, minimumSequence=0, acquireRequestId=0;
  std::string provider, mode="realtime", releaseReason;
  bool release=false, command=false, infrastructureEmitted=false, visibilityRecovery=false, movementStopPending=false;
  bool visibilityTurning=false;
  uint64_t visibilityRecoveryStep=0, visibilityRecoveryStart=0;
  Clock::time_point heartbeat;
  Command active;
  std::deque<Retry> retries;
  std::deque<std::function<void()>> deferred;
  std::unique_ptr<BodyEnginePolicy> policy;
  };
struct PendingAcquire final {
  Npc* npc=nullptr;
  uint64_t timeout=500;
  std::string provider,mode;
  bool enginePolicy=false;
  BodyController::BoundaryRequest boundary;
  };
struct State final {
  uint64_t nextGeneration=0, nextExecution=0, nextAcquireRequest=0, eventSequence=0, dropped=0;
  bool paused=false, infrastructure=false;
  bool visibilityDiagnostics=false, visibilityCaptureFailed=false;
  uint64_t visibilityGeneration=0, visibilityRecords=0, visibilityDropped=0;
  std::string infrastructureReason;
  std::vector<Npc*> actors;
  std::unordered_map<const Npc*,Lease> leases;
  std::unordered_map<const Npc*,PendingAcquire> pending;
  std::deque<std::string> events;
  std::deque<std::string> policyEvents;
  std::function<void(std::string_view)> sink;
  };
State& state() { static State* s=new State; return *s; }
uint64_t wallMs() { return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count()); }
uint64_t wallNs() { return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count()); }
#ifdef __linux__
constexpr const char* ClockDomain="linux-clock-monotonic";
#else
constexpr const char* ClockDomain="cxx-steady-clock";
#endif
std::string encode(const rapidjson::Value& value) {
  rapidjson::StringBuffer b;
  rapidjson::Writer<rapidjson::StringBuffer> w(b);
  value.Accept(w);
  return std::string(b.GetString(),b.GetSize());
  }
void str(Json& d,const char* key,std::string_view value) {
  auto& a=d.GetAllocator();
  d.AddMember(rapidjson::Value(key,a),rapidjson::Value(value.data(),rapidjson::SizeType(value.size()),a),a);
  }
void u(Json& d,const char* key,uint64_t value) { d.AddMember(rapidjson::Value(key,d.GetAllocator()),rapidjson::Value(value),d.GetAllocator()); }
void b(Json& d,const char* key,bool value) { d.AddMember(rapidjson::Value(key,d.GetAllocator()),rapidjson::Value(value),d.GetAllocator()); }
Json object() { Json d; d.SetObject(); return d; }
Lease* lease(const Npc& actor) { auto it=state().leases.find(&actor); return it==state().leases.end() ? nullptr : &it->second; }
void identity(Json& d,const Lease* l,const Npc* actor) {
  u(d,"session_epoch",BodyObservations::sessionEpoch()); u(d,"world_epoch",BodyObservations::worldEpoch());
  u(d,"entity_id",l ? l->entity : actor ? BodyObservations::entityId(*actor) : 0);
  u(d,"control_generation",l ? l->generation : 0); u(d,"goal_version",l ? l->goal : 0);
  u(d,"target_entity_id",l ? l->target : actor && actor->target() ? BodyObservations::entityId(*actor->target()) : 0);
  u(d,"command_id",l && l->command ? l->active.id : 0); u(d,"execution_id",l && l->command ? l->active.execution : 0);
  str(d,"owner",l ? "scripted" : "native"); b(d,"release_pending",l && l->release);
  if(l && l->acquireRequestId) u(d,"acquire_request_id",l->acquireRequestId);
  }
void enqueueEvent(const Json& d) {
  const auto row=encode(d);
  if(state().events.size()==MaxEvents) { state().events.pop_front(); ++state().dropped; }
  state().events.push_back(row);
  if(state().sink) state().sink(row);
  }
void policyReport(Lease& l,bool final=false,std::string_view reason={}) {
  if(!l.policy) return;
  auto encoded=l.policy->report(final);
  if(encoded.empty()) return;
  Json d; d.Parse(encoded.c_str());
  u(d,"session_epoch",BodyObservations::sessionEpoch()); u(d,"world_epoch",BodyObservations::worldEpoch());
  u(d,"entity_id",l.entity); u(d,"control_generation",l.generation); u(d,"goal_version",l.goal);
  u(d,"target_entity_id",l.target); u(d,"t_game_ms",l.npc->world().tickCount()); u(d,"t_wall_ms",wallMs());
  str(d,"reason",reason);
  if(state().policyEvents.size()==MaxEvents) state().policyEvents.pop_front();
  state().policyEvents.push_back(encode(d));
  }
void event(const Lease& l,std::string_view phase,std::string_view reason,const Command* command=nullptr,
           std::string_view acquireCancelReason={});
void cancelAcquire(PendingAcquire& pending,std::string_view reason) {
  pending.boundary.cancel();
  // Encode the same complete event schema as an ordinary lease event, while retaining
  // native ownership and generation zero: a waiting request never owned the body.
  Lease cancelled; cancelled.npc=pending.npc; cancelled.entity=BodyObservations::entityId(*pending.npc);
  cancelled.goal=0; cancelled.target=pending.boundary.target; cancelled.acquireRequestId=pending.boundary.id;
  cancelled.minimumSequence=BodyObservations::latestSequence(*pending.npc);
  cancelled.provider=pending.provider; cancelled.mode=pending.mode; cancelled.timeout=pending.timeout;
  event(cancelled,"ownership","acquire_cancelled",nullptr,reason);
  }
void cancelAcquire(const Npc* actor,std::string_view reason) {
  auto it=state().pending.find(actor);
  if(it==state().pending.end()) return;
  cancelAcquire(it->second,reason); state().pending.erase(it);
  }
void event(const Lease& l,std::string_view phase,std::string_view reason,const Command* command,
           std::string_view acquireCancelReason) {
  Json d=object(); u(d,"schema_version",1); str(d,"event","body_control_event"); str(d,"phase",phase); str(d,"reason",reason);
  identity(d,&l,l.npc);
  if(phase=="ownership" && (reason=="native_restored" || reason=="acquire_cancelled")) {
    d["owner"].SetString("native",d.GetAllocator()); d["release_pending"].SetBool(false);
    }
  if(!acquireCancelReason.empty()) str(d,"acquire_cancel_reason",acquireCancelReason);
  str(d,"provider",l.provider); str(d,"mode",l.mode);
  u(d,"event_seq",++state().eventSequence); u(d,"t_wall_ms",wallMs()); u(d,"t_game_ms",l.npc->world().tickCount());
  u(d,"wall_monotonic_ns",wallNs()); str(d,"clock_domain",ClockDomain);
  u(d,"last_heartbeat_wall_ms",uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(l.heartbeat.time_since_epoch()).count()));
  str(d,"pose_identity",l.npc->animNames());
  u(d,"sim_step",BodyObservations::currentStep());
  const auto& c=command ? *command : l.active;
  d["command_id"].SetUint64(command || l.command ? c.id : 0);
  d["execution_id"].SetUint64(command || l.command ? c.execution : 0);
  u(d,"observation_seq",phase=="ownership" && (reason=="acquired" || reason=="acquire_cancelled") && l.acquireRequestId ? l.minimumSequence :
                         command || l.command ? c.observation : 0);
  u(d,"action_id",uint64_t(c.action)); u(d,"effective_primitive",uint64_t(c.primitive));
  str(d,"segment_identity",c.identity); str(d,"pose_before",c.beforeIdentity); str(d,"pose_after",c.identity);
  str(d,"release_reason",l.releaseReason);
  const auto gates=l.npc->bodyObservationState();
  u(d,"remaining_lock_game_ms",std::max({gates.faiWaitMs,gates.aniWaitMs,gates.aiWaitMs,gates.outputWaitMs,gates.segmentRemainingMs}));
  b(d,"segment_started",c.started); b(d,"primitive_consumed",c.consumed);
  u(d,"issued_game_ms",c.issued); u(d,"ttl_ms",c.ttl); u(d,"watchdog_timeout_ms",l.timeout);
  b(d,"infrastructure_failure",state().infrastructure);
  enqueueEvent(d);
  }
void cancel(Lease& l,std::string_view reason) {
  if(l.command) {
    if(l.active.started && (l.active.action==PolicyAction::Approach || l.active.action==PolicyAction::TurnToTarget))
      l.movementStopPending=true;
    event(l,"cancelled",reason); l.command=false;
    }
  }
bool movementPrimitive(FightAlgo::Action primitive) {
  return primitive==FightAlgo::MV_MOVE || primitive==FightAlgo::MV_TURN2HIT ||
         primitive==FightAlgo::MV_STRAFEL || primitive==FightAlgo::MV_STRAFER || primitive==FightAlgo::MV_JUMPBACK;
  }
void requestRelease(Lease& l,std::string_view reason) {
  if(l.release) return;
  policyReport(l,true,reason);
  l.release=true; l.releaseReason=reason;
  if(l.active.started && movementPrimitive(l.active.primitive)) l.movementStopPending=true;
  cancel(l,reason);
  if(reason=="disconnect" || reason=="watchdog_timeout") event(l,"ownership","provider_lost");
  event(l,"ownership","release_pending");
  }
std::string result(const Lease* l,const Npc* actor,bool accepted,std::string_view reason,bool duplicate=false,uint64_t command=0) {
  Json d=object(); str(d,"type","body_control_result"); identity(d,l,actor);
  b(d,"accepted",accepted); b(d,"duplicate",duplicate); str(d,"reason",reason);
  if(command) d["command_id"].SetUint64(command);
  if(l && l->acquireRequestId) u(d,"observation_seq",l->minimumSequence);
  return encode(d);
  }
std::string pendingResult(const PendingAcquire& pending,bool duplicate=false) {
  Json d; d.Parse(result(nullptr,pending.npc,true,"acquire_pending",duplicate).c_str());
  d["target_entity_id"].SetUint64(pending.boundary.target);
  u(d,"acquire_request_id",pending.boundary.id);
  return encode(d);
  }
bool integer(const rapidjson::Value& d,const char* key,uint64_t& out,bool required=true) {
  if(!d.HasMember(key)) return !required;
  if(!d[key].IsUint64()) return false;
  out=d[key].GetUint64(); return true;
  }
std::string textField(const rapidjson::Value& d,const char* key) {
  return d.HasMember(key) && d[key].IsString() ? std::string(d[key].GetString(),d[key].GetStringLength()) : std::string();
  }
bool terminal(const Npc& actor) {
  const auto bs=actor.bodyStateMasked();
  // Script state may still be in the deferred native handoff when onNoHealth runs.
  return actor.attribute(ATR_HITPOINTS)<=0 || actor.isDown() || bs==BS_DEAD || bs==BS_UNCONSCIOUS;
  }
bool boundTarget(const Npc& actor,uint64_t target) {
  const auto t=actor.target();
  return t && !terminal(*t) && BodyObservations::entityId(*t)==target;
  }
bool targetValid(const Npc& actor,uint64_t target) {
  return boundTarget(actor,target) &&
         (actor.handlePtr()->senses & int32_t(SensesBit::SENSE_SEE))!=0 && actor.canSeeNpc(*actor.target(),false);
  }
bool safeBoundary(const Npc& npc) {
  const auto p=npc.bodyObservationState();
  return terminal(npc) || (!p.faiGate && !p.aniGate && !p.aiGate && !p.outputGate && p.segmentRemainingMs==0 && p.interruptible);
  }
std::string signature(const Command& c) {
  return std::to_string(c.observation)+":"+std::to_string(c.goal)+":"+std::to_string(c.target)+":"+
         std::to_string(c.issued)+":"+std::to_string(c.ttl)+":"+std::to_string(unsigned(c.action));
  }
void cache(Lease& l,const Command& c,const std::string& response) {
  l.lastCommand=c.id;
  if(l.retries.size()==MaxRetries) l.retries.pop_front();
  l.retries.push_back({c.id,signature(c),response});
  }
std::string applyAction(Lease& owner,const rapidjson::Value& req,uint64_t session,uint64_t world,
                        uint64_t generation,bool sentObservations,bool refreshHeartbeat) {
  auto l=&owner; auto actor=l->npc;
  Command c;
  uint64_t action=0;
  if(!integer(req,"command_id",c.id) || c.id==0 || !integer(req,"observation_seq",c.observation) || c.observation==0 ||
     !integer(req,"goal_version",c.goal) || !integer(req,"target_entity_id",c.target) || !integer(req,"action_id",action) || action>=12 ||
     !integer(req,"issued_game_ms",c.issued) || !integer(req,"ttl_ms",c.ttl) || c.ttl==0 || c.ttl>1000)
    return result(l,actor,false,"invalid_command");
  c.action=PolicyAction(action);
  if(c.id<=l->lastCommand) {
    for(const auto& retry:l->retries) if(retry.id==c.id) {
      if(retry.signature!=signature(c)) return result(l,actor,false,"duplicate_conflict",false,c.id);
      Json cached; cached.Parse(retry.result.c_str()); cached["duplicate"].SetBool(true); return encode(cached);
      }
    return result(l,actor,false,"out_of_order",false,c.id);
    }
  const auto reject=[&](std::string_view reason) {
    event(*l,"rejected",reason,&c); auto r=result(l,actor,false,reason,false,c.id); cache(*l,c,r); return r;
    };
  if(c.observation<=l->minimumSequence) return reject("stale_observation");
  BodyController::GateState gate;
  gate.session=session; gate.world=world; gate.generation=l->generation; gate.goal=l->goal; gate.target=l->target;
  auto observation=BodyObservations::SentObservation{BodyObservations::latestSequence(*actor),
                     BodyObservations::latestGameMs(*actor),BodyObservations::latestWallMs(*actor)};
  if(sentObservations && l->mode=="realtime") observation=BodyObservations::latestSent(*actor);
  gate.observation=observation.sequence; gate.observedGame=observation.gameMs;
  gate.gameNow=actor->world().tickCount(); gate.wallAge=wallMs()-observation.wallMs;
  gate.targetValid=targetValid(*actor,c.target); gate.paused=state().paused; gate.infrastructure=state().infrastructure;
  gate.busy=l->command || l->movementStopPending; gate.realtime=l->mode=="realtime"; gate.resolver=BodyController::resolverState(*actor);
  BodyController::GateCommand requested{session,world,generation,c.goal,c.target,c.observation,c.issued,c.ttl,c.action};
  const auto refusal=BodyController::validate(gate,requested);
  if(std::string_view(refusal)!="allowed") return reject(refusal);
  const auto resolution=FightAlgo::resolve(c.action,gate.resolver);
  c.primitive=resolution.primitive;
  c.deadline=Clock::time_point(std::chrono::milliseconds(observation.wallMs+c.ttl));
  if(l->mode=="realtime" && Clock::now()>=c.deadline) return reject("expired_wall");
  if(refreshHeartbeat) l->heartbeat=Clock::now();
  if(c.action==PolicyAction::Continue) {
    c.execution=l->command ? l->active.execution : 0;
    if(l->command) { c.identity=l->active.identity; c.beforeIdentity=l->active.beforeIdentity; c.started=l->active.started; c.consumed=l->active.consumed; }
    event(*l,"accepted","accepted",&c); event(*l,"continued",l->command ? "hold_execution" : "hold_idle",&c);
    event(*l,"completed","continue_acknowledged",&c);
    auto r=result(l,actor,true,"continued",false,c.id); cache(*l,c,r); return r;
    }
  c.execution=++state().nextExecution; l->active=std::move(c); l->command=true;
  event(*l,"accepted","accepted"); auto r=result(l,actor,true,"accepted"); cache(*l,l->active,r); return r;
  }

}

void BodyController::setActors(const std::vector<Npc*>& actors) {
  if(actors.size()>MaxActors) throw std::invalid_argument("body controller supports at most four actors");
  for(auto actor:actors) if(actor==nullptr) throw std::invalid_argument("null body actor");
  state().actors=actors;
  }

bool BodyController::hasOwners() { return !state().leases.empty() || !state().pending.empty(); }

const char* BodyController::updateWatch(const std::vector<Npc*>& actors, uint64_t periodMs) {
  if(actors.size()>MaxActors || (periodMs!=50 && periodMs!=100))
    return "body_watch needs up to 4 actors and rate_hz 10 or 20";
  for(size_t i=0;i<actors.size();++i)
    if(actors[i]==nullptr || std::find(actors.begin(),actors.begin()+std::ptrdiff_t(i),actors[i])!=actors.begin()+std::ptrdiff_t(i))
      return "null or duplicate body_watch actor";
  const auto included=[&](const auto& owners) {
    return std::all_of(owners.begin(),owners.end(),[&](const auto& owner) {
      return std::find(actors.begin(),actors.end(),owner.first)!=actors.end();
      });
    };
  if(!included(state().leases) || !included(state().pending))
    return "body_watch must retain active body owners and pending acquisitions";
  if(periodMs!=BodyObservations::samplePeriodMs())
    return "release active body owners before changing body_watch rate_hz";
  BodyObservations::updateWatch(actors);
  setActors(actors);
  return "allowed";
  }

FightAlgo::ResolverState BodyController::resolverState(const Npc& npc) {
  const auto own=npc.bodyObservationState();
  FightAlgo::ResolverState s;
  const auto target=npc.target();
  s.targetValid=target!=nullptr && !terminal(*target) &&
                (npc.handlePtr()->senses & int32_t(SensesBit::SENSE_SEE))!=0 && npc.canSeeNpc(*target,false);
  s.eligible=own.nativeEligible && !terminal(npc);
  s.sword=npc.weaponState()==WeaponState::W1H || npc.weaponState()==WeaponState::W2H;
  s.parade=npc.hasState(BS_PARADE); s.running=npc.bodyStateMasked()==BS_RUN;
  s.hasMove=own.hasMove; s.hasLeft=own.hasLeft; s.hasRight=own.hasRight; s.hasBack=own.hasBack;
  s.hasAttack=own.hasAttack; s.hasAttackLeft=own.hasAttackLeft; s.hasAttackRight=own.hasAttackRight; s.hasBlock=own.hasBlock;
  s.canRotate=npc.isRotationAllowed();
  if(s.targetValid) {
    const auto t=npc.bodyObservationTarget(*target);
    s.inWRange=t.inWRange; s.inCloseup=t.inCloseup; s.focus5=t.focus5; s.focus30=t.focus30; s.lineClear=t.meleeLineClear;
    }
  return s;
  }

bool BodyController::stopOwnedMovement(Npc& actor) {
  const auto l=lease(actor);
  if(!l || state().paused || state().infrastructure || terminal(actor))
    return false;
  const bool releaseMovement=l->release && l->movementStopPending;
  if(!releaseMovement && !safeBoundary(actor)) return false;
  if(l->release) {
    // A script queue or external-control takeover makes nativeEligible false, but must
    // not prevent returning authority to that very script. Keep protected segments,
    // physical reactions and waits that do not belong to the cancelled movement.
    const auto physical=actor.bodyObservationState();
    const auto bs=actor.bodyStateMasked();
    if(actor.isInAir() || actor.isSwim() || actor.isDive() || bs==BS_HIT || bs==BS_LIE ||
       bs==BS_STUMBLE || bs==BS_FALL || bs==BS_ITEMINTERACT || physical.attackAnim ||
       physical.segmentRemainingMs>0 || physical.aniGate || physical.aiGate || physical.outputGate)
      return false;
    // RUN/strafe/turn transitions are locomotion, not protected combat segments.
    // Stop their pose even when canInterrupt() is false, along with their own FAI wait.
    if(releaseMovement) actor.faiWaitTime=0;
    } else if(!actor.bodyObservationState().nativeEligible) return false;
  // Remove the original Run layer before selecting Idle. Reversing this order creates
  // a moving RUNL-to-RUN transition tagged STAND that stopWalkAnim cannot remove.
  // This is the same hard stop used by the native MOVE executor in weapon range.
  actor.visual.stopWalkAnim(actor);
  actor.setAnimRotate(0); actor.clearGoTo(); actor.wayPath.clear(); actor.stopWalking();
  return actor.isStanding();
  }

BodyController::Snapshot BodyController::snapshot(const Npc& actor) {
  Snapshot out;
  const auto l=lease(actor);
  if(!l) return out;
  out.owned=true; out.releasePending=l->release; out.controlGeneration=l->generation;
  out.goalVersion=l->goal; out.provider=l->provider;
  out.commandId=l->command ? l->active.id : 0; out.executionId=l->command ? l->active.execution : 0;
  out.controlAllowed=!terminal(actor) && !l->release && !state().paused && !state().infrastructure && targetValid(actor,l->target);
  out.mask=FightAlgo::actionMask(resolverState(actor));
  const bool fresh=BodyObservations::latestSequence(actor)>l->minimumSequence;
  out.canDecide=out.controlAllowed && fresh && !l->command && !l->movementStopPending && resolverState(actor).eligible;
  if(!out.controlAllowed || !fresh || l->command || l->movementStopPending) { out.mask.fill(false); out.mask[0]=out.controlAllowed && fresh; }
  return out;
  }

bool BodyController::owns(const Npc& actor) { return lease(actor)!=nullptr; }
void BodyController::activateBenchReaction(Npc& primary,Npc& opponent,bool native,std::string_view mode) {
  auto first=lease(primary), second=lease(opponent);
  if((mode!="lockstep" && mode!="realtime") || !first || !second || first->release || second->release ||
     first->command || first->movementStopPending || !safeBoundary(primary) ||
     !second->command || !second->active.started || second->active.action!=PolicyAction::AttackFront ||
     state().paused || state().infrastructure)
    throw std::invalid_argument("reaction leases are not ready for activation");
  const auto now=Clock::now();
  // Only accepted/started preparation attacks cross this boundary. Their physical
  // segment is uninterrupted; new realtime commands regain ordinary wall TTL checks.
  first->mode=std::string(mode); second->mode=std::string(mode);
  first->heartbeat=now; second->heartbeat=now;
  event(*first,"ownership","reaction_mode_armed"); event(*second,"ownership","reaction_mode_armed");
  if(native) {
    requestRelease(*first,"reaction_activation");
    pumpWatchdogs();
    if(owns(primary)) throw std::logic_error("idle reaction defender did not return to native authority");
    }
  }
void BodyController::takeover(Npc& actor,std::string_view reason) {
  cancelAcquire(&actor,reason);
  if(auto l=lease(actor)) requestRelease(*l,reason);
  }
bool BodyController::deferNative(Npc& actor,std::function<void()> mutation) {
  auto l=lease(actor); if(!l) return false;
  if(l->deferred.size()>=32) throw std::runtime_error("body deferred native mutation limit");
  l->deferred.push_back(std::move(mutation)); requestRelease(*l,terminal(actor) ? "actor_down" : "native_script_takeover"); return true;
  }
void BodyController::legacyRejected(Npc& actor,std::string_view reason) {
  if(auto l=lease(actor)) { Command rejected; event(*l,"rejected",reason,&rejected); }
  }
void BodyController::cancelPending(std::string_view reason) {
  for(auto& pair:state().pending) cancelAcquire(pair.second,reason);
  state().pending.clear();
  }
void BodyController::disconnect(std::string_view provider) {
  for(auto it=state().pending.begin();it!=state().pending.end();) {
    if(provider.empty() || it->second.provider==provider) { cancelAcquire(it->second,"disconnect"); it=state().pending.erase(it); }
    else ++it;
    }
  for(auto& pair:state().leases) if(provider.empty() || pair.second.provider==provider) requestRelease(pair.second,"disconnect");
  pumpWatchdogs();
  }
void BodyController::forget(const Npc* actor) {
  cancelAcquire(actor,"destroyed");
  auto it=state().leases.find(actor);
  if(it!=state().leases.end()) { cancel(it->second,"destroyed"); state().leases.erase(it); }
  auto& actors=state().actors;
  actors.erase(std::remove(actors.begin(),actors.end(),actor),actors.end());
  }
void BodyController::reset(std::string_view reason) {
  cancelPending(reason);
  for(auto& pair:state().leases) { cancel(pair.second,reason); event(pair.second,"ownership",reason); }
  // Final reset events have already reached the journal sink. Old-world IPC rows must not
  // be wrapped in the next world epoch; reconnect/load explicitly invalidate that stream.
  state().events.clear();
  state().policyEvents.clear();
  state().leases.clear(); state().pending.clear(); state().actors.clear(); state().infrastructure=false; state().infrastructureReason.clear();
  state().paused=false;
  state().visibilityDiagnostics=false; state().visibilityCaptureFailed=false;
  ++state().visibilityGeneration; state().visibilityRecords=state().visibilityDropped=0;
  }
void BodyController::pause(bool paused) {
  if(state().paused==paused) return;
  state().paused=paused;
  if(paused) {
    for(auto& pair:state().pending) cancelAcquire(pair.second,"pause");
    state().pending.clear();
    }
  if(paused) for(auto& pair:state().leases) requestRelease(pair.second,"pause");
  }
bool BodyController::isPaused() { return state().paused; }
bool BodyController::infrastructureFailure() { return state().infrastructure; }
std::string BodyController::infrastructureReason() { return state().infrastructureReason; }
void BodyController::setSink(std::function<void(std::string_view)> sink) { state().sink=std::move(sink); }
bool BodyController::visibilityDiagnosticsEnabled() { return state().visibilityDiagnostics; }
uint64_t BodyController::visibilityDiagnosticsGeneration() { return state().visibilityGeneration; }
void BodyController::visibilityDiagnosticRecorded(bool dropped) noexcept {
  if(dropped) ++state().visibilityDropped; else ++state().visibilityRecords;
  }
void BodyController::visibilityDiagnosticFailed() noexcept {
  state().visibilityCaptureFailed=true; state().visibilityDiagnostics=false; ++state().visibilityDropped;
  }
std::string BodyController::visibilityDiagnosticsJson() {
  Json d=object(); str(d,"type","body_visibility_diagnostics");
  b(d,"enabled",state().visibilityDiagnostics); b(d,"capture_failed",state().visibilityCaptureFailed);
  u(d,"diagnostic_generation",state().visibilityGeneration); u(d,"records_total",state().visibilityRecords);
  u(d,"dropped_total",state().visibilityDropped); u(d,"max_records_per_actor",8192); u(d,"sample_period_ms",50);
  str(d,"clock_domain",ClockDomain); str(d,"journal","events.jsonl"); str(d,"phase","visibility_diagnostic");
  return encode(d);
  }

void BodyController::pumpWatchdogs() {
  const auto now=Clock::now();
  // Cancellation/expiration is wall-clock driven even when simulation does not advance.
  // Only the actor's simulation tick may grant a waiting request before native combat runs.
  for(auto it=state().pending.begin();it!=state().pending.end();) {
    const auto actor=it->second.npc; ++it;
    pollAcquire(*actor,false);
    }
  for(auto it=state().leases.begin();it!=state().leases.end();) {
    auto& l=it->second;
    if(!l.release) policyReport(l);
    const bool isTerminal=terminal(*l.npc);
    if(isTerminal) requestRelease(l,"actor_down");
    if(!l.release && uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(now-l.heartbeat).count())>l.timeout)
      requestRelease(l,"watchdog_timeout");
    if(!isTerminal && l.release && l.mode=="lockstep" && (l.releaseReason=="watchdog_timeout" || l.releaseReason=="disconnect")) {
      state().infrastructure=true; state().infrastructureReason=l.releaseReason;
      if(!l.infrastructureEmitted) { event(l,"ownership","infrastructure_failure"); l.infrastructureEmitted=true; }
      // Keep the final locked state for the episode collector; never silently resume native.
      ++it; continue;
      }
    // Far policies disable ordinary near perception at 30 m. A combat segment is
    // measured in game time, so waiting for it here can outlive the provider's wall
    // release deadline even while the pose still advances. Return authority now;
    // only normalize poses that belong entirely to combat/locomotion.
    // Policy is updated after NPC ticks: an old far policy must not cut a segment
    // if the hero has just returned nearby, before that update.
    const auto hero=l.npc->world().player();
    const bool farRelease=l.release && !isTerminal && !state().paused &&
                          l.npc->processPolicy()>=NpcProcessPolicy::AiFar &&
                          hero!=nullptr && l.npc->qDistTo(*hero)>=3000.f*3000.f;
    if(farRelease) {
      auto& npc=*l.npc;
      npc.go2.clear(); npc.wayPath.clear(); l.movementStopPending=false;
      const auto layers=npc.visual.pose().combatLayers(npc.owner.tickCount());
      const bool combatOnly=std::all_of(layers.begin(),layers.end(),[](const Pose::CombatLayer& layer) {
        switch(layer.state & BS_MAX) {
          case BS_NONE: case (BS_WALK & BS_MAX): case (BS_SNEAK & BS_MAX):
          case BS_RUN: case BS_SPRINT: case (BS_HIT & BS_MAX): case BS_PARADE:
            return true;
          default: return false;
          }
        });
      if(combatOnly && !npc.ragdoll && !npc.isInAir() && !npc.isSwim() && !npc.isDive() &&
         !npc.spellFrozen() && !npc.spellWhirling() && npc.interactive()==nullptr) {
        npc.setAnimRotate(0); npc.visual.pose().stopAllAnim();
        npc.faiWaitTime=0; npc.aniWaitTime=0;
        npc.lastEventTime=npc.owner.tickCount();
        npc.setAnim(Npc::Anim::Idle);
        }
      }
    if(l.release && l.movementStopPending && !isTerminal && stopOwnedMovement(*l.npc))
      l.movementStopPending=false;
    if(l.release && (isTerminal || !state().paused) && (farRelease || safeBoundary(*l.npc))) {
      if(!isTerminal && l.movementStopPending) {
        if(!stopOwnedMovement(*l.npc)) { ++it; continue; }
        l.movementStopPending=false;
        }
      l.npc->fghAlgo.onClearTarget();
      auto npc=l.npc;
      // The normal path reaches Idle only after its legal combat boundary.
      if(isTerminal) {
        // onNoHealth already installed the dead/unconscious pose; clearing GoTo through
        // stopWalking() would overwrite it before the deferred native state is applied.
        npc->go2.clear(); npc->wayPath.clear();
        } else if(!farRelease) {
        npc->clearGoTo(); npc->setAnim(Npc::Anim::Idle);
        }
      event(l,"ownership","native_restored");
      auto deferred=std::move(l.deferred);
      it=state().leases.erase(it);
      for(auto& mutation:deferred) mutation();
      } else ++it;
    }
  }

std::string BodyController::statusJson(const Npc* actor,bool boundaryDiagnostics) {
  Json d=object(); str(d,"type","body_control_status");
  u(d,"session_epoch",BodyObservations::sessionEpoch()); u(d,"world_epoch",BodyObservations::worldEpoch());
  b(d,"paused",state().paused); b(d,"infrastructure_failure",state().infrastructure);
  Json diagnostics; diagnostics.Parse(visibilityDiagnosticsJson().c_str());
  d.AddMember("visibility_diagnostics",rapidjson::Value(diagnostics,d.GetAllocator()),d.GetAllocator());
  str(d,"infrastructure_reason",state().infrastructureReason);
  rapidjson::Value actors(rapidjson::kArrayType);
  auto add=[&](const Npc& npc) {
    const auto l=lease(npc); const auto s=snapshot(npc);
    Json row=object(); identity(row,l,&npc); str(row,"provider",l ? l->provider : ""); str(row,"mode",l ? l->mode : "native");
    const auto pending=state().pending.find(&npc);
    if(pending!=state().pending.end()) {
      u(row,"acquire_request_id",pending->second.boundary.id); b(row,"acquire_pending",true);
      row["provider"].SetString(pending->second.provider.c_str(),row.GetAllocator());
      row["mode"].SetString(pending->second.mode.c_str(),row.GetAllocator());
      row["target_entity_id"].SetUint64(pending->second.boundary.target);
      }
    u(row,"t_game_ms",npc.owner.tickCount()); u(row,"observation_seq",BodyObservations::latestSequence(npc));
    u(row,"observed_game_ms",BodyObservations::latestGameMs(npc)); u(row,"watchdog_timeout_ms",l ? l->timeout : 0);
    b(row,"control_allowed",s.controlAllowed); b(row,"can_decide",s.canDecide);
    if(boundaryDiagnostics) {
      const auto physical=npc.bodyObservationState();
      u(row,"process_policy",uint64_t(npc.processPolicy()));
      u(row,"segment_remaining_ms",physical.segmentRemainingMs);
      b(row,"fai_gate",physical.faiGate); b(row,"ani_gate",physical.aniGate);
      b(row,"ai_gate",physical.aiGate); b(row,"output_gate",physical.outputGate);
      b(row,"interruptible",physical.interruptible);
      }
    rapidjson::Value mask(rapidjson::kArrayType);
    for(bool value:s.mask) mask.PushBack(value,row.GetAllocator());
    row.AddMember("action_mask",mask,row.GetAllocator());
    if(TestBench::isActive()) {
    Json pure=object();
    str(pure,"pose_identity",npc.combatPoseIdentity());
    u(pure,"loop_next_time",npc.aiState.loopNextTime);
    u(pure,"fai_wait_until",npc.faiWaitTime); u(pure,"ani_wait_until",npc.aniWaitTime);
    rapidjson::Value queue(rapidjson::kArrayType);
    for(auto primitive:npc.fghAlgo.queueSnapshot()) queue.PushBack(uint64_t(primitive),pure.GetAllocator());
    pure.AddMember("queue",queue,pure.GetAllocator());
    str(pure,"rng_state",npc.owner.script().randomState(&npc));
    row.AddMember("purity_state",rapidjson::Value(pure,row.GetAllocator()),row.GetAllocator());
    }
    actors.PushBack(rapidjson::Value(row,d.GetAllocator()),d.GetAllocator());
    };
  if(actor) add(*actor); else for(auto npc:state().actors) add(*npc);
  d.AddMember("actors",actors,d.GetAllocator());
  return encode(d);
  }

std::string BodyController::drainJson() {
  Json d=object(); str(d,"type","body_control_events"); u(d,"session_epoch",BodyObservations::sessionEpoch());
  u(d,"world_epoch",BodyObservations::worldEpoch()); u(d,"dropped_total",state().dropped);
  rapidjson::Value events(rapidjson::kArrayType);
  for(const auto& encoded:state().events) {
    Json row; row.Parse(encoded.c_str()); events.PushBack(rapidjson::Value(row,d.GetAllocator()),d.GetAllocator());
    }
  state().events.clear(); d.AddMember("events",events,d.GetAllocator()); return encode(d);
  }

std::vector<std::string> BodyController::drainPolicyJson() {
  auto& queue=state().policyEvents;
  std::vector<std::string> reports;
  reports.reserve(queue.size());
  while(!queue.empty()) { reports.push_back(std::move(queue.front())); queue.pop_front(); }
  return reports;
  }

void BodyController::onObservation(Npc& actor,std::string_view encoded) {
  auto l=lease(actor);
  if(!l || !l->policy || l->release) return;
  FrameProfile::Scope timing(FrameProfile::Zone::BodyNet);
  Json row; row.Parse(encoded.data(),encoded.size());
  const auto reason=l->policy->process(row,actor.world().tickCount(),[&](int action,const rapidjson::Value& observation) {
    Json request=object();
    u(request,"command_id",l->lastCommand+1); u(request,"observation_seq",observation["observation_seq"].GetUint64());
    u(request,"goal_version",l->goal); u(request,"target_entity_id",l->target); u(request,"action_id",uint64_t(action));
    u(request,"issued_game_ms",observation["t_game_ms"].GetUint64()); u(request,"ttl_ms",100);
    // Identical acceptance, TTL, resolver and executor as a server action, using the
    // locally published row rather than waiting for a WebSocket send acknowledgement.
    Json receipt; receipt.Parse(applyAction(*l,request,BodyObservations::sessionEpoch(),
                              BodyObservations::worldEpoch(),l->generation,false,false).c_str());
    return receipt["accepted"].GetBool() ? std::string() : std::string(receipt["reason"].GetString());
    });
  if(reason.empty()) { policyReport(*l); return; }
  requestRelease(*l,reason);
  }

BodyController::AcquireState BodyController::acquireState(const Npc& actor,uint64_t target) {
  AcquireState acquire;
  acquire.targetValid=targetValid(actor,target); acquire.externalControl=actor.externalCtrl;
  acquire.player=actor.isPlayer(); acquire.talking=actor.isTalk(); acquire.terminal=terminal(actor);
  acquire.safeBoundary=safeBoundary(actor);
  acquire.nativeCombat=actor.aiState.started && std::string_view(actor.aiState.hint)=="ZS_ATTACK";
  acquire.queueEmpty=actor.aiQueue.size()==0; acquire.overlayEmpty=actor.aiQueueOverlay.size()==0;
  acquire.queueCombatOnly=actor.aiQueue.nativeCombatOnly(actor.target());
  acquire.gotoEmpty=actor.go2.empty();
  acquire.gotoToTarget=nativeCombatGoto(actor.go2.flag,actor.target(),actor.go2.nativeCombatTarget,
                                       actor.go2.npc,actor.go2.wp!=nullptr);
  return acquire;
  }

void BodyController::grantAcquire(Npc& actor,uint64_t target,uint64_t timeout,const std::string& provider,
                                  const std::string& mode,uint64_t requestId,bool enginePolicy) {
  Lease newLease; newLease.npc=&actor; newLease.entity=BodyObservations::entityId(actor);
  newLease.generation=++state().nextGeneration; newLease.acquireRequestId=requestId;
  newLease.minimumSequence=BodyObservations::latestSequence(actor);
  newLease.provider=provider; newLease.mode=mode; newLease.target=target; newLease.timeout=timeout;
  newLease.heartbeat=Clock::now();
  if(enginePolicy)
    newLease.policy=std::make_unique<BodyEnginePolicy>(BodyObservations::sessionEpoch(),BodyObservations::worldEpoch(),
                                                     newLease.entity,newLease.generation,actor.world().tickCount(),newLease.minimumSequence);
  // The caller validated only this target's replaceable native combat work, on a real
  // safe boundary. Neither pending requests nor refusal paths mutate native execution.
  if(actor.aiQueue.size()!=0) actor.aiQueue.clear();
  if(!actor.go2.empty()) { actor.go2.clear(); actor.wayPath.clear(); newLease.movementStopPending=true; }
  actor.fghAlgo.onClearTarget();
  auto l=&state().leases.emplace(&actor,std::move(newLease)).first->second;
  if(l->movementStopPending && stopOwnedMovement(actor)) l->movementStopPending=false;
  event(*l,"ownership","acquired");
  }

void BodyController::pollAcquire(Npc& actor,bool grant) {
  auto it=state().pending.find(&actor);
  if(it==state().pending.end()) return;
  auto acquire=acquireState(actor,it->second.boundary.target);
  if(!grant) acquire.safeBoundary=false;
  const auto currentTarget=actor.target() ? BodyObservations::entityId(*actor.target()) : 0;
  const auto outcome=it->second.boundary.advance(acquire,currentTarget,wallMs());
  if(std::string_view(outcome)=="waiting") return;
  if(std::string_view(outcome)=="acquired") {
    const auto pending=std::move(it->second); state().pending.erase(it);
    grantAcquire(actor,pending.boundary.target,pending.timeout,pending.provider,pending.mode,pending.boundary.id,pending.enginePolicy);
    } else {
    cancelAcquire(it->second,outcome); state().pending.erase(it);
    }
  }

std::string BodyController::handleJson(const rapidjson::Value& req,bool sentObservations) {
  pumpWatchdogs();
  if(!req.IsObject()) return result(nullptr,nullptr,false,"invalid_request");
  const auto type=textField(req,"type");
  if(type=="body_visibility_watch") {
    if(!req.HasMember("enabled") || !req["enabled"].IsBool()) throw std::invalid_argument("visibility enabled must be boolean");
    if(req["enabled"].GetBool() && (!BodyObservations::enabled() || !TestBench::bodyTracing()))
      throw std::invalid_argument("visibility diagnostics require watched actors and an active body trace");
    state().visibilityDiagnostics=req["enabled"].GetBool(); state().visibilityCaptureFailed=false;
    ++state().visibilityGeneration; state().visibilityRecords=state().visibilityDropped=0;
    return visibilityDiagnosticsJson();
    }
  if(type=="body_self_test") return selfTestJson();
  if(type=="body_control_status") return statusJson();
  if(type=="body_events") return drainJson();
  uint64_t entity=0, session=0, world=0;
  if(!integer(req,"entity_id",entity) || !integer(req,"session_epoch",session) || !integer(req,"world_epoch",world))
    return result(nullptr,nullptr,false,"invalid_identity");
  if(session!=BodyObservations::sessionEpoch()) return result(nullptr,nullptr,false,"stale_session");
  if(world!=BodyObservations::worldEpoch()) return result(nullptr,nullptr,false,"stale_world");
  auto actor=BodyObservations::findEntity(entity);
  if(actor==nullptr) return result(nullptr,nullptr,false,"unknown_entity");
  auto l=lease(*actor);
  const auto provider=textField(req,"provider");
  if(provider.empty() || provider.size()>64) return result(l,actor,false,"invalid_provider");
  if(type=="body_acquire") {
    if(l) return result(l,actor,false,"already_owned");
    const auto policyRefusal=BodyEnginePolicy::validateAcquire(req);
    if(std::string_view(policyRefusal)!="allowed") return result(nullptr,actor,false,policyRefusal);
    const bool enginePolicy=BodyEnginePolicy::requested(req);
    if(state().paused || state().infrastructure) return result(nullptr,actor,false,"paused_or_failed");
    if(!req.HasMember("body_control") || !req["body_control"].IsBool() || !req["body_control"].GetBool())
      return result(nullptr,actor,false,"body_control_disabled");
    if(std::find(state().actors.begin(),state().actors.end(),actor)==state().actors.end())
      return result(nullptr,actor,false,"scenario_actor_required");
    uint64_t target=actor->target() ? BodyObservations::entityId(*actor->target()) : 0, timeout=500,awaitMs=0,requestId=0;
    if(!integer(req,"target_entity_id",target,false) || !integer(req,"timeout_ms",timeout,false) || timeout<100 || timeout>5000 ||
       !integer(req,"await_boundary_ms",awaitMs,false) || awaitMs>2000 ||
       !integer(req,"acquire_request_id",requestId,false))
      return result(nullptr,actor,false,"invalid_acquire");
    const auto mode=textField(req,"mode");
    if(mode!="realtime" && mode!="lockstep") return result(nullptr,actor,false,"invalid_mode");
    const auto pending=state().pending.find(actor);
    if(pending!=state().pending.end()) {
      const auto& p=pending->second;
      if(awaitMs>0 && provider==p.provider && mode==p.mode && enginePolicy==p.enginePolicy && target==p.boundary.target && timeout==p.timeout &&
         (requestId==0 || requestId==p.boundary.id)) return pendingResult(p,true);
      return result(nullptr,actor,false,"acquire_pending_busy");
      }
    if(state().leases.size()+state().pending.size()>=MaxActors) return result(nullptr,actor,false,"owner_limit");
    const auto acquire=acquireState(*actor,target);
    const auto refusal=validateAcquire(acquire);
    if(std::string_view(refusal)!="allowed") {
      if(awaitMs==0 || std::string_view(refusal)!="not_safe_boundary" || !acquire.nativeCombat)
        return result(nullptr,actor,false,refusal);
      auto boundary=acquire; boundary.safeBoundary=true;
      const auto otherRefusal=validateAcquire(boundary);
      if(std::string_view(otherRefusal)!="allowed") return result(nullptr,actor,false,otherRefusal);
      if(requestId==0) { requestId=++state().nextAcquireRequest; if(requestId==0) requestId=++state().nextAcquireRequest; }
      PendingAcquire pending; pending.npc=actor; pending.timeout=timeout;
      pending.enginePolicy=enginePolicy;
      pending.provider=provider; pending.mode=mode; pending.boundary.begin(requestId,target,wallMs(),awaitMs);
      return pendingResult(state().pending.emplace(actor,std::move(pending)).first->second);
      }
    if(awaitMs>0 && requestId==0) { requestId=++state().nextAcquireRequest; if(requestId==0) requestId=++state().nextAcquireRequest; }
    grantAcquire(*actor,target,timeout,provider,mode,awaitMs>0 ? requestId : 0,enginePolicy);
    return result(lease(*actor),actor,true,"acquired");
    }
  uint64_t generation=0;
  // A cancellation can race the boundary grant. The request identity is valid for both
  // the waiting request and its resulting lease, never for a later unrelated owner.
  if(type=="body_release" || type=="body_disconnect") {
    uint64_t requestId=0;
    if(integer(req,"control_generation",generation) && generation==0 &&
       integer(req,"acquire_request_id",requestId,false) && requestId>0) {
      const auto pending=state().pending.find(actor);
      if(pending!=state().pending.end()) {
        if(provider!=pending->second.provider) return result(nullptr,actor,false,"owner_mismatch");
        if(requestId!=pending->second.boundary.id) return result(nullptr,actor,false,"stale_acquire_request");
        const auto reason=type=="body_disconnect" ? "disconnect" : "released";
        cancelAcquire(pending->second,reason); state().pending.erase(pending);
        Json response; response.Parse(result(nullptr,actor,true,"acquire_cancelled").c_str());
        u(response,"acquire_request_id",requestId); str(response,"acquire_cancel_reason",reason);
        return encode(response);
        }
      if(l && l->acquireRequestId==requestId && provider==l->provider) {
        requestRelease(*l,type=="body_disconnect" ? "disconnect" : "released");
        const auto r=result(l,actor,true,"release_pending"); pumpWatchdogs(); return r;
        }
      return result(l,actor,false,"stale_acquire_request");
      }
    }
  if(!l) return result(nullptr,actor,false,"not_owned");
  if(!integer(req,"control_generation",generation) || generation!=l->generation)
    return result(l,actor,false,"stale_generation");
  if(provider!=l->provider) return result(l,actor,false,"owner_mismatch");
  if(type=="body_disconnect") { requestRelease(*l,"disconnect"); const auto r=result(l,actor,true,"disconnected"); pumpWatchdogs(); return r; }
  if(type=="body_release") { requestRelease(*l,"released"); const auto r=result(l,actor,true,"release_pending"); pumpWatchdogs(); return r; }
  if(l->release) return result(l,actor,false,"release_pending");
  if(type=="body_heartbeat") { l->heartbeat=Clock::now(); return result(l,actor,true,"heartbeat"); }
  if(type=="body_goal") {
    uint64_t goal=0,target=0;
    if(!integer(req,"goal_version",goal) || !integer(req,"target_entity_id",target)) return result(l,actor,false,"invalid_goal");
    if(goal<=l->goal) return result(l,actor,false,"stale_goal");
    if(!targetValid(*actor,target)) return result(l,actor,false,"target_invalid");
    cancel(*l,"goal_changed"); l->minimumSequence=BodyObservations::latestSequence(*actor); l->goal=goal; l->target=target; l->heartbeat=Clock::now();
    event(*l,"ownership","goal_changed"); return result(l,actor,true,"goal_updated");
    }
  if(type!="body_action") return result(l,actor,false,"unknown_operation");
  return applyAction(*l,req,session,world,generation,sentObservations,true);
  }

void BodyController::consumed(Npc& actor) { if(auto l=lease(actor); l && l->command) l->active.consumed=true; }
void BodyController::takeHit(Npc& actor) { if(auto l=lease(actor)) cancel(*l,"hit_reaction"); }

bool BodyController::tick(Npc& actor,uint64_t dt) {
  pumpWatchdogs();
  pollAcquire(actor,true);
  auto l=lease(actor);
  if(!l) return false;
  if(l->movementStopPending && stopOwnedMovement(actor)) l->movementStopPending=false;
  if(terminal(actor) || actor.isTalk() || actor.externalCtrl || actor.aiQueue.size()>0 || !boundTarget(actor,l->target)) {
    requestRelease(*l,terminal(actor) ? "actor_down" : !boundTarget(actor,l->target) ? "target_changed_or_lost" : "native_takeover");
    }
  if(!l->release && !state().paused && !state().infrastructure) {
    if(targetValid(actor,l->target)) {
      if(l->visibilityRecovery) actor.traceVisibilityDiagnostic("controller_recovered",true);
      l->visibilityRecovery=false; l->visibilityTurning=false; l->visibilityRecoveryStep=0;
      } else {
      // Pose-based perception can turn during an already accepted strafe/block/attack.
      // It gates every new decision, but cannot cancel that segment's physical lock.
      const bool boundary=safeBoundary(actor);
      const bool executing=l->command && l->active.started;
      // Game time has already advanced here; a consumed primitive can cross its real
      // end boundary between the preceding postStep and this tick. Leave the pose
      // intact until postStep classifies that end as completed or segment_replaced.
      const bool movement=executing && (l->active.action==PolicyAction::Approach || l->active.action==PolicyAction::TurnToTarget);
      const bool awaitingCompletion=executing && l->active.consumed && !movement;
      if(boundary && !awaitingCompletion && !l->visibilityRecovery) {
        actor.traceVisibilityDiagnostic("controller_loss_before_idle",true);
        if(executing) cancel(*l,"target_visibility_lost");
        if(l->movementStopPending) {
          if(stopOwnedMovement(actor)) l->movementStopPending=false;
          } else {
          actor.clearGoTo(); actor.setAnim(Npc::Anim::Idle);
          }
        l->visibilityRecovery=true;
        l->visibilityRecoveryStep=BodyObservations::currentStep()+1;
        l->visibilityRecoveryStart=actor.world().tickCount();
        actor.traceVisibilityDiagnostic("controller_loss_after_idle_request",true);
        }
      if(l->visibilityRecovery && BodyObservations::currentStep()<=l->visibilityRecoveryStep) {
        actor.mvAlgo.tick(dt,MoveAlgo::WaitMove); return true;
        }
      // The native fight loop keeps turning toward its held target between primitives and
      // never consults the view cone. Give the owned body that same FAI turn for a bounded
      // time before handing it back; no primitive is chosen and no RNG is drawn here, and
      // every decision stays masked until the target is visible again.
      if(l->visibilityRecovery && !awaitingCompletion && boundary && boundTarget(actor,l->target) &&
         actor.world().tickCount()-l->visibilityRecoveryStart<VisibilityRecoveryMs) {
        if(!l->visibilityTurning) actor.traceVisibilityDiagnostic("controller_recovery_turn",true);
        l->visibilityTurning=true;
        actor.implTurnToFai(*actor.target(),dt);
        actor.mvAlgo.tick(dt,MoveAlgo::FaiMove);
        return true;
        }
      // A native hit can cancel the policy command while installing an uninterruptible
      // stumble. Loss of that command is not a physical decision boundary: retain the
      // same locked animation, with decisions still disabled, until its real boundary.
      if(!awaitingCompletion && boundary) {
        actor.traceVisibilityDiagnostic("controller_release_visibility",true);
        requestRelease(*l,"target_visibility_lost");
        }
      }
    }
  if(l->release || state().paused || state().infrastructure) {
    if(actor.bodyStateMasked()==BS_HIT && actor.target()!=nullptr)
      actor.implAttack(dt,true,FightAlgo::MV_NULL);
    else actor.mvAlgo.tick(dt,MoveAlgo::WaitMove);
    return true;
    }
  if(!l->command) {
    if(safeBoundary(actor) && (actor.bodyStateMasked()==BS_HIT || actor.bodyStateMasked()==BS_PARADE))
      actor.setAnim(Npc::Anim::Idle);
    actor.mvAlgo.tick(dt,MoveAlgo::WaitMove); return true;
    }
  auto& c=l->active;
  if(!c.started) {
    const auto now=actor.world().tickCount();
    if(c.goal!=l->goal || c.target!=l->target || !targetValid(actor,c.target)) { cancel(*l,"invalidated_before_start"); return true; }
    if(now<c.issued || now-c.issued>=c.ttl || (l->mode=="realtime" && Clock::now()>=c.deadline)) { cancel(*l,"expired_before_start"); return true; }
    const auto resolution=FightAlgo::resolve(c.action,resolverState(actor));
    if(!resolution.allowed) {
      if(std::string_view(resolution.reason)=="locked") {
        if(actor.bodyStateMasked()==BS_HIT && actor.target()!=nullptr) actor.implAttack(dt,true,FightAlgo::MV_NULL);
        else actor.mvAlgo.tick(dt,MoveAlgo::WaitMove);
        return true;
        }
      cancel(*l,"mask_changed_before_start"); return true;
      }
    c.primitive=resolution.primitive;
    }
  // MOVE may consume its intent merely because Idle changed to Run. Ownership must
  // keep driving the ordinary approach executor until its physical range condition,
  // rather than leave that run animation moving after an early command completion.
  const bool movement=c.action==PolicyAction::Approach || c.action==PolicyAction::TurnToTarget;
  if(c.consumed && !movement) {
    const auto now=actor.world().tickCount();
    const bool hit=actor.bodyStateMasked()==BS_HIT;
    const bool nativeWait=actor.faiWaitTime>=now || actor.waitTime>=now;
    const uint8_t handling=hit ? 1 : nativeWait ? 2 : 3;
    const bool edge=c.housekeepingCase!=handling;
    const auto before=hit ? "owned_consumed_hit_lock_before" : nativeWait ? "owned_consumed_fai_or_script_wait_before" : "owned_consumed_wait_move_before";
    const auto after=hit ? "owned_consumed_hit_lock_after" : nativeWait ? "owned_consumed_fai_or_script_wait_after" : "owned_consumed_wait_move_after";
    if(edge) actor.traceVisibilityDiagnostic(before,true);
    // The ordinary native FAI/script wait gate turns toward the held target before
    // advancing root motion. Skipping that housekeeping froze owned strafe body yaw
    // while BIP01 rotated ~73 degrees, causing a genuine pose-cone loss on clear rays.
    if(hit || nativeWait) actor.implAttack(dt,true,FightAlgo::MV_NULL);
    else actor.mvAlgo.tick(dt,MoveAlgo::WaitMove);
    if(edge) actor.traceVisibilityDiagnostic(after,true);
    c.housekeepingCase=handling;
    return true;
    }
  const auto before=actor.combatPoseIdentity();
  const auto previousLayers=actor.visual.pose().combatLayers(actor.world().tickCount());
  const auto position=actor.position(); const float yaw=actor.rotation(); const auto wait=actor.faiWaitTime;
  if(!actor.implAttack(dt,true,c.primitive) && !actor.go2.empty())
    actor.implGoTo(dt);
  const auto after=actor.combatPoseIdentity();
  if(!c.started && (before!=after || actor.position()!=position || actor.rotation()!=yaw || actor.faiWaitTime>wait)) {
    c.started=true; c.beforeIdentity=before; c.identity=after; c.comboLength=actor.visual.pose().comboLength();
    for(const auto& layer:actor.visual.pose().combatLayers(actor.world().tickCount())) {
      const auto unchanged=std::find_if(previousLayers.begin(),previousLayers.end(),[&](const Pose::CombatLayer& old) {
        return old.sequence==layer.sequence && old.origin==layer.origin && old.combination==layer.combination && old.state==layer.state;
        });
      if(unchanged==previousLayers.end()) c.layers.push_back(layer);
      }
    event(*l,"started",before!=after ? "segment_started" : actor.faiWaitTime>wait ? "wait_started" : "movement_started");
    }
  if(!c.started && c.consumed) cancel(*l,"consumed_without_start");
  return true;
  }

void BodyController::postStep() {
  for(auto& pair:state().leases) {
    auto& l=pair.second;
    if(l.movementStopPending && stopOwnedMovement(*l.npc)) l.movementStopPending=false;
    if(!l.command || !l.active.started) continue;
    auto& c=l.active;
    if(c.action==PolicyAction::Approach || c.action==PolicyAction::TurnToTarget) {
      auto& actor=*l.npc;
      if(!safeBoundary(actor) || !boundTarget(actor,l.target)) continue;
      const auto& target=*actor.target();
      const bool reached=c.action==PolicyAction::Approach ?
                         actor.fghAlgo.isInWRange(actor,target,actor.world().script()) :
                         actor.fghAlgo.isInFocusAngle(actor,target,5.f);
      if(reached && stopOwnedMovement(actor)) {
        // Stop through normal locomotion APIs at this legal owned boundary. A running
        // pose must not survive into an idle hold or select ATTACKMOVE for the next hit.
        event(l,"completed","movement_condition"); l.command=false;
        }
      continue;
      }
    if(!c.consumed) continue;
    const auto now=l.npc->world().tickCount();
    if(c.primitive==FightAlgo::MV_WAIT || c.primitive==FightAlgo::MV_WAITLONG) {
      if(l.npc->faiWaitTime<now) { event(l,"completed","wait_elapsed"); l.command=false; }
      continue;
      }
    bool unfinished=false, replacedEarly=false;
    const auto current=l.npc->visual.pose().combatLayers(now);
    for(const auto& original:c.layers) {
      if(!original.sequence) continue;
      const auto found=std::find_if(current.begin(),current.end(),[&](const Pose::CombatLayer& layer) {
        return layer.sequence==original.sequence && layer.origin==original.origin && layer.combination==original.combination;
        });
      if(found!=current.end()) {
        if(!found->sequence->isFinished(found->time,found->start,c.comboLength)) unfinished=true;
        } else {
        const auto predicted=now<=original.start ? now : original.start+uint64_t(double(now-original.start)*double(original.speed));
        if(!original.sequence->isFinished(predicted,original.start,c.comboLength)) replacedEarly=true;
        }
      }
    if(replacedEarly) { cancel(l,"segment_replaced"); continue; }
    if(!unfinished) {
      // The native strafe queue's MV_STRAFE_E is an internal completion, not a policy choice.
      if(c.primitive==FightAlgo::MV_STRAFEL || c.primitive==FightAlgo::MV_STRAFER)
        l.npc->setAnim(Npc::Anim::Idle);
      l.npc->traceVisibilityDiagnostic("controller_segment_completed",true);
      event(l,"completed","segment_finished"); l.command=false;
      if(!targetValid(*l.npc,l.target)) {
        l.visibilityRecovery=true;
        l.visibilityRecoveryStep=BodyObservations::currentStep()+1;
        l.visibilityRecoveryStart=now;
        }
      }
    }
  pumpWatchdogs();
  }

void BodyController::fixturePrepare(Npc& actor,Npc& target,float distanceCm,float yawOffsetDeg) {
  if(owns(actor) || !std::isfinite(distanceCm) || distanceCm<80 || distanceCm>2000 || !std::isfinite(yawOffsetDeg))
    throw std::invalid_argument("invalid or owned body fixture");
  actor.fghAlgo.onClearTarget(); actor.aiQueue.clear(); actor.aiQueueOverlay.clear();
  actor.waitTime=0; actor.aniWaitTime=0; actor.faiWaitTime=0; actor.outWaitTime=0;
  actor.clearGoTo(); actor.clearSpeed(); actor.setAnimRotate(0);
  target.setExternalControl(true);
  target.aiQueue.clear(); target.aiQueueOverlay.clear(); target.fghAlgo.onClearTarget();
  target.waitTime=0; target.aniWaitTime=0; target.faiWaitTime=0; target.outWaitTime=0;
  target.clearGoTo(); target.clearSpeed(); target.setAnimRotate(0);
  const_cast<Pose&>(target.visual.pose()).stopAllAnim(); target.setAnim(Npc::Anim::Idle);
  const_cast<Pose&>(actor.visual.pose()).stopAllAnim();
  if(auto weapon=actor.invent.currentMeleeWeapon()) {
    const auto ws=weapon->is2H() ? WeaponState::W2H : WeaponState::W1H;
    actor.visual.setToFightMode(ws);
    actor.invent.switchActiveWeapon(actor,1);
    actor.hnpc->weapon=ws==WeaponState::W1H ? 3 : 4;
    actor.updateWeaponSkeleton();
    } else throw std::invalid_argument("body fixture requires equipped melee weapon");
  actor.setAnim(Npc::Anim::Idle);
  actor.setTarget(&target);
  const auto at=actor.position();
  target.setPosition(at.x+distanceCm,at.y,at.z);
  // Npc::setDirection(Vec3) takes a direction vector, not a world position.
  actor.setDirection(target.position()-actor.position());
  actor.setDirection(actor.rotation()+yawOffsetDeg);
  // Visibility reads transformed collision centers/facing, so fixture queries must see
  // the same geometry that the next ordinary post-animation sample will publish.
  actor.updateTransform(); target.updateTransform();
  BodyObservations::discontinuity(actor); BodyObservations::discontinuity(target);
  }

const char* BodyController::validate(const GateState& s,const GateCommand& c) {
  if(c.session!=s.session) return "stale_session";
  if(c.world!=s.world) return "stale_world";
  if(c.generation!=s.generation) return "stale_generation";
  if(c.goal!=s.goal) return "stale_goal";
  if(c.target!=s.target || !s.targetValid) return "target_invalid";
  if(c.observation!=s.observation || c.issuedGame!=s.observedGame) return "stale_observation";
  if(c.ttl==0 || c.ttl>1000 || c.issuedGame>s.gameNow || s.gameNow-c.issuedGame>=c.ttl) return "expired";
  if(s.realtime && s.wallAge>=c.ttl) return "expired_wall";
  if(s.paused || s.infrastructure) return "paused_or_failed";
  if(s.busy && c.action!=PolicyAction::Continue) return "command_busy";
  return FightAlgo::resolve(c.action,s.resolver).allowed ? "allowed" : FightAlgo::resolve(c.action,s.resolver).reason;
  }

void BodyController::fixtureLock(Npc& actor,uint64_t durationMs) {
  if(durationMs==0 || durationMs>5000) throw std::invalid_argument("body fixture lock outside [1,5000]");
  // This is the same real game-time lock used by native combat waits, never a lifecycle event.
  actor.implFaiWait(durationMs);
  }

bool BodyController::nativeCombatGoto(uint8_t hint,const Npc* target,const Npc* provenance,const Npc* gotoNpc,bool gotoWaypoint) {
  if(target==nullptr || provenance!=target)
    return false;
  if(hint==Npc::GT_Enemy)
    return gotoNpc==target && !gotoWaypoint;
  if(hint==Npc::GT_Way)
    return (gotoNpc==target && !gotoWaypoint) || (gotoNpc==nullptr && gotoWaypoint);
  return false;
  }

const char* BodyController::validateAcquire(const AcquireState& s) {
  if(!s.targetValid) return "target_invalid";
  if(s.externalControl) return "external_control";
  if(s.player) return "player_actor";
  if(s.talking) return "talking";
  if(s.terminal) return "actor_down";
  if(!s.safeBoundary) return "not_safe_boundary";
  if(!s.overlayEmpty) return "native_overlay_busy";
  if(!s.queueEmpty && (!s.nativeCombat || !s.queueCombatOnly)) return "native_queue_busy";
  if(!s.gotoEmpty && (!s.nativeCombat || !s.gotoToTarget)) return "goto_busy";
  return "allowed";
  }

bool BodyController::BoundaryRequest::begin(uint64_t request,uint64_t enemy,uint64_t nowMs,uint64_t awaitMs) {
  if(pending || request==0 || enemy==0 || awaitMs==0 || awaitMs>2000) return false;
  id=request; target=enemy; deadlineMs=nowMs+awaitMs; pending=true; return true;
  }

const char* BodyController::BoundaryRequest::advance(const AcquireState& state,uint64_t currentTarget,uint64_t nowMs) {
  if(!pending) return "not_pending";
  const auto finish=[&](const char* reason) { pending=false; return reason; };
  if(nowMs>=deadlineMs) return finish("expired");
  if(state.terminal) return finish("actor_down");
  if(state.talking) return finish("talking");
  if(currentTarget!=target) return finish("target_changed");
  if(!state.nativeCombat) return finish("native_combat_ended");
  auto unlocked=state; unlocked.safeBoundary=true;
  const auto reason=BodyController::validateAcquire(unlocked);
  if(std::string_view(reason)!="allowed") return finish(reason);
  if(!state.safeBoundary) return "waiting";
  return finish("acquired");
  }

bool BodyController::BoundaryRequest::cancel() {
  const bool wasPending=pending; pending=false; return wasPending;
  }

std::string BodyController::selfTestJson() {
  Json out=object(); str(out,"type","body_self_test");
  GateState s; s.session=11; s.world=12; s.generation=13; s.goal=14; s.target=15; s.observation=16;
  s.observedGame=100; s.gameNow=110; s.wallAge=10; s.targetValid=true; s.realtime=true;
  s.resolver.eligible=true; s.resolver.targetValid=true; s.resolver.sword=true;
  s.resolver.hasMove=true; s.resolver.hasLeft=true; s.resolver.hasRight=true; s.resolver.hasBack=true;
  s.resolver.hasAttack=true; s.resolver.hasAttackLeft=true; s.resolver.hasAttackRight=true; s.resolver.hasBlock=true;
  s.resolver.canRotate=true; s.resolver.focus5=true; s.resolver.focus30=true; s.resolver.inWRange=true; s.resolver.lineClear=true;
  GateCommand c{11,12,13,14,15,16,100,200,PolicyAction::AttackFront};
  rapidjson::Value checks(rapidjson::kArrayType); bool passed=true;
  auto check=[&](const char* name,std::string_view actual,std::string_view expected) {
    Json row=object(); str(row,"name",name); str(row,"actual",actual); str(row,"expected",expected);
    const bool success=actual==expected; b(row,"passed",success); passed=passed && success;
    checks.PushBack(rapidjson::Value(row,out.GetAllocator()),out.GetAllocator());
    };
  auto reason=[&](const char* name,const char* expected) { check(name,validate(s,c),expected); };
  reason("valid_command","allowed");
  ++c.session; reason("stale_session","stale_session"); --c.session;
  ++c.world; reason("stale_world","stale_world"); --c.world;
  ++c.generation; reason("stale_generation","stale_generation"); --c.generation;
  ++c.goal; reason("stale_goal","stale_goal"); --c.goal;
  ++c.target; reason("unknown_target","target_invalid"); --c.target;
  ++c.observation; reason("stale_observation","stale_observation"); --c.observation;
  s.gameNow=300; reason("game_expiry","expired"); s.gameNow=110;
  s.wallAge=200; reason("realtime_wall_expiry","expired_wall");
  s.realtime=false; reason("lockstep_wall_not_game_expiry","allowed"); s.realtime=true; s.wallAge=10;
  s.busy=true; reason("bounded_single_slot","command_busy"); c.action=PolicyAction::Continue;
  reason("continue_locked_execution","allowed"); s.busy=false; c.action=PolicyAction::AttackFront;
  s.resolver.lineClear=false; reason("collision_mask","masked"); s.resolver.lineClear=true;
  s.resolver.eligible=false; reason("lock_mask","locked"); s.resolver.eligible=true;
  s.resolver.inCloseup=true;
  check("native_wait_promotion",std::to_string(FightAlgo::resolveNative(FightAlgo::MV_WAIT,s.resolver)),"3");
  check("policy_wait_same_promotion",std::to_string(FightAlgo::resolve(PolicyAction::Wait200,s.resolver).primitive),"3");
  s.resolver.focus5=false;
  check("native_attack_focus_normalization",std::to_string(FightAlgo::resolveNative(FightAlgo::MV_ATTACK,s.resolver)),"12");
  const auto before=FightAlgo::actionMask(s.resolver);
  auto after=before;
  for(int i=0;i<100;++i) after=FightAlgo::actionMask(s.resolver);
  std::string beforeMask,afterMask;
  for(bool value:before) beforeMask+=value ? '1' : '0';
  for(bool value:after) afterMask+=value ? '1' : '0';
  check("mask_repeat_purity",afterMask,beforeMask);
  out.AddMember("checks",checks,out.GetAllocator()); b(out,"passed",passed); return encode(out);
  }
