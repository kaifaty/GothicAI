#include "shaders.h"

#include <Tempest/Application>
#include <Tempest/Device>
#include <Tempest/Log>
#include <cassert>

#include "gothic.h"
#include "commandline.h"
#include "resources.h"
#include "loadprobe.h"

#include "shader.h"
#include "utils/string_frm.h"

using namespace Tempest;

static bool hasBindless() {
  const auto& p = Resources::device().properties();
  if(p.descriptors.nonUniformIndexing && p.descriptors.maxTexture>=65000 && p.descriptors.maxStorage>=65000)
    return true;
  return false;
  }

Shaders* Shaders::instance = nullptr;

Shaders::Shaders() {
  instance = this;
  setupOptions();
  compileKeyShaders();
  deferredCompilation = std::async(std::launch::async, [this]() {
    Workers::setThreadName("Shader compilation");
    auto time = Application::tickCount();
    compileShaders();
    time = Application::tickCount() - time;
    if(time>1000)
      Log::i("Shader compilation took: ",time/1000," seconds");
    if(LoadProbe::enabled())
      Log::i("[loadprobe-shaders] compile_ms=",time);
    });
  }

Shaders::~Shaders() {
  deferredCompilation.wait();
  instance = nullptr;
  }

void Shaders::waitCompiler() {
  // Load probe: how long a caller (the world load) actually waited for the background compilation.
  if(LoadProbe::enabled() && deferredCompilation.valid() &&
     deferredCompilation.wait_for(std::chrono::seconds(0))!=std::future_status::ready) {
    const auto start = Application::tickCount();
    deferredCompilation.wait();
    Log::i("[loadprobe-shaders] wait_ms=",Application::tickCount()-start);
    return;
    }
  deferredCompilation.wait();
  }

Shaders& Shaders::inst(bool waitCompiler) {
  if(waitCompiler)
    instance->waitCompiler();
  return *instance;
  }

const Shaders::Options& Shaders::options() {
  assert(instance!=nullptr);
  return instance->opts;
  }

void Shaders::setupOptions() {
  auto& gpu = Resources::device().properties();
  opts.doBindless       = hasBindless() && CommandLine::inst().isBindless();
  opts.doRtScene        = gpu.raytracing.rayQuery && CommandLine::inst().isRayQuery();
  opts.doRayQuery       = opts.doRtScene && opts.doBindless;
  opts.doMeshShading    = gpu.meshlets.meshShader && CommandLine::inst().isMeshShading();
  opts.doVirtualShadow  = Shaders::isVsmSupported()  && CommandLine::inst().isVirtualShadow();
  opts.doMaterialMaps   = Resources::materialMapsEnabled();
  }

void Shaders::compileKeyShaders() {
  bink      = postEffect("bink");
  downscale = postEffect("downscale");
  }

void Shaders::compileShaders() noexcept {
  auto& device = Resources::device();
  const char* materialMaps = opts.doMaterialMaps ? "_orm" : "";

  copyBuf   = computeShader("copy.comp.sprv");
  copyImg   = computeShader("copy_img.comp.sprv");
  copy      = postEffect("copy");

  patch     = computeShader("patch.comp.sprv");
  patchCopy = computeShader("patch_copy.comp.sprv");

  stash     = postEffect("stash");

  clusterInit         = computeShader("cluster_init.comp.sprv");
  clusterPatch        = computeShader("cluster_patch.comp.sprv");
  visibilityPassSh    = computeShader("visibility_pass.comp.sprv");
  visibilityPassHiZ   = computeShader("visibility_pass_hiz.comp.sprv");
  visibilityPassHiZCr = computeShader("visibility_pass_hiz_cr.comp.sprv");
  visibilityPassShOrd    = computeShader("visibility_pass_ordered.comp.sprv");
  visibilityPassHiZOrd   = computeShader("visibility_pass_hiz_ordered.comp.sprv");
  visibilityPassHiZCrOrd = computeShader("visibility_pass_hiz_cr_ordered.comp.sprv");

  ssao                = computeShader("ssao.comp.sprv");
  ssaoBlur            = computeShader("ssao_blur.comp.sprv");
  ssaoDepth           = computeShader("ssao_depth.comp.sprv");

  bloodDecal       = bloodDecalShader(opts.doMaterialMaps);
  bloodDecalAlbedo = bloodDecalShader(false);

  directLight      = postEffect(string_frm("direct_light", materialMaps),    RenderState::ZTestMode::NoEqual);
  directLightSh    = postEffect(string_frm("direct_light_sh", materialMaps), RenderState::ZTestMode::NoEqual);
  if(opts.doRayQuery && device.properties().descriptors.nonUniformIndexing)
    directLightRq  = postEffect(string_frm("direct_light_rq", materialMaps), RenderState::ZTestMode::NoEqual);

  ambientLight       = ambientLightShader(string_frm("ambient_light", materialMaps));
  ambientLightSsao   = ambientLightShader(string_frm("ambient_light_ssao", materialMaps));
  ambientLightSurf   = ambientLightShader(string_frm("ambient_light_surf", materialMaps));

  irradiance         = computeShader("irradiance.comp.sprv");
  cloudsLut          = computeShader("clouds_lut.comp.sprv");
  skyTransmittance   = postEffect("sky_transmittance");
  skyMultiScattering = postEffect("sky_multi_scattering");
  skyViewLut         = postEffect("sky_view_lut");
  skyViewCldLut      = postEffect("sky_view_clouds_lut");

  fogViewLut3d       = computeShader("fog_view_lut.comp.sprv");
  fogViewLutSep      = computeShader("fog_view_lut_sep.comp.sprv");
  fogOcclusion       = computeShader("fog3d.comp.sprv");
  fogOcclusionEmpty  = computeShader("fog3d_empty.comp.sprv");

  skyExposure        = computeShader("sky_exposure.comp.sprv");
  sky                = postEffect("sky");
  skySep             = postEffect("sky_sep");
  fog                = fogShader ("fog");
  fog3dHQ            = fogShader ("fog3d_hq");

  underwaterT        = inWaterShader("underwater_t", false);
  underwaterS        = inWaterShader("underwater_s", true);
  waterReflection    = reflectionShader("water_reflection.frag.sprv", opts.doMeshShading);
  waterReflectionSSR = reflectionShader("water_reflection_ssr.frag.sprv", opts.doMeshShading);

  {
  RenderState state;
  state.setCullFaceMode (RenderState::CullMode::Front);
  state.setBlendSource  (RenderState::BlendMode::One);
  state.setBlendDest    (RenderState::BlendMode::SrcAlpha);
  state.setZTestMode    (RenderState::ZTestMode::Equal);
  state.setZWriteEnabled(false);

  auto sh      = GothicShader::get("sun.vert.sprv");
  auto vsLight = device.shader(sh.data,sh.len);
  sh           = GothicShader::get("sun.frag.sprv");
  auto fsLight = device.shader(sh.data,sh.len);
  sun          = device.pipeline(Triangles, state, vsLight, fsLight);
  }

  {
  RenderState state;
  state.setCullFaceMode (RenderState::CullMode::Front);
  state.setBlendSource  (RenderState::BlendMode::One);
  state.setBlendDest    (RenderState::BlendMode::One);
  state.setZTestMode    (RenderState::ZTestMode::Less);

  state.setZWriteEnabled(false);

  auto sh      = GothicShader::get("light.vert.sprv");
  auto vsLight = device.shader(sh.data,sh.len);
  sh           = GothicShader::get(string_frm("light", materialMaps, ".frag.sprv"));
  auto fsLight = device.shader(sh.data,sh.len);
  lights       = device.pipeline(Triangles, state, vsLight, fsLight);
  if(opts.doRtScene) {
    if(opts.doRayQuery) {
      sh      = GothicShader::get(string_frm("light_rq_at", materialMaps, ".frag.sprv"));
      fsLight = device.shader(sh.data,sh.len);
      } else {
      sh      = GothicShader::get(string_frm("light_rq", materialMaps, ".frag.sprv"));
      fsLight = device.shader(sh.data,sh.len);
      }
    lightsRq = device.pipeline(Triangles, state, vsLight, fsLight);
    }
  if(Shaders::isVsmSupported()) {
    sh      = GothicShader::get(string_frm("light_vsm", materialMaps, ".frag.sprv"));
    fsLight = device.shader(sh.data,sh.len);
    lightsVsm = device.pipeline(Triangles, state, vsLight, fsLight);
    sh      = GothicShader::get(string_frm("light_vsm_hard", materialMaps, ".frag.sprv"));
    fsLight = device.shader(sh.data,sh.len);
    lightsVsmHard = device.pipeline(Triangles, state, vsLight, fsLight);
    sh      = GothicShader::get(string_frm("light_vsm_probe", materialMaps, ".frag.sprv"));
    fsLight = device.shader(sh.data,sh.len);
    lightsVsmProbe = device.pipeline(Triangles, state, vsLight, fsLight);
    }
  }

  tonemapping        = postEffect("triangle_uv", "tonemapping",    RenderState::ZTestMode::Always);
  motionView         = postEffect("triangle_uv", "motion_view",    RenderState::ZTestMode::Always);
  tonemappingUpscale = postEffect("triangle_uv", "tonemapping_up", RenderState::ZTestMode::Always);
  bloomDownKaris     = postEffect("triangle_uv", "bloom_down_karis", RenderState::ZTestMode::Always);
  bloomDown          = postEffect("triangle_uv", "bloom_down",       RenderState::ZTestMode::Always);
  {
    // up the chain onto the larger level (One, One); into the scene as scene*(1-k) + bloom*k (One, SrcAlpha)
    auto blended = [&](std::string_view fs, RenderState::BlendMode dst) {
      RenderState state;
      state.setZTestMode    (RenderState::ZTestMode::Always);
      state.setZWriteEnabled(false);
      state.setBlendSource  (RenderState::BlendMode::One);
      state.setBlendDest    (dst);
      auto sh = GothicShader::get("triangle_uv.vert.sprv");
      auto vs = device.shader(sh.data,sh.len);
      sh      = GothicShader::get(string_frm(fs,".frag.sprv"));
      auto ps = device.shader(sh.data,sh.len);
      return device.pipeline(Triangles,state,vs,ps);
      };
    bloomUp  = blended("bloom_up",  RenderState::BlendMode::One);
    bloomMix = blended("bloom_mix", RenderState::BlendMode::SrcAlpha);
  }

  cmaa2EdgeColor2x2Presets[uint32_t(AaPreset::OFF)]    = Tempest::ComputePipeline();
  cmaa2EdgeColor2x2Presets[uint32_t(AaPreset::MEDIUM)] = computeShader("cmaa2_edges_color2x2_quality_0.comp.sprv");
  cmaa2EdgeColor2x2Presets[uint32_t(AaPreset::ULTRA)]  = computeShader("cmaa2_edges_color2x2_quality_1.comp.sprv");

  cmaa2ProcessCandidates = computeShader("cmaa2_process_candidates.comp.sprv");
#if defined(__APPLE__)
  // the Mac is always Metal: half colours, -14% of the pass; Vulkan would need shaderFloat16
  taaResolve             = postEffect("triangle", "taa_resolve_h", RenderState::ZTestMode::Always);
#else
  taaResolve             = postEffect("triangle", "taa_resolve", RenderState::ZTestMode::Always);
#endif
  taaSharpen             = postEffect("triangle", "taa_sharpen", RenderState::ZTestMode::Always);
  motionCamera           = postEffect("triangle",    "motion_camera",     RenderState::ZTestMode::Always);
  tonemappingBloom       = postEffect("triangle_uv", "tonemapping_bloom", RenderState::ZTestMode::Always);
  tonemappingTaa         = postEffect("triangle_uv", "tonemapping_taa",   RenderState::ZTestMode::Always);
  {
    auto sh = GothicShader::get("cmaa2_deferred_color_apply_2x2.vert.sprv");
    auto vs = device.shader(sh.data,sh.len);
    sh = GothicShader::get("cmaa2_deferred_color_apply_2x2.frag.sprv");
    auto fs = device.shader(sh.data,sh.len);
    cmaa2DeferredColorApply2x2 = device.pipeline(Tempest::Points,RenderState(),vs,fs);
  }

  hiZPot  = computeShader("hiz_pot.comp.sprv");
  hiZMip  = computeShader("hiz_mip.comp.sprv");

  if(opts.doRayQuery) {
    rtDbg       = postEffect("triangle_uv", "rt_dbg", RenderState::ZTestMode::NoEqual);
    }
  hashDbg = postEffect("triangle_uv", "hash_dbg");

  if(opts.doRayQuery) {
    RenderState state;
    state.setZTestMode    (RenderState::ZTestMode::Always);
    state.setZWriteEnabled(true);
    state.setBlendSource  (RenderState::BlendMode::SrcAlpha);
    state.setBlendDest    (RenderState::BlendMode::OneMinusSrcAlpha);

    auto sh = GothicShader::get(string_frm("triangle_uv",".vert.sprv"));
    auto vs = device.shader(sh.data,sh.len);

    sh      = GothicShader::get(string_frm("pathtrace",".frag.sprv"));
    auto fs = device.shader(sh.data,sh.len);
    rtPathtrace = device.pipeline(Triangles,state,vs,fs);
    }

  if(isGi1Supported()) {
    RenderState state;
    state.setCullFaceMode(RenderState::CullMode::NoCull);
    state.setZTestMode   (RenderState::ZTestMode::Less);

    auto sh = GothicShader::get("probe_dbg.vert.sprv");
    auto vs = device.shader(sh.data,sh.len);
    sh = GothicShader::get("probe_dbg.frag.sprv");
    auto fs = device.shader(sh.data,sh.len);
    probeDbg = device.pipeline(Triangles,state,vs,fs);

    sh = GothicShader::get("probe_hit_dbg.vert.sprv");
    vs = device.shader(sh.data,sh.len);
    sh = GothicShader::get("probe_hit_dbg.frag.sprv");
    fs = device.shader(sh.data,sh.len);
    probeHitDbg = device.pipeline(Triangles,state,vs,fs);

    probeInit      = computeShader("probe_init.comp.sprv");
    probeClear     = computeShader("probe_clear.comp.sprv");
    probeClearHash = computeShader("probe_clear_hash.comp.sprv");
    probeMakeHash  = computeShader("probe_make_hash.comp.sprv");
    probeVote      = computeShader("probe_vote.comp.sprv");
    probePrune     = computeShader("probe_prune.comp.sprv");
    probeAlocation = computeShader("probe_allocation.comp.sprv");
    probeHitAlloc  = computeShader("probe_hit_alloc.comp.sprv");
    probeClearVotes= computeShader("probe_clear_votes.comp.sprv");
    probeTrace     = computeShader("probe_trace.comp.sprv");
    probeRelight   = computeShader("probe_relight.comp.sprv");
    probeRelightNew= computeShader("probe_relight_new.comp.sprv");
    probeLighting  = computeShader(string_frm("probe_lighting", materialMaps, ".comp.sprv"));
    probeStats     = computeShader("probe_stats.comp.sprv");

    state.setBlendSource  (RenderState::BlendMode::One);
    state.setBlendDest    (RenderState::BlendMode::SrcAlpha);  // for debugging
    state.setZTestMode    (RenderState::ZTestMode::NoEqual);
    state.setZWriteEnabled(false);
    sh = GothicShader::get("probe_ambient.vert.sprv");
    vs = device.shader(sh.data,sh.len);
    sh = GothicShader::get(string_frm("probe_ambient", materialMaps, ".frag.sprv"));
    fs = device.shader(sh.data,sh.len);
    probeAmbient = device.pipeline(Triangles,state,vs,fs);
    }

  if(isGi2Supported()) {
    RenderState state;
    state.setCullFaceMode(RenderState::CullMode::NoCull);
    state.setZTestMode   (RenderState::ZTestMode::Less);
    state.setZWriteEnabled(false);

    auto sh = GothicShader::get("surf_dbg.vert.sprv");
    auto vs = device.shader(sh.data,sh.len);
    sh = GothicShader::get("surf_dbg.frag.sprv");
    auto fs = device.shader(sh.data,sh.len);
    surfDbg = device.pipeline(Triangles,state,vs,fs);

    surfBinPass  = computeShader("surf_bin_pass.comp.sprv");
    surfBinClear = computeShader("surf_bin_clear.comp.sprv");
    surfBinAlloc = computeShader("surf_bin_alloc.comp.sprv");
    surfBinSort  = computeShader("surf_bin_sort.comp.sprv");

    surfUpdate   = computeShader("surf_update.comp.sprv");
    surfCulling  = computeShader("surf_culling.comp.sprv");
    surfDecimate = computeShader("surf_decimate.comp.sprv");
    surfCompact  = computeShader("surf_compact.comp.sprv");

    surfAlloc    = computeShader("surf_alloc.comp.sprv");
    surfApply    = computeShader("surf_apply.comp.sprv");
    surfFList    = computeShader("surf_freelist.comp.sprv");

    surfPathtrace = computeShader("surf_pathtrace.comp.sprv");
    surfRaycast   = computeShader("surf_raycast.comp.sprv");
    surLighting   = computeShader("surf_lighting.comp.sprv");
    }

  if(Shaders::isLightsTreeSupported()) {
    // the sort in 32 KB of shared memory where a workgroup has more (Metal gives 32 KB in all)
    const bool sharedSort = Resources::device().properties().compute.maxSharedMemory>=33*1024;
    lightsMorton   = computeShader(sharedSort ? "lights_morton_shared.comp.sprv" : "lights_morton.comp.sprv");
    lightsTopology = computeShader("lights_topology.comp.sprv");
    lightsTree     = computeShader("lights_tree.comp.sprv");
    lightsBvh      = computeShader("lights_bvh.comp.sprv");

    lightsTreeDbg    = postEffect("triangle_uv", "lightstree_dbg");
    }

  if(Shaders::isVsmSupported()) {
    fogEpipolarOcclusion = computeShader("fog_epipolar_occlusion.comp.sprv");
    fogEpipolarVsm       = computeShader("fog_epipolar_vsm.comp.sprv");
    }

  if(Shaders::isVsmSupported()) {
    vsmVisibilityPass  = computeShader("vsm_visibility_pass.comp.sprv");
    vsmClear           = computeShader("vsm_clear.comp.sprv");
    vsmClearOmni       = computeShader("vsm_clear_omni.comp.sprv");
    vsmCullLights      = computeShader("vsm_cull_lights.comp.sprv");
    vsmMarkPages       = computeShader("vsm_mark_pages.comp.sprv");
    vsmMarkOmniPages   = computeShader("vsm_mark_omni_pages.comp.sprv");
    vsmPostprocessOmni = computeShader("vsm_postprocess_omni.comp.sprv");
    vsmTrimPages       = computeShader("vsm_trim_pages.comp.sprv");
    vsmClumpPages      = computeShader("vsm_clump_pages.comp.sprv");
    vsmListPages       = computeShader("vsm_list_pages.comp.sprv");
    vsmSortPages       = computeShader("vsm_sort_pages.comp.sprv");
    vsmAllocPages      = computeShader("vsm_alloc_pages.comp.sprv");
    vsmAlloc2Pages     = computeShader("vsm_alloc_pages2.comp.sprv");
    vsmMergePages      = computeShader("vsm_merge_pages.comp.sprv");
    vsmPackDraw0       = computeShader("vsm_pack_draws0.comp.sprv");
    vsmPackDraw1       = computeShader("vsm_pack_draws1.comp.sprv");
    vsmFogPages        = computeShader("vsm_fog_mark_pages.comp.sprv");
    vsmFogShadow       = computeShader("vsm_fog_shadow.comp.sprv");

    vsmDirectLight     = postEffect(string_frm("direct_light_vsm", materialMaps), RenderState::ZTestMode::NoEqual);
    vsmDbg             = postEffect("vsm_dbg", RenderState::ZTestMode::Always);
    vsmRendering       = computeShader("vsm_rendering.comp.sprv");
    }

  {
    RenderState state;
    state.setCullFaceMode(RenderState::CullMode::Front);
    state.setZTestMode   (RenderState::ZTestMode::LEqual);

    auto sh = GothicShader::get("item.vert.sprv");
    auto vs = device.shader(sh.data,sh.len);
    sh = GothicShader::get("item.frag.sprv");
    auto fs = device.shader(sh.data,sh.len);
    inventory = device.pipeline(Triangles,state,vs,fs);
  }
  }

bool Shaders::isVsmSupported() {
  auto& gpu = Resources::device().properties();
  if(gpu.compute.maxInvocations>=1024 && gpu.render.maxClipCullDistances>=4 &&
     gpu.render.maxViewportSize.w>=8192 && gpu.render.maxViewportSize.h>=8192) {
    return true;
    }
  return false;
  }

bool Shaders::isLightsTreeSupported() {
  auto& gpu = Resources::device().properties();
  if(gpu.compute.maxInvocations<512 || gpu.compute.maxSharedMemory<16*1024)
    return false;
  return true;
  }

bool Shaders::isGi1Supported() {
  // Not checked: the subgroup operations of probe_ambient.frag (subgroupAllEqual, subgroupBallot, subgroupShuffle in
  // the fragment stage, SPIR-V 1.3): Tempest does not report the supported stages and operations of subgroups. Run on
  // an RTX 3080 (Vulkan) and an M1 Pro (Metal); a GPU with ray query but without them is not covered (step-2.md, «2а»).
  auto& gpu = Resources::device().properties();
  if(!gpu.raytracing.rayQuery || !isLightsTreeSupported())
    return false;
  if(!gpu.hasStorageFormat(RGBA16F) || !gpu.hasStorageFormat(R32U) || !gpu.hasStorageFormat(R16))
    return false;
  if(gpu.tex2d.maxSize<4096 || gpu.compute.maxInvocations<256 || !gpu.descriptors.nonUniformIndexing)
    return false;
  return true;
  }

bool Shaders::isGi2Supported() {
  auto& gpu = Resources::device().properties();
  if(!gpu.raytracing.rayQuery)
    return false; //TODO: splatting
  if(gpu.compute.maxInvocations<256 || !gpu.descriptors.nonUniformIndexing)
    return false;
  if(!gpu.hasStorageFormat(RGBA16F))
    return false;
  return true;
  }

const RenderPipeline* Shaders::materialPipeline(const Material& mat, DrawCommands::Type t, PipelineType pt, bool bl) const {
  if(t==DrawCommands::Static) {
    // same shader
    t = DrawCommands::Movable;
    }

  if(pt!=PipelineType::T_Main && !mat.isSolid()) {
    return nullptr;
    }
  if(pt!=PipelineType::T_Main && mat.alpha==Material::Water) {
    return nullptr;
    }

  const auto alpha   = (mat.isGhost ? Material::Ghost : mat.alpha);
  const bool trivial = (!mat.hasUvAnimation() && alpha==Material::Solid && t==DrawCommands::Landscape);

  for(auto& i:materials) {
    if(i.alpha==alpha && i.type==t && i.pipelineType==pt && i.bindless==bl && i.trivial==trivial)
      return &i.pipeline;
    }

  RenderState state;
  state.setCullFaceMode(RenderState::CullMode::Front);
  state.setZTestMode   (RenderState::ZTestMode::LEqual);
  //state.setZTestMode   (RenderState::ZTestMode::Less);

  if(pt==PipelineType::T_Shadow || pt==PipelineType::T_Vsm) {
    state.setZTestMode(RenderState::ZTestMode::Greater); //FIXME
    }
  if(pt==PipelineType::T_Vsm) {
    // state.setZTestMode(RenderState::ZTestMode::Always); //FIXME
    state.setZTestMode(RenderState::ZTestMode::Greater); //FIXME
    }

  switch(alpha) {
    case Material::Solid:
    case Material::AlphaTest:
    case Material::Water:
    case Material::Ghost:
      break;
    case Material::Transparent:
      state.setBlendSource  (RenderState::BlendMode::SrcAlpha); // premultiply in shader
      state.setBlendDest    (RenderState::BlendMode::OneMinusSrcAlpha);
      state.setZWriteEnabled(false);
      break;
    case Material::AdditiveLight:
      state.setBlendSource  (RenderState::BlendMode::SrcAlpha);
      state.setBlendDest    (RenderState::BlendMode::One);
      state.setZWriteEnabled(false);
      break;
    case Material::Multiply:
    case Material::Multiply2:
      state.setBlendSource  (RenderState::BlendMode::DstColor);
      state.setBlendDest    (RenderState::BlendMode::SrcColor);
      state.setZWriteEnabled(false);
      break;
    }

  static bool overdrawDbg = false;
  if(overdrawDbg &&
      (alpha==Material::Solid || alpha==Material::AlphaTest) && t!=DrawCommands::Landscape && pt!=T_Shadow) {
    state.setBlendSource(RenderState::BlendMode::One);
    state.setBlendDest  (RenderState::BlendMode::One);
    state.setZWriteEnabled(false);
    state.setZTestMode(RenderState::ZTestMode::Always);
    }

  const char* vsTok = "";
  const char* fsTok = "";

  switch(t) {
    case DrawCommands::Landscape:
      vsTok = "lnd";
      fsTok = "lnd";
      break;
    case DrawCommands::Static:
    case DrawCommands::Movable:
      vsTok = "obj";
      fsTok = "obj";
      break;
    case DrawCommands::Animated:
      vsTok = "ani";
      fsTok = "obj";
      break;
    case DrawCommands::Pfx:
      vsTok = "pfx";
      fsTok = "pfx";
      break;
    case DrawCommands::Morph:
      vsTok = "mph";
      fsTok = "obj";
      break;
    }

  const char* typeVsM = nullptr;
  const char* typeVsD = nullptr;
  const char* typeVsV = nullptr;
  const char* typeFsM = nullptr;
  const char* typeFsD = nullptr;
  const char* typeFsV = nullptr;

  // the G-buffer pass of objects has vertex variants of its own: they also compute the previous vertex for motion
  // vectors (main_obj_g, main_ani_g, main_mph_g); landscape and particles write no motion of their own
  const char* gbufVs = (t==DrawCommands::Landscape || t==DrawCommands::Pfx) ? "" : "_g";
  switch(alpha) {
    case Material::Solid:
      typeVsM = gbufVs;
      typeFsM = "_g";
      typeVsD = "_d";
      typeFsD = "_d";
      typeVsV = "_v";
      typeFsV = "_v";
      if(trivial)
        typeFsM = "_g_s";
      break;
    case Material::AlphaTest:
      typeVsM = gbufVs;
      typeFsM = "_g_at";
      typeVsD = "_d_at";
      typeFsD = "_d_at";
      typeVsV = "_v_at";
      typeFsV = "_v_at";
      break;
    case Material::Transparent:
      typeVsM = "_f";
      typeFsM = "_f";
      break;
    case Material::Multiply:
    case Material::Multiply2:
      typeVsM = "";
      typeFsM = "_m";
      break;
    case Material::AdditiveLight:
      typeVsM = "";
      typeFsM = "_e";
      break;
    case Material::Water:
      typeVsM = "_f";
      typeFsM = "_w";
      break;
    case Material::Ghost:
      typeVsM = "";
      typeFsM = "_x";
      break;
    }

  const char* typeVs = "";
  const char* typeFs = "";
  switch(pt) {
    case T_Shadow:
    case T_Depth:
      typeVs = typeVsD;
      typeFs = typeFsD;
      break;
    case T_Vsm:
      typeVs = typeVsV;
      typeFs = typeFsV;
      break;
    case T_Main:
      typeVs = typeVsM;
      typeFs = typeFsM;
      break;
    }

  if(typeVs==nullptr || typeFs==nullptr)
    return nullptr;

  const char* bindless = bl ? "_bindless" : "_slot";
  const bool mapsMain = pt==T_Main && (alpha==Material::Solid || alpha==Material::AlphaTest);
  const bool mapsDepth = (pt==T_Depth || pt==T_Shadow) && alpha==Material::AlphaTest;
  const char* materialMaps = opts.doMaterialMaps && (mapsMain || mapsDepth) ? "_orm" : "";

  materials.emplace_front();
  auto& b = materials.front();
  b.alpha        = alpha;
  b.type         = t;
  b.pipelineType = pt;
  b.bindless     = bl;
  b.trivial      = trivial;

  auto& device = Resources::device();
  if(mat.isTesselated() && device.properties().tesselationShader && t==DrawCommands::Landscape && true) {
    auto shVs = GothicShader::get(string_frm("main_", vsTok, typeVs, bindless, ".vert.sprv"));
    auto shTc = GothicShader::get(string_frm("main_", vsTok, typeVs, bindless, ".tesc.sprv"));
    auto shTe = GothicShader::get(string_frm("main_", vsTok, typeVs, bindless, ".tese.sprv"));
    auto shFs = GothicShader::get(string_frm("main_", fsTok, typeFs, bindless, ".frag.sprv"));

    auto vs = device.shader(shVs.data,shVs.len);
    auto tc = device.shader(shTc.data,shTc.len);
    auto te = device.shader(shTe.data,shTe.len);
    auto fs = device.shader(shFs.data,shFs.len);
    b.pipeline = device.pipeline(Triangles, state, vs, tc, te, fs);
    }
  else if(options().doMeshShading && t!=DrawCommands::Pfx) {
    auto shMs = GothicShader::get(string_frm("main_", vsTok, typeVs, materialMaps, bindless, ".mesh.sprv"));
    auto shFs = GothicShader::get(string_frm("main_", fsTok, typeFs, materialMaps, bindless, ".frag.sprv"));

    auto ms = device.shader(shMs.data,shMs.len);
    auto fs = device.shader(shFs.data,shFs.len);
    b.pipeline = device.pipeline(state, Shader(), ms, fs);
    }
  else if(t!=DrawCommands::Pfx) {
    auto shVs = GothicShader::get(string_frm("main_", vsTok, typeVs, materialMaps, bindless, ".vert.sprv"));
    auto shFs = GothicShader::get(string_frm("main_", fsTok, typeFs, materialMaps, bindless, ".frag.sprv"));

    auto vs = device.shader(shVs.data,shVs.len);
    auto fs = device.shader(shFs.data,shFs.len);
    b.pipeline = device.pipeline(Triangles, state, vs, fs);
    }
  else {
    auto shVs = GothicShader::get(string_frm("main_", vsTok, typeVs, materialMaps, ".vert.sprv"));
    auto shFs = GothicShader::get(string_frm("main_", fsTok, typeFs, materialMaps, ".frag.sprv"));

    auto vs = device.shader(shVs.data,shVs.len);
    auto fs = device.shader(shFs.data,shFs.len);
    b.pipeline = device.pipeline(Triangles, state, vs, fs);
    }

  return &b.pipeline;
  }

RenderPipeline Shaders::postEffect(std::string_view name) {
  return postEffect("triangle",name);
  }

RenderPipeline Shaders::postEffect(std::string_view name, Tempest::RenderState::ZTestMode ztest) {
  return postEffect("triangle",name,ztest);
  }

RenderPipeline Shaders::postEffect(std::string_view vsName, std::string_view fsName, Tempest::RenderState::ZTestMode ztest) {
  auto& device = Resources::device();

  RenderState stateFsq;
  stateFsq.setZTestMode    (ztest);
  stateFsq.setZWriteEnabled(false);

  auto sh = GothicShader::get(string_frm(vsName,".vert.sprv"));
  auto vs = device.shader(sh.data,sh.len);

  sh      = GothicShader::get(string_frm(fsName,".frag.sprv"));
  auto fs = device.shader(sh.data,sh.len);
  return device.pipeline(Triangles,stateFsq,vs,fs);
  }

ComputePipeline Shaders::computeShader(std::string_view name) {
  auto& device = Resources::device();
  auto  sh     = GothicShader::get(name);
  return device.pipeline(device.shader(sh.data,sh.len));
  }

RenderPipeline Shaders::fogShader(std::string_view name) {
  auto& device = Resources::device();
  const bool fogDbg = false;

  RenderState state;
  state.setZWriteEnabled(false);
  state.setBlendSource  (RenderState::BlendMode::One);
  if(!fogDbg) {
    state.setBlendDest(RenderState::BlendMode::OneMinusSrcAlpha);
    }

  auto sh = GothicShader::get("triangle.vert.sprv");
  auto vs = device.shader(sh.data,sh.len);

  sh      = GothicShader::get(string_frm(name,".frag.sprv"));
  auto fs = device.shader(sh.data,sh.len);
  return device.pipeline(Triangles,state,vs,fs);
  }

RenderPipeline Shaders::bloodDecalShader(bool materialMaps) {
  auto& device = Resources::device();

  // No depth attachment: the fragment shader projects the box onto the scene depth itself.
  // The box is drawn from inside (front faces culled) so it works with the camera in it too.
  // One multiplying blend for both targets, dst*(1-a + src): the shader writes a*multiplier, so the
  // surface shows through the blood (albedo) and the wet film scales its roughness down (ORM G).
  RenderState state;
  state.setZTestMode    (RenderState::ZTestMode::Always);
  state.setZWriteEnabled(false);
  state.setCullFaceMode (RenderState::CullMode::Back);
  state.setBlendSource  (RenderState::BlendMode::DstColor);
  state.setBlendDest    (RenderState::BlendMode::OneMinusSrcAlpha);
  state.setColorWriteMask(0, RenderState::RGB); // albedo alpha holds the material hint bits
  if(materialMaps)
    state.setColorWriteMask(1, RenderState::G);  // roughness only: occlusion, metal and the map flag stay

  auto sh = GothicShader::get("blood_decal.vert.sprv");
  auto vs = device.shader(sh.data,sh.len);

  sh      = GothicShader::get(materialMaps ? "blood_decal_orm.frag.sprv" : "blood_decal.frag.sprv");
  auto fs = device.shader(sh.data,sh.len);
  return device.pipeline(Triangles,state,vs,fs);
  }

RenderPipeline Shaders::inWaterShader(std::string_view name, bool isScattering) {
  auto& device = Resources::device();

  RenderState state;
  state.setZWriteEnabled(false);

  if(isScattering) {
    state.setBlendSource(RenderState::BlendMode::One);
    state.setBlendDest  (RenderState::BlendMode::One);
    } else {
    state.setBlendSource(RenderState::BlendMode::Zero);
    state.setBlendDest  (RenderState::BlendMode::SrcColor);
    }

  auto sh = GothicShader::get("triangle.vert.sprv");
  auto vs = device.shader(sh.data,sh.len);

  sh      = GothicShader::get(string_frm(name,".frag.sprv"));
  auto fs = device.shader(sh.data,sh.len);
  return device.pipeline(Triangles,state,vs,fs);
  }

RenderPipeline Shaders::reflectionShader(std::string_view name, bool hasMeshlets) {
  auto& device = Resources::device();

  RenderState state;
  state.setZTestMode    (RenderState::ZTestMode::LEqual);
  state.setZWriteEnabled(false);
  state.setBlendSource  (RenderState::BlendMode::One);
  state.setBlendDest    (RenderState::BlendMode::One);

  auto sh = GothicShader::get("triangle.vert.sprv");
  auto vs = device.shader(sh.data,sh.len);
  sh      = GothicShader::get(name);
  auto fs = device.shader(sh.data,sh.len);

  if(hasMeshlets) {
    sh = GothicShader::get("water_reflection.mesh.sprv");
    vs = device.shader(sh.data,sh.len);
    }

  return device.pipeline(Triangles, state, vs, fs);
  }

RenderPipeline Shaders::ambientLightShader(std::string_view name) {
  auto& device = Resources::device();

  RenderState state;
  state.setBlendSource  (RenderState::BlendMode::One);
  state.setBlendDest    (RenderState::BlendMode::SrcAlpha); // debug
  state.setZTestMode    (RenderState::ZTestMode::NoEqual);
  state.setZWriteEnabled(false);

  auto sh = GothicShader::get("triangle.vert.sprv");
  auto vs = device.shader(sh.data,sh.len);
  sh      = GothicShader::get(string_frm(name,".frag.sprv"));
  auto fs = device.shader(sh.data,sh.len);

  return device.pipeline(Triangles, state, vs, fs);
  }
