#pragma once

#include "collisionworld.h"

namespace JPH { class CharacterVirtual; }

// Read-only obstacle evidence, retained across the stair pass of one move.
void recordCharacterObstacle(CollisionWorld::CharacterMoveResult& result,
                             const JPH::CharacterVirtual& character, const JPH::PhysicsSystem& world,
                             const Tempest::Vec3& displacement);
