#include "npc.h"

#include "graphics/mesh/skeleton.h"
#include "game/gamescript.h"
#include "game/meleeriposte.h"
#include "world/objects/interactive.h"
#include "world/objects/item.h"
#include "world/world.h"
#include "gothic.h"
#include "loadprobe.h"
#include "navdemos.h"

#include <algorithm>
#include <cmath>
#include <memory>

using namespace Tempest;

bool Npc::setPosition(float ix, float iy, float iz) {
  if(x==ix && y==iy && z==iz)
    return false;
  const bool liveTeleport = !isDown() && !ragdoll && (position()-Vec3(ix,iy,iz)).length()>300.f;
  const auto* skeleton = visual.visualSkeleton();
  std::optional<Ragdoll::State> movedBody, movedDormant;
  auto relocate = [&](Ragdoll::State& state) {
    // A disabled body's origin can differ from the legacy animated origin.
    const Vec3 at(float(double(ix)+(double(state.rootTransform.at(3,0))-double(x))),
                  float(double(iy)+(double(state.rootTransform.at(3,1))-double(y))),
                  float(double(iz)+(double(state.rootTransform.at(3,2))-double(z))));
    return skeleton!=nullptr && Ragdoll::relocateState(*skeleton,state,at);
    };
  if(ragdoll) {
    movedBody = ragdoll->state();
    if(!relocate(*movedBody))
      return false;
    }
  if(ragdollDormant) {
    movedDormant = *ragdollDormant;
    if(!relocate(*movedDormant))
      return false;
    }
  if(movedBody) {
    if(!ragdoll->restore(*movedBody))
      return false;
    ragdollObjectMatrix = ragdoll->rootTransform();
    }
  if(movedDormant)
    ragdollDormant = std::move(movedDormant);
  x = ix;
  y = iy;
  z = iz;
  visual.setTextureOwner(Vec3(x,y,z),!isPlayer());
  resetCharacterVisualHeight();
  if(!isPlayer() && (isHuman() || Gothic::physicsFlags().monsterCharacterVirtual))
    mvAlgo.resetCharacterVirtual();
  if(liveTeleport) {
    // Recreate at the destination instead of sweeping a kinematic body through
    // every item between a scripted teleport's endpoints.
    living.reset();
    arrowImpacts.clear();
    // Large scripted relocations start a new history; normal movement samples
    // must retain the last supported position when the NPC steps off an edge.
    ragdollQuestReturn = {};
    ragdollQuestSpellThrow = false;
    ragdollQuestSampleTick = 0;
    }
  ragdollHasLastPosition = false;
  ragdollLastVelocity = {};
  durtyTranform |= TR_Pos;
  physic.setPosition(Vec3{x,y,z});
  if(movedBody)
    syncRagdoll();
  return true;
  }

bool Npc::setPosition(const Tempest::Vec3& pos) {
  return setPosition(pos.x,pos.y,pos.z);
  }

void Npc::setViewPosition(const Tempest::Vec3& pos) {
  // Routine legacy alignment must not move the frozen physical pose. Explicit
  // teleports use setPosition and translate the dormant state there.
  x = pos.x;
  y = pos.y;
  z = pos.z;
  visual.setTextureOwner(pos,!isPlayer());
  durtyTranform |= TR_Pos;
  }

void Npc::syncCharacterVirtualSupport(const Tempest::Vec3& pos) {
  // Animation already sampled these feet before the physics step. Transport
  // that sample with the accepted support motion so a lift does not become a
  // fictitious stair correction at the same timestamp.
  if(characterVisualHasSample && characterVisualSampleTick==owner.tickCount())
    characterVisualSamplePosition += pos-position();
  setViewPosition(pos);
  updateTransform();
  }

void Npc::setDirection(const Tempest::Vec3& pos) {
  float a = angleDir(pos.x, pos.z);
  setDirection(a);
  }

void Npc::setDirection(float rotation) {
  durtyTranform |= TR_Rot;
  angle = rotation;
  physic.setRotation(angle);
  }

void Npc::setDirectionY(float rotation) {
  if(rotation>90)
    rotation = 90;
  if(rotation<-90)
    rotation = -90;
  rotation = std::fmod(rotation,360.f);
  if(!mvAlgo.isDive() && !(interactive()!=nullptr && interactive()->isLadder()))
    return;
  angleY = rotation;
  durtyTranform |= TR_Rot;
  }

void Npc::setRunAngle(float angle) {
  durtyTranform |= TR_Rot;
  runAng = angle;
  }

float Npc::angleDir(float x, float z) {
  float a = 0;
  if(x!=0.f || z!=0.f)
    a = 180.f*std::atan2(z,x)/float(M_PI);
  return a;
  }

bool Npc::resetPositionToTA() {
  LoadProbe::Startup::Detail::Scope reset("reset_npc");
  endMeleeAiSpecial();
  const bool isDragon = (guild()==GIL_DRAGON);
  const bool isDead   = this->isDead();

  if(isDead && !invent.hasMissionItems() && !isDragon)
    return false;

  invent.clearSlot(*this,"",currentInteract!=nullptr);
  if(!isPlayer())
    setInteraction(nullptr,true);

  // return monsters to their way-points
  // if(routines.empty() && !isPlayer())
  //   return currentTaPoint()!=nullptr;

  attachToPoint(nullptr);
  clearAiQueue();

  if(!isDead) {
    visual.stopAnim(*this,"");
    clearState(true);
    }

  if(isPlayer())
    return true;

  LoadProbe::Startup::Detail::Scope routine("current_ta");
  auto at = currentTaPoint();
  routine.stop();
  if(at==nullptr)
    return false;

  if(at->isLocked() && !isDead) {
    LoadProbe::Startup::Detail::Scope next("next_point");
    auto p = owner.findNextPoint(*at);
    if(p!=nullptr)
      at = p;
    }
  owner.script().startupFloor().beforeResetMove(*this,at->position());
  setPosition (at->position() );
  setDirection(at->direction());
  owner.script().fixNpcPosition(*this,0,100);

  if(!isDead) {
    attachToPoint(at);
    LoadProbe::Startup::Detail::Scope equip("auto_equip");
    invent.autoEquipWeapons(*this);
    }

  owner.script().invokeRefreshAtInsert(*this);
  return true;
  }

void Npc::clearSpeed() {
  mvAlgo.clearSpeed();
  }

void Npc::resetNavigationEpisode() {
  if(!NavDemos::fullSimulation(*this))
    return;
  setInteraction(nullptr,true);
  clearAiQueue();
  clearState(true);
  hnpc->attribute[ATR_HITPOINTS] = hnpc->attribute[ATR_HITPOINTSMAX];
  releaseRagdoll(false);
  ragdollDormant.reset();
  ragdollLegacyDown = ragdollRecovering = ragdollHasLastPosition = false;
  meleePosture.reset();
  meleeBargeOrigin.reset();
  meleeKnockdown = false;
  meleeBargeKnockdown = false;
  meleeRiposteKnockdown = false;
  meleeGetupForced = false;
  meleeFallTick = meleeRiseTick = 0;
  meleeSlowSince = 0;
  ragdollLastVelocity = {};
  ragdollQuestReturn = {};
  ragdollQuestSpellThrow = false;
  ragdollQuestSampleTick = 0;
  living.reset();
  livingRetryTick = 0;
  livingNeedsRebuild = livingWasFrozen = livingWasInteracting = false;
  bodyImpactWindow.clear();
  bodyImpactLaunchCulprit.reset();
  bodyImpactPendingCulprit.reset();
  bodyImpactSpellFlight=false;
  arrowImpacts.clear();
  meleeRiposte.reset();
  meleeRiposteHoldUntil = 0;
  pendingSpellImpulse = {};
  spellImpulseCulprit.reset();
  visual.clearPhysicalPose();
  visual.pose().stopAllAnim();
  // Called between world ticks: reconstruct transient state (including bounce,
  // cached ground, speed multipliers and controller hysteresis) for a fresh episode.
  mvAlgo.resetCharacterVirtual();
  std::destroy_at(&mvAlgo);
  std::construct_at(&mvAlgo,*this);
  currentTarget = nearestEnemy = currentOther = currentVictim = lastHit = nullptr;
  outWaitTime = aiOutputBarrier = physicsNoiseUntil = 0;
  castLevel = CS_NoCast;
  currentSpellCast = size_t(-1);
  castNextTime = 0;
  lastEventTime = owner.tickCount();
  resetCharacterVisualHeight();
  angleY = runAng = 0;
  stopWalking();
  physic.setEnable(true);
  }

Npc::EyePose Npc::navigationEyePose() const {
  syncDeferredAnimation();
  EyePose result;
  const auto& model = visual.transform();
  result.origin = Vec3(model.at(3,0),model.at(3,1)+180.f,model.at(3,2));
  result.forward = Vec3::normalize(Vec3(model.at(2,0),model.at(2,1),model.at(2,2)));
  if(result.forward.length()<0.5f)
    result.forward = Vec3(std::cos(rotationRad()),0,std::sin(rotationRad()));
  const auto* skeleton = visual.visualSkeleton();
  const auto axis = [](const Tempest::Matrix4x4& m, size_t k) {
    const int column = int(k);
    return Vec3::normalize(Vec3(m.at(column,0),m.at(column,1),m.at(column,2)));
    };
  if(skeleton!=nullptr && skeleton->BIP01_HEAD<visual.pose().boneCount() &&
     skeleton->BIP01_HEAD<skeleton->tr.size()) {
    const size_t head = skeleton->BIP01_HEAD;
    const auto& bind = skeleton->tr[head];
    Vec3 bindForward(0,0,1);
    const size_t left = skeleton->findNode("BIP01 L THIGH");
    const size_t right = skeleton->findNode("BIP01 R THIGH");
    if(left<skeleton->tr.size() && right<skeleton->tr.size()) {
      const auto& l = skeleton->tr[left];
      const auto& r = skeleton->tr[right];
      const Vec3 hips = Vec3::crossProduct(Vec3(0,1,0),Vec3(l.at(3,0)-r.at(3,0),0,l.at(3,2)-r.at(3,2)));
      if(hips.length()>1.e-3f)
        bindForward = Vec3::normalize(hips);
      }
    // The same bind-pose face-axis choice used by Gaze; apply it to the finished head.
    size_t forwardAxis = 0, upAxis = 0;
    float forwardDot = 0, upDot = 0;
    for(size_t k=0; k<3; ++k) {
      const auto a = axis(bind,k);
      const float f = Vec3::dotProduct(a,bindForward);
      if(std::abs(f)>std::abs(forwardDot)) {
        forwardAxis = k;
        forwardDot = f;
        }
      if(std::abs(a.y)>std::abs(upDot)) {
        upAxis = k;
        upDot = a.y;
        }
      }
    const auto& shown = visual.renderedBone(head);
    result.origin = Vec3(shown.at(3,0),shown.at(3,1),shown.at(3,2));
    result.forward = axis(shown,forwardAxis)*(forwardDot<0.f ? -1.f : 1.f);
    result.up = axis(shown,upAxis)*(upDot<0.f ? -1.f : 1.f);
    result.animatedHead = true;
    }
  result.right = Vec3::crossProduct(result.forward,result.up);
  if(result.right.length()<1.e-3f)
    result.right = Vec3::crossProduct(result.forward,Vec3(0,0,1));
  result.right = Vec3::normalize(result.right);
  result.up = Vec3::normalize(Vec3::crossProduct(result.right,result.forward));
  return result;
  }

bool Npc::turnTo(float dx, float dz, bool noAnim, uint64_t dt) {
  return implTurnTo(dx,dz,noAnim?AnimationSolver::TurnType::None:AnimationSolver::TurnType::Std,dt);
  }

bool Npc::rotateTo(float dx, float dz, float step, AnimationSolver::TurnType anim, uint64_t dt) {
  //step *= (float(dt)/1000.f)*60.f/100.f;
  step *= (float(dt)/1000.f);

  if(dx==0.f && dz==0.f) {
    if(!usesSimplifiedMovement() || !nativeBodyAction())
      setAnimRotate(0);
    return false;
    }

  if(!isRotationAllowed())
    return false;

  if(usesSimplifiedMovement()) {
    const float target = angleDir(dx,dz);
    const float delta = std::remainder(target-angle,360.f);
    if(!nativeBodyAction())
      setAnimRotate(0);
    if(std::abs(delta)<=step*2.f) {
      setDirection(target);
      return false;
      }
    setDirection(angle+std::clamp(delta,-step,step));
    return true;
    }

  float a  = angleDir(dx,dz);
  float da = a-angle;

  if(anim == AnimationSolver::TurnType::None || std::cos(double(da)*M_PI/180.0)>0) {
    if(float(std::abs(int(da)%360))<=(step*2.f)) {
      setAnimRotate(0);
      setDirection(a);
      return false;
      }
    } else {
    visual.stopWalkAnim(*this);
    }

  const auto sgn = std::sin(double(da)*M_PI/180.0);
  if(sgn==0) {
    setAnimRotate(0);
    } else {
    const int rot = (sgn<0) ? +1 : -1;
    switch(anim) {
      case AnimationSolver::TurnType::Std:
        setAnimRotate(rot);
        break;
      case AnimationSolver::TurnType::None:
        setAnimRotate(0);
        break;
      case AnimationSolver::TurnType::Whirl:
        visual.setAnimWhirl(*this, rot);
        break;
      }
    setDirection(angle - float(rot)*step);
    }
  return true;
  }

bool Npc::isRotationAllowed() const {
  auto bs  = bodyStateMasked();
  bool air = (!isPlayer() && isInAir()) || isFallingDeep();
  return currentInteract==nullptr && !isFinishingMove() && bs!=BS_CLIMB && bs!=BS_LIE && !air;
  }

void Npc::multSpeed(float s) {
  mvAlgo.multSpeed(s);
  }

bool Npc::testMove(const Vec3& pos) {
  DynamicWorld::CollisionTest out;
  return physic.testMove(pos,out);
  }

bool Npc::tryMove(const Vec3& dp) {
  DynamicWorld::CollisionTest out;
  return tryMove(dp, out);
  }

bool Npc::tryMove(const Vec3& dp, DynamicWorld::CollisionTest& out) {
  return tryTranslate(Vec3(x,y,z) + dp, out);
  }

bool Npc::tryTranslate(const Vec3& to) {
  DynamicWorld::CollisionTest out;
  return tryTranslate(to,out);
  }

bool Npc::tryTranslate(const Vec3& to, DynamicWorld::CollisionTest& out) {
  switch(physic.tryMove(to, out)) {
    case DynamicWorld::MoveCode::MC_Fail:
      return false;
    case DynamicWorld::MoveCode::MC_Partial:
      setViewPosition(out.partial);
      return true;
    case DynamicWorld::MoveCode::MC_Skip:
    case DynamicWorld::MoveCode::MC_OK:
      setViewPosition(to);
      return true;
    }
  return false;
  }

Npc::JumpStatus Npc::tryJump() {
  float len = MoveAlgo::climbMove;
  float rot = rotationRad();
  float s   = std::sin(rot), c = std::cos(rot);
  Vec3  dp  = Vec3{len*c, 0, len*s};

  auto& g  = owner.script().guildVal();
  auto  gl = guild();

  if(isSlide() || isSwim() || isDive()) {
    JumpStatus ret;
    ret.anim   = Anim::Idle;
    return ret;
    }

  const float jumpLow = float(g.jumplow_height[gl]);
  const float jumpMid = float(g.jumpmid_height[gl]);
  const float jumpUp  = float(g.jumpup_height[gl]);

  auto pos0 = physic.position();

  JumpStatus ret;
  DynamicWorld::CollisionTest info;
  if(!mvAlgo.isJumpUp() && physic.testMove(pos0+dp,info)) {
    // jump forward
    ret.anim   = Anim::Jump;
    ret.noClimb = true;
    return ret;
    }

  auto  lnd   = owner.physic()->landRay(pos0 + dp + Vec3(0, jumpUp + jumpLow, 0));
  float jumpY = lnd.v.y;
  auto  pos1  = Vec3(pos0.x,jumpY,pos0.z);
  auto  pos2  = pos1 + dp;

  float dY    = jumpY - y;

  if(dY<=0.f ||
     !physic.testMove(pos2,pos1,info)) {
    ret.anim    = Anim::JumpUp;
    ret.height  = y + jumpUp;
    ret.noClimb = true;
    return ret;
    }

  if(!physic.testMove(pos1,pos0,info) ||
     !physic.testMove(pos2,pos1,info)) {
    // check approximate path of climb failed
    ret.anim    = Anim::JumpUp;
    ret.noClimb = true;
    return ret;
    }

  if(dY>=jumpUp || dY>=jumpMid) {
    // Jump to the edge, and then pull up. Height: 200-350cm
    ret.anim   = Anim::JumpUp;
    ret.height = y + jumpUp;
    return ret;
    }

  DynamicWorld::CollisionTest out;
  if(mvAlgo.testSlide(Vec3{pos0.x,jumpY,pos0.z}+dp,out)) {
    // cannot climb to non angled surface
    ret.anim    = Anim::Jump;
    ret.noClimb = true;
    return ret;
    }

  if(mvAlgo.isJumpUp() && dY<=jumpLow + visual.pose().translateY()) {
    // jumpup -> climb
    ret.anim   = Anim::JumpHang;
    ret.height = jumpY;
    return ret;
    }

  if(mvAlgo.isJumpUp()) {
    ret.anim    = Anim::Idle;
    ret.noClimb = true;
    return ret;
    }

  if(dY<=jumpLow) {
    // Without using the hands, just big footstep. Height: 50-100cm
    ret.anim   = Anim::JumpUpLow;
    ret.height = jumpY;
    return ret;
    }

  if(dY<=jumpMid) {
    // Supported on the hands in one sentence. Height: 100-200cm
    ret.anim   = Anim::JumpUpMid;
    ret.height = jumpY;
    return ret;
    }

  return JumpStatus(); // error
  }

void Npc::startDive() {
  mvAlgo.startDive();
  }
