#pragma once

#include "drawbuckets.h"
#include "drawclusters.h"
#include "drawcommands.h"
#include "instancestorage.h"

#include <unordered_set>
#include <map>
#include <mutex>

class SceneGlobals;
class Camera;
class RtScene;
class Landscape;
class Sky;
class AnimMesh;
class ChunkResidency;

class VisualObjects final {
  public:
    struct MorphSample {
      uint32_t frame0 = 0, frame1 = 0;
      float alpha = 0, intensity = 0;
      };

    class Item final {
      public:
      Item()=default;
      Item(VisualObjects& owner,size_t id)
          :owner(&owner),id(id){}
      Item(Item&& obj):owner(obj.owner),id(obj.id){
        obj.owner=nullptr;
        obj.id   =0;
        }
      Item& operator=(Item&& obj) {
        std::swap(obj.owner,owner);
        std::swap(obj.id,   id);
        return *this;
        }
      ~Item() {
        if(owner)
          owner->free(this->id);
        }

      bool     isEmpty() const { return owner==nullptr; }

      void     setObjMatrix(const Tempest::Matrix4x4& mt);
      void     setObjMatrix(const Tempest::Matrix4x4& mt, const Tempest::Vec3& texturePosition);
      void     setTexturePosition(const Tempest::Vec3& position);
      void     setTextureOwner(const Tempest::Vec3& position, bool spatial);
      void     setTextureCategory(Resources::TextureCategory category);
      void     setAsGhost  (bool g);
      void     setFatness  (float f);
      void     setWind     (asset::AnimationType m, float intensity);
      void     setVisible  (bool visible);
      // Two vec4 entries (local position, local normal) per packed vertex.
      void     setDeformation(const std::vector<Tempest::Vec4>& vertices);
      void     startMMAnim (std::string_view anim, float intensity, uint64_t timeUntil, bool freeOnly = false);
      void     setMMShape  (std::string_view anim, uint32_t frame0, uint32_t frame1, float alpha, float intensity);
      float    morphWeight (std::string_view anim);  // as shown now: intensity and fade
      MorphSample morphSample(std::string_view anim, uint64_t tick) const;

      const Material&    material() const;
      const Bounds&      bounds()   const;
      Tempest::Matrix4x4 position() const;
      const StaticMesh*  mesh()     const;
      std::pair<uint32_t,uint32_t> meshSlice() const;

      private:
      VisualObjects* owner = nullptr;
      size_t         id    = 0;
      };

    VisualObjects(const SceneGlobals& globals, const std::pair<Tempest::Vec3, Tempest::Vec3>& bbox);
    ~VisualObjects();

    Item   get(const StaticMesh& mesh, const Material& mat, size_t iboOffset, size_t iboLength, bool staticDraw);
    Item   get(const AnimMesh& mesh,   const Material& mat, size_t iboOff, size_t iboLen, const InstanceStorage::Id& anim);
    Item   get(const StaticMesh& mesh, const Material& mat, size_t iboOff, size_t iboLen, const PackedMesh::Cluster* cluster, DrawCommands::Type type,
               const Tempest::Vec3* texturePosition = nullptr);
    // All Items referencing the mesh must have been released first.
    void   invalidate(const StaticMesh& mesh);
    void   setTextureResidency(ChunkResidency* residency);
    bool   textureChunkActive(const Tempest::Vec3& position) const;
    ChunkResidency* textureResidencyManager() const;

    InstanceStorage::Id alloc(size_t size);
    bool                realloc(InstanceStorage::Id& id, size_t size);
    auto                instanceSsbo() const -> const Tempest::StorageBuffer&;

    const DrawClusters& clusters()     const { return clustersMem; }
    const DrawCommands& drawCommands() const { return drawCmd; }
    const DrawBuckets&  drawBuckets()  const { return bucketsMem; };

    // The VSM lamp-face cache (Renderer::drawVsm): spheres (centre, radius) where shadow casters changed since the
    // last frame — a caster's place before and after it moved or turned, one added or removed, shown or hidden;
    // and every frame the animated, morphed, deformed and wind-blown ones, now and a frame ago. Static casters
    // (Landscape, Static draws) first, then the movable ones. Built by prepareGlobals while VSM and its lamp-face cache are on (SceneGlobals::vsmCache)
    struct VsmChanges {
      const Tempest::StorageBuffer* spheres  = nullptr;
      uint32_t                      count    = 0;
      uint32_t                      staticCount = 0;  // the first ones: static casters (Landscape, Static draws)
      uint64_t                      serial   = 0;     // prepareGlobals calls so far: a gap is a frame nobody read
      uint64_t                      epoch    = 0;     // this VisualObjects (another world: another epoch)
      bool                          overflow = false; // more than the list holds: the cache starts empty
      uint32_t                      moved    = 0;     // of count: casters that moved, came or went (the rest: dynamic)
      };
    VsmChanges vsmChanges() const;
    // a caster's move the cache gathers before it counts, cm: a twentieth of a texel of a lamp face at 2 m (0.8 cm);
    // at 0.25 cm the stale place showed as dotted shadow edges against a frame without the cache
    static constexpr float VsmMoveTolerance = 0.05f;
    // the farthest a point within r of the object's origin went from matrix a to b
    static float vsmDisplacement(const Tempest::Matrix4x4& a, const Tempest::Matrix4x4& b, float r);

    void preFrameUpdate ();
    void prepareGlobals (Tempest::Encoder<Tempest::CommandBuffer>& cmd, uint8_t fId);
    void postFrameupdate();

    void visibilityPass (Tempest::Encoder<Tempest::CommandBuffer>& cmd, int pass);
    void visibilityVsm  (Tempest::Encoder<Tempest::CommandBuffer>& cmd);

    void drawMultiply   (Tempest::Encoder<Tempest::CommandBuffer>& cmd);
    void drawTranslucent(Tempest::Encoder<Tempest::CommandBuffer>& cmd);
    void drawWater      (Tempest::Encoder<Tempest::CommandBuffer>& cmd);
    void drawGBuffer    (Tempest::Encoder<Tempest::CommandBuffer>& cmd);
    void drawShadow     (Tempest::Encoder<Tempest::CommandBuffer>& cmd, int layer);
    void drawVsm        (Tempest::Encoder<Tempest::CommandBuffer>& cmd, DrawCommands::VsmDraw draw = DrawCommands::VsmAll);
    void drawHiZ        (Tempest::Encoder<Tempest::CommandBuffer>& cmd);

    bool updateRtScene(RtScene& out);

    void dbgClusters(Tempest::Painter& p, Tempest::Vec2 wsz);

  private:
    struct InstanceDesc;
    struct MorphDesc;
    struct MorphData;

    // A morph anim fades in from timeStart and out until timeUntil, at the speeds of its mesh (the original
    // engine's blend in/out: a face comes and goes in about a second); one face replacing another on the
    // same layer fades in while the other fades out
    struct MorphAnim {
      size_t   id        = 0;
      uint64_t timeStart = 0;
      uint64_t timeUntil = 0;
      float    intensity = 0;
      bool     manual    = false;  // frames set by the owner (setMMShape) instead of played in time
      uint32_t frame0    = 0;
      uint32_t frame1    = 0;
      float    alpha     = 0;
      };

    struct Object {
      bool     isEmpty() const { return cmdId==uint16_t(-1); }

      Tempest::Matrix4x4  pos;
      InstanceStorage::Id objInstance;
      InstanceStorage::Id objMorphAnim;
      InstanceStorage::Id objDeform;
      uint32_t            deformCount = 0;

      DrawCommands::Type  type      = DrawCommands::Type::Landscape;
      uint32_t            iboOff    = 0;
      uint32_t            iboLen    = 0;
      uint32_t            animPtr   = 0;
      DrawBuckets::Id     bucketId;
      uint16_t            cmdId     = uint16_t(-1);
      uint32_t            clusterId = 0;
      uint64_t            timeShift = 0;

      Material::AlphaFunc alpha = Material::Solid;
      MorphAnim           morphAnim[Resources::MAX_MORPH_LAYERS];
      asset::AnimationType wind        = asset::AnimationType::NONE;
      float               windIntensity = 0;
      float               fatness       = 0;
      bool                isGhost       = false;
      bool                visible       = true;
      Resources::TextureCategory textureCategory = Resources::TextureCategory::Dynamic;
      bool                textureDemand = false;
      bool                textureChunkRegistered = false;
      bool                textureChunkedLandscape = false;
      bool                textureWorldOwner = false;
      // VSM lamp-face cache: the caster as the cache last saw it (updateInstance)
      bool                vsmPlaced     = false;
      bool                vsmMovable    = false; // its command's class: DrawCommands::commandId
      uint64_t            vsmHash       = 0;  // all of the instance but the matrix, and the radius
      Tempest::Matrix4x4  vsmPos;
      Tempest::Vec4       vsmSphere;
      uint32_t            textureDistance = Resources::TextureAlwaysFull;
      Tempest::Vec3       texturePosition;
      std::pair<int32_t,int32_t> textureChunk;
      };

    void     preFrameUpdateWind();
    void     preFrameUpdateMorph();

    size_t   implAlloc();
    void     free(size_t id);
    void     updateTextureDemand(size_t id);
    void     setTextureDemand(Object& object, bool active);
    void     updateTextureFocus();

    uint32_t clusterId(const PackedMesh::Cluster* cx, size_t firstMeshlet, size_t meshletCount, uint16_t bucketId, uint16_t commandId);
    uint32_t clusterId(const DrawBuckets::Bucket& bucket, size_t firstMeshlet, size_t meshletCount, uint16_t bucketId, uint16_t commandId);

    void     startMMAnim(size_t i, std::string_view animName, float intensity, uint64_t timeUntil, bool freeOnly);
    void     setMMShape (size_t i, std::string_view animName, uint32_t frame0, uint32_t frame1, float alpha, float intensity);
    float    morphWeight(Object& obj, const MorphAnim& a);
    float    morphWeight(size_t i, std::string_view animName);
    auto     morphSlot  (Object& obj) -> MorphAnim&;
    void     setAsGhost(size_t i, bool g);
    void     setVsmMovable(size_t i, bool movable);

    bool     isVsmDynamic(const Object& obj) const;
    bool     castsVsm(const Object& obj) const;
    Tempest::Vec4 vsmSphere(const Object& obj, const Tempest::Matrix4x4& m) const;
    void     vsmChanged(const Object& obj, const Tempest::Vec4& sphere, bool force = false);
    void     vsmChangedClusters(const Object& obj, bool force = false);
    void     commitVsmChanges(uint8_t fId);

    void     notifyTlas(const Material& m, RtScene::Category cat);
    void     updateInstance(size_t id, Tempest::Matrix4x4* pos = nullptr);
    void     updateRtAs(size_t id);

    void     dbgDraw(Tempest::Painter& p, Tempest::Vec2 wsz, const Camera& cam, const DrawClusters::Cluster& cx);
    void     dbgDrawBBox(Tempest::Painter& p, Tempest::Vec2 wsz, const Camera& cam, const DrawClusters::Cluster& c);

    const SceneGlobals&        scene;

    InstanceStorage            instanceMem;
    DrawBuckets                bucketsMem;
    DrawClusters               clustersMem;
    DrawCommands               drawCmd;

    std::vector<Object>        objects;
    std::unordered_set<size_t> objectsWind;
    std::unordered_set<size_t> objectsMorph;
    std::unordered_set<size_t> objectsFree;

    // VSM lamp-face cache (vsmChanges)
    static constexpr size_t    VsmMaxChanges = 256*1024;
    std::mutex                 vsmSync;
    std::vector<Tempest::Vec4> vsmMoved[2];       // static, movable casters since the last prepareGlobals (vsmSync)
    std::unordered_set<size_t> objectsDynamic;    // Animated, Morph and deformed objects, under vsmSync
    std::vector<Tempest::Vec4> vsmDynamicPrev[2]; // the ones changing every frame as last frame saw them
    std::vector<Tempest::Vec4> vsmUpload;
    Tempest::StorageBuffer     vsmSpheres[Resources::MaxFramesInFlight];
    VsmChanges                 vsmLast;
    mutable std::recursive_mutex textureSync;
    ChunkResidency*            textureResidency = nullptr;
    uint64_t                   textureSubscription = 0;
    uint64_t                   textureFocusSubscription = 0;
    std::map<std::pair<int32_t,int32_t>,std::unordered_set<size_t>> textureChunks;

    friend class Item;
  };

