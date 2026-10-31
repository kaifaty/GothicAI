#pragma once

#include "game/npckeys.h"
#include "physics/navmesh.h"

// Stand-only privileged keyboard teacher. This is a function of the current body and route;
// it owns no timer, previous action, cursor or successful-transition history. Replacing the
// route with findPath(currentPosition, goal) is sufficient to label a student's state.
class NavKeyTeacher final {
  public:
    struct State final {
      NavMesh::Point position = {};       // metres, Y up
      NavMesh::Point velocity = {};       // metres/second
      float yawDegrees = 0;               // NPC convention: 0 = +X, 90 = +Z
      bool supported = true;
      bool standing = true;               // native animation gate for tryJump/startClimb
      bool inAir = false;
      bool falling = false;
      bool sliding = false;
      bool climbing = false;
      bool swimming = false;
      bool jumping = false;
      float turnSpeedDegrees = 90;        // degrees/second (180 with a weapon)
      };

    struct Hint final {
      NavMesh::Point nextCorner = {};     // actual steering target: mesh corner or v2 carrot, metres
      float remainingLength = 0;          // current position to route endpoint, metres
      size_t cornerIndex = 0;
      uint32_t transitionId = 0;
      NavMesh::LinkType transitionType = NavMesh::LinkType::Jump;
      bool onTransition = false;
      };

    struct Decision final {
      NpcKeys::KeyState keys;
      Hint hint;
      bool pathValid = false;
      bool replan = false;                // no path, invalid input or >1.5 m from route
      bool arrived = false;               // within 1 m of goal, as in the native teacher
      };

    // Decisions are held for exactly 100 ms by the runner. While airborne/climbing retain
    // the pre-takeoff path: a new Detour query can legitimately fail between the two banks.
    // Jump/climb approach their precise start walking, wait there for standing and yaw
    // alignment in run-mode idle, then press run+forward+action. Drops share the precise
    // start; forward velocity along their narrow corridor keeps an ongoing departure moving.
    static Decision decide(const State& body, const NavMesh::Path& path,
                           const NavMesh::Point& goal);

    // V2 shares the learner's carrot. Clear ground steers directly to that point;
    // obstacles and transitions use the local mesh route ending at the carrot.
    // Only the final goal can satisfy arrival. Pass false for directWalkable beyond
    // the familiar road, where the mesh route leads to the final goal. Ground turns
    // use a 10-degree deadband; transitions retain the v1 digital steering contract.
    static Decision decideV2(const State& body, const NavMesh::Path& localPath,
                             const NavMesh::Point& goal, const NavMesh::Point& carrot,
                             bool directWalkable);
  };
