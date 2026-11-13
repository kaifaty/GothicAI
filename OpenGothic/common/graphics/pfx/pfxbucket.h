#pragma once

#include <Tempest/VertexBuffer>
#include <vector>
#include <random>
#include <map>
#include <unordered_set>

#include "graphics/pfx/pfxslot.h"
#include "graphics/pfx/pfxobjects.h"
#include "graphics/pfx/pfxmotion.h"
#include "resources.h"

class ParticleTarget;
class ParticleFx;
class VisualObjects;

class PfxBucket {
  public:
    using Vertex = Resources::Vertex;

    PfxBucket(const ParticleFx &decl, PfxObjects& parent, const SceneGlobals& scene, VisualObjects& visual);
    ~PfxBucket();

    enum AllocState: uint8_t {
      S_Free,
      S_Fade,
      S_Inactive,
      S_Active,
      };

    struct ImplEmitter final {
      AllocState    st           = S_Free;
      size_t        block        = size_t(-1);
      bool          textureDemand = false;
      std::pair<int32_t,int32_t> textureChunk;

      Tempest::Vec3 pos          = {};
      Tempest::Vec3 direction[3] = {{1,0,0}, {0,1,0}, {0,0,1}};
      bool          isLoop       = false;

      const ParticleTarget* targetNpc    = nullptr;

      const PfxEmitterMesh* mesh = nullptr;
      const Pose*           pose = nullptr;
      // the mesh's own bone matrices (a render rig's palette), read at each emission; nullptr - the pose's
      const std::vector<Tempest::Matrix4x4>* skin = nullptr;

      uint64_t      waitforNext = 0;
      std::unique_ptr<PfxSlot> next;

      PfxMotion::Flicker flicker;  // a layer emitter's flame signal (vobs.d "flicker"): the particles' brightness
      };

    struct PfxState {
      Tempest::Vec3 pos;
      uint32_t      color  = 0;
      Tempest::Vec3 size;
      uint32_t      bits0  = 0;
      Tempest::Vec3 dir;
      uint32_t      colorB = 0;
      };

    const ParticleFx&           decl;
    PfxObjects&                 parent;
    // the push data of main_pfx_f/_e (main.frag PushPfx)
    struct Push {
      float reactive    = 0; // ParticleFx::taaReactive (graphics plan 1v)
      float hdrFrom     = 0; // particles.d "hdr": the night's gain over a texel's brightness
      float hdrInvRange = 0; // 1/(to - from); 0: off
      float hdrEdge     = 1;
      };
    const Push                  push;

    bool                        isEmpty() const;
    void                        invalidateDecal();

    void                        preFrameUpdate(const SceneGlobals& scene, uint8_t fId);
    void                        drawGBuffer    (Tempest::Encoder<Tempest::CommandBuffer>& cmd, const SceneGlobals &scene, uint8_t fId);
    void                        drawShadow     (Tempest::Encoder<Tempest::CommandBuffer>& cmd, const SceneGlobals &scene, uint8_t fId, int layer);
    void                        drawMultiply   (Tempest::Encoder<Tempest::CommandBuffer>& cmd, const SceneGlobals &scene, uint8_t fId);
    void                        drawTranslucent(Tempest::Encoder<Tempest::CommandBuffer>& cmd, const SceneGlobals &scene, uint8_t fId);

    size_t                      allocEmitter();
    void                        freeEmitter(size_t& id);

    ImplEmitter&                get(size_t id) { return impl[id]; }
    void                        tick(uint64_t dt, const Tempest::Vec3& viewPos);
    void                        buildSsbo();

  private:
    enum UboLinkpackage : uint8_t {
      L_Scene    = 0,
      L_Payload  = 1,
      L_Instance = 2,
      L_Pfx      = L_Instance,
      L_Bucket   = 3,
      L_Ibo      = 4,
      L_Vbo      = 5,
      L_Diffuse  = 6,
      L_Sampler  = 7,
      L_Shadow0  = 8,
      L_Shadow1  = 9,
      L_MorphId  = 10,
      L_Morph    = 11,
      L_SceneClr = 12,
      L_GDepth   = 13,
      L_VsmPageTbl   = 19, // materials_common.glsl
      L_VsmPageAtlas = 20,
      L_FogLut   = 17,
      L_FogLutMs = 18,
      L_FogOcc   = 22, // materials_common.glsl: the sun's bits along the fog's ray
      };

    struct Block final {
      bool          allocated = false;
      uint64_t      timeTotal = 0;

      size_t        offset    = 0;
      size_t        count     = 0;

      Tempest::Vec3 pos       = {};
      float         flicker   = 1.f;  // the emitter's brightness this frame (ImplEmitter::flicker)
      };

    struct Trail final {
      Tempest::Vec3 pos;
      uint64_t      time = 0;
      };

    struct ParState final {
      uint16_t      life=0, maxLife=1;
      Tempest::Vec3 pos, dir;

      std::vector<Trail> trail;

      float         lifeTime() const;
      };

    struct Draw {
      const Tempest::RenderPipeline* pMain   = nullptr;
      const Tempest::RenderPipeline* pShadow = nullptr;

      Tempest::StorageBuffer         pfxGpu;
      uint64_t                       timeShift = 0;

      bool                           isEmpty() const;
      };

    void                        drawCommon(Tempest::Encoder<Tempest::CommandBuffer>& cmd, const SceneGlobals &scene, const Draw& itm,
                                           SceneGlobals::VisCamera view, Material::AlphaFunc func, bool trl);

    void                        tickEmit(Block& p, ImplEmitter& emitter, uint64_t emited);
    bool                        shrink();
    void                        updateTextureDemand(size_t id);
    void                        setTextureDemand(ImplEmitter& emitter, bool active);

    size_t                      allocBlock();
    void                        freeBlock(size_t& s);

    static float                randf();
    static float                randf(float base, float var);

    Block&                      getBlock(ImplEmitter& emitter);
    Block&                      getBlock(PfxSlot&     emitter);

    void                        init     (Block& block, ImplEmitter& emitter, size_t particle);
    void                        finalize (size_t particle);
    void                        tick     (Block& sys, ImplEmitter& emitter, size_t particle, uint64_t dt);
    void                        tickTrail(ParState& ps, ImplEmitter& emitter, uint64_t dt);

    void                        implTickCommon(uint64_t dt, const Tempest::Vec3& viewPos);
    void                        implTickDecals(uint64_t dt, const Tempest::Vec3& viewPos);

    void                        buildSsboTrails();
    void                        buildBilboard(PfxState& v, const Block& p, const ParState& ps, const uint32_t color,
                                              float szX, float szY, float szZ);
    void                        buildTrailSegment(PfxState& v, const Trail& a, const Trail& b, float maxT);
    uint32_t                    mkTrailColor(float clA) const;

    VisualObjects&              visual;
    ChunkResidency*             textureResidency = nullptr;
    uint64_t                    textureSubscription = 0;
    std::map<std::pair<int32_t,int32_t>,std::unordered_set<size_t>> textureChunks;

    Draw                        item  [Resources::MaxFramesInFlight];
    std::vector<PfxState>       pfxCpu;

    Draw                        itemTrl[Resources::MaxFramesInFlight];
    std::vector<PfxState>       trlCpu;

    uint64_t                    maxTrlTime = 0;
    size_t                      blockSize = 0;

    std::vector<ParState>       particles;
    std::vector<ImplEmitter>    impl;
    std::vector<Block>          block;
    bool                        forceUpdate[Resources::MaxFramesInFlight] = {};

    static std::mt19937         rndEngine;

    friend class PfxSlot;
  };

