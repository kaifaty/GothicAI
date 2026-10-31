#pragma once

#include <Tempest/Font>
#include <Tempest/Texture2d>
#include <Tempest/Device>
#include <Tempest/Pixmap>
#include <Tempest/SoundDevice>

#include "asset/staticmesh.h"
#include "asset/modeltypes.h"
#include "basestore.h"
#include "programdata.h"
#include "outputunits.h"
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
#include <future>

#include "graphics/material.h"
#include "sound/soundfx.h"

class StaticMesh;
class ProtoMesh;
class Skeleton;
class Animation;
class AttachBinder;
class PfxEmitterMesh;
class GthFont;
class ContentCensus;
struct ChunkTable;
class ChunkResidency;

namespace asset { struct WorldMesh; }

namespace WorldData { struct Data; struct ZenFile; }
namespace VideoData { class Reader; }

namespace Dx8 {
class DirectMusic;
class PatternList;
}

class Resources final {
  public:
    // Simulation resources: base geometry, skeletons and animation data,
    // without constructing a graphics or audio device.
    Resources();
    explicit Resources(Tempest::Device& device);
    ~Resources();

    static void zenkitLog(zenkit::LogLevel level, const char* component, const char* message);

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
      uint32_t color/*unused*/;
      float    pos[4][3];
      uint8_t  boneId[4];
      float    weights[4];
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
    static const char*               renderer();
    static void                      loadBase();

    static const Tempest::Sampler&   shadowSampler();

    static const GthFont&            dialogFont(const float scale);
    static const GthFont&            font(const float scale);
    static const GthFont&            font(FontType type, const float scale);
    static const GthFont&            font(std::string_view fname, FontType type, const float scale);

    static const Tempest::Texture2d& fallbackTexture();
    static const Tempest::Texture2d& fallbackBlack();
    static auto                      fallbackImage() -> const Tempest::StorageImage&;
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
      Material::Antitile       antitile = Material::Antitile::Off;
      bool                     present = false;
      bool                     hasAlbedo = false;
      };
    static MaterialMaps              loadMaterialMaps(std::string_view name);
    static const Tempest::Texture2d* loadMaterialAlbedo(std::string_view name);
    static void                      materialAlbedoReplaced(std::string_view name);
    static bool                      materialMapsEnabled();

    enum class TextureCategory : uint8_t { Landscape, Static, Dynamic, Other };
    struct TextureStats {
      uint64_t liveBytes = 0, retiredBytes = 0, peakBytes = 0;
      uint64_t count = 0, evicted = 0, reloaded = 0;
      std::array<uint64_t,4> categoryBytes{}, categoryCount{};
      };
    static TextureStats              textureStats();
    static uint64_t                  textureGeneration();
    static bool                      texturesReady();
    static Tempest::TextureFormat    textureFormat(const Tempest::Texture2d* texture);
    static void                      textureDemand(const Material& material, TextureCategory category, bool add);
    static void                      updateTextures(ChunkResidency* residency);

    static const AttachBinder*       bindMesh       (const ProtoMesh& anim, const Skeleton& s);
    static const ProtoMesh*          loadMesh       (std::string_view name);
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
    static const Animation*          loadAnimation  (std::string_view name);
    static bool                      loadAnimationData(std::string_view name, asset::ModelAnimation& out);
    static asset::MorphMesh           loadMorphData(std::string_view name);
    static Tempest::Sound            loadSoundBuffer(std::string_view name);

    static Dx8::PatternList          loadDxMusic(std::string_view name);
    static DmSegment*                loadMusicSegment(char const* name);
    static std::unique_ptr<VideoData::Reader> loadVideo(std::string_view name);
    static void                      startVideoBase();
    static void                      videoFrameRead(std::string_view name);
    static const ProtoMesh*          decalMesh(const asset::VisualDecal& decal);

    static const VobTree*            loadVobBundle(std::string_view name);

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
    static int                       baseLanguage();
    static zenkit::DaedalusScript     loadBaseProgram(std::string_view request, int language);
    static bool                      loadBaseOutputUnits(std::string_view request, int language,
                                                        std::vector<std::shared_ptr<asset::CutsceneBlock>>& blocks);
    static zenkit::DaedalusScript     loadBaseProgram(std::string_view request, int language,
                                                     std::string_view branch, std::u16string_view hostPath = {});
    static bool                      loadBaseOutputUnits(std::string_view request, int language,
                                                        std::string_view branch,
                                                        std::vector<std::shared_ptr<asset::CutsceneBlock>>& blocks,
                                                        std::u16string_view hostPath = {});

    static std::filesystem::path      worldBaseDir(std::string_view world);
    static std::filesystem::path      worldOverrideDir(std::string_view world);
    struct WayHeightInputs {
      std::string baseFingerprint;
      std::string contentFingerprint;
      };
    static WayHeightInputs           wayHeightInputs(std::string_view world);
    static void                      censusFile(std::u16string_view path);
    static void                      censusProfile(std::string_view game, std::string_view outputUnits, int language);
    static void                      loadWorld(WorldData::Data& world, asset::WorldMesh& worldMesh, std::string_view name,
                                               ChunkTable* chunks = nullptr);
    static bool                      loadBaseZen(std::string_view name, std::string_view kind, WorldData::ZenFile& out);
    static bool                      writeCensus();

    static const Tempest::IndexBuffer<uint16_t>&   cubeIbo();

  private:
    friend struct BaseContentContract;
    friend struct WorldDataContract;
    friend struct AssetDigest;
    static Resources* inst;
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
    ProgramData::Index basePrograms;
    OutputUnits::Index baseOutputUnits;
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
    static std::filesystem::path baseDir();
    void indexBaseContent();
    static std::filesystem::path baseModelFile(std::string_view name);
    static bool loadHierarchyData(std::string_view name, asset::ModelHierarchy& out);
    static bool loadModelMeshData(std::string_view name, asset::ModelMesh& out);
    static bool loadModelData(std::string_view name, asset::Model& out);
    static bool loadModelScriptData(std::string_view name, asset::ModelScript& out);

    struct TextureData {
      Tempest::Pixmap pixmap;
      bool useMipmap = false;
      std::filesystem::path source;
      };
    static TextureData decodeBaseTexture(const std::filesystem::path& path, bool forceMips);

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
    enum class TextureKind : uint8_t { Color, Normal, Orm };
    const Tempest::Texture2d* implLoadMaterialTexture(const std::string& path, TextureKind kind);
    Tempest::Texture2d*   implLoadTexture(std::string_view cname, bool forceMips, bool pinned);
    Tempest::Texture2d    implLoadTextureUncached(std::string_view name, bool forceMips);
    TextureData          implDecodeTexture(std::string_view name, bool forceMips);
    void                 trackTexture(Tempest::Texture2d* texture, const TextureData& data, TextureKind kind, bool pinned);
    void                 retireTexture(Tempest::Texture2d&& texture);
    void                 publishTexture();
    bool                 hasTexture(std::string_view name) const;
    ProtoMesh*            implLoadMesh(std::string_view name);
    std::unique_ptr<ProtoMesh> implLoadMeshMain(std::string name);
    bool                  replaceModelMesh(asset::ModelMesh& mesh, std::unique_ptr<Skeleton>& skeleton,
                                           std::string_view visual, std::string_view meshFile);
    std::unique_ptr<Animation> implLoadAnimation(std::string name);
    ProtoMesh*            implDecalMesh(const asset::VisualDecal& decal);
    Tempest::Sound        implLoadSoundBuffer(std::string_view name);
    Dx8::PatternList      implLoadDxMusic(std::string_view name);
    DmSegment*            implLoadMusicSegment(char const* name);
    DmResult              readGkMusic(std::string_view name, const char* kind, std::vector<uint8_t>& bytes) noexcept;
    std::vector<uint8_t>  readDxMusic(const std::vector<std::u16string>& paths, const char16_t* name, const char* kind);
    std::vector<uint8_t>  readMusicRecord(std::string_view provider, std::string_view request,
                                        const char* kind, const MusicData::Source& source);
    GthFont&              implLoadFont(std::string_view fname, FontType type, const float scale);
    PfxEmitterMesh*       implLoadEmiterMesh(std::string_view name);
    const VobTree*        implLoadVobBundle(std::string_view name);

    Tempest::VertexBuffer<Vertex> sphere(int passCount, float R);

    Tempest::Texture2d fallback, fbZero;
    Tempest::StorageImage fbImg;

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
    std::unique_ptr<Dx8::DirectMusic> dxMusic;
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
      bool mipmaps = false, pinned = false, resident = true, failed = false;
      std::array<uint32_t,4> demand{};
      TextureCategory category = TextureCategory::Other;
      uint64_t lastDemand = 0, bytes = 0;
      bool wanted() const;
      };
    struct TextureReload {
      Tempest::Texture2d* identity = nullptr;
      std::future<Tempest::Texture2d> pending;
      Tempest::Texture2d ready;
      std::weak_ptr<int> queued;
      };
    std::unordered_map<const Tempest::Texture2d*,TextureRecord> textureRecords;
    std::shared_ptr<TextureReload> textureReload;
    Tempest::Texture2d texturePlaceholders[3];
    uint64_t textureEpoch = 0, textureClock = 0;
    uint64_t texturesEvicted = 0, texturesReloaded = 0;
    bool textureOverBudget = false;

    TextureCache                                                      texCache;
    // OPENGOTHIC_TEXTURES: sharper textures of our own (scripts/hd-textures.sh) in place of the game's,
    // upper-case name without .TGA -> the DDS (or PNG) file
    std::unordered_map<std::string,std::string>                       hdTextures;
    struct MaterialMapFiles {
      std::string albedo, normal, orm;
      std::string albedoReplacement;
      float       scale = 1.f;
      float       antitileCell = 1.f;
      Material::Antitile antitile = Material::Antitile::Off;
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
