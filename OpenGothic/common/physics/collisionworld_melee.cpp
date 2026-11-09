#include "collisionworld_internal.h"

#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Body/BodyFilter.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/CollisionDispatch.h>
#include <Jolt/Physics/Collision/ShapeCast.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <stdexcept>

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

void CollisionWorld::boundShadowBody(MeleeShadowBody& body) {
  JPH::AABox bounds;
  body.valid=true;
  for(const auto& capsule:body.capsules) {
    const auto from=toMeters(capsule.from),to=toMeters(capsule.to);
    if(std::isfinite(from.LengthSq()) && std::isfinite(to.LengthSq()) &&
       std::isfinite(capsule.radiusCm) && capsule.radiusCm>0.f) {
      const auto radius=JPH::Vec3::sReplicate(capsule.radiusCm*.01f);
      bounds.Encapsulate(JPH::AABox(JPH::Vec3::sMin(from,to)-radius,JPH::Vec3::sMax(from,to)+radius));
      }
    else {
      body.valid=false;
      break;
      }
    }
  body.boundsMin={bounds.mMin.GetX(),bounds.mMin.GetY(),bounds.mMin.GetZ()};
  body.boundsMax={bounds.mMax.GetX(),bounds.mMax.GetY(),bounds.mMax.GetZ()};
  }

void CollisionWorld::updateShadowBody(Npc* owner, const std::vector<MeleeBodyCapsule>& capsules) {
  if(owner==nullptr) throw std::runtime_error("Melee shadow body requires an owner");
  MeleeShadowBody body;
  body.capsules=capsules;
  boundShadowBody(body);
  updateShadowBody(owner,body);
  }

void CollisionWorld::updateShadowBody(Npc* owner, const MeleeShadowBody& body, size_t* slotHint) {
  if(owner==nullptr) throw std::runtime_error("Melee shadow body requires an owner");
  if(!body.valid) throw std::runtime_error("Invalid melee shadow body capsule");
  std::lock_guard<std::mutex> guard(impl->shadowMutex);
  auto&  bodies=impl->shadowBodies;
  size_t at=slotHint!=nullptr ? *slotHint : size_t(-1);
  if(at>=bodies.size() || bodies[at].owner!=owner) {
    const auto [slot,added]=impl->shadowBodySlots.try_emplace(owner,bodies.size());
    if(added)
      bodies.push_back({owner,{},{},0});
    at=slot->second;
    }
  if(slotHint!=nullptr)
    *slotHint=at;
  auto& published=bodies[at];
  if(body.serial!=0 && published.serial==body.serial)
    return;
  published.capsules=body.capsules;
  // Vec3 keeps w equal to z, as the box that boundShadowBody encapsulated.
  published.bounds=JPH::AABox(JPH::Vec3(body.boundsMin.x,body.boundsMin.y,body.boundsMin.z),
                              JPH::Vec3(body.boundsMax.x,body.boundsMax.y,body.boundsMax.z));
  published.serial=body.serial;
  }

static MeleeShadowBody publishedShadowBody(const std::vector<MeleeBodyCapsule>& capsules, const JPH::AABox& bounds) {
  MeleeShadowBody out;
  out.capsules=capsules;
  out.boundsMin={bounds.mMin.GetX(),bounds.mMin.GetY(),bounds.mMin.GetZ()};
  out.boundsMax={bounds.mMax.GetX(),bounds.mMax.GetY(),bounds.mMax.GetZ()};
  out.valid=true;
  return out;
  }

bool CollisionWorld::shadowBody(const Npc* owner, MeleeShadowBody& out) const {
  std::lock_guard<std::mutex> guard(impl->shadowMutex);
  const auto slot=impl->shadowBodySlots.find(owner);
  if(slot==impl->shadowBodySlots.end())
    return false;
  const auto& body=impl->shadowBodies[slot->second];
  out=publishedShadowBody(body.capsules,body.bounds);
  return true;
  }

std::vector<std::pair<const Npc*,MeleeShadowBody>> CollisionWorld::shadowBodies() const {
  std::lock_guard<std::mutex> guard(impl->shadowMutex);
  std::vector<std::pair<const Npc*,MeleeShadowBody>> bodies;
  for(const auto& body:impl->shadowBodies)
    bodies.emplace_back(body.owner,publishedShadowBody(body.capsules,body.bounds));
  return bodies;
  }

void CollisionWorld::removeShadowBody(const Npc* owner) {
  std::lock_guard<std::mutex> guard(impl->shadowMutex);
  if(const auto slot=impl->shadowBodySlots.find(owner); slot!=impl->shadowBodySlots.end()) {
    // Keep the publication order: a cast records contacts of equal time and
    // bone in it. Removal is rare (death, suspension), reindexing the tail is cheap.
    const size_t at=slot->second;
    impl->shadowBodySlots.erase(slot);
    impl->shadowBodies.erase(impl->shadowBodies.begin()+std::ptrdiff_t(at));
    for(size_t i=at;i<impl->shadowBodies.size();++i)
      impl->shadowBodySlots[impl->shadowBodies[i].owner]=i;
    }
  // NPC destruction can precede a different attacker's journal flush.
  for(auto& blade:impl->shadowBlades) {
    std::erase(blade.hit,owner);
    std::erase_if(blade.contacts,[owner](const auto& contact) { return contact.npc==owner; });
    }
  }

void CollisionWorld::updateShadowBlade(const Npc* owner, uint64_t swingId,
                                      const std::vector<MeleeBladeSegment>& segments, uint64_t timeMs,
                                      double activeBeginMs, double activeEndMs, float walkableCos,
                                      Tempest::Vec3 forward, MeleeImpact::Weapon weapon) {
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
    it->hitItems.clear();
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
  it->weapon=weapon;
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
  wakeShadowItems();
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
    struct TimedContact {
      float time;
      MeleeBladeContact contact;
      JPH::BodyID item;
      JPH::Vec3 impulse=JPH::Vec3::sZero();
      bool itemOnly=false;
      };
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
            const auto layer=body.GetObjectLayer();
            const auto extent=body.GetWorldSpaceBounds().GetSize();
            record(hit,from.center,nullptr,size_t(-1),0);
            auto& contact=contacts.back().contact;
            if(itemWake && layer==DynamicWorld::C_Item && body.IsDynamic()) {
              // Shadow blades are swept queries, not solver bodies. Reuse the
              // same weapon momentum as physical melee reactions.
              contacts.back().item=body.GetID();
              const auto impulse=MeleeImpact::impulse(blade.weapon,contact.velocityCmS,contact.surfaceAngleDeg);
              contacts.back().impulse={impulse.x,impulse.y,impulse.z};
              }
            // Small loose objects receive momentum without arresting the
            // human swing or entering its damage/contact journal.
            if((layer==DynamicWorld::C_Item || layer==DynamicWorld::C_Fragment || layer==DynamicWorld::C_MovableVob) &&
               std::max({extent.GetX(),extent.GetY(),extent.GetZ()})<fragmentBlockSize) {
              if(!contacts.back().item.IsInvalid())contacts.back().itemOnly=true;
              else contacts.pop_back();
              continue;
              }
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
            // A board, crate or fence, or a remnant of one: the body that may take a stopped blow.
            if(object!=nullptr && object->ptrType()==DynamicWorld::P_MovableVob)
              contact.movableVob=reinterpret_cast<uintptr_t>(object->payload());
            else if(object!=nullptr && object->ptrType()==DynamicWorld::P_Fragment)
              contact.fragment=reinterpret_cast<uintptr_t>(object);
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
      if(!value.item.IsInvalid() && value.impulse.LengthSq()>0.f &&
         std::find(blade.hitItems.begin(),blade.hitItems.end(),value.item)==blade.hitItems.end()) {
        auto& interface=bodiesLocked ? impl->system.GetBodyInterfaceNoLock() : impl->system.GetBodyInterface();
        interface.AddImpulse(value.item,value.impulse,JPH::RVec3(toMeters(contact.point)));
        blade.hitItems.push_back(value.item);
        }
      if(value.itemOnly)continue;
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

void CollisionWorld::wakeItemsInBounds(const Tempest::Vec3& min, const Tempest::Vec3& max) {
  if(!itemWake || !itemWakeCallback || itemWakeQueries>=32 || itemWakeCreated>=8)
    return;
  ++itemWakeQueries;
  itemWakeCreated+=itemWakeCallback(min,max,8-itemWakeCreated);
  }

void CollisionWorld::wakeShadowItems() {
  if(!itemWake || !itemWakeCallback || !impl->hasShadowBlades.load(std::memory_order_acquire))
    return;
  std::vector<std::pair<Tempest::Vec3,Tempest::Vec3>> bounds;
  {
    std::lock_guard<std::mutex> guard(impl->shadowMutex);
    for(const auto& blade:impl->shadowBlades) {
      if(blade.blocked || blade.targetTimeMs<=blade.poseTimeMs ||
         blade.targetTimeMs<=blade.activeBeginMs || blade.poseTimeMs>=blade.activeEndMs)
        continue;
      JPH::AABox box;
      const double duration=blade.targetTimeMs-blade.poseTimeMs;
      const float first=float(std::clamp((blade.activeBeginMs-blade.poseTimeMs)/duration,0.,1.));
      const float last=float(std::clamp((blade.activeEndMs-blade.poseTimeMs)/duration,0.,1.));
      for(size_t i=0;i<blade.pose.size();++i) {
        // Every rotating endpoint remains inside the longest blade sphere
        // about its translating grip. This also encloses the fist/forearm.
        const auto& from=blade.pose[i];const auto& to=blade.target[i];
        const float reach=std::max((from.to-from.from).length(),(to.to-to.from).length())+
                          std::max(from.radiusCm,to.radiusCm);
        const auto radius=JPH::Vec3::sReplicate(reach*.01f);
        const auto a=toMeters(from.from+(to.from-from.from)*first);
        const auto b=toMeters(from.from+(to.from-from.from)*last);
        box.Encapsulate(JPH::AABox(JPH::Vec3::sMin(a,b)-radius,JPH::Vec3::sMax(a,b)+radius));
        }
      bounds.emplace_back(toCentimeters(box.mMin)-Tempest::Vec3(50),toCentimeters(box.mMax)+Tempest::Vec3(50));
      }
    }
  if(bounds.empty())return;
  itemShadowWakeCursor%=bounds.size();
  for(size_t visited=0;visited<bounds.size() && itemWakeQueries<32 && itemWakeCreated<8;++visited) {
    const auto& box=bounds[itemShadowWakeCursor];
    itemShadowWakeCursor=(itemShadowWakeCursor+1)%bounds.size();
    wakeItemsInBounds(box.first,box.second);
    }
  }
