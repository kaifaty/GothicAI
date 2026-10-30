#pragma once

#include "asset/worldmesh.h"
#include "physics/physicsnoise.h"
#include "physics/charactervirtualdiagnostics.h"
#include "physics/navmesh.h"
#include "physics/fracture.h"

#include <Tempest/Matrix4x4>
#include <cstdint>
#include <functional>
#include <memory>
#include <limits>
#include <vector>

class PhysicMeshShape;
class PhysicVbo;
class PackedMesh;
class Skeleton;
class Bounds;
class HitZones;
class Ragdoll;
class ProtoMesh;

class World;
class Bullet;
class Npc;
class Item;
class Interactive;
class DbgPainter;
class Serialize;

class CollisionWorld;
class Cloth;

// Query-only melee geometry. Positions and radii are world centimetres.
struct MeleeBladeSegment final {
  Tempest::Vec3 from, to, edgeNormal;
  float radiusCm = 2.f;
  };

struct MeleeBodyCapsule final {
  Tempest::Vec3 from, to;
  float radiusCm = 0;
  size_t bone = size_t(-1);
  uint8_t zone = 0;
  };

struct MeleeBladeContact final {
  Npc* npc = nullptr;
  size_t bone = size_t(-1);
  uint8_t zone = 0;
  Tempest::Vec3 point;
  Tempest::Vec3 normal;
  Tempest::Vec3 velocityCmS;
  asset::MaterialGroup material = asset::MaterialGroup::STONE;
  float bladeFraction = 0, speedCmS = 0, surfaceAngleDeg = 0;
  bool world = false;
  bool stopsSwing = false;
  bool windup = false;     // a wall met by a blade going back or up (wind-up, follow-through): passed
  uint64_t step = 0;
  };

class DynamicWorld final {
  private:
    struct BodyData;
    struct WorldBodies;
    struct BBoxShape;
    struct NpcBody;
    struct NpcBodyList;
    struct BulletsList;
    struct BBoxList;
    struct MovableVobs;
    struct ItemHulls;

  public:
    static constexpr float gravityMS   = 9.8f; // meters per second^2
    static constexpr float gravity     = gravityMS*100.f/(1000.f*1000.f); // centimeters per milliseconds^2
    static constexpr float bulletSpeed = 3; // centimeters per milliseconds
    static constexpr float spellSpeed  = 1; // centimeters per milliseconds
    static constexpr float characterItemRange = 300.f; // centimetres around the opt-in hero

    DynamicWorld(World *world, const asset::WorldMesh& mesh, const asset::WorldMesh* originalCloth = nullptr);
    DynamicWorld(const DynamicWorld&)=delete;
    ~DynamicWorld();

    enum Category {
      C_Null      = 1,
      C_Landscape = 2,
      C_Water     = 3,
      C_Object    = 4,
      C_Ghost     = 5,
      C_Item      = 6,
      C_Ragdoll   = 7,
      C_LivingBody= 8,
      C_ActiveBody= 9,
      C_MovableVob = 10,
      C_MovableQuery = 11,
      C_SmallVobQuery = 12,
      C_Fragment = 13,
      C_Cloth = 14,
      };

    enum PtrType {
      P_Null,
      P_Item,
      P_Interactive,
      P_Editor,
      P_MovableVob
      };

    enum MoveCode : uint8_t {
      MC_Fail,
      MC_OK,
      MC_Skip,
      MC_Partial,
      };

    struct CollisionTest {
      Tempest::Vec3 partial = {};
      Tempest::Vec3 normal  = {};
      bool          preFall = false;

      Interactive*  vob     = nullptr;
      Npc*          npc     = nullptr;
      float         npcCol  = 0;
      bool          landCol = false;
      };

    struct NpcItem {
      public:
        NpcItem()=default;
        NpcItem(DynamicWorld* owner, NpcBody* obj):owner(owner),obj(obj){}
        NpcItem(NpcItem&& it):owner(it.owner),obj(it.obj){it.obj=nullptr;}
        ~NpcItem();

        NpcItem& operator = (NpcItem&& it){
          std::swap(owner,it.owner);
          std::swap(obj,it.obj);
          return *this;
          }

        void  setPosition(const Tempest::Vec3& pos);
        const Tempest::Vec3& position() const;

        void  setRotation(float a);

        void  setScale(const Tempest::Vec3& sz);

        void  debugDraw(DbgPainter& p) const;

        void  setEnable(bool e);
        bool  isEnabled() const;
        void  setUserPointer(void* p);

        auto  center()  const -> Tempest::Vec3;
        float centerY() const;
        auto  centerAsym()  const -> Tempest::Vec3;
        float groundOffset() const;
        float radiusXZ() const;
        // Read-only dimensions of the landscape-query capsule, centimetres.
        float capsuleRadius() const;
        float capsuleHeight() const;

        bool  testMove(const Tempest::Vec3& to, CollisionTest& out);
        bool  testMove(const Tempest::Vec3& to, const Tempest::Vec3& from, CollisionTest& out);
        auto  tryMove (const Tempest::Vec3& to, CollisionTest& out) -> DynamicWorld::MoveCode;
        bool  hasFloorBelow(const Tempest::Vec3& feet) const;

        struct VirtualMoveResult {
          Tempest::Vec3 position = {}, velocity = {}, groundNormal = {}, groundVelocity = {};
          Tempest::Vec3 steepContactNormal = {};
          CollisionTest collision;
          float advancementRatio = 1.f;
          bool grounded = false;
          bool steep = false;
          bool steepContact = false;
          bool blocked = false;
          bool supportMoved = false;
          };
        // Walking only: displacement in cm, dt in ms, slope in radians, mass in kg.
        VirtualMoveResult moveVirtual(const Tempest::Vec3& displacement, uint64_t dt,
                                      float stepHeight, float maxSlope, float mass, Npc* culprit = nullptr,
                                      bool limitStairAdvance = false, bool bounceOnSteepContact = false);
        VirtualMoveResult carryVirtual(uint64_t dt, Npc* culprit = nullptr);
        void resetVirtual();
        void setVirtualDiagnosticsEnabled(bool enabled);
        bool virtualDiagnostics(CharacterVirtualDiagnostics& out) const;

        bool  hasCollision() const;

      private:
        DynamicWorld*       owner  = nullptr;
        NpcBody*            obj    = nullptr;

        auto  implTryMove    (const Tempest::Vec3& dp, const Tempest::Vec3& pos0, CollisionTest& out) -> DynamicWorld::MoveCode;
        void  implSetPosition(const Tempest::Vec3& pos);
        VirtualMoveResult implMoveVirtual(const Tempest::Vec3& displacement, uint64_t dt,
                                         float stepHeight, float maxSlope, float mass, Npc* culprit, bool carry,
                                         bool limitStairAdvance = false, bool bounceOnSteepContact = false);

      friend class DynamicWorld;
      };

    struct Item {
      public:
        Item()=default;
        Item(DynamicWorld* owner, BodyData* obj):owner(owner),obj(obj){}
        Item(Item&& it):owner(it.owner),obj(it.obj){ it.obj=nullptr; }
        ~Item();

        Item& operator = (Item&& it){
          std::swap(owner,it.owner);
          std::swap(obj,  it.obj);
          return *this;
          }

        void setObjMatrix(const Tempest::Matrix4x4& m);
        void setAttachmentMatrix(const Tempest::Matrix4x4& m);
        void setItem(::Item* it);
        void setInteractive(Interactive* it);
        void setPayloadPtr(void* ptr);
        bool isEmpty() const { return obj==nullptr; }
        float mass() const; // kilograms; zero for empty/static/kinematic handles
        Tempest::Matrix4x4 matrix() const;
        Tempest::Vec3 position() const;
        void restorePhysics(const Tempest::Matrix4x4& matrix, bool sleeping,
                            const Tempest::Vec3& velocity = {}, const Tempest::Vec3& angular = {});
        Tempest::Vec3 velocity() const; // centimetres/second; zero without dynamics
        Tempest::Vec3 angularVelocity() const; // world radians/second; zero without dynamics
        bool isSleeping() const;
        void retainForItemWake();
        bool touchedByNpc() const;
        // World-space newton seconds (kg * metres/second), with native wake.
        bool addImpulse(const Tempest::Vec3& impulseNs, Npc* culprit = nullptr);
        void setTransformCallback(std::function<void(const Tempest::Matrix4x4&)> callback);
        std::shared_ptr<const FractureMesh> fractureMesh() const;
        bool isBroken() const;
        // Preload GPU geometry with fractureMesh() before installing this.
        void setFractureCallback(std::function<void(bool,const std::vector<Tempest::Matrix4x4>&)> callback);

      private:
        DynamicWorld*       owner  = nullptr;
        BodyData*           obj    = nullptr;
      };

    struct RayLandResult {
      Tempest::Vec3           v={};
      Tempest::Vec3           n={};
      asset::MaterialGroup   mat    = asset::MaterialGroup::UNDEFINED;
      bool                    hasCol = false;
      float                   hitFraction = 0;

      const char*             sector = nullptr;
      Interactive*            vob    = nullptr;
      void*                   uptr   = nullptr;
      };

    struct RayWaterResult {
      float               wdepth  = 0.f;
      bool                hasCol = false;
      };

    struct RayCamResult {
      int                 waterCol = 0;
      };

    struct RayQueryResult : RayLandResult {
      Npc* npcHit = nullptr;
      };

    enum class NavigationHit : uint8_t { None, World, StaticVob, MovableVob, Npc, Water };
    struct NavigationRayResult {
      bool hasCol = false;
      Tempest::Vec3 point = {}, normal = {};
      NavigationHit kind = NavigationHit::None;
      };

    struct MovableVobPose {
      uint32_t zenId = uint32_t(-1);
      std::string visual;
      Tempest::Matrix4x4 origin, transform;
      bool broken = false;
      };

    struct BulletWorldHit {
      Tempest::Vec3          point = {}; // world centimetres
      Tempest::Vec3          normal = {}; // unit world normal, unchanged winding
      Tempest::Vec3          incoming = {}; // original flight segment, before any reflection
      asset::MaterialGroup material = asset::MaterialGroup::UNDEFINED;
      // Borrowed only for onWorldHit; game code owns the attachment lifetime.
      Ragdoll*              ragdoll = nullptr;
      size_t                bone = size_t(-1);
      Interactive*          vob = nullptr;
      std::shared_ptr<const MovableVobPose> movable;
      };

    struct BulletCallback {
      virtual ~BulletCallback()=default;
      virtual Npc* origin() const { return nullptr; }
      virtual void onStop(){}
      virtual void onMove(){}
      virtual void onCollide(asset::MaterialGroup matId){(void)matId;}
      virtual bool onCollide(Npc& other){(void)other; return true; }
      // Requests an attachment after a legacy world stop; never changes flight.
      virtual bool stopsAtWorldHit() const { return false; }
      virtual void onWorldHit(const BulletWorldHit& hit) { (void)hit; }
      virtual void onSpellWorldHit(const BulletWorldHit& hit) { onCollide(hit.material); }
      // an arrow aimed by hand: hits an npc where it is (its bones), not its box; from-to: this step
      virtual bool isPrecise() const { return false; }
      virtual bool onCollidePrecise(Npc& other, const Tempest::Vec3& from, const Tempest::Vec3& to){(void)from; (void)to; return onCollide(other); }
      };

    struct BulletBody final {
      public:
        BulletBody(DynamicWorld* wrld, BulletCallback* cb);
        BulletBody(BulletBody&&);

        void  setSpellId(int spl);
        void  setTargetRange(float tgRange);

        void  move(const Tempest::Vec3& to);
        void  setPosition  (const Tempest::Vec3& pos);
        void  setDirection (const Tempest::Vec3& dir);
        float pathLength() const;
        void  addPathLen(float v);

        float               speed()       const { return dirL; }
        float               targetRange() const { return tgRange; }
        Tempest::Vec3       position()    const { return pos; }
        Tempest::Vec3       direction()   const { return dir; }
        Tempest::Matrix4x4  matrix()      const;
        bool                isSpell()     const { return spl!=std::numeric_limits<int>::max(); }
        int                 spellId()     const { return spl; }

        void                addHit() { hitCnt++; }
        int                 hitCount()    const { return hitCnt; }

      private:
        DynamicWorld*       owner = nullptr;
        BulletCallback*     cb    = nullptr;

        Tempest::Vec3       pos={};
        Tempest::Vec3       lastPos={};

        Tempest::Vec3       dir={};
        float               dirL=0.f;
        float               totalL=0.f;
        int                 spl=std::numeric_limits<int>::max();
        int                 hitCnt  = 0;
        float               tgRange = 0;
        bool                worldStopped = false;

      friend class DynamicWorld;
      };

    struct BBoxCallback {
      BBoxCallback() = default;
      virtual ~BBoxCallback()=default;
      virtual void onCollide(BulletBody& other){(void)other;}
      };

    struct BBoxBody final {
      public:
        BBoxBody() = default;
        BBoxBody(DynamicWorld* wrld, BBoxCallback* cb, const zenkit::AxisAlignedBoundingBox& bbox);
        BBoxBody(DynamicWorld* wrld, BBoxCallback* cb, const Tempest::Vec3& pos, float R);
        BBoxBody(BBoxBody&& other);
        ~BBoxBody();

        BBoxBody& operator=(BBoxBody&& other);

      private:
        DynamicWorld*       owner = nullptr;
        BBoxCallback*       cb    = nullptr;

        BBoxShape*          shape = nullptr;

      friend class DynamicWorld;
      };

    RayLandResult  landRay      (const Tempest::Vec3& from, float maxDy=0) const;
    RayLandResult  landSweep    (const Tempest::Vec3& from, float R, float maxDy) const;
    RayWaterResult waterRay     (const Tempest::Vec3& from, float stepHeight) const;
    RayCamResult   cameraRay    (const Tempest::Vec3& from, const Tempest::Vec3& to) const;

    RayLandResult  ray          (const Tempest::Vec3& from, const Tempest::Vec3& to) const;
    // Bench camera only: retain world occlusion while looking past the selected
    // cloth object's own rigid attachments. Does not alter collision filters.
    RayLandResult  rayClothCamera(const Tempest::Vec3& from, const Tempest::Vec3& to,
                                 std::string_view ownVisual, const Tempest::Vec3* clothBounds,
                                 size_t& selfHits, std::string* blockedLabel = nullptr) const;
    RayLandResult  sweep        (const Tempest::Vec3& from, const Tempest::Vec3& to, float R) const;
    RayQueryResult rayNpc       (const Tempest::Vec3& from, const Tempest::Vec3& to, const Npc* except) const;
    // Physical Jolt hulls used by bodies, plus water and NPC rigs; no duplicate visual meshes.
    NavigationRayResult navigationRay(const Tempest::Vec3& from, const Tempest::Vec3& to, const Npc* except) const;
    // where an arrow aimed by hand would stop flying straight from-to: the first npc 'hit' takes (it says
    // how far along, t), else the ground; v: the point
    RayQueryResult rayArrow     (const Tempest::Vec3& from, const Tempest::Vec3& to,
                                 const std::function<bool(Npc&,const Tempest::Vec3&,const Tempest::Vec3&,float&)>& hit) const;
    float          soundOclusion(const Tempest::Vec3& from, const Tempest::Vec3& to) const;

    NpcItem        addGhostObj  (const Skeleton* src);
    Item           addStaticObj (const PhysicMeshShape *src, const Tempest::Matrix4x4& m, std::string_view label = {});
    Item           addMovableObj(const PhysicMeshShape *src, const Tempest::Matrix4x4& m, std::string_view label = {});
    Item           addDynamicObj(const Tempest::Matrix4x4& pos, const Bounds& bbox, asset::MaterialGroup mat);
    Item           addDynamicObj(const Tempest::Matrix4x4& pos, const Bounds& bbox, asset::MaterialGroup mat,
                                  const ProtoMesh* proto, std::string_view visual, bool hulls = true);
    struct ItemHull {
      JPH::ShapeRefC shape, originalShape;
      std::vector<Tempest::Vec3> vertices; // hull vertices in visual-local centimetres
      std::string fallbackReason;
      float volume = 0.f, boundsVolume = 0.f; // cubic metres, before object scale
      float extraPaddingCm = 0.f; // collision padding beyond the base centimetre
      };
    const ItemHull& itemHullShape(const ProtoMesh& proto, std::string_view visual);
    Item           addMovableVob(const ProtoMesh& mesh, std::string_view visual, const Tempest::Matrix4x4& matrix,
                                 float massMultiplier, bool blocksWalking, std::string_view className = {},
                                 bool legacyCollision = true, uint32_t zenId = uint32_t(-1));
    void           setMovableVobsEnabled(bool enabled);
    void           setDestructionEnabled(bool enabled);
    void           setBuoyancyEnabled(bool enabled);
    void           setItemWakeEnabled(bool enabled);
    bool           buoyancyEnabled() const;
    bool           destructionEnabled() const;
    Cloth&         cloth();
    const Cloth&   cloth() const;
    void           setClothEnabled(bool enabled);
    bool           clothEnabled() const;
    // Called after the complete authored vob tree has installed its collisions.
    void           finalizeMovableVobs();
    double         movableVobValidationMilliseconds() const;
    bool           movableVobsEnabled() const;
    void           setPhysicsNoiseEnabled(bool enabled);
    bool           physicsNoiseEnabled() const;
    void           setPhysicsNoiseCallback(std::function<void(const PhysicsNoise&)> callback);
    void           saveMovableVobs(Serialize& fout) const;
    void           loadMovableVobs(Serialize& fin);
    // Absolute game minutes. Trace creation is recorded at the physics tick,
    // independently of which NPC happens to inspect the body.
    void           setTraceTime(int64_t minutes);
    // Hero position in world centimetres, used only to retire sleeping debris.
    void           setFragmentObserver(const Tempest::Vec3& position);
    bool           setMovableVobTransform(uint32_t zenId, const Tempest::Matrix4x4& matrix, Npc* culprit = nullptr);
    std::shared_ptr<const MovableVobPose> movableVobPose(uint32_t zenId) const;
    std::shared_ptr<const MovableVobPose> rayMovableVob(const Tempest::Vec3& from, const Tempest::Vec3& to) const;
    // One animation's optimal hit frame: each first-contact body takes damage once.
    size_t         meleeMovableVobs(const Tempest::Vec3& origin, const Tempest::Vec3& forward,
                                    float rangeCm, float weaponDamage, float strength, FractureRule::Melee kind,
                                    Npc* attacker = nullptr);
    struct MovableVobSnapshot {
      uintptr_t          id = 0;
      uint32_t           zenId = uint32_t(-1);
      std::string        visual, className, disposition;
      bool               blocksWalking = false, legacyCollision = true, enabled = false, sleeping = true, touchedByNpc = false;
      float              mass = 0;
      float              hullVolume = 0, density = 0, massMultiplier = 1;
      asset::MaterialGroup material = asset::MaterialGroup::UNDEFINED;
      bool               destructible = false, broken = false;
      float              health = 0, maxHealth = 0, impactThreshold = 0;
      Tempest::Matrix4x4 transform, origin;
      Tempest::Vec3      velocity;
      Tempest::Vec3      localBounds[2];
      bool               trace = false, tilted = false;
      // Moved-prop appearance or, for broken props, the moment of destruction.
      int64_t            traceTime = -1;
      // Engine/save truth only; never serialize this field into npc_view.
      std::string        traceCulprit;
      };
    std::vector<MovableVobSnapshot> movableVobSnapshot() const;
    struct FragmentSnapshot {
      uintptr_t parentId = 0;
      uint32_t zenId = uint32_t(-1);
      size_t piece = 0;
      bool sleeping = true;
      Tempest::Matrix4x4 transform;
      Tempest::Vec3 velocity, angularVelocity;
      bool permanent = false;
      int64_t asleepSince = -1;
      };
    std::vector<FragmentSnapshot> fragmentSnapshot() const;
    struct WaterBodySnapshot {
      uintptr_t id = 0, parentId = 0;
      const Ragdoll* ragdoll = nullptr;
      const ::Item* item = nullptr;
      uint32_t bone = 0;
      float targetFraction = 0;
      uint32_t zenId = uint32_t(-1);
      size_t piece = size_t(-1);
      int category = C_Null;
      std::string visual;
      Tempest::Matrix4x4 transform;
      Tempest::Vec3 worldBounds[2], collisionBounds[2], velocity;
      float mass = 0, density = 0, waterY = 0, submergedFraction = 0;
      bool sleeping = true, inWater = false;
      };
    std::vector<WaterBodySnapshot> waterBodySnapshot() const;
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
    float meleeBladeAdvance(const std::vector<MeleeBladeSegment>& blades, const std::vector<MeleeBodyCapsule>& target,
                            const Tempest::Vec3& direction, float maxAdvanceCm) const;
    void accountMeleeReachTime(float milliseconds);
    struct RagdollSupport {
      size_t        bone = 0;
      uint32_t      partBodyId = uint32_t(-1), supportBodyId = uint32_t(-1);
      int           category = 0;
      bool          sleeping = false;
      float         depth = 0;
      Tempest::Vec3 axis, point1, point2;
      uintptr_t     parentId = 0;
      uint32_t      zenId = uint32_t(-1);
      size_t        piece = size_t(-1);
      std::string   visual, kind;
      bool          broken = false, visible = true;
      float         visibleDistance = -1;
      Tempest::Vec3 visiblePoint;
      uint32_t      sourceTriangle = uint32_t(-1);
      };
    // Native contacts, including sleeping parts, annotated with exact source
    // faces to distinguish real visible support from a convex hull's cavity.
    std::vector<RagdollSupport> ragdollSupports(const Ragdoll& ragdoll, float marginCm) const;
    // Diagnostic: what each movable hull touches or penetrates at its origin pose.
    struct MovableVobContact {
      std::string   other;      // visual of the other body, "landscape" for the world mesh
      uint32_t      otherZenId = uint32_t(-1);
      int           category = 0;
      bool          interactive = false;
      bool          below = false; // contacts under the hull: its support
      int           side = 0;      // 0 below, 1 above, 2..5 = +x,-x,+z,-z (direction into the other body)
      Tempest::Vec3 otherMin, otherMax; // world AABB, centimetres
      float         depth = 0;  // deepest contact, centimetres; positive = penetration
      size_t        count = 0;  // contact pairs (triangles)
      Tempest::Vec3 axis;       // axis of the deepest contact, moves the other body out
      Tempest::Vec3 point;      // deepest contact on the movable hull
      float         pointYMin = 0, pointYMax = 0; // contact heights on the hull
      std::string   material, sector;
      };
    struct MovableVobContacts {
      uint32_t      zenId = uint32_t(-1);
      std::string   visual, className;
      std::string   disposition;
      bool          enabled = false;
      float         mass = 0;
      Tempest::Vec3 position, hullMin, hullMax; // world centimetres
      std::vector<MovableVobContact> contacts;
      };
    std::vector<MovableVobContacts> movableVobContacts(float marginCm) const;
    // Diagnostic: wakes every movable body at rest, without any impulse.
    size_t         wakeMovableVobs();
    // callback returns a world-space impulse in newton seconds; target is the
    // centre of mass in world centimetres. Geometry occludes the blast, with the
    // target's own mesh excluded from the visibility query.
    size_t applyMovableVobImpulse(const Tempest::Vec3& center, float radius,
                                 const std::function<Tempest::Vec3(const Tempest::Vec3&,float)>& impulse,
                                 Npc* culprit = nullptr, const Tempest::Vec3* pointCm = nullptr);
    std::unique_ptr<Ragdoll> createRagdoll(const Skeleton& skeleton, const HitZones& zones,
                                          const Tempest::Matrix4x4* worldPose, size_t numBones,
                                          const Tempest::Matrix4x4& objectMatrix,
                                          const Tempest::Vec3& initialVelocity = {});
    std::unique_ptr<Ragdoll> createKinematicBody(const Skeleton& skeleton, const HitZones& zones,
                                               const Tempest::Matrix4x4* worldPose, size_t numBones,
                                               const Tempest::Matrix4x4& objectMatrix);

    BulletBody*    bulletObj(BulletCallback* cb);
    BBoxBody       bboxObj(BBoxCallback* cb, const zenkit::AxisAlignedBoundingBox& bbox);
    BBoxBody       bboxObj(BBoxCallback* cb, const Tempest::Vec3& pos, float R);

    void           tick(uint64_t dt);

    struct PhysicsStats {
      float    stepMilliseconds = 0;
      size_t   bodies = 0;
      uint64_t rays = 0;
      };
    PhysicsStats physicsStats() const;
    uint64_t simulationStep() const;

    // Called explicitly by the runner; ordinary worlds never build a navmesh.
    NavMesh::Geometry navigationGeometry() const;

    void           deleteObj(BulletBody* obj);

    static float   materialFriction(asset::MaterialGroup mat);
    static float   materialDensity (asset::MaterialGroup mat);

    static float   rayBox(const Tempest::Vec3& orig, const Tempest::Vec3& dir, const float TMax,
                          const Tempest::Matrix4x4& obj, const Tempest::Vec3& min, const Tempest::Vec3& max,
                          const float padd = 0.f);
    static float   rayBox(const Tempest::Vec3& orig, const Tempest::Vec3& dir, const float TMax,
                          const Tempest::Vec3& min, const Tempest::Vec3& max,
                          const float padd = 0.f);

    std::string_view validateSectorName(std::string_view name) const;

  private:
    friend class ClothBulletBenchDriver;
    void           moveBullet(BulletBody& b, const Tempest::Vec3& dir, uint64_t dt);
    RayWaterResult implWaterRay(const Tempest::Vec3& from, const Tempest::Vec3& to, float stepHeight,
                                 uint32_t solidMask = 0) const;
    bool           hasCollision(const NpcItem &it, CollisionTest& out);
    void           prepareMovableVobShape(BodyData& body);
    void           rebuildMovableVob(BodyData& body);
    void           validateMovableVobs();
    void           syncMovableVob(BodyData& body);
    void           damageMovableVob(BodyData& body, float impulse, const Tempest::Vec3& point, bool arrow = false,
                                    std::string_view impactCulprit = {});
    void           breakMovableVob(BodyData& body);
    void           syncFragments(BodyData& body);
    void           cleanupFragments(BodyData& body);
    void           saveDestructionVobs(Serialize& fout) const;
    void           loadDestructionVobs(Serialize& fin);

    std::unique_ptr<CollisionWorld>    world;
    std::unique_ptr<Cloth>             clothWorld;
    World*                            ownerWorld = nullptr;

    std::vector<std::string>           sectors;

    std::unique_ptr<PhysicVbo>         landMesh;
    std::unique_ptr<PhysicVbo>         waterMesh;
    std::unique_ptr<PhysicVbo>         originalClothMesh;
    std::unique_ptr<WorldBodies>       bodies;

    std::unique_ptr<NpcBodyList>       npcList;
    std::unique_ptr<BulletsList>       bulletList;
    std::unique_ptr<BBoxList>          bboxList;
    std::unique_ptr<MovableVobs>       movableVobs;
    std::unique_ptr<ItemHulls>         itemHulls;
    int64_t                           traceTime = -1;
    Tempest::Vec3                     fragmentObserver;
    bool                              hasFragmentObserver = false;

    static const float                 worldHeight;
  };
