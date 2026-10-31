#pragma once

#include <Tempest/Matrix4x4>
#include <Tempest/Vec>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

class CollisionWorld;
class DynamicWorld;
class HitZones;
class Skeleton;
class Npc;

// A limp body or an animation-driven kinematic rig. Game code uses centimetres,
// centimetres/second and radians/second;
// the implementation alone owns Jolt bodies and constraints.
class Ragdoll final {
  public:
    struct PartState final {
      uint32_t            bone = 0;
      Tempest::Matrix4x4  transform;
      // Initial rendered world frame, including the NPC's positive scale.
      // Native shapes/anchors and the visual affine correction are rebuilt from it.
      Tempest::Matrix4x4  referenceTransform;
      // Version 3 retains the native float pose without the lossy cm/m and
      // matrix/quaternion round trips. These are COM metres and quaternion xyzw.
      Tempest::Vec3       centerOfMassMeters = {};
      Tempest::Vec4       rotation = {0,0,0,1};
      Tempest::Vec3       linearVelocityMeters = {};
      Tempest::Vec3       linearVelocity = {};
      Tempest::Vec3       angularVelocity = {};
      bool                sleeping = false;
      };
    struct AncestorState final {
      uint32_t           bone = 0;
      uint32_t           anchorBone = 0;
      Tempest::Matrix4x4 referenceTransform;
      };
    struct State final {
      uint32_t               version = 3;
      std::string            skeleton;
      Tempest::Matrix4x4     rootTransform;
      std::vector<PartState> parts;
      std::vector<AncestorState> ancestors;
      // Only old development snapshots (v1/v2) lack historic ancestor frames.
      // Their v3 re-export explicitly retains the legacy animated-root fallback.
      bool                   legacyAncestors = false;
      };

    ~Ragdoll();
    Ragdoll(const Ragdoll&) = delete;
    Ragdoll& operator=(const Ragdoll&) = delete;

    bool valid() const;
    // True only for a completely kinematic living rig, never an active reaction
    // or a limp corpse.
    bool isKinematic() const;
    // Queue an already solved world pose for the next fixed physics step.
    // Rejects invalid poses or a changed object scale without changing the rig.
    bool moveKinematic(const Tempest::Matrix4x4* worldPose, size_t numBones,
                       const Tempest::Matrix4x4& objectMatrix, float deltaSeconds);
    // Queue one point impulse for the next fixed step. pointCm is world-space
    // centimetres; impulseNs is world-space newton seconds (kg * metres/second),
    // not a velocity. strength is the visual/motor response in (0, 1].
    // A living pelvis hit reacts through the nearest spine; a limp hit always
    // addresses the supplied physical bone. No native world calls happen here.
    // stiffness >= 1 scales the living motors' spring frequency (torque with its
    // square): a blade's reaction rides a fast authored stumble, an arrow's does not.
    bool queueHit(size_t bone, const Tempest::Vec3& pointCm,
                  const Tempest::Vec3& impulseNs, float strength, Npc* culprit = nullptr,
                  float stiffness = 1.f);
    void setNpc(Npc* npc);
    void setNoiseCulprit(Npc* culprit);
    float reactionWeight() const;
    const std::vector<size_t>& reactionBones() const;
    enum class BenchMode : uint8_t { Full, Chain };
    enum class BenchPelvis : uint8_t { Kinematic, Dynamic };
    struct BenchStats final {
      size_t dynamicParts = 0;
      float maxLinearCmS = 0, maxAngularRadS = 0;
      float poseRmsCm = 0, poseMaxCm = 0;
      // Feet/toes' world AABB bottom and downward deviation from animation.
      float maxFootDropCm = 0;
      float maxFootPenetrationCm = 0;
      size_t footFloorSamples = 0, footFloorMisses = 0;
      bool finite = true;
      };
    // Isolated physics experiment; requires OPENGOTHIC_MELEE_BENCH != 0.
    // Keeps animation/render reaction state unchanged and never applies damage.
    bool configureBench(BenchMode mode, BenchPelvis pelvis);
    void clearBench();
    BenchStats benchStats() const;
    size_t partCount() const;
    size_t boneCount() const;
    // Actual native mass in kilograms: speciesMass * absdet(object scale).
    float mass() const;
    const std::vector<size_t>& boneIndices() const;
    // Ancestors have no bodies; their shown frame is carried by a native child.
    const std::vector<size_t>& poseIndices() const;
    // Output is indexed by skeleton bone. Only boneIndices() are physical;
    // the pose pass keeps the animation-relative transforms of other bones.
    bool getPose(std::vector<Tempest::Matrix4x4>& worldBones) const;
    Tempest::Matrix4x4 pelvisTransform() const;
    Tempest::Matrix4x4 rootTransform() const;
    bool isSleeping() const;
    bool settled() const;
    bool onBack() const;
    void activate();
    // Runtime policy for this rig only; native sleeping is allowed by default.
    // Disabling it also awakens every part. The policy is not part of saved state.
    void setAllowSleeping(bool allow);
    // Total impulse, distributed by mass over the parts (newton seconds).
    void addImpulse(const Tempest::Vec3& impulse, Npc* culprit = nullptr);
    // A living body knocked off its feet: the trunk is thrown along the blow while the
    // feet stay, so it topples over them; the legs hold a moment longer than in death.
    void knockOff(const Tempest::Vec3& direction, float impulseNs, Npc* culprit = nullptr);
    State state() const;
    // Rejects incompatible, nonfinite or malformed states without changing bodies.
    bool restore(const State& state);
    // An explicit teleport defines new public bone-origin positions. Rebuilds
    // matching COM precision fields from the saved geometry, without adding bodies.
    static bool translateState(const Skeleton&, State&, const Tempest::Vec3& displacementCm);
    static bool relocateState(const Skeleton&, State&, const Tempest::Vec3& rootPositionCm);

    static float speciesMass(std::string_view skeletonName);

  private:
    // Native ragdoll parts have no CollisionBody wrapper. CollisionWorld keeps
    // their water support on the same fixed-step path through this small cache.
    // A living rig, including a temporary dynamic hit reaction, has no entries.
    struct BuoyancyPart final {
      uint32_t body = 0;
      float fraction = .9f;
      bool initialized = false, inWater = false, sleepDisabled = false, preserveSleep = false;
      float restTime = 0, waterY = 0;
      uint64_t nextSplashStep = 0;
      };
    struct Impl;
    Ragdoll(CollisionWorld& world, const Skeleton& skeleton, const HitZones& zones,
            const Tempest::Matrix4x4* worldPose, size_t numBones,
            const Tempest::Matrix4x4& objectMatrix, const Tempest::Vec3& initialVelocity,
            bool kinematic = false);
    void prepareKinematicStep(float deltaSeconds, float targetFraction = 1.f);
    void finishKinematicStep();
    size_t boneForBodyId(uint32_t id) const;
    std::vector<uint32_t> diagnosticBodyIds() const;
    Npc* noiseNpc() const;
    Npc* noiseCulprit() const;
    bool acceptNoise(uint64_t step);
    uint64_t noiseId() const;
    std::vector<BuoyancyPart>& buoyancyParts();
    const std::vector<BuoyancyPart>& buoyancyParts() const;
    void resetBuoyancy();
    void updateBuoyancyFractions();
    void detach();
    std::unique_ptr<Impl> impl;
    friend class CollisionWorld;
    friend class DynamicWorld;
  };
