#pragma once

#include <Tempest/RenderPipeline>
#include <Tempest/Shader>
#include <Tempest/Device>
#include <future>
#include <list>

#include "graphics/drawcommands.h"
#include "game/constants.h"
#include "material.h"

class Shaders {
  public:
    Shaders();
    ~Shaders();

    void waitCompiler();

    enum PipelineType: uint8_t {
      T_Depth,
      T_Shadow,
      T_Vsm,
      T_Main,
      };

    struct Options {
      bool     doBindless        = false;
      bool     doRtScene         = false;
      bool     doRayQuery        = false;
      bool     doMeshShading     = false;
      bool     doVirtualShadow   = false;
      bool     doMaterialMaps    = false;
      };

    static Shaders& inst(bool waitCompiler = true);
    static const Options& options();

    static bool isVsmSupported();
    static bool isLightsTreeSupported();
    static bool isGi1Supported();
    static bool isGi2Supported();

    Tempest::RenderPipeline  lights, lightsRq, lightsVsm;
    Tempest::RenderPipeline  lightsVsmHard;  // [INTERNAL] vsmLampSoft=0: lamp shadows in one gather, no PCSS
    Tempest::RenderPipeline  lightsVsmProbe; // OPENGOTHIC_VSM_LAMP and OPENGOTHIC_VSM_STATS: lamp filter study
    Tempest::RenderPipeline  directLight,  directLightSh, directLightRq;
    Tempest::RenderPipeline  ambientLight, ambientLightSsao, ambientLightSurf;

    Tempest::ComputePipeline copyBuf;
    Tempest::ComputePipeline copyImg;
    Tempest::ComputePipeline patch;
    Tempest::ComputePipeline patchCopy; // InstanceStorage history: cur -> prev by a list in the patch buffer
    Tempest::RenderPipeline  copy, downscale;
    Tempest::RenderPipeline  stash;
    Tempest::RenderPipeline  bink;

    Tempest::ComputePipeline ssao, ssaoBlur, ssaoDepth;

    Tempest::ComputePipeline irradiance;

    // Scalable and Production Ready Sky and Atmosphere
    Tempest::RenderPipeline  skyTransmittance, skyMultiScattering;
    Tempest::RenderPipeline  skyViewLut, skyViewCldLut, sky, skySep;
    Tempest::RenderPipeline  fog;
    Tempest::RenderPipeline  fog3dHQ;
    Tempest::RenderPipeline  fog3dHQSky;     // fog3dHQ with the sky seen from the camera (FOG_SKY_VIS), under GI
    Tempest::ComputePipeline fogSkyVis;      // fog_sky_vis.comp: the sky seen from the camera, with ray query

    // Blood on the GBuffer (physics stage 8, step 4c): albedo RGB, and roughness with material maps
    Tempest::RenderPipeline  bloodDecal, bloodDecalAlbedo; // with and without the ORM target
    Tempest::RenderPipeline  sun;
    Tempest::ComputePipeline cloudsLut, fogOcclusion, fogOcclusionEmpty;
    Tempest::ComputePipeline fogViewLut3d, fogViewLutSep;
    Tempest::ComputePipeline skyExposure;
    Tempest::ComputePipeline eyeMeter;

    Tempest::RenderPipeline  underwaterT, underwaterS;
    Tempest::RenderPipeline  waterReflection, waterReflectionSSR;

    Tempest::RenderPipeline  tonemapping, tonemappingUpscale;
    Tempest::RenderPipeline  motionView; // debug: gbufMotion as the frame (OPENGOTHIC_MOTION_VIEW=1)
    Tempest::RenderPipeline  reactiveView; // debug: the TAA reactive mask as the frame (OPENGOTHIC_TAA_REACTIVE_VIEW=1)
    Tempest::RenderPipeline  bloomDownKaris, bloomDown, bloomUp, bloomMix;

    // AA
    Tempest::ComputePipeline cmaa2EdgeColor2x2Presets[uint32_t(AaPreset::PRESETS_COUNT)];
    Tempest::ComputePipeline cmaa2ProcessCandidates;
    Tempest::RenderPipeline  cmaa2DeferredColorApply2x2;
    Tempest::RenderPipeline  taaResolve, taaSharpen;
    // temporal upscale (MetalFX Temporal): camera motion vectors, tonemapping with the bloom in it
    Tempest::RenderPipeline  motionCamera, tonemappingBloom, tonemappingTaa;

    // HUD

    // HiZ
    Tempest::ComputePipeline hiZPot, hiZMip;

    // Cluster
    Tempest::ComputePipeline clusterInit, clusterPatch;
    Tempest::ComputePipeline visibilityPassSh, visibilityPassHiZ, visibilityPassHiZCr;
    Tempest::ComputePipeline visibilityPassShOrd, visibilityPassHiZOrd, visibilityPassHiZCrOrd;

    // RT/RQ
    Tempest::RenderPipeline  rtDbg;
    Tempest::RenderPipeline  rtPathtrace;
    Tempest::RenderPipeline  hashDbg;

    // GI
    Tempest::RenderPipeline  probeDbg, probeHitDbg;
    Tempest::ComputePipeline probeInit, probeClear, probeClearHash, probeMakeHash;
    Tempest::ComputePipeline probeVote, probePrune, probeAlocation, probeHitAlloc, probeClearVotes;
    Tempest::ComputePipeline probeTrace, probeRelight, probeRelightNew, probeLighting, probeStats;
    Tempest::RenderPipeline  probeAmbient;

    // GI-Surf
    Tempest::RenderPipeline  surfDbg;
    Tempest::ComputePipeline surfBinClear, surfBinPass, surfBinAlloc, surfBinSort;

    Tempest::ComputePipeline surfAlloc, surfApply, surfFList;
    Tempest::ComputePipeline surfUpdate, surfCulling, surfDecimate, surfCompact;

    Tempest::ComputePipeline surfPathtrace, surfRaycast, surLighting;

    // Lights tree
    Tempest::ComputePipeline lightsMorton, lightsTopology, lightsTree, lightsBvh;
    Tempest::RenderPipeline  lightsTreeDbg;

    // Epipolar
    Tempest::ComputePipeline fogEpipolarVsm;
    Tempest::ComputePipeline fogEpipolarOcclusion;

    // Virtual shadow
    Tempest::ComputePipeline vsmVisibilityPass;
    Tempest::ComputePipeline vsmClear, vsmClearOmni, vsmCullLights, vsmMarkPages, vsmMarkOmniPages, vsmPostprocessOmni;
    Tempest::ComputePipeline vsmTrimPages, vsmSortPages, vsmListPages, vsmClumpPages, vsmAllocPages, vsmAlloc2Pages, vsmMergePages;
    Tempest::ComputePipeline vsmPackDraw0, vsmPackDraw1;
    Tempest::ComputePipeline vsmFogPages, vsmFogShadow;
    Tempest::RenderPipeline  vsmDirectLight;
    Tempest::RenderPipeline  vsmDbg;

    Tempest::ComputePipeline vsmRendering;

    // Inventory
    Tempest::RenderPipeline  inventory;

    const Tempest::RenderPipeline* materialPipeline(const Material& desc, DrawCommands::Type t, PipelineType pt, bool bindless) const;

  private:
    struct Entry {
      Tempest::RenderPipeline pipeline;
      Material::AlphaFunc     alpha        = Material::Solid;
      DrawCommands::Type      type         = DrawCommands::Static;
      PipelineType            pipelineType = PipelineType::T_Main;
      bool                    bindless     = false;
      bool                    trivial      = false;
      };

    void                     setupOptions();
    void                     compileKeyShaders();
    void                     compileShaders() noexcept;

    Tempest::RenderPipeline  postEffect(std::string_view name);
    Tempest::RenderPipeline  postEffect(std::string_view name, Tempest::RenderState::ZTestMode ztest);
    Tempest::RenderPipeline  postEffect(std::string_view vs, std::string_view fs, Tempest::RenderState::ZTestMode ztest = Tempest::RenderState::ZTestMode::LEqual);
    Tempest::ComputePipeline computeShader(std::string_view name);
    Tempest::RenderPipeline  fogShader (std::string_view name);
    Tempest::RenderPipeline  inWaterShader   (std::string_view name, bool isScattering);
    Tempest::RenderPipeline  reflectionShader(std::string_view name, bool hasMeshlets);
    Tempest::RenderPipeline  ambientLightShader(std::string_view name);
    Tempest::RenderPipeline bloodDecalShader(bool materialMaps);

    Options         opts;
    static Shaders* instance;

    std::future<void>        deferredCompilation;
    mutable std::list<Entry> materials;
  };
