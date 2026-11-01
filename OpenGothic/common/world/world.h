#pragma once

#include <Tempest/VertexBuffer>
#include <Tempest/IndexBuffer>
#include <Tempest/Matrix4x4>
#include <string>
#include <functional>
#include <array>

#include "worlddata.h"

#include "graphics/worldview.h"
#include "graphics/lightgroup.h"
#include "graphics/meshobjects.h"
#include "game/gamescript.h"
#include "physics/dynamicworld.h"
#include "worldobjects.h"
#include "worldsound.h"
#include "waypoint.h"
#include "waymatrix.h"
#include "portalrooms.h"
#include "chunkresidency.h"
#include "bodytraces.h"

class GameSession;
class Focus;
class WayMatrix;
class VisualFx;
class GlobalEffects;
class ParticleFx;
class Interactive;
class VersionInfo;
class GlobalFx;
class ClothVisual;
struct PhysicsNoise;
struct HeroDeed;

class World final {
  public:
    World()=delete;
    World(const World&)=delete;
    World(GameSession& game, std::string_view file, bool startup, std::function<void(int)> loadProgress);
    ~World();

    void                 createPlayer(std::string_view cls);
    void                 insertPlayer(std::unique_ptr<Npc>&& npc, std::string_view waypoint);
    void                 setPlayer(Npc* npc);
    void                 postInit();
    std::string_view     name() const { return wname; }
    double               loadMilliseconds() const { return worldLoadMs; }
    const std::vector<WorldObjects::PlacedItem>& placedItemCensus() const { return wobj.placedItemCensus(); }
    double               placedItemValidationMilliseconds() const { return wobj.placedItemValidationMilliseconds(); }
    size_t               placedItemsCreatedThisFrame() const { return wobj.placedItemsCreatedThisFrame(); }
    const ChunkTable&    chunkTable() const { return chunks; }
    ChunkResidency*      chunkResidency() const { return residency.get(); }
    void                 updateLandscape(const Camera& camera);
    bool                 landscapeReady() const { return !wview || wview->landscapeReady(); }

    void                 load(Serialize& fin );
    void                 save(Serialize& fout);

    uint32_t             npcId(const Npc* ptr) const;
    Npc*                 npcById(uint32_t id);
    uint32_t             npcCount() const;
    Npc*                 npcForRagdoll(const Ragdoll* body);
    bool                 stickArrow(MeshObjects::Mesh&& arrow, const Tempest::Matrix4x4& worldMatrix,
                                    std::string_view visual = {},
                                    std::shared_ptr<const DynamicWorld::MovableVobPose> movable = {},
                                    const Npc* culprit = nullptr, const Tempest::Vec3* sightPoint = nullptr,
                                    std::string_view surface = "unknown");
    size_t               stuckArrowCount() const { return stuckArrows.size(); }
    bool                 stuckArrowTransform(size_t index, Tempest::Matrix4x4& out) const;
    bool                 stuckArrowAttachment(size_t index, uint32_t& zenId, Tempest::Matrix4x4& local) const;
    struct PhysicsTrace {
      std::string id, kind, name;
      Tempest::Vec3 position;
      std::array<Tempest::Vec3,6> sightPoints;
      int64_t appearedAt = -1; // game minutes; -1 for traces from legacy saves
      std::string culprit; // engine truth: never serialize this field to the AI bridge
      std::string npc;     // a body: the dead one's instance symbol, as npc_view's look names it
      };
    // `viewer` (with `range`, cm): body rows farther from it are not built; the rows that are
    // built do not change. Without it, every trace of the world.
    std::vector<PhysicsTrace> physicsTraces(const Tempest::Vec3* viewer = nullptr, float range = 0) const;

    // Bodies (physics stage 8, step 4b; world/bodytraces.h holds the rules).
    int64_t              gameMinute() const;                  // minutes since the first day began
    uint32_t             allocBodyTraceId() { return nextBodyTraceId++; }
    std::string          bodyTraceId(const Npc& body) const;  // "<world>:body:<n>"
    std::array<Tempest::Vec3,6> bodySightPoints(const Npc& body) const;
    bool                 heroSeesBody(const Npc& hero, const Npc& body) const;
    bool                 isStoryNpcType(int32_t type) const;
    BodyTraces::Body     bodyRule(const Npc& body, const Npc* hero) const; // all but heroSees

    uint32_t             mobsiId(const Interactive* ptr) const;
    Interactive*         mobsiById(uint32_t id);

    uint32_t             itmId(const void* ptr) const;
    Item*                itmById(uint32_t id);

    const WayPoint*      findPoint(std::string_view name, bool inexact=true) const;
    const WayPoint*      findWayPoint(std::string_view name) const;
    void                 forEachWayPoint(const std::function<void(const WayPoint&)>& f) const;
    const WayPoint*      findWayPoint(const Tempest::Vec3& pos) const;
    const WayPoint*      findWayPoint(const Tempest::Vec3& pos, const std::function<bool(const WayPoint&)>& f) const;
    const WayPoint*      findWayPoint(const Tempest::Vec3& pos, std::string_view name) const;

    const WayPoint*      findFreePoint(const Npc& pos,           std::string_view name) const;
    const WayPoint*      findFreePoint(const Tempest::Vec3& pos, std::string_view name) const;

    const WayPoint*      findNextFreePoint(const Npc& pos, std::string_view name) const;
    const WayPoint*      findNextWayPoint(const Npc &npc) const;
    const WayPoint*      findNextPoint(const WayPoint& pos) const;

    const WayPoint&      startPoint() const;
    const WayPoint&      deadPoint() const;

    void                 detectNpcNear(std::function<void(Npc&)> f);
    void                 detectNpc (const Tempest::Vec3& p, const float r, const std::function<void(Npc&)>& f);
    void                 detectItem(const Tempest::Vec3& p, const float r, const std::function<void(Item&)>& f);

    WayPath              wayTo(const Npc& pos,const WayPoint& end) const;
    WayPath              wayTo(const WayPoint& begin, const WayPoint& end) const;

    WorldView*           view()     const { return wview.get();    }
    WorldSound*          sound()          { return &wsound;        }
    DynamicWorld*        physic()   const { return wdynamic.get(); }
    GlobalEffects*       globalFx() const { return globFx.get();   }

    GameScript&          script()   const;
    GameSession&         gameSession() const { return game; }
    auto                 version()  const -> const VersionInfo&;

    void                 assignRoomToGuild(std::string_view room, int32_t guildId);
    int32_t              guildOfRoom(const Tempest::Vec3& pos);
    int32_t              guildOfRoom(std::string_view portalName);

    void                 runEffect(Effect&& e);
    void                 stopEffect(const VisualFx& vfx);

    GlobalFx             addGlobalEffect(std::string_view what, uint64_t len, const std::string* argv, size_t argc);
    MeshObjects::Mesh    addView(std::string_view visual) const;
    MeshObjects::Mesh    addView(std::string_view visual, int32_t headTex, int32_t teetTex, int32_t bodyColor) const;
    MeshObjects::Mesh    addView(const zenkit::IItem& itm);
    MeshObjects::Mesh    addView(const ProtoMesh* visual);
    MeshObjects::Mesh    addAtachView (const ProtoMesh::Attach& visual, const int32_t version);
    MeshObjects::Mesh    addStaticView(const ProtoMesh* visual, bool staticDraw);
    MeshObjects::Mesh    addStaticView(std::string_view visual);
    MeshObjects::Mesh    addDecalView (const asset::VisualDecal& decal);
    LightGroup::Light    addLight(const asset::VLight& vob);
    LightGroup::Light    addLight(std::string_view preset);

    void                 updateAnimation(uint64_t dt);
    void                 resetPositionToTA();

    auto                 takeHero() -> std::unique_ptr<Npc>;
    Npc*                 player() const { return npcPlayer; }
    Npc*                 findNpcByInstance(size_t instance, size_t n = 0);
    Item*                findItemByInstance(size_t instance, size_t n = 0);
    std::string_view     roomAt(const Tempest::Vec3& arr);
    bool                 isIndoor(const Tempest::Vec3& point) const;

    void                 scaleTime(uint64_t& dt);
    void                 tick(uint64_t dt);
    uint64_t             tickCount() const;
    void                 setDayTime(int32_t h,int32_t min);
    gtime                time() const;

    Focus                validateFocus(const Focus& def);
    Focus                findFocus(const Npc& pl, const Focus &def);
    Focus                findFocus(const Focus& def);
    bool                 testFocusNpc(Npc *def);

    void                 triggerOnStart(bool firstTime);
    void                 triggerEvent(const TriggerEvent& e);
    void                 detectMover(const std::function<void(MoveTrigger&)>& f);
    void                 triggerChangeWorld(std::string_view world, std::string_view wayPoint);
    void                 execTriggerEvent(const TriggerEvent& e);
    void                 enableDefTrigger(AbstractTrigger& t);
    void                 enableTicks (AbstractTrigger& t);
    void                 disableTicks(AbstractTrigger& t);
    void                 setCurrentCs(CsCamera* cs);
    CsCamera*            currentCs() const;
    bool                 isCutsceneLock() const;
    void                 enableCollizionZone (CollisionZone& z);
    void                 disableCollizionZone(CollisionZone& z);

    Interactive*         availableMob(const Npc &pl, std::string_view name);
    Interactive*         findInteractive(const Npc& pl);
    void                 setMobRoutine(gtime time, std::string_view scheme, int32_t state);

    void                 marchInteractives(DbgPainter& p) const;
    void                 marchPoints      (DbgPainter& p) const;
    void                 marchCsCameras   (DbgPainter& p) const;
    void                 drawVobBoxNpcNear(DbgPainter& p) const;
    void                 drawMeleeShadow  (DbgPainter& p) const;

    AiOuputPipe*         openDlgOuput(Npc &player, Npc &npc);
    void                 aiOutputSound(Npc &player, std::string_view msg);
    bool                 isInDialog() const;

    bool                 isTargeted (Npc& npc);
    Npc*                 addNpc     (std::string_view name, std::string_view     at);
    Npc*                 addNpc     (size_t npcInstance,    std::string_view     at);
    Npc*                 addNpc     (size_t npcInstance,    const Tempest::Vec3& at);
    void                 removeNpc  (Npc& npc);

    Item*                addItem    (size_t itemInstance,   std::string_view     at);
    Item*                addItem    (const asset::VItem& vob);
    Item*                addItem    (size_t itemInstance, const Tempest::Vec3&      pos);
    Item*                addItemDyn (size_t itemInstance, const Tempest::Matrix4x4& pos, size_t owner);
    auto                 takeItem   (Item& it) -> std::unique_ptr<Item>;
    void                 removeItem (Item& it);
    size_t               hasItems(std::string_view tag, size_t itemCls);

    Bullet&              shootBullet(const Item &itmId, const Npc& npc, const Npc* target, const Interactive* inter);
    // an arrow aimed by hand: from there, that way (a unit vector), that fast (cm/ms)
    Bullet&              shootArrow(const Item& itm, const Tempest::Vec3& pos, const Tempest::Vec3& dir, float speed);
    // the velocity an arrow, a bolt leaves with to fall onto the target
    static Tempest::Vec3 bulletVelocity(const Tempest::Vec3& from, const Tempest::Vec3& to);
    Bullet&              shootSpell(const Item &itm, const Npc &npc, const Npc *target);
    void                 applySpellImpact(int32_t spellId, const Tempest::Vec3& point, Npc* caster, Npc* target);
    void                 startSpellField(int32_t spellId, const Tempest::Vec3& point, Npc* caster, Npc* target);
    size_t               spellFieldCount() const { return spellFields.size(); }

    void                 sendPassivePerc  (Npc& self, Npc& other, int32_t perc);
    void                 sendPassivePerc  (Npc& self, Npc& other, Npc& victim, int32_t perc, std::shared_ptr<HeroDeed> deed = {});
    void                 sendPassivePerc  (Npc& self, Npc& other, Item& item, int32_t perc, std::shared_ptr<HeroDeed> deed = {});
    void                 sendPassivePerc  (Npc& self, Npc& other, Npc& victim, Item& item, int32_t perc);

    void                 sendImmediatePerc(Npc& self, Npc& other, Npc& victim, int32_t perc);
    void                 sendImmediatePerc(Npc& self, Npc& other, Npc& victim, Item& item, int32_t perc);

    bool                 isInSfxRange(const Tempest::Vec3& pos) const;
    bool                 isInPfxRange(const Tempest::Vec3& pos) const;

    void                 addDlgSound(std::string_view s, const Tempest::Vec3& pos, float range, uint64_t &timeLen,
                                     std::shared_ptr<LipSync::Track>* voice = nullptr);

    Sound                addWeaponHitEffect(Npc&         src, const Bullet* srcArrow, Npc&  reciver);
    Sound                addWeaponBlkEffect(ItemMaterial src, ItemMaterial          reciver, const Tempest::Matrix4x4& pos);
    Sound                addLandHitEffect  (ItemMaterial src, asset::MaterialGroup reciver, const Tempest::Matrix4x4& pos);
    void                 emitPhysicsNoise(const PhysicsNoise& noise);

    void                 addTrigger    (AbstractTrigger* trigger);
    void                 addInteractive(Interactive* inter);
    void                 addStartPoint (const Tempest::Vec3& pos, const Tempest::Vec3& dir, std::string_view name);
    void                 addFreePoint  (const Tempest::Vec3& pos, const Tempest::Vec3& dir, std::string_view name);
    void                 addSound      (const asset::VirtualObject& vob);

    void                 invalidateVobIndex();
    std::vector<Vob::AttachmentSnapshot> vobAttachmentSnapshot(uint32_t zenId) const { return wobj.vobAttachmentSnapshot(zenId); }

  private:
    friend struct WorldDataContract;
    friend struct WayHeightsContract;
    double               worldLoadMs = 0;
    bool                 textureLoadingPending = true;
    const zenkit::IFocus& searchPolicy(const Npc& pl, TargetCollect& collAlgo, TargetType& collType, WorldObjects::SearchFlg& opt) const;
    std::string                           wname;
    GameSession&                          game;

    std::unique_ptr<WayMatrix>            wmatrix;
    ChunkTable                            chunks;
    std::unique_ptr<ChunkResidency>        residency;

    struct BspSector final {
      int32_t guild=GIL_NONE;
      };

    struct {
      std::vector<asset::BspNode>        nodes;
      std::vector<asset::BspSector>      sectors;
      std::vector<std::uint64_t>          leaf_node_indices;
      std::vector<BspSector>              sectorsData;
      } bsp;
    PortalRooms::Volumes                  windRooms;

    Npc*                                  npcPlayer=nullptr;

    std::unique_ptr<DynamicWorld>         wdynamic;
    std::unique_ptr<WorldView>            wview;
    std::vector<std::unique_ptr<ClothVisual>> clothSurfaces;
    mutable MeshObjects                  cpuObjects;
    std::unique_ptr<GlobalEffects>        globFx;
    WorldSound                            wsound;
    WorldObjects                          wobj;
    struct StuckArrow {
      MeshObjects::Mesh mesh;
      std::string visual;
      Tempest::Matrix4x4 local;
      std::weak_ptr<const DynamicWorld::MovableVobPose> movable;
      bool attached = false;
      uint64_t traceId = 0;
      int64_t appearedAt = -1;
      std::string culprit, surface = "unknown";
      Tempest::Vec3 sightPoint;
      };
    std::vector<StuckArrow>               stuckArrows;
    uint64_t                             nextArrowTraceId = 1;
    uint32_t                             nextBodyTraceId  = 1;
    int64_t                              bodySweepMinute  = -1;
    mutable std::array<int32_t,3>        storyNpcTypes    = BodyTraces::StoryTypes;
    mutable bool                         storyNpcTypesRead = false;
    void                                 sweepBodies();
    void                                 saveBodyTraces(Serialize& fout) const;
    void                                 loadBodyTraces(Serialize& fin);
    void                                 saveStuckArrows(Serialize& fout) const;
    void                                 loadStuckArrows(Serialize& fin);
    void                                 updateStuckArrows();
    struct SpellField {
      Tempest::Vec3 center;
      Npc* caster = nullptr; // identities only; never dereferenced by a field
      Npc* target = nullptr;
      uint64_t until = 0;
      int32_t spellId = 0;
      };
    std::vector<SpellField>               spellFields;
    std::unique_ptr<Npc>                  lvlInspector;

    auto         roomAt(const asset::BspNode &node) -> std::string_view;
    auto         portalAt(std::string_view tag) -> BspSector*;

    void         initScripts(bool firstTime);
    void         tickSpellFields(uint64_t dt);

    Sound        addHitEffect(std::string_view src, std::string_view reciver, std::string_view scheme, const Tempest::Matrix4x4& pos);
  };
