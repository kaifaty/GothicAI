#pragma once

#include <Tempest/Matrix4x4>
#include <Tempest/Vec>
#include "asset/material.h"

#include <functional>
#include <atomic>
#include <memory>
#include <vector>
#include <string_view>

#include "physics/physics.h"
#include "physics/physicsnoise.h"
#include "physics/charactervirtualdiagnostics.h"
#include <Jolt/Physics/Body/BodyID.h>

class Item;
class Npc;
class Interactive;
class PhysicVbo;
class Skeleton;
class HitZones;
class Ragdoll;
class Cloth;
struct MeleeBladeSegment;
struct MeleeBodyCapsule;
struct MeleeBladeContact;
namespace zenkit { struct INpc; }
namespace JPH { class PhysicsSystem; }

class CollisionWorld final {
  public:
    CollisionWorld();
    ~CollisionWorld();
    CollisionWorld(const CollisionWorld&) = delete;

    static float         toMeters(float v);
    static JPH::Vec3     toMeters(const Tempest::Vec3& v);
    static Tempest::Vec3 toCentimeters(JPH::Vec3Arg v);

    class DynamicBody;
    class CollisionBody {
      public:
        virtual ~CollisionBody();
        CollisionBody(const CollisionBody&) = delete;

        void                setObjMatrix(const Tempest::Matrix4x4& matrix);
        void                restoreTransform(const Tempest::Matrix4x4& matrix, bool sleeping);
        void                setPosition(const Tempest::Vec3& position);
        void                setScale(const Tempest::Vec3& scale);
        void                setShape(JPH::ShapeRefC shape);
        void                setPayload(void* pointer, int type);
        Tempest::Vec3       position() const;
        Tempest::Matrix4x4  matrix() const;
        void                getAabb(JPH::Vec3& min, JPH::Vec3& max) const;
        float               mass() const; // kilograms; zero for non-dynamic bodies
        bool                isMovable() const; // native dynamic or kinematic, including doors/lifts
        Tempest::Vec3        velocity() const; // centimetres/second
        Tempest::Vec3        angularVelocity() const; // world radians/second
        bool                isSleeping() const;
        bool                keepSleepingItem(const Tempest::Vec3& positionCm) const;
        void                retainForItemWake();
        // World-space newton seconds. Rejects non-dynamic, zero or nonfinite input.
        bool                addImpulse(const Tempest::Vec3& impulseNs, Npc* culprit = nullptr,
                                       const Tempest::Vec3* pointCm = nullptr);
        bool                addImpulseAtPoint(const Tempest::Vec3& impulseNs, const Tempest::Vec3& pointCm,
                                              Npc* culprit = nullptr);
        void                setVelocity(const Tempest::Vec3& velocityCm, const Tempest::Vec3& angularRadians);
        void                setBuoyancyShape(JPH::ShapeRefC shape); // unpadded envelope; native collision stays unchanged
        // Target submerged-volume fraction, independent of native push mass.
        // Zero uses physical density; a target above one makes the body sink.
        void                setBuoyancyFraction(float fraction);
        void                setNoiseProfile(ItemMaterial material, asset::MaterialGroup surface, std::string_view what);
        // The latest engine-known cause survives the short noise attribution
        // window and NPC destruction, until a new impulse replaces it.
        void                setTraceCulprit(Npc* culprit);
        const std::string&  traceCulprit() const { return traceCause; }
        Tempest::Vec3       centerOfMass() const;
        bool                touchedByNpc() const { return npcContact.load(std::memory_order_relaxed); }

        int                 category() const { return categoryId; }
        int                 ptrType() const { return pointerType; }
        void*               payload() const { return pointer; }
        const JPH::Shape*    shape() const { return scaledShape.GetPtr(); }
        void                setLabel(std::string_view value) { debugLabel = value; }
        const std::string&  label() const { return debugLabel; }
        uint32_t            nativeId() const { return id.GetIndexAndSequenceNumber(); }

      private:
        CollisionBody(CollisionWorld& owner, JPH::ShapeRefC shape, int category);
        CollisionWorld*     owner = nullptr;
        JPH::BodyID         id;
        JPH::ShapeRefC      baseShape;
        JPH::ShapeRefC      scaledShape;
        JPH::ShapeRefC      buoyancyBaseShape, buoyancyShape;
        float               buoyancyFraction = 0.f;
        bool                waterInitialized = false, inWater = false, waterSleepDisabled = false, waterPreserveSleep = false;
        float               waterRestTime = 0, waterY = 0;
        uint64_t            nextSplashStep = 0;
        JPH::Vec3           scale = JPH::Vec3::sOne();
        const PhysicVbo*    mesh = nullptr;
        int                 categoryId = 0;
        int                 pointerType = 0;
        void*               pointer = nullptr;
        std::atomic<bool>   npcContact{false};
        ItemMaterial       noiseMaterial = MAT_WOOD;
        asset::MaterialGroup noiseSurface = asset::MaterialGroup::STONE;
        std::string         noiseWhat = "item";
        std::weak_ptr<zenkit::INpc> noiseCulprit;
        uint64_t            noiseCulpritUntilStep = 0;
        uint64_t            nextNoiseStep = 0;
        std::string         traceCause;
        std::string         debugLabel;
        Tempest::Matrix4x4  itemRestTransform;
        uint64_t            itemWakeUntilStep = 0;
        bool                itemRecovered = false;

      friend class CollisionWorld;
      friend class DynamicBody;
      };

    class DynamicBody final : public CollisionBody {
      private:
        DynamicBody(CollisionWorld& owner, JPH::ShapeRefC shape, float mass, int category);
        const float bodyMass;
      friend class CollisionWorld;
      };

    struct QueryResult {
      bool                 hasHit = false;
      float                fraction = 1.f;
      Tempest::Vec3        point = {};
      Tempest::Vec3        normal = {};
      CollisionBody*       body = nullptr;
      asset::MaterialGroup material = asset::MaterialGroup::UNDEFINED;
      const char*          sector = nullptr;
      Ragdoll*             ragdoll = nullptr;
      size_t               bone = size_t(-1);
      // Selected ray hit only; navigation resolves live native rigs without changing ragdoll/bone semantics.
      JPH::BodyID          nativeBodyId;
      };

    struct CharacterMoveResult {
      Tempest::Vec3 position = {}, velocity = {}, groundNormal = {}, groundVelocity = {};
      // Opposing, non-walkable world contact left unresolved by the stair pass.
      Tempest::Vec3 steepContactNormal = {};
      bool grounded = false;
      bool steep = false;
      bool steepContact = false;
      bool supportMoved = false;
      };

    // Transient, feet-based walking controller. Nothing in this object is saved.
    class VirtualCharacter final {
      public:
        ~VirtualCharacter();
        CharacterMoveResult move(const Tempest::Vec3& displacement, uint64_t dt,
                                 float stepHeight, float maxSlope, float mass, Npc* culprit,
                                 bool limitStairAdvance = false);
        CharacterMoveResult carry(uint64_t dt);
        void setPosition(const Tempest::Vec3& feet);
        void setDiagnosticsEnabled(bool enabled);
        bool diagnostics(CharacterVirtualDiagnostics& out) const;

      private:
        struct Impl;
        VirtualCharacter(CollisionWorld& owner, float radius, float height, const Tempest::Vec3& feet);
        std::unique_ptr<Impl> impl;
        friend class CollisionWorld;
      };

    std::unique_ptr<VirtualCharacter> createVirtualCharacter(float radius, float height, const Tempest::Vec3& feet);

    void tick(uint64_t dt);
    // Stage-0 cost probe. All entry points require OPENGOTHIC_MELEE_BENCH.
    void configureMeleeBench(int substeps);
    void updateMeleeBlade(const Npc* owner, const Tempest::Matrix4x4& hand, float lengthCm);
    void removeMeleeBlade(const Npc* owner);
    void measureMeleeBench(std::string path);
    void reportMeleeBench();
    void updateShadowBody(Npc* owner, const std::vector<MeleeBodyCapsule>& capsules);
    void removeShadowBody(const Npc* owner);
    // Pose time and the half-open active window use absolute game milliseconds.
    // forward: the attacker's facing; a wall met by a blade going back or up (the wind-up,
    // the follow-through) does not stop the swing. Zero: every wall stops it.
    void updateShadowBlade(const Npc* owner, uint64_t swingId,
                           const std::vector<MeleeBladeSegment>& segments, uint64_t timeMs,
                           double activeBeginMs, double activeEndMs, float walkableCos = .7f,
                           Tempest::Vec3 forward = {});
    std::vector<MeleeBladeContact> takeShadowBladeContacts(const Npc* owner);
    void removeShadowBlade(const Npc* owner);
    void flushShadowBlades();
    // Pure capsule query in centimetres; infinity means no contact along the cast.
    float meleeBladeAdvance(const std::vector<MeleeBladeSegment>& blades, const std::vector<MeleeBodyCapsule>& target,
                            const Tempest::Vec3& direction, float maxAdvanceCm) const;
    void accountMeleeReachTime(float milliseconds);
    void setWaterSurfaceCallback(std::function<bool(const Tempest::Vec3&,float,float&)> callback);
    void setBuoyancyEnabled(bool enabled);
    bool buoyancyEnabled() const;
    void setItemWakeEnabled(bool enabled);
    void setItemWakeCallback(std::function<size_t(const Tempest::Vec3&,const Tempest::Vec3&,size_t)> callback);
    struct WaterBodySnapshot {
      const CollisionBody* body = nullptr;
      const Ragdoll* ragdoll = nullptr;
      uint32_t bone = 0, nativeId = 0;
      float targetFraction = 0;
      Tempest::Matrix4x4 transform;
      Tempest::Vec3 worldBounds[2], collisionBounds[2], velocity;
      float mass = 0, density = 0, waterY = 0, submergedFraction = 0;
      bool sleeping = true, inWater = false;
      };
    std::vector<WaterBodySnapshot> waterBodySnapshot() const;
    void setBBox(JPH::Vec3Arg min, JPH::Vec3Arg max);
    void setItemHitCallback(std::function<void(Item&, asset::MaterialGroup, float impulse, float mass)> f);
    void setPhysicsNoiseEnabled(bool enabled);
    bool physicsNoiseEnabled() const;
    void setPhysicsNoiseCallback(std::function<void(const PhysicsNoise&)> callback);
    // Whole collision-pair normal momentum (Ns), deferred until native workers
    // finish. Excludes walking/living rig contact; noise retains solver impulse.
    void setBodyImpactCallback(std::function<void(CollisionBody&,float,const Tempest::Vec3&,std::string_view)> callback);
    void emitPhysicsNoise(const PhysicsNoise& noise);

    // BodyInterface updates the broad phase immediately when a body moves.
    void updateAabbs() const {}
    void touchAabbs() const {}

    bool hasCollision(const JPH::Shape* shape, const Tempest::Vec3& center, Tempest::Vec3& normal, Interactive*& vob) const;

    // Diagnostic: every contact of the body's own shape at its current pose,
    // including separated ones within marginCm. Depth is positive when penetrating.
    struct Overlap {
      CollisionBody*        body = nullptr;
      int                   category = 0;
      float                 depth = 0; // centimetres
      Tempest::Vec3         axis = {}; // unit direction that moves the other body out
      Tempest::Vec3         point1 = {}, point2 = {}; // world centimetres
      asset::MaterialGroup material = asset::MaterialGroup::UNDEFINED;
      const char*           sector = nullptr;
      };
    std::vector<Overlap> overlaps(const CollisionBody& body, float marginCm, uint32_t mask) const;

    // Bench-only narrow-phase snapshot at the native parts' current poses.
    // Unlike contact callbacks, this also includes sleeping rigs and supports.
    struct RagdollContact {
      size_t               bone = size_t(-1);
      uint32_t             partBodyId = uint32_t(-1), supportBodyId = uint32_t(-1);
      CollisionBody*       body = nullptr;
      int                  category = 0;
      bool                 sleeping = false;
      float                depth = 0; // centimetres; negative means separation
      Tempest::Vec3        axis, point1, point2; // axis moves support out of part
      float                sourceDistance = -1; // actual source triangle, centimetres
      Tempest::Vec3        sourcePoint;
      uint32_t             sourceTriangle = uint32_t(-1);
      };
    std::vector<RagdollContact> ragdollContacts(const Ragdoll& rig, float marginCm) const;

    std::unique_ptr<CollisionBody> addCollisionBody(JPH::ShapeRefC shape, const Tempest::Matrix4x4& matrix, float friction,
                                                   int category, bool movable = false, bool sensor = false, const PhysicVbo* mesh = nullptr);
    std::unique_ptr<DynamicBody> addDynamicBody(JPH::ShapeRefC shape, const Tempest::Matrix4x4& matrix, float friction, float mass,
                                               int category = 6, bool startSleeping = false);
    std::unique_ptr<Ragdoll> createRagdoll(const Skeleton& skeleton, const HitZones& zones,
                                         const Tempest::Matrix4x4* worldPose, size_t numBones,
                                         const Tempest::Matrix4x4& objectMatrix,
                                         const Tempest::Vec3& initialVelocity = {});
    std::unique_ptr<Ragdoll> createKinematicBody(const Skeleton& skeleton, const HitZones& zones,
                                               const Tempest::Matrix4x4* worldPose, size_t numBones,
                                               const Tempest::Matrix4x4& objectMatrix);
    size_t bodyCount() const;
    float stepMilliseconds() const;
    uint64_t rayCount() const;
    uint64_t simulationStep() const;

    struct StaticTriangle {
      Tempest::Vec3 vertices[3]; // world metres, including the native shape's scale
      bool water = false;
      };
    // Bench-only snapshot of static collision geometry, never render meshes.
    // Water is exported separately from solid surfaces for depth filtering.
    void staticTriangles(const std::function<void(const StaticTriangle&)>& output) const;

    // Masks use 1u << DynamicWorld::Category. Normals keep the mesh's winding.
    QueryResult rayCast(const Tempest::Vec3& from, const Tempest::Vec3& to, uint32_t mask,
                        bool backfaces = false, std::vector<QueryResult>* all = nullptr,
                        const std::vector<const CollisionBody*>* excluded = nullptr,
                        const Ragdoll* excludedRig = nullptr) const;
    // Eye-only native hit classification. A removed/reused body ID is rejected by its sequence number.
    int navigationBodyCategory(JPH::BodyID body) const;
    QueryResult raySweep(const Tempest::Vec3& from, const Tempest::Vec3& to, float radius, uint32_t mask,
                         bool backfaces = false) const;

  private:
    struct Impl;
    void addBody(CollisionBody& body, const Tempest::Matrix4x4& matrix, float friction,
                 bool movable, bool sensor, float mass, bool startSleeping = false);
    void removeBody(CollisionBody& body);
    struct BuoyancyBody;
    template<class F> void forEachBuoyancyBody(F&& callback) const;
    void restoreRagdollBuoyancy(Ragdoll& rig);
    void prepareBuoyancy(std::vector<PhysicsNoise>& splashes);
    void applyBuoyancyStep(float seconds);
    void finishBuoyancy(float seconds);
    void stepMeleeBlades(float seconds, float fraction);
    void stepShadowBlades(float fraction, bool bodiesLocked = false);
    void sampleMeleeBench();
    void wakeItems();
    void recoverItems();
    void recoverItem(CollisionBody& body, const Tempest::Vec3& failedPosition);
    void fillMaterial(QueryResult& result, const JPH::Shape& shape, JPH::SubShapeID subShape) const;
    void fillRagdoll(QueryResult& result, JPH::BodyID body) const;
    JPH::PhysicsSystem& physicsSystem();
    friend class ClothContractDriver;
    uint32_t registerRagdoll(Ragdoll& body);
    void unregisterRagdoll(Ragdoll& body);
    std::unique_ptr<Impl> impl;
    std::vector<CollisionBody*> bodies;
    std::vector<DynamicBody*> rigid;
    std::vector<Ragdoll*> ragdolls;
    uint32_t nextRagdollGroup = 1;
    std::function<void(Item&, asset::MaterialGroup, float, float)> hitItem;
    std::function<void(const PhysicsNoise&)> physicsNoise;
    std::function<void(CollisionBody&,float,const Tempest::Vec3&,std::string_view)> bodyImpact;
    std::function<bool(const Tempest::Vec3&,float,float&)> waterSurface;
    Cloth* cloth = nullptr;
    bool buoyancy = true;
    bool itemWake = true;
    std::function<size_t(const Tempest::Vec3&,const Tempest::Vec3&,size_t)> itemWakeCallback;
    size_t itemWakeCursor = 0;
    JPH::Vec3 bboxMin = JPH::Vec3::sZero();
    float accumulatedTime = 0;
    friend class Ragdoll;
    friend class Cloth;
  };
