#include "collisionworld_internal.h"
#include "frameprofile.h"
#include "world/objects/npc.h"

#include <Jolt/Geometry/Plane.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/Shape/ConvexShape.h>

#include <cmath>
#include <stdexcept>

namespace CollisionWorldInternal {
  bool waterThing(int category) {
    return category==DynamicWorld::C_Item || category==DynamicWorld::C_MovableVob || DynamicWorld::isFragment(category);
    }

  }

namespace {
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

  }

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
