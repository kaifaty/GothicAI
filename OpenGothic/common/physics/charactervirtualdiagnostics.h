#pragma once

#include <Tempest/Vec>
#include <cstdint>
#include <string>
#include <vector>

class Npc;

// Opt-in, transient bench data. Positions/distances use cm, linear velocities
// cm/s, angular velocities rad/s. Resolve payload pointers when taking a sample;
// copied diagnostics do not own the game objects to which those pointers refer.
struct CharacterVirtualDiagnostics {
  struct NpcCollision {
    Npc* npc = nullptr;
    Tempest::Vec3 position = {}, normal = {}, testedPosition = {};
    float allowedDepth = 0.f, rejectedDepth = 0.f;
    };

  struct NpcMove {
    // initial/nativePosition/accepted are world feet; requested/clippedRequest
    // are displacement vectors. Neighbour pointers have the same lifetime
    // restriction as Body::payload and must be resolved when sampling.
    Tempest::Vec3 initial = {}, requested = {}, clippedRequest = {}, nativePosition = {}, accepted = {};
    Tempest::Vec3 steepContactNormal = {};
    uint64_t sequence = 0, dt = 0;
    float advancementRatio = 1.f;
    bool steepContact = false;
    bool requestClipped = false, resolvedClipped = false, blocked = false;
    NpcCollision requestCollision, resolvedCollision;
    };

  struct Body {
    uint32_t id = uint32_t(-1);
    int layer = 0, pointerType = 0, motionType = 0;
    uintptr_t nativeUserData = 0;
    void* payload = nullptr;
    std::string label;
    float mass = 0.f;
    Tempest::Vec3 position = {}, linearVelocity = {}, angularVelocity = {}, pointVelocity = {};
    bool present = false;
    };

  struct Contact {
    Body body;
    uint32_t subShape = uint32_t(-1);
    uintptr_t contactUserData = 0;
    Tempest::Vec3 position = {}, contactNormal = {}, surfaceNormal = {}, pointVelocity = {};
    float distance = 0.f, supportingVolumeDistance = 0.f;
    bool hadCollision = false, discarded = false, sensor = false, canPushCharacter = false;
    bool withinSupportingVolume = false, walkableSlope = false, supportAllowedByPolicy = false;
    // Individual walkable support. Jolt can also support a character through a
    // combination of steep contacts, which is described by pose.groundState.
    bool supportEligible = false;
    };

  struct Pose {
    Tempest::Vec3 position = {}, velocity = {}, groundPosition = {}, groundNormal = {}, groundVelocity = {};
    Body ground;
    uint32_t groundSubShape = uint32_t(-1);
    uintptr_t groundUserData = 0;
    uint8_t groundState = 0; // Jolt: 0 OnGround, 1 OnSteepGround, 2 NotSupported, 3 InAir.
    std::vector<Contact> contacts;
    };

  struct StairProbe {
    std::string reason = "notAttempted";
    Tempest::Vec3 upPosition = {}, forwardPosition = {}, finalPosition = {};
    Tempest::Vec3 testPosition = {}, testFinalPosition = {};
    Tempest::Vec3 landingNormal = {}, testLandingNormal = {};
    float upFraction = 1.f, landingFraction = -1.f, testLandingFraction = -1.f;
    };

  uint64_t dt = 0;
  // Cumulative invalid/near-zero native normal reads for this controller;
  // includes ordinary airborne zero sentinels and diagnostic reads.
  uint64_t groundNormalFallbacks = 0;
  Tempest::Vec3 requested = {};
  Tempest::Vec3 requestedVelocity = {}, stairDesiredVelocity = {};
  float maxSlopeRadians = 0.f, stairHeight = 0.f;
  float stairDesiredDistance = 0.f, stairAchievedDistance = 0.f;
  Pose before, afterExtendedUpdate, afterStairs;
  NpcMove npcMove;
  StairProbe stairProbe;
  uint32_t stairObstacleId = uint32_t(-1);
  // ExtendedUpdate includes native floor sticking; automatic native stairs are
  // disabled there. afterStairs is the separate world-stair recipe's result.
  bool stairSupported = false, stairCanWalk = false, stairBlockedByPolicy = false;
  bool stairProgressBlocked = false;
  bool stairAttempted = false, stairSucceeded = false, valid = false;
  };
