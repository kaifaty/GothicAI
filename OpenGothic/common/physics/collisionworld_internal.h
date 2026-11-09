#pragma once

#include "collisionworld.h"
#include "dynamicworld.h"
#include "physicvbo.h"
#include "ragdoll.h"

#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/PhysicsStepListener.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/EstimateCollisionResponse.h>
#include <Jolt/Physics/Collision/TransformedShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>

#include <algorithm>
#include <mutex>
#include <unordered_map>

namespace CollisionWorldInternal {
  constexpr JPH::BroadPhaseLayer staticLayer(0);
  constexpr JPH::BroadPhaseLayer dynamicLayer(1);
  constexpr JPH::BroadPhaseLayer sensorLayer(2);

  struct LayerInterface final : JPH::BroadPhaseLayerInterface {
    JPH::uint GetNumBroadPhaseLayers() const override { return 3; }
    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override {
      if(layer==DynamicWorld::C_Item || layer==DynamicWorld::C_Ragdoll || layer==DynamicWorld::C_LivingBody || layer==DynamicWorld::C_ActiveBody || layer==DynamicWorld::C_MovableVob || layer==DynamicWorld::C_Fragment)
        return dynamicLayer;
      if(layer==DynamicWorld::C_Water || layer==DynamicWorld::C_Ghost || layer==DynamicWorld::C_MovableQuery || layer==DynamicWorld::C_SmallVobQuery)
        return sensorLayer;
      if(layer==DynamicWorld::C_Cloth)
        return dynamicLayer;
      return staticLayer;
      }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const override {
      return layer==staticLayer ? "World" : layer==dynamicLayer ? "Items" : "Sensors";
      }
#endif
    };

  struct LayerPairFilter final : JPH::ObjectLayerPairFilter {
    bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override {
      if(a==DynamicWorld::C_Cloth || b==DynamicWorld::C_Cloth)
        return false;
      if(a==DynamicWorld::C_MovableQuery || a==DynamicWorld::C_SmallVobQuery ||
         b==DynamicWorld::C_MovableQuery || b==DynamicWorld::C_SmallVobQuery)
        return false;
      if(a==DynamicWorld::C_ActiveBody)
        return b==DynamicWorld::C_Landscape || b==DynamicWorld::C_Object || b==DynamicWorld::C_Item || b==DynamicWorld::C_Ragdoll || b==DynamicWorld::C_MovableVob;
      if(b==DynamicWorld::C_ActiveBody)
        return a==DynamicWorld::C_Landscape || a==DynamicWorld::C_Object || a==DynamicWorld::C_Item || a==DynamicWorld::C_Ragdoll || a==DynamicWorld::C_MovableVob;
      if(a==DynamicWorld::C_LivingBody)
        return b==DynamicWorld::C_Item || b==DynamicWorld::C_Ragdoll || b==DynamicWorld::C_MovableVob;
      if(b==DynamicWorld::C_LivingBody)
        return a==DynamicWorld::C_Item || a==DynamicWorld::C_Ragdoll || a==DynamicWorld::C_MovableVob;
      return (a==DynamicWorld::C_Item || b==DynamicWorld::C_Item ||
              a==DynamicWorld::C_Fragment || b==DynamicWorld::C_Fragment ||
              a==DynamicWorld::C_Ragdoll || b==DynamicWorld::C_Ragdoll ||
              a==DynamicWorld::C_MovableVob || b==DynamicWorld::C_MovableVob) &&
             a!=DynamicWorld::C_Null && b!=DynamicWorld::C_Null;
      }
    };

  struct BroadPhaseFilter final : JPH::ObjectVsBroadPhaseLayerFilter {
    bool ShouldCollide(JPH::ObjectLayer layer, JPH::BroadPhaseLayer broad) const override {
      if(layer==DynamicWorld::C_Cloth)
        return false;
      if(layer==DynamicWorld::C_LivingBody)
        return broad==dynamicLayer;
      if(layer==DynamicWorld::C_MovableQuery || layer==DynamicWorld::C_SmallVobQuery)
        return false;
      return layer==DynamicWorld::C_Item || layer==DynamicWorld::C_Ragdoll || layer==DynamicWorld::C_ActiveBody || layer==DynamicWorld::C_MovableVob || layer==DynamicWorld::C_Fragment ||
             (layer!=DynamicWorld::C_Null && broad==dynamicLayer);
      }
    };

  struct QueryLayerFilter final : JPH::ObjectLayerFilter {
    explicit QueryLayerFilter(uint32_t mask):mask(mask) {}
    bool ShouldCollide(JPH::ObjectLayer layer) const override {
      // Cloth has only its attachment guard; it never contacts the world or
      // becomes a solid target for gameplay rays, sweeps or character queries.
      return layer!=DynamicWorld::C_Cloth && layer<32 && (mask & (1u<<layer))!=0;
      }
    const uint32_t mask;
    };

  struct QueryBroadPhaseFilter final : JPH::BroadPhaseLayerFilter {
    explicit QueryBroadPhaseFilter(uint32_t mask):mask(mask) {}
    bool ShouldCollide(JPH::BroadPhaseLayer layer) const override {
      constexpr uint32_t itemMask = (1u<<DynamicWorld::C_Item)|(1u<<DynamicWorld::C_Ragdoll)|
                                    (1u<<DynamicWorld::C_LivingBody)|(1u<<DynamicWorld::C_ActiveBody)|(1u<<DynamicWorld::C_MovableVob);
      constexpr uint32_t sensorMask = (1u<<DynamicWorld::C_Water)|(1u<<DynamicWorld::C_Ghost)|
                                      (1u<<DynamicWorld::C_MovableQuery)|(1u<<DynamicWorld::C_SmallVobQuery);
      if(layer==dynamicLayer)
        return (mask & itemMask)!=0;
      if(layer==sensorLayer)
        return (mask & sensorMask)!=0;
      return layer==staticLayer && (mask & ~(itemMask|sensorMask))!=0;
      }
    const uint32_t mask;
    };

  CollisionWorld::NativeTriangle traceTriangle(const JPH::Body& body, JPH::SubShapeID subShape,
                                                JPH::Vec3Arg normal);
  int workerCount();
  bool waterThing(int category);
  }

using namespace CollisionWorldInternal;

struct CollisionWorld::Impl final : JPH::ContactListener, JPH::PhysicsStepListener {
  std::function<void(float)> waterStep;
  std::function<void(float)> shadowStep;
  bool waterSteps = false;
  void OnStep(const JPH::PhysicsStepListenerContext& context) override {
    if(waterSteps && waterStep) waterStep(context.mDeltaTime);
    if(shadowStep) shadowStep(context.mDeltaTime);
    }
  struct NoiseImpact {
    JPH::BodyID body,other;
    Tempest::Vec3 point;
    asset::MaterialGroup surface;
    float impulse, structuralImpulse;
    uint64_t step;
    uint64_t logicalBody;
    bool otherPushing;
    };
  struct Impact {
    JPH::BodyID item;
    asset::MaterialGroup material;
    float impulse;
    float mass;
    uint64_t step;
    uint32_t landscape;
    uint32_t subShape;
    };

  Impl() : allocator(32*1024*1024), jobs(JPH::cMaxPhysicsJobs,JPH::cMaxPhysicsBarriers,workerCount()) {
    system.Init(65536,0,65536,32768,layers,broadFilter,pairFilter);
    system.SetGravity(JPH::Vec3(0,-DynamicWorld::gravityMS,0));
    system.SetCombineFriction([](const JPH::Body& a, const JPH::SubShapeID&, const JPH::Body& b, const JPH::SubShapeID&) {
      return std::clamp(a.GetFriction()*b.GetFriction(),-10.f,10.f);
      });
    auto settings = system.GetPhysicsSettings();
    settings.mTimeBeforeSleep = 2.f;
    settings.mPointVelocitySleepThreshold = 0.8f;
    settings.mFineSlopLayers = 1u<<DynamicWorld::C_Fragment;
    settings.mFinePenetrationSlop = CollisionWorld::fragmentPenetrationSlop;
    settings.mDeepestContactLayers = 1u<<DynamicWorld::C_Item;
    system.SetPhysicsSettings(settings);
    system.SetContactListener(this);
    }

  bool guardRagdollContact(const JPH::Body& a, const JPH::Body& b) {
    if(birthGuards.empty()) return false;
    const auto living=[](const JPH::Body& body) {
      return body.GetObjectLayer()==DynamicWorld::C_LivingBody || body.GetObjectLayer()==DynamicWorld::C_ActiveBody;
      };
    const JPH::Body* ragdoll=nullptr;
    const JPH::Body* rig=nullptr;
    if(a.GetObjectLayer()==DynamicWorld::C_Ragdoll && living(b)) { ragdoll=&a;rig=&b; }
    else if(b.GetObjectLayer()==DynamicWorld::C_Ragdoll && living(a)) { ragdoll=&b;rig=&a; }
    else return false;
    const auto group=ragdoll->GetCollisionGroup().GetGroupID();
    const auto found=birthGuards.find(group);
    if(found==birthGuards.end()) return false;
    const auto& guard=found->second;
    const auto livingGroup=rig->GetCollisionGroup().GetGroupID();
    if(!guard.birth && std::find(guard.livingGroups.begin(),guard.livingGroups.end(),livingGroup)==guard.livingGroups.end())
      return false;
    std::lock_guard<std::mutex> lock(birthMutex);
    birthTouches.emplace_back(group,livingGroup);
    return true;
    }

  void finishRagdollBirthStep() {
    if(birthGuards.empty()) return;
    // Update has joined its workers. Only this thread changes the guard table;
    // callbacks append touches under birthMutex, including cached sensor contacts.
    std::sort(birthTouches.begin(),birthTouches.end());
    birthTouches.erase(std::unique(birthTouches.begin(),birthTouches.end()),birthTouches.end());
    for(const auto& touch:birthTouches) {
      auto& guard=birthGuards.at(touch.first);
      if(guard.birth) guard.livingGroups.push_back(touch.second);
      }
    for(auto it=birthGuards.begin();it!=birthGuards.end();) {
      auto& guard=it->second;
      guard.birth=false;
      // Sleeping parts stop reporting contacts. Retain protection until at
      // least one part is active and can confirm that the rig has separated.
      if(!guard.body->isSleeping()) {
        auto& groups=guard.livingGroups;
        groups.erase(std::remove_if(groups.begin(),groups.end(),[&](uint32_t group) {
          return !std::binary_search(birthTouches.begin(),birthTouches.end(),std::make_pair(it->first,group));
          }),groups.end());
        }
      if(guard.livingGroups.empty()) it=birthGuards.erase(it); else ++it;
      }
    birthTouches.clear();
    }

  void onContact(const JPH::Body& a, const JPH::Body& b, const JPH::ContactManifold& manifold,
                 JPH::ContactSettings& settings, bool persisted) {
    if(guardRagdollContact(a,b)) {
      settings.mIsSensor=true;
      return;
      }
    if(emitBodyImpacts && !a.IsSensor() && !b.IsSensor()) {
      const auto accepts=[](const JPH::Body& body) {
        const auto layer=body.GetObjectLayer();
        return layer==DynamicWorld::C_Landscape || layer==DynamicWorld::C_Object || layer==DynamicWorld::C_MovableVob;
        };
      const bool first=a.GetObjectLayer()==DynamicWorld::C_Ragdoll && accepts(b);
      const bool second=b.GetObjectLayer()==DynamicWorld::C_Ragdoll && accepts(a);
      if(first || second) {
        float speed=0;
        for(JPH::uint i=0;i<manifold.mRelativeContactPointsOn1.size();++i) {
          const auto point=manifold.GetWorldSpaceContactPointOn1(i);
          speed=std::max(speed,(a.GetPointVelocity(point)-b.GetPointVelocity(point)).Dot(manifold.mWorldSpaceNormal));
          }
        if(speed>0.f) {
          std::lock_guard<std::mutex> guard(impactMutex);
          bodyImpacts.push_back({first ? a.GetID() : b.GetID(),speed*.1f});
          }
        }
      }
    if(const auto* target=solverTraceTarget.load(std::memory_order_relaxed)) {
      const auto traced=[&](const JPH::Body& body) {
        if(body.GetObjectLayer()!=DynamicWorld::C_Item)return false;
        const auto* wrapper=reinterpret_cast<const CollisionBody*>(body.GetUserData());
        return wrapper!=nullptr && wrapper->ptrType()==DynamicWorld::P_Item && wrapper->payload()==target;
      };
      const bool first=traced(a),second=traced(b);
      if(first || second) {
        const auto& item=first?a:b;
        const auto& other=first?b:a;
        const auto subshape=first?manifold.mSubShapeID2:manifold.mSubShapeID1;
        for(JPH::uint i=0;i<manifold.mRelativeContactPointsOn1.size();++i) {
          ItemSolverContact entry;
          entry.nativeStep=simulationStep;
          entry.itemBodyId=item.GetID().GetIndexAndSequenceNumber();
          entry.bodyId=other.GetID().GetIndexAndSequenceNumber();
          entry.persisted=persisted;
          auto& contact=entry.contact;
          const auto layer=other.GetObjectLayer();
          if(layer!=DynamicWorld::C_Ragdoll && layer!=DynamicWorld::C_ActiveBody && layer!=DynamicWorld::C_LivingBody)
            contact.body=reinterpret_cast<CollisionBody*>(other.GetUserData());
          contact.category=int(layer);
          contact.depth=manifold.mPenetrationDepth*100.f;
          const auto outward=first?manifold.mWorldSpaceNormal:-manifold.mWorldSpaceNormal;
          contact.axis={outward.GetX(),outward.GetY(),outward.GetZ()};
          const auto point=first?manifold.GetWorldSpaceContactPointOn1(i):manifold.GetWorldSpaceContactPointOn2(i);
          const auto otherPoint=first?manifold.GetWorldSpaceContactPointOn2(i):manifold.GetWorldSpaceContactPointOn1(i);
          contact.point1=toCentimeters(point);
          contact.point2=toCentimeters(otherPoint);
          contact.triangle=traceTriangle(other,subshape,other.GetWorldSpaceSurfaceNormal(subshape,otherPoint));
          std::lock_guard<std::mutex> guard(impactMutex);
          solverContacts.push_back(std::move(entry));
        }
      }
    }
    auto markNpcContact = [](const JPH::Body& movable, const JPH::Body& other) {
      if(movable.GetObjectLayer()!=DynamicWorld::C_MovableVob ||
         (other.GetObjectLayer()!=DynamicWorld::C_LivingBody && other.GetObjectLayer()!=DynamicWorld::C_ActiveBody))
        return;
      auto* body = reinterpret_cast<CollisionBody*>(movable.GetUserData());
      if(body!=nullptr)
        body->npcContact.store(true,std::memory_order_relaxed);
      };
    markNpcContact(a,b);
    markNpcContact(b,a);
    if(a.GetObjectLayer()==DynamicWorld::C_MovableVob && b.GetObjectLayer()==DynamicWorld::C_MovableVob) {
      auto* first = reinterpret_cast<CollisionBody*>(a.GetUserData());
      auto* second = reinterpret_cast<CollisionBody*>(b.GetUserData());
      if(first!=nullptr && second!=nullptr && (first->touchedByNpc() || second->touchedByNpc())) {
        first->npcContact.store(true,std::memory_order_relaxed);
        second->npcContact.store(true,std::memory_order_relaxed);
        }
      }
    // Movable traces need the same push attribution even when audible noise
    // is disabled. Resolve identities after the native workers have finished.
    if((emitNoise || a.GetObjectLayer()==DynamicWorld::C_MovableVob || b.GetObjectLayer()==DynamicWorld::C_MovableVob) &&
       !manifold.mRelativeContactPointsOn1.empty() && !a.IsSensor() && !b.IsSensor()) {
      auto source=[this](const JPH::Body& body) {
        if(!emitNoise && body.GetObjectLayer()!=DynamicWorld::C_MovableVob) return false;
        return body.IsDynamic() && (body.GetObjectLayer()==DynamicWorld::C_Item ||
               body.GetObjectLayer()==DynamicWorld::C_MovableVob || body.GetObjectLayer()==DynamicWorld::C_Ragdoll ||
               body.GetObjectLayer()==DynamicWorld::C_ActiveBody);
        };
      const bool first=source(a),second=source(b);
      if(first || second) {
        const auto point=manifold.GetWorldSpaceContactPointOn1(0);
        const auto firstVelocity=a.GetPointVelocity(point),secondVelocity=b.GetPointVelocity(point);
        const float speed=(firstVelocity-secondVelocity).Dot(manifold.mWorldSpaceNormal);
        const bool firstNpc=b.GetObjectLayer()==DynamicWorld::C_LivingBody || b.GetObjectLayer()==DynamicWorld::C_ActiveBody ||
                            b.GetObjectLayer()==DynamicWorld::C_Ragdoll;
        const bool secondNpc=a.GetObjectLayer()==DynamicWorld::C_LivingBody || a.GetObjectLayer()==DynamicWorld::C_ActiveBody ||
                             a.GetObjectLayer()==DynamicWorld::C_Ragdoll;
        const bool firstPushed=first && firstNpc && secondVelocity.Dot(manifold.mWorldSpaceNormal)<-.05f;
        const bool secondPushed=second && secondNpc && firstVelocity.Dot(manifold.mWorldSpaceNormal)>.05f;
        // Resting contacts do not need the native impulse solver. Slow contact
        // with an NPC rig still retains who pushed a body before its fall.
        if(speed>=.9f || firstPushed || secondPushed) {
          float impulse=0.f,structuralImpulse=0.f;
          if(speed>=.9f) {
            JPH::CollisionEstimationResult response;
            JPH::EstimateCollisionResponse(a,b,manifold,response,settings.mCombinedFriction,settings.mCombinedRestitution);
            for(float contact:response.mContactImpulse) impulse+=contact;
            // A slightly tilted floor first contacts one corner. The native
            // manifold response then spends part of its impulse on rotation;
            // it is only a fraction of the whole impact before later corners
            // arrive. Structural damage uses the incoming normal momentum of
            // the collision pair, independent of how triangles split contacts.
            // A static/kinematic support contributes no inverse mass; dynamic
            // impactors contribute their finite mass through the reduced mass.
            const float inverseMass=(a.IsDynamic() ? a.GetMotionProperties()->GetInverseMass() : 0.f)+
                                    (b.IsDynamic() ? b.GetMotionProperties()->GetInverseMass() : 0.f);
            if(inverseMass>0.f) structuralImpulse=speed/inverseMass;
            }
          auto surface=[](const JPH::Body& body,JPH::SubShapeID subShape) {
            const auto layer=body.GetObjectLayer();
            if(layer==DynamicWorld::C_Ragdoll || layer==DynamicWorld::C_LivingBody || layer==DynamicWorld::C_ActiveBody)
              return asset::MaterialGroup::UNDEFINED;
            auto* obj=reinterpret_cast<CollisionBody*>(body.GetUserData());
            if(obj==nullptr) return asset::MaterialGroup::STONE;
            if(obj->mesh!=nullptr)
              return obj->mesh->materialId(Physics::triangleData(*body.GetShape(),subShape));
            return obj->noiseSurface;
            };
          auto identity=[](const JPH::Body& body) -> uint64_t {
            const auto layer=body.GetObjectLayer();
            if(layer==DynamicWorld::C_Ragdoll || layer==DynamicWorld::C_ActiveBody)
              return (uint64_t(1)<<63)|body.GetCollisionGroup().GetGroupID();
            return body.GetID().GetIndexAndSequenceNumber();
            };
          std::lock_guard<std::mutex> guard(impactMutex);
          if(first && (impulse>=.08f || firstPushed))
            noises.push_back({a.GetID(),b.GetID(),toCentimeters(point),surface(b,manifold.mSubShapeID2),impulse,structuralImpulse,simulationStep,identity(a),
                              firstPushed});
          if(second && (impulse>=.08f || secondPushed))
            noises.push_back({b.GetID(),a.GetID(),toCentimeters(point),surface(a,manifold.mSubShapeID1),impulse,structuralImpulse,simulationStep,identity(b),
                              secondPushed});
          }
        }
      }
    if(!emitImpacts)
      return;
    const JPH::Body* item = nullptr;
    const JPH::Body* land = nullptr;
    JPH::SubShapeID landSubShape;
    if(a.GetObjectLayer()==DynamicWorld::C_Item && b.GetObjectLayer()==DynamicWorld::C_Landscape) {
      item = &a;
      land = &b;
      landSubShape = manifold.mSubShapeID2;
      }
    else if(b.GetObjectLayer()==DynamicWorld::C_Item && a.GetObjectLayer()==DynamicWorld::C_Landscape) {
      item = &b;
      land = &a;
      landSubShape = manifold.mSubShapeID1;
      }
    if(item==nullptr || manifold.mRelativeContactPointsOn1.empty())
      return;
    auto* obj = reinterpret_cast<DynamicBody*>(item->GetUserData());
    auto* terrain = reinterpret_cast<CollisionBody*>(land->GetUserData());
    if(obj==nullptr || obj->pointer==nullptr || obj->pointerType!=DynamicWorld::P_Item)
      return;
    JPH::CollisionEstimationResult response;
    JPH::EstimateCollisionResponse(a,b,manifold,response,settings.mCombinedFriction,settings.mCombinedRestitution);
    if(response.mContactImpulse.empty())
      return;
    const float impulse = response.mContactImpulse[0];
    if(impulse/obj->bodyMass<0.9f)
      return;
    auto material = asset::MaterialGroup::STONE;
    if(terrain!=nullptr && terrain->mesh!=nullptr)
      material = terrain->mesh->materialId(Physics::triangleData(*land->GetShape(),landSubShape));
    std::lock_guard<std::mutex> guard(impactMutex);
    impacts.push_back({item->GetID(),material,impulse,obj->bodyMass,simulationStep,land->GetID().GetIndexAndSequenceNumber(),landSubShape.GetValue()});
    }

  void OnContactAdded(const JPH::Body& a, const JPH::Body& b, const JPH::ContactManifold& manifold,
                      JPH::ContactSettings& settings) override {
    onContact(a,b,manifold,settings,false);
    }
  void OnContactPersisted(const JPH::Body& a, const JPH::Body& b, const JPH::ContactManifold& manifold,
                          JPH::ContactSettings& settings) override {
    onContact(a,b,manifold,settings,true);
    }

  LayerInterface layers;
  BroadPhaseFilter broadFilter;
  LayerPairFilter pairFilter;
  JPH::PhysicsSystem system;
  JPH::TempAllocatorImpl allocator;
  JPH::JobSystemThreadPool jobs;
  std::mutex impactMutex;
  struct BirthGuard {
    Ragdoll* body;
    bool birth=true;
    std::vector<uint32_t> livingGroups;
    };
  bool birthGuardEnabled=true;
  std::unordered_map<uint32_t,BirthGuard> birthGuards;
  std::mutex birthMutex;
  std::vector<std::pair<uint32_t,uint32_t>> birthTouches;
  std::atomic<const Item*> solverTraceTarget {nullptr};
  std::vector<ItemSolverContact> solverContacts;
  struct BodyImpact { JPH::BodyID body;float speed; };
  std::vector<BodyImpact> bodyImpacts;
  bool emitBodyImpacts=true;
  std::vector<Impact> impacts;
  std::vector<NoiseImpact> noises;
  uint64_t simulationStep = 0;
  bool emitImpacts = false;
  bool noiseEnabled = true,emitNoise = false;
  std::atomic<uint64_t> rayCount {0};
  uint64_t lastRayCount = 0;
  float stepMilliseconds = 0;
  float characterMilliseconds = 0;
  std::vector<VirtualCharacter*> virtualCharacters;
  struct MeleeBlade {
    const Npc* owner=nullptr;
    JPH::BodyID body;
    JPH::RefConst<JPH::CapsuleShape> shape;
    JPH::RVec3 position=JPH::RVec3::sZero(),targetPosition=JPH::RVec3::sZero();
    JPH::Quat rotation=JPH::Quat::sIdentity(),targetRotation=JPH::Quat::sIdentity();
    };
  std::vector<MeleeBlade> meleeBlades;
  std::mutex meleeMutex;
  struct ShadowBody {
    Npc* owner = nullptr;
    std::vector<MeleeBodyCapsule> capsules;
    JPH::AABox bounds;
    uint64_t serial = 0; // MeleeShadowBody::serial of the capsules held
    };
  struct ShadowBlade {
    const Npc* owner = nullptr;
    uint64_t swing = 0;
    bool blocked = false, groundRecorded = false;
    float walkableCos = .7f;
    double poseTimeMs = 0, targetTimeMs = 0, activeBeginMs = 0, activeEndMs = 0;
    Tempest::Vec3 forward;
    MeleeImpact::Weapon weapon=MeleeImpact::Weapon::OneHand;
    std::vector<MeleeBladeSegment> pose, target;
    std::vector<Npc*> hit;
    std::vector<JPH::BodyID> hitItems;
    std::vector<MeleeBladeContact> contacts;
    };
  std::vector<ShadowBody> shadowBodies;
  std::unordered_map<const Npc*,size_t> shadowBodySlots; // owner: its index in shadowBodies
  std::vector<ShadowBlade> shadowBlades;
  std::mutex shadowMutex;
  std::atomic<bool> hasShadowBlades{false};
  int meleeSubsteps=0;
  struct MeleeSample {
    uint64_t step=0;
    float milliseconds=0,solveMs=0,castMs=0;
    size_t updates=0,casts=0,bodyHits=0,worldHits=0,parts=0,blades=0,floorSamples=0,floorMisses=0;
    float linear=0,angular=0,poseRms=0,poseMax=0,footDrop=0,footPenetration=0;
    bool finite=true;
    };
  MeleeSample meleeSample;
  std::vector<MeleeSample> meleeSamples;
  struct MeleeStep { uint64_t step;float milliseconds,castMs;int substeps; };
  std::vector<MeleeStep> meleeSteps;
  std::string meleeOutput;
  float meleeDiagnosticMs=0;
  };
