#pragma once

#include "drawbuckets.h"
#include "drawclusters.h"
#include "drawcommands.h"
#include "instancestorage.h"
#include "texturescreendemand.h"

#include <filesystem>
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
      // An owner placed by a position (spatial: an NPC, an item, a mobsi; not spatial: the hero's, always whole).
      // Returns how far, in centimetres, it may move from `position` before a call can change its demand: the margin
      // of its exponent, or the edge of its chunk; infinity when not spatial.
      float    setTextureOwner(const Tempest::Vec3& position, bool spatial);
      void     setTextureCategory(Resources::TextureCategory category);
      void     setAsGhost  (bool g);
      void     setFatness  (float f);
      void     setWind     (asset::AnimationType m, float intensity);
      void     setVisible  (bool visible);
      // Two vec4 entries (local position, local normal) per packed vertex.
      void     setDeformation(const std::vector<Tempest::Vec4>& vertices);
      // The same for the packed vertices [first, first+count) only: the object reads no others (vertexRange).
      // With the same range as before only the vertices [changedFirst, changedEnd) of the mesh are written.
      void     setDeformation(const Tempest::Vec4* vertices, size_t count, uint32_t first,
                              uint32_t changedFirst, uint32_t changedEnd);
      // the packed vertices of the mesh this object draws: [first, end)
      std::pair<uint32_t,uint32_t> vertexRange() const;
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
    // texturePosition: a landscape package's chunk; texel: its triangles' UV density and world bounds
    Item   get(const StaticMesh& mesh, const Material& mat, size_t iboOff, size_t iboLen, const PackedMesh::Cluster* cluster, DrawCommands::Type type,
               const Tempest::Vec3* texturePosition = nullptr, const TexelDensity* texel = nullptr);
    // All Items referencing the mesh must have been released first.
    void   invalidate(const StaticMesh& mesh);
    void   setTextureResidency(ChunkResidency* residency);
    // Evaluates the owners whose distance demand is due at the residency's view; `flush`: all of them, without
    // the frame budget (before the first frame's synchronous texture loading).
    void   updateTextureView(bool flush = false);
    // OPENGOTHIC_TEXTURE_DUMP (texturedump.h): every texture owner, as CSV
    void   dumpTextureOwners(const std::filesystem::path& file) const;
    // OPENGOTHIC_TEXTURE_DUMP: a line a second with the demand work done since the last one
    void   reportTextureCensus();
    // Queue entries dropped because their owner is past the end of `objects`: by the due loop, by the compaction.
    // For gothic-material-texture-test.
    std::pair<uint64_t,uint64_t> textureQueueDroppedPastEnd() const;
    bool   textureChunkActive(const Tempest::Vec3& position) const;
    ChunkResidency* textureResidencyManager() const;

    InstanceStorage::Id alloc(size_t size);
    bool                realloc(InstanceStorage::Id& id, size_t size);
    auto                instanceSsbo() const -> const Tempest::StorageBuffer&;
    // instanceMem as it was after the previous commit, or instanceSsbo() while history is off
    auto                instanceSsboPrev() const -> const Tempest::StorageBuffer&;
    void                setInstanceHistory(bool enabled);

    const DrawClusters& clusters()     const { return clustersMem; }
    const DrawCommands& drawCommands() const { return drawCmd; }
    const DrawBuckets&  drawBuckets()  const { return bucketsMem; };

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
    void drawVsm        (Tempest::Encoder<Tempest::CommandBuffer>& cmd);
    void drawHiZ        (Tempest::Encoder<Tempest::CommandBuffer>& cmd);

    // The objects marked since the last frame into their slots of `out`, then the frame's upload and TLAS build
    void updateRtScene(RtScene& out, Tempest::Encoder<Tempest::CommandBuffer>& cmd, uint8_t fId);

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
      uint32_t            deformFirst = 0;  // the packed vertex objDeform starts at

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
      Resources::TextureNeed textureNeed;
      // Measured on screen (texeldensity.h): a static mesh's submesh in mesh space, a landscape package's material in
      // the world. textureTexel: in the world, where a static one was last placed (textureAnchor, its position).
      bool                textureMeasured = false;
      bool                texturePlaced = false;
      bool                textureScheduled = false;
      uint64_t            textureStamp = 0;     // of its entry in textureScreen
      TexelDensity        textureLocal;
      TexelDensity        textureTexel;
      // An owner placed by a position (textureWorldOwner): a sphere of this radius around texturePosition, centimetres;
      // -1 for bounds from textureTexel.
      float               textureRadius = -1;
      float               textureScale  = 1;   // the scale of pos it was placed with
      // How far it may move from texturePosition without calling back (setTextureOwner), centimetres: set only by its
      // own calls; its evaluations measure to the sphere grown by it.
      float               textureReach  = 0;
      Tempest::Vec3       textureAnchor;
      float               textureAxes[3][3] = {};  // the axes of pos it was placed with
      Tempest::Vec3       texturePosition;
      std::pair<int32_t,int32_t> textureChunk;
      uint32_t            rtSlot  = RtScene::NoSlot;  // its slot of the ray scene
      bool                rtDirty = false;            // in rtDirty: the slot takes the state at the next frame
      };

    void     preFrameUpdateWind();
    void     preFrameUpdateMorph();

    size_t   implAlloc();
    void     free(size_t id);
    void     updateTextureDemand(size_t id);
    void     setTextureDemand(Object& object, bool active);
    void     updateTextureFocus();
    bool     textureScreenOwner(const Object& object) const;
    // Bounds and density in the world from where the owner is (texturePosition, pos)
    void     placeTexture(Object& object);
    // An owner placed by a position whose matrix now scales by more than 5% otherwise than when it was placed (a body
    // registered before its pose is set): placed and evaluated again, its anchor and reach unchanged
    void     updateTextureScale(size_t id);
    // An owner placed by a position: measured to its sphere grown by its reach
    TextureScreenDemand::Result evaluateTexture(const Object& object) const;
    // the distance class it registered with its exponent, NoClass before it had one
    static uint32_t textureClass(const Object& object);
    // The reach of an owner placed by a position at its own evaluation (setTextureOwner); `moved`: it was registered
    // there already and moved past its previous reach
    float    textureAllowance(const Object& object, bool moved) const;
    // Metres from the nearer of the hero and the camera to its bounds; -1 without a density or a view
    double   textureDistance(const Object& object) const;
    void     scheduleTexture(Object& object, size_t id, double expiry);
    void     unscheduleTexture(Object& object);

    uint32_t clusterId(const PackedMesh::Cluster* cx, size_t firstMeshlet, size_t meshletCount, uint16_t bucketId, uint16_t commandId);
    uint32_t clusterId(const DrawBuckets::Bucket& bucket, size_t firstMeshlet, size_t meshletCount, uint16_t bucketId, uint16_t commandId);

    void     startMMAnim(size_t i, std::string_view animName, float intensity, uint64_t timeUntil, bool freeOnly);
    void     setMMShape (size_t i, std::string_view animName, uint32_t frame0, uint32_t frame1, float alpha, float intensity);
    float    morphWeight(Object& obj, const MorphAnim& a);
    float    morphWeight(size_t i, std::string_view animName);
    auto     morphSlot  (Object& obj) -> MorphAnim&;
    void     setAsGhost(size_t i, bool g);

    void     updateInstance(size_t id, Tempest::Matrix4x4* pos = nullptr);
    // Marks the object for the ray scene: its slot takes the object's state when the frame is built
    void     updateRtAs(size_t id);
    void     updateRtSlot(RtScene& out, Object& obj, bool& landscape);

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
    mutable std::recursive_mutex textureSync;
    ChunkResidency*            textureResidency = nullptr;
    uint64_t                   textureSubscription = 0;
    uint64_t                   textureFocusSubscription = 0;
    uint64_t                   textureViewSubscription = 0;
    TextureScreenDemand        textureScreen;
    uint64_t                   textureStampNext = 0;
    struct TextureCensus {
      bool     enabled = false;
      uint64_t demandCalls = 0, placements = 0, moves = 0, evaluations = 0, stale = 0, compactions = 0, focusOwners = 0;
      uint64_t classes = 0;  // registrations from a distance class change with the same exponent
      uint64_t sweep = 0;    // owners evaluated by the sweep on entering a stage that uses the class
      double   viewMs = 0, focusMs = 0, viewMaxMs = 0, focusMaxMs = 0;
      uint64_t resourceCalls = 0, requeues = 0, loads = 0;  // Resources totals at the last line
      std::chrono::steady_clock::time_point since{};
      } textureCensus;
    size_t                     textureScheduledCount = 0;
    uint64_t                   textureDroppedPastEnd[2] = {};
    TextureScreenDemand::View  textureView;
    size_t                     textureClassSweep = size_t(-1);  // the next owner the sweep evaluates; size_t(-1): none
    bool                       textureScreenMode = false;
    struct TextureChunk {
      std::unordered_set<size_t> ids;
      uint32_t                   ring = uint32_t(-1);  // the ring the last focus change applied to it
      };
    std::map<std::pair<int32_t,int32_t>,TextureChunk> textureChunks;

    // Marks of the ray scene: objects move and appear from worker threads too (Interactive::updateAnimation)
    std::mutex                 rtSync;
    std::vector<size_t>        rtDirty, rtDirtyFrame;
    std::vector<uint32_t>      rtFreed, rtFreedFrame;
    bool                       rtLandscape = false;  // the merged landscape changed

    friend class Item;
  };

