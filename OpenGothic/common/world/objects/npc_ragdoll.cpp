#include "npc.h"

#include <Tempest/Log>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <stdexcept>

#include "game/bodyarena.h"
#include "graphics/mesh/skeleton.h"
#include "physics/collisionworld.h"
#include "world/bullet.h"
#include "world/world.h"
#include "frameprofile.h"
#include "gothic.h"
#include "navdemos.h"

using namespace Tempest;

bool Npc::setRagdollDown(bool death) {
  hnpc->attribute[ATR_HITPOINTS] = death ? 0 : 1;
  onNoHealth(death,HS_NoSound);
  return isDown();
  }

void Npc::addRagdollImpulse(const Vec3& impulse) {
  if(ragdoll)
    ragdoll->addImpulse(impulse);
  }

float Npc::physicsMass() const {
  if(ragdoll)
    return ragdoll->mass();
  if(living)
    return living->mass();
  const auto* skeleton = visual.visualSkeleton();
  const float base = skeleton!=nullptr ? Ragdoll::speciesMass(skeleton->name()) : 75.f;
  return std::max(0.01f,base*std::abs(sz[0]*sz[1]*sz[2]));
  }

bool Npc::addSpellImpulse(const Vec3& impulseNs, bool reaction, Npc* culprit) {
  if(!Gothic::physicsFlags().spellPhysics || spellFrozen() ||
     !std::isfinite(impulseNs.x) || !std::isfinite(impulseNs.y) || !std::isfinite(impulseNs.z) ||
     impulseNs.quadLength()<=0.f)
    return false;
  // A spell's blow inside a dodge's window does not throw the dodger (its damage is refused in takeDamage)
  if(reaction && isMeleeDodgeInvulnerable())
    return false;
  if(ragdoll) {
    ragdoll->addImpulse(impulseNs,culprit);
    bodyImpactLaunchCulprit=culprit!=nullptr ? culprit->handlePtr() : std::weak_ptr<daedalus::INpc>();
    return true;
    }
  if(isDown()) {
    pendingSpellImpulse += impulseNs;
    spellImpulseCulprit = culprit!=nullptr ? culprit->handlePtr() : std::weak_ptr<daedalus::INpc>();
    return true;
    }
  updateRagdollQuestAnchor();
  if(!mvAlgo.addSpellImpulse(impulseNs,physicsMass(),reaction))
    return false;
  spellImpulseCulprit = culprit!=nullptr ? culprit->handlePtr() : std::weak_ptr<daedalus::INpc>();
  bodyImpactLaunchCulprit=spellImpulseCulprit;
  bodyImpactSpellFlight=true;
  if(living)
    living->setNoiseCulprit(culprit);
  if(!isPlayer() && isHuman() && ragdollQuestReturn.hasAnchor &&
     Gothic::physicsFlags().questGuard) {
    if(!ragdollQuestReturn.armed)
      ragdollQuestReturn.hazards = 0;
    ragdollQuestReturn.armed = true;
    ragdollQuestSpellThrow = true;
    }
  return true;
  }

void Npc::suspendRagdoll() {
  // Removed/invalid NPCs can survive until world unload and still receive a
  // transform update at deadPoint. Keep that update out of shadow physics.
  meleeShadowSuspended = true;
  meleeFighter = {};
  clearMeleeShadow();
  pendingSpellImpulse = {};
  spellImpulseCulprit.reset();
  living.reset();
  arrowImpacts.clear();
  ragdollHasLastPosition = false;
  ragdollLastVelocity = {};
  const bool hadBody = ragdoll!=nullptr;
  if(hadBody)
    ragdollDormant = ragdoll->state();
  releaseRagdoll(false);
  if(hadBody)
    physic.setEnable(false);
  }

bool Npc::createRagdoll() {
  if(usesSimplifiedMovement())
    return false;
  const auto* skeleton = visual.visualSkeleton();
  if(skeleton==nullptr || !visual.hitZoneShape().isValid())
    return false;
  updateAnimation(0,true);
  Vec3 velocity;
  const auto now = owner.tickCount();
  if(ragdollHasLastPosition && now>=ragdollLastTick && now-ragdollLastTick<=250) {
    // A hit before this NPC's tick, or immediately after the world tick, leaves its root at
    // the last live sample. Carry that sample's velocity instead of treating it as a stop.
    velocity = ragdollLastVelocity;
    const auto delta = position()-ragdollLastPosition;
    if(now>ragdollLastTick && delta.length()>0.f)
      velocity = delta*(1000.f/float(now-ragdollLastTick));
    const float speed = velocity.length();
    if(!std::isfinite(speed))
      velocity = {}; else if(speed>3000.f)
      velocity *= 3000.f/speed;
    }
  auto body = owner.physic()->createRagdoll(*skeleton,visual.hitZoneShape(),visual.pose().transform(),
                                          visual.pose().boneCount(),visual.transform(),velocity);
  if(!body || !body->valid())
    return false;
  if(ragdollDormant && !body->restore(*ragdollDormant)) {
    ragdollDormant.reset();
    return false;
    }
  ragdollDormant.reset();
  ragdoll = std::move(body);
  ragdoll->setNpc(this);
  if(const auto culprit = spellImpulseCulprit.lock())
    ragdoll->setNoiseCulprit(static_cast<Npc*>(culprit->user_ptr));
  dispatchArrowPhysics(*ragdoll);
  if(Gothic::physicsFlags().spellPhysics && pendingSpellImpulse.quadLength()>0.f) {
    const auto culprit = spellImpulseCulprit.lock();
    ragdoll->addImpulse(pendingSpellImpulse,culprit ? reinterpret_cast<Npc*>(culprit->user_ptr) : nullptr);
    }
  pendingSpellImpulse = {};
  spellImpulseCulprit.reset();
  ragdollStartTick = owner.tickCount();
  physic.setEnable(false);
  mvAlgo.takeRagdollControl();
  syncRagdoll();
  if(const auto* trace=std::getenv("OPENGOTHIC_RAGDOLL_TRACE");trace!=nullptr && trace[0]=='1')
    Log::i("[ragdoll] created actor=",displayName()," hero=",isPlayer()," parts=",ragdoll->partCount()," dead=",isDead());
  return true;
  }

void Npc::releaseRagdoll(bool blend) {
  const bool hadBody = ragdoll!=nullptr;
  if(hadBody)
    if(const auto* trace=std::getenv("OPENGOTHIC_RAGDOLL_TRACE");trace!=nullptr && trace[0]=='1')
      Log::i("[ragdoll] released actor=",displayName()," hero=",isPlayer()," parts=",ragdoll->partCount()," dead=",isDead());
  visual.pose().clearRagdollPose(blend,owner.tickCount());
  ragdoll.reset();
  ragdollPose.clear();
  if(hadBody && hnpc)
    physic.setEnable(!isDead());
  }

bool Npc::recoverRagdoll(bool force) {
  if(!ragdoll)
    return true;
  if(isDead())
    return false;
  checkRagdollQuestHazard();
  bool returnToAnchor = needsRagdollQuestReturn();
  if(!force && !returnToAnchor && !ragdoll->isSleeping() && (owner.tickCount()-ragdollStartTick<500 || !ragdoll->settled()))
    return false;

  const bool onBack = ragdoll->onBack();
  const auto pelvis = ragdoll->pelvisTransform();
  Vec3 at(pelvis.at(3,0),pelvis.at(3,1),pelvis.at(3,2));
  const auto floor = owner.physic()->landRay(at+Vec3(0,80,0),380.f);
  if(floor.hasCol && floor.n.y>0.25f)
    at.y = floor.v.y;
  // A timed live get-up must not release the body into a ravine before the
  // usual ten-metre hazard has had time to arm the quest return.
  if(meleeKnockdown && !floor.hasCol && ragdollQuestReturn.armed &&
     ragdollQuestReturn.hasAnchor && Gothic::physicsFlags().questGuard && !isPlayer()) {
    ragdollQuestReturn.hazards |= RagdollSave::QuestReturn::fell;
    returnToAnchor = true;
    }

  if(returnToAnchor) {
    ragdollQuestFrom = position();
    at = ragdollQuestReturn.anchor;
    ++ragdollQuestReturns;
    }

  // Rebase the animated frame while the physical overlay still holds the shown pose.
  const auto physicalRoot = ragdoll->rootTransform();
  const float fx = physicalRoot.at(2,0), fz = physicalRoot.at(2,2);
  if(returnToAnchor)
    angle = ragdollQuestReturn.angle;
  else if(fx*fx+fz*fz>1e-6f)
    angle = 90.f-std::atan2(fx,fz)*180.f/float(M_PI);
  x = at.x; y = at.y; z = at.z;
  visual.setTextureOwner(at,!isPlayer());
  physic.setPosition(at);
  ragdollObjectMatrix.identity();
  ragdollObjectMatrix.translate(x,y,z);
  ragdollObjectMatrix.rotateOY(90-angle);
  ragdollObjectMatrix.scale(sz[0],sz[1],sz[2]);
  visual.setObjMatrix(ragdollObjectMatrix,false);
  visual.pose().stopAllAnim();
  const char* wounded = onBack ? "S_WOUNDEDB" : "S_WOUNDED";
  if(visual.startAnimAndGet(*this,wounded,calcAniComb(),meleeKnockdown ? BS_LIE : BS_UNCONSCIOUS)==nullptr) {
    // Creatures without wounded poses retain the standard idle fallback.
    visual.startAnimAndGet("S_RUN",owner.tickCount(),true);
    }
  // Do not interpolate a world-space corpse pose across the whole ravine.
  releaseRagdoll(!returnToAnchor);
  ragdollDormant.reset();
  if(!returnToAnchor)
    ragdollQuestReturn.armed = false;
  ragdollRecovering = true;
  ragdollHasLastPosition = false;
  ragdollLastVelocity = {};
  mvAlgo.clearSpeed();
  durtyTranform = TR_Pos|TR_Rot|TR_Scale;
  return true;
  }

void Npc::updateRagdollQuestAnchor() {
  if(usesSimplifiedMovement())
    return;
  // Training (OPENGOTHIC_FREEZE_WORLD): NPCs outside the arena stand still, so their anchors
  // keep the value sampled before the arena; resampling all of them costs two rays each.
  if(BodyArena::frozenWorld() && !BodyArena::isActor(*this))
    return;
  if(isPlayer() || !isHuman() || isDown() || ragdoll || interactive()!=nullptr ||
     ragdollQuestReturn.armed ||
     !Gothic::physicsFlags().questGuard ||
     (!Gothic::physicsFlags().ragdoll && !Gothic::physicsFlags().meleePosture) || mvAlgo.state()!=MoveAlgo::Run)
    return;
  const auto now = owner.tickCount();
  if(ragdollQuestReturn.hasAnchor && now>=ragdollQuestSampleTick && now-ragdollQuestSampleTick<250)
    return;
  ragdollQuestSampleTick = now;
  const auto at = position();
  const auto floor = owner.physic()->landRay(at+Vec3(0,15,0),45.f);
  if(!floor.hasCol || floor.n.y<0.7f || std::abs(floor.v.y-at.y)>30.f)
    return;
  const auto water = owner.physic()->waterRay(at,15.f);
  if(water.hasCol)
    return;
  ragdollQuestReturn.anchor = {at.x,floor.v.y,at.z};
  ragdollQuestReturn.angle = angle;
  ragdollQuestReturn.hasAnchor = true;
  }

bool Npc::needsRagdollQuestReturn() const {
  return ragdollQuestReturn.armed && ragdollQuestReturn.hasAnchor && ragdollQuestReturn.hazards!=0 &&
         !isPlayer() && isHuman() && !isDead() && Gothic::physicsFlags().questGuard;
  }

void Npc::checkRagdollQuestHazard() {
  if(!ragdollQuestReturn.armed || !Gothic::physicsFlags().questGuard)
    return;
  auto at = position();
  if(ragdoll) {
    const auto pelvis = ragdoll->pelvisTransform();
    at = Vec3(pelvis.at(3,0),pelvis.at(3,1),pelvis.at(3,2));
    }
  if(at.y<ragdollQuestReturn.anchor.y-RagdollSave::QuestReturn::maxFall)
    ragdollQuestReturn.hazards |= RagdollSave::QuestReturn::fell;
  if((ragdollQuestReturn.hazards & RagdollSave::QuestReturn::water)==0) {
    const auto water = owner.physic()->waterRay(at,60.f);
    if(water.hasCol) {
      // The padded water ray can start below a dry bank or bridge. A solid
      // support between the pelvis and that lower surface keeps the body dry.
      const auto support = water.wdepth<at.y ? owner.physic()->landRay(at,at.y-water.wdepth) : DynamicWorld::RayLandResult();
      if(!support.hasCol || support.v.y<=water.wdepth)
        ragdollQuestReturn.hazards |= RagdollSave::QuestReturn::water;
      }
    }
  }

bool Npc::recoverSpellThrow() {
  if(!ragdollQuestSpellThrow || ragdoll || isDown() || !ragdollQuestReturn.armed)
    return false;
  checkRagdollQuestHazard();
  const bool returnToAnchor = needsRagdollQuestReturn();
  ragdollQuestReturn.armed = false;
  ragdollQuestSpellThrow = false;
  if(!returnToAnchor)
    return false;
  ragdollQuestFrom = position();
  ++ragdollQuestReturns;
  angle = ragdollQuestReturn.angle;
  const auto at = ragdollQuestReturn.anchor;
  x = at.x; y = at.y; z = at.z;
  visual.setTextureOwner(at,!isPlayer());
  physic.setPosition(at);
  living.reset();
  arrowImpacts.clear();
  mvAlgo.takeRagdollControl();
  ragdollHasLastPosition = false;
  ragdollLastVelocity = {};
  setAnim(Anim::Idle);
  durtyTranform = TR_Pos|TR_Rot|TR_Scale;
  return true;
  }

void Npc::prepareRagdoll() {
  if(usesSimplifiedMovement())
    return;
  if(!isDown() || !Gothic::physicsFlags().spellPhysics)
    pendingSpellImpulse = {};
  if(!Gothic::physicsFlags().spellPhysics || (!isDown() && !isInAir()))
    spellImpulseCulprit.reset();
  if(!Gothic::physicsFlags().arrowSticking)
    visual.clearStuckArrows();
  if(!Gothic::physicsFlags().arrowPhysics)
    arrowImpacts.clear();
  if(!ragdollQuestSpellThrow && !ragdoll && !isDown() &&
     ragdollQuestReturn.armed && ragdollQuestReturn.hazards!=0) {
    if(!needsRagdollQuestReturn()) {
      ragdollQuestReturn.armed = false;
      }
    else if(visual.pose().isStanding()) {
      // Finish at the dry point too: get-up root motion must not carry a quest
      // NPC straight over the same edge again. 'armed' also survives a mid-rise save.
      const auto at = ragdollQuestReturn.anchor;
      x = at.x; y = at.y; z = at.z;
      visual.setTextureOwner(at,!isPlayer());
      physic.setPosition(at);
      mvAlgo.clearSpeed();
      durtyTranform |= TR_Pos;
      ragdollQuestReturn.armed = false;
      }
    }
  updateRagdollQuestAnchor();
  if(meleeKnockdown)
    return;
  const bool enabled = Gothic::physicsFlags().ragdoll;
  if(!enabled) {
    if(isDown())
      arrowImpacts.clear();
    if(ragdollHasLastPosition) {
      ragdollHasLastPosition = false;
      ragdollLastVelocity = {};
      }
    if(ragdoll) {
      ragdollDormant = ragdoll->state();
      // Legacy fall alignment expects the actor origin at the floor. Rebase while
      // the overlay still holds the shown world pose, keeping the dormant body fixed.
      const auto pelvis = ragdoll->pelvisTransform();
      const Vec3 at(x,pelvis.at(3,1),z);
      const auto floor = owner.physic()->landRay(at+Vec3(0,80,0),380.f);
      if(floor.hasCol && floor.n.y>0.25f)
        y = floor.v.y;
      ragdoll.reset();
      physic.setPosition(position());
      visual.setObjMatrix(mkPositionMatrix(),false);
      releaseRagdoll(true);
      physic.setEnable(!isDead());
      mvAlgo.clearSpeed();
      durtyTranform = TR_Pos|TR_Rot|TR_Scale;
      }
    if(!isDown()) {
      if(ragdollDormant)
        physic.setEnable(true);
      ragdollDormant.reset();
      ragdollLegacyDown = false;
      ragdollRecovering = false;
      }
    return;
    }
  if(!isDown()) {
    ragdollLegacyDown = false;
    ragdollDormant.reset();
    if(ragdoll)
      recoverRagdoll();
    else {
      ragdollRecovering = false;
      physic.setEnable(true);
      }
    const auto now = owner.tickCount();
    if(ragdollHasLastPosition && now>ragdollLastTick && now-ragdollLastTick<=250) {
      ragdollLastVelocity = (position()-ragdollLastPosition)*(1000.f/float(now-ragdollLastTick));
      const float speed = ragdollLastVelocity.length();
      if(!std::isfinite(speed))
        ragdollLastVelocity = {}; else if(speed>3000.f)
        ragdollLastVelocity *= 3000.f/speed;
      } else if(!ragdollHasLastPosition || now!=ragdollLastTick) {
      ragdollLastVelocity = {};
      }
    ragdollLastPosition = position();
    ragdollLastTick = now;
    ragdollHasLastPosition = true;
    return;
    }
  if(ragdoll || ragdollLegacyDown || (ragdollRecovering && !ragdollDormant)) {
    if(ragdoll)
      dispatchArrowPhysics(*ragdoll);
    else
      arrowImpacts.clear();
    return;
    }
  if(!createRagdoll()) {
    ragdollLegacyDown = true;
    arrowImpacts.clear();
    Log::e("ragdoll: unsupported skeleton or body state for ",displayName());
    }
  }

void Npc::prepareLivingBody() {
  if(usesSimplifiedMovement()) {
    living.reset();
    return;
    }
  const auto benchMode = meleeBenchMode();
  const bool benchRig = benchMode==MeleeBenchMode::Full || benchMode==MeleeBenchMode::Chain || benchMode==MeleeBenchMode::Blade;
  if(currentInteract!=nullptr)
    livingWasInteracting = true;
  else if(visual.pose().isStanding())
    livingWasInteracting = false;
  else if(visual.pose().hasState(BS_MOBINTERACT))
    livingWasInteracting = true;
  else if(!visual.pose().hasState(BS_CLIMB))
    livingWasInteracting = false;
  // Mob clips align their bones independently of the walking root. Their short
  // pose changes are real animation, but are not contact velocities. Detach can
  // clear currentInteract before ToStand finishes; retain the suspension until
  // that clip ends or ordinary movement interrupts it. Idle successors can keep
  // the interaction body-state tag, so recognize their standing pose explicitly.
  const bool interacting = currentInteract!=nullptr || livingWasInteracting;
  const bool frozen = spellFrozen();
  if(frozen!=livingWasFrozen) {
    // Enter with a completely kinematic rig, discarding any active hit reaction.
    // On thaw the ordinary living-body policy decides whether this rig is still needed.
    living.reset();
    livingRetryTick = 0;
    livingNeedsRebuild = false;
    livingWasFrozen = frozen;
    arrowImpacts.clear();
    }
  const auto* hero = owner.player();
  // Fighters and population actors retain the same rig as actors near the hero.
  const bool keep = MeleeFighter::keepPhysics(Gothic::physicsFlags().livingBodies,!isPlayer() && hero!=nullptr,
                                             isMeleeFighter() || NavDemos::fullSimulation(*this),bool(living),
                                             hero!=nullptr ? qDistTo(*hero) : 0.f);
  if(benchMode==MeleeBenchMode::Base || isDown() || ragdoll || interacting || (!benchRig && !frozen && !keep)) {
    living.reset();
    livingRetryTick = 0;
    livingNeedsRebuild = false;
    arrowImpacts.clear();
    return;
    }
  if(frozen)
    arrowImpacts.clear();
  if(livingNeedsRebuild || (!benchRig && living && !living->isKinematic() &&
                            !Gothic::physicsFlags().arrowPhysics && !Gothic::physicsFlags().meleeReactions)) {
    living.reset();
    livingNeedsRebuild = false;
    }
  if(living) {
    if(!frozen)
      dispatchArrowPhysics(*living);
    return;
    }
  if(owner.tickCount()<livingRetryTick) {
    arrowImpacts.clear();
    return;
    }
  const auto* skeleton = visual.visualSkeleton();
  if(skeleton==nullptr || !visual.hitZoneShape().isValid()) {
    if(benchRig) throw std::runtime_error("Melee benchmark requires a skeleton with fitted HitZones");
    arrowImpacts.clear();
    return;
    }
  // Only birth needs a forced solve. Subsequent targets come from the normal
  // animation update, including IK/aim/flinch, without solving the pose twice.
  updateAnimation(0,true);
  living = owner.physic()->createKinematicBody(*skeleton,visual.hitZoneShape(),visual.pose().transform(),
                                             visual.pose().boneCount(),visual.transform());
  if(benchRig && !living)
    throw std::runtime_error("melee benchmark requires a valid 24-part living body");
  if(living) {
    living->setNpc(this);
    if(benchMode==MeleeBenchMode::Full || benchMode==MeleeBenchMode::Chain)
      if(!living->configureBench(benchMode==MeleeBenchMode::Full ? Ragdoll::BenchMode::Full : Ragdoll::BenchMode::Chain,
                                meleeBenchDynamicPelvis ? Ragdoll::BenchPelvis::Dynamic : Ragdoll::BenchPelvis::Kinematic))
        throw std::runtime_error("melee benchmark motor configuration failed");
    if(const auto culprit = spellImpulseCulprit.lock())
      living->setNoiseCulprit(static_cast<Npc*>(culprit->user_ptr));
    }
  else
    livingRetryTick = owner.tickCount()+1000;
  if(living && !frozen)
    dispatchArrowPhysics(*living);
  else
    arrowImpacts.clear();
  }

bool Npc::stickArrow(MeshObjects::Mesh&& arrow, size_t bone, const Matrix4x4& worldMatrix,
                     std::string_view visualName) {
  // A corpse hit is selected after the native step, before the usual pose export.
  // Bind against that same current pose so the first sync cannot move the pin twice.
  if(ragdoll)
    syncRagdoll();
  if(bone>=visual.pose().boneCount()) {
    const auto& transform = visual.transform();
    const float scale = Vec3(transform.at(0,0),transform.at(0,1),transform.at(0,2)).length();
    const Vec3 at(worldMatrix.at(3,0),worldMatrix.at(3,1),worldMatrix.at(3,2));
    bone = visual.hitZoneShape().closestBone(visual.pose().transform(),visual.pose().boneCount(),scale,at);
    }
  return visual.stickArrow(std::move(arrow),bone,worldMatrix,visualName);
  }

void Npc::addCorpseArrowImpulse(const Bullet& bullet) {
  if(ragdoll && isDown())
    queueArrowPhysics(bullet);
  }

void Npc::queueArrowPhysics(const Bullet& bullet) {
  syncDeferredAnimation();
  if(bullet.isSpell() || !Gothic::physicsFlags().arrowPhysics)
    return;
  const auto direction = bullet.direction(); // cm/ms, as used by the projectile integrator
  const float speed = direction.length();
  const auto point = bullet.impactPoint();
  if(!std::isfinite(speed) || speed<1.e-3f ||
     !std::isfinite(point.x) || !std::isfinite(point.y) || !std::isfinite(point.z))
    return;
  size_t bone = bullet.impactBone();
  if(bone>=visual.pose().boneCount()) {
    const auto& transform = visual.transform();
    const float scale = Vec3(transform.at(0,0),transform.at(0,1),transform.at(0,2)).length();
    bone = visual.hitZoneShape().closestBone(visual.pose().transform(),visual.pose().boneCount(),scale,point);
    }
  if(bone>=visual.pose().boneCount())
    return;
  // An 80 g projectile: cm/ms -> m/s gives momentum in N*s. Draw and weapon
  // strength already determine the actual projectile speed; damage dice do not.
  const float momentum = std::clamp(0.08f*speed*10.f,0.2f,12.f);
  const float strength = std::clamp(momentum/8.f,0.1f,1.f);
  const Vec3 impulse = direction*(momentum/speed);
  arrowImpacts.push_back({bone,point,impulse,strength,
                           bullet.origin()!=nullptr ? bullet.origin()->handlePtr() : std::weak_ptr<daedalus::INpc>()});
  arrowPhysics.bone = bone;
  arrowPhysics.point = point;
  arrowPhysics.impulseNs = impulse;
  arrowPhysics.strength = strength;
  ++arrowPhysics.hits;
  }

void Npc::dispatchArrowPhysics(Ragdoll& body) {
  for(const auto& hit:arrowImpacts) {
    const auto culprit = hit.culprit.lock();
    if(body.queueHit(hit.bone,hit.point,hit.impulseNs,hit.strength,culprit ? reinterpret_cast<Npc*>(culprit->user_ptr) : nullptr,
                     hit.stiffness)) {
      auto& probe = hit.melee ? meleeReaction : arrowPhysics;
      ++probe.applied;
      if(&body==ragdoll.get())
        ++probe.corpseImpulses;
      }
    }
  arrowImpacts.clear();
  }

void Npc::syncRagdoll() {
  const bool hasPose=[&] { FrameProfile::Scope timing(FrameProfile::Zone::NativeExport,FrameProfile::watchedActor(this));
    return ragdoll && ragdoll->getPose(ragdollPose); }();
  if(!hasPose)
    return;
  ragdollObjectMatrix = ragdoll->rootTransform();
  x = ragdollObjectMatrix.at(3,0);
  y = ragdollObjectMatrix.at(3,1);
  z = ragdollObjectMatrix.at(3,2);
  visual.setTextureOwner(position(),!isPlayer());
  checkRagdollQuestHazard();
  physic.setPosition(position());
  visual.pose().setRagdollPose(visual.visualSkeleton(),ragdollPose,ragdoll->poseIndices());
  durtyTranform = TR_Pos|TR_Rot|TR_Scale;
  { FrameProfile::Scope timing(FrameProfile::Zone::ForcedSyncAnimation,FrameProfile::watchedActor(this)); updateAnimation(0,true); }
  }

Npc* CollisionWorld::CollisionBody::noiseCulpritNpc(uint64_t step) const {
  const auto culprit=step<=noiseCulpritUntilStep ? noiseCulprit.lock() : nullptr;
  return culprit!=nullptr ? static_cast<Npc*>(culprit->user_ptr) : nullptr;
  }
