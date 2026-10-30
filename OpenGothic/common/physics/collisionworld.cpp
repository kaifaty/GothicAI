#include "collisionworld.h"
#include "physicvbo.h"
#include "dynamicworld.h"
#include "ragdoll.h"
#include "cloth.h"
#include "frameprofile.h"
#include "world/objects/item.h"
#include "world/objects/npc.h"
#include "world/world.h"
#include <Tempest/Log>

#include <cstdlib>

#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Geometry/GJKClosestPoint.h>
#include <Jolt/Geometry/Plane.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/PhysicsStepListener.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Body/BodyFilter.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/CollisionDispatch.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/EstimateCollisionResponse.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <Jolt/Physics/Collision/ShapeFilter.h>
#include <Jolt/Physics/Collision/TransformedShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <limits>
#include <iterator>
#include <stdexcept>
#include <thread>
#include <tuple>
#ifdef __APPLE__
#include <sys/types.h>
#include <sys/sysctl.h>
#endif

namespace {
  bool waterThing(int category) {
    return category==DynamicWorld::C_Item || category==DynamicWorld::C_MovableVob || category==DynamicWorld::C_Fragment;
    }

  JPH::AABox itemCollisionBounds(const JPH::Body& body) {
    auto leaf=body.GetTransformedShape();
    JPH::SubShapeID remainder;
    while(leaf.mShape->GetType()==JPH::EShapeType::Decorated)
      leaf=leaf.GetSubShapeTransformedShape(JPH::SubShapeID(),remainder);
    if(leaf.mShape->GetType()!=JPH::EShapeType::Convex)
      throw std::runtime_error("Item collision bounds require a convex shape");
    const auto& shape=static_cast<const JPH::ConvexShape&>(*leaf.mShape);
    JPH::ConvexShape::SupportBuffer buffer;
    const auto* support=shape.GetSupportFunction(JPH::ConvexShape::ESupportMode::Default,buffer,leaf.GetShapeScale());
    const float radius=support->GetConvexRadius();
    JPH::AABox bounds;
    for(JPH::uint axis=0;axis<3;++axis) {
      JPH::Vec3 worldDirection=JPH::Vec3::sZero();
      worldDirection.SetComponent(axis,1.f);
      const auto localDirection=leaf.mShapeRotation.Conjugated()*worldDirection;
      const auto min=leaf.mShapePositionCOM+leaf.mShapeRotation*support->GetSupport(-localDirection);
      const auto max=leaf.mShapePositionCOM+leaf.mShapeRotation*support->GetSupport(localDirection);
      bounds.mMin.SetComponent(axis,float(min[axis])-radius);
      bounds.mMax.SetComponent(axis,float(max[axis])+radius);
      }
    return bounds;
    }

  void submergedVolume(const JPH::Body& body, const JPH::Shape& shape, float waterY,
                       float& total, float& submerged, JPH::Vec3& center) {
    const auto relative=JPH::RVec3(0,waterY,0)-body.GetCenterOfMassPosition();
    const auto plane=JPH::Plane::sFromPointAndNormal(JPH::Vec3(relative),JPH::Vec3::sAxisY());
    shape.GetSubmergedVolume(JPH::Mat44::sRotation(body.GetRotation()),JPH::Vec3::sOne(),
                             plane,total,submerged,center JPH_IF_DEBUG_RENDERER(,body.GetCenterOfMassPosition()));
    }

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

  // Use simulated hulls, including small props. The movable query meshes are
  // duplicate sensor geometry used by the legacy controller and aiming only.
  constexpr uint32_t characterMask = (1u<<DynamicWorld::C_Landscape)|(1u<<DynamicWorld::C_Object)|
                                     (1u<<DynamicWorld::C_Item)|(1u<<DynamicWorld::C_MovableVob)|
                                     (1u<<DynamicWorld::C_Ragdoll);

  float angularMotionDisc(const JPH::Shape& shape) {
    const auto bounds = shape.GetLocalBounds();
    // Jolt's bounds are centered on the center of mass. The previous query
    // measured the bounding sphere from the original shape's local origin.
    return bounds.GetExtent().Length()+(bounds.GetCenter()+shape.GetCenterOfMass()).Length();
    }

  bool npcContactCandidate(const JPH::Shape& shape, JPH::Vec3Arg center, const JPH::Body& body,
                           JPH::Vec3Arg scale, const JPH::Vec3 (&vertices)[3], float contactDistance, JPH::Vec3* outNormal=nullptr) {
    if(shape.GetSubType()!=JPH::EShapeSubType::Capsule && shape.GetSubType()!=JPH::EShapeSubType::Sphere)
      return true;
    struct CapsuleCore {
      JPH::Vec3 center;
      float halfHeight;
      JPH::Vec3 GetSupport(JPH::Vec3Arg direction) const {
        // Keep the first endpoint on ties, including the zero direction.
        const bool upper = direction.GetY()>=0.f || direction.LengthSq()<std::numeric_limits<float>::epsilon()*std::numeric_limits<float>::epsilon();
        return center+JPH::Vec3(0,upper ? halfHeight : -halfHeight,0);
        }
      };
    struct TriangleCore {
      JPH::Vec3 vertices[3];
      JPH::Vec3 GetSupport(JPH::Vec3Arg direction) const {
        // The original mesh's support function kept its first vertex on ties.
        size_t index = 0;
        float best = vertices[0].Dot(direction);
        for(size_t i=1;i<3;++i) {
          const float projection = vertices[i].Dot(direction);
          if(projection>best) {
            index = i;
            best = projection;
            }
          }
        return vertices[index];
        }
      };
    const auto bodyPosition = JPH::Vec3(body.GetPosition());
    const auto offset = (center+bodyPosition)*0.5f;
    const float halfHeight = shape.GetSubType()==JPH::EShapeSubType::Capsule ?
                              static_cast<const JPH::CapsuleShape&>(shape).GetHalfHeightOfCylinder() : 0.f;
    const CapsuleCore capsule{center-offset,halfHeight};
    TriangleCore triangle;
    const auto transform = body.GetWorldTransform();
    const auto translation = bodyPosition-offset;
    for(size_t i=0;i<3;++i)
      triangle.vertices[i] = transform.Multiply3x3(scale*vertices[i])+translation;
    JPH::GJKClosestPoint gjk;
    auto direction = -JPH::Vec3::sAxisY();
    JPH::Vec3 pointA,pointB;
    // Scalar witness interpolation retains small components used by the old
    // movement query to choose a capsule support endpoint and its distance bound.
    const float distanceSq = gjk.GetClosestPoints<CapsuleCore,TriangleCore,true>(capsule,triangle,0.001f,contactDistance*contactDistance,direction,pointA,pointB);
    // The old query forwarded the previous simplex even when its conservative
    // core-distance bound stopped GJK early. An exact closest-distance query
    // would accept additional contacts and stop NPCs a frame before that query.
    if(distanceSq==std::numeric_limits<float>::max()) {
      JPH::Vec3 simplex[4],pointsA[4],pointsB[4];
      JPH::uint count = 0;
      gjk.GetClosestPointsSimplex(simplex,pointsA,pointsB,count);
      if(count==0)
        return false;
      }
    const float distance = (distanceSq==std::numeric_limits<float>::max() ? direction.Length() : std::sqrt(distanceSq))-shape.GetInnerRadius();
    const bool accepted = distance<0.f || distance*distance<contactDistance*contactDistance;
    // The controller also consumed the previous conservative witness direction
    // for separated contacts. It need not be the exact closest-surface normal.
    if(accepted && distance>=0.f && outNormal!=nullptr) {
      const auto axis = -direction;
      const float lengthSq = (axis.GetX()*axis.GetX()+axis.GetY()*axis.GetY())+axis.GetZ()*axis.GetZ();
      if(lengthSq>0.f && std::isfinite(lengthSq)) {
        const float inverseLength = 1.f/std::sqrt(lengthSq);
        *outNormal = JPH::Vec3(axis.GetX()*inverseLength,axis.GetY()*inverseLength,axis.GetZ()*inverseLength);
        }
      }
    return accepted;
    }

  struct SweepCollector final : JPH::CastShapeCollector {
    explicit SweepCollector(JPH::Vec3Arg direction):direction(direction) {}
    void AddHit(const JPH::ShapeCastResult& result) override {
      if(result.mFraction>=1.f)
        return;
      // The old conservative advancement rejected contacts whose separation grows.
      if(result.mFraction==0.f && direction.Dot(result.mPenetrationAxis)<=std::numeric_limits<float>::epsilon())
        return;
      if(!hadHit || result.mFraction<=hit.mFraction) {
        hit = result;
        hadHit = true;
        UpdateEarlyOutFraction(result.mFraction);
        }
      }
    bool HadHit() const { return hadHit; }
    const JPH::Vec3 direction;
    JPH::ShapeCastResult hit;
    bool hadHit = false;
    };

  struct Transform {
    JPH::Vec3 position;
    JPH::Quat rotation;
    JPH::Vec3 scale;
    };

  Transform toTransform(const Tempest::Matrix4x4& matrix) {
    const auto* m = matrix.data();
    auto x = JPH::Vec3(m[0],m[1],m[2]);
    auto y = JPH::Vec3(m[4],m[5],m[6]);
    auto z = JPH::Vec3(m[8],m[9],m[10]);
    auto s = JPH::Vec3(std::max(x.Length(),0.00001f), std::max(y.Length(),0.00001f), std::max(z.Length(),0.00001f));
    if(x.Cross(y).Dot(z)<0.f)
      s.SetX(-s.GetX());
    for(JPH::uint i=0;i<3;++i)
      if(std::abs(s[i]-1.f)<0.00001f)
        s.SetComponent(i,1.f);
    auto rotation = JPH::Mat44(JPH::Vec4(x/s.GetX(),0), JPH::Vec4(y/s.GetY(),0),
                              JPH::Vec4(z/s.GetZ(),0), JPH::Vec3::sZero()).GetQuaternion().Normalized();
    return {JPH::Vec3(m[12]*0.01f,m[13]*0.01f,m[14]*0.01f),rotation,s};
    }

  JPH::ShapeRefC scaledShape(const JPH::Shape* shape, JPH::Vec3Arg scale) {
    if(scale==JPH::Vec3::sOne())
      return shape;
    auto result = shape->ScaleShape(scale);
    if(result.HasError())
      throw std::runtime_error("Jolt shape scale: " + std::string(result.GetError().c_str()));
    return result.Get();
    }

  int workerCount() {
    // OPENGOTHIC_PHYSICS_THREADS: Jolt's worker threads; 0 runs every physics job on the stepping
    // thread. The headless body runner forks its loaded world and needs a single-threaded process.
    if(const char* env = std::getenv("OPENGOTHIC_PHYSICS_THREADS"); env!=nullptr && *env!='\0')
      return std::max(0,std::atoi(env));
    unsigned int cores = std::max(1u,std::thread::hardware_concurrency());
#ifdef __APPLE__
    unsigned int performanceCores = 0;
    size_t size = sizeof(performanceCores);
    if(sysctlbyname("hw.perflevel0.physicalcpu",&performanceCores,&size,nullptr,0)==0 && performanceCores>0)
      cores = performanceCores;
#endif
    return int(cores>2 ? cores-2 : 1);
    }
  }

void Physics::initialize() {
  struct Registration {
    Registration() {
      JPH::RegisterDefaultAllocator();
      JPH::Factory::sInstance = new JPH::Factory();
      JPH::RegisterTypes();
      }
    ~Registration() {
      JPH::UnregisterTypes();
      delete JPH::Factory::sInstance;
      JPH::Factory::sInstance = nullptr;
      }
    };
  static Registration registration;
  }

uint32_t Physics::triangleData(const JPH::Shape& shape, JPH::SubShapeID subShape) {
  JPH::SubShapeID remainder;
  auto* leaf = shape.GetLeafShape(subShape,remainder);
  if(leaf!=nullptr && leaf->GetSubType()==JPH::EShapeSubType::Mesh)
    return static_cast<const JPH::MeshShape*>(leaf)->GetTriangleUserData(remainder);
  return 0;
  }

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
    system.SetPhysicsSettings(settings);
    system.SetContactListener(this);
    }

  void onContact(const JPH::Body& a, const JPH::Body& b, const JPH::ContactManifold& manifold,
                 const JPH::ContactSettings& settings) {
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
    onContact(a,b,manifold,settings);
    }
  void OnContactPersisted(const JPH::Body& a, const JPH::Body& b, const JPH::ContactManifold& manifold,
                          JPH::ContactSettings& settings) override {
    onContact(a,b,manifold,settings);
    }

  LayerInterface layers;
  BroadPhaseFilter broadFilter;
  LayerPairFilter pairFilter;
  JPH::PhysicsSystem system;
  JPH::TempAllocatorImpl allocator;
  JPH::JobSystemThreadPool jobs;
  std::mutex impactMutex;
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
    };
  struct ShadowBlade {
    const Npc* owner = nullptr;
    uint64_t swing = 0;
    bool blocked = false, groundRecorded = false;
    float walkableCos = .7f;
    double poseTimeMs = 0, targetTimeMs = 0, activeBeginMs = 0, activeEndMs = 0;
    Tempest::Vec3 forward;
    std::vector<MeleeBladeSegment> pose, target;
    std::vector<Npc*> hit;
    std::vector<MeleeBladeContact> contacts;
    };
  std::vector<ShadowBody> shadowBodies;
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

struct CollisionWorld::VirtualCharacter::Impl final : JPH::CharacterContactListener {
  explicit Impl(CollisionWorld& world):world(world) {}

  static bool isPushable(const JPH::Body& body) {
    const auto layer = body.GetObjectLayer();
    return body.IsDynamic() && (layer==DynamicWorld::C_Item || layer==DynamicWorld::C_MovableVob);
    }

  static bool blocksAutomaticStairs(const JPH::Body& body) {
    if(isPushable(body))
      return true;
    // C_Object bodies are CollisionBody wrappers, unlike native ragdoll parts.
    // Interactive furniture/containers are not architectural treads. Doors
    // retain their blocking/perception path and ladders use explicit climbing.
    if(body.GetObjectLayer()!=DynamicWorld::C_Object)
      return false;
    const auto* source = reinterpret_cast<const CollisionBody*>(body.GetUserData());
    return source!=nullptr && source->ptrType()==DynamicWorld::P_Interactive;
    }

  bool rejectsSupport(const JPH::Body& body, const JPH::CharacterContact& contact) const {
    if(!isPushable(body) || contact.mIsSensorB)
      return false;
    const auto up = character->GetUp();
    const float mass = 1.f/body.GetMotionProperties()->GetInverseMass();
    if(carrying && body.GetID()==ground && mass>=10.f)
      return false; // This top was accepted before the supporting body advanced.
    const float contactHeight = JPH::Vec3(contact.mPosition-character->GetPosition()).Dot(up);
    return mass<10.f ? contact.mSurfaceNormal.Dot(up)>1.e-3f :
                      contact.mSurfaceNormal.Dot(up)>.99f && contactHeight>.02f;
    }

  JPH::Vec3 finiteGroundNormal(const char* phase) {
    const auto normal=character->GetGroundNormal();
    const float x=normal.GetX(),y=normal.GetY(),z=normal.GetZ();
    const bool finite=std::isfinite(x) && std::isfinite(y) && std::isfinite(z);
    const double squared=double(x)*x+double(y)*y+double(z)*z;
    if(finite && squared>1.e-12) {
      lastGroundNormal=normal;
      return normal; // Keep every valid native normal bit-for-bit, without normalization.
    }
    ++groundNormalFallbacks;
    // A zero normal while airborne is Jolt's ordinary no-contact sentinel. Log
    // anomalous support only; all fallback reads still count in diagnostics.
    if(!finite || character->IsSupported()) {
      const auto position=toCentimeters(JPH::Vec3(character->GetPosition()));
      std::fprintf(stderr,"CV_GROUND_NORMAL_FALLBACK phase=%s count=%llu tick_ms=%llu body=%u subshape=%u state=%u "
                          "position_cm=(%.9g,%.9g,%.9g) raw=(%.9g,%.9g,%.9g) fallback=(%.9g,%.9g,%.9g) contacts=%zu\n",
                   phase,static_cast<unsigned long long>(groundNormalFallbacks),
                   static_cast<unsigned long long>(culprit!=nullptr?culprit->world().tickCount():0),
                   character->GetGroundBodyID().GetIndexAndSequenceNumber(),character->GetGroundSubShapeID().GetValue(),
                   unsigned(character->GetGroundState()),double(position.x),double(position.y),double(position.z),
                   double(x),double(y),double(z),double(lastGroundNormal.GetX()),double(lastGroundNormal.GetY()),
                   double(lastGroundNormal.GetZ()),static_cast<size_t>(character->GetActiveContacts().size()));
    }
    return lastGroundNormal;
    }

  JPH::Vec3 initialIdleGravity(JPH::Vec3Arg gravity) {
    // A tangent pose can have tiny negative distances from floating-point
    // contact calculations. Anything beyond one percent of the solver's
    // collision tolerance still needs the ordinary initial recovery solve.
    const float numericalPenetration = .01f*character->GetCharacterVirtualSettings().mCollisionTolerance;
    for(const auto& contact:character->GetActiveContacts())
      if(!contact.mWasDiscarded && !contact.mIsSensorB && contact.mDistance < -numericalPenetration)
        return gravity;

    const auto groundNormal=finiteGroundNormal("initial_idle");
    const auto id = character->GetGroundBodyID();
    JPH::BodyLockRead lock(world.impl->system.GetBodyLockInterface(),id);
    if(!lock.SucceededAndIsInBroadPhase() || !lock.GetBody().IsStatic())
      return gravity;
    const auto& body = lock.GetBody();
    const auto subShape = character->GetGroundSubShapeID();
    JPH::SubShapeID remainder;
    const auto* leaf = body.GetShape()->GetLeafShape(subShape,remainder);
    if(leaf==nullptr || (leaf->GetSubType()!=JPH::EShapeSubType::Mesh && leaf->GetSubType()!=JPH::EShapeSubType::Box))
      return gravity;
    // Keep the full subshape ID here: compound and decorated shapes must
    // resolve their leaf's face in the body's world transform.
    const auto face = body.GetWorldSpaceSurfaceNormal(subShape,character->GetGroundPosition());
    constexpr float cosTwoDegrees = .999390827f;
    if(character->IsSlopeTooSteep(face) || face.Dot(groundNormal)<cosTwoDegrees)
      return gravity;
    for(const auto& contact:character->GetActiveContacts()) {
      if(contact.mBodyB!=id || contact.mSubShapeIDB!=subShape || !contact.mHadCollision ||
         contact.mWasDiscarded || contact.mIsSensorB)
        continue;
      // A rounded toe on a tread edge is not an already acquired plane.
      if(face.Dot(contact.mContactNormal)>=cosTwoDegrees && face.Dot(contact.mSurfaceNormal)>=cosTwoDegrees)
        return face*gravity.Dot(face);
      }
    return gravity;
    }

  CharacterVirtualDiagnostics::Body diagnosticBody(JPH::BodyID id, JPH::RVec3Arg point) const {
    CharacterVirtualDiagnostics::Body result;
    result.id = id.GetIndexAndSequenceNumber();
    if(id.IsInvalid())
      return result;
    JPH::BodyLockRead lock(world.impl->system.GetBodyLockInterface(),id);
    if(!lock.SucceededAndIsInBroadPhase())
      return result;
    const auto& body = lock.GetBody();
    result.present = true;
    result.layer = body.GetObjectLayer();
    result.motionType = int(body.GetMotionType());
    result.nativeUserData = uintptr_t(body.GetUserData());
    result.mass = body.IsDynamic() ? 1.f/body.GetMotionProperties()->GetInverseMass() : 0.f;
    result.position = toCentimeters(JPH::Vec3(body.GetPosition()));
    result.linearVelocity = toCentimeters(body.GetLinearVelocity());
    const auto angular = body.GetAngularVelocity();
    result.angularVelocity = Tempest::Vec3(angular.GetX(),angular.GetY(),angular.GetZ());
    result.pointVelocity = toCentimeters(body.GetPointVelocity(point));
    // Ragdoll / living-body user data is not owned by CollisionBody. Resolve
    // metadata through this world's actual wrappers rather than casting it.
    const auto sourceIt = std::find_if(world.bodies.begin(),world.bodies.end(),
                                      [id](const auto* candidate) { return candidate->id==id; });
    if(sourceIt!=world.bodies.end()) {
      const auto* source = *sourceIt;
      result.pointerType = source->ptrType();
      result.payload = source->payload();
      result.label = source->label();
      }
    return result;
    }

  CharacterVirtualDiagnostics::Pose diagnosticPose() {
    CharacterVirtualDiagnostics::Pose result;
    result.position = toCentimeters(JPH::Vec3(character->GetPosition()));
    result.velocity = toCentimeters(character->GetLinearVelocity());
    result.groundPosition = toCentimeters(JPH::Vec3(character->GetGroundPosition()));
    const auto normal = finiteGroundNormal("diagnostic");
    result.groundNormal = Tempest::Vec3(normal.GetX(),normal.GetY(),normal.GetZ());
    result.groundVelocity = toCentimeters(character->GetGroundVelocity());
    result.ground = diagnosticBody(character->GetGroundBodyID(),character->GetGroundPosition());
    result.groundSubShape = character->GetGroundSubShapeID().GetValue();
    result.groundUserData = uintptr_t(character->GetGroundUserData());
    result.groundState = uint8_t(character->GetGroundState());
    const auto inverse = JPH::RMat44::sInverseRotationTranslation(character->GetRotation(),character->GetPosition());
    result.contacts.reserve(character->GetActiveContacts().size());
    for(const auto& contact:character->GetActiveContacts()) {
      CharacterVirtualDiagnostics::Contact out;
      out.body = diagnosticBody(contact.mBodyB,contact.mPosition);
      out.subShape = contact.mSubShapeIDB.GetValue();
      out.contactUserData = uintptr_t(contact.mUserData);
      out.position = toCentimeters(JPH::Vec3(contact.mPosition));
      out.contactNormal = Tempest::Vec3(contact.mContactNormal.GetX(),contact.mContactNormal.GetY(),contact.mContactNormal.GetZ());
      out.surfaceNormal = Tempest::Vec3(contact.mSurfaceNormal.GetX(),contact.mSurfaceNormal.GetY(),contact.mSurfaceNormal.GetZ());
      out.pointVelocity = toCentimeters(contact.mLinearVelocity);
      out.distance = contact.mDistance*100.f;
      const float supportingDistance = character->GetSupportingVolume().SignedDistance(JPH::Vec3(inverse*contact.mPosition));
      out.supportingVolumeDistance = supportingDistance*100.f;
      out.hadCollision = contact.mHadCollision;
      out.discarded = contact.mWasDiscarded;
      out.sensor = contact.mIsSensorB;
      out.canPushCharacter = contact.mCanPushCharacter;
      out.withinSupportingVolume = supportingDistance<=0.f;
      out.walkableSlope = !character->IsSlopeTooSteep(contact.mSurfaceNormal);
      out.supportAllowedByPolicy = true;
      if(out.body.present) {
        JPH::BodyLockRead lock(world.impl->system.GetBodyLockInterface(),contact.mBodyB);
        if(lock.SucceededAndIsInBroadPhase())
          out.supportAllowedByPolicy = !rejectsSupport(lock.GetBody(),contact);
        }
      out.supportEligible = out.hadCollision && !out.discarded && !out.sensor &&
                            out.withinSupportingVolume && out.walkableSlope && out.supportAllowedByPolicy;
      result.contacts.push_back(std::move(out));
      }
    return result;
    }

  bool OnContactValidate(const JPH::CharacterVirtual*, const JPH::CharacterContact& contact) override {
    bool rejectSupport = false;
    {
      JPH::BodyLockRead lock(world.impl->system.GetBodyLockInterface(),contact.mBodyB);
      if(!lock.SucceededAndIsInBroadPhase() || !isPushable(lock.GetBody()) || contact.mIsSensorB)
        return true;
      // Loose utensils never become a floor. A heavy body's top may support a
      // character placed on it, but walking into it must not acquire that top
      // as a new step. Keep the side contacts, which still stop and push it.
      rejectSupport = rejectsSupport(lock.GetBody(),contact);
      }
    if(!rejectSupport)
      return true;

    // Rejecting a support in Jolt also removes its solver impulse. Retain the
    // real contact point and horizontal push, within the same per-move budget.
    // The capsule's rounded toe must slide through a mug rather than climb it.
    // Sweeps also validate distant future contacts, so only push a point that
    // is already touching the current capsule (including its padding).
    auto local = JPH::Vec3(contact.mPosition-character->GetPosition());
    auto closest = character->GetUp()*std::clamp(local.Dot(character->GetUp()),radius,height-radius);
    if((local-closest).Length()<=radius+character->GetCharacterPadding()+.001f) {
      auto horizontal = contact.mContactNormal-contact.mContactNormal.Dot(character->GetUp())*character->GetUp();
      if(horizontal.IsNearZero()) {
        const auto velocity = character->GetLinearVelocity();
        horizontal = -(velocity-velocity.Dot(character->GetUp())*character->GetUp());
        }
      if(!horizontal.IsNearZero()) {
        auto pushContact = contact;
        pushContact.mContactNormal = horizontal.Normalized();
        pushContact.mDistance = std::max(contact.mDistance,0.f);
        pushContact.mHadCollision = true;
        JPH::CharacterContactSettings settings;
        onContact(pushContact,settings);
        }
      }
    return false;
    }

  void OnContactSolve(const JPH::CharacterVirtual*, const JPH::BodyID& bodyID, const JPH::SubShapeID&,
                      JPH::RVec3Arg point, JPH::Vec3Arg normal, JPH::Vec3Arg, const JPH::PhysicsMaterial*,
                      JPH::Vec3Arg velocity, JPH::Vec3& newVelocity) override {
    JPH::BodyLockRead lock(world.impl->system.GetBodyLockInterface(),bodyID);
    if(!lock.SucceededAndIsInBroadPhase() || !isPushable(lock.GetBody()) ||
       (carrying && bodyID==ground && !character->IsSlopeTooSteep(normal)))
      return;
    const auto up = character->GetUp();
    if(JPH::Vec3(point-character->GetPosition()).Dot(up)<=.02f)
      return; // Existing top support, including a moving body.
    // The rounded capsule can turn even a vertical furniture side into an
    // upward velocity. Side contacts still block us, but cannot lift the feet.
    const float gained = (newVelocity-velocity).Dot(up);
    if(gained>0.f)
      newVelocity -= gained*up;
    }

  void onContact(const JPH::CharacterContact& contact, JPH::CharacterContactSettings& settings) {
    // Jolt's native limit applies afresh to each contact / stair probe. Dose
    // the impulse here instead, sharing one force budget across the whole move.
    settings.mCanReceiveImpulses = false;
    JPH::BodyLockWrite lock(world.impl->system.GetBodyLockInterface(),contact.mBodyB);
    if(!lock.SucceededAndIsInBroadPhase())
      return;
    const auto& nativeBody = lock.GetBody();
    const auto layer = nativeBody.GetObjectLayer();
    if((layer!=DynamicWorld::C_Item && layer!=DynamicWorld::C_MovableVob) || !nativeBody.IsDynamic() || contact.mIsSensorB)
      return;
    const float inverseMass = nativeBody.GetMotionProperties()->GetInverseMass();
    if(inverseMass<=0.f)
      return;
    const float bodyMass = 1.f/inverseMass;
    // A rotating utensil's side-point velocity can enter the combined floor
    // constraint and lift the feet even with a perfectly horizontal normal.
    // Keep it as a blocking, pushable object without letting it move the walker.
    // Heavy supports retain their native velocity and continue to carry feet.
    if(bodyMass<10.f)
      settings.mCanPushCharacter = false;
    auto* body = reinterpret_cast<CollisionBody*>(contact.mUserData);
    if(body==nullptr)
      return;
    // Attribute an approaching contact, including the character's weight on a
    // simulated support, rather than a separating or predictive-only contact.
    const auto relative = character->GetLinearVelocity()-contact.mLinearVelocity;
    const float projectedVelocity = relative.Dot(contact.mContactNormal);
    if(projectedVelocity<-0.01f) {
      body->npcContact.store(true,std::memory_order_relaxed);
      body->setTraceCulprit(culprit);
      }
    if(!contact.mHadCollision || pushSeconds<=0.f || pushRemaining<=0.f)
      return;

    // Use the native effective contact mass, including the torque caused by an
    // off-centre push. Penetration recovery has the same damping as Jolt.
    const float deltaVelocity = -projectedVelocity*0.9f-std::min(contact.mDistance,0.f)*0.4f/pushSeconds;
    if(deltaVelocity<=0.f)
      return;
    const auto jacobian = JPH::Vec3(contact.mPosition-nativeBody.GetCenterOfMassPosition()).Cross(contact.mContactNormal);
    const float inverseEffectiveMass = nativeBody.GetInverseInertia().Multiply3x3(jacobian).Dot(jacobian)+inverseMass;
    auto impulse = -(deltaVelocity/inverseEffectiveMass)*contact.mContactNormal;
    // Downward weight is applied independently by CharacterVirtual::Update;
    // do not spend the walking push budget on that supporting contact.
    const float down = impulse.Dot(character->GetUp());
    if(down<0.f)
      impulse -= down*character->GetUp();
    const float magnitude = impulse.Length();
    if(magnitude<=0.f)
      return;

    auto used = std::find_if(pushBodies.begin(),pushBodies.end(),[&](const auto& entry) {return entry.first==contact.mBodyB;});
    if(used==pushBodies.end()) {
      pushBodies.emplace_back(contact.mBodyB,0.f);
      used = std::prev(pushBodies.end());
      }
    // Ordinary walking has less effective effort against a heavier load than
    // a braced push. Keep this continuous in both masses; no furniture classes.
    const float force = character->GetMaxStrength()*character->GetMass()/(character->GetMass()+bodyMass);
    const float available = std::min(pushRemaining,std::max(0.f,force*pushSeconds-used->second));
    const float amount = std::min(magnitude,available);
    if(amount<=0.f)
      return;
    pushRemaining -= amount;
    used->second += amount;
    // The write lock is already held. AddImpulse activates a sleeping body and
    // applies the torque at the real contact point, in native SI units.
    world.impl->system.GetBodyInterfaceNoLock().AddImpulse(contact.mBodyB,impulse*(amount/magnitude),contact.mPosition);
    }

  void OnContactAdded(const JPH::CharacterVirtual*, const JPH::CharacterContact& contact,
                      JPH::CharacterContactSettings& settings) override { onContact(contact,settings); }
  void OnContactPersisted(const JPH::CharacterVirtual*, const JPH::CharacterContact& contact,
                          JPH::CharacterContactSettings& settings) override { onContact(contact,settings); }

  void OnAdjustBodyVelocity(const JPH::CharacterVirtual*, const JPH::Body& body, JPH::Vec3& linear,
                            JPH::Vec3& angular) override {
    // Carry sweeps through the already integrated world. Predicting the bodies
    // for another interval here would count their motion a second time.
    // An idle update also already accounts for its tracked support's observed
    // motion; its endpoint velocity must not advance that support again.
    if(carrying || (idleSupportMotionObserved && body.GetID()==ground))
      linear = angular = JPH::Vec3::sZero();
    }

  CollisionWorld& world;
  std::unique_ptr<JPH::CharacterVirtual> character;
  std::unique_ptr<CharacterVirtualDiagnostics> diagnostic;
  Npc* culprit = nullptr;
  float pushSeconds = 0.f;
  float pushRemaining = 0.f;
  std::vector<std::pair<JPH::BodyID,float>> pushBodies;
  struct LimitedSteepFace {
    JPH::BodyID body;
    JPH::SubShapeID subShape;
    bool seen = true;
    };
  std::vector<LimitedSteepFace> limitedSteepFaces;
  bool contactsDirty = true;
  bool needsInitialUpdate = true;
  bool carrying = false;
  bool idleSupportMotionObserved = false;
  float radius = 0.f;
  float height = 0.f;
  JPH::Vec3 lastGroundNormal = JPH::Vec3::sAxisY();
  uint64_t groundNormalFallbacks = 0;
  JPH::BodyID ground;
  JPH::RMat44 groundTransform = JPH::RMat44::sIdentity();
  };

CollisionWorld::VirtualCharacter::VirtualCharacter(CollisionWorld& owner, float radius, float height,
                                                  const Tempest::Vec3& feet)
  :impl(std::make_unique<Impl>(owner)) {
  const float r = std::max(toMeters(radius),0.01f);
  const float h = std::max(toMeters(height),2.f*r);
  impl->radius = r;
  impl->height = h;
  JPH::CharacterVirtualSettings settings;
  settings.mShape = new JPH::CapsuleShape(std::max(h*0.5f-r,0.001f),r);
  settings.mShapeOffset = JPH::Vec3(0,h*0.5f,0);
  settings.mSupportingVolume = JPH::Plane(JPH::Vec3::sAxisY(),-r);
  settings.mCharacterPadding = 0.01f;
  settings.mPredictiveContactDistance = 0.05f;
  settings.mCollisionTolerance = 0.001f;
  settings.mEnhancedInternalEdgeRemoval = true;
  impl->character = std::make_unique<JPH::CharacterVirtual>(&settings,JPH::RVec3(toMeters(feet)),
                                                          JPH::Quat::sIdentity(),&owner.impl->system);
  impl->character->SetListener(impl.get());
  owner.impl->virtualCharacters.push_back(this);
  }

CollisionWorld::VirtualCharacter::~VirtualCharacter() {
  auto& characters = impl->world.impl->virtualCharacters;
  characters.erase(std::remove(characters.begin(),characters.end(),this),characters.end());
  }

std::unique_ptr<CollisionWorld::VirtualCharacter> CollisionWorld::createVirtualCharacter(
    float radius, float height, const Tempest::Vec3& feet) {
  return std::unique_ptr<VirtualCharacter>(new VirtualCharacter(*this,radius,height,feet));
  }

void CollisionWorld::VirtualCharacter::setPosition(const Tempest::Vec3& feet) {
  impl->character->SetPosition(JPH::RVec3(toMeters(feet)));
  impl->contactsDirty = true;
  impl->needsInitialUpdate = true;
  impl->ground = JPH::BodyID();
  impl->limitedSteepFaces.clear();
  }

void CollisionWorld::VirtualCharacter::setDiagnosticsEnabled(bool enabled) {
  if(enabled && impl->diagnostic==nullptr)
    impl->diagnostic = std::make_unique<CharacterVirtualDiagnostics>();
  if(!enabled)
    impl->diagnostic.reset();
  }

bool CollisionWorld::VirtualCharacter::diagnostics(CharacterVirtualDiagnostics& out) const {
  if(impl->diagnostic==nullptr || !impl->diagnostic->valid)
    return false;
  out = *impl->diagnostic;
  out.groundNormalFallbacks = impl->groundNormalFallbacks;
  return true;
  }

CollisionWorld::CharacterMoveResult CollisionWorld::VirtualCharacter::move(
    const Tempest::Vec3& displacement, uint64_t dt, float stepHeight, float maxSlope, float mass, Npc* culprit,
    bool limitStairAdvance) {
  const auto started = std::chrono::steady_clock::now();
  auto& state = *impl;
  auto& character = *state.character;
  auto& world = *state.world.impl;
  const QueryBroadPhaseFilter broadFilter(characterMask);
  const QueryLayerFilter layerFilter(characterMask);
  const JPH::BodyFilter bodyFilter;
  const JPH::ShapeFilter shapeFilter;
  state.culprit = culprit;
  character.SetMass(std::max(mass,1.f));
  // Jolt uses metres, kilograms and seconds: this is a 450 N human push,
  // capped independently of the character's weight on a supporting body.
  character.SetMaxStrength(std::min(character.GetMass()*DynamicWorld::gravityMS,450.f));
  const float seconds = float(dt)*0.001f;
  // Zero-dt contact refreshes do not end a physical contact's classification.
  if(dt>0) {
    if(!limitStairAdvance)
      state.limitedSteepFaces.clear();
    else
      for(auto& face:state.limitedSteepFaces)
        face.seen = false;
    }
  state.pushSeconds = seconds;
  state.pushRemaining = character.GetMaxStrength()*seconds;
  state.pushBodies.clear();
  character.SetMaxSlopeAngle(maxSlope);
  if(state.contactsDirty) {
    character.RefreshContacts(broadFilter,layerFilter,bodyFilter,shapeFilter,world.allocator);
    state.contactsDirty = false;
    }
  // A zero-dt refresh after NPC clipping must not replace the main move's trace.
  auto* diagnostic = state.diagnostic!=nullptr && (dt>0 || !state.diagnostic->valid) ? state.diagnostic.get() : nullptr;
  if(diagnostic!=nullptr) {
    diagnostic->valid = false;
    diagnostic->dt = dt;
    diagnostic->requested = displacement;
    diagnostic->requestedVelocity = {};
    diagnostic->stairDesiredVelocity = {};
    diagnostic->maxSlopeRadians = maxSlope;
    diagnostic->stairHeight = stepHeight;
    diagnostic->stairDesiredDistance = 0.f;
    diagnostic->stairAchievedDistance = 0.f;
    diagnostic->stairSupported = false;
    diagnostic->stairCanWalk = false;
    diagnostic->stairBlockedByPolicy = false;
    diagnostic->stairProgressBlocked = false;
    diagnostic->stairProbe = {};
    diagnostic->stairAttempted = false;
    diagnostic->stairSucceeded = false;
    diagnostic->stairObstacleId = uint32_t(-1);
    diagnostic->before = state.diagnosticPose();
    }
  auto inheritedGroundVelocity = character.GetGroundVelocity();
  bool inferredGroundVelocity = false;
  bool climbedStep = false;
  if(seconds>0.f) {
    bool onGround = character.GetGroundState()==JPH::CharacterBase::EGroundState::OnGround;
    auto carriedVelocity = JPH::Vec3::sZero();
    if(character.IsSupported() && state.ground==character.GetGroundBodyID() && !state.ground.IsInvalid()) {
      JPH::BodyLockRead lock(world.system.GetBodyLockInterface(),state.ground);
      if(!lock.SucceededAndIsInBroadPhase())
        onGround = false;
      else if(!lock.GetBody().IsStatic()) {
        // Both direct-matrix movers and fixed-step dynamic bodies have already
        // advanced. Carry the observed displacement at the feet, including the
        // finite rotation, rather than extrapolating their endpoint velocity
        // over a render interval that may contain a different number of steps.
        const auto transform = lock.GetBody().GetWorldTransform();
        const auto position = character.GetPosition();
        carriedVelocity = JPH::Vec3(transform*(state.groundTransform.InversedRotationTranslation()*position)-position)/seconds;
        inheritedGroundVelocity = carriedVelocity;
        inferredGroundVelocity = true;
        }
      }
    character.UpdateGroundVelocity();
    auto groundVelocity = character.GetGroundVelocity();
    if(inferredGroundVelocity) {
      groundVelocity = carriedVelocity;
      // The floor has already advanced vertically. Resolve its new height
      // through penetration recovery / floor sticking; adding its upward
      // speed again would detach the feet from a direct-matrix lift.
      groundVelocity.SetY(0.f);
      }
    auto velocity = toMeters(displacement)/seconds;
    if(onGround)
      velocity += groundVelocity;
    auto gravityStep = JPH::Vec3(0,-DynamicWorld::gravityMS*seconds,0);
    if(displacement==Tempest::Vec3() && onGround) {
      // A supported idle character has static friction. Feeding vertical
      // gravity to Jolt's frictionless contact solver adds a downhill tangent
      // every frame, even on a walkable slope. Keep only pressure into the
      // support; moving platforms still carry us through groundVelocity and
      // Update applies the character's weight independently. Steep slopes and
      // requested walking/jumping retain their ordinary gravity response.
      if(state.needsInitialUpdate)
        gravityStep = state.initialIdleGravity(gravityStep);
      else {
        const auto normal = state.finiteGroundNormal("idle_gravity");
        gravityStep = normal*gravityStep.Dot(normal);
        }
      }
    velocity += gravityStep;
    if(diagnostic!=nullptr)
      diagnostic->requestedVelocity = toCentimeters(velocity);
    // ExtendedUpdate cancels steep-slope motion internally, while preserving
    // this original request for the separate world-stair recipe below.
    character.SetLinearVelocity(velocity);
    JPH::CharacterVirtual::ExtendedUpdateSettings settings;
    const float step = std::max(toMeters(stepHeight),0.f);
    // A steep, slanted riser can be longer than Jolt's fixed 15 cm probe.
    // Search its possible horizontal run, bounded by the permitted step's
    // height and slope, plus the toe. This changes only the support test;
    // accepted forward travel still uses the original per-frame request.
    const float riserRun = maxSlope>0.f ? std::min(step,step/std::tan(maxSlope)) : step;
    settings.mWalkStairsStepForwardTest = std::max(settings.mWalkStairsStepForwardTest,
                                                  riserRun+state.radius+character.GetCharacterPadding());
    // Run Jolt's ordinary update and floor sticking first. Its automatic stair
    // recipe is applied below only for world geometry, after contact solving
    // has identified pushable objects or interactive walking obstacles.
    settings.mWalkStairsStepUp = JPH::Vec3::sZero();
    settings.mStickToFloorStepDown = JPH::Vec3(0,-step,0);
    const auto oldPosition = character.GetPosition();
    const auto desiredVelocity = character.GetLinearVelocity();
    if(diagnostic!=nullptr)
      diagnostic->stairDesiredVelocity = toCentimeters(desiredVelocity);
    state.idleSupportMotionObserved = inferredGroundVelocity && onGround && displacement==Tempest::Vec3();
    character.ExtendedUpdate(seconds,JPH::Vec3(0,-DynamicWorld::gravityMS,0),settings,
                             broadFilter,layerFilter,bodyFilter,shapeFilter,world.allocator);
    state.idleSupportMotionObserved = false;
    // RefreshContacts only classifies the initial overlap. Let the first
    // physical solve after creation/teleport recover penetration with native
    // gravity. Only a verified nonpenetrating static plane can start at rest;
    // edges retain the native contact pose used to enter stairs.
    // A zero-dt refresh does not complete this acquisition.
    state.needsInitialUpdate = false;
    if(diagnostic!=nullptr)
      diagnostic->afterExtendedUpdate = state.diagnosticPose();
    bool stairObstacle = false;
    for(const auto& contact:character.GetActiveContacts()) {
      if(!contact.mHadCollision || contact.mWasDiscarded || contact.mIsSensorB ||
         contact.mSurfaceNormal.Dot(desiredVelocity-contact.mLinearVelocity)>=0.f)
        continue;
      for(auto& face:state.limitedSteepFaces)
        if(face.body==contact.mBodyB && face.subShape==contact.mSubShapeIDB)
          face.seen = true;
      JPH::BodyLockRead lock(world.system.GetBodyLockInterface(),contact.mBodyB);
      if(lock.SucceededAndIsInBroadPhase() && Impl::blocksAutomaticStairs(lock.GetBody())) {
        stairObstacle = true;
        if(diagnostic!=nullptr)
          diagnostic->stairObstacleId = contact.mBodyB.GetIndexAndSequenceNumber();
        break;
        }
      }
    // The remainder is the WalkStairs part of Jolt's ExtendedUpdate recipe,
    // including its direction and minimum-distance tests for oblique steps.
    const auto up = character.GetUp();
    // Static friction has a horizontal component on a slope; it is a contact
    // force, not a walking request. Keep support motion in the stair intent.
    const auto stairVelocity = desiredVelocity-gravityStep;
    const auto desiredStep = (stairVelocity-stairVelocity.Dot(up)*up)*seconds;
    const float desiredLength = desiredStep.Length();
    if(diagnostic!=nullptr) {
      const auto achieved = JPH::Vec3(character.GetPosition()-oldPosition);
      const float achievedLength = desiredLength>0.f ? std::max(0.f,achieved.Dot(desiredStep/desiredLength)) : 0.f;
      diagnostic->stairDesiredDistance = desiredLength*100.f;
      diagnostic->stairAchievedDistance = achievedLength*100.f;
      diagnostic->stairSupported = character.IsSupported();
      diagnostic->stairCanWalk = character.CanWalkStairs(desiredVelocity);
      diagnostic->stairBlockedByPolicy = stairObstacle;
      diagnostic->stairProgressBlocked = achievedLength+1.e-4f<desiredLength;
      }
    if(step>0.f && !stairObstacle && desiredLength>0.f && character.CanWalkStairs(desiredVelocity)) {
      const auto forward = desiredStep/desiredLength;
      const auto achieved = JPH::Vec3(character.GetPosition()-oldPosition);
      const float achievedLength = std::max(0.f,achieved.Dot(forward));
      if(achievedLength+1.e-4f<desiredLength) {
        auto direction = forward;
        float best = settings.mWalkStairsCosAngleForwardContact;
        bool extendedSteepFace = false;
        for(const auto& contact:character.GetActiveContacts()) {
          if(!contact.mHadCollision || contact.mWasDiscarded ||
             contact.mSurfaceNormal.Dot(desiredVelocity-contact.mLinearVelocity)>=0.f ||
             !character.IsSlopeTooSteep(contact.mSurfaceNormal))
            continue;
          const auto candidate = contact.mSurfaceNormal.Dot(up)*up-contact.mSurfaceNormal;
          const float length = candidate.Length();
          if(length>1.e-6f && candidate.Dot(forward)/length>best) {
            direction = candidate/length;
            best = direction.Dot(forward);
            }
          if(limitStairAdvance && !contact.mIsSensorB) {
            const auto limited = std::find_if(state.limitedSteepFaces.begin(),state.limitedSteepFaces.end(),[&](const auto& face) {
              return face.body==contact.mBodyB && face.subShape==contact.mSubShapeIDB;
              });
            if(limited!=state.limitedSteepFaces.end()) {
              extendedSteepFace = true;
              continue;
              }
            JPH::BodyLockRead lock(world.system.GetBodyLockInterface(),contact.mBodyB);
            if(lock.SucceededAndIsInBroadPhase()) {
              JPH::Shape::SupportingFace face;
              lock.GetBody().GetTransformedShape().GetSupportingFace(contact.mSubShapeIDB,contact.mSurfaceNormal,oldPosition,face);
              if(face.size()>=3) {
                float low = std::numeric_limits<float>::infinity(), high = -low;
                for(const auto& vertex:face) {
                  low = std::min(low,vertex.Dot(up));
                  high = std::max(high,vertex.Dot(up));
                  }
                // Face vertices are relative to the physical feet before this
                // update. Buried geometry below that support level cannot add
                // to the height of the obstacle the monster must climb.
                const float exposedHeight = std::max(0.f,high-std::max(low,0.f));
                if(exposedHeight>step+character.GetCharacterPadding()) {
                  extendedSteepFace = true;
                  // Retain this decision until contact ends: approaching the
                  // crest must not turn the same bank into a new low ledge.
                  state.limitedSteepFaces.push_back({contact.mBodyB,contact.mSubShapeIDB,true});
                  }
                }
              }
            }
          }
        // Keep a monster's stair advance within the remaining animation travel
        // instead of imposing Jolt's 2 cm minimum. A face exposed above the feet
        // by more than the guild step is extended terrain, not a permitted riser:
        // ascend it gradually. Ordinary ledges and shapes without polygonal
        // faces retain the full guild step and the native safe landing sweeps.
        const float forwardDistance = desiredLength-achievedLength;
        const auto forwardStep = direction*(limitStairAdvance ? forwardDistance :
                                            std::max(settings.mWalkStairsMinStepForward,forwardDistance));
        const float stairUp = extendedSteepFace ?
                                std::max(0.f,std::min(step,.12f)-std::max(0.f,achieved.Dot(up))) : step;
        JPH::CharacterVirtual::WalkStairsDiagnostics stairProbe;
        const bool stepped = stairUp>0.f && character.WalkStairs(seconds,up*stairUp,forwardStep,direction*settings.mWalkStairsStepForwardTest,
                                                settings.mWalkStairsStepDownExtra,broadFilter,layerFilter,bodyFilter,shapeFilter,world.allocator,
                                                diagnostic!=nullptr ? &stairProbe : nullptr);
        // A completed ordinary step is traversable even if a toe still touches
        // its riser. The limited rise against an extended bank is not one.
        climbedStep = stepped && !extendedSteepFace;
        if(diagnostic!=nullptr) {
          diagnostic->stairAttempted = true;
          diagnostic->stairSucceeded = stepped;
          static constexpr const char* reasons[]={"notAttempted","upBlocked","noSteep","noHorizontal","noProgress","noFloor",
                                                   "noForwardTest","testNoProgress","testNoFloor","testSteep","noHeight","success"};
          auto& probe=diagnostic->stairProbe;
          probe.reason = reasons[unsigned(stairProbe.mResult)];
          probe.upPosition = toCentimeters(JPH::Vec3(stairProbe.mUpPosition));
          probe.forwardPosition = toCentimeters(JPH::Vec3(stairProbe.mForwardPosition));
          probe.finalPosition = toCentimeters(JPH::Vec3(stairProbe.mFinalPosition));
          probe.testPosition = toCentimeters(JPH::Vec3(stairProbe.mTestPosition));
          probe.testFinalPosition = toCentimeters(JPH::Vec3(stairProbe.mTestFinalPosition));
          const auto normal=stairProbe.mLandingNormal, testNormal=stairProbe.mTestLandingNormal;
          probe.landingNormal = Tempest::Vec3(normal.GetX(),normal.GetY(),normal.GetZ());
          probe.testLandingNormal = Tempest::Vec3(testNormal.GetX(),testNormal.GetY(),testNormal.GetZ());
          probe.upFraction = stairProbe.mUpFraction;
          probe.landingFraction = stairProbe.mLandingFraction;
          probe.testLandingFraction = stairProbe.mTestLandingFraction;
          }
        }
      }
    }
  if(dt>0)
    state.limitedSteepFaces.erase(std::remove_if(state.limitedSteepFaces.begin(),state.limitedSteepFaces.end(),
                                                [](const auto& face) { return !face.seen; }),state.limitedSteepFaces.end());
  if(diagnostic!=nullptr) {
    if(seconds<=0.f)
      diagnostic->afterExtendedUpdate = state.diagnosticPose();
    diagnostic->afterStairs = state.diagnosticPose();
    diagnostic->valid = true;
    }
  const auto previousGround = state.ground;
  state.ground = character.GetGroundBodyID();
  if(!state.ground.IsInvalid()) {
    JPH::BodyLockRead lock(world.system.GetBodyLockInterface(),state.ground);
    if(lock.Succeeded())
      state.groundTransform = lock.GetBody().GetWorldTransform();
    else
      state.ground = JPH::BodyID();
    }
  CharacterMoveResult result;
  result.position = toCentimeters(JPH::Vec3(character.GetPosition()));
  // Jolt keeps its requested velocity after moving. Resolve the carried
  // velocity against actual contacts so downhill acceleration survives, while
  // stair snaps and penetration recovery never turn into airborne momentum.
  auto solvedVelocity = character.GetLinearVelocity();
  for(int iteration=0;iteration<2;++iteration) {
    for(const auto& contact:character.GetActiveContacts()) {
      if(!contact.mHadCollision || contact.mWasDiscarded || contact.mIsSensorB)
        continue;
      const float into = (solvedVelocity-contact.mLinearVelocity).Dot(contact.mContactNormal);
      if(into<0.f)
        solvedVelocity -= contact.mContactNormal*into;
      }
    }
  result.velocity = toCentimeters(solvedVelocity);
  result.groundVelocity = toCentimeters(inferredGroundVelocity && previousGround==state.ground ?
                                        inheritedGroundVelocity : character.GetGroundVelocity());
  const auto normal = state.finiteGroundNormal("move_output");
  result.groundNormal = Tempest::Vec3(normal.GetX(),normal.GetY(),normal.GetZ());
  result.grounded = character.GetGroundState()==JPH::CharacterBase::EGroundState::OnGround;
  result.steep = character.GetGroundState()==JPH::CharacterBase::EGroundState::OnSteepGround;
  if(!climbedStep) {
    const auto horizontal = toMeters(Tempest::Vec3(displacement.x,0,displacement.z));
    float strongest = -.01f*horizontal.Length();
    for(const auto& contact:character.GetActiveContacts()) {
      if(!contact.mHadCollision || contact.mWasDiscarded || contact.mIsSensorB ||
         !character.IsSlopeTooSteep(contact.mSurfaceNormal))
        continue;
      const float into = contact.mSurfaceNormal.Dot(horizontal);
      if(into>=strongest)
        continue;
      JPH::BodyLockRead lock(world.system.GetBodyLockInterface(),contact.mBodyB);
      if(!lock.SucceededAndIsInBroadPhase() || lock.GetBody().IsDynamic())
        continue;
      strongest = into;
      result.steepContact = true;
      const auto normal = contact.mSurfaceNormal;
      result.steepContactNormal = Tempest::Vec3(normal.GetX(),normal.GetY(),normal.GetZ());
      }
    }
  world.characterMilliseconds += std::chrono::duration<float,std::milli>(std::chrono::steady_clock::now()-started).count();
  return result;
  }

CollisionWorld::CharacterMoveResult CollisionWorld::VirtualCharacter::carry(uint64_t dt) {
  auto& state = *impl;
  auto& character = *state.character;
  auto& world = *state.world.impl;
  CharacterMoveResult result;
  result.position = toCentimeters(JPH::Vec3(character.GetPosition()));
  if(dt==0 || character.GetGroundState()!=JPH::CharacterBase::EGroundState::OnGround ||
     state.ground.IsInvalid() || state.ground!=character.GetGroundBodyID())
    return result;
  JPH::RMat44 transform;
  {
    JPH::BodyLockRead lock(world.system.GetBodyLockInterface(),state.ground);
    if(!lock.SucceededAndIsInBroadPhase() || lock.GetBody().IsStatic())
      return result;
    transform = lock.GetBody().GetWorldTransform();
  }
  const auto position = character.GetPosition();
  const auto delta = JPH::Vec3(transform*(state.groundTransform.InversedRotationTranslation()*position)-position);
  state.groundTransform = transform;
  if(delta.IsNearZero(1.e-7f))
    return result;
  const auto started = std::chrono::steady_clock::now();
  const QueryBroadPhaseFilter broadFilter(characterMask);
  const QueryLayerFilter layerFilter(characterMask);
  const JPH::BodyFilter bodyFilter;
  const JPH::ShapeFilter shapeFilter;
  const float seconds = float(dt)*.001f;
  const auto velocity = character.GetLinearVelocity();
  state.pushSeconds = state.pushRemaining = 0.f;
  state.carrying = true;
  // This is a collision sweep of the support's observed motion, never a pose
  // assignment. No second root input, gravity impulse, push budget or stairs.
  character.SetLinearVelocity(delta/seconds);
  character.Update(seconds,JPH::Vec3::sZero(),broadFilter,layerFilter,bodyFilter,shapeFilter,world.allocator);
  // A rising support can separate relative to the frozen body's zero velocity.
  // Re-evaluate support at the swept endpoint without that velocity test.
  character.SetLinearVelocity(JPH::Vec3::sZero());
  character.RefreshContacts(broadFilter,layerFilter,bodyFilter,shapeFilter,world.allocator);
  state.carrying = false;
  character.SetLinearVelocity(velocity);
  character.UpdateGroundVelocity();
  state.ground = character.GetGroundBodyID();
  if(!state.ground.IsInvalid()) {
    JPH::BodyLockRead lock(world.system.GetBodyLockInterface(),state.ground);
    if(lock.SucceededAndIsInBroadPhase())
      state.groundTransform = lock.GetBody().GetWorldTransform();
    else
      state.ground = JPH::BodyID();
  }
  result.position = toCentimeters(JPH::Vec3(character.GetPosition()));
  result.velocity = toCentimeters(velocity);
  result.groundVelocity = toCentimeters(delta/seconds);
  const auto normal = state.finiteGroundNormal("carry_output");
  result.groundNormal = Tempest::Vec3(normal.GetX(),normal.GetY(),normal.GetZ());
  result.grounded = character.GetGroundState()==JPH::CharacterBase::EGroundState::OnGround;
  result.steep = character.GetGroundState()==JPH::CharacterBase::EGroundState::OnSteepGround;
  result.supportMoved = true;
  world.characterMilliseconds += std::chrono::duration<float,std::milli>(std::chrono::steady_clock::now()-started).count();
  return result;
  }

CollisionWorld::CollisionWorld() {
  Physics::initialize();
  impl = std::make_unique<Impl>();
  impl->waterStep=[this](float seconds) { applyBuoyancyStep(seconds); };
  if(const auto* mode=std::getenv("OPENGOTHIC_MELEE_BENCH"); mode!=nullptr &&
     (std::string_view(mode)=="blade" || std::string_view(mode)=="blade-motors"))
    impl->meleeSubsteps=2;
  }

CollisionWorld::~CollisionWorld() {
  for(const auto& blade:impl->meleeBlades) {
    impl->system.GetBodyInterface().RemoveBody(blade.body);
    impl->system.GetBodyInterface().DestroyBody(blade.body);
    }
  while(!ragdolls.empty())
    ragdolls.back()->detach();
  while(!bodies.empty()) {
    auto* body = bodies.back();
    removeBody(*body);
    body->owner = nullptr;
    }
  }

float CollisionWorld::toMeters(float v) {
  return v*0.01f;
  }

JPH::Vec3 CollisionWorld::toMeters(const Tempest::Vec3& v) {
  return JPH::Vec3(v.x*0.01f,v.y*0.01f,v.z*0.01f);
  }

Tempest::Vec3 CollisionWorld::toCentimeters(JPH::Vec3Arg v) {
  return Tempest::Vec3(v.GetX()*100.f,v.GetY()*100.f,v.GetZ()*100.f);
  }

CollisionWorld::CollisionBody::CollisionBody(CollisionWorld& owner, JPH::ShapeRefC shape, int category)
  :owner(&owner), baseShape(std::move(shape)), scaledShape(baseShape), categoryId(category) {
  }

CollisionWorld::CollisionBody::~CollisionBody() {
  if(owner!=nullptr)
    owner->removeBody(*this);
  }

CollisionWorld::DynamicBody::DynamicBody(CollisionWorld& owner, JPH::ShapeRefC shape, float mass, int category)
  :CollisionBody(owner,std::move(shape),category), bodyMass(mass) {
  }

void CollisionWorld::CollisionBody::setObjMatrix(const Tempest::Matrix4x4& matrix) {
  if(owner==nullptr || id.IsInvalid())
    return;
  const Tempest::Vec3 position(matrix.at(3,0),matrix.at(3,1),matrix.at(3,2));
  if(categoryId==DynamicWorld::C_Item &&
     (!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z))) {
    owner->recoverItem(*this,position);
    return;
    }
  auto transform = toTransform(matrix);
  if((scale-transform.scale).LengthSq()>1.0e-10f) {
    scale = transform.scale;
    scaledShape = ::scaledShape(baseShape.GetPtr(),scale);
    if(buoyancyBaseShape!=nullptr) buoyancyShape=::scaledShape(buoyancyBaseShape.GetPtr(),scale);
    owner->impl->system.GetBodyInterface().SetShape(id,scaledShape.GetPtr(),false,JPH::EActivation::DontActivate);
    }
  owner->impl->system.GetBodyInterface().SetPositionAndRotationWhenChanged(id,transform.position,transform.rotation,JPH::EActivation::DontActivate);
  }

void CollisionWorld::CollisionBody::restoreTransform(const Tempest::Matrix4x4& matrix, bool sleeping) {
  const Tempest::Vec3 position(matrix.at(3,0),matrix.at(3,1),matrix.at(3,2));
  if(categoryId==DynamicWorld::C_Item &&
     (!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z))) {
    owner->recoverItem(*this,position);
    return;
    }
  setObjMatrix(matrix);
  itemRestTransform=matrix;
  // A restored sleeping float is already in equilibrium. Discovering its
  // water surface must not turn load into a fresh water entry or wake it.
  bool restoredSleep=sleeping;
  if(sleeping && !owner->buoyancy && waterThing(categoryId) && owner->waterSurface) {
    Tempest::Vec3 center;
    float bottom=0;
    {
      JPH::BodyLockRead lock(owner->impl->system.GetBodyLockInterface(),id);
      if(lock.Succeeded()) {
        const auto& body=lock.GetBody();
        const auto* shape=buoyancyShape!=nullptr ? buoyancyShape.GetPtr() : body.GetShape();
        const auto bounds=shape->GetWorldSpaceBounds(body.GetCenterOfMassTransform(),JPH::Vec3::sOne());
        center=toCentimeters(body.GetCenterOfMassPosition());
        bottom=bounds.mMin.GetY()*100.f;
        }
      }
    // OFF changes a saved float, while dry sleeping props retain the previous
    // load behaviour. Query outside the native lock, just like ordinary water.
    float surface=0;
    if(owner->waterSurface(center,bottom,surface) && surface>bottom) restoredSleep=false;
    }
  waterInitialized=false;
  waterPreserveSleep=restoredSleep;
  inWater=false;
  waterRestTime=0;
  auto& bodies = owner->impl->system.GetBodyInterface();
  bodies.SetLinearAndAngularVelocity(id,JPH::Vec3::sZero(),JPH::Vec3::sZero());
  if(restoredSleep)
    bodies.DeactivateBody(id);
  else
    bodies.ActivateBody(id);
  }

void CollisionWorld::CollisionBody::setPosition(const Tempest::Vec3& position) {
  if(categoryId==DynamicWorld::C_Item &&
     (!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z))) {
    owner->recoverItem(*this,position);
    return;
    }
  owner->impl->system.GetBodyInterface().SetPosition(id,toMeters(position),JPH::EActivation::DontActivate);
  }

void CollisionWorld::CollisionBody::retainForItemWake() {
  // Keep an approached sleeper until a later budgeted query can visit it
  // again. This changes neither its native activation nor its velocity.
  itemWakeUntilStep=owner->impl->simulationStep+8;
  }

bool CollisionWorld::CollisionBody::keepSleepingItem(const Tempest::Vec3& positionCm) const {
  if((owner->buoyancy && inWater) || itemRecovered ||
     (owner->itemWake && itemWakeUntilStep>=owner->impl->simulationStep && itemWakeUntilStep!=0))
    return true;
  const auto position=toMeters(positionCm);
  const float range=toMeters(DynamicWorld::characterItemRange);
  for(const auto* character:owner->impl->virtualCharacters)
    if((position-character->impl->character->GetPosition()).LengthSq()<=range*range)
      return true;
  return false;
  }

void CollisionWorld::CollisionBody::setScale(const Tempest::Vec3& value) {
  const auto next = JPH::Vec3(value.x,value.y,value.z);
  if(scale==next)
    return;
  auto result = ::scaledShape(baseShape.GetPtr(),next);
  scale = next;
  scaledShape = std::move(result);
  if(buoyancyBaseShape!=nullptr) buoyancyShape=::scaledShape(buoyancyBaseShape.GetPtr(),scale);
  owner->impl->system.GetBodyInterface().SetShape(id,scaledShape.GetPtr(),false,JPH::EActivation::DontActivate);
  }

void CollisionWorld::CollisionBody::setShape(JPH::ShapeRefC shape) {
  auto result = ::scaledShape(shape.GetPtr(),scale);
  baseShape = std::move(shape);
  scaledShape = std::move(result);
  owner->impl->system.GetBodyInterface().SetShape(id,scaledShape.GetPtr(),false,JPH::EActivation::DontActivate);
  }

void CollisionWorld::CollisionBody::setPayload(void* payload, int type) {
  pointer = payload;
  pointerType = type;
  }

void CollisionWorld::CollisionBody::setBuoyancyShape(JPH::ShapeRefC shape) {
  buoyancyBaseShape=std::move(shape);
  buoyancyShape=::scaledShape(buoyancyBaseShape.GetPtr(),scale);
  waterInitialized=false;
  waterRestTime=0;
  }

void CollisionWorld::CollisionBody::setBuoyancyFraction(float fraction) {
  buoyancyFraction=std::isfinite(fraction) && fraction>0.f ? fraction : 0.f;
  }

void CollisionWorld::CollisionBody::setNoiseProfile(ItemMaterial material, asset::MaterialGroup surface, std::string_view what) {
  noiseMaterial=material;
  noiseSurface=surface;
  noiseWhat=what;
  }

Tempest::Vec3 CollisionWorld::CollisionBody::position() const {
  return toCentimeters(owner->impl->system.GetBodyInterface().GetPosition(id));
  }

Tempest::Vec3 CollisionWorld::CollisionBody::centerOfMass() const {
  return toCentimeters(owner->impl->system.GetBodyInterface().GetCenterOfMassPosition(id));
  }

Tempest::Matrix4x4 CollisionWorld::CollisionBody::matrix() const {
  auto transform = owner->impl->system.GetBodyInterface().GetWorldTransform(id);
  Tempest::Matrix4x4 result;
  float m[16];
  for(JPH::uint c=0;c<4;++c) {
    auto column = transform.GetColumn4(c);
    const float factor = c<3 ? scale[c] : 1.f;
    m[c*4+0] = column.GetX()*factor;
    m[c*4+1] = column.GetY()*factor;
    m[c*4+2] = column.GetZ()*factor;
    m[c*4+3] = column.GetW();
    }
  m[12] *= 100.f;
  m[13] *= 100.f;
  m[14] *= 100.f;
  result.setData(m);
  return result;
  }

void CollisionWorld::CollisionBody::getAabb(JPH::Vec3& min, JPH::Vec3& max) const {
  JPH::BodyLockRead lock(owner->impl->system.GetBodyLockInterface(),id);
  if(!lock.Succeeded()) {
    min = max = JPH::Vec3::sZero();
    return;
    }
  const auto& box = lock.GetBody().GetWorldSpaceBounds();
  min = box.mMin;
  max = box.mMax;
  }

float CollisionWorld::CollisionBody::mass() const {
  if(owner==nullptr || id.IsInvalid()) return 0.f;
  JPH::BodyLockRead lock(owner->impl->system.GetBodyLockInterface(),id);
  if(!lock.Succeeded() || !lock.GetBody().IsDynamic()) return 0.f;
  return 1.f/lock.GetBody().GetMotionProperties()->GetInverseMass();
  }

bool CollisionWorld::CollisionBody::isMovable() const {
  if(owner==nullptr || id.IsInvalid()) return false;
  JPH::BodyLockRead lock(owner->impl->system.GetBodyLockInterface(),id);
  return lock.Succeeded() && !lock.GetBody().IsStatic();
  }

Tempest::Vec3 CollisionWorld::CollisionBody::velocity() const {
  if(owner==nullptr || id.IsInvalid()) return {};
  JPH::BodyLockRead lock(owner->impl->system.GetBodyLockInterface(),id);
  if(!lock.Succeeded() || !lock.GetBody().IsDynamic()) return {};
  return toCentimeters(lock.GetBody().GetLinearVelocity());
  }

Tempest::Vec3 CollisionWorld::CollisionBody::angularVelocity() const {
  if(owner==nullptr || id.IsInvalid()) return {};
  JPH::BodyLockRead lock(owner->impl->system.GetBodyLockInterface(),id);
  if(!lock.Succeeded() || !lock.GetBody().IsDynamic()) return {};
  const auto velocity=lock.GetBody().GetAngularVelocity();
  return {velocity.GetX(),velocity.GetY(),velocity.GetZ()};
  }

bool CollisionWorld::CollisionBody::isSleeping() const {
  if(owner==nullptr || id.IsInvalid()) return true;
  JPH::BodyLockRead lock(owner->impl->system.GetBodyLockInterface(),id);
  return !lock.Succeeded() || !lock.GetBody().IsDynamic() || !lock.GetBody().IsActive();
  }

void CollisionWorld::CollisionBody::setTraceCulprit(Npc* culprit) {
  traceCause.clear();
  if(culprit==nullptr)
    return;
  if(culprit->isPlayer())
    traceCause = "PLAYER";
  else if(auto symbol = culprit->world().script().findSymbol(size_t(culprit->instanceSymbol())))
    traceCause = symbol->name();
  }

bool CollisionWorld::CollisionBody::addImpulse(const Tempest::Vec3& impulseNs, Npc* culprit,
                                               const Tempest::Vec3* pointCm) {
  if(owner==nullptr || id.IsInvalid() || !std::isfinite(impulseNs.x) || !std::isfinite(impulseNs.y) ||
     !std::isfinite(impulseNs.z) || (impulseNs.x==0.f && impulseNs.y==0.f && impulseNs.z==0.f)) return false;
  if(pointCm!=nullptr && (!std::isfinite(pointCm->x) || !std::isfinite(pointCm->y) || !std::isfinite(pointCm->z)))
    return false;
  {
    JPH::BodyLockRead lock(owner->impl->system.GetBodyLockInterface(),id);
    if(!lock.Succeeded() || !lock.GetBody().IsDynamic()) return false;
    }
  // BodyInterface adds the impulse in SI units and activates a sleeping body.
  auto& interface=owner->impl->system.GetBodyInterface();
  if(pointCm!=nullptr)
    interface.AddImpulse(id,JPH::Vec3(impulseNs.x,impulseNs.y,impulseNs.z),JPH::RVec3(toMeters(*pointCm)));
  else
    interface.AddImpulse(id,JPH::Vec3(impulseNs.x,impulseNs.y,impulseNs.z));
  noiseCulprit=culprit!=nullptr ? culprit->handlePtr() : std::weak_ptr<zenkit::INpc>();
  noiseCulpritUntilStep=owner->impl->simulationStep+300;
  setTraceCulprit(culprit);
  return true;
  }

bool CollisionWorld::CollisionBody::addImpulseAtPoint(const Tempest::Vec3& impulseNs,
                                                      const Tempest::Vec3& pointCm, Npc* culprit) {
  return addImpulse(impulseNs,culprit,&pointCm);
  }

void CollisionWorld::CollisionBody::setVelocity(const Tempest::Vec3& linear, const Tempest::Vec3& angular) {
  owner->impl->system.GetBodyInterface().SetLinearAndAngularVelocity(id,toMeters(linear),JPH::Vec3(angular.x,angular.y,angular.z));
  }

void CollisionWorld::addBody(CollisionBody& body, const Tempest::Matrix4x4& matrix, float friction,
                             bool movable, bool sensor, float mass, bool startSleeping) {
  const auto transform = toTransform(matrix);
  body.scale = transform.scale;
  body.itemRestTransform = matrix;
  body.scaledShape = ::scaledShape(body.baseShape.GetPtr(),body.scale);
  const auto type = mass>0 ? JPH::EMotionType::Dynamic : movable ? JPH::EMotionType::Kinematic : JPH::EMotionType::Static;
  JPH::BodyCreationSettings settings(body.scaledShape.GetPtr(),transform.position,transform.rotation,type,JPH::ObjectLayer(body.categoryId));
  settings.mFriction = friction;
  settings.mIsSensor = sensor;
  settings.mUserData = reinterpret_cast<JPH::uint64>(&body);
  if(type==JPH::EMotionType::Kinematic) {
    // Triangle meshes have no volume or inertia, but their animated bodies need motion properties.
    settings.mOverrideMassProperties = JPH::EOverrideMassProperties::MassAndInertiaProvided;
    settings.mMassPropertiesOverride.mMass = 1.f;
    settings.mMassPropertiesOverride.mInertia = JPH::Mat44::sIdentity();
    }
  if(type==JPH::EMotionType::Dynamic) {
    settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
    settings.mMassPropertiesOverride.mMass = mass;
    settings.mMotionQuality = JPH::EMotionQuality::LinearCast;
    settings.mLinearDamping = 0.f;
    settings.mAngularDamping = 0.f;
    if(body.categoryId==DynamicWorld::C_MovableVob) {
      settings.mLinearDamping = 0.1f;
      settings.mAngularDamping = 0.2f;
      }
    if(body.categoryId==DynamicWorld::C_Fragment) {
      settings.mLinearDamping = 0.4f;
      settings.mAngularDamping = 0.6f;
      }
    }
  auto& interface = impl->system.GetBodyInterface();
  auto* native = interface.CreateBody(settings);
  if(native==nullptr)
    throw std::runtime_error("Jolt body capacity exhausted");
  body.id = native->GetID();
  interface.AddBody(body.id,type==JPH::EMotionType::Dynamic && !startSleeping ? JPH::EActivation::Activate : JPH::EActivation::DontActivate);
  bodies.push_back(&body);
  }

std::unique_ptr<CollisionWorld::CollisionBody> CollisionWorld::addCollisionBody(JPH::ShapeRefC shape, const Tempest::Matrix4x4& matrix,
                                                                               float friction, int category, bool movable, bool sensor,
                                                                               const PhysicVbo* mesh) {
  auto result = std::unique_ptr<CollisionBody>(new CollisionBody(*this,std::move(shape),category));
  result->mesh = mesh;
  addBody(*result,matrix,friction,movable,sensor,0.f);
  return result;
  }

std::unique_ptr<CollisionWorld::DynamicBody> CollisionWorld::addDynamicBody(JPH::ShapeRefC shape, const Tempest::Matrix4x4& matrix,
                                                                           float friction, float mass, int category, bool startSleeping) {
  mass = std::isfinite(mass) && mass>0.f ? mass : 1.f;
  auto result = std::unique_ptr<DynamicBody>(new DynamicBody(*this,std::move(shape),mass,category));
  addBody(*result,matrix,friction,false,false,mass,startSleeping);
  rigid.push_back(result.get());
  return result;
  }

void CollisionWorld::removeBody(CollisionBody& body) {
  if(body.id.IsInvalid())
    return;
  auto& interface = impl->system.GetBodyInterface();
  interface.RemoveBody(body.id);
  interface.DestroyBody(body.id);
  body.id = JPH::BodyID();
  bodies.erase(std::remove(bodies.begin(),bodies.end(),&body),bodies.end());
  rigid.erase(std::remove(rigid.begin(),rigid.end(),&body),rigid.end());
  }

void CollisionWorld::staticTriangles(const std::function<void(const StaticTriangle&)>& output) const {
  struct TriangleCollector final : JPH::TransformedShapeCollector {
    TriangleCollector(const std::function<void(const StaticTriangle&)>& output, bool water)
      :output(output),water(water) {
      }

    void AddHit(const JPH::TransformedShape& leaf) override {
      // CollectTransformedShapes resolves compounds and every scale/transform
      // wrapper before GetTrianglesStart, which only accepts leaf shapes.
      JPH::Shape::GetTrianglesContext context;
      const auto bounds = leaf.GetWorldSpaceBounds();
      leaf.GetTrianglesStart(context,bounds,JPH::RVec3::sZero());
      constexpr int batchSize = 128;
      JPH::Float3 vertices[batchSize*3];
      for(;;) {
        const int count = leaf.GetTrianglesNext(context,batchSize,vertices);
        if(count==0)
          break;
        for(int triangle=0;triangle<count;++triangle) {
          StaticTriangle result;
          result.water = water;
          for(int i=0;i<3;++i) {
            const auto& v = vertices[triangle*3+i];
            result.vertices[i] = Tempest::Vec3(v.x,v.y,v.z);
            }
          output(result);
          }
        }
      }

    const std::function<void(const StaticTriangle&)>& output;
    bool water;
    };

  for(const auto* object:bodies) {
    if(object->categoryId!=DynamicWorld::C_Landscape &&
       object->categoryId!=DynamicWorld::C_Object &&
       object->categoryId!=DynamicWorld::C_Water)
      continue;
    JPH::TransformedShape shape;
    {
      JPH::BodyLockRead lock(impl->system.GetBodyLockInterface(),object->id);
      if(!lock.Succeeded() || !lock.GetBody().IsStatic())
        continue; // no living bodies, dynamic props or animated/kinematic vobs
      shape = lock.GetBody().GetTransformedShape();
    }
    TriangleCollector collector(output,object->categoryId==DynamicWorld::C_Water);
    shape.CollectTransformedShapes(shape.GetWorldSpaceBounds(),collector);
    }
  }

void CollisionWorld::fillMaterial(QueryResult& result, const JPH::Shape& shape, JPH::SubShapeID subShape) const {
  if(result.body==nullptr || result.body->mesh==nullptr)
    return;
  const auto segment = Physics::triangleData(shape,subShape);
  result.material = result.body->mesh->materialId(segment);
  result.sector = result.body->mesh->sectorName(segment);
  }

void CollisionWorld::fillRagdoll(QueryResult& result, JPH::BodyID native) const {
  // Metadata for this selected native hit only: never cast another ray or keep
  // a stale BodyID table across corpse restores and native rig replacements.
  for(auto* body:ragdolls) {
    const size_t bone=body->boneForBodyId(native.GetIndexAndSequenceNumber());
    if(bone==size_t(-1)) continue;
    result.ragdoll=body;
    result.bone=bone;
    return;
    }
  }

int CollisionWorld::navigationBodyCategory(JPH::BodyID native) const {
  if(native.IsInvalid())
    return 0;
  JPH::BodyLockRead lock(impl->system.GetBodyLockInterface(),native);
  return lock.Succeeded() ? int(lock.GetBody().GetObjectLayer()) : 0;
  }

CollisionWorld::QueryResult CollisionWorld::rayCast(const Tempest::Vec3& from, const Tempest::Vec3& to, uint32_t mask,
                                                    bool backfaces, std::vector<QueryResult>* all,
                                                    const std::vector<const CollisionBody*>* excluded,
                                                    const Ragdoll* excludedRig) const {
  impl->rayCount.fetch_add(1,std::memory_order_relaxed);
  QueryResult result;
  if(all!=nullptr)
    all->clear();
  auto origin = toMeters(from);
  const auto end = toMeters(to);
  auto direction = end-origin;
  if(direction.IsNearZero())
    return result;
  JPH::RRayCast ray(origin,direction);
  JPH::RayCastSettings settings;
  settings.mTriangleEdgeTolerance = 1.0e-4f;
  settings.SetBackFaceMode(backfaces ? JPH::EBackFaceMode::CollideWithBackFaces : JPH::EBackFaceMode::IgnoreBackFaces);
  QueryLayerFilter filter(mask);
  QueryBroadPhaseFilter broadFilter(mask);
  struct RayBodyFilter final : JPH::IgnoreMultipleBodiesFilter {
    const Ragdoll* rig = nullptr;
    bool ShouldCollide(const JPH::BodyID& id) const override {
      return JPH::IgnoreMultipleBodiesFilter::ShouldCollide(id) &&
             (rig==nullptr || rig->boneForBodyId(id.GetIndexAndSequenceNumber())==size_t(-1));
      }
    } bodyFilter;
  bodyFilter.rig = excludedRig;
  if(excluded!=nullptr)
    for(auto* body:*excluded)
      if(body!=nullptr)
        bodyFilter.IgnoreBody(body->id);
  struct RayEdge {
    const PhysicVbo* mesh = nullptr;
    JPH::TransformedShape shape;
    JPH::Vec3 vertices[3];
    JPH::Vec3 from, to;
    uint32_t triangle = 0;
    } edge;
  auto convert = [&](const JPH::RayCastResult& hit, bool inspectEdge=false) {
    QueryResult value;
    JPH::BodyLockRead lock(impl->system.GetBodyLockInterface(),hit.mBodyID);
    if(!lock.Succeeded())
      return value;
    const auto& body = lock.GetBody();
    value.hasHit = true;
    value.fraction = hit.mFraction;
    value.nativeBodyId = hit.mBodyID;
    value.body = reinterpret_cast<CollisionBody*>(body.GetUserData());
    value.point = toCentimeters(origin*(1.f-hit.mFraction)+end*hit.mFraction);
    const auto normal = body.GetWorldSpaceSurfaceNormal(hit.mSubShapeID2,ray.GetPointOnRay(hit.mFraction));
    value.normal = Tempest::Vec3(normal.GetX(),normal.GetY(),normal.GetZ());
    if(value.body!=nullptr && value.body->mesh!=nullptr) {
      const auto* mesh = value.body->mesh;
      auto triangle = Physics::triangleData(*body.GetShape(),hit.mSubShapeID2);
      const bool duplicates = mesh->hasRayDuplicates(triangle);
      if(inspectEdge || duplicates || all==nullptr) {
        const auto inverse = body.GetWorldTransform().InversedRotationTranslation();
        const auto localFrom = JPH::Vec3(inverse*origin)/value.body->scale;
        const auto localTo = JPH::Vec3(inverse*end)/value.body->scale;
        const auto localPoint = localFrom*(1.f-hit.mFraction)+localTo*hit.mFraction;
        if(inspectEdge && mesh->isRayEdge(triangle,localPoint,settings.mTriangleEdgeTolerance) &&
           mesh->triangleVertices(triangle,edge.vertices)) {
          edge.mesh = mesh;
          edge.shape = body.GetTransformedShape();
          edge.from = localFrom;
          edge.to = localTo;
          edge.triangle = triangle;
          }
        if(duplicates) {
          // Coincident faces can carry different Gothic materials. Quantization
          // can reverse their fraction order; resolve their source metadata.
          triangle = mesh->resolveRayTriangle(triangle,localFrom,localTo,backfaces);
          }
        if(all==nullptr) {
          // Quantized floor heights change the game's slope and movement inputs.
          // Refine the selected native face from its original source plane.
          // Identity terrain also exports that plane's original scalar normal;
          // all-hit queries retain their native normals and fraction ordering.
          const float fraction = mesh->rayTriangleFraction(triangle,localFrom,localTo,backfaces);
          if(fraction<1.f) {
            value.fraction = fraction;
            value.point = toCentimeters(origin*(1.f-fraction)+end*fraction);
            if(fraction>0.f && value.body->category()==DynamicWorld::C_Landscape &&
               value.body->scale==JPH::Vec3::sOne() && body.GetRotation()==JPH::Quat::sIdentity()) {
              JPH::Vec3 sourceNormal;
              if(mesh->rayTriangleNormal(triangle,sourceNormal))
                value.normal = Tempest::Vec3(sourceNormal.GetX(),sourceNormal.GetY(),sourceNormal.GetZ());
              }
            }
          }
        }
      value.material = mesh->materialId(triangle);
      value.sector = mesh->sectorName(triangle);
      }
    if(body.GetObjectLayer()==DynamicWorld::C_Ragdoll)
      fillRagdoll(value,hit.mBodyID);
    return value;
    };
  if(all!=nullptr) {
    JPH::AllHitCollisionCollector<JPH::CastRayCollector> collector;
    impl->system.GetNarrowPhaseQuery().CastRay(ray,settings,collector,broadFilter,filter,bodyFilter);
    collector.Sort();
    for(const auto& hit:collector.mHits) {
      auto value = convert(hit);
      if(value.hasHit)
        all->push_back(value);
      }
    if(!all->empty())
      result = all->front();
    }
  else {
    JPH::ClosestHitCollisionCollector<JPH::CastRayCollector> collector;
    impl->system.GetNarrowPhaseQuery().CastRay(ray,settings,collector,broadFilter,filter,bodyFilter);
    if(collector.HadHit()) {
      result = convert(collector.mHit,true);
      if(edge.mesh!=nullptr) {
        // Compressed mesh vertices can reverse the closest face at a shared edge.
        // Requery this shape after releasing its body lock, and compare only
        // adjacent native hits against the original mesh planes. All-hit queries
        // retain their native fraction ordering.
        JPH::AllHitCollisionCollector<JPH::CastRayCollector> candidates;
        edge.shape.CastRay(ray,settings,candidates);
        auto closest = collector.mHit;
        auto triangle = edge.triangle;
        float fraction = edge.mesh->rayTriangleFraction(triangle,edge.from,edge.to,backfaces);
        for(const auto& hit:candidates.mHits) {
          if(hit.mBodyID!=closest.mBodyID)
            continue;
          const auto candidate = Physics::triangleData(*edge.shape.mShape,hit.mSubShapeID2);
          JPH::Vec3 vertices[3];
          if(!edge.mesh->triangleVertices(candidate,vertices))
            continue;
          unsigned shared = 0;
          for(const auto& vertex:edge.vertices)
            if(vertex==vertices[0] || vertex==vertices[1] || vertex==vertices[2])
              ++shared;
          if(shared<2)
            continue;
          const float candidateFraction = edge.mesh->rayTriangleFraction(candidate,edge.from,edge.to,backfaces);
          if(candidateFraction>=1.f || candidateFraction>fraction ||
             (candidateFraction==fraction && candidate>=triangle))
            continue;
          closest = hit;
          triangle = candidate;
          fraction = candidateFraction;
          }
        if(closest.mSubShapeID2!=collector.mHit.mSubShapeID2)
          result = convert(closest);
        }
      }
    }
  return result;
  }

CollisionWorld::QueryResult CollisionWorld::raySweep(const Tempest::Vec3& from, const Tempest::Vec3& to, float radius,
                                                     uint32_t mask, bool backfaces) const {
  if(radius<=0.f)
    return rayCast(from,to,mask,backfaces);
  QueryResult result;
  const auto origin = toMeters(from);
  const auto end = toMeters(to);
  const auto direction = end-origin;
  if(direction.IsNearZero())
    return result;
  const auto r = toMeters(radius);
  JPH::SphereShape probe(r);
  JPH::RShapeCast cast(&probe,JPH::Vec3::sOne(),JPH::RMat44::sTranslation(origin),direction);
  JPH::ShapeCastSettings settings;
  // The previous sphere sweep was two-sided; only the ray fallback rejects backfaces.
  settings.SetBackFaceMode(JPH::EBackFaceMode::CollideWithBackFaces);
  settings.mReturnDeepestPoint = true;
  settings.mActiveEdgeMode = JPH::EActiveEdgeMode::CollideWithAll;
  settings.mUsePreciseSphereTriangleCast = true;
  SweepCollector collector(direction);
  QueryLayerFilter filter(mask);
  QueryBroadPhaseFilter broadFilter(mask);
  impl->system.GetNarrowPhaseQuery().CastShape(cast,settings,JPH::RVec3::sZero(),collector,broadFilter,filter);
  if(!collector.HadHit())
    return result;
  const auto& hit = collector.hit;
  auto delta = end-hit.mContactPointOn2;
  delta.SetY(0.f);
  const bool fallback = delta.LengthSq()>=0.00001f;
  JPH::BodyLockRead lock(impl->system.GetBodyLockInterface(),hit.mBodyID2);
  if(!lock.Succeeded())
    return result;
  const auto& body = lock.GetBody();
  result.hasHit = true;
  // Walking expects the bottom of the probe projected onto the original vertical ray.
  // Preserve the operation order: shallow rays can extrapolate this fraction far
  // beyond the segment, where algebraically equivalent expressions round differently.
  const float ySphere = (1.f-hit.mFraction)*origin.GetY()+hit.mFraction*end.GetY();
  const float yHit = ySphere-r;
  result.fraction = direction.GetY()!=0.f ? (yHit-origin.GetY())/direction.GetY() : hit.mFraction;
  result.point = toCentimeters(origin*(1.f-result.fraction)+end*result.fraction);
  auto normal = -hit.mPenetrationAxis.NormalizedOr(JPH::Vec3::sAxisY());
  result.normal = Tempest::Vec3(normal.GetX(),normal.GetY(),normal.GetZ());
  result.body = reinterpret_cast<CollisionBody*>(body.GetUserData());
  fillMaterial(result,*body.GetShape(),hit.mSubShapeID2);
  if(body.GetObjectLayer()==DynamicWorld::C_Ragdoll)
    fillRagdoll(result,hit.mBodyID2);
  // A failed center ray kept the old sphere hit, while resetting only its fraction.
  if(fallback) {
    lock.ReleaseLock();
    auto ray = rayCast(from,to,mask,backfaces);
    if(ray.hasHit)
      return ray;
    result.fraction = 1.f;
    }
  return result;
  }

std::vector<CollisionWorld::Overlap> CollisionWorld::overlaps(const CollisionBody& body, float marginCm, uint32_t mask) const {
  std::vector<Overlap> ret;
  JPH::RMat44 com;
  JPH::ShapeRefC shape;
  {
    JPH::BodyLockRead lock(impl->system.GetBodyLockInterface(),body.id);
    if(!lock.Succeeded())
      return ret;
    com   = lock.GetBody().GetCenterOfMassTransform();
    shape = lock.GetBody().GetShape();
  }
  // The simulation's own narrow phase settings: back faces and inactive edges never push.
  JPH::CollideShapeSettings settings;
  settings.mActiveEdgeMode = JPH::EActiveEdgeMode::CollideOnlyWithActive;
  settings.mMaxSeparationDistance = toMeters(marginCm);
  JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> collector;
  QueryLayerFilter filter(mask);
  QueryBroadPhaseFilter broadFilter(mask);
  JPH::IgnoreSingleBodyFilter self(body.id);
  const auto base = com.GetTranslation();
  impl->system.GetNarrowPhaseQuery().CollideShape(shape,JPH::Vec3::sOne(),com,settings,base,collector,
                                               broadFilter,filter,self);
  for(const auto& hit:collector.mHits) {
    JPH::BodyLockRead lock(impl->system.GetBodyLockInterface(),hit.mBodyID2);
    if(!lock.Succeeded())
      continue;
    const auto& native = lock.GetBody();
    Overlap o;
    o.body     = reinterpret_cast<CollisionBody*>(native.GetUserData());
    o.category = int(native.GetObjectLayer());
    o.depth    = hit.mPenetrationDepth*100.f;
    o.axis     = toCentimeters(hit.mPenetrationAxis.NormalizedOr(JPH::Vec3::sZero())*0.01f);
    o.point1   = toCentimeters(base+hit.mContactPointOn1);
    o.point2   = toCentimeters(base+hit.mContactPointOn2);
    if(o.body!=nullptr && o.body->mesh!=nullptr) {
      QueryResult info;
      info.body = o.body;
      fillMaterial(info,*native.GetShape(),hit.mSubShapeID2);
      o.material = info.material;
      o.sector   = info.sector;
      }
    ret.push_back(o);
    }
  return ret;
  }

std::vector<CollisionWorld::RagdollContact> CollisionWorld::ragdollContacts(const Ragdoll& rig, float marginCm) const {
  std::vector<RagdollContact> result;
  if(!std::isfinite(marginCm) || marginCm<0.f)
    return result;
  const auto partIds=rig.diagnosticBodyIds();
  struct OtherBodyFilter final : JPH::BodyFilter {
    explicit OtherBodyFilter(const std::vector<uint32_t>& ids):ids(ids) {}
    bool ShouldCollide(const JPH::BodyID& id) const override {
      return std::find(ids.begin(),ids.end(),id.GetIndexAndSequenceNumber())==ids.end();
      }
    const std::vector<uint32_t>& ids;
    } bodyFilter(partIds);
  constexpr uint32_t mask=(1u<<DynamicWorld::C_Landscape)|(1u<<DynamicWorld::C_Object)|
                          (1u<<DynamicWorld::C_Item)|(1u<<DynamicWorld::C_Ragdoll)|
                          (1u<<DynamicWorld::C_LivingBody)|(1u<<DynamicWorld::C_ActiveBody)|
                          (1u<<DynamicWorld::C_MovableVob)|(1u<<DynamicWorld::C_Fragment);
  QueryLayerFilter layerFilter(mask);
  QueryBroadPhaseFilter broadFilter(mask);
  JPH::CollideShapeSettings settings;
  settings.mActiveEdgeMode=JPH::EActiveEdgeMode::CollideOnlyWithActive;
  settings.mMaxSeparationDistance=toMeters(marginCm);
  for(const auto partId:partIds) {
    JPH::RMat44 com;
    JPH::ShapeRefC shape;
    bool sleeping=false;
    {
      JPH::BodyLockRead lock(impl->system.GetBodyLockInterface(),JPH::BodyID(partId));
      if(!lock.Succeeded()) continue;
      com=lock.GetBody().GetCenterOfMassTransform();
      shape=lock.GetBody().GetShape();
      sleeping=!lock.GetBody().IsActive();
      }
    const auto base=com.GetTranslation();
    JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> collector;
    impl->system.GetNarrowPhaseQuery().CollideShape(shape,JPH::Vec3::sOne(),com,settings,base,collector,
                                                 broadFilter,layerFilter,bodyFilter);
    for(const auto& hit:collector.mHits) {
      JPH::BodyLockRead lock(impl->system.GetBodyLockInterface(),hit.mBodyID2);
      if(!lock.Succeeded()) continue;
      RagdollContact contact;
      contact.bone=rig.boneForBodyId(partId);
      contact.partBodyId=partId;
      contact.supportBodyId=hit.mBodyID2.GetIndexAndSequenceNumber();
      contact.category=int(lock.GetBody().GetObjectLayer());
      contact.sleeping=sleeping;
      contact.depth=hit.mPenetrationDepth*100.f;
      contact.axis=toCentimeters(hit.mPenetrationAxis.NormalizedOr(JPH::Vec3::sZero())*.01f);
      contact.point1=toCentimeters(base+hit.mContactPointOn1);
      contact.point2=toCentimeters(base+hit.mContactPointOn2);
      // Native rigs deliberately have no CollisionBody payload. Resolve only
      // owned wrappers, rather than interpreting arbitrary body user data.
      for(auto* body:bodies)
        if(body->id==hit.mBodyID2) {contact.body=body;break;}
      if(contact.body!=nullptr && contact.body->mesh!=nullptr) {
        const auto triangle=Physics::triangleData(*lock.GetBody().GetShape(),hit.mSubShapeID2);
        JPH::Vec3 vertices[3];
        if(contact.body->mesh->triangleVertices(triangle,vertices)) {
          const auto transform=lock.GetBody().GetWorldTransform();
          const auto point=toMeters(contact.point1);
          for(auto& vertex:vertices)
            vertex=JPH::Vec3(transform*(vertex*contact.body->scale))-point;
          JPH::uint32 feature=0;
          const auto nearest=JPH::ClosestPoint::GetClosestPointOnTriangle(vertices[0],vertices[1],vertices[2],feature);
          contact.sourceDistance=nearest.Length()*100.f;
          contact.sourcePoint=toCentimeters(nearest+point);
          contact.sourceTriangle=triangle;
          }
        }
      result.push_back(contact);
      }
    }
  return result;
  }

bool CollisionWorld::hasCollision(const JPH::Shape* shape, const Tempest::Vec3& center,
                                  Tempest::Vec3& normal, Interactive*& vob) const {
  if(shape==nullptr)
    return false;
  JPH::CollideShapeSettings settings;
  settings.mBackFaceMode = JPH::EBackFaceMode::CollideWithBackFaces;
  settings.mActiveEdgeMode = JPH::EActiveEdgeMode::CollideWithAll;
  const auto bounds = shape->GetLocalBounds();
  const float disc = angularMotionDisc(*shape);
  const float radius = shape->GetInnerRadius();
  // The old contact callback also received separated contacts: its final GJK
  // distance bound included the capsule radius and relative breaking threshold.
  // Keep that query tolerance without changing the physical capsule.
  settings.mMaxSeparationDistance = radius+disc*0.02f;
  JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> collector;
  constexpr uint32_t mask = (1u<<DynamicWorld::C_Landscape)|(1u<<DynamicWorld::C_Object)|(1u<<DynamicWorld::C_Ragdoll)|
                            (1u<<DynamicWorld::C_MovableQuery);
  QueryLayerFilter filter(mask);
  QueryBroadPhaseFilter broadFilter(mask);
  const auto position = toMeters(center);
  impl->system.GetNarrowPhaseQuery().CollideShape(shape,JPH::Vec3::sOne(),JPH::RMat44::sTranslation(position),
                                               settings,position,collector,broadFilter,filter);
  auto sum = JPH::Vec3::sZero();
  bool hasSourceWitnessNormal = false;
  size_t count = 0;
  Interactive* hitVob = nullptr;
  for(const auto& hit:collector.mHits) {
    JPH::BodyLockRead lock(impl->system.GetBodyLockInterface(),hit.mBodyID2);
    if(!lock.Succeeded())
      continue;
    const auto& native = lock.GetBody();
    // The animation controller collides with the actual corpse surface. The
    // legacy positive-gap compatibility rule applies only to the old world.
    if(native.GetObjectLayer()==DynamicWorld::C_Ragdoll && hit.mPenetrationDepth<=0.f)
      continue;
    auto* body = reinterpret_cast<CollisionBody*>(native.GetUserData());
    const auto* baseShape = body==nullptr ? native.GetShape() : body->baseShape.GetPtr();
    const float contactDistance = radius+std::min(disc,angularMotionDisc(*baseShape))*0.02f;
    if(-hit.mPenetrationDepth>=contactDistance)
      continue;
    auto contactNormal = -hit.mPenetrationAxis.NormalizedOr(JPH::Vec3::sAxisY());
    if(body!=nullptr && body->mesh!=nullptr) {
      JPH::Vec3 vertices[3];
      const auto triangle = Physics::triangleData(*native.GetShape(),hit.mSubShapeID2);
      if(!body->mesh->triangleVertices(triangle,vertices))
        continue;
      JPH::AABox triangleBounds;
      for(const auto& vertex:vertices)
        triangleBounds.Encapsulate(vertex);
      // HumShape ignored the transform basis when building its mesh-local AABB.
      // Bullet's inverse transposed the supplied basis, including its scale.
      // Preserve this original candidate box before accepting proximity hits;
      // Jolt's separation tolerance expands both broad and triangle queries.
      const auto inverse = native.GetWorldTransform().InversedRotationTranslation();
      const auto localCenter = body->scale*JPH::Vec3(inverse*position);
      if(!triangleBounds.Overlaps(JPH::AABox(localCenter+bounds.mMin,localCenter+bounds.mMax)))
        continue;
      if(hit.mPenetrationDepth<0.f) {
        auto sourceNormal = JPH::Vec3::sZero();
        if(!npcContactCandidate(*shape,position,native,body->scale,vertices,contactDistance,&sourceNormal))
          continue;
        if(!sourceNormal.IsNearZero()) {
          contactNormal = sourceNormal;
          hasSourceWitnessNormal = true;
          }
        }
      }
    sum += contactNormal;
    ++count;
    if(body!=nullptr && body->categoryId==DynamicWorld::C_Object)
      hitVob = body->pointerType==DynamicWorld::P_Interactive ? reinterpret_cast<Interactive*>(body->pointer) : nullptr;
    }
  if(count==0)
    return false;
  if(hasSourceWitnessNormal) {
    normal = Tempest::Vec3::normalize(Tempest::Vec3(sum.GetX(),sum.GetY(),sum.GetZ()));
    }
  else {
    sum = sum.NormalizedOr(JPH::Vec3::sZero());
    normal = Tempest::Vec3(sum.GetX(),sum.GetY(),sum.GetZ());
    }
  vob = hitVob;
  return true;
  }

void CollisionWorld::configureMeleeBench(int substeps) {
  const auto* flag=std::getenv("OPENGOTHIC_MELEE_BENCH");
  if(flag==nullptr || flag[0]==0 || std::string_view(flag)=="0")
    throw std::runtime_error("Melee physics probe requires OPENGOTHIC_MELEE_BENCH");
  if(substeps!=1 && substeps!=2) throw std::runtime_error("Melee physics probe requires one or two substeps");
  impl->meleeSubsteps=substeps;
  }

void CollisionWorld::updateMeleeBlade(const Npc* owner, const Tempest::Matrix4x4& hand, float lengthCm) {
  std::lock_guard<std::mutex> guard(impl->meleeMutex);
  if(impl->meleeSubsteps==0) throw std::runtime_error("Melee blade probe is not configured");
  if(owner==nullptr || !std::isfinite(lengthCm) || lengthCm<=4.f)
    throw std::runtime_error("Melee blade probe requires an equipped weapon");
  const auto* m=hand.data();
  // Stock sword meshes extend along local -X. The native capsule's
  // cylinder is centred halfway along the blade, with 2 cm rounded ends.
  const auto axis=JPH::Vec3(m[4],m[5],m[6]).Normalized();
  const auto x=JPH::Vec3(m[0],m[1],m[2]).Normalized();
  const auto z=JPH::Vec3(m[8],m[9],m[10]).Normalized();
  const auto rotation=JPH::Mat44(JPH::Vec4(x,0),JPH::Vec4(axis,0),JPH::Vec4(z,0),JPH::Vec3::sZero()).GetQuaternion().Normalized();
  const auto bladeRotation=rotation*JPH::Quat::sRotation(JPH::Vec3::sAxisZ(),JPH::JPH_PI*.5f);
  const auto position=JPH::RVec3(m[12]*.01f,m[13]*.01f,m[14]*.01f)-x*(lengthCm*.005f);
  auto it=std::find_if(impl->meleeBlades.begin(),impl->meleeBlades.end(),[owner](const auto& b) { return b.owner==owner; });
  if(it==impl->meleeBlades.end()) {
    Impl::MeleeBlade blade;
    blade.owner=owner;blade.position=position;blade.rotation=bladeRotation;
    blade.shape=new JPH::CapsuleShape(lengthCm*.005f-.02f,.02f);
    JPH::BodyCreationSettings settings(blade.shape,position,bladeRotation,JPH::EMotionType::Kinematic,DynamicWorld::C_Cloth);
    settings.mIsSensor=true;
    settings.mAllowSleeping=false;
    blade.body=impl->system.GetBodyInterface().CreateAndAddBody(settings,JPH::EActivation::Activate);
    if(blade.body.IsInvalid()) throw std::runtime_error("Melee blade probe body creation failed");
    impl->meleeBlades.push_back(std::move(blade));it=impl->meleeBlades.end()-1;
    std::fprintf(stderr,"[melee-bench] blade length_cm=%.1f\n",double(lengthCm));
    }
  it->targetPosition=position;it->targetRotation=bladeRotation;
  }

void CollisionWorld::removeMeleeBlade(const Npc* owner) {
  std::lock_guard<std::mutex> guard(impl->meleeMutex);
  auto it=std::find_if(impl->meleeBlades.begin(),impl->meleeBlades.end(),[owner](const auto& blade) { return blade.owner==owner; });
  if(it==impl->meleeBlades.end()) return;
  impl->system.GetBodyInterface().RemoveBody(it->body);
  impl->system.GetBodyInterface().DestroyBody(it->body);
  impl->meleeBlades.erase(it);
  }

void CollisionWorld::updateShadowBody(Npc* owner, const std::vector<MeleeBodyCapsule>& capsules) {
  if(owner==nullptr) throw std::runtime_error("Melee shadow body requires an owner");
  JPH::AABox bounds;
  for(const auto& capsule:capsules) {
    const auto from=toMeters(capsule.from),to=toMeters(capsule.to);
    if(std::isfinite(from.LengthSq()) && std::isfinite(to.LengthSq()) &&
       std::isfinite(capsule.radiusCm) && capsule.radiusCm>0.f) {
      const auto radius=JPH::Vec3::sReplicate(capsule.radiusCm*.01f);
      bounds.Encapsulate(JPH::AABox(JPH::Vec3::sMin(from,to)-radius,JPH::Vec3::sMax(from,to)+radius));
      }
    else throw std::runtime_error("Invalid melee shadow body capsule");
    }
  std::lock_guard<std::mutex> guard(impl->shadowMutex);
  auto it=std::find_if(impl->shadowBodies.begin(),impl->shadowBodies.end(),[owner](const auto& body) { return body.owner==owner; });
  if(it==impl->shadowBodies.end()) {
    impl->shadowBodies.push_back({owner,capsules,bounds});
    }
  else {
    it->capsules=capsules;
    it->bounds=bounds;
    }
  }

void CollisionWorld::removeShadowBody(const Npc* owner) {
  std::lock_guard<std::mutex> guard(impl->shadowMutex);
  std::erase_if(impl->shadowBodies,[owner](const auto& body) { return body.owner==owner; });
  // NPC destruction can precede a different attacker's journal flush.
  for(auto& blade:impl->shadowBlades) {
    std::erase(blade.hit,owner);
    std::erase_if(blade.contacts,[owner](const auto& contact) { return contact.npc==owner; });
    }
  }

void CollisionWorld::updateShadowBlade(const Npc* owner, uint64_t swingId,
                                      const std::vector<MeleeBladeSegment>& segments, uint64_t timeMs,
                                      double activeBeginMs, double activeEndMs, float walkableCos,
                                      Tempest::Vec3 forward) {
  if(owner==nullptr || segments.empty()) throw std::runtime_error("Melee shadow blade requires an owner and geometry");
  for(const auto& segment:segments) {
    const auto from=toMeters(segment.from),to=toMeters(segment.to),edge=toMeters(segment.edgeNormal);
    if(!std::isfinite(from.LengthSq()) || !std::isfinite(to.LengthSq()) || !std::isfinite(edge.LengthSq()) ||
       !std::isfinite(segment.radiusCm) || segment.radiusCm<=0.f)
      throw std::runtime_error("Invalid melee shadow blade capsule");
    }
  std::lock_guard<std::mutex> guard(impl->shadowMutex);
  auto it=std::find_if(impl->shadowBlades.begin(),impl->shadowBlades.end(),[owner](const auto& blade) { return blade.owner==owner; });
  if(it==impl->shadowBlades.end()) {
    Impl::ShadowBlade blade;
    blade.owner=owner;
    blade.swing=swingId;
    blade.pose=segments;
    blade.poseTimeMs=double(timeMs);
    impl->shadowBlades.push_back(std::move(blade));
    it=impl->shadowBlades.end()-1;
    }
  if(it->swing!=swingId) {
    it->swing=swingId;
    it->blocked=false;
    it->groundRecorded=false;
    it->hit.clear();
    it->contacts.clear();
    }
  if(it->pose.size()!=segments.size() || double(timeMs)<=it->poseTimeMs) {
    // Same-timestamp transform updates are not motion through elapsed time.
    it->pose=segments;
    it->poseTimeMs=double(timeMs);
    }
  it->target=segments;
  it->targetTimeMs=double(timeMs);
  it->activeBeginMs=activeBeginMs;
  it->activeEndMs=activeEndMs;
  it->forward=forward;
  it->walkableCos=walkableCos;
  impl->hasShadowBlades.store(true,std::memory_order_release);
  }

std::vector<MeleeBladeContact> CollisionWorld::takeShadowBladeContacts(const Npc* owner) {
  std::lock_guard<std::mutex> guard(impl->shadowMutex);
  for(auto& blade:impl->shadowBlades)
    if(blade.owner==owner) {
      std::vector<MeleeBladeContact> result;
      result.swap(blade.contacts);
      return result;
      }
  return {};
  }

void CollisionWorld::removeShadowBlade(const Npc* owner) {
  std::lock_guard<std::mutex> guard(impl->shadowMutex);
  std::erase_if(impl->shadowBlades,[owner](const auto& blade) { return blade.owner==owner; });
  impl->hasShadowBlades.store(!impl->shadowBlades.empty(),std::memory_order_release);
  }

void CollisionWorld::flushShadowBlades() {
  // Consume poses published after the last physics tick without advancing any
  // native bodies, animation, game clocks or physics side effects.
  const auto started=std::chrono::steady_clock::now();
  stepShadowBlades(1.f);
  const float elapsed=std::chrono::duration<float,std::milli>(std::chrono::steady_clock::now()-started).count();
  impl->stepMilliseconds += elapsed;
  // The normal frame sample has already been published by tick(). Include this
  // query in that frame's physics budget, without inventing a fixed physics step.
  if(!impl->meleeOutput.empty() && !impl->meleeSamples.empty()) {
    impl->meleeSamples.back().milliseconds += elapsed;
    impl->meleeSamples.back().castMs += elapsed;
    }
  }

float CollisionWorld::meleeBladeAdvance(const std::vector<MeleeBladeSegment>& blades, const std::vector<MeleeBodyCapsule>& target,
                                      const Tempest::Vec3& direction, float maxAdvanceCm) const {
  const auto travel=toMeters(direction).NormalizedOr(JPH::Vec3::sZero())*(maxAdvanceCm*.01f);
  JPH::ShapeCastSettings settings;
  settings.mReturnDeepestPoint=true;
  float advance=std::numeric_limits<float>::infinity();
  for(const auto& blade:blades) {
    const auto from=toMeters(blade.from),to=toMeters(blade.to),axis=to-from,center=(from+to)*.5f;
    const JPH::CapsuleShape shape(std::max(axis.Length()*.5f,.00001f),blade.radiusCm*.01f);
    const auto rotation=JPH::Quat::sFromTo(JPH::Vec3::sAxisY(),axis.NormalizedOr(JPH::Vec3::sAxisY()));
    for(const auto& capsule:target) {
      const auto a=toMeters(capsule.from),b=toMeters(capsule.to),targetAxis=b-a,targetCenter=(a+b)*.5f;
      const JPH::CapsuleShape targetShape(std::max(targetAxis.Length()*.5f,.00001f),capsule.radiusCm*.01f);
      const auto targetRotation=JPH::Quat::sFromTo(JPH::Vec3::sAxisY(),targetAxis.NormalizedOr(JPH::Vec3::sAxisY()));
      const JPH::ShapeCast cast(&shape,JPH::Vec3::sOne(),
          JPH::Mat44::sRotationTranslation(rotation,center-targetCenter),travel);
      JPH::ClosestHitCollisionCollector<JPH::CastShapeCollector> hit;
      JPH::CollisionDispatch::sCastShapeVsShapeWorldSpace(cast,settings,&targetShape,JPH::Vec3::sOne(),{},
          JPH::Mat44::sRotation(targetRotation),{},{},hit);
      if(hit.HadHit()) {
        advance=std::min(advance,std::clamp(hit.mHit.mFraction,0.f,1.f)*maxAdvanceCm);
        if(advance==0.f)
          return 0.f;
        }
      }
    }
  return advance;
  }

void CollisionWorld::accountMeleeReachTime(float milliseconds) {
  // Reach previews run in native AI before tick(), like CharacterVirtual queries.
  // Charge the entire caller's pose replay and capsule casts to physics_ms.
  impl->characterMilliseconds+=milliseconds;
  }

void CollisionWorld::stepShadowBlades(float fraction, bool bodiesLocked) {
  if(!impl->hasShadowBlades.load(std::memory_order_acquire)) return;
  std::lock_guard<std::mutex> guard(impl->shadowMutex);
  constexpr uint32_t mask=(1u<<DynamicWorld::C_Landscape)|(1u<<DynamicWorld::C_Object)|
                          (1u<<DynamicWorld::C_Item)|(1u<<DynamicWorld::C_MovableVob)|(1u<<DynamicWorld::C_Fragment);
  const QueryLayerFilter layers(mask);
  const QueryBroadPhaseFilter broad(mask);
  JPH::ShapeCastSettings settings;
  settings.SetBackFaceMode(JPH::EBackFaceMode::CollideWithBackFaces);
  settings.mReturnDeepestPoint=true;
  settings.mActiveEdgeMode=JPH::EActiveEdgeMode::CollideWithAll;
  const auto& query=bodiesLocked ? impl->system.GetNarrowPhaseQueryNoLock() : impl->system.GetNarrowPhaseQuery();
  struct Pose {
    JPH::Vec3 center,edge;
    JPH::Quat rotation;
    float halfLength,radius;
    };
  auto pose=[](const MeleeBladeSegment& segment) {
    const auto from=toMeters(segment.from),to=toMeters(segment.to),axis=to-from;
    return Pose{(from+to)*.5f,JPH::Vec3(segment.edgeNormal.x,segment.edgeNormal.y,segment.edgeNormal.z),
                JPH::Quat::sFromTo(JPH::Vec3::sAxisY(),axis.NormalizedOr(JPH::Vec3::sAxisY())),axis.Length()*.5f,segment.radiusCm*.01f};
    };
  auto interpolate=[](const Pose& from,const Pose& to,float t) {
    const auto delta=JPH::Quat::sFromTo(from.rotation*JPH::Vec3::sAxisY(),to.rotation*JPH::Vec3::sAxisY());
    const auto rotation=JPH::Quat::sIdentity().SLERP(delta,t)*from.rotation;
    const float length=from.halfLength+(to.halfLength-from.halfLength)*t;
    const auto a=from.center-from.rotation*JPH::Vec3(0,from.halfLength,0);
    const auto b=to.center-to.rotation*JPH::Vec3(0,to.halfLength,0);
    // The first endpoint is the grip. Its translation stays linear while the
    // blade rotates about it, retaining the tip's curved path through a swing.
    return Pose{a+(b-a)*t+rotation*JPH::Vec3(0,length,0),from.edge+(to.edge-from.edge)*t,
                rotation,length,from.radius+(to.radius-from.radius)*t};
    };
  auto endpoints=[](const Pose& value,float bladeFraction) {
    return value.center+value.rotation*JPH::Vec3(0,(2.f*bladeFraction-1.f)*value.halfLength,0);
    };
  for(auto& blade:impl->shadowBlades) {
    struct TimedContact { float time; MeleeBladeContact contact; };
    std::vector<TimedContact> contacts;
    // Animation publications have their own clock. Native fixed steps divide
    // that interval, but must not supply its duration (including no-step frames).
    const double endTime=blade.poseTimeMs+(blade.targetTimeMs-blade.poseTimeMs)*fraction;
    const double duration=endTime-blade.poseTimeMs;
    const float first=duration>0 ? float(std::clamp((blade.activeBeginMs-blade.poseTimeMs)/duration,0.,1.)) : 0.f;
    const float last=duration>0 ? float(std::clamp((blade.activeEndMs-blade.poseTimeMs)/duration,0.,1.)) : 0.f;
    const float seconds=float(duration*.001)*(last-first);
    for(size_t capsule=0;capsule<blade.pose.size();++capsule) {
      const auto previous=pose(blade.pose[capsule]),target=pose(blade.target[capsule]);
      const auto current=interpolate(previous,target,fraction);
      const auto start=interpolate(previous,current,first),end=interpolate(previous,current,last);
      if(last>first && !blade.blocked) {
        const float angle=2.f*std::acos(std::clamp(std::abs(start.rotation.Dot(end.rotation)),0.f,1.f));
        const int subdivisions=std::max(1,int(std::ceil(angle/(5.f*JPH::JPH_PI/180.f))));
        for(int sub=0;sub<subdivisions;++sub) {
          const float t0=float(sub)/float(subdivisions),t1=float(sub+1)/float(subdivisions);
          const auto from=interpolate(start,end,t0),to=interpolate(start,end,t1),middle=interpolate(start,end,(t0+t1)*.5f);
          const float halfLength=std::max(from.halfLength,to.halfLength);
          // A translated capsule has fixed orientation. Enclose every point of
          // this <=5 degree rotation. The second term covers orientation; the
          // third covers the centre's arc around the interpolated grip.
          const float halfAngle=angle/(2.f*float(subdivisions));
          const float radius=std::max(from.radius,to.radius)+halfLength*(std::sin(halfAngle)+1.f-std::cos(halfAngle));
          const JPH::CapsuleShape sweep(std::max(halfLength,.00001f),radius);
          const auto transform=JPH::RMat44::sRotationTranslation(middle.rotation,JPH::RVec3(from.center));
          const JPH::RShapeCast cast(&sweep,JPH::Vec3::sOne(),transform,to.center-from.center);
          auto record=[&](const JPH::ShapeCastResult& hit,JPH::Vec3Arg base,Npc* npc,size_t bone,uint8_t zone) {
            const float local=std::clamp(hit.mFraction,0.f,1.f);
            const float time=t0+(t1-t0)*local;
            const auto at=interpolate(start,end,time);
            const auto point=base+hit.mContactPointOn2;
            const auto axis=at.rotation*JPH::Vec3::sAxisY();
            const float location=at.halfLength>0.f ? std::clamp((point-at.center).Dot(axis)/(2.f*at.halfLength)+.5f,0.f,1.f) : .5f;
            const auto normal=-hit.mPenetrationAxis.NormalizedOr(JPH::Vec3::sAxisY());
            const auto edge=at.edge.NormalizedOr(JPH::Vec3::sAxisY());
            MeleeBladeContact contact;
            contact.npc=npc;contact.bone=bone;contact.zone=zone;
            contact.point=toCentimeters(point);
            contact.normal={normal.GetX(),normal.GetY(),normal.GetZ()};
            contact.bladeFraction=location;
            const auto velocity=seconds>0.f ? (endpoints(to,location)-endpoints(from,location))*100.f/(seconds*(t1-t0)) : JPH::Vec3::sZero();
            contact.velocityCmS={velocity.GetX(),velocity.GetY(),velocity.GetZ()};
            contact.speedCmS=seconds>0.f ? (endpoints(to,location)-endpoints(from,location)).Length()*100.f/(seconds*(t1-t0)) : 0.f;
            contact.surfaceAngleDeg=std::acos(std::clamp(std::abs(edge.Dot(normal)),0.f,1.f))*180.f/JPH::JPH_PI;
            contact.world=npc==nullptr;
            contact.step=impl->simulationStep;
            contacts.push_back({time,contact});
            };
          JPH::AllHitCollisionCollector<JPH::CastShapeCollector> worldHits;
          query.CastShape(cast,settings,JPH::RVec3(from.center),worldHits,broad,layers);
          for(const auto& hit:worldHits.mHits) {
            const JPH::BodyLockInterface& locks = bodiesLocked ?
                static_cast<const JPH::BodyLockInterface&>(impl->system.GetBodyLockInterfaceNoLock()) : impl->system.GetBodyLockInterface();
            JPH::BodyLockRead lock(locks,hit.mBodyID2);
            if(!lock.Succeeded()) continue;
            const auto& body=lock.GetBody();
            // Small loose objects cannot arrest a human swing.
            const auto layer=body.GetObjectLayer();
            const auto extent=body.GetWorldSpaceBounds().GetSize();
            if((layer==DynamicWorld::C_Item || layer==DynamicWorld::C_Fragment || layer==DynamicWorld::C_MovableVob) &&
               std::max({extent.GetX(),extent.GetY(),extent.GetZ()})<.5f) continue;
            record(hit,from.center,nullptr,size_t(-1),0);
            auto& contact=contacts.back().contact;
            const auto normal=body.GetWorldSpaceSurfaceNormal(hit.mSubShapeID2,JPH::RVec3(contact.point.x*.01f,contact.point.y*.01f,contact.point.z*.01f));
            contact.normal={normal.GetX(),normal.GetY(),normal.GetZ()};
            contact.stopsSwing=normal.GetY()<blade.walkableCos || layer==DynamicWorld::C_Item ||
                               layer==DynamicWorld::C_Fragment || layer==DynamicWorld::C_MovableVob;
            // The blow stops at a wall; the wind-up and the follow-through pass it, as the
            // original passes every wall: a blade going back from the facing, or lifted.
            const auto& v=contact.velocityCmS;
            const bool back=v.x*blade.forward.x+v.z*blade.forward.z<0.f;
            const bool lifted=v.y>.5f*contact.speedCmS;
            if(contact.stopsSwing && blade.forward.quadLength()>0.f && (back || lifted)) {
              contact.stopsSwing=false;
              contact.windup=true;
              }
            auto* object=reinterpret_cast<CollisionBody*>(body.GetUserData());
            if(object!=nullptr)
              contact.material=object->mesh!=nullptr ? object->mesh->materialId(Physics::triangleData(*body.GetShape(),hit.mSubShapeID2)) : object->noiseSurface;
            }

          auto bounds=cast.mShapeWorldBounds;
          bounds.Encapsulate(JPH::AABox(bounds.mMin+cast.mDirection,bounds.mMax+cast.mDirection));
          for(const auto& body:impl->shadowBodies) {
            if(body.owner==blade.owner || !bounds.Overlaps(body.bounds) ||
               std::find(blade.hit.begin(),blade.hit.end(),body.owner)!=blade.hit.end()) continue;
            for(const auto& targetCapsule:body.capsules) {
              const auto a=toMeters(targetCapsule.from),b=toMeters(targetCapsule.to),axis=b-a,center=(a+b)*.5f;
              const auto extent=JPH::Vec3::sReplicate(targetCapsule.radiusCm*.01f);
              if(!bounds.Overlaps(JPH::AABox(JPH::Vec3::sMin(a,b)-extent,JPH::Vec3::sMax(a,b)+extent))) continue;
              const JPH::CapsuleShape shape(std::max(axis.Length()*.5f,.00001f),targetCapsule.radiusCm*.01f);
              const auto rotation=JPH::Quat::sFromTo(JPH::Vec3::sAxisY(),axis.NormalizedOr(JPH::Vec3::sAxisY()));
              const JPH::ShapeCast localCast(&sweep,JPH::Vec3::sOne(),
                  JPH::Mat44::sRotationTranslation(middle.rotation,from.center-center),cast.mDirection);
              JPH::ClosestHitCollisionCollector<JPH::CastShapeCollector> hit;
              JPH::CollisionDispatch::sCastShapeVsShapeWorldSpace(localCast,settings,&shape,JPH::Vec3::sOne(),{},
                  JPH::Mat44::sRotation(rotation),{},{},hit);
              if(hit.HadHit()) record(hit.mHit,center,body.owner,targetCapsule.bone,targetCapsule.zone);
              }
            }
          }
        }
      const auto axis=current.rotation*JPH::Vec3(0,current.halfLength,0);
      blade.pose[capsule]={toCentimeters(current.center-axis),toCentimeters(current.center+axis),
                          {current.edge.GetX(),current.edge.GetY(),current.edge.GetZ()},current.radius*100.f};
      }
    blade.poseTimeMs=endTime;
    std::stable_sort(contacts.begin(),contacts.end(),[](const auto& a,const auto& b) {
      if(a.time!=b.time) return a.time<b.time;
      if(a.contact.world!=b.contact.world) return a.contact.world;
      return a.contact.bone<b.contact.bone;
      });
    for(auto& value:contacts) {
      auto& contact=value.contact;
      if(contact.world) {
        if(contact.stopsSwing) {
          blade.contacts.push_back(contact);
          blade.blocked=true;
          break;
          }
        if(!blade.groundRecorded) {
          blade.contacts.push_back(contact);
          blade.groundRecorded=true;
          }
        continue;
        }
      if(std::find(blade.hit.begin(),blade.hit.end(),contact.npc)!=blade.hit.end()) continue;
      blade.hit.push_back(contact.npc);
      blade.contacts.push_back(contact);
      }
    }
  }

void CollisionWorld::stepMeleeBlades(float seconds, float fraction) {
  std::lock_guard<std::mutex> guard(impl->meleeMutex);
  const auto started=std::chrono::steady_clock::now();
  auto& sample=impl->meleeSample;
  constexpr uint32_t mask=(1u<<DynamicWorld::C_Landscape)|(1u<<DynamicWorld::C_Object)|
                          (1u<<DynamicWorld::C_Item)|(1u<<DynamicWorld::C_MovableVob)|
                          (1u<<DynamicWorld::C_Ragdoll)|(1u<<DynamicWorld::C_LivingBody)|(1u<<DynamicWorld::C_ActiveBody);
  const QueryLayerFilter layers(mask);
  const QueryBroadPhaseFilter broad(mask);
  JPH::ShapeCastSettings settings;
  settings.SetBackFaceMode(JPH::EBackFaceMode::CollideWithBackFaces);
  settings.mReturnDeepestPoint=true;
  struct ExcludeOwner final : JPH::BodyFilter {
    std::vector<uint32_t> ids;
    bool ShouldCollide(const JPH::BodyID& id) const override {
      return std::find(ids.begin(),ids.end(),id.GetIndexAndSequenceNumber())==ids.end();
      }
    };
  for(auto& blade:impl->meleeBlades) {
    ExcludeOwner filter;
    for(const auto* rig:ragdolls)
      if(rig->noiseNpc()==blade.owner) filter.ids=rig->diagnosticBodyIds();
    const auto end=blade.position+(blade.targetPosition-blade.position)*fraction;
    const auto rotation=blade.rotation.SLERP(blade.targetRotation,fraction);
    const float angle=2.f*std::acos(std::clamp(std::abs(blade.rotation.Dot(rotation)),0.f,1.f));
    const int segments=std::max(1,int(std::ceil(angle/(5.f*JPH::JPH_PI/180.f))));
    // CastShape translates a fixed orientation. Cover rotation conservatively:
    // each <=5 degree interval uses its middle orientation and a radius band
    // enclosing the whole endpoint arc. No sampled thin rotational gaps.
    const float radius=blade.shape->GetRadius()+blade.shape->GetHalfHeightOfCylinder()*std::sin(angle/(2.f*float(segments)));
    const JPH::CapsuleShape sweep(blade.shape->GetHalfHeightOfCylinder(),radius);
    for(int segment=0;segment<segments;++segment) {
      const float t0=float(segment)/float(segments),t1=float(segment+1)/float(segments);
      const auto from=blade.position+(end-blade.position)*t0;
      const auto to=blade.position+(end-blade.position)*t1;
      const auto q=blade.rotation.SLERP(rotation,(t0+t1)*.5f);
      JPH::RShapeCast cast(&sweep,JPH::Vec3::sOne(),JPH::RMat44::sRotationTranslation(q,from),JPH::Vec3(to-from));
      JPH::AllHitCollisionCollector<JPH::CastShapeCollector> hits;
      impl->system.GetNarrowPhaseQuery().CastShape(cast,settings,JPH::RVec3::sZero(),hits,broad,layers,filter);
      ++sample.casts;
      for(const auto& hit:hits.mHits) {
        JPH::BodyLockRead lock(impl->system.GetBodyLockInterface(),hit.mBodyID2);
        if(!lock.Succeeded()) continue;
        const auto layer=lock.GetBody().GetObjectLayer();
        if(layer==DynamicWorld::C_LivingBody || layer==DynamicWorld::C_ActiveBody || layer==DynamicWorld::C_Ragdoll) ++sample.bodyHits;
        else ++sample.worldHits;
        }
      }
    impl->system.GetBodyInterface().MoveKinematic(blade.body,end,rotation,seconds);
    blade.position=end;blade.rotation=rotation;
    }
  sample.castMs+=std::chrono::duration<float,std::milli>(std::chrono::steady_clock::now()-started).count();
  }

void CollisionWorld::measureMeleeBench(std::string path) {
  if(impl->meleeSubsteps==0) throw std::runtime_error("Melee measurement requires a configured probe");
  impl->meleeOutput=std::move(path);impl->meleeSamples.clear();impl->meleeSamples.reserve(20000);
  impl->meleeSteps.clear();impl->meleeSteps.reserve(4000);
  }

void CollisionWorld::reportMeleeBench() {
  auto* file=std::fopen(impl->meleeOutput.c_str(),"w");
  if(file==nullptr) throw std::runtime_error("Cannot write melee physics measurements");
  std::fprintf(file,"step,physics_ms,solve_ms,cast_ms,updates,casts,body_hits,world_hits,dynamic_parts,blades,max_linear_cm_s,max_angular_rad_s,pose_rms_cm,pose_max_cm,max_foot_drop_cm,max_foot_penetration_cm,floor_samples,floor_misses,finite\n");
  for(const auto& s:impl->meleeSamples)
    std::fprintf(file,"%llu,%.9g,%.9g,%.9g,%zu,%zu,%zu,%zu,%zu,%zu,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%zu,%zu,%d\n",
      (unsigned long long)s.step,double(s.milliseconds),double(s.solveMs),double(s.castMs),s.updates,s.casts,s.bodyHits,s.worldHits,
      s.parts,s.blades,double(s.linear),double(s.angular),double(s.poseRms),double(s.poseMax),double(s.footDrop),double(s.footPenetration),s.floorSamples,s.floorMisses,int(s.finite));
  std::fclose(file);
  file=std::fopen((impl->meleeOutput+".steps.csv").c_str(),"w");
  if(file==nullptr) throw std::runtime_error("Cannot write melee fixed-step measurements");
  std::fprintf(file,"step,physics_ms,cast_ms,substeps\n");
  for(const auto& s:impl->meleeSteps)
    std::fprintf(file,"%llu,%.9g,%.9g,%d\n",(unsigned long long)s.step,double(s.milliseconds),double(s.castMs),s.substeps);
  std::fclose(file);impl->meleeOutput.clear();
  }

void CollisionWorld::sampleMeleeBench() {
  if(impl->meleeOutput.empty()) return;
  const auto started=std::chrono::steady_clock::now();
  auto& sample=impl->meleeSample;
  size_t parts=0;
  float squaredError=0;
  for(const auto* rig:ragdolls) {
    const auto stats=rig->benchStats();
    parts+=stats.dynamicParts;
    sample.linear=std::max(sample.linear,stats.maxLinearCmS);
    sample.angular=std::max(sample.angular,stats.maxAngularRadS);
    sample.poseMax=std::max(sample.poseMax,stats.poseMaxCm);
    sample.footDrop=std::max(sample.footDrop,stats.maxFootDropCm);
    sample.footPenetration=std::max(sample.footPenetration,stats.maxFootPenetrationCm);
    sample.floorSamples+=stats.footFloorSamples;sample.floorMisses+=stats.footFloorMisses;
    sample.finite&=stats.finite;
    squaredError+=stats.poseRmsCm*stats.poseRmsCm*float(stats.dynamicParts);
    }
  sample.parts=std::max(sample.parts,parts);
  sample.poseRms=std::max(sample.poseRms,parts>0 ? std::sqrt(squaredError/float(parts)) : 0.f);
  impl->meleeDiagnosticMs+=std::chrono::duration<float,std::milli>(std::chrono::steady_clock::now()-started).count();
  }

void CollisionWorld::wakeItems() {
  if(!itemWake || !itemWakeCallback)
    return;
  JPH::BodyIDVector active;
  impl->system.GetActiveBodies(JPH::EBodyType::RigidBody,active);
  if(active.empty())
    return;
  constexpr size_t maxQueries=32, maxCreated=8;
  size_t queries=0,created=0,visited=0;
  itemWakeCursor%=active.size();
  while(visited<active.size() && queries<maxQueries && created<maxCreated) {
    const auto id=active[itemWakeCursor];
    itemWakeCursor=(itemWakeCursor+1)%active.size();
    ++visited;
    Tempest::Vec3 min,max;
    {
      JPH::BodyLockRead lock(impl->system.GetBodyLockInterface(),id);
      if(!lock.Succeeded() || !lock.GetBody().IsDynamic())
        continue;
      const auto& native=lock.GetBody();
      const auto bounds=native.GetWorldSpaceBounds();
      min=toCentimeters(bounds.mMin)-Tempest::Vec3(50,50,50);
      max=toCentimeters(bounds.mMax)+Tempest::Vec3(50,50,50);
      }
    ++queries;
    created+=itemWakeCallback(min,max,maxCreated-created);
    }
  }

void CollisionWorld::recoverItems() {
  for(auto* body:rigid) {
    if(body->category()!=DynamicWorld::C_Item)
      continue;
    bool recover=false;
    bool sleeping=false;
    Tempest::Vec3 failedPosition;
    {
      JPH::BodyLockRead lock(impl->system.GetBodyLockInterface(),body->id);
      if(!lock.Succeeded())
        continue;
      const auto& native=lock.GetBody();
      const auto p=native.GetPosition();
      recover=!std::isfinite(p.GetX()) || !std::isfinite(p.GetY()) || !std::isfinite(p.GetZ()) ||
              p.GetY()<bboxMin.GetY()-100.f;
      sleeping=!native.IsActive();
      failedPosition=toCentimeters(p);
      }
    if(!recover) {
      if(sleeping)
        body->itemRestTransform=body->matrix();
      continue;
      }
    recoverItem(*body,failedPosition);
    }
  }

void CollisionWorld::recoverItem(CollisionBody& body, const Tempest::Vec3& failedPosition) {
  auto& native=impl->system.GetBodyInterface();
  const auto& restMatrix=body.itemRestTransform;
  if(!std::isfinite(restMatrix.at(3,0)) || !std::isfinite(restMatrix.at(3,1)) || !std::isfinite(restMatrix.at(3,2))) {
    // A body created from a nonfinite pose has no place to return to;
    // setObjMatrix would send it straight back here.
    native.SetLinearAndAngularVelocity(body.id,JPH::Vec3::sZero(),JPH::Vec3::sZero());
    native.DeactivateBody(body.id);
    return;
    }
  const auto rest=toTransform(body.itemRestTransform);
  body.setObjMatrix(body.itemRestTransform);
  native.SetPositionAndRotation(body.id,rest.position,rest.rotation,JPH::EActivation::DontActivate);
  native.SetLinearAndAngularVelocity(body.id,JPH::Vec3::sZero(),JPH::Vec3::sZero());
  native.DeactivateBody(body.id);
  body.waterInitialized=false;
  body.waterPreserveSleep=true;
  body.inWater=false;
  body.waterRestTime=0;
  body.itemRecovered=true;
  Tempest::Log::i("[physics] item recovered: body=",body.nativeId()," from=",failedPosition.x,",",
                 failedPosition.y,",",failedPosition.z," rest=",body.itemRestTransform.at(3,0),",",
                 body.itemRestTransform.at(3,1),",",body.itemRestTransform.at(3,2));
  }

void CollisionWorld::tick(uint64_t dt) {
  const auto started=std::chrono::steady_clock::now();
  // Bad coordinates must never reach Jolt's integration or water queries.
  recoverItems();
  if(impl->meleeSubsteps!=0) { impl->meleeSample={};impl->meleeDiagnosticMs=0; }
  if(cloth!=nullptr) cloth->prepareFrame(dt);
  constexpr float step = 1.f/60.f;
  bool advanced = false;
  std::vector<PhysicsNoise> splashes;
  if(!rigid.empty() || !ragdolls.empty() || impl->meleeSubsteps!=0) {
    accumulatedTime += float(dt)/1000.f;
    int wholeSteps = 0;
    if(accumulatedTime>=step) {
      wholeSteps = int(accumulatedTime/step);
      // Preserve the previous float scheduling and discard steps beyond the
      // frame budget before applying the maximum of two simulated substeps.
      accumulatedTime -= float(wholeSteps)*step;
      }
    const int count = std::min(wholeSteps,2);
    for(int i=0;i<count;++i) {
      ++impl->simulationStep;
      wakeItems();
      if(impl->meleeSubsteps!=0) {
        const float solveBefore=impl->meleeSample.solveMs,castBefore=impl->meleeSample.castMs;
        const float seconds=step/float(impl->meleeSubsteps);
        for(int sub=0;sub<impl->meleeSubsteps;++sub) {
          const auto solveStart=std::chrono::steady_clock::now();
          prepareBuoyancy(splashes);
          for(auto* body:ragdolls) body->prepareKinematicStep(seconds,1.f/float(impl->meleeSubsteps-sub));
          stepMeleeBlades(seconds,1.f/float(impl->meleeSubsteps-sub));
          if(impl->hasShadowBlades.load(std::memory_order_acquire))
            stepShadowBlades(1.f/float((count-i)*impl->meleeSubsteps-sub));
          impl->system.Update(seconds,1,&impl->allocator,&impl->jobs);
          finishBuoyancy(seconds);
          for(auto* body:ragdolls) body->finishKinematicStep();
          impl->meleeSample.solveMs+=std::chrono::duration<float,std::milli>(std::chrono::steady_clock::now()-solveStart).count();
          ++impl->meleeSample.updates;
          sampleMeleeBench();
          }
        if(!impl->meleeOutput.empty())
          impl->meleeSteps.push_back({impl->simulationStep,impl->meleeSample.solveMs-solveBefore,impl->meleeSample.castMs-castBefore,impl->meleeSubsteps});
        advanced=true;
        continue;
        }
      prepareBuoyancy(splashes);
      for(auto* body:ragdolls) body->prepareKinematicStep(step);
      const bool dynamicRagdolls=std::any_of(ragdolls.begin(),ragdolls.end(),[](const Ragdoll* body) { return !body->isKinematic(); });
      constexpr int fragmentSteps=4;
      bool activeFragments=false;
      auto& nativeBodies=impl->system.GetBodyInterface();
      const float contactDistance=impl->system.GetPhysicsSettings().mSpeculativeContactDistance;
      for(const auto* body:rigid) {
        if(body->category()!=DynamicWorld::C_Fragment || body->isSleeping())
          continue;
        activeFragments=true;
        const auto* shape=body->shape();
        const auto bounds=shape->GetLocalBounds();
        // Local bounds are relative to the native centre of mass. In particular,
        // a cut piece's source-visual origin is not its rotation centre.
        const float radius=JPH::Vec3::sMax(bounds.mMin.Abs(),bounds.mMax.Abs()).Length();
        const float travel=std::min(shape->GetInnerRadius()*.25f,contactDistance*.5f);
        const float maxOmega=std::isfinite(radius) && radius>0.f && std::isfinite(travel) && travel>0.f
                               ? float(fragmentSteps)*travel/(radius*step) : 0.f;
        nativeBodies.SetMaxAngularVelocity(body->id,std::min(.25f*JPH::JPH_PI*60.f,maxOmega));
        // Limit inherited motion before the contact solve as well as impulses
        // generated during it. Jolt clamps again before integrating rotation.
        nativeBodies.SetAngularVelocity(body->id,nativeBodies.GetAngularVelocity(body->id));
        }
      // A capsule stopped by CCD otherwise compresses its connected limb while
      // the rest of the rig finishes the 1/60 s step. Two native collision steps
      // keep that late-contact displacement small. The public fixed-step clock
      // and two-update frame budget stay unchanged.
      // Linear CCD rotates first, then casts translation. A fast piece can
      // rotate through a one-sided terrain seam before that cast sees it.
      // Bound the furthest rotating point to half the speculative-contact band
      // and a quarter inner radius per fragment step. A fixed step count alone
      // cannot stop long pieces rotating through a thin terrain triangle.
      // Sleeping fragments leave the previous simulation cost intact.
      // A dry neighbour may wake a sleeping float during Update. Keep water
      // support available on the remaining substeps without waking the float.
      const bool waterSteps=buoyancy && impl->system.GetNumActiveBodies(JPH::EBodyType::RigidBody)>0 &&
                            (std::any_of(rigid.begin(),rigid.end(),[](const DynamicBody* body) { return body->inWater; }) ||
                             std::any_of(ragdolls.begin(),ragdolls.end(),[](Ragdoll* rig) {
                               const auto& parts=rig->buoyancyParts();
                               return std::any_of(parts.begin(),parts.end(),[](const auto& part) { return part.inWater; });
                               }));
      // The same fixed step can contain four fragment collision steps or two
      // rig steps. Support must be applied before each of them, just like
      // gravity, or still floats acquire a persistent integration velocity.
      const int collisionSteps=activeFragments ? fragmentSteps : dynamicRagdolls ? 2 : 1;
      const bool shadowSteps=impl->hasShadowBlades.load(std::memory_order_acquire);
      int shadowRemaining=(count-i)*collisionSteps;
      if(shadowSteps) {
        if(impl->system.GetNumActiveBodies(JPH::EBodyType::RigidBody)>0)
          impl->shadowStep=[this,&shadowRemaining](float) { stepShadowBlades(1.f/float(shadowRemaining--),true); };
        else
          stepShadowBlades(1.f/float(count-i));
        }
      impl->waterSteps=waterSteps;
      const bool listener=waterSteps || bool(impl->shadowStep);
      if(listener) impl->system.AddStepListener(impl.get());
      impl->system.Update(float(step),collisionSteps,&impl->allocator,&impl->jobs);
      if(listener) impl->system.RemoveStepListener(impl.get());
      impl->shadowStep={};
      impl->waterSteps=false;
      finishBuoyancy(step);
      for(auto* body:ragdolls) body->finishKinematicStep();
      advanced = true;
      }
    }
  if(!advanced) {
    // Individually added bodies deepen Jolt's broad phase until Update maintains
    // its tree. Static worlds and frames below the fixed-step threshold need
    // this maintenance too. A zero delta rebuilds the tree without simulating
    // bodies, advancing contacts or emitting callbacks, at the main tick boundary.
    impl->system.Update(0.f,1,&impl->allocator,&impl->jobs);
    // Headless worlds may have only static geometry and posed HitZones. Query
    // their animation frame without adding a native simulation update.
    if(impl->hasShadowBlades.load(std::memory_order_acquire)) stepShadowBlades(1.f);
    }
  if(cloth!=nullptr) cloth->finishFrame();

  for(const auto& splash:splashes) emitPhysicsNoise(splash);

  std::vector<Impl::Impact> impacts;
  std::vector<Impl::NoiseImpact> noises;
  {
    std::lock_guard<std::mutex> guard(impl->impactMutex);
    impacts.swap(impl->impacts);
    noises.swap(impl->noises);
    }
  auto rigFor=[&](JPH::BodyID id) -> Ragdoll* {
    QueryResult result;
    fillRagdoll(result,id);
    return result.ragdoll;
    };
  // Resolve identities only after all Jolt workers finish. The native sequence
  // number rejects a removed/reused body; NPC handles are weak and user_ptr is
  // cleared by NPC destruction even while the script retains the instance.
  for(const auto& impact:noises) {
    Npc* culprit=nullptr;
    bool knownCulprit=false;
    {
      JPH::BodyLockRead lock(impl->system.GetBodyLockInterface(),impact.other);
      if(lock.Succeeded()) {
        const auto layer=lock.GetBody().GetObjectLayer();
        if(layer==DynamicWorld::C_LivingBody || layer==DynamicWorld::C_ActiveBody || layer==DynamicWorld::C_Ragdoll)
          if(auto* other=rigFor(impact.other)) {
            culprit=other->noiseCulprit();
            knownCulprit=culprit!=nullptr;
            // A stationary NPC struck by an unknown falling body is its
            // victim. Attribute an owner only when that rig moves into it.
            if(culprit==nullptr && impact.otherPushing) culprit=other->noiseNpc();
            }
        }
      }
    if(culprit==nullptr) continue;
    JPH::BodyLockRead lock(impl->system.GetBodyLockInterface(),impact.body);
    if(!lock.Succeeded()) continue;
    const auto layer=lock.GetBody().GetObjectLayer();
    // A body thrown by a known actor keeps that cause when it happens to
    // strike a bystander. A counterpart's known cause can still propagate.
    if(layer==DynamicWorld::C_Ragdoll || layer==DynamicWorld::C_ActiveBody || layer==DynamicWorld::C_LivingBody) {
      if(auto* rig=rigFor(impact.body))
        if(knownCulprit || rig->noiseCulprit()==nullptr) rig->setNoiseCulprit(culprit);
      }
    else if(auto* body=reinterpret_cast<CollisionBody*>(lock.GetBody().GetUserData())) {
      const auto current=impact.step<=body->noiseCulpritUntilStep ? body->noiseCulprit.lock() : nullptr;
      if(!knownCulprit && current!=nullptr && current->user_ptr!=nullptr) continue;
      body->noiseCulprit=culprit->handlePtr();
      body->noiseCulpritUntilStep=impact.step+300;
      body->setTraceCulprit(culprit);
      }
    }
  std::sort(noises.begin(),noises.end(),[](const auto& a,const auto& b) {
    return std::make_tuple(a.step,a.logicalBody,-a.impulse,a.body.GetIndexAndSequenceNumber()) <
           std::make_tuple(b.step,b.logicalBody,-b.impulse,b.body.GetIndexAndSequenceNumber());
    });
  if(bodyImpact) {
    std::vector<const Impl::NoiseImpact*> structural;
    structural.reserve(noises.size());
    for(const auto& impact:noises)
      if(impact.structuralImpulse>0.f) structural.push_back(&impact);
    std::sort(structural.begin(),structural.end(),[](const auto* a,const auto* b) {
      return std::make_tuple(a->step,a->body.GetIndexAndSequenceNumber(),-a->structuralImpulse) <
             std::make_tuple(b->step,b->body.GetIndexAndSequenceNumber(),-b->structuralImpulse);
      });
    JPH::BodyID previous;
    uint64_t previousStep=uint64_t(-1);
    for(const auto* candidate:structural) {
      const auto& impact=*candidate;
      // Count the strongest collision once, rather than summing duplicate
      // terrain triangles. The audible impulse and its ordering stay unchanged.
      if(impact.body==previous && impact.step==previousStep) continue;
      CollisionBody* body=nullptr;
      std::string otherCulprit;
      {
        JPH::BodyLockRead other(impl->system.GetBodyLockInterface(),impact.other);
        if(!other.Succeeded()) continue;
        const auto category=other.GetBody().GetObjectLayer();
        if(category==DynamicWorld::C_LivingBody || category==DynamicWorld::C_ActiveBody) continue;
        // The impactor's recorded push/throw takes precedence over an older
        // cause on the struck prop. This is trace truth only: audible noise
        // keeps its existing short attribution window and visibility rules.
        if(category==DynamicWorld::C_Item || category==DynamicWorld::C_MovableVob)
          if(const auto* source=reinterpret_cast<const CollisionBody*>(other.GetBody().GetUserData()))
            otherCulprit=source->traceCulprit();
        }
      {
        JPH::BodyLockRead lock(impl->system.GetBodyLockInterface(),impact.body);
        if(!lock.Succeeded() || lock.GetBody().GetObjectLayer()!=DynamicWorld::C_MovableVob) continue;
        body=reinterpret_cast<CollisionBody*>(lock.GetBody().GetUserData());
        }
      previous=impact.body;
      previousStep=impact.step;
      if(body!=nullptr) bodyImpact(*body,impact.structuralImpulse,impact.point,otherCulprit);
      }
    }
  if(impl->noiseEnabled && physicsNoise) {
    for(const auto& impact:noises) {
      if(impact.impulse<.08f) continue;
      PhysicsNoise noise;
      {
        JPH::BodyLockRead lock(impl->system.GetBodyLockInterface(),impact.body);
        if(!lock.Succeeded()) continue;
        const auto layer=lock.GetBody().GetObjectLayer();
        if(layer==DynamicWorld::C_Ragdoll || layer==DynamicWorld::C_ActiveBody || layer==DynamicWorld::C_LivingBody) {
          auto* rig=rigFor(impact.body);
          if(rig==nullptr || !rig->acceptNoise(impact.step)) continue;
          noise.body=rig->noiseId();
          noise.material=MAT_LEATHER;
          noise.what="ragdoll";
          noise.culprit=rig->noiseCulprit();
          }
        else {
          auto* body=reinterpret_cast<CollisionBody*>(lock.GetBody().GetUserData());
          if(body==nullptr || impact.step<body->nextNoiseStep) continue;
          body->nextNoiseStep=impact.step+30;
          noise.body=impact.body.GetIndexAndSequenceNumber();
          noise.material=body->noiseMaterial;
          noise.what=body->noiseWhat;
          const auto culprit=impact.step<=body->noiseCulpritUntilStep ? body->noiseCulprit.lock() : nullptr;
          noise.culprit=culprit!=nullptr ? static_cast<Npc*>(culprit->user_ptr) : nullptr;
          }
        }
      noise.position=impact.point;
      noise.surface=impact.surface;
      noise.impulse=impact.impulse;
      const float gain=noise.material==MAT_METAL ? 1.15f : noise.material==MAT_CLAY ? .9f :
                       noise.material==MAT_LEATHER ? .55f : 1.f;
      noise.volume=std::clamp(std::sqrt(noise.impulse)*gain/18.f,.04f,1.f);
      noise.radius=100.f+1100.f*noise.volume;
      // Game callbacks may remove bodies and must run without native locks.
      physicsNoise(noise);
      }
    }
  std::sort(impacts.begin(),impacts.end(),[](const auto& a, const auto& b) {
    return std::make_tuple(a.step,a.item.GetIndexAndSequenceNumber(),a.landscape,a.subShape,a.material,a.impulse,a.mass) <
           std::make_tuple(b.step,b.item.GetIndexAndSequenceNumber(),b.landscape,b.subShape,b.material,b.impulse,b.mass);
    });
  if(hitItem) {
    for(const auto& impact:impacts) {
      Item* item = nullptr;
      {
        JPH::BodyLockRead lock(impl->system.GetBodyLockInterface(),impact.item);
        if(lock.Succeeded()) {
          auto* body = reinterpret_cast<CollisionBody*>(lock.GetBody().GetUserData());
          if(body!=nullptr && body->pointerType==DynamicWorld::P_Item)
            item = reinterpret_cast<Item*>(body->pointer);
          }
        }
      // Game callbacks may remove bodies and must run without Jolt locks.
      if(item!=nullptr)
        hitItem(*item,impact.material,impact.impulse,impact.mass);
      }
    }

  std::vector<JPH::BodyID> active;
  recoverItems();
  active.reserve(rigid.size());
  for(auto* body:rigid)
    active.push_back(body->id);
  for(auto id:active) {
    Item* item = nullptr;
    Tempest::Matrix4x4 matrix;
    bool disable = false;
    {
      JPH::BodyLockRead lock(impl->system.GetBodyLockInterface(),id);
      if(!lock.Succeeded())
        continue;
      const auto& native = lock.GetBody();
      auto* body = reinterpret_cast<CollisionBody*>(native.GetUserData());
      if(body==nullptr || body->pointerType!=DynamicWorld::P_Item || body->pointer==nullptr)
        continue;
      item = reinterpret_cast<Item*>(body->pointer);
      auto transform = native.GetWorldTransform();
      float m[16];
      for(JPH::uint c=0;c<4;++c) {
        auto column = transform.GetColumn4(c);
        const float factor = c<3 ? body->scale[c] : 1.f;
        m[c*4+0] = column.GetX()*factor;
        m[c*4+1] = column.GetY()*factor;
        m[c*4+2] = column.GetZ()*factor;
        m[c*4+3] = column.GetW();
        }
      m[12] *= 100.f;
      m[13] *= 100.f;
      m[14] *= 100.f;
      matrix.setData(m);
      // Legacy items lose their body at rest. A nearby virtual character needs
      // that sleeping body to remain queryable so a native contact can wake it.
      // A sleeping water item also retains its native body, independently of
      // characters, so save/load, pickup and a buoyancy toggle remain physical.
      disable = !native.IsActive() && !body->keepSleepingItem(toCentimeters(native.GetPosition()));
      }
    item->setPhysicsMatrix(matrix);
    if(disable)
      item->setPhysicsDisable();
    }
  for(auto* body:rigid)
    body->itemRecovered=false;
  impl->stepMilliseconds=std::chrono::duration<float,std::milli>(std::chrono::steady_clock::now()-started).count()+impl->characterMilliseconds-impl->meleeDiagnosticMs;
  if(!impl->meleeOutput.empty()) {
    auto& sample=impl->meleeSample;
    sample.step=impl->simulationStep;sample.milliseconds=impl->stepMilliseconds;
    sample.solveMs-=sample.castMs;
    sample.blades=impl->meleeBlades.size();
    if(impl->meleeSamples.size()==20000) throw std::runtime_error("Melee physics measurement overflow");
    impl->meleeSamples.push_back(sample);
    }
  impl->characterMilliseconds = 0;
  impl->lastRayCount=impl->rayCount.exchange(0,std::memory_order_relaxed);
  }

std::unique_ptr<Ragdoll> CollisionWorld::createRagdoll(const Skeleton& skeleton, const HitZones& zones,
                                                      const Tempest::Matrix4x4* worldPose, size_t numBones,
                                                      const Tempest::Matrix4x4& objectMatrix,
                                                      const Tempest::Vec3& initialVelocity) {
  std::unique_ptr<Ragdoll> result;
  try {
    result.reset(new Ragdoll(*this,skeleton,zones,worldPose,numBones,objectMatrix,initialVelocity));
    }
  catch(const std::runtime_error&) {
    return nullptr;
    }
  return result->valid() ? std::move(result) : nullptr;
  }

std::unique_ptr<Ragdoll> CollisionWorld::createKinematicBody(const Skeleton& skeleton, const HitZones& zones,
                                                            const Tempest::Matrix4x4* worldPose, size_t numBones,
                                                            const Tempest::Matrix4x4& objectMatrix) {
  std::unique_ptr<Ragdoll> result;
  try {
    result.reset(new Ragdoll(*this,skeleton,zones,worldPose,numBones,objectMatrix,{},true));
    }
  catch(const std::runtime_error&) {
    return nullptr;
    }
  return result->valid() ? std::move(result) : nullptr;
  }

JPH::PhysicsSystem& CollisionWorld::physicsSystem() { return impl->system; }

uint32_t CollisionWorld::registerRagdoll(Ragdoll& body) {
  // Zero is the default group of ordinary world bodies.
  if(nextRagdollGroup==0) throw std::runtime_error("Jolt ragdoll collision group capacity exhausted");
  ragdolls.push_back(&body);
  return nextRagdollGroup++;
  }

void CollisionWorld::unregisterRagdoll(Ragdoll& body) {
  ragdolls.erase(std::remove(ragdolls.begin(),ragdolls.end(),&body),ragdolls.end());
  }

size_t CollisionWorld::bodyCount() const { return impl->system.GetNumBodies(); }
float CollisionWorld::stepMilliseconds() const { return impl->stepMilliseconds; }
uint64_t CollisionWorld::rayCount() const { return impl->lastRayCount; }
uint64_t CollisionWorld::simulationStep() const { return impl->simulationStep; }

void CollisionWorld::setWaterSurfaceCallback(std::function<bool(const Tempest::Vec3&,float,float&)> callback) {
  waterSurface=std::move(callback);
  }

struct CollisionWorld::BuoyancyBody {
  JPH::BodyID id;
  const JPH::Shape* shape;
  float fraction;
  bool& initialized;
  bool& inWater;
  bool& sleepDisabled;
  bool& preserveSleep;
  float& restTime;
  float& waterY;
  uint64_t& nextSplashStep;
  CollisionBody* wrapper;
  Ragdoll* rig;
  uint32_t bone;
  };

template<class F>
void CollisionWorld::forEachBuoyancyBody(F&& callback) const {
  for(auto* body:rigid) {
    if(!waterThing(body->category())) continue;
    BuoyancyBody target {body->id,body->buoyancyShape.GetPtr(),body->buoyancyFraction,
      body->waterInitialized,body->inWater,body->waterSleepDisabled,body->waterPreserveSleep,
      body->waterRestTime,body->waterY,body->nextSplashStep,body,nullptr,0};
    callback(target);
    }
  for(auto* rig:ragdolls) {
    const auto& bones=rig->boneIndices();
    auto& parts=rig->buoyancyParts();
    for(size_t i=0;i<parts.size();++i) {
      auto& p=parts[i];
      BuoyancyBody target {JPH::BodyID(p.body),nullptr,p.fraction,p.initialized,p.inWater,
        p.sleepDisabled,p.preserveSleep,p.restTime,p.waterY,p.nextSplashStep,nullptr,rig,uint32_t(bones[i])};
      callback(target);
      }
    }
  }

void CollisionWorld::restoreRagdollBuoyancy(Ragdoll& rig) {
  for(auto& part:rig.buoyancyParts()) {
    part.initialized=false;
    part.inWater=false;
    part.sleepDisabled=false;
    part.restTime=0;
    const JPH::BodyID id(part.body);
    Tempest::Vec3 center;
    float bottom=0;
    {
      JPH::BodyLockRead lock(impl->system.GetBodyLockInterface(),id);
      if(!lock.Succeeded()) continue;
      const auto& body=lock.GetBody();
      part.preserveSleep=!body.IsActive();
      center=toCentimeters(body.GetCenterOfMassPosition());
      bottom=body.GetWorldSpaceBounds().mMin.GetY()*100.f;
      }
    float surface=0;
    if(!buoyancy && part.preserveSleep && waterSurface && waterSurface(center,bottom,surface) && surface>bottom) {
      part.preserveSleep=false;
      impl->system.GetBodyInterface().ActivateBody(id);
      }
    }
  }

void CollisionWorld::setBuoyancyEnabled(bool enabled) {
  if(buoyancy==enabled) return;
  buoyancy=enabled;
  forEachBuoyancyBody([&](BuoyancyBody& target) {
    float surface=0;
    JPH::Vec3 min;
    Tempest::Vec3 center;
    {
      JPH::BodyLockRead lock(impl->system.GetBodyLockInterface(),target.id);
      if(!lock.Succeeded()) return;
      const auto& native=lock.GetBody();
      const auto* shape=target.shape!=nullptr ? target.shape : native.GetShape();
      const auto bounds=shape->GetWorldSpaceBounds(native.GetCenterOfMassTransform(),JPH::Vec3::sOne());
      min=bounds.mMin;
      center=toCentimeters(native.GetCenterOfMassPosition());
      }
    const bool wet=waterSurface && waterSurface(center,min.GetY()*100.f,surface) &&
                   surface>min.GetY()*100.f;
    {
      JPH::BodyLockWrite lock(impl->system.GetBodyLockInterface(),target.id);
      if(lock.Succeeded()) lock.GetBody().SetAllowSleeping(true);
      }
    target.sleepDisabled=false;
    target.restTime=0;
    // Both switching off a sleeping float and switching on a sunken wooden
    // object must take effect immediately. Unrelated dry props stay asleep.
    if(wet || target.inWater) impl->system.GetBodyInterface().ActivateBody(target.id);
    });
  }

bool CollisionWorld::buoyancyEnabled() const { return buoyancy; }

void CollisionWorld::setItemWakeEnabled(bool enabled) { itemWake=enabled; }

void CollisionWorld::setItemWakeCallback(std::function<size_t(const Tempest::Vec3&,const Tempest::Vec3&,size_t)> callback) {
  itemWakeCallback=std::move(callback);
  }

void CollisionWorld::prepareBuoyancy(std::vector<PhysicsNoise>& splashes) {
  if(!buoyancy || !waterSurface) return;
  FrameProfile::Scope timing(FrameProfile::Zone::Water);
  forEachBuoyancyBody([&](BuoyancyBody& target) {
    if(target.initialized && !impl->system.GetBodyInterface().IsActive(target.id)) return;
    // Never hold a native body lock while querying the world: rayCast obtains
    // its own locks, including for overlapping sensors.
    Tempest::Vec3 center;
    float bottom=0;
    {
      JPH::BodyLockRead lock(impl->system.GetBodyLockInterface(),target.id);
      if(!lock.Succeeded()) return;
      const auto& body=lock.GetBody();
      const auto* shape=target.shape!=nullptr ? target.shape : body.GetShape();
      const auto bounds=shape->GetWorldSpaceBounds(body.GetCenterOfMassTransform(),JPH::Vec3::sOne());
      bottom=bounds.mMin.GetY()*100.f;
      center=toCentimeters(body.GetCenterOfMassPosition());
      }
    float surface=0;
    const bool hasWater=waterSurface(center,bottom,surface);
    bool activate=false;
    {
      JPH::BodyLockWrite lock(impl->system.GetBodyLockInterface(),target.id);
      if(!lock.Succeeded()) return;
      auto& body=lock.GetBody();
      if(!body.IsDynamic()) return;
      const auto* shape=target.shape!=nullptr ? target.shape : body.GetShape();
      float total=0,submerged=0;
      JPH::Vec3 buoyancyCenter=JPH::Vec3::sZero();
      if(hasWater) submergedVolume(body,*shape,surface*.01f,total,submerged,buoyancyCenter);
      const bool wet=hasWater && submerged>0.f && total>0.f;
      const bool entering=target.initialized && !target.inWater && wet;
      activate=wet && !target.initialized && !target.preserveSleep && !body.IsActive();
      target.preserveSleep=false;
      target.initialized=true;
      target.waterY=surface;
      target.inWater=wet;
      if(!wet) {
        target.restTime=0;
        if(target.sleepDisabled) body.SetAllowSleeping(true);
        target.sleepDisabled=false;
        return;
        }
      if(entering && body.IsActive() && impl->simulationStep>=target.nextSplashStep) {
        const float speed=body.GetLinearVelocity().Length();
        const float momentum=speed*(target.rig!=nullptr ? target.rig->mass() : 1.f/body.GetMotionProperties()->GetInverseMass());
        if(speed>.2f && momentum>.02f) {
          PhysicsNoise noise;
          noise.position={center.x,surface,center.z};
          noise.surface=asset::MaterialGroup::WATER;
          noise.impulse=momentum;
          noise.volume=std::clamp(std::sqrt(momentum)/12.f,.015f,1.f);
          noise.radius=100.f+1500.f*noise.volume;
          if(target.wrapper!=nullptr) {
            noise.material=target.wrapper->noiseMaterial;
            noise.what=target.wrapper->noiseWhat+" splash";
            noise.body=target.wrapper->nativeId();
            const auto culprit=impl->simulationStep<=target.wrapper->noiseCulpritUntilStep ? target.wrapper->noiseCulprit.lock() : nullptr;
            noise.culprit=culprit!=nullptr ? static_cast<Npc*>(culprit->user_ptr) : nullptr;
            }
          else {
            // One splash per connected body, using its total native mass.
            // Keep water-entry cooldown separate from solid contact noises.
            for(auto& part:target.rig->buoyancyParts()) part.nextSplashStep=impl->simulationStep+45;
            noise.material=MAT_LEATHER;
            noise.what="body splash";
            noise.body=target.rig->noiseId();
            noise.culprit=target.rig->noiseCulprit();
            }
          splashes.push_back(std::move(noise));
          target.nextSplashStep=impl->simulationStep+45;
          }
        }
      // BodyInterface::ApplyBuoyancyImpulse activates on every call. Calling
      // Body directly lets settled and restored sleeping things stay asleep.
      if(!body.IsActive() && !activate) return;
      if(target.restTime<2.f && !target.sleepDisabled) {
        body.SetAllowSleeping(false);
        target.sleepDisabled=true;
        }
      }
    if(activate) impl->system.GetBodyInterface().ActivateBody(target.id);
    });
  }

void CollisionWorld::applyBuoyancyStep(float seconds) {
  FrameProfile::Scope timing(FrameProfile::Zone::Water);
  // Jolt invokes step listeners while all body mutexes are held. Water rays
  // ran before Update; this callback only touches our cached wet rigid bodies
  // through the NoLock interface and never invokes game callbacks.
  forEachBuoyancyBody([&](BuoyancyBody& target) {
    if(!target.inWater) return;
    JPH::BodyLockWrite lock(impl->system.GetBodyLockInterfaceNoLock(),target.id);
    if(!lock.Succeeded()) return;
    auto& body=lock.GetBody();
    if(!body.IsActive()) return;
    const auto* shape=target.shape!=nullptr ? target.shape : body.GetShape();
    float total=0,submerged=0;
    JPH::Vec3 buoyancyCenter=JPH::Vec3::sZero();
    submergedVolume(body,*shape,target.waterY*.01f,total,submerged,buoyancyCenter);
    if(total<=0.f || submerged<=0.f) return;
    const float inverseMass=body.GetMotionProperties()->GetInverseMass();
    const float factor=target.fraction>0.f ? 1.f/target.fraction : 1000.f*total*inverseMass;
    // Dampen old velocity, then add support with the same equilibrium fraction.
    const float supportedMass=std::clamp((submerged/total)*factor,0.f,1.f);
    const float damping=std::exp(-2.f*supportedMass*seconds);
    body.SetLinearVelocity(body.GetLinearVelocity()*damping);
    body.SetAngularVelocity(body.GetAngularVelocity()*damping);
    body.ApplyBuoyancyImpulse(total,submerged,buoyancyCenter,factor,.8f,.15f,
                              JPH::Vec3::sZero(),impl->system.GetGravity(),seconds);
    });
  }

void CollisionWorld::finishBuoyancy(float seconds) {
  if(!buoyancy) return;
  FrameProfile::Scope timing(FrameProfile::Zone::Water);
  forEachBuoyancyBody([&](BuoyancyBody& target) {
    if(!target.inWater) return;
    {
      JPH::BodyLockWrite lock(impl->system.GetBodyLockInterface(),target.id);
      if(!lock.Succeeded()) return;
      auto& body=lock.GetBody();
      if(!body.IsActive()) return;
      const auto* shape=target.shape!=nullptr ? target.shape : body.GetShape();
      const float radius=shape->GetLocalBounds().GetExtent().Length();
      const float pointSpeed=body.GetLinearVelocity().Length()+radius*body.GetAngularVelocity().Length();
      target.restTime=pointSpeed<.01f ? target.restTime+seconds : 0.f;
      // Let Jolt sleep the whole contact island together. Deactivating one
      // float by itself can freeze a support while its neighbour still moves,
      // immediately waking it and dropping its water support mid-update.
      if(target.restTime>=2.f && target.sleepDisabled) {
        body.SetAllowSleeping(true);
        target.sleepDisabled=false;
        }
      else if(target.restTime==0.f && !target.sleepDisabled) {
        body.SetAllowSleeping(false);
        target.sleepDisabled=true;
        }
      }
    });
  }

std::vector<CollisionWorld::WaterBodySnapshot> CollisionWorld::waterBodySnapshot() const {
  std::vector<WaterBodySnapshot> result;
  forEachBuoyancyBody([&](BuoyancyBody& target) {
    WaterBodySnapshot entry;
    entry.body=target.wrapper;
    entry.ragdoll=target.rig;
    entry.bone=target.bone;
    entry.nativeId=target.id.GetIndexAndSequenceNumber();
    entry.targetFraction=target.fraction;
    Tempest::Vec3 center;
    {
      JPH::BodyLockRead lock(impl->system.GetBodyLockInterface(),target.id);
      if(!lock.Succeeded()) return;
      const auto& body=lock.GetBody();
      const auto transform=body.GetWorldTransform();
      float matrix[16];
      for(JPH::uint c=0;c<4;++c) {
        const auto column=transform.GetColumn4(c);
        const float scale=target.wrapper!=nullptr && c<3 ? target.wrapper->scale[c] : 1.f;
        for(JPH::uint r=0;r<4;++r) matrix[c*4+r]=column[r]*scale;
        }
      matrix[12]*=100.f; matrix[13]*=100.f; matrix[14]*=100.f;
      entry.transform.setData(matrix);
      const auto* shape=target.shape!=nullptr ? target.shape : body.GetShape();
      const auto bounds=shape->GetWorldSpaceBounds(body.GetCenterOfMassTransform(),JPH::Vec3::sOne());
      entry.worldBounds[0]=toCentimeters(bounds.mMin);
      entry.worldBounds[1]=toCentimeters(bounds.mMax);
      // The broad-phase box can include empty corners of a rotated hull.
      const auto collisionBounds=body.GetObjectLayer()==DynamicWorld::C_Item ? itemCollisionBounds(body) : body.GetWorldSpaceBounds();
      entry.collisionBounds[0]=toCentimeters(collisionBounds.mMin);
      entry.collisionBounds[1]=toCentimeters(collisionBounds.mMax);
      entry.mass=1.f/body.GetMotionProperties()->GetInverseMass();
      entry.density=entry.mass/shape->GetVolume();
      entry.sleeping=!body.IsActive();
      entry.velocity=toCentimeters(body.GetLinearVelocity());
      center=toCentimeters(body.GetCenterOfMassPosition());
      }
    if(waterSurface && waterSurface(center,entry.worldBounds[0].y,entry.waterY)) {
      JPH::BodyLockRead lock(impl->system.GetBodyLockInterface(),target.id);
      if(lock.Succeeded()) {
        const auto& body=lock.GetBody();
        const auto* shape=target.shape!=nullptr ? target.shape : body.GetShape();
        float total=0,submerged=0;
        JPH::Vec3 centerOfBuoyancy;
        submergedVolume(body,*shape,entry.waterY*.01f,total,submerged,centerOfBuoyancy);
        entry.submergedFraction=total>0.f ? submerged/total : 0.f;
        entry.inWater=submerged>0.f;
        }
      }
    else entry.waterY=0.f;
    result.push_back(std::move(entry));
    });
  return result;
  }

void CollisionWorld::setBBox(JPH::Vec3Arg min, JPH::Vec3Arg) {
  bboxMin = min;
  }

void CollisionWorld::setItemHitCallback(std::function<void(Item&, asset::MaterialGroup, float, float)> callback) {
  hitItem = std::move(callback);
  impl->emitImpacts = bool(hitItem) && !impl->noiseEnabled;
  }

void CollisionWorld::setPhysicsNoiseEnabled(bool enabled) {
  impl->noiseEnabled=enabled;
  impl->emitNoise=enabled && bool(physicsNoise);
  impl->emitImpacts=!enabled && bool(hitItem);
  }

bool CollisionWorld::physicsNoiseEnabled() const {
  return impl->noiseEnabled;
  }

void CollisionWorld::setPhysicsNoiseCallback(std::function<void(const PhysicsNoise&)> callback) {
  physicsNoise=std::move(callback);
  impl->emitNoise=impl->noiseEnabled && bool(physicsNoise);
  }

void CollisionWorld::setBodyImpactCallback(std::function<void(CollisionBody&,float,const Tempest::Vec3&,std::string_view)> callback) {
  bodyImpact=std::move(callback);
  }

void CollisionWorld::emitPhysicsNoise(const PhysicsNoise& noise) {
  if(impl->noiseEnabled && physicsNoise) physicsNoise(noise);
  }
