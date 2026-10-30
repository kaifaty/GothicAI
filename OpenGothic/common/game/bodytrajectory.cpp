#include "bodytrajectory.h"

#include "bodyobservations.h"
#include "bodycontroller.h"
#include "fightalgo.h"
#include "graphics/mesh/pose.h"
#include "world/objects/npc.h"
#include "world/world.h"

#include <algorithm>
#include <array>
#include <deque>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>
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

namespace {
using Json=rapidjson::Document;
using Tempest::Vec3;
struct Active final {
  Npc* npc=nullptr;
  uint64_t entity=0,target=0,decision=0,execution=0,observation=0,entry=0,parent=0;
  int intent=0,effective=0,action=-1;
  bool valid=false,started=false,consumed=false;
  std::string beforePose,currentPose,startedPose;
  Vec3 beforePosition;
  float beforeYaw=0;
  uint64_t beforeWait=0;
  uint16_t comboLength=0;
  std::vector<Pose::CombatLayer> beforeLayers,layers;
  };
struct State final {
  bool enabled=false;
  size_t capacity=4096;
  uint64_t generation=0,sequence=0,decisionSequence=0,executionSequence=0;
  uint64_t dropped=0,lastDrainDropped=0,resetDiscarded=0,emitted=0,drained=0,maxQueue=0;
  bool failed=false;
  uint64_t failures=0;
  std::deque<std::string> queue;
  std::unordered_map<const Npc*,Active> active;
  std::unordered_map<const Npc*,std::pair<uint64_t,int>> lastOpportunity;
  };
State& state() { static State* value=new State; return *value; }
template<class Fn> void guarded(Fn&& fn) noexcept {
  try { fn(); }
  catch(...) {
    // This state already exists whenever an enabled callback can throw. Telemetry failure
    // disables capture and is observable to the collector; it never unwinds the native executor.
    auto& s=state(); s.failed=true; ++s.failures; ++s.dropped; s.enabled=false;
    }
  }
Json object() { Json row; row.SetObject(); return row; }
void u(Json& row,const char* key,uint64_t value) { auto& a=row.GetAllocator(); row.AddMember(rapidjson::Value(key,a),rapidjson::Value(value),a); }
void i(Json& row,const char* key,int value) { auto& a=row.GetAllocator(); row.AddMember(rapidjson::Value(key,a),rapidjson::Value(value),a); }
void b(Json& row,const char* key,bool value) { auto& a=row.GetAllocator(); row.AddMember(rapidjson::Value(key,a),rapidjson::Value(value),a); }
void str(Json& row,const char* key,std::string_view value) { auto& a=row.GetAllocator(); row.AddMember(rapidjson::Value(key,a),rapidjson::Value(value.data(),rapidjson::SizeType(value.size()),a),a); }
std::string encode(const Json& row) {
  rapidjson::StringBuffer buffer; rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  row.Accept(writer); return {buffer.GetString(),buffer.GetSize()};
  }
uint64_t number(const Json& row,const char* key) {
  if(!row.HasMember(key) || !row[key].IsUint64()) throw std::runtime_error(std::string("trajectory missing native integer ")+key);
  return row[key].GetUint64();
  }
Json parse(std::string_view encoded) {
  Json row; row.Parse(encoded.data(),encoded.size());
  if(row.HasParseError() || !row.IsObject()) throw std::runtime_error("invalid native body trajectory row");
  return row;
  }
uint64_t emit(Json& row) {
  auto& s=state(); u(row,"schema_version",1); u(row,"capture_generation",s.generation);
  const auto sequence=++s.sequence; u(row,"record_seq",sequence);
  if(s.queue.size()==s.capacity) { s.queue.pop_front(); ++s.dropped; }
  s.queue.push_back(encode(row)); ++s.emitted; s.maxQueue=std::max(s.maxQueue,uint64_t(s.queue.size()));
  return sequence;
  }
void identity(Json& row,Npc& npc) {
  u(row,"session_epoch",BodyObservations::sessionEpoch()); u(row,"world_epoch",BodyObservations::worldEpoch());
  u(row,"entity_id",BodyObservations::entityId(npc)); u(row,"control_generation",BodyController::snapshot(npc).controlGeneration);
  u(row,"t_game_ms",npc.world().tickCount()); u(row,"sim_step",BodyObservations::currentStep());
  }
uint64_t observation(std::string_view encoded,std::string_view source) {
  auto native=parse(encoded); auto row=object(); str(row,"kind","observation"); str(row,"source",source);
  for(const char* key:{"session_epoch","world_epoch","entity_id","control_generation","t_game_ms","sim_step"}) u(row,key,number(native,key));
  row.AddMember("observation",rapidjson::Value(native,row.GetAllocator()),row.GetAllocator());
  return emit(row);
  }
void execution(Active& active,std::string_view phase,std::string_view reason,bool incomplete=false,bool physicalCancelled=false) {
  auto row=object(); identity(row,*active.npc); str(row,"kind","execution"); str(row,"source","native");
  u(row,"decision_id",active.decision); u(row,"execution_id",active.execution); u(row,"observation_record_seq",active.observation);
  u(row,"entry_serial",active.entry); u(row,"parent_choice_id",active.parent);
  str(row,"phase",phase); str(row,"reason",reason); i(row,"native_intent",active.intent); i(row,"effective_primitive",active.effective);
  b(row,"decision_valid",active.valid); b(row,"segment_started",active.started); b(row,"primitive_consumed",active.consumed);
  b(row,"execution_incomplete",incomplete); b(row,"physical_cancelled",physicalCancelled);
  str(row,"pose_before",active.beforePose); str(row,"pose_after",active.currentPose);
  const auto at=active.npc->position();
  rapidjson::Value before(rapidjson::kArrayType),after(rapidjson::kArrayType);
  for(float value:{active.beforePosition.x,active.beforePosition.y,active.beforePosition.z}) before.PushBack(rapidjson::Value(double(value)),row.GetAllocator());
  for(float value:{at.x,at.y,at.z}) after.PushBack(rapidjson::Value(double(value)),row.GetAllocator());
  row.AddMember("position_before_cm",before,row.GetAllocator()); row.AddMember("position_after_cm",after,row.GetAllocator());
  row.AddMember("yaw_before_deg",rapidjson::Value(double(active.beforeYaw)),row.GetAllocator());
  row.AddMember("yaw_after_deg",rapidjson::Value(double(active.npc->rotation())),row.GetAllocator());
  emit(row);
  }
int label(int primitive) {
  switch(primitive) {
    case FightAlgo::MV_MOVE: return 1;
    case FightAlgo::MV_TURN: case FightAlgo::MV_TURN2HIT: return 2;
    case FightAlgo::MV_STRAFEL: return 3;
    case FightAlgo::MV_STRAFER: return 4;
    case FightAlgo::MV_JUMPBACK: return 5;
    case FightAlgo::MV_ATTACK: return 6;
    case FightAlgo::MV_ATTACKL: return 7;
    case FightAlgo::MV_ATTACKR: return 8;
    case FightAlgo::MV_BLOCK: return 9;
    case FightAlgo::MV_WAIT: return 10;
    case FightAlgo::MV_WAITLONG: return 11;
    default: return -1;
    }
  }
enum class Completion { Pending, Finished, Replaced };
Completion classify(const Active& active,const std::vector<Pose::CombatLayer>& layers,uint64_t now,
                    bool waitGate,bool focus5) {
  if(!active.started) return Completion::Pending;
  if(active.action==1) return active.consumed ? Completion::Finished : Completion::Pending;
  if(active.action==2) return active.consumed || focus5 ? Completion::Finished : Completion::Pending;
  if(active.effective==FightAlgo::MV_WAIT || active.effective==FightAlgo::MV_WAITLONG)
    return !waitGate ? Completion::Finished : Completion::Pending;
  if(!active.consumed) return Completion::Pending;
  bool unfinished=false,replacedEarly=false;
  for(const auto& original:active.layers) {
    if(!original.sequence) continue;
    const auto current=std::find_if(layers.begin(),layers.end(),[&](const Pose::CombatLayer& layer) {
      return layer.sequence==original.sequence && layer.origin==original.origin && layer.combination==original.combination;
      });
    if(current!=layers.end()) {
      if(!current->sequence->isFinished(current->time,current->start,active.comboLength)) unfinished=true;
      } else {
      const auto predicted=now<=original.start ? now : original.start+uint64_t(double(now-original.start)*double(original.speed));
      if(!original.sequence->isFinished(predicted,original.start,active.comboLength)) replacedEarly=true;
      }
    }
  return replacedEarly ? Completion::Replaced : unfinished ? Completion::Pending : Completion::Finished;
  }
const char* completedReason(const Active& active) {
  if(active.action==2) return "turn_condition_reached";
  if(active.action==1) return "movement_condition";
  if(active.effective==FightAlgo::MV_WAIT || active.effective==FightAlgo::MV_WAITLONG) return "wait_elapsed";
  return "segment_finished";
  }
}

bool BodyTrajectory::enabled() { return state().enabled; }
void BodyTrajectory::configure(bool enabled,size_t capacity) {
  auto& s=state();
  if(capacity<64 || capacity>16384) throw std::invalid_argument("trajectory capacity must be 64..16384");
  if(enabled) {
    if(s.enabled) throw std::logic_error("trajectory recorder is already enabled");
    if(!s.queue.empty()) throw std::logic_error("drain old trajectory records before enabling");
    s.capacity=capacity; s.enabled=true; s.failed=false; ++s.generation; s.lastOpportunity.clear();
    } else {
    for(auto& pair:s.active) {
      pair.second.currentPose=pair.second.npc->combatPoseIdentity();
      execution(pair.second,"cancelled","recorder_disabled",true,false);
      }
    s.active.clear(); s.lastOpportunity.clear(); s.enabled=false;
    }
  }
void BodyTrajectory::reset(std::string_view reason) {
  auto& s=state();
  if(s.enabled) for(auto& pair:s.active) {
    pair.second.currentPose=pair.second.npc->combatPoseIdentity();
    execution(pair.second,"cancelled",reason,true,false);
    }
  s.resetDiscarded+=s.queue.size(); s.dropped+=s.queue.size(); s.queue.clear(); s.active.clear(); s.lastOpportunity.clear();
  s.enabled=false; ++s.generation;
  }
void BodyTrajectory::observe(std::string_view row) noexcept { if(enabled()) guarded([&] { observation(row,"periodic"); }); }
void BodyTrajectory::noteControl(std::string_view encoded) noexcept {
  if(!enabled()) return;
  guarded([&] {
  auto native=parse(encoded); auto row=object(); str(row,"kind","control"); str(row,"source","scripted");
  for(const char* key:{"session_epoch","world_epoch","entity_id","control_generation","t_game_ms","sim_step"}) u(row,key,number(native,key));
  row.AddMember("body_control_event",rapidjson::Value(native,row.GetAllocator()),row.GetAllocator()); emit(row);
  });
  }
BodyTrajectory::Attempt::Attempt(Npc& npc,bool native,int intent,int effective,uint64_t parentChoice,int parentInstruction,uint64_t entrySerial) noexcept {
  if(native && BodyTrajectory::enabled() && BodyObservations::watched(npc)) {
    actor=&npc; BodyTrajectory::beginNative(npc,intent,effective,parentChoice,parentInstruction,entrySerial);
    }
  }
BodyTrajectory::Attempt::~Attempt() noexcept { if(actor!=nullptr) BodyTrajectory::nativeAfterAttempt(*actor); }

void BodyTrajectory::beginNative(Npc& npc,int intent,int effective,uint64_t parentChoice,int parentInstruction,uint64_t entrySerial) noexcept {
  if(enabled()) guarded([&] { beginNativeImpl(npc,intent,effective,parentChoice,parentInstruction,entrySerial); });
  }
void BodyTrajectory::beginNativeImpl(Npc& npc,int intent,int effective,uint64_t parentChoice,int parentInstruction,uint64_t entrySerial) {
  auto& s=state(); if(!s.enabled || !BodyObservations::watched(npc)) return;
  auto existing=s.active.find(&npc);
  if(existing!=s.active.end() && existing->second.entry==entrySerial && existing->second.effective==effective) {
    auto& active=existing->second;
    active.beforePose=npc.combatPoseIdentity(); active.currentPose=active.beforePose; active.beforePosition=npc.position(); active.beforeYaw=npc.rotation();
    active.beforeWait=npc.faiWaitTime; active.beforeLayers=npc.visual.pose().combatLayers(npc.world().tickCount());
    return;
    }
  if(existing!=s.active.end()) {
    existing->second.currentPose=npc.combatPoseIdentity();
    const auto focus=npc.target()!=nullptr && npc.fghAlgo.isInFocusAngle(npc,*npc.target(),5.f);
    const auto completion=classify(existing->second,npc.visual.pose().combatLayers(npc.world().tickCount()),
                                   npc.world().tickCount(),npc.bodyObservationState().faiGate,focus);
    if(completion==Completion::Finished)
      execution(existing->second,"completed",completedReason(existing->second));
    else if(!existing->second.started && existing->second.consumed)
      execution(existing->second,"cancelled","consumed_without_start",false,false);
    else
      execution(existing->second,"cancelled",completion==Completion::Replaced ? "segment_replaced" : "native_opportunity_replaced",!existing->second.started,true);
    s.active.erase(existing);
    }
  const auto key=std::make_pair(entrySerial,effective);
  if(s.lastOpportunity.contains(&npc) && s.lastOpportunity.at(&npc)==key) return;
  s.lastOpportunity[&npc]=key;
  const auto encoded=BodyObservations::captureDecision(npc);
  if(encoded.empty()) return;
  const auto observationRecord=observation(encoded,"native_capture");
  const auto resolver=BodyController::resolverState(npc);
  const auto mask=FightAlgo::actionMask(resolver); const int action=label(effective);
  Active active; active.npc=&npc; active.entity=BodyObservations::entityId(npc);
  active.target=npc.target()==nullptr ? 0 : BodyObservations::entityId(*npc.target());
  active.decision=++s.decisionSequence; active.execution=++s.executionSequence;
  active.observation=observationRecord; active.entry=entrySerial; active.parent=parentChoice;
  active.intent=intent; active.effective=effective; active.action=action;
  active.valid=action>0 && resolver.eligible && resolver.targetValid && mask[size_t(action)];
  active.beforePose=npc.combatPoseIdentity(); active.beforePosition=npc.position(); active.beforeYaw=npc.rotation(); active.beforeWait=npc.faiWaitTime;
  active.currentPose=active.beforePose;
  active.beforeLayers=npc.visual.pose().combatLayers(npc.world().tickCount());
  auto row=object(); identity(row,npc); str(row,"kind","decision"); str(row,"source","native");
  u(row,"decision_id",active.decision); u(row,"execution_id",active.execution); u(row,"observation_record_seq",observationRecord);
  u(row,"entry_serial",entrySerial); u(row,"parent_choice_id",parentChoice); i(row,"parent_fai_instruction",parentInstruction);
  i(row,"native_intent",intent); i(row,"effective_primitive",effective);
  if(action<0) row.AddMember("action_id",rapidjson::Value(rapidjson::kNullType),row.GetAllocator()); else i(row,"action_id",action);
  b(row,"accepted",true); str(row,"acceptance_kind","native_executor_dispatch");
  b(row,"decision_valid",active.valid); str(row,"reason",active.valid ? "eligible_native_opportunity" : action<0 ? "unsupported_or_internal_primitive" : !resolver.eligible ? "native_locked" : !resolver.targetValid ? "target_not_visible" : "normalized_action_masked");
  rapidjson::Value actionMask(rapidjson::kArrayType);
  for(bool allowed:mask) actionMask.PushBack(rapidjson::Value(allowed),row.GetAllocator());
  row.AddMember("action_mask",actionMask,row.GetAllocator()); emit(row);
  s.active.emplace(&npc,std::move(active)); execution(s.active.at(&npc),"accepted","native_opportunity_observed");
  }
void BodyTrajectory::nativeAfterAttempt(Npc& npc) noexcept { if(enabled()) guarded([&] { nativeAfterAttemptImpl(npc); }); }
void BodyTrajectory::nativeAfterAttemptImpl(Npc& npc) {
  auto& s=state(); if(!s.enabled) return;
  const auto found=s.active.find(&npc); if(found==s.active.end()) return;
  auto& active=found->second; active.currentPose=npc.combatPoseIdentity(); if(active.started) return;
  const auto after=npc.combatPoseIdentity();
  if(after==active.beforePose && npc.position()==active.beforePosition && npc.rotation()==active.beforeYaw && npc.faiWaitTime<=active.beforeWait) return;
  active.started=true; active.startedPose=after; active.comboLength=npc.visual.pose().comboLength();
  for(const auto& layer:npc.visual.pose().combatLayers(npc.world().tickCount())) {
    const auto old=std::find_if(active.beforeLayers.begin(),active.beforeLayers.end(),[&](const Pose::CombatLayer& prior) {
      return prior.sequence==layer.sequence && prior.origin==layer.origin && prior.combination==layer.combination && prior.state==layer.state;
      });
    if(old==active.beforeLayers.end()) active.layers.push_back(layer);
    }
  execution(active,"started",after!=active.beforePose ? "segment_started" : npc.faiWaitTime>active.beforeWait ? "wait_started" : "movement_started");
  }
void BodyTrajectory::nativeConsumed(Npc& npc) noexcept { if(enabled()) guarded([&] { nativeConsumedImpl(npc); }); }
void BodyTrajectory::nativeConsumedImpl(Npc& npc) {
  auto& s=state(); if(!s.enabled) return;
  nativeAfterAttempt(npc);
  const auto found=s.active.find(&npc); if(found==s.active.end() || found->second.consumed) return;
  found->second.consumed=true; execution(found->second,"consumed","native_queue_consumed");
  }
void BodyTrajectory::postStep() noexcept { if(enabled()) guarded([] { postStepImpl(); }); }
void BodyTrajectory::postStepImpl() {
  auto& s=state(); if(!s.enabled) return;
  for(auto it=s.active.begin();it!=s.active.end();) {
    auto& active=it->second; auto& npc=*active.npc; const auto now=npc.world().tickCount();
    active.currentPose=npc.combatPoseIdentity();
    const auto target=npc.target();
    if(npc.isDown() || npc.attribute(ATR_HITPOINTS)<=0 || target==nullptr || target->isDown() || target->attribute(ATR_HITPOINTS)<=0 ||
       BodyObservations::entityId(*target)!=active.target || BodyController::owns(npc)) {
      execution(active,"cancelled","actor_or_target_or_owner_changed",!active.started,true); it=s.active.erase(it); continue;
      }
    if(!active.started) {
      if(active.consumed) { execution(active,"cancelled","consumed_without_start",false,false); it=s.active.erase(it); continue; }
      ++it; continue;
      }
    const auto completion=classify(active,npc.visual.pose().combatLayers(now),now,npc.bodyObservationState().faiGate,
                                   npc.fghAlgo.isInFocusAngle(npc,*target,5.f));
    if(completion!=Completion::Pending) {
      const bool replaced=completion==Completion::Replaced;
      execution(active,replaced ? "cancelled" : "completed",replaced ? "segment_replaced" : completedReason(active),false,replaced);
      it=s.active.erase(it); continue;
      }
    ++it;
    }
  }
void BodyTrajectory::forget(const Npc* npc) noexcept { guarded([&] { forgetImpl(npc); }); }
void BodyTrajectory::forgetImpl(const Npc* npc) {
  auto& s=state(); const auto found=s.active.find(npc);
  if(found!=s.active.end()) {
    found->second.currentPose=found->second.npc->combatPoseIdentity();
    if(s.enabled) execution(found->second,"cancelled","actor_destroyed",!found->second.started,true);
    s.active.erase(found);
    }
  s.lastOpportunity.erase(npc);
  }
std::string BodyTrajectory::statsJson() {
  const auto& s=state(); auto row=object(); b(row,"enabled",s.enabled); u(row,"capture_generation",s.generation);
  u(row,"capacity",s.capacity); u(row,"queue_size",s.queue.size()); u(row,"max_queue_size",s.maxQueue);
  u(row,"record_seq_high_watermark",s.sequence); u(row,"dropped_total",s.dropped); u(row,"reset_discarded_total",s.resetDiscarded);
  u(row,"emitted",s.emitted); u(row,"drained",s.drained); u(row,"active_native_executions",s.active.size());
  b(row,"capture_failed",s.failed); u(row,"capture_failures_total",s.failures); str(row,"capture_failure_reason",s.failed ? "native_telemetry_exception" : "");
  return encode(row);
  }
std::string BodyTrajectory::drainJson(size_t max) {
  if(max==0 || max>256) throw std::invalid_argument("trajectory drain max must be 1..256");
  auto& s=state(); auto row=object(); str(row,"type","body_trajectory_events"); u(row,"schema_version",1);
  u(row,"capture_generation",s.generation); u(row,"record_seq_high_watermark",s.sequence);
  u(row,"dropped_total",s.dropped); u(row,"dropped_since_drain",s.dropped-s.lastDrainDropped); s.lastDrainDropped=s.dropped;
  u(row,"reset_discarded_total",s.resetDiscarded); u(row,"capacity",s.capacity);
  b(row,"capture_failed",s.failed); u(row,"capture_failures_total",s.failures); str(row,"capture_failure_reason",s.failed ? "native_telemetry_exception" : "");
  rapidjson::Value rows(rapidjson::kArrayType);
  for(size_t i=0;i<max && !s.queue.empty();++i) {
    auto item=parse(s.queue.front()); rows.PushBack(rapidjson::Value(item,row.GetAllocator()),row.GetAllocator()); s.queue.pop_front(); ++s.drained;
    }
  row.AddMember("events",rows,row.GetAllocator()); u(row,"queue_size",s.queue.size()); return encode(row);
  }
