#include "dynamicworld.h"

using Tempest::Vec3;

DynamicWorld::NavigationRayResult DynamicWorld::navigationRay(const Vec3& from, const Vec3& to,
                                                              const Ragdoll* ownRig) const {
  constexpr uint32_t mask = (1u<<C_Landscape)|(1u<<C_Object)|(1u<<C_Item)|(1u<<C_MovableVob)|
                            (1u<<C_Ragdoll)|(1u<<C_LivingBody)|(1u<<C_ActiveBody)|(1u<<C_Water);
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

std::array<size_t,4> DynamicWorld::navigationCollisionTriangles(
    const std::function<void(const CollisionWorld::StaticTriangle&)>& output) const {
  return world->staticTriangles(output,true);
  }
