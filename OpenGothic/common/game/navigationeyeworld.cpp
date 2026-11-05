// Engine side of NavigationEye: the game's physics as its RayCaster. The eye core
// itself (navigationeye.cpp) has no engine headers, so the nav-sim stand can build it.
#include "navigationeye.h"

#include "physics/collisionworld.h"
#include "physics/dynamicworld.h"
#include "world/objects/npc.h"
#include "world/world.h"

#include <algorithm>
#include <chrono>

namespace {
using Tempest::Vec3;

NavigationEye::HitKind eyeKind(DynamicWorld::NavigationHit kind) {
  using H = DynamicWorld::NavigationHit;
  using E = NavigationEye::HitKind;
  switch(kind) {
    case H::None:       return E::None;
    case H::World:      return E::World;
    case H::StaticVob:  return E::StaticVob;
    case H::MovableVob: return E::MovableVob;
    case H::Npc:        return E::Npc;
    case H::Water:      return E::Water;
    }
  return E::None;
  }

class WorldRays final : public NavigationEye::RayCaster {
  public:
    explicit WorldRays(const DynamicWorld& physics):physics(physics) {}

    NavigationEye::RayHit cast(const Vec3& from, const Vec3& to, const Npc* except) const override {
      const auto hit = physics.navigationRay(from,to,except);
      NavigationEye::RayHit result;
      result.hit = hit.hasCol;
      result.point = hit.point;
      result.kind = eyeKind(hit.kind);
      const float normalLength = hit.normal.length();
      result.normalY = normalLength>1.e-6f ? std::clamp(hit.normal.y/normalLength,-1.f,1.f) : 0.f;
      return result;
      }

  private:
    const DynamicWorld& physics;
  };
}

// Kept beside the sensor so the navigation experiment has no edits in walking code.
DynamicWorld::NavigationRayResult DynamicWorld::navigationRay(const Vec3& from, const Vec3& to,
                                                              const Npc* except) const {
  constexpr uint32_t mask = (1u<<C_Landscape)|(1u<<C_Object)|(1u<<C_Item)|(1u<<C_MovableVob)|
                            (1u<<C_Ragdoll)|(1u<<C_LivingBody)|(1u<<C_ActiveBody)|(1u<<C_Water);
  const Ragdoll* ownRig = except!=nullptr ? except->livingBody() : nullptr;
  if(ownRig==nullptr && except!=nullptr)
    ownRig = except->ragdollBody();
  const auto hit = world->rayCast(from,to,mask,false,nullptr,nullptr,ownRig);
  NavigationRayResult result;
  result.hasCol = hit.hasHit;
  result.point = hit.hasHit ? hit.point : to;
  result.normal = hit.normal;
  if(!hit.hasHit)
    return result;
  const int category = hit.body!=nullptr ? hit.body->category() : world->navigationBodyCategory(hit.nativeBodyId);
  switch(category) {
    case C_Ragdoll:
    case C_LivingBody:
    case C_ActiveBody: result.kind = NavigationHit::Npc; break;
    case C_Landscape: result.kind = NavigationHit::World; break;
    case C_Water: result.kind = NavigationHit::Water; break;
    case C_Object:
      result.kind = hit.body->isMovable() ? NavigationHit::MovableVob : NavigationHit::StaticVob;
      break;
    case C_Item:
    case C_MovableVob: result.kind = NavigationHit::MovableVob; break;
    default: result.kind = NavigationHit::StaticVob; break;
    }
  return result;
  }

NavigationEye::Update NavigationEye::update(const Npc& npc, uint64_t gameMs) {
  const auto begin = std::chrono::steady_clock::now();
  if(sampled && gameMs>=lastNear && gameMs-lastNear<cfg.nearPeriodMs)
    return {};
  const auto* physics = npc.world().physic();
  if(physics==nullptr)
    return {};
  const auto head = npc.navigationEyePose();
  auto result = update({head.origin,head.forward,head.right,head.up,npc.position()},*physics,&npc,gameMs);
  if(result.updated)
    result.costUs = std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-begin).count();
  return result;
  }

NavigationEye::Update NavigationEye::update(const Pose& pose, const DynamicWorld& physics,
                                            const Npc* except, uint64_t gameMs) {
  return update(pose,WorldRays(physics),except,gameMs);
  }
