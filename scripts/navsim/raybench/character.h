#pragma once
#include "backends.h"
#include <barrier>
#include <thread>
#include <malloc.h>

struct SolidLayers final : JPH::ObjectLayerFilter {
  bool ShouldCollide(JPH::ObjectLayer layer) const override {return layer!=3;}
};
// Running human recipe from VirtualCharacter::move: native floor sticking,
// then WalkStairs. No NPCs or dynamic props exist here; NAVSTAT1 does not
// identify interactive furniture, so its engine-specific stair veto is unavailable.
inline void moveCharacter(JPH::CharacterVirtual& c,JPH::TempAllocator& alloc,
                          JPH::Vec3 velocity,float step,float radius,float slope) {
  constexpr float dt=1.f/30;
  auto up=JPH::Vec3::sAxisY();
  c.UpdateGroundVelocity();
  velocity+=JPH::Vec3(0,-9.8f*dt,0);c.SetLinearVelocity(velocity);
  JPH::CharacterVirtual::ExtendedUpdateSettings settings;
  settings.mWalkStairsStepForwardTest=std::max(settings.mWalkStairsStepForwardTest,
      std::min(step,step/std::tan(slope))+radius+c.GetCharacterPadding());
  settings.mWalkStairsStepUp=JPH::Vec3::sZero();settings.mStickToFloorStepDown=JPH::Vec3(0,-step,0);
  SolidLayers layers;auto before=c.GetPosition();
  c.ExtendedUpdate(dt,JPH::Vec3(0,-9.8f,0),settings,{},layers,{},{},alloc);
  auto desired=velocity;desired.SetY(0);auto desiredStep=desired*dt;float length=desiredStep.Length();
  if(step>0&&length>0&&c.CanWalkStairs(velocity)) {
    auto forward=desiredStep/length;float achieved=std::max(0.f,JPH::Vec3(c.GetPosition()-before).Dot(forward));
    if(achieved+1.e-4f<length) {
      auto direction=forward;float best=settings.mWalkStairsCosAngleForwardContact;
      for(const auto& contact:c.GetActiveContacts()) {
        if(!contact.mHadCollision||contact.mWasDiscarded||contact.mSurfaceNormal.Dot(velocity-contact.mLinearVelocity)>=0||!c.IsSlopeTooSteep(contact.mSurfaceNormal))continue;
        auto candidate=contact.mSurfaceNormal.Dot(up)*up-contact.mSurfaceNormal;float n=candidate.Length();
        if(n>1.e-6f&&candidate.Dot(forward)/n>best) {direction=candidate/n;best=direction.Dot(forward);}
      }
      c.WalkStairs(dt,up*step,direction*std::max(settings.mWalkStairsMinStepForward,length-achieved),
                   direction*settings.mWalkStairsStepForwardTest,settings.mWalkStairsStepDownExtra,{},layers,{},{},alloc);
    }
  }
}
inline void characters(JoltScene& scene,const Input& in,const std::filesystem::path& out,const std::vector<int>& allowed,bool scaling=false) {
  float radius,height,step,slope,speed,eyeHeight;
  std::ifstream params(out/"character.txt");params>>radius>>height>>step>>slope>>speed>>eyeHeight;require(bool(params),"Invalid character parameters");
  radius*=.01f;height*=.01f;step*=.01f;speed*=.01f;slope*=float(JPH::JPH_PI)/180;
  std::vector<Vec> feet;std::vector<double> floorOffsets;
  // Sample the full corpus evenly, accepting only nearby walkable solid floors.
  for(size_t k=0;k<in.updates.size();++k) {
    size_t i=(k*239)%in.updates.size();auto foot=in.updates[i].origin-Vec{0,eyeHeight,0};
    JPH::RRayCast ray(meters(foot+Vec{0,80,0}),JPH::Vec3(0,-3.8f,0));
    JPH::RayCastSettings settings;settings.SetBackFaceMode(JPH::EBackFaceMode::IgnoreBackFaces);
    JPH::ClosestHitCollisionCollector<JPH::CastRayCollector> hit;SolidLayers solid;
    scene.physics.GetNarrowPhaseQueryNoLock().CastRay(ray,settings,hit,{},solid);
    if(!hit.HadHit())continue;
    const auto& h=hit.mHit;JPH::BodyLockRead lock(scene.physics.GetBodyLockInterfaceNoLock(),h.mBodyID);
    if(lock.GetBody().GetWorldSpaceSurfaceNormal(h.mSubShapeID2,ray.GetPointOnRay(h.mFraction)).GetY()<std::cos(slope))continue;
    float offset=80-380*h.mFraction;if(std::abs(offset)>60)continue;
    floorOffsets.push_back(offset);foot.y+=offset+1;feet.push_back(foot);if(feet.size()==1000)break;
  }
  require(feet.size()==1000,"Fewer than 1000 supported character positions");
  std::ofstream positions(out/"character-feet.bin",std::ios::binary);positions.write(reinterpret_cast<const char*>(feet.data()),feet.size()*sizeof(Vec));
  std::cout<<"{\"type\":\"character_setup\",\"positions\":"<<feet.size()<<",\"radius_cm\":"<<radius*100<<",\"height_cm\":"<<height*100
           <<",\"step_cm\":"<<step*100<<",\"slope_degrees\":"<<slope*180/JPH::JPH_PI<<",\"speed_cm_s\":"<<speed*100<<",\"eye_height_cm\":"<<eyeHeight
           <<",\"floor_offset_cm\":";distribution(std::cout,floorOffsets);std::cout<<"}\n";
  auto counts=scaling?std::vector<size_t>{1000}:std::vector<size_t>{1,100,1000};
  auto threadCounts=scaling?std::vector<size_t>{1,4,8,16,allowed.size()}:std::vector<size_t>{1,allowed.size()};
  std::sort(threadCounts.begin(),threadCounts.end());threadCounts.erase(std::unique(threadCounts.begin(),threadCounts.end()),threadCounts.end());
  const auto layouts=scaling?std::vector<std::string>{"shared","private","shared-zero-mass"}:std::vector<std::string>{"private"};
  for(size_t count:counts)for(size_t threads:threadCounts)if(threads<=allowed.size())for(const auto& layout:layouts) {
    if(layout=="shared-zero-mass"&&threads!=1&&threads!=allowed.size())continue;
    for(int repeat=0;repeat<Repeats;++repeat) {
      std::vector<std::unique_ptr<JoltScene>> copies;
      copies.reserve(layout=="private"?threads:0);
      const auto before=mallinfo2();
      if(layout=="private")for(size_t t=0;t<threads;++t)copies.push_back(std::make_unique<JoltScene>(scene));
      const auto after=mallinfo2();
      const int64_t copyBytes=int64_t(after.uordblks+after.hblkhd)-int64_t(before.uordblks+before.hblkhd);
      require(copyBytes>=0,"Invalid scene allocation measurement");
      std::vector<JPH::Ref<JPH::CharacterVirtual>> chars;std::vector<JPH::Vec3> velocities;
      // Construct IDs/settings on one thread; each character is then private to one worker.
      for(size_t i=0;i<count;++i) {
        JPH::CharacterVirtualSettings settings;
        settings.mShape=new JPH::CapsuleShape(std::max(height*.5f-radius,.001f),radius);
        settings.mShapeOffset=JPH::Vec3(0,height*.5f,0);settings.mSupportingVolume=JPH::Plane(JPH::Vec3::sAxisY(),-radius);
        settings.mCharacterPadding=.01f;settings.mPredictiveContactDistance=.05f;settings.mCollisionTolerance=.001f;
        settings.mEnhancedInternalEdgeRemoval=true;settings.mMaxSlopeAngle=slope;
        // Diagnostic control: no-op static-ground impulses still take write locks.
        if(layout=="shared-zero-mass")settings.mMass=0;
        auto* system=layout=="private"?&copies[i%threads]->physics:&scene.physics;
        chars.emplace_back(new JPH::CharacterVirtual(&settings,JPH::RVec3(meters(feet[i])),JPH::Quat::sIdentity(),system));
        // Fixed pseudorandom course per independent agent, identical between repeats.
        float angle=float((i*2654435761u)%100000)*float(2*JPH::JPH_PI/100000);
        velocities.emplace_back(std::cos(angle)*speed,0,std::sin(angle)*speed);
      }
      std::barrier warm(int(threads)+1),ready(int(threads)+1),done(int(threads)+1);
      std::vector<std::thread> workers;std::vector<double> checksums(threads);
      for(size_t t=0;t<threads;++t)workers.emplace_back([&,t] {
        pin(allowed[t]);JPH::TempAllocatorImpl alloc(16*1024*1024);
        for(int frame=0;frame<30;++frame)for(size_t i=t;i<count;i+=threads)moveCharacter(*chars[i],alloc,velocities[i],step,radius,slope);
        warm.arrive_and_wait();ready.arrive_and_wait();
        for(int frame=0;frame<90;++frame)for(size_t i=t;i<count;i+=threads)moveCharacter(*chars[i],alloc,velocities[i],step,radius,slope);
        for(size_t i=t;i<count;i+=threads)checksums[t]+=chars[i]->GetPosition().GetY();
        done.arrive_and_wait();
      });
      warm.arrive_and_wait();
      const auto load=loadAverage();
      auto start=Clock::now();ready.arrive_and_wait();done.arrive_and_wait();double wall=us(start);
      for(auto& w:workers)w.join();
      double checksum=0;for(size_t t=0;t<threads;++t)checksum+=checksums[t];
      std::cout<<"{\"type\":\"character\",\"count\":"<<count<<",\"threads\":"<<threads<<",\"layout\":"<<std::quoted(layout)<<",\"repeat\":"<<repeat
               <<",\"wall_us_per_character\":"<<wall/(90*count)
               <<",\"character_updates_per_second\":"<<90*count*1.e6/wall<<",\"checksum\":"<<checksum<<",\"physics_copies\":"<<copies.size()
               <<",\"allocated_bytes_per_copy\":"<<(copies.empty()?0:double(copyBytes)/copies.size());
      loadJson(load);std::cout<<"}\n"<<std::flush;
    }
  }
}
