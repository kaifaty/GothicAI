// Ground motion of the nav-sim stand (docs/nav-sim.md, task S2).
//
// One decision of an agent: its keys, the key history and the age of the keys
// pick the 100 ms translation from the root-motion table (scripts/navsim/
// motion_table.py), forward and right in the decision's starting frame. Each of
// the decision's 33/33/34 ms ticks then runs in the engine's order
// (npckeys.cpp:42-162): first the movement, then the code turn of 90 deg/s.
// The movement is Jolt CharacterVirtual with the NPC capsule and the guild
// limits, ported from CollisionWorld::VirtualCharacter::move
// (collisionworld.cpp:1128-1453) and DynamicWorld::NpcItem::moveVirtual
// (dynamicworld.cpp:3362-3543) for a static world: no pushable bodies, moving
// supports or other NPCs exist in the dump. Vertical motion is physics only.
//
// The static meshes are shared; every shard of agents owns a PhysicsSystem with
// its own copies of the three static bodies. A shard is stepped by one worker
// at a time, so no body or broadphase lock is ever shared between threads
// (research §3.2: a shared PhysicsSystem stops scaling at 8 threads).
#include "motion.h"
#include "pool.h"

#include <Jolt/Jolt.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
#include <iterator>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace navsim {
namespace {

constexpr float Pi = 3.14159265358979323846f;
constexpr float GravityMS = 9.8f;          // DynamicWorld::gravityMS
constexpr float WorldHeightCm = 20000.f;   // DynamicWorld::worldHeight, the landRay depth
constexpr int MissingCode = 54;            // motion_table.key_ids: no previous decision
constexpr int AgeLower[] = {0,1,2,3,5,10,20,50}; // motion_table.AGE_LOWER_STEPS, decisions
constexpr int Levels = 5;                  // table levels -1..3
constexpr uint8_t Kinematic = 4;           // NAVSTAT1 category of kinematic objects (doors, mobsi)

void registerJolt() {
  static std::once_flag once;
  std::call_once(once,[] {
    JPH::RegisterDefaultAllocator();
    JPH::Factory::sInstance = new JPH::Factory();
    JPH::RegisterTypes();
    });
  }

struct OneBroadPhaseLayer final : JPH::BroadPhaseLayerInterface {
  JPH::uint GetNumBroadPhaseLayers() const override { return 1; }
  JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer) const override { return JPH::BroadPhaseLayer(0); }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
  const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer) const override { return "static"; }
#endif
  };

struct StaticPairs final : JPH::ObjectLayerPairFilter {
  bool ShouldCollide(JPH::ObjectLayer, JPH::ObjectLayer) const override { return false; } // static never meets static
  };

struct AnyBroadPhase final : JPH::ObjectVsBroadPhaseLayerFilter {
  bool ShouldCollide(JPH::ObjectLayer, JPH::BroadPhaseLayer) const override { return true; }
  };

// CollisionWorld::toMeters / toCentimeters.
JPH::Vec3 toMeters(const JPH::Vec3& cm) { return JPH::Vec3(cm.GetX()*0.01f,cm.GetY()*0.01f,cm.GetZ()*0.01f); }
JPH::Vec3 toCentimeters(JPH::Vec3Arg m) { return JPH::Vec3(m.GetX()*100.f,m.GetY()*100.f,m.GetZ()*100.f); }

// isNpcMoveSkipped (dynamicworld.cpp:65): the legacy component-wise 2 cm skip.
bool moveSkipped(const JPH::Vec3& d) {
  constexpr float eps = 2.f;
  return std::abs(d.GetX())<eps && std::abs(d.GetY())<eps && std::abs(d.GetZ())<eps;
  }

struct Shard final {
  OneBroadPhaseLayer layers;
  StaticPairs pairs;
  AnyBroadPhase broad;
  JPH::PhysicsSystem physics;
  std::vector<JPH::BodyID> bodies;
  JPH::BodyID kinematic;
  size_t first = 0, end = 0;

  Shard(const std::vector<std::pair<uint8_t,JPH::ShapeRefC>>& shapes) {
    physics.Init(uint32_t(shapes.size())+1,0,64,64,layers,broad,pairs);
    auto& bi = physics.GetBodyInterfaceNoLock();
    for(const auto& [category,shape]:shapes) {
      JPH::BodyCreationSettings body(shape.GetPtr(),JPH::RVec3::sZero(),JPH::Quat::sIdentity(),
                                     JPH::EMotionType::Static,JPH::ObjectLayer(category));
      body.mUserData = category;
      const auto id = bi.CreateAndAddBody(body,JPH::EActivation::DontActivate);
      if(id.IsInvalid())
        throw std::runtime_error("Jolt static body allocation failed");
      bodies.push_back(id);
      if(category==Kinematic)
        kinematic = id;
      }
    physics.OptimizeBroadPhase();
    }

  ~Shard() {
    auto& bi = physics.GetBodyInterfaceNoLock();
    for(auto id:bodies) {
      bi.RemoveBody(id);
      bi.DestroyBody(id);
      }
    }
  };

struct Agent final {
  std::unique_ptr<JPH::CharacterVirtual> character;
  Shard* shard = nullptr;
  bool posed = false;
  float yaw = 0;                 // body yaw, degrees: forward (cos, 0, sin), like Npc::rotation()
  uint64_t gameMs = 0;
  std::array<int8_t,4> keys{};   // forward, strafe, turn, walk of the next decision
  std::array<int,3> history = {MissingCode,MissingCode,MissingCode};
  int held = 0;                  // age of history[0] at its own decision
  // MoveAlgo state after the last tick: Run (supported), Slide (steep) or InAir.
  enum State : uint8_t { Ground, Slide, Air } state = Ground;
  JPH::Vec3 carried = JPH::Vec3::sZero(); // fallSpeed of Slide/InAir, cm/s
  // CollisionWorld::VirtualCharacter::Impl
  bool contactsDirty = true;
  bool needsInitialUpdate = true;
  JPH::Vec3 lastGroundNormal = JPH::Vec3::sAxisY();
  // The last decision
  float tableForward = 0, tableRight = 0;
  int8_t level = -2;
  uint8_t flags = 0;
  };

struct MoveResult final {
  JPH::Vec3 position, velocity; // cm, cm/s
  bool grounded = false, steep = false, stepped = false, blocked = false, preFall = false;
  };

int code(const std::array<int8_t,4>& k) {
  return (((k[0]+1)*3+k[1]+1)*3+k[2]+1)*2+k[3];
  }

int ageBin(int age) {
  int bin = 0;
  for(int i=0; i<int(std::size(AgeLower)); ++i)
    if(AgeLower[i]<=age)
      bin = i;
  return bin;
  }

bool validKeys(const int8_t* k) {
  for(int i=0; i<3; ++i)
    if(k[i]<-1 || k[i]>1)
      return false;
  return k[3]==0 || k[3]==1;
  }
}

struct Motion::Impl final {
  MotionParams params;
  FloorProbe floor;
  float radius = 0, height = 0, step = 0, maxSlope = 0, slopeDeg = 0, groundOffset = 0;
  JPH::RefConst<JPH::Shape> capsule;
  std::vector<std::pair<uint8_t,JPH::ShapeRefC>> shapes;
  // Declaration order: agents (their characters) go before the shards they live in.
  std::vector<std::unique_ptr<Shard>> shards;
  std::vector<Agent> agents;
  struct Table final {
    std::vector<int64_t> keys;
    std::vector<std::array<float,2>> values;
    };
  std::array<Table,Levels> tables;

  bool lookup(int level, int64_t key, float& forward, float& right) const {
    const auto& t = tables[size_t(level+1)];
    const auto it = std::lower_bound(t.keys.begin(),t.keys.end(),key);
    if(it==t.keys.end() || *it!=key)
      return false;
    const auto& v = t.values[size_t(it-t.keys.begin())];
    forward = v[0];
    right = v[1];
    return true;
    }

  // motion_table.key_ids and lookup(): exact history 3 first, then shorter
  // histories with the age bin, then the current keys alone.
  void decide(Agent& a) const {
    const int current = code(a.keys);
    const int age = current==a.history[0] ? a.held+1 : 0;
    const int64_t bin = ageBin(age);
    std::array<int64_t,4> packed;
    packed[0] = current;
    int64_t scale = 1;
    for(int h=1; h<=3; ++h) {
      scale *= 55;
      packed[size_t(h)] = packed[size_t(h-1)]+scale*a.history[size_t(h-1)];
      }
    a.level = -2;
    a.tableForward = a.tableRight = 0;
    for(int level=3; level>=0; --level)
      if(lookup(level,bin+8*packed[size_t(level)],a.tableForward,a.tableRight)) {
        a.level = int8_t(level);
        break;
        }
    if(a.level==-2 && lookup(-1,current,a.tableForward,a.tableRight))
      a.level = -1;
    a.history = {current,a.history[0],a.history[1]};
    a.held = age;
    }

  // VirtualCharacter::Impl::finiteGroundNormal: keep the last valid normal.
  JPH::Vec3 groundNormal(Agent& a) const {
    const auto n = a.character->GetGroundNormal();
    const float x = n.GetX(), y = n.GetY(), z = n.GetZ();
    if(std::isfinite(x) && std::isfinite(y) && std::isfinite(z) && double(x)*x+double(y)*y+double(z)*z>1.e-12) {
      a.lastGroundNormal = n;
      return n;
      }
    return a.lastGroundNormal;
    }

  // VirtualCharacter::Impl::initialIdleGravity (collisionworld.cpp:793): only a
  // verified nonpenetrating static plane may start at rest after a teleport.
  JPH::Vec3 initialIdleGravity(Agent& a, JPH::Vec3Arg gravity) const {
    auto& c = *a.character;
    const float numericalPenetration = .01f*0.001f; // 1% of mCollisionTolerance
    for(const auto& contact:c.GetActiveContacts())
      if(!contact.mWasDiscarded && !contact.mIsSensorB && contact.mDistance<-numericalPenetration)
        return gravity;
    const auto normal = groundNormal(a);
    const auto id = c.GetGroundBodyID();
    JPH::BodyLockRead lock(a.shard->physics.GetBodyLockInterfaceNoLock(),id);
    if(!lock.SucceededAndIsInBroadPhase() || !lock.GetBody().IsStatic())
      return gravity;
    const auto& body = lock.GetBody();
    const auto subShape = c.GetGroundSubShapeID();
    JPH::SubShapeID remainder;
    const auto* leaf = body.GetShape()->GetLeafShape(subShape,remainder);
    if(leaf==nullptr || (leaf->GetSubType()!=JPH::EShapeSubType::Mesh && leaf->GetSubType()!=JPH::EShapeSubType::Box))
      return gravity;
    const auto face = body.GetWorldSpaceSurfaceNormal(subShape,c.GetGroundPosition());
    constexpr float cosTwoDegrees = .999390827f;
    if(c.IsSlopeTooSteep(face) || face.Dot(normal)<cosTwoDegrees)
      return gravity;
    for(const auto& contact:c.GetActiveContacts()) {
      if(contact.mBodyB!=id || contact.mSubShapeIDB!=subShape || !contact.mHadCollision ||
         contact.mWasDiscarded || contact.mIsSensorB)
        continue;
      if(face.Dot(contact.mContactNormal)>=cosTwoDegrees && face.Dot(contact.mSurfaceNormal)>=cosTwoDegrees)
        return face*gravity.Dot(face);
      }
    return gravity;
    }

  // VirtualCharacter::setPosition.
  void teleport(Agent& a, const JPH::Vec3& feetCm) const {
    a.character->SetPosition(JPH::RVec3(toMeters(feetCm)));
    a.character->SetLinearVelocity(JPH::Vec3::sZero());
    a.contactsDirty = true;
    a.needsInitialUpdate = true;
    }

  // VirtualCharacter::move for a static world (collisionworld.cpp:1128-1453,
  // human: limitStairAdvance and bounceOnSteepContact off).
  MoveResult move(Agent& a, JPH::TempAllocator& alloc, const JPH::Vec3& displacement, uint64_t dt, bool standing) const {
    auto& c = *a.character;
    const float seconds = float(dt)*0.001f;
    if(a.contactsDirty) {
      c.RefreshContacts({},{},{},{},alloc);
      a.contactsDirty = false;
      }
    MoveResult result;
    if(seconds>0.f) {
      const bool onGround = c.GetGroundState()==JPH::CharacterBase::EGroundState::OnGround;
      c.UpdateGroundVelocity();
      auto velocity = toMeters(displacement)/seconds;
      if(onGround)
        velocity += c.GetGroundVelocity(); // zero on the static dump
      auto gravityStep = JPH::Vec3(0,-GravityMS*seconds,0);
      const bool idle = displacement==JPH::Vec3::sZero() || standing;
      if(idle && onGround) {
        if(a.needsInitialUpdate)
          gravityStep = initialIdleGravity(a,gravityStep);
        else {
          const auto normal = groundNormal(a);
          gravityStep = normal*gravityStep.Dot(normal);
          }
        }
      velocity += gravityStep;
      c.SetLinearVelocity(velocity);
      JPH::CharacterVirtual::ExtendedUpdateSettings settings;
      const float riserRun = maxSlope>0.f ? std::min(step,step/std::tan(maxSlope)) : step;
      settings.mWalkStairsStepForwardTest = std::max(settings.mWalkStairsStepForwardTest,
                                                    riserRun+radius+c.GetCharacterPadding());
      settings.mWalkStairsStepUp = JPH::Vec3::sZero();
      settings.mStickToFloorStepDown = JPH::Vec3(0,-step,0);
      const auto oldPosition = c.GetPosition();
      const auto desiredVelocity = c.GetLinearVelocity();
      c.ExtendedUpdate(seconds,JPH::Vec3(0,-GravityMS,0),settings,{},{},{},{},alloc);
      a.needsInitialUpdate = false;
      // Impl::blocksAutomaticStairs: interactive objects are not treads. The dump
      // marks kinematic objects (doors, mobsi) only; static interactive furniture
      // is indistinguishable from other static objects there.
      bool stairObstacle = false;
      for(const auto& contact:c.GetActiveContacts()) {
        if(!contact.mHadCollision || contact.mWasDiscarded || contact.mIsSensorB ||
           contact.mSurfaceNormal.Dot(desiredVelocity-contact.mLinearVelocity)>=0.f)
          continue;
        if(contact.mBodyB==a.shard->kinematic) {
          stairObstacle = true;
          break;
          }
        }
      const auto up = c.GetUp();
      const auto stairVelocity = desiredVelocity-gravityStep;
      const auto desiredStep = (stairVelocity-stairVelocity.Dot(up)*up)*seconds;
      const float desiredLength = desiredStep.Length();
      if(step>0.f && !stairObstacle && desiredLength>0.f && c.CanWalkStairs(desiredVelocity)) {
        const auto forward = desiredStep/desiredLength;
        const auto achieved = JPH::Vec3(c.GetPosition()-oldPosition);
        const float achievedLength = std::max(0.f,achieved.Dot(forward));
        if(achievedLength+1.e-4f<desiredLength) {
          auto direction = forward;
          float best = settings.mWalkStairsCosAngleForwardContact;
          for(const auto& contact:c.GetActiveContacts()) {
            if(!contact.mHadCollision || contact.mWasDiscarded ||
               contact.mSurfaceNormal.Dot(desiredVelocity-contact.mLinearVelocity)>=0.f ||
               !c.IsSlopeTooSteep(contact.mSurfaceNormal))
              continue;
            const auto candidate = contact.mSurfaceNormal.Dot(up)*up-contact.mSurfaceNormal;
            const float length = candidate.Length();
            if(length>1.e-6f && candidate.Dot(forward)/length>best) {
              direction = candidate/length;
              best = direction.Dot(forward);
              }
            }
          const auto forwardStep = direction*std::max(settings.mWalkStairsMinStepForward,desiredLength-achievedLength);
          result.stepped = c.WalkStairs(seconds,up*step,forwardStep,direction*settings.mWalkStairsStepForwardTest,
                                        settings.mWalkStairsStepDownExtra,{},{},{},{},alloc);
          }
        }
      }
    result.position = toCentimeters(JPH::Vec3(c.GetPosition()));
    // Jolt keeps the requested velocity: resolve it against the actual contacts.
    auto solved = c.GetLinearVelocity();
    for(int iteration=0; iteration<2; ++iteration)
      for(const auto& contact:c.GetActiveContacts()) {
        if(!contact.mHadCollision || contact.mWasDiscarded || contact.mIsSensorB)
          continue;
        const float into = (solved-contact.mLinearVelocity).Dot(contact.mContactNormal);
        if(into<0.f)
          solved -= contact.mContactNormal*into;
        }
    result.velocity = toCentimeters(solved);
    (void)groundNormal(a); // the engine's move_output read keeps the last valid normal
    result.grounded = c.GetGroundState()==JPH::CharacterBase::EGroundState::OnGround;
    result.steep = c.GetGroundState()==JPH::CharacterBase::EGroundState::OnSteepGround;
    return result;
    }

  // DynamicWorld::NpcItem::moveVirtual without other NPCs: the fall guard and the blocked test.
  MoveResult advance(Agent& a, JPH::TempAllocator& alloc, const JPH::Vec3& displacement, uint64_t dt, bool standing) const {
    const auto initial = toCentimeters(JPH::Vec3(a.character->GetPosition()));
    auto moved = move(a,alloc,displacement,dt,standing);
    const auto candidate = moved.position;
    const bool preFall = floor && !floor(candidate.GetX(),std::max(initial.GetY(),candidate.GetY())+groundOffset+1.f,
                                         candidate.GetZ(),WorldHeightCm);
    const auto accepted = preFall ? initial : candidate;
    if(preFall) {
      teleport(a,initial);
      const auto support = move(a,alloc,JPH::Vec3::sZero(),0,false);
      moved.grounded = support.grounded;
      moved.steep = support.steep;
      }
    auto velocity = moved.velocity;
    const auto back = moved.position-accepted;
    if(back.LengthSq()>0.0001f) {
      const auto normal = back.Normalized();
      const float into = velocity.Dot(normal);
      if(into>0.f)
        velocity -= normal*into;
      }
    MoveResult result = moved;
    result.position = accepted;
    result.velocity = velocity;
    result.preFall = preFall;
    result.blocked = preFall;
    const JPH::Vec3 horizontal(displacement.GetX(),0,displacement.GetZ());
    const float desiredLength = horizontal.Length();
    if(!moveSkipped(displacement) && desiredLength>0.1f) {
      const float advancement = (accepted-initial).Dot(horizontal)/desiredLength;
      result.blocked |= advancement<=std::max(0.1f,desiredLength*0.1f);
      }
    return result;
    }

  void stepAgent(Agent& a, size_t index, size_t first, uint8_t ticks, JPH::TempAllocator& alloc, const MotionTicks& out) const {
    const size_t row = index-first;
    for(int slot=0; slot<Motion::Ticks-int(ticks); ++slot) {
      const size_t t = row*Motion::Ticks+size_t(slot);
      if(out.position) std::fill_n(out.position+3*t,3,0.f);
      if(out.forward)  std::fill_n(out.forward+3*t,3,0.f);
      if(out.limits)   std::fill_n(out.limits+3*t,3,0.f);
      if(out.ms)       out.ms[t] = 0;
      if(out.flags)    out.flags[t] = 0;
      if(out.yaw)      out.yaw[t] = 0.f;
      }
    if(ticks==0)
      return;
    decide(a);
    const float rad = a.yaw*Pi/180.f;
    const float s = std::sin(rad), co = std::cos(rad);
    // The table's forward/right are in the decision's starting frame (motion_table.targets).
    const JPH::Vec3 decision(a.tableForward*co-a.tableRight*s,0,a.tableForward*s+a.tableRight*co);
    const bool standing = a.keys[0]==0 && a.keys[1]==0;
    for(int k=0; k<int(ticks); ++k) {
      // TestBench::frameDt: 30 Hz on the game clock, 33/33/34 ms.
      uint64_t tick = a.gameMs*30/1000+1;
      while(tick*1000/30<=a.gameMs)
        ++tick;
      const uint64_t dt = tick*1000/30-a.gameMs;
      JPH::Vec3 displacement;
      if(a.state==Agent::Ground)
        displacement = decision*(float(dt)/100.f);
      else
        displacement = a.carried*(float(dt)*0.001f);
      const auto moved = advance(a,alloc,displacement,dt,a.state==Agent::Ground && standing);
      // MoveAlgo::tickCharacterVirtual: the fall guard keeps the state; otherwise
      // steep ground slides, support runs, anything else is airborne.
      if(!moved.preFall) {
        a.state = moved.steep ? Agent::Slide : moved.grounded ? Agent::Ground : Agent::Air;
        a.carried = a.state==Agent::Ground ? JPH::Vec3::sZero() : moved.velocity;
        }
      // NpcKeys::applyKeys after the movement: turn<0 adds, turn>0 subtracts.
      const float speed = params.turnDegS*(float(dt)/1000.f);
      if(a.keys[2]<0)
        a.yaw += speed;
      else if(a.keys[2]>0)
        a.yaw -= speed;
      a.gameMs += dt;
      uint8_t flags = 0;
      if(moved.grounded) flags |= Motion::Supported;
      if(moved.blocked)  flags |= Motion::Blocked;
      if(!moved.grounded && !moved.steep) flags |= Motion::Falling;
      if(moved.steep)    flags |= Motion::Steep;
      if(a.level==-2)    flags |= Motion::UnknownKeys;
      if(moved.stepped)  flags |= Motion::Stepped;
      a.flags = flags;
      const size_t t = row*Motion::Ticks+size_t(Motion::Ticks-int(ticks)+k);
      if(out.position) {
        out.position[3*t] = moved.position.GetX();
        out.position[3*t+1] = moved.position.GetY();
        out.position[3*t+2] = moved.position.GetZ();
        }
      if(out.forward) {
        const float r = a.yaw*Pi/180.f; // Npc::rotationRad
        out.forward[3*t] = std::cos(r);
        out.forward[3*t+1] = 0.f;
        out.forward[3*t+2] = std::sin(r);
        }
      if(out.limits) {
        out.limits[3*t] = params.stepCm;
        out.limits[3*t+1] = slopeDeg;
        out.limits[3*t+2] = params.bodyCm;
        }
      if(out.ms)    out.ms[t] = uint32_t(a.gameMs);
      if(out.flags) out.flags[t] = flags;
      if(out.yaw)   out.yaw[t] = a.yaw;
      }
    }
  };

Motion::Motion(const float* vertices, const uint8_t* categories, size_t triangles, size_t agents,
               size_t shards, const MotionParams& params, FloorProbe floor, Pool& pool)
  :impl(std::make_unique<Impl>()) {
  if(!(params.radiusCm>0.f) || !(params.heightCm>0.f) || !(params.stepCm>=0.f) || !(params.slideAngleDeg>=0.f) ||
     !(params.slideAngleDeg<=90.f) || !std::isfinite(params.turnDegS) || !std::isfinite(params.bodyCm) ||
     !std::isfinite(params.massKg))
    throw std::invalid_argument("invalid motion parameters");
  if(shards==0)
    throw std::invalid_argument("shards must be >= 1");
  registerJolt();
  auto& m = *impl;
  m.params = params;
  m.floor = std::move(floor);
  // VirtualCharacter::VirtualCharacter (collisionworld.cpp:1074).
  m.radius = std::max(params.radiusCm*0.01f,0.01f);
  m.height = std::max(params.heightCm*0.01f,2.f*m.radius);
  m.step = std::max(params.stepCm*0.01f,0.f);
  // MoveAlgo::slideAngle and tickCharacterVirtual's maxSlope (movealgo.cpp:735, 1187).
  const float k = float(M_PI)/180.f;
  m.maxSlope = std::acos(std::clamp(std::sin((90.f-params.slideAngleDeg)*k),0.f,1.f));
  // navdemos observe(): the NavigationMemory pose limit (navdemos.cpp:1095).
  m.slopeDeg = std::acos(std::clamp(std::cos(params.slideAngleDeg*Pi/180.f),0.f,1.f))*180.f/Pi;
  // NpcBody::groundOffset with ghostPadding = height/2 (dynamicworld.cpp:391, 421).
  m.groundOffset = params.heightCm*0.5f+params.heightCm*0.25f+15.f;
  m.capsule = new JPH::CapsuleShape(std::max(m.height*0.5f-m.radius,0.001f),m.radius);

  // One shared mesh per solid category, built in parallel; water is not solid.
  std::vector<uint8_t> solid;
  for(uint8_t c:{uint8_t(1),uint8_t(2),Kinematic})
    if(std::find(categories,categories+triangles,c)!=categories+triangles)
      solid.push_back(c);
  m.shapes.resize(solid.size());
  std::vector<std::string> errors(solid.size());
  std::vector<std::thread> builders;
  for(size_t s=0; s<solid.size(); ++s)
    builders.emplace_back([&,s] {
      try {
        JPH::TriangleList list;
        for(size_t i=0; i<triangles; ++i) {
          if(categories[i]!=solid[s])
            continue;
          const float* v = vertices+9*i;
          list.emplace_back(JPH::Float3(v[0]*0.01f,v[1]*0.01f,v[2]*0.01f),JPH::Float3(v[3]*0.01f,v[4]*0.01f,v[5]*0.01f),
                            JPH::Float3(v[6]*0.01f,v[7]*0.01f,v[8]*0.01f),0);
          }
        JPH::MeshShapeSettings mesh(list);
        auto shape = mesh.Create();
        if(shape.HasError())
          throw std::runtime_error("Jolt mesh: "+std::string(shape.GetError().c_str()));
        m.shapes[s] = {solid[s],shape.Get()};
        }
      catch(const std::exception& e) {
        errors[s] = e.what();
        }
      });
  for(auto& b:builders)
    b.join();
  for(const auto& e:errors)
    if(!e.empty())
      throw std::runtime_error(e);

  shards = std::min(shards,std::max<size_t>(agents,1));
  m.shards.resize(shards);
  std::vector<std::string> shardErrors(shards);
  pool.run(shards,[&](size_t begin, size_t end) {
    for(size_t s=begin; s<end; ++s)
      try {
        m.shards[s] = std::make_unique<Shard>(m.shapes);
        m.shards[s]->first = s*agents/shards;
        m.shards[s]->end = (s+1)*agents/shards;
        }
      catch(const std::exception& e) {
        shardErrors[s] = e.what();
        }
    });
  for(const auto& e:shardErrors)
    if(!e.empty())
      throw std::runtime_error(e);

  JPH::CharacterVirtualSettings settings;
  settings.mShape = m.capsule;
  settings.mShapeOffset = JPH::Vec3(0,m.height*0.5f,0);
  settings.mSupportingVolume = JPH::Plane(JPH::Vec3::sAxisY(),-m.radius);
  settings.mCharacterPadding = 0.01f;
  settings.mPredictiveContactDistance = 0.05f;
  settings.mCollisionTolerance = 0.001f;
  settings.mEnhancedInternalEdgeRemoval = true;
  settings.mMaxSlopeAngle = m.maxSlope;
  settings.mMass = std::max(params.massKg,1.f);
  // Only pushes dynamic bodies, which the dump does not have (the engine: 450 N cap).
  settings.mMaxStrength = std::min(settings.mMass*GravityMS,450.f);
  m.agents.resize(agents);
  for(auto& shard:m.shards)
    for(size_t i=shard->first; i<shard->end; ++i) {
      auto& a = m.agents[i];
      a.shard = shard.get();
      a.character = std::make_unique<JPH::CharacterVirtual>(&settings,JPH::RVec3::sZero(),JPH::Quat::sIdentity(),
                                                             &shard->physics);
      }
  }

Motion::~Motion() = default;

size_t Motion::shards() const {
  return impl->shards.size();
  }

void Motion::setTable(int level, size_t count, const int64_t* keys, const float* forwardRight) {
  if(level<-1 || level>3)
    throw std::invalid_argument("table level must be -1..3");
  if(count>0 && (keys==nullptr || forwardRight==nullptr))
    throw std::invalid_argument("null table arrays");
  std::vector<size_t> order(count);
  for(size_t i=0; i<count; ++i) {
    order[i] = i;
    if(!std::isfinite(forwardRight[2*i]) || !std::isfinite(forwardRight[2*i+1]))
      throw std::invalid_argument("non-finite table value");
    }
  std::sort(order.begin(),order.end(),[&](size_t a, size_t b) { return keys[a]<keys[b]; });
  Impl::Table table;
  table.keys.reserve(count);
  table.values.reserve(count);
  for(size_t i:order) {
    if(!table.keys.empty() && table.keys.back()==keys[i])
      throw std::invalid_argument("duplicate table key");
    table.keys.push_back(keys[i]);
    table.values.push_back({forwardRight[2*i],forwardRight[2*i+1]});
    }
  impl->tables[size_t(level+1)] = std::move(table);
  }

void Motion::setState(size_t first, size_t count, const float* position, const float* yawDeg,
                      const uint64_t* gameMs, const int8_t* history, const int32_t* held) {
  if(!position || !yawDeg || !gameMs)
    throw std::invalid_argument("null state array");
  for(size_t i=0; i<count; ++i) {
    for(int j=0; j<3; ++j)
      if(!std::isfinite(position[3*i+size_t(j)]))
        throw std::invalid_argument("non-finite position");
    if(!std::isfinite(yawDeg[i]))
      throw std::invalid_argument("non-finite yaw");
    if(held!=nullptr && held[i]<0)
      throw std::invalid_argument("held must be >= 0");
    if(history!=nullptr)
      for(int h=0; h<3; ++h) {
        const int8_t* k = history+12*i+4*size_t(h);
        if(k[3]>=0 && !validKeys(k))
          throw std::invalid_argument("history keys must be -1/0/+1 and walk 0/1 (walk < 0: no decision)");
        }
    }
  for(size_t i=0; i<count; ++i) {
    auto& a = impl->agents[first+i];
    impl->teleport(a,JPH::Vec3(position[3*i],position[3*i+1],position[3*i+2]));
    a.posed = true;
    a.yaw = yawDeg[i];
    a.gameMs = gameMs[i];
    a.state = Agent::Ground;
    a.carried = JPH::Vec3::sZero();
    a.flags = 0;
    a.level = -2;
    a.tableForward = a.tableRight = 0;
    for(int h=0; h<3; ++h) {
      const int8_t* k = history!=nullptr ? history+12*i+4*size_t(h) : nullptr;
      a.history[size_t(h)] = k==nullptr || k[3]<0 ? MissingCode : code({k[0],k[1],k[2],k[3]});
      }
    a.held = held!=nullptr ? held[i] : 0;
    }
  }

void Motion::setYaw(size_t first, size_t count, const float* yawDeg) {
  if(!yawDeg)
    throw std::invalid_argument("null yaw");
  for(size_t i=0; i<count; ++i)
    if(!std::isfinite(yawDeg[i]))
      throw std::invalid_argument("non-finite yaw");
  for(size_t i=0; i<count; ++i)
    impl->agents[first+i].yaw = yawDeg[i];
  }

void Motion::setKeys(size_t first, size_t count, const int8_t* keys) {
  if(!keys)
    throw std::invalid_argument("null keys");
  for(size_t i=0; i<count; ++i)
    if(!validKeys(keys+4*i))
      throw std::invalid_argument("keys must be forward/strafe/turn -1/0/+1 and walk 0/1");
  for(size_t i=0; i<count; ++i)
    std::copy_n(keys+4*i,4,impl->agents[first+i].keys.begin());
  }

void Motion::step(Pool& pool, size_t first, size_t count, const uint8_t* ticks, const MotionTicks& out) {
  if(ticks!=nullptr)
    for(size_t i=0; i<count; ++i)
      if(ticks[i]>Ticks)
        throw std::invalid_argument("ticks must be 0..3");
  auto& m = *impl;
  std::mutex failure;
  std::string message;
  const size_t end = first+count;
  pool.run(m.shards.size(),[&](size_t begin, size_t stop) {
    thread_local JPH::TempAllocatorImpl alloc(4*1024*1024);
    for(size_t s=begin; s<stop; ++s) {
      const auto& shard = *m.shards[s];
      for(size_t i=std::max(first,shard.first); i<std::min(end,shard.end); ++i) {
        auto& a = m.agents[i];
        const uint8_t n = a.posed ? (ticks!=nullptr ? ticks[i-first] : uint8_t(Ticks)) : uint8_t(0);
        try {
          m.stepAgent(a,i,first,n,alloc,out);
          }
        catch(const std::exception& e) {
          std::lock_guard<std::mutex> lock(failure);
          if(message.empty())
            message = e.what();
          }
        }
      }
    });
  if(!message.empty())
    throw std::runtime_error(message);
  }

void Motion::read(size_t first, size_t count, float* position, float* yawDeg, uint64_t* gameMs,
                  uint8_t* flags, float* tableCm, int8_t* level) const {
  for(size_t i=0; i<count; ++i) {
    const auto& a = impl->agents[first+i];
    if(position) {
      const auto p = toCentimeters(JPH::Vec3(a.character->GetPosition()));
      position[3*i] = p.GetX();
      position[3*i+1] = p.GetY();
      position[3*i+2] = p.GetZ();
      }
    if(yawDeg)  yawDeg[i] = a.yaw;
    if(gameMs)  gameMs[i] = a.gameMs;
    if(flags)   flags[i] = a.flags;
    if(tableCm) { tableCm[2*i] = a.tableForward; tableCm[2*i+1] = a.tableRight; }
    if(level)   level[i] = a.level;
    }
  }

}
