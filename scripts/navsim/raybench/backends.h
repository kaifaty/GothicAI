#pragma once
#include <Jolt/Jolt.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <embree4/rtcore.h>
#include "scene.h"

struct Layers final : JPH::BroadPhaseLayerInterface {
  JPH::uint GetNumBroadPhaseLayers() const override {return 1;}
  JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer) const override {return JPH::BroadPhaseLayer(0);}
};
struct PairFilter final : JPH::ObjectLayerPairFilter {
  bool ShouldCollide(JPH::ObjectLayer,JPH::ObjectLayer) const override {return false;}
};
struct BroadFilter final : JPH::ObjectVsBroadPhaseLayerFilter {
  bool ShouldCollide(JPH::ObjectLayer,JPH::BroadPhaseLayer) const override {return true;}
};
inline JPH::Vec3 meters(Vec v) {return {v.x*.01f,v.y*.01f,v.z*.01f};}
struct JoltScene {
  Layers layers;PairFilter pair;BroadFilter broad;
  JPH::PhysicsSystem physics;
  std::vector<JPH::BodyID> ids;
  double buildUs=0;
  // NAVSTAT1 has no body IDs. This layout measures four category meshes only;
  // it cannot reproduce the insertion tree of the engine's 6993 source bodies.
  explicit JoltScene(const Input& in) {
    auto start=Clock::now();physics.Init(16,0,1024,1024,layers,broad,pair);
    for(uint8_t c=1;c<=4;++c) {
      JPH::TriangleList triangles;
      for(size_t i=0;i<in.triangles.size();++i)if(in.categories[i]==c) {
        const auto& t=in.triangles[i];triangles.emplace_back(meters(t.v[0]),meters(t.v[1]),meters(t.v[2]),0,uint32_t(i));
      }
      if(triangles.empty())continue;
      JPH::MeshShapeSettings mesh(triangles);mesh.mPerTriangleUserData=true;
      auto shape=mesh.Create();require(!shape.HasError(),shape.HasError()?shape.GetError().c_str():"");
      JPH::BodyCreationSettings body(shape.Get(),JPH::RVec3::sZero(),JPH::Quat::sIdentity(),JPH::EMotionType::Static,c);
      body.mUserData=kind(c);
      ids.push_back(physics.GetBodyInterface().CreateAndAddBody(body,JPH::EActivation::DontActivate));
      require(!ids.back().IsInvalid(),"Jolt body allocation failed");
    }
    buildUs=us(start);
  }
  // Each worker owns its body locks and broadphase; immutable mesh BVHs are shared.
  explicit JoltScene(const JoltScene& source) {
    physics.Init(16,0,1024,1024,layers,broad,pair);
    for(auto id:source.ids) {
      JPH::BodyLockRead lock(source.physics.GetBodyLockInterfaceNoLock(),id);
      const auto& original=lock.GetBody();require(original.IsStatic(),"Only static scene copies are supported");
      JPH::BodyCreationSettings body(original.GetShape(),original.GetPosition(),original.GetRotation(),
                                    JPH::EMotionType::Static,original.GetObjectLayer());
      body.mUserData=original.GetUserData();
      auto copy=physics.GetBodyInterface().CreateAndAddBody(body,JPH::EActivation::DontActivate);
      require(!copy.IsInvalid(),"Jolt body copy failed");ids.push_back(copy);
      require(physics.GetBodyInterfaceNoLock().GetShape(copy).GetPtr()==original.GetShape(),"Static mesh was not shared");
    }
    physics.OptimizeBroadPhase();
  }
  ~JoltScene() {for(auto id:ids) {physics.GetBodyInterface().RemoveBody(id);physics.GetBodyInterface().DestroyBody(id);}}
  void cast(const Vec* o,const Vec* d,size_t count,Hit* out,int,bool cull) const {
    JPH::RayCastSettings settings;settings.mTriangleEdgeTolerance=1.e-4f;
    settings.SetBackFaceMode(cull?JPH::EBackFaceMode::IgnoreBackFaces:JPH::EBackFaceMode::CollideWithBackFaces);
    for(size_t i=0;i<count;++i) {
      // Inputs are the exact cm endpoints, formed by NavigationEye.
      // cast() returns local segment fractions; eye() converts them to cm.
      JPH::RRayCast ray(meters(o[i]),meters(d[i])-meters(o[i]));
      JPH::ClosestHitCollisionCollector<JPH::CastRayCollector> hit;
      physics.GetNarrowPhaseQueryNoLock().CastRay(ray,settings,hit);
      out[i]={};
      if(hit.HadHit()) {
        const auto& h=hit.mHit;
        JPH::BodyLockRead lock(physics.GetBodyLockInterfaceNoLock(),h.mBodyID);
        const auto& b=lock.GetBody();
        out[i]={h.mFraction,b.GetWorldSpaceSurfaceNormal(h.mSubShapeID2,ray.GetPointOnRay(h.mFraction)).GetY(),uint32_t(b.GetUserData()),1};
      }
    }
  }
};

struct EmbreeScene {
  RTCDevice device=nullptr;RTCScene scene=nullptr;
  double buildUs=0;
  explicit EmbreeScene(const Input& in) {
    auto start=Clock::now();
    // Scene construction uses one worker; ray traversal uses caller threads.
    device=rtcNewDevice("threads=1");require(device!=nullptr,"Embree device creation failed");
    scene=rtcNewScene(device);
    rtcSetSceneFlags(scene,RTC_SCENE_FLAG_FILTER_FUNCTION_IN_ARGUMENTS);
    for(uint8_t c=1;c<=4;++c) {
      size_t n=std::count(in.categories.begin(),in.categories.end(),c);if(!n)continue;
      auto g=rtcNewGeometry(device,RTC_GEOMETRY_TYPE_TRIANGLE);
      auto* v=static_cast<Vec*>(rtcSetNewGeometryBuffer(g,RTC_BUFFER_TYPE_VERTEX,0,RTC_FORMAT_FLOAT3,sizeof(Vec),n*3));
      auto* idx=static_cast<uint32_t*>(rtcSetNewGeometryBuffer(g,RTC_BUFFER_TYPE_INDEX,0,RTC_FORMAT_UINT3,12,n));
      size_t j=0;for(size_t i=0;i<in.triangles.size();++i)if(in.categories[i]==c) {
        for(int k=0;k<3;++k) {v[j*3+k]=in.triangles[i].v[k];idx[j*3+k]=uint32_t(j*3+k);}++j;
      }
      rtcSetGeometryEnableFilterFunctionFromArguments(g,true);
      rtcCommitGeometry(g);rtcAttachGeometryByID(scene,g,c);rtcReleaseGeometry(g);
    }
    rtcCommitScene(scene);require(rtcGetDeviceError(device)==RTC_ERROR_NONE,"Embree BVH construction failed");buildUs=us(start);
  }
  ~EmbreeScene() {rtcReleaseScene(scene);rtcReleaseDevice(device);}
  static void backfaces(const RTCFilterFunctionNArguments* a) {
    for(unsigned i=0;i<a->N;++i)if(a->valid[i]) {
      const auto n=a->N;
      float dot=RTCRayN_dir_x(a->ray,n,i)*RTCHitN_Ng_x(a->hit,n,i)
               +RTCRayN_dir_y(a->ray,n,i)*RTCHitN_Ng_y(a->hit,n,i)
               +RTCRayN_dir_z(a->ray,n,i)*RTCHitN_Ng_z(a->hit,n,i);
      if(dot>=0)a->valid[i]=0;
    }
  }
  template<int W,class Packet> void packet(const Vec* o,const Vec* d,size_t count,Hit* out,bool cull) const {
    for(size_t base=0;base<count;base+=W) {
      alignas(64) Packet p{};alignas(64) int valid[W]{};
      auto* rn=reinterpret_cast<RTCRayN*>(&p.ray);auto* hn=reinterpret_cast<RTCHitN*>(&p.hit);
      for(int j=0;j<W;++j) {
        size_t i=base+j;
        RTCHitN_geomID(hn,W,j)=RTC_INVALID_GEOMETRY_ID;
        RTCHitN_instID(hn,W,j,0)=RTC_INVALID_GEOMETRY_ID;
        if(i>=count)continue;
        valid[j]=-1;RTCRayN_org_x(rn,W,j)=o[i].x;RTCRayN_org_y(rn,W,j)=o[i].y;RTCRayN_org_z(rn,W,j)=o[i].z;
        RTCRayN_dir_x(rn,W,j)=d[i].x-o[i].x;RTCRayN_dir_y(rn,W,j)=d[i].y-o[i].y;RTCRayN_dir_z(rn,W,j)=d[i].z-o[i].z;
        RTCRayN_tfar(rn,W,j)=1;RTCRayN_mask(rn,W,j)=~0u;
      }
      RTCIntersectArguments args;rtcInitIntersectArguments(&args);if(cull)args.filter=backfaces;
      if constexpr(W==1)rtcIntersect1(scene,&p,&args);
      if constexpr(W==4)rtcIntersect4(valid,scene,&p,&args);
      if constexpr(W==8)rtcIntersect8(valid,scene,&p,&args);
      if constexpr(W==16)rtcIntersect16(valid,scene,&p,&args);
      for(int j=0;j<W&&base+j<count;++j) {
        auto& h=out[base+j];h={};unsigned c=RTCHitN_geomID(hn,W,j);
        if(c!=RTC_INVALID_GEOMETRY_ID) {
          float x=RTCHitN_Ng_x(hn,W,j),y=RTCHitN_Ng_y(hn,W,j),z=RTCHitN_Ng_z(hn,W,j);
          h={RTCRayN_tfar(rn,W,j),y/std::sqrt(x*x+y*y+z*z),kind(uint8_t(c)),1};
        }
      }
    }
  }
  void cast(const Vec* o,const Vec* d,size_t n,Hit* h,int width,bool cull) const {
    switch(width) {
      case 1:packet<1,RTCRayHit>(o,d,n,h,cull);break;
      case 4:packet<4,RTCRayHit4>(o,d,n,h,cull);break;
      case 8:packet<8,RTCRayHit8>(o,d,n,h,cull);break;
      case 16:packet<16,RTCRayHit16>(o,d,n,h,cull);break;
      default:throw std::runtime_error("Unsupported packet width");
    }
  }
};
