// Engine side of NavigationEye: the game's physics as its RayCaster. The eye core
// itself (navigationeye.cpp) has no engine headers, so the nav-sim stand can build it.
#include "navigationeye.h"
#include "navigationeyeworld.h"

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
      const auto hit = navigationRay(physics,from,to,except);
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

DynamicWorld::NavigationRayResult navigationRay(const DynamicWorld& physics, const Vec3& from,
                                                const Vec3& to, const Npc* except) {
  const Ragdoll* ownRig = except!=nullptr ? except->livingBody() : nullptr;
  if(ownRig==nullptr && except!=nullptr)
    ownRig = except->ragdollBody();
  return physics.navigationRay(from,to,ownRig);
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
