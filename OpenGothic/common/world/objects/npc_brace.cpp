#include "npc.h"
#include "npc_melee.h"
#include "npc_melee_shadow.h"

#include "graphics/mesh/animationsolver.h"
#include "graphics/mesh/pose.h"
#include "graphics/mesh/skeleton.h"
#include "physics/ragdoll.h"
#include "world/world.h"
#include "settings.h"

#include <string>
#include <vector>

using namespace Tempest;

// A protective fall, step 1 of docs/physical-character-plan.md. A conscious human knocked
// off the feet (the hero too) takes the model's S_BRACE_* pose with the motors of the new
// ragdoll; dead, unconscious, restored and animal bodies fall as before.

void Npc::startRagdollBrace(const Vec3& thrown) {
  if(!ragdoll || !Settings::physicsFlags().ragdollBrace || !isHuman() || isDown())
    return;
  const auto  kind     = ragdoll->braceKind(thrown);
  const auto* skeleton = visual.visualSkeleton();
  const auto* seq      = kind!=RagdollBrace::Kind::None && skeleton!=nullptr ? skeleton->sequence(RagdollBrace::clipName(kind)) : nullptr;
  // A model without the clip keeps the ordinary fall, silently: no log line per knockdown.
  if(seq==nullptr || seq->data==nullptr)
    return;
  // The first frame in model space, sampled without playing the clip, as the melee shadow does.
  Pose sampled;
  sampled.setSkeleton(skeleton);
  AnimationSolver solver;
  solver.setSkeleton(skeleton);
  if(!sampled.startAnim(solver,seq,0,BS_NONE,Pose::Force,0))
    return;
  std::vector<Matrix4x4> model(skeleton->nodes.size());
  for(size_t bone=0; bone<model.size(); ++bone)
    if(!sampled.animRelative(seq->name,0.f,size_t(-1),bone,model[bone]))
      return;
  ragdoll->brace(kind,model.data(),model.size(),RagdollBrace::profile());
  }

std::string Npc::ragdollBraceField() const {
  RagdollBrace::Report report;
  const bool braced = ragdoll && ragdoll->braceReport(report);
  return ",\"brace\":\""+std::string(RagdollBrace::clipName(braced ? report.kind : RagdollBrace::Kind::None))+"\"";
  }

void Npc::finishRagdollBrace(std::string_view end) {
  RagdollBrace::Report r;
  if(!ragdoll || !ragdoll->braceReport(r))
    return;
  // Death ends the protection in the same step: the next one drives the ordinary fade.
  ragdoll->endBrace();
  const auto value = [](float v) { return v>=0.f ? std::to_string(v) : std::string("null"); };
  writeMeleePosture("{\"event\":\"brace\",\"tick_ms\":"+std::to_string(owner.tickCount())+
                    ",\"target\":"+meleeShadowWho(this)+",\"fall_ms\":"+std::to_string(meleeFallTick)+
                    ",\"end\":\""+std::string(end)+"\",\"pose\":\""+std::string(RagdollBrace::clipName(r.kind))+"\""+
                    ",\"hand_l_ms\":"+value(r.handMs[0])+",\"hand_r_ms\":"+value(r.handMs[1])+
                    ",\"arm_l_ms\":"+value(r.armMs[0])+",\"arm_r_ms\":"+value(r.armMs[1])+
                    ",\"torso_ms\":"+value(r.torsoMs)+",\"head_impact_m_s\":"+value(r.headImpact)+
                    ",\"elbow_l_deg\":"+value(r.elbowDeg[0])+",\"elbow_r_deg\":"+value(r.elbowDeg[1])+
                    ",\"hand_depth_cm\":"+value(r.handDepthCm)+
                    ",\"mass_upperarm_kg\":"+value(r.upperArmKg)+",\"mass_forearm_kg\":"+value(r.forearmKg)+
                    ",\"mass_hand_kg\":"+value(r.handKg)+
                    ",\"clamp_max_deg\":"+value(r.clampMaxDeg)+",\"clamp_bone\":\""+r.clampBone+"\"}");
  }
