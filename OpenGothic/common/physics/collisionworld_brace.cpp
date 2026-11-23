#include "collisionworld_internal.h"
#include "ragdoll_brace.h"

// Touches of braced rigs (ragdoll_brace.h): contact listener on the physics workers,
// the table of rigs on the main thread between native updates.

void CollisionWorld::trackBraceContacts(uint32_t group, RagdollBrace::Contacts* contacts) {
  auto& list=impl->braceContacts;
  list.erase(std::remove_if(list.begin(),list.end(),[&](const auto& entry) { return entry.first==group; }),list.end());
  if(contacts!=nullptr)
    list.emplace_back(group,contacts);
  }

void CollisionWorld::Impl::markBraceContact(const JPH::Body& a, const JPH::Body& b, const JPH::ContactManifold& manifold) {
  // A rig's own parts share its group: they never touch "the world". Sensors are no support.
  const auto groupA=a.GetCollisionGroup().GetGroupID(), groupB=b.GetCollisionGroup().GetGroupID();
  if(a.IsSensor() || b.IsSensor() || (groupA==groupB && groupA!=JPH::CollisionGroup::cInvalidGroup))
    return;
  float approach=-1.f;
  for(const JPH::Body* part:{&a,&b}) {
    if(part->GetObjectLayer()!=DynamicWorld::C_Ragdoll) continue;
    const auto group=part->GetCollisionGroup().GetGroupID();
    for(const auto& [id,contacts]:braceContacts) {
      if(id!=group) continue;
      if(approach<0.f) {
        approach=0.f;
        for(JPH::uint i=0;i<manifold.mRelativeContactPointsOn1.size();++i) {
          const auto point=manifold.GetWorldSpaceContactPointOn1(i);
          approach=std::max(approach,(a.GetPointVelocity(point)-b.GetPointVelocity(point)).Dot(manifold.mWorldSpaceNormal));
          }
        }
      // A speculative contact (negative depth) of surfaces still apart is a touch only if
      // they close within this native step: a hand passing a centimetre from a table is not.
      constexpr float step=1.f/60.f; // CollisionWorld::tick
      if(manifold.mPenetrationDepth<0.f && approach*step< -manifold.mPenetrationDepth)
        return;
      contacts->mark(part->GetCollisionGroup().GetSubGroupID(),manifold.mPenetrationDepth,approach);
      break;
      }
    }
  }
