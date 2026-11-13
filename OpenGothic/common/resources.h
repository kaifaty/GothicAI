#pragma once

#include <Tempest/Font>
#include <Tempest/Texture2d>
#include <Tempest/Device>
#include <Tempest/Pixmap>
#include <Tempest/SoundDevice>

#include "asset/staticmesh.h"
#include "asset/modeltypes.h"
#include "basestore.h"
#include "audiodata.h"
#include "musicdata.h"
#include "asset/worldtypes.h"
#include <dmusic.h>

#include <tuple>
#include <string_view>
#include <map>
#include <set>
#include <thread>
#include <array>
#include <atomic>
#include <functional>
#include <future>
#include <limits>

#include "graphics/material.h"
#include "sound/soundfx.h"
#include "texturebudget.h"
#include "textureflow.h"

class StaticMesh;
class ProtoMesh;
class Skeleton;
class Animation;
class AttachBinder;
class RenderRig;
class RenderRigIndex;
class PfxEmitterMesh;
class GthFont;
class ContentCensus;
struct ChunkTable;
class ChunkResidency;

namespace asset { struct WorldMesh; }

namespace WorldData { struct Data; struct ZenFile; }
namespace VideoData { class Reader; }

class Resources final {
  public:
    static void probeMemory(std::string_view stage);

    // Simulation resources: base geometry, skeletons and animation data,
    // without constructing a graphics or audio device.
    Resources();
    // Audio::Off leaves the sound device closed: a program of the platform without sound (a render test, the island)
    enum class Audio : uint8_t { Open, Off };
    explicit Resources(Tempest::Device& device, Audio audio = Audio::Open);
    ~Resources();

    enum class FontType : uint8_t {
      Normal,
      Hi,
      Disabled,
      Yellow,
      Red
      };

    enum {
      MaxFramesInFlight = 2,
      ShadowLayers      = 2,
      };

    static const size_t MAX_NUM_SKELETAL_NODES = 96;
    static const size_t MAX_MORPH_LAYERS       = 4;

    struct Vertex {
      float    pos[3];
      float    norm[3];
      float    uv[2];
      uint32_t color;
      };

    struct VertexA {
      float    norm[3];
      float    uv[2];
      uint32_t color;          // not a colour: 0 when normalLocal is valid, 0xFFFFFFFF for the rest normal only
      float    pos[4][3];
      uint8_t  boneId[4];
      float    weights[4];
      uint32_t normalLocal[4]; // the normal in each bone's rest frame, octahedral snorm16x2 (packedmesh.cpp)
      };

    struct VertexFsq {
      float    pos[2];
      };

    struct VertexL {
      Tempest::Vec3 pos;
      Tempest::Vec4 cen;
      Tempest::Vec3 color;
      };

    using VobTree = std::vector<std::shared_ptr<asset::VirtualObject>>;

    static Tempest::Device&          device();
    static bool                      hasGraphicsDevice() { return inst!=nullptr && inst->dev!=nullptr; }
    static bool                      isHeadless() { return inst!=nullptr && inst->dev==nullptr; }
    static uint64_t                  generation() { return instGeneration.load(); }
    static const char*               renderer();
    static void                      loadBase();
    // The fingerprint of the selected game base (the one OPENGOTHIC_BASE_FINGERPRINT selects), empty before loadBase.
    static std::string               selectedBaseFingerprint();

    static const Tempest::Sampler&   shadowSampler();

    static const GthFont&            dialogFont(const float scale);
    static const GthFont&            font(const float scale);
    static const GthFont&            font(FontType type, const float scale);
    static const GthFont&            font(std::string_view fname, FontType type, const float scale);

    static const Tempest::Texture2d& fallbackTexture();
    static const Tempest::Texture2d& fallbackBlack();
    // The texture of a material whose own one is missing: DEFAULT.TGA of the base, else a grey checker built in code,
    // so that the mesh is still drawn without a base. nullptr in a headless simulation.
    static const Tempest::Texture2d* loadDefaultTexture();
    static auto                      fallbackImage() -> const Tempest::StorageImage&;
    static auto                      fallbackImage3d() -> const Tempest::StorageImage&;
    // an R32U 1x1x1 image: the VSM page table bound where VSM is off (materials/main.frag reads the cascades then)
    static auto                      fallbackPageTable() -> const Tempest::StorageImage&;
    static Tempest::Texture2d        loadTextureUncached(std::string_view name, bool forceMips = false);
    static const Tempest::Texture2d* loadTexture(std::string_view name, bool forceMips = false, bool pinned = true);
    static const Tempest::Texture2d* loadTexture(Tempest::Color color);
    static bool                      hasTextureVariant(std::string_view name);
    static std::string               textureVariantName(std::string_view name, int32_t v, int32_t c);
    static const Tempest::Texture2d* loadTexture(std::string_view name, int32_t v, int32_t c);
    static       Tempest::Texture2d  loadTexturePm(const Tempest::Pixmap& pm);
    static auto                      loadTextureAnim(std::string_view name, bool pinned = true) -> std::vector<const Tempest::Texture2d*>;
    static       Material            loadMaterial(const asset::Material& src, bool enableAlphaTest,
                                                   bool requireAlphaTest = false);

    struct MaterialMaps {
      const Tempest::Texture2d* albedo = nullptr;
      const Tempest::Texture2d* normal = nullptr;
      const Tempest::Texture2d* orm    = nullptr;
      float                    scale  = 1.f;
      float                    antitileCell = 1.f;
      float                    turbidity = 0.f;
      float                    turbidityDepth = 0.f;
      float                    emissive = 0.f;
      float                    projectionTile = 0.f; // metres per repeat by world position ("projection"); 0: mesh UV
      Material::Fringe         fringe; // "fringe": the second layer over the projection, its maps loaded
      Material::Antitile       antitile = Material::Antitile::Off;
      bool                     hair = false; // hair cards: the ORM slot is the hair map, lit by the hair BRDF
      bool                     strand = false; // strands as ribbons: the albedo is the hair's colour ramp
      bool                     eye = false; // an eyeball: wet highlight, the lid's shade (reports/content/2026-11-hero-head/eye-shader.md)
      FoliageWind::Params      foliage; // "foliage": leaves in the wind, the sun through them (graphics/foliagewind.h)
      bool                     present = false;
      bool                     hasAlbedo = false;
      };
    static MaterialMaps              loadMaterialMaps(std::string_view name);
    static const Tempest::Texture2d* loadMaterialAlbedo(std::string_view name);
    // Drops the materials.json record of `name` with one log line, for a fault seen only once the material is built
    // (projection on an alpha-tested texture): the texture then loads as if the record were not there
    static void                      rejectMaterialMaps(std::string_view name, std::string_view reason);
    static void                      materialAlbedoReplaced(std::string_view name);
    static bool                      materialMapsEnabled();

    enum class TextureCategory : uint8_t { Landscape, Static, Dynamic, Other };
    enum class TextureSource : uint8_t { Base, Hd, Material, Other };
    struct TextureStats {
      uint64_t liveBytes = 0, retiredBytes = 0, peakBytes = 0;
      uint64_t count = 0, evicted = 0, reloaded = 0;
      uint64_t fullBytes = 0, tailBytes = 0, promoted = 0, demoted = 0;
      uint64_t promotionQueue = 0, nearTailCount = 0;
      double nearTailSeconds = 0;
      uint64_t fullRing2Bytes = 0;
      // live bytes by first resident level: 0..4, the last one 5 and coarser
      std::array<uint64_t,6> levelBytes{};
      // wanted textures resident coarser than the wanted level, or missing, and their texture-seconds
      uint64_t shortfallCount = 0;
      double shortfallSeconds = 0;
      uint64_t loadsInFlight = 0;
      // diagnostic totals: demand calls, full requeues of every record, loads started
      uint64_t demandCalls = 0, requeues = 0, loadsStarted = 0;
      std::array<uint64_t,4> sourceBytes{}, sourceFullBytes{};
      std::array<uint64_t,4> categoryBytes{}, categoryCount{};
      // view jumps (completeTextureJump): how many, how many have not reached a zero shortfall (the next jump came
      // first, or still open), the longest synchronous completion and the longest time to a zero shortfall
      uint64_t jumps = 0, jumpsOpen = 0;
      double jumpSyncMsMax = 0, jumpZeroMsMax = 0;
      // the memory budget (texturebudget.h), at the last evaluation: the device's budget and this process's usage,
      // the working target and where it came from, the LRU budget of unrequested textures
      TextureBudget::Device device;
      uint64_t targetBytes = 0, budgetBytes = 0;
      TextureBudget::Source targetSource = TextureBudget::Source::Default;
      // the stage above the target, the deepest so far, the seconds on each stage, and the bytes the demanded
      // textures would take at stage 0 and at the current stage, D(0) and D(stage)
      uint32_t stage = 0, stageDeepest = 0;
      std::array<double,TextureBudget::MaxStage+1> stageSeconds{};
      uint64_t stageBytes0 = 0, stageBytes = 0;
      // step 4 (textureflow.h): the shortfall by state, the visible shortfall, latencies, bursts, bytes in flight
      TextureFlow::Summary flow;
      };
    static TextureStats              textureStats();
    // Step 4: the distributions of the loads' latency, their wait for a slot and the bursts, since the start
    static TextureFlow::Distributions textureFlowDistributions();
    static uint64_t                  textureGeneration();
    static std::vector<const Tempest::Texture2d*> textureChanges(uint64_t since);
    static bool                      texturesReady();
    static std::string               texturesWaitState();
    static Tempest::TextureFormat    textureFormat(const Tempest::Texture2d* texture);
    class TextureLoadScope {
      public:
        explicit TextureLoadScope(bool tail);
        ~TextureLoadScope();
        TextureLoadScope(const TextureLoadScope&) = delete;
        TextureLoadScope& operator=(const TextureLoadScope&) = delete;
      private:
        bool previous;
      };
    static void                      finishTextureLoading();
    // World load with levels by screen size: while a world holds this, a texture that can have a tail is read at its
    // coarsest allowed level whatever its TextureLoadScope; finishTextureLoading before the first frame then loads the
    // wanted levels. With texture_screen_mips=0 the scopes decide, as before.
    class TextureStartup {
      public:
        TextureStartup()=default;
        ~TextureStartup() { end(); }
        TextureStartup(const TextureStartup&) = delete;
        TextureStartup& operator=(const TextureStartup&) = delete;
        void begin();
        void end();
      private:
        bool active = false;
      };
    // The view jumped (ChunkResidency::viewJumped), its owners evaluated: completes now the textures short of their
    // wanted level by two levels or more, the largest shortfall first, then the nearest, within OPENGOTHIC_TEXTURE_JUMP_MS
    // (300 by default, 0: none); the rest loads as usual. Levels only.
    static void                      completeTextureJump();
    static constexpr uint32_t TextureAlwaysFull = uint32_t(-1);
    // What one owner asks of the textures of its material: the ring of its chunk to the hero's or camera's chunk
    // (TextureAlwaysFull: an owner with no place, the whole chain) and, for an owner measured on screen, its resolution
    // exponent (texturelevels.h; NoExponent: the ring decides). With the look-ahead (step 4) `resolution` is from the
    // closest point of the view's path and `present` from the view as it is, for the shortfall; NoExponent: the same.
    struct TextureNeed {
      uint32_t ring       = TextureAlwaysFull;
      int32_t  resolution = std::numeric_limits<int32_t>::min();
      int32_t  present    = std::numeric_limits<int32_t>::min();
      bool operator==(const TextureNeed& other) const {
        return ring==other.ring && resolution==other.resolution && present==other.present;
        }
      bool operator!=(const TextureNeed& other) const { return !(*this==other); }
      };
    static void                      textureDemand(const Material& material, TextureCategory category, bool add, uint32_t distance = TextureAlwaysFull);
    static void                      textureDemand(const Material& material, TextureCategory category, bool add, const TextureNeed& need);
    // One owner's need changes from `from` to `to`, in one pass over its textures.
    static void                      textureDemand(const Material& material, TextureCategory category,
                                                   const TextureNeed& from, const TextureNeed& to);
    // The main view's height in pixels and vertical angle in degrees: the scale of resolution demands.
    static void                      setTextureView(float height, float fovYDegrees);
    // Landscape and static owners demand levels by their size on screen ([GAME] texture_screen_mips).
    static bool                      textureScreenMips();
    static uint32_t                  textureMipReserve();
    // the stage above the memory target (texturebudget.h); owners' distance classes add a margin from stage 2 on
    static uint32_t                  textureStage();
    // log2 of the view scale the levels use (texturelevels.h)
    static float                     textureViewScaleLog2();
    static void                      updateTextures(ChunkResidency* residency);
    // OPENGOTHIC_TEXTURE_DUMP (texturedump.h): every tracked texture with its levels and demand, as CSV
    static void                      dumpTextures(const std::filesystem::path& file);
    static void                      configureTextures(std::string_view budget = {}, std::string_view skipMips = {}, std::string_view target = {},
                                                       std::string_view screenMips = {}, std::string_view lookAhead = {});
    // Step 4 (resources_textureflow.cpp): the look-ahead horizon by the view's velocity, seconds; 0 when it is off
    // ([GAME] texture_lookahead, OPENGOTHIC_TEXTURE_LOOKAHEAD) or with the ring rule
    static double                    textureLookAhead();

    static const AttachBinder*       bindMesh       (const ProtoMesh& anim, const Skeleton& s);
    static const ProtoMesh*          loadMesh       (std::string_view name);
    static const ProtoMesh*          loadCharacterHead(std::string_view instance);
    // the strands of that head (characters.json "groom"), nullptr without them
    static const ProtoMesh*          loadCharacterGroom(std::string_view instance);
    // All static visual readers share the selected base and content overrides.
    static asset::StaticMesh loadVisualMesh(std::string_view name, bool* replaced = nullptr);
    struct MeshReplacementSource {
      std::filesystem::path file;
      size_t size = 0;
      uint32_t crc32 = 0;
      };
    // True after loadVisualMesh successfully selected the replacement;
    // optionally returns its path and the identity of the bytes actually loaded.
    static bool                      meshReplacementLoaded(std::string_view name, MeshReplacementSource* source = nullptr);
    static const PfxEmitterMesh*     loadEmiterMesh (std::string_view name);
    static const Skeleton*           loadSkeleton   (std::string_view name);
    // our render skeleton (models/render-rigs.json, graphics/mesh/renderrig.h): nullptr - none or broken
    static const RenderRig*          loadRenderRig  (std::string_view name);
    static const RenderRig*          renderRigOfMesh(std::string_view mesh);
    // the mesh a body is drawn as for its texture variant ("bodies" of render-rigs.json); "" - the body itself
    static std::string               bodyOfVariant  (std::string_view body, int32_t variant);
    static const Animation*          loadAnimation  (std::string_view name);
    static bool                      loadAnimationData(std::string_view name, asset::ModelAnimation& out);
    static asset::MorphMesh           loadMorphData(std::string_view name);
    static Tempest::Sound            loadSoundBuffer(std::string_view name);

    static std::vector<uint8_t>      readDxMusic(const std::vector<std::u16string>& paths, const char16_t* name,
                                               const char* kind);
    static DmSegment*                loadMusicSegment(char const* name);
    static std::unique_ptr<VideoData::Reader> loadVideo(std::string_view name);
    static void                      startVideoBase();
    static void                      videoFrameRead(std::string_view name);
    static const ProtoMesh*          decalMesh(const asset::VisualDecal& decal);

    static const VobTree*            loadVobBundle(std::string_view name, bool (*readLayer)(VobTree&,std::string_view));

    template<class V>
    static Tempest::VertexBuffer<V>  vbo(const V* data,size_t sz){ return device().vbo(data,sz); }

    template<class I>
    static Tempest::IndexBuffer<I>   ibo(const I* data,size_t sz){ return device().ibo(data,sz); }

    static Tempest::StorageBuffer    ssbo(const void* data, size_t size)         { return device().ssbo(data,size); }
    static Tempest::StorageBuffer    ssbo(Tempest::Uninitialized_t, size_t size) { return device().ssbo(Tempest::Uninitialized,size); }

    template<class V, class I>
    static Tempest::AccelerationStructure
                                     blas(const Tempest::VertexBuffer<V>& b,
                                          const Tempest::IndexBuffer<I>&  i,
                                          size_t offset, size_t size){
      if(!device().properties().raytracing.rayQuery)
        return Tempest::AccelerationStructure();
      return device().blas(b,i,offset,size);
      }

    static void resetRecycled(uint8_t fId);
    static void recycle(Tempest::DescriptorArray&& arr);
    static void recycle(Tempest::StorageBuffer&& ssbo);
    static void recycle(Tempest::StorageImage&& img);
    static void recycle(Tempest::Attachment&& img);
    static void recycle(Tempest::ZBuffer&& img);
    static void recycle(Tempest::AccelerationStructure&& rtas);
    static void recycle(std::shared_ptr<void>&& lifetime);

    static bool                      hasWorld(std::string_view name);

    static std::filesystem::path      baseDir();
    static std::filesystem::path      worldBaseDir(std::string_view world);
    static std::filesystem::path      contentDir();
    static std::filesystem::path      worldOverrideDir(std::string_view world);
    struct WayHeightInputs {
      std::string baseFingerprint;
      std::string contentFingerprint;
      };
    static WayHeightInputs           wayHeightInputs(std::string_view world);
    static void                      censusFile(std::u16string_view path);
    static ContentCensus*            contentCensus();
    static void                      censusProfile(std::string_view game, std::string_view outputUnits, int language);
    static const std::vector<std::pair<std::string,uintmax_t>>& baseArchiveFiles();
    static std::map<std::string,std::filesystem::path> meshReplacementFiles();
    static std::filesystem::path      worldContentDir(const std::filesystem::path& root, std::string_view world);
    static bool                      loadBaseZen(std::string_view name, std::string_view kind, WorldData::ZenFile& out);
    static bool                      writeCensus();

    static const Tempest::IndexBuffer<uint16_t>&   cubeIbo();

  private:
    friend struct BaseContentContract;
    friend struct WorldDataContract;
    friend struct AssetDigest;
    static Resources* inst;
    static std::atomic<uint64_t> instGeneration;
    void initCensus();

    struct BaseIndex {
      std::filesystem::path dir;
      std::vector<std::pair<std::string,uintmax_t>> archives;
      std::vector<std::pair<std::string,uintmax_t>> looseFiles;
      std::map<std::string,std::string> files;
      bool enabled = false;

      void load(const std::filesystem::path& root, const char* kind);
      void validate(const std::vector<std::pair<std::string,uintmax_t>>& mounted);
      };
    BaseIndex baseTextures, baseMeshes, baseZen, baseModels, baseFonts;
    std::map<std::string,std::filesystem::path> contentModels;
    std::unique_ptr<RenderRigIndex> renderRigs;
    AudioData::Index baseAudio;
    MusicData::Index baseMusic;
    std::vector<std::pair<std::string,uintmax_t>> baseArchives;
    std::set<std::string> baseWorlds;
    BaseStore::Selection automaticBase;
    std::string          baseFingerprint;
    struct VideoBase;
    std::shared_ptr<VideoBase> videoBase;
    std::mutex videoStartSync;
    std::thread videoWorker;
    void indexBaseContent();
    void indexContentModels();
    void indexRenderRigs(const std::filesystem::path& file);
    static std::filesystem::path baseModelFile(std::string_view name);
    template<class T>
    static bool loadModelRecord(std::string_view name, T& out, T (*read)(const std::filesystem::path&));
    static bool loadHierarchyData(std::string_view name, asset::ModelHierarchy& out);
    static bool loadModelMeshData(std::string_view name, asset::ModelMesh& out);
    static bool loadModelData(std::string_view name, asset::Model& out);
    static bool loadModelScriptData(std::string_view name, asset::ModelScript& out);

    struct TextureData {
      Tempest::Pixmap pixmap;
      bool useMipmap = false;
      std::filesystem::path source;
      uint8_t firstMip = 0;                  // the first level read, 0: the whole chain
      uint32_t width = 0, height = 0, mipCount = 1; // the whole chain in the file
      };
    static TextureData decodeBaseTexture(const std::filesystem::path& path, bool forceMips, uint32_t skipMips = 0);

    struct DecalK {
      Material mat;
      float    sX = 1;
      float    sY = 1;
      bool     decal2Sided = false;
      bool     operator == (const DecalK& other) const {
        return mat        ==other.mat &&
               sX         ==other.sX &&
               sY         ==other.sY &&
               decal2Sided==other.decal2Sided;
        }
      };

    using TextureCache = std::unordered_map<std::string,std::unique_ptr<Tempest::Texture2d>>;


    void                  indexHdTextures();
    void                  indexMaterialMaps();
    void                  indexMeshOverrides();
    void                  indexCharacterHeads();
    enum class TextureKind : uint8_t { Color, Normal, Orm };
    const Tempest::Texture2d* implLoadMaterialTexture(const std::string& path, TextureKind kind);
    Tempest::Texture2d*   implLoadTexture(std::string_view cname, bool forceMips, bool pinned);
    Tempest::Texture2d    implLoadTextureUncached(std::string_view name, bool forceMips);
    TextureData          implDecodeTexture(std::string_view name, bool forceMips, uint32_t skipMips = 0);
    void                 trackTexture(Tempest::Texture2d* texture, const TextureData& data, TextureKind kind, bool pinned);
    void                 retireTexture(Tempest::Texture2d&& texture);
    struct TextureReload;
    void                 publishTexture(const std::shared_ptr<TextureReload>& reload);
    struct TextureRecord;
    void                 notifyTextureChanged(TextureRecord& record);
    void                 queueTexture(TextureRecord& record);
    void                 unqueueTexture(TextureRecord& record);
    void                 updateNearTailTime();
    uint8_t              wantedLevel(const TextureRecord& record) const { return wantedLevel(record,textureLadder.stage); }
    // `present`: from the owners' present exponents, without the look-ahead (step 4)
    uint8_t              wantedLevel(const TextureRecord& record, uint32_t stage, bool present = false) const;
    // Step 4 (resources_textureflow.cpp): the shortfall's states. The level the screen needs: reserve 0.
    uint8_t              visibleLevel(const TextureRecord& record) const;
    // the level of the owners' present exponents, which the shortfall is measured against
    uint8_t              presentLevel(const TextureRecord& record) const { return wantedLevel(record,textureLadder.stage,true); }
    // an owner's present exponent: its resolution exponent unless the look-ahead set one apart
    static int32_t       presentOf(const TextureNeed& need) {
      return need.present==std::numeric_limits<int32_t>::min() ? need.resolution : std::min(need.present,need.resolution);
      }
    // a record put into the missing or promotion queue `wanted`, or out of it; then whether its shortfall ended
    void                 flowEnter(TextureRecord& record, uint8_t wanted);
    void                 flowLeave(TextureRecord& record);
    void                 flowSettle(TextureRecord& record);
    // its load started, or the main thread saw it move on
    void                 flowRestate(TextureRecord& record);
    void                 flowStarted(TextureRecord& record, TextureReload& reload, uint8_t level);
    void                 flowObserve(TextureReload& reload);
    // the worker's moves since the main thread last saw them, at the times it wrote, up to `phase`
    void                 flowAdvance(TextureRecord& record, TextureFlow::Phase phase);
    void                 textureFlowStats(TextureStats& stats);
    // Part B: the bytes a load of `record` at `level` makes, whether it fits beside the loads in flight; the ready
    // loads published within the frame's budget
    uint64_t             textureLoadBytes(const TextureRecord& record, uint8_t level) const;
    bool                 admitsTextureLoad(const TextureRecord& record, uint8_t level) const;
    // The look-ahead switch from `lookAhead` and OPENGOTHIC_TEXTURE_LOOKAHEAD: read with the other texture settings,
    // applied by the returned function within their requeue
    std::function<void()> configureTextureFlow(std::string_view lookAhead);
    void                 publishReadyTextures(const std::vector<std::shared_ptr<TextureReload>>& ready);
    // Step 3 (resources_texturebudget.cpp): the level of the owners measured on screen at `stage`; `present`: of their
    // present exponents (step 4)
    uint8_t              resolutionLevel(const TextureRecord& record, uint32_t stage, bool present = false) const;
    void                 setTextureBudget(const TextureBudget::Choice& budget, const TextureBudget::Choice& target,
                                          const TextureBudget::Choice& vram);
    // Once a TextureBudget::Period on the main thread, or now with `force`: the device's budget, the working target,
    // the stage. A world load forces it before its first frame.
    void                 updateTextureBudget(bool force);
    void                 chooseTextureStage(bool force, std::chrono::steady_clock::time_point now);
    std::vector<uint64_t> textureStageSums();
    void                 accountTextureStage(std::chrono::steady_clock::time_point now);
    void                 logTextureBudget(std::chrono::steady_clock::time_point now);
    void                 textureBudgetStats(TextureStats& stats);
    uint8_t              tailLevel(const TextureRecord& record) const;
    bool                 wantsFull(const TextureRecord& record) const { return wantedLevel(record)==0; }
    // unqueues every record, applies `change` to the rules, queues them again
    void                 requeueTextures(const std::function<void()>& change);
    void                 implTextureDemand(const Material& material, TextureCategory category, const TextureNeed* release,
                                           const TextureNeed* add);
    std::tuple<uint64_t,uint64_t,uintptr_t> textureDemotionKey(const TextureRecord& record) const;
    void                 startTextureLoad(TextureRecord& record, uint8_t level, bool lowering);
    // The first level to read a texture at when it is loaded: 0 for the whole chain
    uint32_t             textureLoadSkip(bool pinned) const;
    // Loads `records` at their wanted levels now, two at a time in order; with `limited` none starts after `deadline`.
    // Returns how many were published.
    size_t               loadTexturesNow(const std::vector<TextureRecord*>& records,
                                         std::chrono::steady_clock::time_point deadline, bool limited);
    void                 updateTextureJump();
    void                 closeTextureJump(bool reached);
    bool                 hasTexture(std::string_view name) const;
    ProtoMesh*            implLoadMesh(std::string_view name);
    std::unique_ptr<ProtoMesh> implLoadMeshMain(std::string name);
    bool                  replaceModelMesh(asset::ModelMesh& mesh, std::unique_ptr<Skeleton>& skeleton,
                                           std::string_view visual, std::string_view meshFile);
    std::unique_ptr<Animation> implLoadAnimation(std::string name);
    ProtoMesh*            implDecalMesh(const asset::VisualDecal& decal);
    Tempest::Sound        implLoadSoundBuffer(std::string_view name);
    DmSegment*            implLoadMusicSegment(char const* name);
    DmResult              readGkMusic(std::string_view name, const char* kind, std::vector<uint8_t>& bytes) noexcept;
    std::vector<uint8_t>  readMusicRecord(std::string_view provider, std::string_view request,
                                        const char* kind, const MusicData::Source& source);
    GthFont&              implLoadFont(std::string_view fname, FontType type, const float scale);
    PfxEmitterMesh*       implLoadEmiterMesh(std::string_view name);
    const VobTree*        implLoadVobBundle(std::string_view name, bool (*readLayer)(VobTree&,std::string_view));

    Tempest::VertexBuffer<Vertex> sphere(int passCount, float R);

    Tempest::Texture2d fallback, fbZero;
    Tempest::Texture2d fbDefault; // loadDefaultTexture without DEFAULT.TGA, made on the first such material
    Tempest::StorageImage fbImg, fbImg3d, fbPageTbl;

    using BindK  = std::tuple<const Skeleton*,const ProtoMesh*>;
    using FontK  = std::tuple<const std::string,FontType,float>;

    struct Hash {
      size_t operator()(const BindK& b) const {
        return std::uintptr_t(std::get<0>(b));
        }
      size_t operator()(const DecalK& b) const {
        return std::uintptr_t(b.mat.tex);
        }
      size_t operator()(const FontK& b) const {
        std::hash<std::string> h;
        return h(std::get<0>(b));
        }
      };

    struct Less {
      size_t operator()(const Tempest::Color& a, const Tempest::Color& b) const {
        auto ta = std::make_tuple(a.r(),a.g(),a.b(),a.a());
        auto tb = std::make_tuple(b.r(),b.g(),b.b(),b.a());
        return ta<tb;
        }
      };

    Tempest::Device*                  dev = nullptr;
    std::unique_ptr<Tempest::SoundDevice> sound;

    std::recursive_mutex              sync;
    DmLoader*                         dmLoader = nullptr;
    std::unique_ptr<ContentCensus>     census;

    std::vector<uint8_t>              fBuff;
    Tempest::IndexBuffer<uint16_t>    cube;

    struct DeleteQueue {
      std::vector<Tempest::StorageBuffer>   ssbo;
      std::vector<Tempest::StorageImage>    img;
      std::vector<Tempest::Attachment>      att;
      std::vector<Tempest::ZBuffer>         zb;
      std::vector<Tempest::DescriptorArray> arr;
      std::vector<Tempest::AccelerationStructure> rtas;
      std::vector<std::shared_ptr<void>> lifetime;
      std::vector<Tempest::Texture2d> textures;
      uint64_t textureBytes = 0;
      };
    DeleteQueue recycled[MaxFramesInFlight];
    uint8_t     recycledId = 0;

    struct TextureRecord {
      Tempest::Texture2d* texture = nullptr;
      std::filesystem::path source;
      Tempest::TextureFormat format = Tempest::TextureFormat::Undefined;
      TextureKind kind = TextureKind::Color;
      bool mipmaps = false, pinned = false, resident = true, failed = false, loading = false;
      uint8_t firstMip = 0;  // the first resident level, 0: the whole chain
      uint8_t lowest = 0;    // the coarsest allowed first level, 0: no readable tail
      uint32_t width = 0, height = 0, mipCount = 1; // the whole chain
      std::array<uint32_t,4> demand{};
      std::map<uint32_t,uint32_t> distances;   // ring -> owners, of every owner
      std::map<uint32_t,uint32_t> ringNeeds;   // ring -> owners without a resolution demand
      std::map<int32_t,uint32_t>  resolutions; // resolution exponent -> owners
      std::map<int32_t,uint32_t>  presents;    // present exponent -> owners, beside `resolutions` (step 4)
      std::chrono::steady_clock::time_point demoteAt{};
      TextureCategory category = TextureCategory::Other;
      TextureSource sourceKind = TextureSource::Other;
      uint64_t lastDemand = 0, lastNeededFull = 0, bytes = 0, changedGeneration = 0;
      std::tuple<uint64_t,uint64_t,uintptr_t> demotionKey{};
      // for OPENGOTHIC_TEXTURE_DUMP: evictions (of them while it never had an owner) and reloads
      uint32_t evictions = 0, evictionsUnowned = 0, reloads = 0;
      // step 3 (resources_texturebudget.cpp), filled once: 0.5*log2(width*height), the chain's bytes from each level
      double stageTop = -1;
      std::array<uint64_t,16> stageChain{};
      // step 4 (resources_textureflow.cpp): its state while short, whether short of the visible level, since when
      // short, whether an async publication or a load came since, and its load in flight as the main thread saw it
      uint8_t flowState = TextureFlow::NotShort, loadLevel = 0;
      bool flowVisible = false, flowPublished = false, flowLoaded = false;
      TextureFlow::Phase loadPhase = TextureFlow::Phase::Queued;
      std::shared_ptr<TextureFlow::Progress> loadProgress;  // of its load in flight, as its worker writes it
      std::chrono::steady_clock::time_point shortSince{}, flowAt{};  // flowAt: since when it is counted in flowState
      bool tail() const { return firstMip!=0; }
      bool hasTail() const { return lowest!=0; }
      bool wanted() const;
      };
    struct TextureReload {
      Tempest::Texture2d* identity = nullptr;
      struct Payload {
        Tempest::Texture2d texture;
        uint8_t level = 0;
        };
      std::future<Payload> pending;
      Tempest::Texture2d ready;
      uint8_t level = 0;  // asked for, then read
      bool lowering = false;
      std::weak_ptr<int> queued;
      // step 4: how far the worker is, and the bytes its payload will take
      std::shared_ptr<TextureFlow::Progress> progress = std::make_shared<TextureFlow::Progress>();
      uint64_t bytes = 0;
      };
    std::unordered_map<const Tempest::Texture2d*,TextureRecord> textureRecords;
    std::map<std::pair<uint64_t,uintptr_t>,const Tempest::Texture2d*> changedTextures;
    std::map<std::pair<uint64_t,uintptr_t>,TextureRecord*> textureIdle;
    std::set<TextureRecord*> textureMissing;
    std::set<TextureRecord*> texturePromotions;
    std::map<std::pair<std::chrono::steady_clock::time_point,uintptr_t>,TextureRecord*> textureDemotions;
    // ring rule: the farthest, then least recently needed whole; levels: the most bytes to give back first
    std::map<std::tuple<uint64_t,uint64_t,uintptr_t>,TextureRecord*> textureDemotionLru;
    std::set<TextureRecord*> nearTailTextures;
    std::chrono::steady_clock::time_point nearTailUpdated = std::chrono::steady_clock::now();
    double nearTailSeconds = 0, shortfallSeconds = 0;
    TextureFlow::Tracker textureFlow;
    TextureFlow::Limits textureLimits;
    std::atomic<bool> textureLookAheadOn{true};
    std::atomic<double> textureHorizon{0.05};  // TextureLookAhead::horizon of the last window of latencies, seconds
    TextureFlow::Histogram textureHorizonWindow;
    std::atomic<uint32_t> textureStartup{0};  // worlds loading (TextureStartup)
    uint32_t textureJumpMs = 300;
    struct TextureJump {
      bool open = false;
      std::chrono::steady_clock::time_point at{};
      size_t shortAt = 0, candidates = 0, completed = 0;
      double syncMs = 0;
      };
    TextureJump textureJump;
    uint64_t textureJumps = 0, textureJumpsUnresolved = 0;
    double textureJumpSyncMsMax = 0, textureJumpZeroMsMax = 0;
    std::vector<std::shared_ptr<TextureReload>> textureReloads;
    Tempest::Texture2d texturePlaceholders[3];
    uint64_t textureEpoch = 0, textureClock = 0;
    uint64_t texturesEvicted = 0, texturesReloaded = 0, texturesPromoted = 0, texturesDemoted = 0;
    uint32_t textureSkipMips = 2;
    uint64_t textureDemandCalls = 0, textureRequeues = 0, textureLoadsStarted = 0;
    std::atomic<uint32_t> textureReserve{1};
    std::atomic<float> textureViewScale{11.f};  // log2 of the view scale the levels use (texturelevels.h)
    std::atomic<bool> textureScreen{true};
    uint64_t textureBudget = TextureBudget::Floor;
    uint64_t textureTarget = TextureBudget::Floor;
    // Step 3: where the target and the LRU budget come from, OPENGOTHIC_TEXTURE_VRAM_MB, the last device query, the
    // stage, and when the next evaluation and the next log line are due
    TextureBudget::Choice textureTargetChoice, textureVram;
    bool textureBudgetFollows = true, textureBudgetLogDue = true;
    TextureBudget::Device textureDevice, textureDeviceRead;  // with OPENGOTHIC_TEXTURE_VRAM_MB, and as the device said
    std::chrono::steady_clock::time_point textureDeviceAt{};  // the device is asked at most once a Period
    TextureBudget::Source textureTargetSource = TextureBudget::Source::Default;
    TextureBudget::Ladder textureLadder;
    std::atomic<uint32_t> textureStageShared{0};  // textureLadder.stage, read without the lock
    uint32_t textureStageDeepest = 0;
    std::array<double,TextureBudget::MaxStage+1> textureStageSeconds{};
    std::vector<uint64_t> textureSums;  // D(0)..D(deepest) at the last evaluation
    uint64_t textureUntracked = 0;      // live bytes outside the records, measured with no load in flight
    std::chrono::steady_clock::time_point textureBudgetAt{}, textureStageClock{}, textureTargetLoggedAt{};
    uint64_t textureTargetLogged = 0;
    bool textureStreaming = true, textureOverBudget = false, texturePayloadChanged = false;

    TextureCache                                                      texCache;
    // OPENGOTHIC_TEXTURES: sharper textures of our own (scripts/hd-textures.sh) in place of the game's,
    // upper-case name without .TGA -> the DDS (or PNG) file
    std::unordered_map<std::string,std::string>                       hdTextures;
    struct MaterialMapFiles {
      std::string albedo, normal, orm;
      std::string albedoReplacement;
      float       scale = 1.f;
      float       antitileCell = 1.f;
      float       turbidity = 0.f;
      float       turbidityDepth = 0.f;
      float       emissive = 0.f;
      float       projectionTile = 0.f;
      std::string fringeAlbedo, fringeNormal, fringeOrm;
      Material::Fringe fringe; // its tile and bands; the maps are loaded with the record
      Material::Antitile antitile = Material::Antitile::Off;
      bool        hair = false;
      bool        strand = false;
      bool        eye = false;
      FoliageWind::Params foliage; // `on` with the field
      };
    std::unordered_map<std::string,MaterialMapFiles>                  materialMaps;
    TextureCache                                                     materialTextureCache;
    bool                                                             materialMapsPresent = false;
    struct MeshOverride {
      std::filesystem::path file;
      std::string replacement;
      std::string initialStamp;
      bool loaded = false;
      size_t sourceSize = 0;
      uint32_t sourceCrc32 = 0;
      };
    std::unordered_map<std::string,MeshOverride>                     meshOverrides;
    struct CharacterHead {
      std::filesystem::path file;
      std::filesystem::path face;  // empty: no face profile
      std::unique_ptr<ProtoMesh> mesh;
      bool attempted = false;
      std::filesystem::path groom; // empty: no strands
      std::unique_ptr<ProtoMesh> groomMesh;
      bool groomAttempted = false;
      };
    std::unordered_map<std::string,CharacterHead>                    characterHeads;
    std::map<Tempest::Color,std::unique_ptr<Tempest::Texture2d>,Less> pixCache;
    std::unordered_map<std::string,std::unique_ptr<ProtoMesh>>        aniMeshCache;
    std::unordered_map<DecalK,std::unique_ptr<ProtoMesh>,Hash>        decalMeshCache;
    std::unordered_map<std::string,std::unique_ptr<Skeleton>>         skeletonCache;
    std::unordered_map<std::string,std::unique_ptr<Animation>>        animCache;
    std::unordered_map<BindK,std::unique_ptr<AttachBinder>,Hash>      bindCache;
    std::unordered_map<std::string,std::unique_ptr<PfxEmitterMesh>>   emiMeshCache;
    std::unordered_map<std::string,std::unique_ptr<VobTree>>          zenCache;

    std::recursive_mutex                                              syncFont;
    std::unordered_map<FontK,std::unique_ptr<GthFont>,Hash>           gothicFnt;
  };
