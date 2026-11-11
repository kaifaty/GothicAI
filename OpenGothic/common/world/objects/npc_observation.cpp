#include "npc.h"

#include "game/bodycontroller.h"
#include "game/bodyobservations.h"
#include "graphics/mesh/skeleton.h"
#include "world/objects/item.h"
#include "world/world.h"
#include "testbench.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <iomanip>
#include <locale>
#include <sstream>
#include <stdexcept>

using namespace Tempest;

std::string Npc::combatPoseIdentity() const {
  std::string result = std::to_string(visual.pose().comboLength());
  for(const auto& layer:visual.pose().combatLayers(owner.tickCount()))
    result += "|" + layer.sequence->name + ":" + std::to_string(layer.start) + ":" +
              std::to_string(layer.speed) + ":" + std::to_string(layer.combination) + ":" +
              std::to_string(unsigned(layer.state));
  return result;
  }

std::string Npc::combatPoseLayers() const {
  std::string result = "[";
  const auto times = [](const auto& values) {
    std::string json = "[";
    for(const auto value:values) {
      if(json.size()>1)
        json += ",";
      json += std::to_string(value);
      }
    return json + "]";
    };
  for(const auto& layer:visual.pose().combatLayers(owner.tickCount())) {
    const auto& seq = *layer.sequence;
    const auto& data = *seq.data;
    std::vector<int32_t> optimal;
    for(const auto& ev:data.events)
      if(ev.type==asset::MdsEventType::OPTIMAL_FRAME)
        for(auto frame:ev.frames)
          optimal.push_back(frame);
    if(result.size()>1)
      result += ",";
    result += "{\"name\":" + TestBench::bodyJson(seq.name) +
              ",\"is_attack\":" + (seq.isAttackAnim() || isFinishSequence(seq) ? "true" : "false") +
              ",\"start\":" + std::to_string(layer.start) + ",\"clock\":" + std::to_string(layer.time) +
              ",\"speed\":" + std::to_string(layer.speed) + ",\"combination\":" + std::to_string(layer.combination) +
              ",\"body_state\":" + std::to_string(unsigned(layer.state)) +
              ",\"combo_length\":" + std::to_string(visual.pose().comboLength()) +
              ",\"fps\":" + std::to_string(data.fpsRate) + ",\"duration_ms\":" + std::to_string(seq.totalTime()) +
              ",\"def_window_ms\":" + times(data.defWindow) + ",\"def_par_ms\":" + times(data.defParFrame) +
              ",\"hit_end_ms\":" + times(data.defHitEnd) + ",\"optimal_frames\":" + times(optimal) + "}";
    }
  return result + "]";
  }

void Npc::traceCombatSnapshot() {
  if(!TestBench::bodyTracing() || currentTarget==nullptr)
    return;
  const auto now = owner.tickCount();
  const auto& pose = visual.pose();
  const bool prehit = pose.isPrehit(now), defence = pose.isDefence(now), jumpback = pose.isJumpBack(now);
  const bool attack = pose.isAttackAnim(), defWindow = pose.isDefWindow(now), parWindow = pose.isDefParWindow(now);
  const std::string state = combatPoseIdentity() + ":" + std::to_string(unsigned(bodyState())) + ":" +
                           std::to_string(int(weaponState())) + ":" + std::to_string(prehit) + ":" +
                           std::to_string(defence) + ":" + std::to_string(jumpback) + ":" +
                           std::to_string(attack) + ":" + std::to_string(defWindow) + ":" +
                           std::to_string(parWindow) + ":" + std::to_string(isDown()) + ":" +
                           std::to_string(isMeleeStunned()) + ":" +
                           std::to_string(attribute(ATR_HITPOINTS)) + ":" +
                           std::to_string(int(fghAlgo.queuedAction())) + ":" +
                           std::to_string(int(processPolicy())) + ":" +
                           std::to_string(faiWaitTime>=now) + ":" + std::to_string(aniWaitTime>=now) + ":" +
                           std::to_string(waitTime>=now) + ":" + std::to_string(outWaitTime>now);
  const bool changed = state!=bodyTraceState;
  // At 30 Hz this samples adjacent 50 ms buckets (33/67 ms spacing); window/layer edges are always logged.
  if(!changed && now/50==bodyTraceSampleTime/50)
    return;
  bodyTraceState = state;
  bodyTraceSampleTime = now;
  char values[1400];
  std::snprintf(values,sizeof(values),
                "\"sample_phase\":\"npc_pre_tick_animation_tags\",\"edge\":%s,\"hp\":%d,\"body_state\":%u,"
                "\"weapon_state\":%d,\"prehit\":%s,\"defence\":%s,\"jumpback\":%s,\"attack_anim\":%s,"
                "\"def_window\":%s,\"def_par_window\":%s,\"down\":%s,\"dead\":%s,\"x\":%.6f,\"y\":%.6f,"
                "\"z\":%.6f,\"yaw\":%.6f,\"fai_wait_until\":%llu,\"ani_wait_until\":%llu,"
                "\"wait_until\":%llu,\"output_wait_until\":%llu,\"script_queue_size\":%zu,\"primitive\":%d,\"choice_id\":%llu",
                changed ? "true" : "false",int(attribute(ATR_HITPOINTS)),unsigned(bodyState()),int(weaponState()),
                prehit ? "true" : "false",defence ? "true" : "false",jumpback ? "true" : "false",attack ? "true" : "false",
                defWindow ? "true" : "false",parWindow ? "true" : "false",isDown() ? "true" : "false",isDead() ? "true" : "false",
                double(x),double(y),double(z),double(angle),(unsigned long long)faiWaitTime,(unsigned long long)aniWaitTime,
                (unsigned long long)waitTime,(unsigned long long)outWaitTime,size_t(aiQueue.size()),int(fghAlgo.queuedAction()),
                (unsigned long long)fghAlgo.traceChoiceId());
  TestBench::bodyEvent(*this,currentTarget,"phase_snapshot",now,std::string(values) +
                       ",\"stunned\":" + (isMeleeStunned() ? "true" : "false") +
                       ",\"stamina\":" + std::to_string(stamina()) +
                       ",\"stamina_max\":" + std::to_string(staminaMax()) +
                       ",\"process_policy\":" + std::to_string(int(processPolicy())) +
                       ",\"native_ai_normal\":" + (processPolicy()==NpcProcessPolicy::AiNormal ? "true" : "false") +
                       ",\"layers\":" + combatPoseLayers(),pose.animNames());
  }

void Npc::traceVisibilityDiagnostic(std::string_view sourcePhase, bool force) noexcept {
  if(!BodyController::visibilityDiagnosticsEnabled() || !TestBench::bodyTracing() ||
     !BodyObservations::watched(*this) || currentTarget==nullptr)
    return;
  try {
    constexpr uint64_t PeriodMs=50, MaxRecords=8192;
    const auto generation=BodyController::visibilityDiagnosticsGeneration();
    if(visibilityDiagnosticGeneration!=generation) {
      visibilityDiagnosticGeneration=generation; visibilityDiagnosticCount=0;
      visibilityDiagnosticTime={}; visibilityDiagnosticKey={};
      }
    const auto now=owner.tickCount();
    const size_t slot=sourcePhase=="post_observation" ? 1 : 0;
    if(visibilityDiagnosticCount>=MaxRecords) {
      if(force || visibilityDiagnosticKey[slot].empty() || now/PeriodMs!=visibilityDiagnosticTime[slot]/PeriodMs) {
        BodyController::visibilityDiagnosticRecorded(true); visibilityDiagnosticTime[slot]=now;
        }
      return;
      }
    using Clock=std::chrono::steady_clock;
    const auto snapshotTime=Clock::now();
    const auto eye=visual.mapHeadBone();
    const auto& other=*currentTarget;
    const auto view=visual.viewDirection();
    const auto range=float(hnpc->senses_range);
    const auto threshold=std::cos(100*M_PI/180.0);
    struct Ray final {
      Vec3 point={}, hitPoint={}, hitNormal={};
      float distance=0, angle=0, fraction=0;
      double coneCos=0, bodyConeCos=0;
      bool rangePass=false, fovPass=false, bodyFovPass=false, hit=false, vob=false, passes=false;
      };
    const auto inspect=[&](const Vec3& point) {
      Ray result; result.point=point;
      result.distance=std::sqrt(qDistTo(point)); result.rangePass=qDistTo(point)<=range*range;
      const auto bearing=angleDir(eye.x-point.x,eye.z-point.z);
      result.angle=view-bearing;
      // Match the legacy float-angle/cos comparison exactly. Unconditional static rays
      // explain a cone failure too, but never feed back into the actual query or model.
      result.coneCos=double(std::cos(float(M_PI)*(view-bearing)/180.f));
      result.bodyConeCos=double(std::cos(float(M_PI)*(rotation()-bearing)/180.f));
      result.fovPass=result.coneCos<=threshold; result.bodyFovPass=result.bodyConeCos<=threshold;
      const auto hit=owner.physic()->ray(eye,point);
      result.hit=hit.hasCol; result.vob=hit.hasCol && hit.vob!=nullptr;
      if(hit.hasCol) { result.hitPoint=hit.v; result.hitNormal=hit.n; result.fraction=hit.hitFraction; }
      result.passes=result.rangePass && result.fovPass && !result.hit;
      return result;
      };
    const auto center=inspect(other.physic.center());
    const auto skeleton=other.visual.visualSkeleton();
    const bool hasHead=skeleton!=nullptr && skeleton->BIP01_HEAD!=size_t(-1);
    const auto head=hasHead ? inspect(other.visual.mapHeadBone()) : Ray{};
    const bool actual=canSeeNpc(other,false);
    const bool sense=(hnpc->senses & int32_t(SensesBit::SENSE_SEE))!=0;
    const bool targetTerminal=other.isDown() || other.attribute(ATR_HITPOINTS)<=0;
    const auto control=BodyController::snapshot(*this);
    const auto animation=animNames();
    const auto key=animation+":"+std::to_string(actual)+":"+std::to_string(center.fovPass)+":"+
                   std::to_string(head.fovPass)+":"+std::to_string(center.hit)+":"+std::to_string(head.hit);
    if(!force && visibilityDiagnosticKey[slot]==key && now/PeriodMs==visibilityDiagnosticTime[slot]/PeriodMs)
      return;
    const char* reason="mixed_ray_rejection";
    if(actual) reason="visible";
    else if(!center.rangePass && (!hasHead || !head.rangePass)) reason="out_of_senses_range";
    else if(!center.fovPass && (!hasHead || !head.fovPass)) reason="outside_pose_cone";
    else if(center.hit && (!hasHead || head.hit)) reason="static_occlusion";
    else if(!hasHead) reason="target_head_missing";
    std::ostringstream out; out.imbue(std::locale::classic()); out << std::boolalpha << std::setprecision(9);
    const auto vec=[&](const Vec3& point) {
      if(!std::isfinite(point.x) || !std::isfinite(point.y) || !std::isfinite(point.z))
        throw std::runtime_error("nonfinite visibility diagnostic position");
      out << '[' << point.x << ',' << point.y << ',' << point.z << ']';
      };
    const auto ray=[&](const Ray& value) {
      if(!std::isfinite(value.distance) || !std::isfinite(value.angle) || !std::isfinite(value.coneCos) ||
         !std::isfinite(value.bodyConeCos) || !std::isfinite(value.fraction))
        throw std::runtime_error("nonfinite visibility diagnostic ray");
      out << "{\"range_pass\":" << value.rangePass << ",\"distance_cm\":" << value.distance
          << ",\"fov_pass\":" << value.fovPass << ",\"legacy_angle_deg\":" << value.angle
          << ",\"cone_cos\":" << value.coneCos << ",\"body_cone_cos\":" << value.bodyConeCos
          << ",\"body_fov_pass\":" << value.bodyFovPass << ",\"ray_hit\":" << value.hit
          << ",\"hit_vob\":" << value.vob << ",\"hit_fraction\":" << value.fraction
          << ",\"query_pass\":" << value.passes << ",\"target_point_cm\":";
      vec(value.point); out << ",\"hit_point_cm\":";
      if(value.hit) vec(value.hitPoint); else out << "null";
      out << ",\"hit_normal\":"; if(value.hit) vec(value.hitNormal); else out << "null";
      out << '}';
      };
    if(!std::isfinite(view) || !std::isfinite(rotation())) throw std::runtime_error("nonfinite visibility diagnostic yaw");
    const auto ns=uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(snapshotTime.time_since_epoch()).count());
    out << "\"diagnostic_schema_version\":1,\"source_phase\":" << TestBench::bodyJson(sourcePhase)
        << ",\"diagnostic_generation\":" << generation << ",\"diagnostic_seq\":" << visibilityDiagnosticCount+1
        << ",\"oracle_debug_only\":true,\"sample_period_ms\":" << PeriodMs << ",\"forced_edge\":" << force
        << ",\"snapshot_wall_monotonic_ns\":" << ns << ",\"wall_monotonic_ns\":" << ns
#ifdef __linux__
        << ",\"clock_domain\":\"linux-clock-monotonic\""
#else
        << ",\"clock_domain\":\"cxx-steady-clock\""
#endif
        << ",\"sim_step\":" << BodyObservations::currentStep() << ",\"session_epoch\":" << BodyObservations::sessionEpoch()
        << ",\"world_epoch\":" << BodyObservations::worldEpoch() << ",\"entity_id\":" << BodyObservations::entityId(*this)
        << ",\"target_entity_id\":" << BodyObservations::entityId(other)
        << ",\"owner\":" << TestBench::bodyJson(control.owned ? "scripted" : "native")
        << ",\"provider\":" << TestBench::bodyJson(control.provider) << ",\"control_generation\":" << control.controlGeneration
        << ",\"command_id\":" << control.commandId << ",\"execution_id\":" << control.executionId
        << ",\"release_pending\":" << control.releasePending << ",\"control_allowed\":" << control.controlAllowed
        << ",\"body_state_base\":" << unsigned(bodyStateMasked()) << ",\"native_fai_wait\":" << (faiWaitTime>=now)
        << ",\"native_script_wait\":" << (waitTime>=now)
        << ",\"native_fai_wait_remaining_ms\":" << (faiWaitTime>now ? faiWaitTime-now : 0)
        << ",\"native_script_wait_remaining_ms\":" << (waitTime>now ? waitTime-now : 0)
        << ",\"body_yaw_deg\":" << rotation() << ",\"pose_view_deg\":" << view
        << ",\"fov_half_angle_deg\":100,\"legacy_cos_threshold\":" << threshold
        << ",\"senses_range_cm\":" << range << ",\"senses_see\":" << sense << ",\"target_terminal\":" << targetTerminal
        << ",\"actual_can_see\":" << actual << ",\"controller_visible\":" << (sense && actual && !targetTerminal)
        << ",\"query_matches_diagnostic\":" << (actual==(center.passes || (hasHead && head.passes)))
        << ",\"reason\":" << TestBench::bodyJson(reason) << ",\"eye_cm\":";
    vec(eye); out << ",\"self_position_cm\":"; vec(position()); out << ",\"target_position_cm\":"; vec(other.position());
    out << ",\"target_has_head\":" << hasHead << ",\"center_ray\":"; ray(center);
    out << ",\"head_ray\":"; if(hasHead) ray(head); else out << "null";
    TestBench::bodyEvent(*this,&other,"visibility_diagnostic",now,out.str(),animation);
    ++visibilityDiagnosticCount; visibilityDiagnosticTime[slot]=now; visibilityDiagnosticKey[slot]=key;
    BodyController::visibilityDiagnosticRecorded();
    }
  catch(...) { BodyController::visibilityDiagnosticFailed(); }
  }

void Npc::traceCombatGate(std::string_view reason) {
  if(!TestBench::bodyTracing() || currentTarget==nullptr)
    return;
  const auto key = std::string(reason) + ":" + std::to_string(fghAlgo.traceChoiceId()) + ":" +
                   std::to_string(int(fghAlgo.queuedAction()));
  if(key==bodyTraceGate)
    return;
  bodyTraceGate = key;
  TestBench::bodyEvent(*this,currentTarget,"native_gate",owner.tickCount(),
                      "\"reason\":" + TestBench::bodyJson(reason) + ",\"choice_id\":" + std::to_string(fghAlgo.traceChoiceId()) +
                      ",\"primitive\":" + std::to_string(int(fghAlgo.queuedAction())));
  }

Npc::BodyObservationState Npc::bodyObservationState() const {
  BodyObservationState s;
  const uint64_t now = owner.tickCount();
  const auto& pose = visual.pose();
  const auto ws = weaponState();
  s.bodyState = uint32_t(bodyState());
  s.weaponState = uint8_t(ws);
  if(const auto weapon = invent.activeWeapon()) {
    s.weaponSymbol = uint32_t(weapon->clsId());
    s.weaponLength = weapon->swordLength();
    }
  switch(ws) {
    case WeaponState::W1H: s.skillTalent = TALENT_1H; break;
    case WeaponState::W2H: s.skillTalent = TALENT_2H; break;
    case WeaponState::Bow: s.skillTalent = TALENT_BOW; break;
    case WeaponState::CBow: s.skillTalent = TALENT_CROSSBOW; break;
    default: break;
    }
  s.skill = s.skillTalent==TALENT_UNKNOWN ? 0 : hitChance(Talent(s.skillTalent));
  s.fightTactic = hnpc->fight_tactic;
  s.primitive = int32_t(fghAlgo.queuedAction());
  s.choiceId = fghAlgo.traceChoiceId();
  const auto remaining = [now](uint64_t deadline) { return deadline>now ? deadline-now : 0; };
  s.faiWaitMs = remaining(faiWaitTime);
  s.aniWaitMs = remaining(aniWaitTime);
  s.aiWaitMs = remaining(waitTime);
  s.outputWaitMs = remaining(outWaitTime);
  s.faiGate = faiWaitTime>=now;
  s.aniGate = aniWaitTime>=now;
  s.aiGate = waitTime>=now;
  s.outputGate = outWaitTime>now;
  s.attackAnim = pose.isAttackAnim();
  s.prehit = pose.isPrehit(now);
  s.defence = pose.isDefence(now);
  s.jumpBack = pose.isJumpBack(now);
  s.defWindow = pose.isDefWindow(now);
  s.parWindow = pose.isDefParWindow(now);
  s.interruptible = true;
  for(const auto& layer:pose.combatLayers(now)) {
    if(layer.sequence==nullptr)
      continue;
    const auto& seq = *layer.sequence;
    if(seq.isAttackAnim() || layer.state==BS_PARADE) {
      const auto duration=seq.isAttackAnim() ? seq.atkTotalTime(pose.comboLength()) : seq.totalTime();
      const auto elapsed=layer.time>=layer.start ? double(layer.time-layer.start) : 0.0;
      if(double(duration)>=elapsed && layer.speed>0)
        s.segmentRemainingMs=std::max(s.segmentRemainingMs,uint64_t(std::ceil((double(duration)-elapsed)/double(layer.speed)))+1);
      }
    const bool finished = seq.isFinished(layer.time,layer.start,pose.comboLength()) &&
                          seq.next.empty() && seq.animCls!=Animation::Loop;
    if(layer.state==BS_ITEMINTERACT || (!finished && !seq.canInterrupt(layer.time,layer.start,pose.comboLength())))
      s.interruptible = false;
    }
  // AttackBlock's generic solver draws std::rand() for G2 sword variants. Only inspect the
  // actual named clips here; every other listed primitive lookup is free of RNG.
  s.hasMove = hasAnim(Anim::Move);
  s.hasLeft = hasAnim(Anim::MoveL);
  s.hasRight = hasAnim(Anim::MoveR);
  s.hasBack = hasAnim(Anim::MoveBack);
  s.hasAttack = hasAnim(Anim::Attack);
  s.hasAttackLeft = hasAnim(Anim::AttackL);
  s.hasAttackRight = hasAnim(Anim::AttackR);
  if(ws==WeaponState::W1H || ws==WeaponState::W2H) {
    const std::string prefix = ws==WeaponState::W1H ? "T_1HPARADE_" : "T_2HPARADE_";
    s.hasBlock = hasAnim(prefix+"0");
    }
  const auto bs = bodyStateMasked();
  const bool melee = ws==WeaponState::W1H || ws==WeaponState::W2H;
  s.nativeEligible = melee && !isPlayer() && !isTalk() && !isDown() && !isInAir() &&
                     !isSwim() && !isDive() && !externalCtrl && aiQueue.size()==0 &&
                     bs!=BS_HIT && bs!=BS_LIE && bs!=BS_STUMBLE && bs!=BS_FALL &&
                     !s.faiGate && !s.aniGate && !s.aiGate && !s.outputGate && s.interruptible;
  return s;
  }

Npc::BodyObservationTarget Npc::bodyObservationTarget(const Npc& tg) const {
  BodyObservationTarget s;
  auto& script = owner.script();
  s.fightDistanceCm = fightDistanceTo(tg).length();
  s.attackRangeCm = fghAlgo.prefferedAttackDistance(*this,tg,script);
  s.gRangeCm = fghAlgo.prefferedGDistance(*this,tg,script);
  s.closeupRangeCm = 0.75f*fghAlgo.baseDistance(*this,tg,script);
  s.inWRange = fghAlgo.isInWRange(*this,tg,script);
  s.inGRange = fghAlgo.isInGRange(*this,tg,script);
  s.inCloseup = fghAlgo.isInCloseupRange(*this,tg,script);
  s.focus30 = fghAlgo.isInFocusAngle(*this,tg);
  s.focus5 = fghAlgo.isInFocusAngle(*this,tg,5.f);
  s.meleeLineClear = !owner.physic()->ray(collosionCenter(),tg.collosionCenter()).hasCol;
  return s;
  }

void Npc::traceCombatAttempt(Anim requested, bool accepted, const std::string& before, std::string_view hook) {
  if(!TestBench::bodyTracing())
    return;
  const auto after = combatPoseIdentity();
  const bool changed = before!=after;
  TestBench::bodyEvent(*this,currentTarget,"native_attempt",owner.tickCount(),
                      "\"hook\":" + TestBench::bodyJson(hook) + ",\"choice_id\":" + std::to_string(fghAlgo.traceChoiceId()) + ",\"primitive\":" +
                      std::to_string(int(fghAlgo.queuedAction())) + ",\"requested_anim\":" + std::to_string(int(requested)) +
                      ",\"accepted\":" + (accepted ? "true" : "false") + ",\"pose_changed\":" + (changed ? "true" : "false") +
                      ",\"pose_before\":" + TestBench::bodyJson(before) + ",\"pose_after\":" + TestBench::bodyJson(after) +
                      ",\"layers_after\":" + combatPoseLayers(),visual.pose().animNames());
  }
