#include "collisionworld.h"
#include "collisionworld_internal.h"
#include "physicvbo.h"
#include "dynamicworld.h"
#include "ragdoll.h"
#include "cloth.h"
#include "frameprofile.h"
#include "loadprobe.h"
#include "gothic.h"
#include "testbench.h"
#include "world/objects/item.h"
#include "world/objects/npc.h"
#include "world/world.h"
#include "sha256.h"
#include <Tempest/Log>

#include <cstdlib>

#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Core/UnorderedSet.h>
#include <Jolt/Geometry/GJKClosestPoint.h>
#include <Jolt/Core/StreamOut.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/PhysicsStepListener.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Body/BodyFilter.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
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
#include <mutex>
#include <limits>
#include <iterator>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <unordered_map>
#ifdef __APPLE__
#include <sys/types.h>
#include <sys/sysctl.h>
#endif

namespace {
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

void CollisionWorld::CollisionBody::setFracturePiece(const JPH::GroupFilter* filter, uint32_t group, uint32_t piece) {
  JPH::BodyLockWrite lock(owner->impl->system.GetBodyLockInterface(),id);
  if(!lock.Succeeded()) return;
  auto& native=lock.GetBody();
  native.SetCollisionGroup(JPH::CollisionGroup(filter,group,piece));
  // Reduction merges the manifolds of a compound stave's hulls into four points,
  // and its other hulls sank 2 cm into the ground unresisted. The new body has no contacts yet.
  native.SetUseManifoldReduction(false);
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

Tempest::Vec3 CollisionWorld::CollisionBody::lowestPoint() const {
  JPH::BodyLockRead lock(owner->impl->system.GetBodyLockInterface(),id);
  if(!lock.Succeeded())
    throw std::runtime_error("Item lowest point requires a native body");
  auto leaf=lock.GetBody().GetTransformedShape();
  JPH::SubShapeID remainder;
  while(leaf.mShape->GetType()==JPH::EShapeType::Decorated)
    leaf=leaf.GetSubShapeTransformedShape(JPH::SubShapeID(),remainder);
  if(leaf.mShape->GetType()!=JPH::EShapeType::Convex)
    throw std::runtime_error("Item lowest point requires a convex shape");
  const auto& shape=static_cast<const JPH::ConvexShape&>(*leaf.mShape);
  const auto direction=leaf.mShapeRotation.Conjugated()*-JPH::Vec3::sAxisY();
  JPH::ConvexShape::SupportBuffer buffer;
  const auto* support=shape.GetSupportFunction(JPH::ConvexShape::ESupportMode::Default,buffer,leaf.GetShapeScale());
  auto point=leaf.mShapeRotation*support->GetSupport(direction)-JPH::Vec3::sAxisY()*support->GetConvexRadius();
  JPH::Shape::SupportingFace face;
  shape.GetSupportingFace(JPH::SubShapeID(),-direction,leaf.GetShapeScale(),
                          JPH::Mat44::sRotation(leaf.mShapeRotation),face);
  JPH::Vec3 sum=JPH::Vec3::sZero();
  size_t count=0;
  for(const auto& vertex:face)
    if(std::abs(vertex.GetY()-point.GetY())<=1.e-6f) {
      sum+=vertex;
      ++count;
      }
  // A flat support face (or edge) has no distinguished vertex. Its centroid
  // gives a deterministic point on the actual envelope, including rotation.
  if(count>1) {
    const auto center=sum/float(count);
    point.SetX(center.GetX());
    point.SetZ(center.GetZ());
    }
  return toCentimeters(leaf.mShapePositionCOM+point);
  }

float CollisionWorld::CollisionBody::mass() const {
  if(owner==nullptr || id.IsInvalid()) return 0.f;
  JPH::BodyLockRead lock(owner->impl->system.GetBodyLockInterface(),id);
  if(!lock.Succeeded() || !lock.GetBody().IsDynamic()) return 0.f;
  return 1.f/lock.GetBody().GetMotionProperties()->GetInverseMass();
  }

float CollisionWorld::CollisionBody::friction() const {
  if(owner==nullptr || id.IsInvalid()) return 0.f;
  JPH::BodyLockRead lock(owner->impl->system.GetBodyLockInterface(),id);
  return lock.Succeeded() ? lock.GetBody().GetFriction() : 0.f;
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
  noiseCulprit=culprit!=nullptr ? culprit->handlePtr() : std::weak_ptr<daedalus::INpc>();
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
  LoadProbe::Vobs::Scope probe("physics_body");
  LoadProbe::Startup::Scope itemProbe(LoadProbe::Startup::PhysicsBody,false,"placed-items");
  auto result = std::unique_ptr<CollisionBody>(new CollisionBody(*this,std::move(shape),category));
  result->mesh = mesh;
  addBody(*result,matrix,friction,movable,sensor,0.f);
  return result;
  }

std::unique_ptr<CollisionWorld::DynamicBody> CollisionWorld::addDynamicBody(JPH::ShapeRefC shape, const Tempest::Matrix4x4& matrix,
                                                                           float friction, float mass, int category, bool startSleeping) {
  LoadProbe::Vobs::Scope probe("physics_body");
  LoadProbe::Startup::Scope itemProbe(LoadProbe::Startup::PhysicsBody,false,"placed-items");
  mass = std::isfinite(mass) && mass>0.f ? mass : 1.f;
  auto result = std::unique_ptr<DynamicBody>(new DynamicBody(*this,std::move(shape),mass,category));
  addBody(*result,matrix,friction,false,false,mass,startSleeping);
  rigid.push_back(result.get());
  return result;
  }

uint32_t CollisionWorld::newFractureGroup() {
  if(nextFractureGroup==JPH::CollisionGroup::cInvalidGroup) nextFractureGroup=0x80000000u;
  return nextFractureGroup++;
  }

void CollisionWorld::cycleBodyIds(size_t count) {
  if(count==0)
    return;
  auto& interface = impl->system.GetBodyInterface();
  const JPH::ShapeRefC shape = new JPH::SphereShape(.01f);
  const JPH::BodyCreationSettings settings(shape.GetPtr(),JPH::RVec3::sZero(),JPH::Quat::sIdentity(),
                                           JPH::EMotionType::Static,JPH::ObjectLayer(DynamicWorld::C_Item));
  std::vector<JPH::BodyID> ids;
  ids.reserve(count);
  for(size_t i=0;i<count;++i) {
    auto* native = interface.CreateBody(settings);
    if(native==nullptr)
      throw std::runtime_error("Jolt body capacity exhausted");
    ids.push_back(native->GetID());
    }
  for(const auto& id:ids)
    interface.DestroyBody(id);
  }

namespace {
// Streams Jolt shape state into a digest, like placedItemCensusInputs does.
class ShapeDigest final : public JPH::StreamOut {
  public:
    explicit ShapeDigest(Sha256& digest):digest(digest) {}
    void WriteBytes(const void* data, size_t size) override { digest.update(data,size); }
    bool IsFailed() const override { return false; }
  private:
    Sha256& digest;
  };
}

CollisionWorld::BodyDigest CollisionWorld::bodyDigest() const {
  Sha256 digest;
  ShapeDigest stream(digest);
  JPH::Shape::ShapeToIDMap shapes;
  JPH::Shape::MaterialToIDMap materials;
  const auto u32=[&](uint32_t value) { digest.update(&value,sizeof(value)); };
  const auto vec=[&](JPH::Vec3Arg value) {
    const float xyz[3] = {value.GetX(),value.GetY(),value.GetZ()};
    digest.update(xyz,sizeof(xyz));
    };
  JPH::BodyIDVector ids;
  impl->system.GetBodies(ids);
  BodyDigest result;
  const auto& locks = impl->system.GetBodyLockInterfaceNoLock();
  for(const auto& id:ids) {
    JPH::BodyLockRead lock(locks,id);
    if(!lock.Succeeded())
      continue;
    const auto& body = lock.GetBody();
    ++result.bodies;
    result.active += body.IsActive() ? 1 : 0;
    u32(id.GetIndexAndSequenceNumber());
    u32(uint32_t(body.GetMotionType()));
    u32(uint32_t(body.GetObjectLayer()));
    u32(uint32_t(body.IsSensor())|(uint32_t(body.IsActive())<<1));
    // Geometry, not just type and bounds: shared shapes are written once,
    // later references write only their ID.
    body.GetShape()->SaveWithChildren(stream,shapes,materials);
    if(body.GetMotionType()==JPH::EMotionType::Kinematic)
      continue;
    vec(JPH::Vec3(body.GetPosition()));
    const auto rotation = body.GetRotation();
    const float quat[4] = {rotation.GetX(),rotation.GetY(),rotation.GetZ(),rotation.GetW()};
    digest.update(quat,sizeof(quat));
    vec(body.GetWorldSpaceBounds().mMin);
    vec(body.GetWorldSpaceBounds().mMax);
    if(body.GetMotionType()==JPH::EMotionType::Dynamic) {
      vec(body.GetLinearVelocity());
      vec(body.GetAngularVelocity());
      }
    }
  result.digest = digest.finish();
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

std::array<size_t,4> CollisionWorld::staticTriangles(const std::function<void(const StaticTriangle&)>& output,
                                                    bool includeKinematic) const {
  struct TriangleCollector final : JPH::TransformedShapeCollector {
    TriangleCollector(const std::function<void(const StaticTriangle&)>& output, uint8_t category)
      :output(output),category(category) {
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
          result.water = category==3;
          result.category = category;
          for(int i=0;i<3;++i) {
            const auto& v = vertices[triangle*3+i];
            result.vertices[i] = Tempest::Vec3(v.x,v.y,v.z);
            }
          output(result);
          }
        }
      }

    const std::function<void(const StaticTriangle&)>& output;
    uint8_t category;
    };

  std::array<size_t,4> bodyCounts = {};
  for(const auto* object:bodies) {
    if(object->categoryId!=DynamicWorld::C_Landscape &&
       object->categoryId!=DynamicWorld::C_Object &&
       object->categoryId!=DynamicWorld::C_Water)
      continue;
    JPH::TransformedShape shape;
    uint8_t category = object->categoryId==DynamicWorld::C_Landscape ? 1 :
                       object->categoryId==DynamicWorld::C_Water ? 3 : 2;
    {
      JPH::BodyLockRead lock(impl->system.GetBodyLockInterface(),object->id);
      if(!lock.Succeeded())
        continue;
      if(!lock.GetBody().IsStatic()) {
        if(!includeKinematic || object->categoryId!=DynamicWorld::C_Object || !lock.GetBody().IsKinematic())
          continue; // no living bodies or dynamic props; kinematic objects only on request
        category = 4;
        }
      shape = lock.GetBody().GetTransformedShape();
    }
    ++bodyCounts[category-1];
    TriangleCollector collector(output,category);
    shape.CollectTransformedShapes(shape.GetWorldSpaceBounds(),collector);
    }
  return bodyCounts;
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
    else if(value.body!=nullptr && (value.body->category()==DynamicWorld::C_Item || value.body->category()==DynamicWorld::C_Fragment)) {
      value.material=value.body->noiseSurface;
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

std::vector<CollisionWorld::Overlap> CollisionWorld::overlaps(const CollisionBody& body, float marginCm, uint32_t mask,
                                                            bool triangles) const {
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
    if(triangles) {
      const auto normal=native.GetWorldSpaceSurfaceNormal(hit.mSubShapeID2,base+hit.mContactPointOn2);
      o.triangle=traceTriangle(native,hit.mSubShapeID2,normal);
      }
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
                                  Tempest::Vec3& normal, Interactive*& vob, const CapsuleContact& contact) const {
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
    if(contact && native.GetObjectLayer()!=DynamicWorld::C_Ragdoll) {
      const auto point=position+hit.mContactPointOn2;
      contact(body,{contactNormal.GetX(),contactNormal.GetY(),contactNormal.GetZ()},toCentimeters(point),
              toCentimeters(native.GetPointVelocity(point)));
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

void CollisionWorld::wakeItems() {
  if(!itemWake || !itemWakeCallback)
    return;
  JPH::BodyIDVector active;
  impl->system.GetActiveBodies(JPH::EBodyType::RigidBody,active);
  if(active.empty())
    return;
  size_t visited=0;
  itemWakeCursor%=active.size();
  while(visited<active.size() && itemWakeQueries<32 && itemWakeCreated<8) {
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
    wakeItemsInBounds(min,max);
    }
  }

void CollisionWorld::prepareItemSweeps() {
  itemSweeps.clear();
  itemSweepTraceBody=nullptr;
  itemSweepTraceIndex=size_t(-1);
  if(!itemWake && itemTraceTarget==nullptr)
    return;
  // A contact or blade can activate a sleeper during this same step.
  for(auto* body:rigid) {
    if(body->category()!=DynamicWorld::C_Item)
      continue;
    const bool traced=itemTraceTarget!=nullptr && body->ptrType()==DynamicWorld::P_Item && body->payload()==itemTraceTarget;
    if(!itemWake && !traced)
      continue;
    JPH::BodyLockRead lock(impl->system.GetBodyLockInterface(),body->id);
    if(lock.Succeeded() && lock.GetBody().IsDynamic()) {
      const auto from=toCentimeters(lock.GetBody().GetCenterOfMassPosition());
      if(itemWake)
        itemSweeps.emplace_back(body,from);
      if(traced) {
        itemSweepTraceBody=body;
        itemSweepTraceIndex=itemSweepTrace.size();
        ItemSweepTrace trace;
        trace.nativeStep=impl->simulationStep;
        trace.itemBodyId=body->nativeId();
        trace.from=trace.to=from;
        trace.status=itemWake ? "budget" : "item_wake_off";
        itemSweepTrace.push_back(std::move(trace));
        }
      }
    }
  }

void CollisionWorld::recoverTunneledItems() {
  if(itemSweepTraceBody!=nullptr)
    itemSweepTrace[itemSweepTraceIndex].to=itemSweepTraceBody->centerOfMass();
  if(itemSweeps.empty())return;
  constexpr uint32_t mask=(1u<<DynamicWorld::C_Landscape)|(1u<<DynamicWorld::C_Object);
  struct StaticBodies final : JPH::BodyFilter {
    bool ShouldCollideLocked(const JPH::Body& body) const override { return body.IsStatic(); }
    } staticBodies;
  JPH::RayCastSettings settings;
  settings.mTriangleEdgeTolerance=1.0e-4f;
  settings.SetBackFaceMode(JPH::EBackFaceMode::CollideWithBackFaces);
  size_t rays=0;
  itemSweepCursor%=itemSweeps.size();
  for(size_t visited=0;visited<itemSweeps.size() && rays<32;++visited) {
    const auto [body,from]=itemSweeps[itemSweepCursor];
    itemSweepCursor=(itemSweepCursor+1)%itemSweeps.size();
    const auto to=body->centerOfMass();
    auto* trace=itemSweepTraceBody==body ? &itemSweepTrace[itemSweepTraceIndex] : nullptr;
    if(!std::isfinite(to.x) || !std::isfinite(to.y) || !std::isfinite(to.z)) {
      if(trace!=nullptr) {trace->status="nonfinite";trace->triggered=true;}
      Tempest::Log::i("[physics] item recovery trigger: body=",body->nativeId()," path=nonfinite");
      recoverItem(*body,body->position());
      continue;
      }
    const auto origin=toMeters(from),end=toMeters(to);
    if((end-origin).IsNearZero()) {
      if(trace!=nullptr)trace->status="zero_motion";
      continue;
      }
    const JPH::RRayCast ray(origin,end-origin);
    JPH::ClosestHitCollisionCollector<JPH::CastRayCollector> hits;
    ++rays;
    impl->rayCount.fetch_add(1,std::memory_order_relaxed);
    impl->system.GetNarrowPhaseQuery().CastRay(ray,settings,hits,QueryBroadPhaseFilter(mask),QueryLayerFilter(mask),staticBodies);
    if(trace!=nullptr) {trace->tested=true;trace->hit=hits.HadHit();trace->status=trace->hit ? "ray" : "nohit";}
    if(!hits.HadHit())continue;
    JPH::Vec3 point,normal;
    {
      JPH::BodyLockRead lock(impl->system.GetBodyLockInterface(),hits.mHit.mBodyID);
      if(!lock.Succeeded()) {
        if(trace!=nullptr)trace->status="body_removed";
        continue;
        }
      point=ray.GetPointOnRay(hits.mHit.mFraction);
      normal=lock.GetBody().GetWorldSpaceSurfaceNormal(hits.mHit.mSubShapeID2,point);
      if(trace!=nullptr) {
        trace->bodyId=hits.mHit.mBodyID.GetIndexAndSequenceNumber();
        trace->category=int(lock.GetBody().GetObjectLayer());
        trace->point=toCentimeters(point);
        trace->normal={normal.GetX(),normal.GetY(),normal.GetZ()};
        trace->fraction=hits.mHit.mFraction;
        trace->triangle=traceTriangle(lock.GetBody(),hits.mHit.mSubShapeID2,normal);
        }
      }
    const float before=(origin-point).Dot(normal),after=(end-point).Dot(normal);
    if(trace!=nullptr) {
      trace->beforeCm=before*100.f;
      trace->afterCm=after*100.f;
      trace->triggered=before>0.f && after<0.f;
      }
    if(before>0.f && after<0.f) {
      const auto face=toCentimeters(point);
      Tempest::Log::i("[physics] item recovery crossing: body=",body->nativeId(),
                     " path=ray from_center_cm=",from.x,",",from.y,",",from.z,
                     " to_center_cm=",to.x,",",to.y,",",to.z);
      Tempest::Log::i("[physics] item recovery face: body=",body->nativeId(),
                     " point_cm=",face.x,",",face.y,",",face.z,
                     " normal=",normal.GetX(),",",normal.GetY(),",",normal.GetZ(),
                     " signed_before_cm=",before*100.f," signed_after_cm=",after*100.f);
      recoverItem(*body,body->position());
      }
    }
  }

void CollisionWorld::setItemTraceTarget(const Item* item) {
  impl->solverTraceTarget.store(item,std::memory_order_relaxed);
  {
    std::lock_guard<std::mutex> guard(impl->impactMutex);
    impl->solverContacts.clear();
  }
  itemTraceTarget=item;
  itemSweepTraceBody=nullptr;
  itemSweepTraceIndex=size_t(-1);
  itemSweepTrace.clear();
  }

std::vector<CollisionWorld::ItemSweepTrace> CollisionWorld::takeItemSweepTrace() {
  std::vector<ItemSweepTrace> result;
  result.swap(itemSweepTrace);
  itemSweepTraceBody=nullptr;
  itemSweepTraceIndex=size_t(-1);
  return result;
  }

std::vector<CollisionWorld::ItemSolverContact> CollisionWorld::takeItemSolverContacts() {
  std::vector<ItemSolverContact> result;
  std::lock_guard<std::mutex> guard(impl->impactMutex);
  result.swap(impl->solverContacts);
  return result;
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
    const bool finite=std::isfinite(failedPosition.x) && std::isfinite(failedPosition.y) && std::isfinite(failedPosition.z);
    Tempest::Log::i("[physics] item recovery trigger: body=",body->nativeId(),
                   " path=",finite?"underworld":"nonfinite"," position_cm=",failedPosition.x,",",
                   failedPosition.y,",",failedPosition.z," lower_limit_cm=",(bboxMin.GetY()-100.f)*100.f);
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
  const auto* item=body.ptrType()==DynamicWorld::P_Item ? static_cast<const Item*>(body.payload()) : nullptr;
  const auto identity=item!=nullptr && item->isPlacedItem() ? " zen_id="+std::to_string(item->placedItemZenId()) : std::string();
  Tempest::Log::i("[physics] item recovered: body=",body.nativeId()," from=",failedPosition.x,",",
                 failedPosition.y,",",failedPosition.z," rest=",body.itemRestTransform.at(3,0),",",
                 body.itemRestTransform.at(3,1),",",body.itemRestTransform.at(3,2),identity);
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
  wakeShadowItems();
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
      prepareItemSweeps();
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
          impl->finishRagdollBirthStep();
          if(TestBench::isActive())
            for(auto* body:ragdolls) body->sampleBirth(seconds);
          impl->meleeSample.solveMs+=std::chrono::duration<float,std::milli>(std::chrono::steady_clock::now()-solveStart).count();
          ++impl->meleeSample.updates;
          sampleMeleeBench();
          }
        if(!impl->meleeOutput.empty())
          impl->meleeSteps.push_back({impl->simulationStep,impl->meleeSample.solveMs-solveBefore,impl->meleeSample.castMs-castBefore,impl->meleeSubsteps});
        itemWakeQueries=0;itemWakeCreated=0;
        recoverTunneledItems();
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
      impl->finishRagdollBirthStep();
      if(TestBench::isActive())
        for(auto* body:ragdolls) body->sampleBirth(step);
      itemWakeQueries=0;itemWakeCreated=0;
      recoverTunneledItems();
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
    // A bodyless world still receives projectile and blade queries each frame.
    // Keep sub-step frames with native bodies on their existing shared budget.
    if(rigid.empty() && ragdolls.empty() && impl->meleeSubsteps==0) {
      itemWakeQueries=0;itemWakeCreated=0;
      }
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
  for(const auto& impact:impl->bodyImpacts) {
    auto* rig=rigFor(impact.body);
    if(rig!=nullptr && rig->impactBone(rig->boneForBodyId(impact.body.GetIndexAndSequenceNumber())))
      rig->recordImpactSpeed(impact.speed);
    }
  impl->bodyImpacts.clear();
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
      disable = !native.IsActive() && !body->keepSleepingItem(toCentimeters(native.GetPosition())) &&
                !(Gothic::physicsFlags().placedItems && item->isPlacedItem());
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
  const auto group=nextRagdollGroup++;
  if(impl->birthGuardEnabled && !body.isKinematic())
    impl->birthGuards.emplace(group,Impl::BirthGuard{&body,true,{}});
  return group;
  }

void CollisionWorld::unregisterRagdoll(Ragdoll& body) {
  for(auto it=impl->birthGuards.begin();it!=impl->birthGuards.end();++it)
    if(it->second.body==&body) { impl->birthGuards.erase(it);break; }
  ragdolls.erase(std::remove(ragdolls.begin(),ragdolls.end(),&body),ragdolls.end());
  }

std::vector<uint32_t> CollisionWorld::ragdollBirthGuardGroups(const Ragdoll& body) const {
  const auto it=impl->birthGuards.find(uint32_t(body.noiseId()));
  return it==impl->birthGuards.end() ? std::vector<uint32_t>() : it->second.livingGroups;
  }

size_t CollisionWorld::bodyCount() const { return impl->system.GetNumBodies(); }
JPH::Shape::Stats CollisionWorld::staticCollisionStats() const {
  JPH::Shape::Stats result(0,0);
  JPH::Shape::VisitedShapes visited;
  for(const auto* body:bodies) {
    if(body->categoryId!=DynamicWorld::C_Landscape && body->categoryId!=DynamicWorld::C_Object)
      continue;
    JPH::BodyLockRead lock(impl->system.GetBodyLockInterface(),body->id);
    if(!lock.Succeeded() || !lock.GetBody().IsStatic())
      continue;
    const auto stats=lock.GetBody().GetShape()->GetStatsRecursive(visited);
    result.mSizeBytes+=stats.mSizeBytes;
    result.mNumTriangles+=stats.mNumTriangles;
    }
  return result;
  }
float CollisionWorld::stepMilliseconds() const { return impl->stepMilliseconds; }
uint64_t CollisionWorld::rayCount() const { return impl->lastRayCount; }
uint64_t CollisionWorld::simulationStep() const { return impl->simulationStep; }

void CollisionWorld::setItemWakeEnabled(bool enabled) { itemWake=enabled; }

void CollisionWorld::setBodyImpactEnabled(bool enabled) {
  impl->emitBodyImpacts=enabled;
  if(!enabled) {
    impl->bodyImpacts.clear();
    for(auto* rig:ragdolls) rig->takeImpactSpeed();
    }
  }

void CollisionWorld::setItemDeepestContactEnabled(bool enabled) {
  auto settings=impl->system.GetPhysicsSettings();
  settings.mDeepestContactLayers=enabled ? 1u<<DynamicWorld::C_Item : 0;
  impl->system.SetPhysicsSettings(settings);
  }

void CollisionWorld::setRagdollBirthGuardEnabled(bool enabled) {
  impl->birthGuardEnabled=enabled;
  if(!enabled) {
    impl->birthGuards.clear();
    impl->birthTouches.clear();
    }
  }

void CollisionWorld::setItemWakeCallback(std::function<size_t(const Tempest::Vec3&,const Tempest::Vec3&,size_t)> callback) {
  itemWakeCallback=std::move(callback);
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

namespace CollisionWorldInternal {
  CollisionWorld::NativeTriangle traceTriangle(const JPH::Body& body, JPH::SubShapeID subShape,
                                                JPH::Vec3Arg normal) {
    CollisionWorld::NativeTriangle result;
    JPH::SubShapeID remainder;
    const auto* leaf=body.GetShape()->GetLeafShape(subShape,remainder);
    if(leaf==nullptr || (leaf->GetSubType()!=JPH::EShapeSubType::Mesh && leaf->GetSubType()!=JPH::EShapeSubType::Triangle))
      return result;
    const auto base=body.GetCenterOfMassPosition();
    JPH::Shape::SupportingFace face;
    body.GetTransformedShape().GetSupportingFace(subShape,normal,base,face);
    if(face.size()!=3)
      return result;
    result.valid=true;
    result.nativeSubShapeId=subShape.GetValue();
    if(leaf->GetSubType()==JPH::EShapeSubType::Mesh)
      result.sourceTriangle=Physics::triangleData(*body.GetShape(),subShape);
    result.normal={normal.GetX(),normal.GetY(),normal.GetZ()};
    for(JPH::uint i=0;i<3;++i) {
      result.vertices[i]=CollisionWorld::toCentimeters(base+face[i]);
      result.edgesCm[i]=(face[(i+1)%3]-face[i]).Length()*100.f;
      }
    return result;
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
