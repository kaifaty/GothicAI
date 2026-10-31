#include "movealgo.h"

#include <Tempest/Log>

#include "world/objects/npc.h"
#include "world/objects/interactive.h"
#include "world/objects/item.h"
#include "world/world.h"
#include "gothic.h"
#include "serialize.h"
#include "testbench.h"
#include "navdemos.h"

#include <cstdio>
#include <algorithm>
#include <cmath>
#include <vector>
#include <limits>

// Read-only measurement around the existing movement step. Collision fields describe only calls
// through MoveAlgo::tryMove: Npc::tryMove exposes success, not DynamicWorld's internal MoveCode.
// No additional collision/ground queries, animation advances or random draws are made by the trace.
struct MoveAlgo::MovementTrace final {
  MovementTrace(MoveAlgo& owner, uint64_t dt, MvFlags moveFlags)
    :owner(owner),previous(owner.movementTrace),dt(dt),moveFlags(moveFlags) {
    active = TestBench::bodyTracing() && (owner.npc.bodyTraceMovementActive || owner.npc.target()!=nullptr);
    if(!active)
      return;
    start = owner.npc.position();
    root = owner.animMoveSpeed(dt); // const pose displacement, rotated into world coordinates
    owner.movementTrace = this;
    }

  ~MovementTrace() {
    if(!active)
      return;
    owner.movementTrace = previous;
    const auto end = owner.npc.position();
    const auto moved = end-start;
    char fields[1800];
    std::snprintf(fields,sizeof(fields),
                  "\"sample_phase\":\"npc_post_movement\",\"movealgo_tick\":true,\"skip_reason\":\"\","
                  "\"dt_ms\":%llu,\"move_flags\":%u,"
                  "\"root_dx\":%.6f,\"root_dy\":%.6f,\"root_dz\":%.6f,"
                  "\"requested_dx\":%.6f,\"requested_dy\":%.6f,\"requested_dz\":%.6f,"
                  "\"moved_dx\":%.6f,\"moved_dy\":%.6f,\"moved_dz\":%.6f,"
                  "\"x\":%.6f,\"y\":%.6f,\"z\":%.6f,\"yaw\":%.6f,"
                  "\"grounded\":%s,\"movement_state\":%u,\"collision_scope\":\"movealgo_trymove\","
                  "\"collision_attempts\":%u,\"collision_failed_count\":%u,"
                  "\"collision_land\":%s,\"collision_npc\":%s,\"collision_vob\":%s,\"collision_prefall\":%s",
                  (unsigned long long)dt,unsigned(moveFlags),double(root.x),double(root.y),double(root.z),
                  double(requested.x),double(requested.y),double(requested.z),
                  double(moved.x),double(moved.y),double(moved.z),double(end.x),double(end.y),double(end.z),
                  double(owner.npc.rotation()),owner.flags==Run ? "true" : "false",unsigned(owner.flags),
                  attempts,failures,land ? "true" : "false",otherNpc ? "true" : "false",vob ? "true" : "false",
                  preFall ? "true" : "false");
    TestBench::bodyEvent(owner.npc,owner.npc.target(),"physics_step",owner.npc.world().tickCount(),fields);
    }

  void collision(const Tempest::Vec3& displacement, bool accepted, const DynamicWorld::CollisionTest& info) {
    if(attempts==0)
      requested = displacement;
    ++attempts;
    failures += accepted ? 0u : 1u;
    land |= info.landCol;
    otherNpc |= info.npc!=nullptr;
    vob |= info.vob!=nullptr;
    preFall |= info.preFall;
    }

  MoveAlgo& owner;
  MovementTrace* previous = nullptr;
  uint64_t dt = 0;
  MvFlags moveFlags = NoFlag;
  bool active = false;
  Tempest::Vec3 start = {}, root = {}, requested = {};
  unsigned attempts = 0, failures = 0;
  bool land = false, otherNpc = false, vob = false, preFall = false;
  };

const float   MoveAlgo::closeToPointThreshold = 40;
const float   MoveAlgo::climbMove             = 55;
const float   MoveAlgo::gravity               = DynamicWorld::gravity;
const float   MoveAlgo::eps                   = 2.f;   // 2-santimeters
const int32_t MoveAlgo::flyOverWaterHint      = 999999;
const float   MoveAlgo::waterPadd             = 15;

MoveAlgo::MoveAlgo(Npc& unit)
  :npc(unit) {
  }

void MoveAlgo::load(Serialize &fin) {
  resetCharacterVirtual();
  npcVirtualInRange = false;
  npcVirtualMobTransition = false;
  fin.read(reinterpret_cast<uint32_t&>(flags));
  fin.read(mulSpeed,fallSpeed,fallCount,climbStart,climbPos0,climbHeight);
  fin.read(reinterpret_cast<uint8_t&>(jmp));

  std::string str;
  fin.read(str);
  portal       = npc.world().physic()->validateSectorName(str);
  fin.read(str);
  formerPortal = npc.world().physic()->validateSectorName(str);

  fin.read(diveStart);
  }

void MoveAlgo::save(Serialize &fout) const {
  fout.write(uint32_t(flags));
  fout.write(mulSpeed,fallSpeed,fallCount,climbStart,climbPos0,climbHeight);
  fout.write(uint8_t(jmp));

  fout.write(portal,formerPortal);
  fout.write(diveStart);
  }

void MoveAlgo::tickMobsi(uint64_t dt) {
  if(npc.interactive()->isStaticState())
    return;

  auto dp = npc.animMoveSpeed(dt);
  if(npc.interactive()->isLadder()) {
    auto mat = npc.interactive()->transform();
    mat.project(dp);
    dp -= npc.interactive()->position();
    } else {
    Tempest::Vec3 ret;
    applyRotation(ret,dp);
    dp = Tempest::Vec3(ret.x, 0, ret.z);
    }

  if(npc.interactive()->isTrueDoor(npc)) {
    // some tight-door require collision-detection
    npc.tryMove(dp);
    } else {
    // but chair/bed do not :)
    auto pos = npc.position();
    npc.setPosition(pos+dp);
    }
  setState(Run);
  }

bool MoveAlgo::tryMove(float x, float y, float z) {
  DynamicWorld::CollisionTest out;
  return tryMove(x,y,z,out);
  }

bool MoveAlgo::tryMove(float x,float y,float z, DynamicWorld::CollisionTest& out) {
  return tryMove(Tempest::Vec3{x,y,z},out);
  }

bool MoveAlgo::tryMove(const Tempest::Vec3& dp, DynamicWorld::CollisionTest& out) {
  const bool accepted = npc.tryMove(dp,out);
  if(movementTrace!=nullptr)
    movementTrace->collision(dp,accepted,out);
  return accepted;
  }

void MoveAlgo::tickJumpup(uint64_t dt) {
  auto pos   = npc.position();
  auto climb = npc.tryJump();

  if(climb.anim==Npc::Anim::JumpHang)
    climbHeight = pos.y;

  if(pos.y<climbHeight) {
    pos.y       += fallSpeed.y*float(dt);
    fallSpeed.y -= gravity*float(dt);

    pos.y = std::min(pos.y,climbHeight);
    if(!npc.tryTranslate(pos))
      climbHeight = pos.y;
    return;
    }

  Tempest::Vec3 p={}, v={0,0,climbMove};
  applyRotation(p,v);
  p = climbPos0;
  p.y = climbHeight;
  npc.tryTranslate(p);

  if(climb.anim==Npc::Anim::JumpHang) {
    startClimb(climb);
    } else {
    setState(InAir);
    clearSpeed();
    }
  }

void MoveAlgo::tickClimb(uint64_t dt) {
  if(npc.bodyStateMasked()!=BS_CLIMB) {
    //NOTE: climb allows npc to violate collision detection, need to readjust
    npc.owner.script().fixNpcPosition(npc, 0, 0);
    setState(Run);

    Tempest::Vec3 p={}, v={0,0,climbMove};
    applyRotation(p,v);
    p += climbPos0;
    p.y = climbHeight;
    if(!npc.tryTranslate(p)) {
      // npc.setPosition(Tempest::Vec3(climbPos0.x,climbHeight,climbPos0.z));
      npc.tryTranslate(Tempest::Vec3(climbPos0.x,climbHeight,climbPos0.z));
      npc.tryTranslate(p);
      }
    return;
    }

  auto dp  = animMoveSpeed(dt);
  auto pos = npc.position();

  if(pos.y<climbHeight) {
    pos.y += dp.y;
    pos.y = std::min(pos.y,climbHeight);
    // npc.tryTranslate(pos);
    npc.setPosition(pos);
    }

  pos.x += dp.x;
  pos.z += dp.z;
  // npc.tryTranslate(pos);
  npc.setPosition(pos);
  }

void MoveAlgo::tick(uint64_t dt, MvFlags moveFlg) {
  ++movementTraceSerial;
  MovementTrace trace(*this,dt,moveFlg);
  if(npc.isDown() && (flags==Swim || flags==Dive)) {
    // 'falling' to bottom of the lake
    setState(InAir);
    }

  if(npc.interactive()!=nullptr) {
    resetCharacterVirtual(true);
    tickMobsi(dt);
    return;
    }

  if(npcVirtualMobTransition) {
    const auto& pose = npc.visual.pose();
    // Detach can precede ToStand's last frame. Its root remains animation-owned,
    // just like the seated/bed/kettle clip, until normal standing or movement resumes.
    if(pose.isStanding() || (!pose.hasState(BS_MOBINTERACT) && !pose.hasState(BS_CLIMB)))
      npcVirtualMobTransition = false;
    }

  if(!implTick(dt,moveFlg))
    return;

  if(cache.sector!=nullptr && portal!=cache.sector) {
    formerPortal = portal;
    portal       = cache.sector;
    if(npc.isPlayer() && npc.bodyStateMasked()!=BS_SNEAK) {
      auto& w = npc.world();
      w.sendImmediatePerc(npc,npc,npc,PERC_ASSESSENTERROOM);
      }
    }
  }

bool MoveAlgo::implTick(uint64_t dt, MvFlags moveFlg) {
  const auto bs = npc.bodyStateMasked();
  const bool liftOff = bs==BS_JUMP && npc.isFlyAnim();
  const bool virtualEnabled = useCharacterVirtual();
  const bool virtualWalking = virtualEnabled && !npcVirtualMobTransition &&
                              !npc.isDown() && npc.physic.isEnabled() && !canFlyOverWater() &&
                              (flags==Run || flags==Slide || flags==InWater) && !liftOff;
  if(!virtualWalking) {
    if(virtualMovementActive && flags==Run && liftOff)
      virtualJumpVelocity = virtualGroundVelocity*0.001f;
    npc.physic.resetVirtual();
    virtualMovementActive = false;
    virtualGroundVelocity = {};
    }

  if(flags==ClimbUp) {
    tickClimb(dt);
    return true;
    }
  if(flags==JumpUp) {
    tickJumpup(dt);
    return true;
    }

  const auto state = flags;
  const auto impulseGeneration=movementImpulseGeneration;
  const bool dead  = npc.isDead();
  const bool swim  = (state==Swim);
  const bool dive  = (state==Dive);
  const bool grav  = (state==InAir || state==Falling || state==JumpUp);
  const auto pos0  = npc.position();
  const auto assist = virtualWalking ? npc.meleeStep(dt) : Tempest::Vec3{};
  auto       dp    = (!grav && state!=Slide) ? npcMoveSpeed(dt,moveFlg) : npcFallSpeed(dt);
  const bool walk  = bool(npc.wlkMode & WalkBit::WM_Walk) && (state==Run);

  const auto guardEdgeMove = [&] {
    const auto weapon = npc.weaponState();
    if(!npc.isPlayer() || npc.isDown() || (state!=Run && state!=Slide) || bs==BS_JUMP ||
       (weapon!=WeaponState::Bow && weapon!=WeaponState::CBow))
      return;
    // Apply the main branch's bow edge guard to the displacement accepted by
    // either controller, including a partial move and a sliding step.
    const auto  pos = npc.position();
    const float k   = edgeMoveFraction(pos0,pos);
    if(k<1.f) {
      npc.setPosition(pos0+(pos-pos0)*k);
      if(state==Slide)
        clearSpeed();
      // Keep the aim animation: onMoveFailed would switch it to Idle.
      }
    };

  if(virtualWalking) {
    // Monster attacks can put a deliberate vertical displacement in the root
    // (the wolf's T_FISTATTACKMOVE). Keep it, unlike ordinary walking bob.
    const bool attackRoot = !npc.isPlayer() && !npc.isHuman() && bs==BS_HIT;
    if(state!=Slide && !attackRoot)
      dp.y = 0.f; // walking root bob cannot detach the controller from its support
    const auto root = dp;
    dp += assist;
    const auto assistOrigin = npc.position();
    const bool moved = tickCharacterVirtual(dt,dp);
    npc.noteMeleeStep(root,assist,npc.position()-assistOrigin,dt);
    if(impulseGeneration!=movementImpulseGeneration)
      return moved;
    guardEdgeMove();
    return moved;
    }

  // Jump animation still owns its launch. Carry the support's velocity through its root motion,
  // then let the existing airborne integrator inherit the accumulated displacement.
  if(state==Jump || (state==Run && liftOff))
    dp += virtualJumpVelocity*float(dt);

  DynamicWorld::CollisionTest info;
  const bool moved=tryMove(dp,info);
  if(preserveMovementImpulse(impulseGeneration,state,"legacy"))
    return true;
  if(!moved) {
    info.preFall = false;
    if(state==Slide) {
      onGravityFailed(info,dt);
      setState(InAir);
      }
    else if(grav) {
      onGravityFailed(info,dt);
      }
    else if(swim) {
      // Khorinis port hack
      /*
      for(int i=0; i<=50; i+=10) {
        if(tryMove(Tempest::Vec3(dp.x,dp.y+float(i),dp.z), info))
          break;
        if(i==50) {
          onMoveFailed(dp,info,dt);
          return false;
          }
        }
      */
      onMoveFailed(dp,info,dt);
      return false;
      }
    else {
      onMoveFailed(dp,info,dt);
      return false;
      }
    }

  guardEdgeMove();

  // CV walking can reach a different takeoff pose beside world geometry.
  // Keep its floor guard through the legacy jump/fall handoff as well: reject
  // lateral travel into a bottomless region, while lower landing floors and
  // ordinary vertical flight retain the existing integrator.
  if(virtualEnabled && !npcVirtualMobTransition && !npc.isDown() && npc.physic.isEnabled() &&
     !swim && !dive && !canFlyOverWater()) {
    const auto pos = npc.position();
    if((pos.x!=pos0.x || pos.z!=pos0.z) &&
       !npc.physic.hasFloorBelow({pos.x,std::max(pos.y,pos0.y),pos.z})) {
      npc.setPosition(pos0);
      DynamicWorld::CollisionTest vertical;
      const bool movedVertical=tryMove({0,pos.y-pos0.y,0},vertical);
      if(preserveMovementImpulse(impulseGeneration,state,"legacy"))
        return true;
      if(!movedVertical && grav)
        onGravityFailed(vertical,dt);
      fallSpeed.x = fallSpeed.z = 0.f;
      virtualJumpVelocity.x = virtualJumpVelocity.z = 0.f;
      // Do not return before vertical gravity/landing below: an inertial jump
      // must descend at the supported X/Z instead of freezing at the boundary.
      }
    }

  if(grav) {
    fallSpeed.y -= gravity*float(dt);
    }

  //if(dp==Tempest::Vec3() && !grav && state!=Jump && state!=Slide)
  //  return false;

  auto        pos            = npc.position();
  const float stickThreshold = grav ? 0.f : stepHeight();
  const float chest          = waterDepthChest();
  const float knee           = waterDepthKnee();

  bool  gValid  = false;
  auto  ground  = dropRay  (pos, gValid);
  auto  normal  = normalRay(pos);
  auto  water   = waterRay (pos);
  float dY      = pos.y-ground;

  if(canFlyOverWater() && ground<water) {
    dY      = pos.y-water;
    // onGound = false;
    //NOTE: should disable slide
    }
  else if(std::isfinite(water) && ground+chest<water && npc.hasSwimAnimations() && !dead) {
    dY      = std::min((pos.y+chest)-water, stickThreshold);
    }

  if(dp==Tempest::Vec3() && pos.y==ground && state!=Jump && state!=Slide)
    return false;

  // jump animation (lift off)
  if(bs==BS_JUMP && npc.isFlyAnim() && state!=InAir && state!=Jump && state!=JumpUp) {
    setState(Jump);
    return true;
    }

  // jump animation
  if(state==Jump) {
    if(gValid && dY<0) {
      npc.tryMove(Tempest::Vec3(0,-dY,0));
      }

    if(npc.isJumpAnim()) {
      fallSpeed += dp;
      fallCount += float(dt);
      }
    else if(!dead) {
      setState(InAir);
      }
    return true;
    }

  if(state==JumpUp) {
    const auto climb = npc.tryJump();
    if(climb.anim==Npc::Anim::JumpHang) {
      startClimb(climb);
      return true;
      }
    }

  // blood-fly over water
  if(canFlyOverWater() && !dead && ground<water) {
    setState(Run);
    npc.tryTranslate(Tempest::Vec3(pos.x, water, pos.z));
    return true;
    }

  npc.checkRagdollQuestHazard();

  // water interaction
  if(!canFlyOverWater() && !dead && ground<water) {
    if(swim) {
      // note need to be carefull about non-planar water patches, near waterfalls
      const float wpos = std::max(water-chest, ground);
      npc.tryTranslate(Tempest::Vec3(pos.x, wpos, pos.z));
      }

    const float gpos = std::max(npc.position().y, ground);
    if(gpos + 3.f*chest <= water) {
      // underwater walk bug-like case: can switch to dive here
      // setState(Dive);
      }
    else if(gpos + chest <= water+0.01f && npc.hasSwimAnimations()) {
      if(npc.recoverSpellThrow())
        return true;
      if(state!=Swim && state!=Dive) {
        const bool splash = grav || fallSpeed.quadLength() >= 1.f;
        setState(Swim);
        if(splash)
          emitWaterSplash(water);
        // if(!npc.hasSwimAnimations())
        //   npc.takeDrownDamage();
        return true;
        }
      }
    else if(gpos + knee <= water && state!=InAir && state!=Slide && state!=Dive) {
      // swimming toward cliff-slide
      if(swim && testSlide(pos,normal,info)) {
        npc.setPosition(pos0);
        onMoveFailed(dp,info,dt);
        return false;
        }
      if(state==Swim) {
        setState(Run);
        }
      }
    else if(state==InWater || state==Swim) {
      setState(Run);
      }
    }

  if(swim) {
    if(dead || !std::isfinite(water)) {
      setState(Run);
      }
    return true;
    }

  if(dive) {
    if(pos.y+chest > water && std::isfinite(water)) {
      npc.tryTranslate(Tempest::Vec3(pos.x, water-chest, pos.z));
      if(npc.world().tickCount()-diveStart>2000) {
        setState(Swim);
        }
      }
    return true;
    }

  // A bottomless fall cannot reach the normal landing callback.
  if(!gValid && npc.needsRagdollQuestReturn() && npc.recoverSpellThrow())
    return true;

  // above ground/void
  if(!gValid || (pos.y>ground && dY >= stickThreshold && state!=InWater)) {
    if(!gValid && swim) {
      // sea monster condition?
      }

    const bool lowHeight = (dY < falldownHeight()*0.75f);
    if((walk || swim) && !(lowHeight && !npc.isPlayer())) {
      npc.setPosition(pos0);

      info.normal  = dp;
      info.preFall = true;
      onMoveFailed(dp,info,dt);
      return false;
      }
    if(!swim && !dive && !dead) {
      // fall animations
      const float h0 = falldownHeight();

      float fallTime = fallSpeed.y/gravity;
      float height   = 0.5f*std::abs(gravity)*fallTime*fallTime;

      if(height>h0) {
        npc.setAnim(AnimationSolver::FallDeep);
        npc.setAnimRotate(0);
        setState(Falling);
        }
      else if(fallSpeed.y<-0.3f && bs!=BS_JUMP && bs!=BS_FALL) {
        npc.setAnim(AnimationSolver::Fall);
        npc.setAnimRotate(0);
        setState(InAir);
        }
      else if(state==InWater) {
        npc.setAnimRotate(0);
        setState(Swim);
        }
      else {
        npc.setAnimRotate(0);
        setState(InAir);
        }
      }
    return true;
    }

  if(state==Slide && !npc.isDown()) {
    const auto norm = normalRay(pos);
    if(!testSlide(pos,norm,info)) {
      setState(Run);
      return true;
      }
    const auto tangent = Tempest::Vec3::crossProduct(norm, Tempest::Vec3(0,1,0));
    const auto slide   = Tempest::Vec3::crossProduct(norm, tangent);

    fallSpeed += slide*float(dt)*gravity;
    //fallCount  = 1;
    if(gValid && std::abs(dY)<stickThreshold) {
      // step up
      npc.tryMove(Tempest::Vec3(0,-dY,0));
      }

    npc.setAnimRotate(0);
    if(slideDir())
      npc.setAnim(AnimationSolver::SlideA); else
      npc.setAnim(AnimationSolver::SlideB);
    return true;
    }

  // no longer in air - ground code
  if(state==InAir || state==Falling) {
    // A protected return records the landing source before leaving this tick.
    // Attach its root to the detected floor first, as the normal path does below;
    // the fall step can otherwise leave that source below ground for one tick.
    if(npc.needsRagdollQuestReturn()) {
      npc.tryMove(Tempest::Vec3(0,-dY,0));
      if(preserveMovementImpulse(impulseGeneration,state,"legacy"))
        return true;
      }
    // Resolve the protected throw before landing damage can lose the NPC.
    if(npc.recoverSpellThrow())
      return true;
    // attach to ground
    npc.takeFallDamage(fallSpeed);
    setState(Run);
    }

  if(ground+chest < water && !npc.hasSwimAnimations()) {
    // no swim animations
    npc.setPosition(pos0);
    DynamicWorld::CollisionTest info;
    info.preFall = true;
    onMoveFailed(dp,info,dt);
    return false;
    }

  if(!dead && testSlide(pos,normal,info)) {
    if(state==InWater || state==Swim) {
      npc.setPosition(pos0);
      return false;
      }

    if(state!=InAir && Tempest::Vec3::dotProduct(normal,dp)<0.45f) {
      // same as wall
      npc.setPosition(pos0);
      info.preFall = false;
      onMoveFailed(dp,info,dt);
      return false;
      }
    if(state==InAir && ground>=pos.y) {
      // rough landing
      npc.tryMove(Tempest::Vec3(0,-dY,0));
      if(preserveMovementImpulse(impulseGeneration,state,"legacy"))
        return true;
      //npc.setAnim(AnimationSolver::Idle);
      npc.setAnim(AnimationSolver::SlideA);
      }
    if(walk) {
      npc.setPosition(pos0);

      info.normal  = dp;
      info.preFall = true;
      onMoveFailed(dp,info,dt);
      return false;
      }
    setState(Slide);
    return true;
    }

  if(gValid && dY <= stickThreshold && fallSpeed.y<=0.f) {
    const float gpos = std::max(npc.position().y, ground);
    if(gpos + knee <= water) {
      setState(InWater);
      } else {
      setState(Run);
      }
    if(ground==pos.y)
      return true;
    if(ground<=pos.y) {
      // step up
      // npc.setPosition(adjPos);
      npc.tryMove(Tempest::Vec3(0,-dY,0));
      return true;
      }
    if(ground>=pos.y) {
      // inside ground
      // npc.setPosition(adjPos);
      npc.tryMove(Tempest::Vec3(0,-dY,0));
      return true;
      }
    }

  // something went wrong - back to origin then
  // npc.setPosition(pos0);
  return true;
  }

bool MoveAlgo::useCharacterVirtual() {
  if(npc.isPlayer())
    return Gothic::physicsFlags().characterVirtual;
  if(NavDemos::forceCharacterVirtual(npc) && npc.isHuman() && !npc.isDown())
    return true;
  const auto* hero = npc.world().player();
  const bool human = npc.isHuman();
  const bool enabled = human ? Gothic::physicsFlags().npcCharacterVirtual :
                               Gothic::physicsFlags().monsterCharacterVirtual &&
                               Gothic::physicsFlags().allowsMonsterCharacterVirtual(npc.isDown(),canFlyOverWater());
  if(!enabled || npc.isDown() || hero==nullptr) {
    npcVirtualInRange = false;
    return false;
    }
  constexpr float enterRadius = 2000.f, leaveRadius = 2200.f;
  const float distance = npc.qDistTo(*hero);
  npcVirtualInRange = npcVirtualInRange ? distance<=leaveRadius*leaveRadius
                                       : distance<enterRadius*enterRadius;
  return npcVirtualInRange;
  }

bool MoveAlgo::tickCharacterVirtual(uint64_t dt, const Tempest::Vec3& displacement) {
  const auto origin = npc.position();
  auto& world = npc.world();
  std::vector<Item*> nearbyItems;
  world.detectItem(origin,DynamicWorld::characterItemRange,[&](Item& item) {
    if(!item.isDynamic())
      nearbyItems.push_back(&item);
    });
  // Creating a native body invalidates the item query index. Finish detection first,
  // and preserve existing bodies with their current mass, sleep state and velocity.
  for(auto* item:nearbyItems)
    item->setPhysicsEnable(world);
  const float maxSlope = std::acos(std::clamp(slideAngle(),0.f,1.f));
  const auto impulseGeneration=movementImpulseGeneration;
  const auto previousState=flags;
  const bool limitStairAdvance = !npc.isPlayer() && !npc.isHuman();
  const auto moved = npc.physic.moveVirtual(displacement,dt,stepHeight(),maxSlope,npc.physicsMass(),&npc,
                                           limitStairAdvance,limitStairAdvance);
  if(preserveMovementImpulse(impulseGeneration,previousState,"virtual"))
    return true;
  npc.setViewPosition(moved.position);
  virtualMovementActive = true;
  virtualGroundVelocity = moved.groundVelocity;

  auto collision = moved.collision;
  const bool faceTravel = limitStairAdvance && moved.steepContact && isForward(displacement) &&
                          !npc.isAttackAnim() && npc.bodyStateMasked()!=BS_HIT;
  const auto faceSteepTravel = [&] {
    if(!faceTravel || collision.npc!=nullptr || collision.preFall || dt==0)
      return;
    const auto travel = moved.position-origin;
    const float distance = std::hypot(travel.x,travel.z);
    if(distance<=.1f*float(dt))
      return; // Ignore slow contact recovery below 1 m/s.
    // Even a failed move may have slid sideways in Jolt. Face the accepted
    // travel so the next animation step no longer keeps driving into the bank.
    npc.setDirection(Tempest::Vec3(travel.x,0,travel.z));
    };
  if(!npc.isPlayer() && moved.blocked) {
    DynamicWorld::CollisionTest legacy;
    // Enrich only the AI notification, after Jolt has accepted the final origin.
    // In particular, doors need their Interactive pointer for PERC_MOVEMOB.
    // testMove restores the ellipsoid and leaves the native controller untouched.
    npc.physic.testMove(origin+displacement,origin,legacy);
    if(legacy.vob!=nullptr || legacy.npc!=nullptr || legacy.landCol) {
      if(legacy.normal!=Tempest::Vec3() && !(limitStairAdvance && moved.steepContact))
        collision.normal = legacy.normal;
      collision.vob = legacy.vob;
      if(collision.npc==nullptr) {
        collision.npc = legacy.npc;
        collision.npcCol = legacy.npcCol;
        }
      collision.landCol |= legacy.landCol;
      collision.preFall |= legacy.preFall;
      }
    }

  if(movementTrace!=nullptr) {
    const bool failed = !npc.isPlayer() && moved.blocked;
    DynamicWorld::CollisionTest contact;
    if(failed) {
      contact = collision;
      } else {
      contact.normal = moved.groundNormal;
      contact.landCol = moved.grounded || moved.steep;
      }
    movementTrace->collision(displacement,!failed,contact);
    }

  // Keep the native material/room cache and water state machine. Controller support is
  // authoritative for the feet: a moving body can be well above the landscape ray.
  bool validGround = false;
  const auto pos = npc.position();
  const auto ground = dropRay(pos,validGround);
  const auto water = waterRay(pos);
  const auto chest = waterDepthChest();
  const auto knee = waterDepthKnee();
  if(moved.grounded || moved.steep)
    cache.n = moved.groundNormal;

  if(collision.preFall) {
    // The common controller retained the supported origin. Match the legacy
    // fall rejection for both the hero and NPCs, including its AI notification.
    onMoveFailed(displacement,collision,dt);
    return false;
    }

  if(std::isfinite(water) && ground<water) {
    if(pos.y+chest<=water+0.01f && npc.hasSwimAnimations()) {
      const bool splash = !moved.grounded || moved.velocity.y<-30.f;
      setState(Swim);
      if(splash)
        emitWaterSplash(water);
      return true;
      }
    if(ground+chest<water && !npc.hasSwimAnimations()) {
      npc.setPosition(origin);
      virtualMovementActive = false;
      virtualGroundVelocity = {};
      return false;
      }
    }

  if(moved.steep) {
    // The controller resolves steep contacts and gravity; MoveAlgo chooses the same slide
    // state and animation, without applying a second collision or floor correction.
    fallSpeed = moved.velocity*0.001f;
    fallCount = 0;
    setState(Slide);
    npc.setAnimRotate(0);
    npc.setAnim(slideDir() ? AnimationSolver::SlideA : AnimationSolver::SlideB);
    return true;
    }

  if(moved.grounded) {
    setState(pos.y+knee<=water && ground<water ? InWater : Run);
    clearSpeed();
    if(!npc.isPlayer() && moved.blocked) {
      // Jolt owns collision resolution; retain the existing AI response when a
      // wall or another actor prevents progress (bounce, climb, or mob perception).
      onMoveFailed(displacement,collision,dt);
      faceSteepTravel();
      return false;
      }
    faceSteepTravel();
    return true;
    }

  if(!npc.isPlayer() && !npc.isHuman() && npc.bodyStateMasked()==BS_HIT &&
     validGround && pos.y-ground<stepHeight()) {
    // Legacy grounded attacks retain their root until they leave the floor by
    // a full step. A wolf lunge can briefly detach native support while its
    // animation still owns both vertical and horizontal travel. Do not replace
    // the remaining attack root with the airborne velocity integrator early.
    setState(Run);
    clearSpeed();
    if(moved.blocked) {
      onMoveFailed(displacement,collision,dt);
      return false;
      }
    return true;
    }

  // Falling and landing remain on the old path. Hand over the actual world velocity,
  // including a departing support, at the exact current origin.
  fallSpeed = moved.velocity*0.001f;
  fallCount = 0;
  setState(InAir);
  npc.setAnimRotate(0);
  if(fallSpeed.y<-0.3f && npc.bodyStateMasked()!=BS_FALL)
    npc.setAnim(AnimationSolver::Fall);
  return true;
  }

bool MoveAlgo::preserveMovementImpulse(uint64_t generation,State previousState,std::string_view backend) {
  if(generation==movementImpulseGeneration)
    return false;
  // Moving the controller also tests live projectiles. Their callbacks can
  // launch this NPC before the move returns; its earlier ground result must
  // not clear that new velocity or replace the airborne state with Run.
  npc.setViewPosition(npc.physic.position());
  resetCharacterVirtual();
  if(TestBench::isActive())
    TestBench::movementImpulseHandoff(npc,backend,uint32_t(previousState),uint32_t(flags),spellVelocity());
  return true;
  }

void MoveAlgo::clearSpeed() {
  fallSpeed.x = 0;
  fallSpeed.y = 0;
  fallSpeed.z = 0;
  fallCount   = 0;
  virtualJumpVelocity = {};
  }

void MoveAlgo::resetCharacterVirtual(bool mobTransition) {
  npc.physic.resetVirtual();
  virtualMovementActive = false;
  virtualGroundVelocity = {};
  virtualJumpVelocity = {};
  npcVirtualMobTransition |= mobTransition && !npc.isPlayer() &&
                            (npc.isHuman() || Gothic::physicsFlags().monsterCharacterVirtual);
  }

void MoveAlgo::carryCharacterVirtual(uint64_t dt) {
  if(!virtualMovementActive || (flags!=Run && flags!=InWater))
    return;
  const auto moved = npc.physic.carryVirtual(dt,&npc);
  if(!moved.supportMoved)
    return;
  virtualGroundVelocity = moved.groundVelocity;
  npc.syncCharacterVirtualSupport(moved.position);
  }

void MoveAlgo::takeRagdollControl() {
  // Native bodies replace this movement state machine, including airborne/climbing state.
  // Its stale InAir gate must not block queued script actions after the body settles.
  clearSpeed();
  npc.physic.resetVirtual();
  virtualMovementActive = false;
  virtualGroundVelocity = {};
  flags = Run;
  }

bool MoveAlgo::addSpellImpulse(const Tempest::Vec3& impulseNs, float massKg, bool reaction) {
  if(npc.isDown() || npc.spellFrozen() || !std::isfinite(massKg) || massKg<=0.f ||
     !std::isfinite(impulseNs.x) || !std::isfinite(impulseNs.y) || !std::isfinite(impulseNs.z) ||
     (flags!=Run && flags!=Falling && flags!=InAir))
    return false;
  if(!npc.setInteraction(nullptr,true))
    return false;
  if(reaction) {
    npc.setAnimAngGet(Npc::Anim::FallDeepB);
    npc.setAnimRotate(0);
    }
  if(fallCount>0.f)
    fallSpeed /= fallCount;
  // N*s / kg gives m/s; the existing movement integrator uses cm/ms.
  fallSpeed += impulseNs*(0.1f/massKg);
  const float speed = fallSpeed.length();
  if(speed>2.f)
    fallSpeed *= 2.f/speed;
  fallCount = 0;
  setState(InAir);
  ++movementImpulseGeneration;
  return true;
  }

bool MoveAlgo::accessDamFly(float dx, float dz, char hitType) {
  if(flags!=Run && flags!=Falling && flags!=InAir)
    return false;

  const auto bs  = npc.bodyStateMasked();
  if(bs==BS_LIE || npc.isDead())
    return false;

  if(!npc.setInteraction(nullptr,true))
    return false;

  if(!npc.setAnimAngGet(hitType=='A' ? Npc::Anim::FallDeepA : Npc::Anim::FallDeepB))
    return false;

  npc.setAnimRotate(0);

  float len = std::sqrt(dx*dx+dz*dz);
  auto  vec = Tempest::Vec3(dx,len*0.5f,dz);
  vec = Tempest::Vec3::normalize(vec);

  fallSpeed = vec*0.75f;
  fallCount = 0;
  setState(InAir);
  ++movementImpulseGeneration;
  return true;
  }

void MoveAlgo::applyRotation(Tempest::Vec3& out, const Tempest::Vec3& dpos) const {
  float mul = mulSpeed;
  if(isDive()) {
    float rot = -npc.rotationYRad();
    float s   = std::sin(rot), c = std::cos(rot);

    out.y = -dpos.length()*s;
    mul   = c*mulSpeed;
    } else {
    out.y = dpos.y;
    }
  float rot = npc.rotationRad()+float(M_PI/2);
  applyRotation(out,dpos,rot);
  out.x *= -mul;
  out.z *= -mul;
  }

void MoveAlgo::applyRotation(Tempest::Vec3& out, const Tempest::Vec3& dpos, float rot) const {
  float s   = std::sin(rot), c = std::cos(rot);
  out.x = (dpos.x*c-dpos.z*s);
  out.z = (dpos.x*s+dpos.z*c);
  }

Tempest::Vec3 MoveAlgo::animMoveSpeed(uint64_t dt) const {
  auto dp = npc.animMoveSpeed(dt);
  Tempest::Vec3 ret;
  applyRotation(ret,dp);
  return ret;
  }

Tempest::Vec3 MoveAlgo::npcMoveSpeed(uint64_t dt, MvFlags moveFlg) {
  auto dp = animMoveSpeed(dt);
  if(moveFlg!=WaitMove) {
    if(npc.go2.npc)
      return go2NpcMoveSpeed(dp,*npc.go2.npc);

    if(npc.go2.wp)
      return go2WpMoveSpeed(dp,npc.go2.wp->position());

    if(npc.go2.flag!=Npc::GT_No)
      return go2WpMoveSpeed(dp,npc.go2.pos);
    }
  return dp;
  }

Tempest::Vec3 MoveAlgo::npcFallSpeed(uint64_t dt) {
  // falling
  if(0.f<fallCount) {
    fallSpeed/=fallCount;
    fallCount = 0.f;
    } else {
    fallSpeed += virtualJumpVelocity;
    }
  virtualJumpVelocity = {};
  return fallSpeed*float(dt);
  }

Tempest::Vec3 MoveAlgo::go2NpcMoveSpeed(const Tempest::Vec3& dp,const Npc& tg) {
  return go2WpMoveSpeed(dp,tg.position());
  }

Tempest::Vec3 MoveAlgo::go2WpMoveSpeed(Tempest::Vec3 dp, const Tempest::Vec3& to) {
  auto  d    = to-npc.position();
  float qLen = (d.x*d.x+d.z*d.z);

  const float qSpeed = dp.x*dp.x+dp.z*dp.z;
  if(qSpeed>0.01f){
    float k = std::sqrt(std::min(qLen,qSpeed)/qSpeed);
    dp.x*=k;
    dp.z*=k;
    }
  return dp;
  }

bool MoveAlgo::testSlide(const Tempest::Vec3& pos, DynamicWorld::CollisionTest& out) const {
  // check ground
  const auto norm = normalRay(pos);
  return testSlide(pos, norm, out);
  }

bool MoveAlgo::testSlide(const Tempest::Vec3& pos, const Tempest::Vec3& norm, DynamicWorld::CollisionTest& out) const {
  const float slideBegin = std::min(slideAngle(), 1.f);
  const float slideEnd   = slideAngle2();

  out.normal = norm;

  if(!(slideEnd<norm.y && norm.y<slideBegin)) {
    return false;
    }

  const auto tangent = Tempest::Vec3::crossProduct(norm, Tempest::Vec3(0,1,0));
  const auto slide   = Tempest::Vec3::crossProduct(norm, tangent);

  if(!npc.testMove(pos+slide*gravity*0.15f))
    return false;
  return true;
  }

float MoveAlgo::stepHeight() const {
  auto gl = npc.guild();
  auto v  = float(npc.world().script().guildVal().step_height[gl]);
  if(v>0.f)
    return v;
  return 50;
  }

bool MoveAlgo::isEdgeMoveBlocked(const Tempest::Vec3& modelDirection) const {
  if(!npc.isPlayer() || npc.isDown() || (flags!=Run && flags!=Slide) || npc.bodyStateMasked()==BS_JUMP)
    return false;
  const auto weapon = npc.weaponState();
  if(weapon!=WeaponState::Bow && weapon!=WeaponState::CBow)
    return false;

  Tempest::Vec3 dir;
  applyRotation(dir,modelDirection);
  dir.y = 0;
  const float len = dir.length();
  if(len==0.f)
    return false;

  const auto pos = npc.position();
  // Less than 2 mm of an independent 2 cm probe can pass: the guard is holding this direction.
  // Rechecking geometry lets retreating, strafing or turning away resume without a stale blocked flag.
  return edgeMoveFraction(pos,pos+dir*(2.f/len))<0.1f;
  }

MoveAlgo::EdgeProbe MoveAlgo::edgeAhead(const Tempest::Vec3& direction, float range) const {
  static constexpr float maxDrop = 100.f;
  static constexpr float step = 10.f;
  const float len = std::sqrt(direction.x*direction.x+direction.z*direction.z);
  if(len==0.f || range<=0.f)
    return {};
  const Tempest::Vec3 dir(direction.x/len,0,direction.z/len);
  const auto from = npc.position();
  const float up = stepHeight()+eps;
  auto floor = [&](float distance, float height) {
    const auto point = from+dir*distance;
    return npc.world().physic()->landRay({point.x,height+up,point.z});
    };
  auto r = floor(0,from.y);
  if(!r.hasCol)
    return {};

  // Match edgeMoveFraction: follow each supported surface, so a continuous hill is not an edge.
  float lo = 0, height = r.v.y;
  for(float distance=std::min(step,range);; distance=std::min(distance+step,range)) {
    r = floor(distance,height);
    if(!r.hasCol || height-r.v.y>maxDrop) {
      const float drop = r.hasCol ? height-r.v.y : std::numeric_limits<float>::infinity();
      float hi = distance;
      for(int i=0;i<8;++i) {
        const float mid = (lo+hi)*0.5f;
        const auto q = floor(mid,height);
        if(q.hasCol && height-q.v.y<=maxDrop)
          lo = mid; else
          hi = mid;
        }
      return {true,lo,drop,height};
      }
    if(distance>=range)
      return {};
    lo = distance;
    height = r.v.y;
    }
  }

float MoveAlgo::edgeMoveFraction(const Tempest::Vec3& from, const Tempest::Vec3& to) const {
  static constexpr float margin  = 30.f;
  static constexpr float maxDrop = 100.f;
  static constexpr float step    = 10.f;

  const auto  dp   = to-from;
  const float dist = std::sqrt(dp.x*dp.x+dp.z*dp.z);
  if(dist==0.f)
    return 1.f;

  const float up = stepHeight()+eps;
  auto floor = [&](float d, float height) {
    const auto p = from+dp*(d/dist);
    return npc.world().physic()->landRay({p.x,height+up,p.z});
    };
  auto r = floor(0,from.y);
  if(!r.hasCol)
    return 1.f;

  // Follow the surface rather than comparing its height with the hero: small steps and continuous
  // slopes stay traversable. Probe beyond the actual move to leave room before a dangerous drop.
  float lo = 0, height = r.v.y;
  const float end = dist+margin;
  for(float d=std::min(step,end);; d=std::min(d+step,end)) {
    r = floor(d,height);
    if(!r.hasCol || height-r.v.y>maxDrop) {
      float hi = d;
      for(int i=0; i<8; ++i) {
        const float mid = (lo+hi)*0.5f;
        const auto  q   = floor(mid,height);
        if(q.hasCol && height-q.v.y<=maxDrop)
          lo = mid; else
          hi = mid;
        }
      return std::clamp((lo-margin)/dist,0.f,1.f);
      }
    if(d>=end)
      return 1.f;
    lo     = d;
    height = r.v.y;
    }
  }

float MoveAlgo::slideAngle() const {
  auto  gl = npc.guild();
  float k  = float(M_PI)/180.f;
  return std::sin((90.f-float(npc.world().script().guildVal().slide_angle[gl]))*k);
  }

float MoveAlgo::slideAngle2() const {
  auto  gl = npc.guild();
  float k  = float(M_PI)/180.f;
  return std::sin(float(npc.world().script().guildVal().slide_angle2[gl])*k);
  }

float MoveAlgo::waterDepthKnee() const {
  auto gl = npc.guild();
  return float(npc.world().script().guildVal().water_depth_knee[gl]);
  }

float MoveAlgo::waterDepthChest() const {
  auto gl = npc.guild();
  return float(npc.world().script().guildVal().water_depth_chest[gl]);
  }

float MoveAlgo::falldownHeight() const {
  auto gl = npc.guild();
  return float(npc.world().script().guildVal().falldown_height[gl]);
  }

bool MoveAlgo::canFlyOverWater() const {
  auto  gl = npc.guild();
  auto& g  = npc.world().script().guildVal();
  return g.water_depth_chest[gl]==flyOverWaterHint &&
         g.water_depth_knee [gl]==flyOverWaterHint;
  }

bool MoveAlgo::canFallByGravity() const {
  return falldownHeight()!=9999;
  }

bool MoveAlgo::checkLastBounce() const {
  uint64_t ticks = npc.world().tickCount();
  return lastBounce<ticks;
  }

void MoveAlgo::emitWaterSplash(float y) {
  auto& world = npc.world();

  auto pos = npc.position();
  pos.y = y;

  Tempest::Matrix4x4 at;
  at.identity();
  at.translate(pos);

  Effect e(PfxEmitter(world,"PFX_WATERSPLASH"),"");
  e.setObjMatrix(at);
  e.setActive(true);
  world.runEffect(std::move(e));

  npc.emitSoundEffect("CS_INTRO_WATERSPLASH.WAV",2500,false);
  }

int32_t MoveAlgo::diveTime() const {
  if(!isDive())
    return 0;
  return int32_t(npc.world().tickCount() - diveStart);
  }

bool MoveAlgo::isClose(const Npc& npc, const Npc& p, float dist) {
  float len = npc.qDistTo(p);
  return (len<dist*dist);
  }

bool MoveAlgo::isClose(const Npc& npc, const WayPoint& p) {
  float dist = closeToPointThreshold;
  if(p.useCounter()>1)
    dist += 100; // needed for some peasants on Onars farm
  return isClose(npc,p,dist);
  }

bool MoveAlgo::isClose(const Npc& npc, const WayPoint& p, float dist) {
  auto  dp  = p.groundPos - npc.position();
  float len = dp.quadLength();
  return (len<dist*dist);
  }

bool MoveAlgo::isClose(const Npc& npc, const Tempest::Vec3& p, float dist) {
  auto  dp  = (p - npc.centerPosition());
  float len = dp.quadLength();
  // NOTE: extra check fo vertical only distances:
  //       some repair planks are too high (~5 meters) in the air
  return (len<dist*dist) || Tempest::Vec2(dp.x,dp.z).quadLength() < 1.f;
  }

bool MoveAlgo::startClimb(JumpStatus jump) {
  auto sq = npc.setAnimAngGet(jump.anim);
  if(sq==nullptr)
    return false;

  climbStart  = npc.world().tickCount();
  climbPos0   = npc.position();
  climbHeight = jump.height;

  const float dHeight = (jump.height-climbPos0.y);

  fallSpeed.x = 0.f;
  fallSpeed.y = dHeight/sq->totalTime();
  fallSpeed.z = 0.f;
  fallCount   = 0.f;
  virtualJumpVelocity = {};

  if(jump.anim==Npc::Anim::JumpUp && dHeight>0.f){
    setState(JumpUp);
    float t = std::sqrt(2.f*dHeight/gravity);
    fallSpeed.y = gravity*t;
    }
  else if(jump.anim==Npc::Anim::JumpUpMid ||
          jump.anim==Npc::Anim::JumpUpLow) {
    setState(ClimbUp);
    }
  else if(isJumpUp() && jump.anim==Npc::Anim::JumpHang) {
    setState(ClimbUp);
    }
  else {
    return false;
    }
  return true;
  }

void MoveAlgo::startDive() {
  if(!isSwim())
    return;

  if(npc.world().tickCount()-diveStart>1000) {
    setState(Dive);

    auto  pos   = npc.position();
    float pY    = pos.y;
    float chest = canFlyOverWater() ? 0 : waterDepthChest();
    auto  water = waterRay(pos);
    tryMove(0,water-chest-pY,0);
    }
  }

bool MoveAlgo::isFalling() const {
  return flags==Falling;
  }

bool MoveAlgo::isSlide() const {
  return flags==Slide;
  }

bool MoveAlgo::isInAir() const {
  return flags==InAir;
  }

bool MoveAlgo::isJumpUp() const {
  return flags==JumpUp;
  }

bool MoveAlgo::isClimb() const {
  return flags==ClimbUp;
  }

bool MoveAlgo::isInWater() const {
  return flags==InWater;
  }

bool MoveAlgo::isSwim() const {
  return flags==Swim;
  }

bool MoveAlgo::isDive() const {
  return flags==Dive;
  }

void MoveAlgo::setState(State f) {
  if(f==flags)
    return;

#ifndef NDEBUG
  assertStateChange(f);
#endif

  auto isFlyLike = [](State f) {
    return f==InAir || f==Falling || f==Slide || f==Jump || f==JumpUp || f==ClimbUp;
    };

  if(isFlyLike(flags) && !isFlyLike(f)) {
    clearSpeed();
    }

  if((f==Swim) && !(flags==Swim)) {
    auto ws = npc.weaponState();
    npc.setAnim(Npc::Anim::NoAnim);
    if(ws!=WeaponState::NoWeapon && ws!=WeaponState::Fist)
      npc.closeWeapon(true);
    npc.dropTorch(true);
    }

  if((f==Dive) && !(flags==Dive)) {
    npc.setDirectionY(-40);
    }
  if((f==Dive) != (flags==Dive)) {
    diveStart = npc.world().tickCount();
    }

  flags = f;
  }

void MoveAlgo::assertStateChange(State f) {
  const bool dead = npc.isDead();
  // release build quirks
  (void)f;
  (void)dead;
  // assert possible transitions
  switch(flags) {
    case Run:
      break;
    case InAir:
      assert(f==Run || f==Falling || f==InWater || f==Swim || f==Dive);
      break;
    case Falling:
      assert(f==Run || f==InAir || f==InWater || f==Swim || f==Dive);
      break;
    case Slide:
      break;
    case Jump:
      assert(f==Run || f==InAir || f==Falling || f==Swim || f==Dive);
      break;
    case JumpUp:
      assert(f==Run || f==InAir || f==ClimbUp);
      break;
    case ClimbUp:
      assert(f==Run);
      break;
    case InWater:
      assert(f==Run || f==InAir || f==Slide || f==Jump || f==JumpUp || f==Swim || f==Dive);
      break;
    case Swim:
      assert(f==Run || f==InAir || f==InWater || f==Dive);
      break;
    case Dive:
      assert(f==InAir || f==Swim || f==InWater);
      break;
    }

  // should be alive to transition into those states
  switch(f) {
    case Slide:
    case Jump:
    case JumpUp:
    case Swim:
    case Dive:
    case ClimbUp:
    case Falling:
      assert(!dead);
      break;
    case Run:
    case InAir:
    case InWater:
      break;
    }
  }

bool MoveAlgo::slideDir() const {
  float a = std::atan2(fallSpeed.x,fallSpeed.z);
  float b = npc.rotationRad();

  auto s = std::sin(a-b);
  return s>0;
  }

bool MoveAlgo::testMoveDirection(const Tempest::Vec3& dp, const Tempest::Vec3& dir) const {
  Tempest::Vec3 tr, tr2;
  applyRotation(tr,dir);
  tr2 = Tempest::Vec3::normalize(dp);
  return Tempest::Vec3::dotProduct(tr,tr2)>0.8f;
  }

bool MoveAlgo::isForward(const Tempest::Vec3& dp) const {
  return testMoveDirection(dp,{0,0, 1});
  }

bool MoveAlgo::isBackward(const Tempest::Vec3& dp) const {
  return testMoveDirection(dp,{0,0,-1});
  }

void MoveAlgo::onMoveFailed(const Tempest::Vec3& dp, const DynamicWorld::CollisionTest& info, uint64_t dt) {
  static float threshold = 0.4f;
  static float speed     = 360.f;

  if(dp==Tempest::Vec3())
    return;

  const auto  ortho   = Tempest::Vec3::crossProduct(Tempest::Vec3::normalize(dp),Tempest::Vec3(0,1,0));
  const float stp     = speed*float(dt)/1000.f;
  const float val     = Tempest::Vec3::dotProduct(ortho,info.normal);
  const bool  forward = isForward(dp);
  const bool  bump    = checkLastBounce();

  if(info.vob!=nullptr && forward && npc.interactive()!=info.vob) {
    npc.setDetectedMob(info.vob);
    npc.perceptionProcess(npc,nullptr,0,PERC_MOVEMOB);
    if(npc.moveHint()==Npc::GT_No)
      return;
    }

  if(forward && !info.preFall && npc.currentTarget==info.npc && npc.bodyStateMasked()==BS_HIT) {
    // if charge attack run into npc, FAI should be reengaged
    // makes ai behaviour more consistent with vanilla, when fighting field-raiders
    TestBench::monsterCharacterCollision(npc,info.npc);
    npc.implFaiWait(0);
    return;
    }

  if(npc.processPolicy()!=NpcProcessPolicy::Player && bump) {
    lastBounce = npc.world().tickCount();
    if(info.npc!=nullptr)
      lastBounce += 800; else
      lastBounce += 750;
    }

  if(std::abs(val)>=threshold && !info.preFall && bump) {
    // emulate bouncing behaviour of original game
    Tempest::Vec3 corr;
    for(int i=5; i<=35; i+=5) {
      for(float angle:{float(i),-float(i)}) {
        applyRotation(corr,dp,float(angle*M_PI)/180.f);
        if(npc.testMove(corr)) {
          if(forward)
            npc.setDirection(npc.rotation()+angle);
          return;
          }
        }
      }
    }

  if(!forward)
    return;

  if(!info.preFall && npc.isAttackAnim())
    return;

  npc.setAnimRotate(0);
  if(val<-threshold) {
    npc.setDirection(npc.rotation()-stp);
    }
  else if(val>threshold) {
    npc.setDirection(npc.rotation()+stp);
    }
  else switch(npc.moveHint()) {
    case Npc::GT_No:
      npc.setAnim(Npc::Anim::Idle);
      break;
    case Npc::GT_NextFp:
      npc.clearGoTo();
      break;
    case Npc::GT_Item:
      npc.setDirection(npc.rotation()+stp);
      break;
    case Npc::GT_Enemy:
    case Npc::GT_EnemyG:
    case Npc::GT_Way:
    case Npc::GT_Point: {
      if(info.npc!=nullptr || info.preFall) {
        npc.setDirection(npc.rotation()+stp);
        } else {
        auto jc = npc.tryJump();
        if(jc.noClimb)
          npc.setDirection(npc.rotation()+stp); else
        if(!npc.startClimb(jc))
          npc.setDirection(npc.rotation()+stp);
        }
      break;
      }
    case Npc::GT_Flee:
      npc.setDirection(npc.rotation()+stp);
      break;
    }
  }

void MoveAlgo::onGravityFailed(const DynamicWorld::CollisionTest& info, uint64_t dt) {
  auto        norm      = info.normal;
  const float normXZ    = std::sqrt(norm.x*norm.x+norm.z*norm.z);
  const float normXZInv = std::sqrt(1.f - normXZ*normXZ);

  if(normXZ>0.001f && normXZ<std::fabs(norm.y) && normXZInv>0.001f) {
    norm.x = normXZInv*norm.x/normXZ;
    norm.z = normXZInv*norm.z/normXZ;
    norm.y = normXZ   *norm.y/normXZInv;
    }

  auto reflect = [](const Tempest::Vec3& I, const Tempest::Vec3& N) {
    return I - 2.f*Tempest::Vec3::dotProduct(N,I) * N;
    };

  assert(fallCount==0.f);
  if(true || Tempest::Vec3::dotProduct(fallSpeed,norm)<0.f) {
    //float len  = fallSpeed.length()/std::max(1.f,fallCount);
    if(Tempest::Vec2::dotProduct({fallSpeed.x, fallSpeed.z}, {norm.x, norm.z})<0.f) {
      fallSpeed = reflect(fallSpeed, norm)*0.75f;
      } else {
      fallSpeed = norm*fallSpeed.length();
      }
    fallCount  = 0;
    } else {
    fallSpeed += 10.f*norm*gravity*float(dt);
    }
  }

float MoveAlgo::waterRay(const Tempest::Vec3& pos) const {
  if(std::fabs(cacheW.x-pos.x)>eps || std::fabs(cacheW.y-pos.y)>eps || std::fabs(cacheW.z-pos.z)>eps) {
    // const float threshold = canFlyOverWater() ? -stepHeight() : -0.1f;
    const auto  spos      = pos; //Tempest::Vec3(pos.x, pos.y+threshold, pos.z);
    static_cast<DynamicWorld::RayWaterResult&>(cacheW) = npc.world().physic()->waterRay(spos, stepHeight());
    cacheW.x = pos.x;
    cacheW.y = pos.y;
    cacheW.z = pos.z;
    }
  return cacheW.wdepth;
  }

void MoveAlgo::rayMain(const Tempest::Vec3& pos) const {
  if(std::fabs(cache.x-pos.x)>eps || std::fabs(cache.y-pos.y)>eps || std::fabs(cache.z-pos.z)>eps) {
    float threshold = npc.physic.groundOffset() + 1.f;
    float dy        = threshold+100;  // 1 meter extra offset
    if(fallSpeed.y<0 || false)
      dy = 0; // whole world
    const auto  spos = Tempest::Vec3(pos.x, pos.y+threshold, pos.z);
    static_cast<DynamicWorld::RayLandResult&>(cache) = npc.world().physic()->landRay(spos,dy);
    cache.x = pos.x;
    cache.y = pos.y;
    cache.z = pos.z;
    }
  }

float MoveAlgo::dropRay(const Tempest::Vec3& pos, bool &hasCol) const {
  rayMain(pos);
  hasCol = cache.hasCol;
  return cache.v.y;
  }

Tempest::Vec3 MoveAlgo::normalRay(const Tempest::Vec3& pos) const {
  rayMain(pos);
  return cache.n;
  }

asset::MaterialGroup MoveAlgo::groundMaterial() const {
  const float stp = stepHeight();
  if(cacheW.wdepth+stp>cache.v.y)
    return asset::MaterialGroup::WATER;
  return cache.mat;
  }

Tempest::Vec3 MoveAlgo::groundNormal() const {
  return cache.n;
  }

std::string_view MoveAlgo::portalName() {
  return portal;
  }

std::string_view MoveAlgo::formerPortalName() {
  return formerPortal;
  }
