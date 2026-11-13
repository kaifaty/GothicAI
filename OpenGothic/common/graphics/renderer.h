#pragma once

#include <Tempest/RenderPipeline>
#include <Tempest/CommandBuffer>
#include <Tempest/Matrix4x4>
#include <Tempest/Widget>
#include <Tempest/Device>
#include <Tempest/UniformBuffer>
#include <Tempest/VectorImage>

#include "worldview.h"
#include "shaders.h"
#include "blooddecals.h"
#include "taa.h"
#include "eyeadaptation.h"

class Camera;
class InventoryMenu;
class VideoWidget;

class Renderer final {
  public:
    Renderer();
    ~Renderer();

    void onWorldChanged();
    // Bench `deterministic on`: the next frame starts the TAA history over at jitter phase 0
    // and the SSAO history over at phase 0 of its sample cycle
    void resetTemporal();
    // bench `vsm sides 1|2|3|0`: the sides a lamp's cube face keeps (settings.vsmLampSides) instead of the setting
    static uint32_t benchVsmLampSides;
    // bench `vsm study <bits>`: the shadow test on a mesh's facets (scene.vsmStudy, VSM_STUDY_* of scene.glsl)
    static uint32_t benchVsmStudy;

    void draw(Tempest::Attachment& result, Tempest::Encoder<Tempest::CommandBuffer>& cmd, uint8_t fId,
              Tempest::VectorImage::Mesh& uiLayer, Tempest::VectorImage::Mesh& numOverlay,
              InventoryMenu &inventory, VideoWidget& video);
    void draw(Tempest::Attachment& result, Tempest::Encoder<Tempest::CommandBuffer>& cmd, uint8_t fId,
              WorldView& view, const Camera& camera);

    void dbgDraw(Tempest::Painter& painter);

    // reuse: an image of an earlier call to draw into again, if its size still fits (a video frame every step)
    Tempest::Attachment screenshoot(uint8_t frameId, Tempest::Attachment&& reuse = Tempest::Attachment());

  private:
    enum Quality : uint8_t {
      None,
      VolumetricLQ,
      VolumetricHQ,
      };
    Tempest::Size internalResolution(Tempest::Size src) const;
    float         internalResolutionScale() const;
    Tempest::Size effectResolution() const;

    void updateCamera(const WorldView& wview, const Camera &camera);
    void prepareTaa(WorldView& wview, const Camera& camera);
    bool requiresRtScene() const;
    bool requiresLightsTree() const;

    Tempest::StorageImage&  usesImage2d(Tempest::StorageImage& ret, Tempest::TextureFormat frm, uint32_t w, uint32_t h, bool mips = false);
    Tempest::StorageImage&  usesImage2d(Tempest::StorageImage& ret, Tempest::TextureFormat frm, Tempest::Size sz, bool mips = false);
    Tempest::StorageImage&  usesImage3d(Tempest::StorageImage& ret, Tempest::TextureFormat frm, uint32_t w, uint32_t h, uint32_t d, bool mips = false);
    Tempest::ZBuffer&       usesZBuffer(Tempest::ZBuffer&      ret, Tempest::TextureFormat frm, uint32_t w, uint32_t h);
    Tempest::Attachment&    usesAttachment(Tempest::Attachment&ret, Tempest::TextureFormat frm, Tempest::Size sz);
    Tempest::Attachment&    usesAttachment(Tempest::Attachment&ret, Tempest::TextureFormat frm, uint32_t w, uint32_t h);
    Tempest::StorageBuffer& usesSsbo(Tempest::StorageBuffer& ret, size_t size);
    Tempest::StorageBuffer& usesSsboInit(Tempest::StorageBuffer& ret, size_t size);

    void prepareUniforms(WorldView& wview);
    void resetViewport(Tempest::Size size, Tempest::Size fullRes);
    void resetShadowmap();
    void resetSkyFog();

    void prepareSky       (Tempest::Encoder<Tempest::CommandBuffer>& cmd, WorldView& wview);
    void prepareSSAO      (Tempest::Encoder<Tempest::CommandBuffer>& cmd, WorldView& wview);
    void prepareFog       (Tempest::Encoder<Tempest::CommandBuffer>& cmd, WorldView& wview);
    void prepareIrradiance(Tempest::Encoder<Tempest::CommandBuffer>& cmd, WorldView& wview);
    void prepareGi        (Tempest::Encoder<Tempest::CommandBuffer>& cmd, uint8_t fId, WorldView& wview);
    void readGiStats      (uint8_t fId);
    void prepareExposure  (Tempest::Encoder<Tempest::CommandBuffer>& cmd, WorldView& wview);
    bool eyeActive() const;
    void updateEye        (uint8_t fId, const Camera& camera);
    void drawEyeMeter     (Tempest::Encoder<Tempest::CommandBuffer>& cmd, uint8_t fId, const WorldView& wview);
    bool fogSkyActive() const;
    void updateFogSky     (const Camera& camera);
    void prepareFogSkyVis (Tempest::Encoder<Tempest::CommandBuffer>& cmd, WorldView& wview);
    void prepareEpipolar  (Tempest::Encoder<Tempest::CommandBuffer>& cmd, WorldView& wview);

    void prepareLightsBvh (Tempest::Encoder<Tempest::CommandBuffer>& cmd, WorldView& wview);

    void prepareSurfels   (Tempest::Encoder<Tempest::CommandBuffer>& cmd, WorldView& wview);
    void surfelsApply     (Tempest::Encoder<Tempest::CommandBuffer>& cmd, WorldView& wview, int32_t tileSize, bool postPass);
    void surfelsBinning   (Tempest::Encoder<Tempest::CommandBuffer>& cmd, WorldView& wview, int32_t tileSize, bool postPass);
    void surfelsTrace     (Tempest::Encoder<Tempest::CommandBuffer>& cmd, WorldView& wview, Tempest::StorageBuffer& surfels, bool postPass);

    void drawHiZ          (Tempest::Encoder<Tempest::CommandBuffer>& cmd, WorldView& view);
    void buildHiZ         (Tempest::Encoder<Tempest::CommandBuffer>& cmd);
    void drawVsm          (Tempest::Encoder<Tempest::CommandBuffer>& cmd, WorldView& view);
    void logVsmStats      (const WorldView& view);
    void drawGBuffer      (Tempest::Encoder<Tempest::CommandBuffer>& cmd, uint8_t fId, WorldView& view);
    void drawGWater       (Tempest::Encoder<Tempest::CommandBuffer>& cmd, WorldView& view);
    void drawShadowMap    (Tempest::Encoder<Tempest::CommandBuffer>& cmd, uint8_t fId, WorldView& view);
    void drawShadowResolve(Tempest::Encoder<Tempest::CommandBuffer>& cmd, const WorldView& view);
    void drawLights       (Tempest::Encoder<Tempest::CommandBuffer>& cmd, const WorldView& view);
    void drawSky          (Tempest::Encoder<Tempest::CommandBuffer>& cmd, const WorldView& view);
    void drawAmbient      (Tempest::Encoder<Tempest::CommandBuffer>& cmd, const WorldView& view);
    void drawTonemapping  (Tempest::Attachment& result, Tempest::Encoder<Tempest::CommandBuffer>& cmd, const WorldView& wview);
    void drawBloom        (Tempest::Encoder<Tempest::CommandBuffer>& cmd, bool fused = false);
    void drawCMAA2        (Tempest::Attachment& result, Tempest::Encoder<Tempest::CommandBuffer>& cmd, const WorldView& wview);
    void drawTaa          (Tempest::Encoder<Tempest::CommandBuffer>& cmd, bool fused = false);
    void drawTonemappingTaa(Tempest::Attachment& result, Tempest::Encoder<Tempest::CommandBuffer>& cmd, const WorldView& wview);
    void drawTaaSharpen   (Tempest::Encoder<Tempest::CommandBuffer>& cmd);
    void drawUpscale      (Tempest::Attachment& result, Tempest::Encoder<Tempest::CommandBuffer>& cmd);
    void drawTemporalUpscale(Tempest::Encoder<Tempest::CommandBuffer>& cmd);
    void drawTonemappingTemporal(Tempest::Attachment& result, Tempest::Encoder<Tempest::CommandBuffer>& cmd, const WorldView& wview);
    bool useTemporalUpscale(Tempest::Size res, Tempest::Size fullRes) const;
    void drawReflections  (Tempest::Encoder<Tempest::CommandBuffer>& cmd, const WorldView& wview);
    void drawUnderwater   (Tempest::Encoder<Tempest::CommandBuffer>& cmd, const WorldView& wview);
    void drawFog          (Tempest::Encoder<Tempest::CommandBuffer>& cmd, const WorldView& wview);
    void drawSunMoon      (Tempest::Encoder<Tempest::CommandBuffer>& cmd, const WorldView& wview);
    void drawSunMoon      (Tempest::Encoder<Tempest::CommandBuffer>& cmd, const WorldView& wview, bool isSun);

    void drawPathtrace    (Tempest::Encoder<Tempest::CommandBuffer>& cmd, WorldView& wview, uint8_t fId);

    void stashSceneAux    (Tempest::Encoder<Tempest::CommandBuffer>& cmd);

    void drawRayQueryDbg  (Tempest::Encoder<Tempest::CommandBuffer>& cmd, const WorldView& wview);
    void drawMotionView   (Tempest::Attachment& result, Tempest::Encoder<Tempest::CommandBuffer>& cmd);
    void drawReactiveView (Tempest::Attachment& result, Tempest::Encoder<Tempest::CommandBuffer>& cmd);
    void prepareMotion    (WorldView& wview);
    void drawProbesDbg    (Tempest::Encoder<Tempest::CommandBuffer>& cmd, const WorldView& wview);
    void drawSurfelsDbg   (Tempest::Encoder<Tempest::CommandBuffer>& cmd, const WorldView& wview);
    void drawProbesHitDbg (Tempest::Encoder<Tempest::CommandBuffer>& cmd);
    void drawVsmDbg       (Tempest::Encoder<Tempest::CommandBuffer>& cmd, const WorldView& wview);
    void drawHashDbg      (Tempest::Attachment& result, Tempest::Encoder<Tempest::CommandBuffer>& cmd, const WorldView& wview);
    void drawLightTreeDbg (Tempest::Attachment& result, Tempest::Encoder<Tempest::CommandBuffer>& cmd, const WorldView& wview);

    void setupSettings();
    void toggleGi();
    void toggleVsm();
    void togglePathtrace();

    struct Settings {
      const uint32_t shadowResolution   = 2048;
      bool           vsmEnabled         = false;
      bool           pathTraceEnabled   = false;

      bool           zEnvMappingEnabled = false;
      bool           zCloudShadowScale  = false;
      bool           halfResAoFog       = true;
      bool           spatialUpscale     = false;
      bool           zFogRadial         = false;

      bool           zWindEnabled       = false;
      uint64_t       windPeriod         = 6000;

      GiMethod       giMethod           = GiMethod::None;
      bool           aaEnabled          = false;
      bool           taaEnabled         = false;
      float          taaSharpness       = 0.75f; // RCAS after the resolve, 0..1
      bool           ssaoHistory        = true;  // SSAO: 8 samples a frame with a history; off: 32 a frame
      float          taaMipBias         = -0.5f; // material textures of the main view with TAA
      // a study of the temporal upscale (graphics plan 1g): MetalFX Temporal in place of the TAA resolve
      bool           temporalUpscale    = false; // OPENGOTHIC_UPSCALE=temporal, on Metal
      float          renderScale        = 0;     // OPENGOTHIC_RENDER_SCALE=0.5..1 over vidResIndex; 0 unset
      bool           upscaleMipBiasSet  = false; // OPENGOTHIC_UPSCALE_MIP_BIAS over log2(in/out) - 1
      float          upscaleMipBias     = 0;
      bool           bloomEnabled       = true;
      // motion vectors of objects (graphics plan 1b), on by default: -mv 0, [INTERNAL] motionVectors=0,
      // OPENGOTHIC_MOTION_VECTORS=0|1 over both
      bool           motionVectors      = false;
      bool           motionView         = false; // OPENGOTHIC_MOTION_VIEW=1: the frame shows gbufMotion (debug)
      float          motionViewPx       = 2;     // OPENGOTHIC_MOTION_VIEW_PX: px/frame of its full brightness
      // the TAA reactive mask of particles and animated translucent (graphics plan 1v), on by default
      // (setupSettings): [INTERNAL] taaReactive=0 turns it off, OPENGOTHIC_TAA_REACTIVE=0|1 over it. The mask is
      // written either way; off, the resolve does not use it
      bool           taaReactive        = false;
      bool           taaReactiveView    = false; // OPENGOTHIC_TAA_REACTIVE_VIEW=1: the frame shows taa.reactive
      float          taaReactiveMax     = 0.9f;  // OPENGOTHIC_TAA_REACTIVE_MAX: the mask's ceiling in the resolve
      float          taaReactiveLref    = 0.25f; // OPENGOTHIC_TAA_REACTIVE_LREF: added light of a full mask

      float          zVidBrightness     = 0.5;
      float          zVidContrast       = 0.5;
      float          zVidGamma          = 0.5;

      float          sunSize            = 0;
      float          moonSize           = 0;

      float          vidResIndex        = 0;

      float          vsmMipBias         = 0.25; //TODO: set to lower, eventually
      bool           vsmStats           = false; // OPENGOTHIC_VSM_STATS=1: VSM counters to the log every 5 s
      uint32_t       vsmLampMode        = 0;     // OPENGOTHIC_VSM_LAMP: 0 pcss, 1 hard, 2 flat (lamp filter study)
      float          vsmLampNarrow      = -1;    // OPENGOTHIC_VSM_LAMP_NARROW: four-tap penumbra, texels; <0 the shader's
      // under TAA (light.frag, the frame-turned pattern): taps of the first and second blocker search, of a narrow
      // and of a wide penumbra; the defaults are the shader's OmniTaa* (OPENGOTHIC_VSM_LAMP_TAPS)
      uint32_t       vsmLampTaps[4]     = {2, 2, 4, 6};
      uint32_t       vsmLampFlags       = 0;     // 1 OPENGOTHIC_VSM_LAMP_FLOOR, 2 OPENGOTHIC_VSM_LAMP_CENTRE (under TAA)
      uint32_t       vsmLampExp         = 0;     // OPENGOTHIC_VSM_LAMP_EXP: experiments (light.frag, probeExp)
      bool           vsmLampTaa         = false; // OPENGOTHIC_VSM_LAMP_TAA=1: the frame-turned lamp taps under TAA (study)
      bool           vsmLampStudy       = false; // any OPENGOTHIC_VSM_LAMP*: the study shader light_vsm_probe
      float          vsmLampNearCm      = 30;    // [INTERNAL] vsmLampNearCm: a lamp's fixture this close casts no shadow
      float          lampNearKnee       = 30;    // OPENGOTHIC_LAMP_KNEE: the light near a lamp never passes this, 0 off
      // [INTERNAL] vsmLampSoft (0 on Metal, 1 elsewhere), OPENGOTHIC_VSM_LAMP_SOFT=0|1 wins: the lamps' PCSS;
      // false: one gather per lamp and pixel (light_vsm_hard)
      bool           vsmLampSoft        = true;
      // [INTERNAL] vsmLampSides, OPENGOTHIC_VSM_LAMP_SIDES wins: the sides a lamp's cube face keeps (1 the far side,
      // 2 both sides of the world mesh, 3 of objects too, the default; main.vert, meshShader)
      uint32_t       vsmLampSides       = 3;
      } settings;

    Frustrum                  frustrum[SceneGlobals::V_Count];
    Tempest::Matrix4x4        proj, viewProj, viewProjLwc;
    Tempest::Matrix4x4        shadowMatrix[Resources::ShadowLayers];
    Tempest::Matrix4x4        shadowMatrixVsm;
    Tempest::Vec3             clipInfo;

    Tempest::Attachment       sceneLinear;
    // bloom chain: half, quarter, ... of the internal resolution (drawBloom)
    std::array<Tempest::Attachment,6> bloomChain;
    Tempest::ZBuffer          zbuffer, shadowMap[Resources::ShadowLayers];
    Tempest::ZBuffer          zbufferUi;

    Tempest::Attachment       sceneOpaque;
    Tempest::Attachment       sceneDepth;

    Tempest::Attachment       gbufDiffuse;
    Tempest::Attachment       gbufNormal;
    Tempest::Attachment       gbufOrm;

    struct Shadow {
      Tempest::RenderPipeline* directLightPso = nullptr;
      } shadow;

    struct Lights {
      Tempest::RenderPipeline* directLightPso = nullptr;
      } lights;

    struct {
      Tempest::StorageBuffer bvh, tree;
      Tempest::StorageBuffer bvhMorton, bvhAlux, bvhCtrl;
      } lightsTree;

    struct Sky {
      Quality                quality       = Quality::None;

      Tempest::TextureFormat lutRGBFormat  = Tempest::TextureFormat::R11G11B10UF;
      Tempest::TextureFormat lutRGBAFormat = Tempest::TextureFormat::RGBA16F;

      Tempest::Sampler       sampler = Tempest::Sampler::bilinear();

      bool                   lutIsInitialized = false;
      Tempest::Attachment    transLut, multiScatLut, viewLut, viewCldLut;
      Tempest::StorageImage  cloudsLut, fogLut3D, fogLut3DMs;
      Tempest::StorageImage  occlusionLut, irradianceLut;
      } sky;

    struct SSAO {
      Tempest::TextureFormat    aoFormat = Tempest::TextureFormat::R8;
      Tempest::StorageImage     ssaoBuf;
      Tempest::StorageImage     ssaoBlur;
      Tempest::StorageImage     depthHalf; // R32F depth at half size for the samples of ssao.comp, ssao_depth.comp
      // the occlusion over frames (ssao.comp): two histories in turn, H and M per sample, linear depth, frames
      Tempest::StorageImage     history[2];
      uint8_t                   cur       = 0;     // history[cur] is written this frame, the other one read
      uint32_t                  frame     = 0;     // the cycle of 4 frames picks the samples
      bool                      valid     = false; // the history holds the last frame of this view
      } ssao;

    struct Cmaa2 {
      Tempest::StorageImage     workingEdges;
      Tempest::StorageBuffer    shapeCandidates;
      Tempest::StorageBuffer    deferredBlendLocationList;
      Tempest::StorageBuffer    deferredBlendItemList;
      Tempest::StorageImage     deferredBlendItemListHeads;
      Tempest::StorageBuffer    controlBuffer;
      Tempest::StorageBuffer    indirectBuffer;
      } cmaa2;

    // temporal AA by the camera (docs/graphics-plan.md, step 1a): two histories in turn, the resolved frame goes
    // into `output`, which then takes the place of sceneLinear
    struct {
      Tempest::Attachment       history[2];
      Tempest::Attachment       output;
      // the reactive mask (graphics plan 1v): R8, the second attachment of the translucent pass, cleared to 0 each
      // frame; how much a pixel shows what changes by itself (particles, animated translucent). There with the TAA
      // off too: the translucent pipelines always write it
      Tempest::Attachment       reactive;
      uint8_t                   cur       = 0;     // history[cur] is written this frame, the other one read
      uint32_t                  frame     = 0;     // the jitter phase
      bool                      active    = false; // this frame is jittered and resolved
      bool                      valid     = false; // the history holds a frame of this view
      bool                      shot      = false; // screenshoot(): an image of the internal size
      Tempest::Matrix4x4        reproject;
      Tempest::Vec2             jitter;              // this frame's offset, pixels of the internal resolution
      } taa;

    // The main camera of this frame and of the drawn frame before it, without jitter (updateCamera). TAA, the SSAO
    // history and the motion vectors of objects all reproject by this one pair; a cut (Camera::cuts), a jump
    // (Taa::isCameraJump), a new world or a reset of the temporal state break it
    struct {
      Taa::View                 cur, prev;
      uint32_t                  cuts       = 0;     // Camera::cuts() of `cur`
      bool                      hasCur     = false;
      bool                      continuous = false; // `prev` is the frame before `cur`, of the same shot
      } camHistory;

    // motion vectors of objects (graphics plan 1b): the objects' own motion, RG16F at the internal resolution, the
    // residual against the camera's in texture coordinates, current -> previous; 0 where nothing moves by itself
    Tempest::Attachment       gbufMotion;

    // the temporal upscale (graphics plan 1g, OPENGOTHIC_UPSCALE=temporal): MetalFX Temporal takes the place of the
    // TAA resolve and keeps the history itself; bloom and tonemapping go on at the swapchain's resolution
    struct {
      Tempest::Attachment       motion;              // RG16F: to where the point was a frame ago, input texture coordinates
      Tempest::StorageImage     output;              // linear HDR of the swapchain's size: bloom and tonemapping read it
      uint32_t                  phases    = Taa::Phases;
      float                     mipBias   = 0;
      bool                      active    = false;   // this frame is resolved by MetalFX
      } temporal;

    struct Upscale {
      Tempest::Attachment       input;  // tonemapped frame at internal resolution
      Tempest::StorageImage     output; // MetalFX result at swapchain resolution
      } upscale;

    struct {
      Tempest::StorageImage     hiZ;
      } hiz;

    struct {
      const uint32_t            atlasDim  = 256; // sqrt(maxProbes)
      const uint32_t            maxProbes = atlasDim*atlasDim; // 65536

      Tempest::StorageBuffer    voteTable, hashTable, freeList, newList, voteTiles;
      Tempest::StorageBuffer    requests;           // the age histogram and the requests of probes at hits
      Tempest::StorageBuffer    probes;
      Tempest::StorageImage     probesGBuffDiff;
      Tempest::StorageImage     probesGBuffNorm;
      Tempest::StorageImage     probesGBuffRayT;
      const uint32_t            irrTileW  = 4;   // probeIrrTileW/H of probe_common.glsl
      const uint32_t            irrTileH  = 2;
      Tempest::StorageImage     probesLighting;     // RGBA16F: the L2 harmonics of irradiance of a probe, 7 texels of 4x2
      Tempest::StorageImage     probesRadiance;     // R32UI, RGB9E5: the radiance of each sample, relit in turns
      uint32_t                  frame    = 0;       // turns the samples relit in a frame
      uint64_t                  lastTick = 0;       // the world's tick of the last lighting, for the blend over time
      float                     frameTime = 0;      // smoothed world time of a frame, s: how many samples to relight
      bool                      resetHistory = false; // another world: the probes start over, as on the first frame
      int64_t                   dayTime     = -1;  // the game's time of day (gtime, ms) of this frame, set by draw
      int64_t                   lastDayTime = -1;  // of the last lighting: a jump lights the probes anew
      // the counters of probe_stats (live probes, traced in the frame), read back a frame slot later (FrameProfile)
      Tempest::StorageBuffer    stats[Resources::MaxFramesInFlight];
      bool                      statsWritten[Resources::MaxFramesInFlight] = {};
      } gi;

    EyeAdaptation             eye; // the eye's adaptation under GI (renderer_eye.cpp)
    // the sky seen from the camera for the fog under GI (renderer_fogsky.cpp, fog_sky_vis.comp)
    struct {
      Tempest::StorageBuffer    state;            // the smoothed share and this frame's
      bool                      hasLast  = false;
      uint64_t                  lastTick = 0;
      Tempest::Vec3             lastCam;
      float                     alpha    = 0;     // 1 - exp(-dt/tau) of this frame
      uint32_t                  reset    = 1;
      bool                      ranLast  = false; // the pass ran in the frame before (reset otherwise)
      uint32_t                  frame    = 0;
      } fogSky;

    struct {
      const Tempest::IVec2      gbufTile   = {8};
      const uint32_t            gbufTilesX = 256;
      const uint32_t            gbufTilesY = 256;
      const uint32_t            maxSurfels = gbufTilesX*gbufTilesY;
      Tempest::StorageBuffer    surfels;

      Tempest::StorageImage     irrImage;
      Tempest::StorageImage     surfCnts, surfBins;
      Tempest::StorageBuffer    surfBinsCtrl, surfList;

      Tempest::StorageBuffer    gbuffFree;
      Tempest::StorageImage     gbuffDiff, gbuffNorm, gbuffHitT;
      } surf;

    struct {
      Tempest::Attachment       frame;
      uint32_t                  numFrames = 0;
      Tempest::Matrix4x4        mvpLast;
      } pt;

    struct {
      Tempest::StorageBuffer    epipoles;
      Tempest::StorageImage     epTrace;
      } epipolar;

    const int32_t VSM_PAGE_SIZE = 128;
    struct {
      Tempest::StorageImage     pageTbl;
      Tempest::StorageImage     pageHiZ;
      Tempest::ZBuffer          pageData;
      Tempest::StorageBuffer    pageList;
      Tempest::StorageBuffer    pageListTmp;
      Tempest::StorageBuffer    pageTblOmni;
      Tempest::StorageBuffer    visibleLights;

      Tempest::StorageImage     vsmDbg;

      uint64_t                  statsTime = 0; // last OPENGOTHIC_VSM_STATS line, ms
      Tempest::StorageBuffer    lampProbe;     // light_vsm_probe: one frame's lamp filter counters, a stamp per pixel
      uint32_t                  lampStamp  = 0; // the counted frame's stamp
      bool                      lampArmed  = false; // the next drawLights counts
      bool                      lampCounted = false; // lampProbe holds a counted frame
      } vsm;

    Tempest::TextureFormat    shadowFormat  = Tempest::TextureFormat::Depth16;
    Tempest::TextureFormat    zBufferFormat = Tempest::TextureFormat::Depth16;

    Shaders&                  shaders = Shaders::inst(false);
    BloodDecals               blood;
  };
