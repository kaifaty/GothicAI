#include "characterobstacle.h"

#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/PhysicsSystem.h>

void recordCharacterObstacle(CollisionWorld::CharacterMoveResult& result,
                             const JPH::CharacterVirtual& character, const JPH::PhysicsSystem& world,
                             const Tempest::Vec3& displacement) {
  const Tempest::Vec3 horizontal(displacement.x,0,displacement.z);
  float strongest = Tempest::Vec3::dotProduct(result.obstacleNormal,horizontal);
  for(const auto& contact:character.GetActiveContacts()) {
    if(!contact.mHadCollision || contact.mWasDiscarded || contact.mIsSensorB ||
       !character.IsSlopeTooSteep(contact.mSurfaceNormal))
      continue;
    Tempest::Vec3 normal(contact.mSurfaceNormal.GetX(),0,contact.mSurfaceNormal.GetZ());
    const float length = normal.length();
    if(length<=0.f)
      continue;
    normal /= length;
    const float into = Tempest::Vec3::dotProduct(normal,horizontal);
    if(into>=strongest)
      continue;
    JPH::BodyLockRead lock(world.GetBodyLockInterface(),contact.mBodyB);
    if(!lock.SucceededAndIsInBroadPhase())
      continue;
    strongest = into;
    result.obstacleNormal = normal;
    result.obstacleDynamic = !lock.GetBody().IsStatic();
    }
  }
