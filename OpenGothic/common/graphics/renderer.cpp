#include "renderer.h"
#include "renderer_internal.h"
#include "physics/cloth.h"

#include <Tempest/Application>
#include <Tempest/Color>
#include <Tempest/Fence>
#include <Tempest/Log>
#include <Tempest/StorageImage>
#include <cassert>
#include <cmath>
#include <cstdio>

#include "ui/inventorymenu.h"
#include "ui/videowidget.h"
#include "graphics/drawcommands.h"
#include "camera.h"
#include "frameprofile.h"
#include "gothic.h"
#include "utils/string_frm.h"

using namespace Tempest;
using RendererInternal::nextPot;

uint32_t RendererInternal::nextPot(uint32_t x) {
  x--;
  x |= x >> 1;
  x |= x >> 2;
  x |= x >> 4;
  x |= x >> 8;
  x |= x >> 16;
  x++;
  return x;
  }

Renderer::Renderer() {
  auto& device = Resources::device();

  static const TextureFormat sfrm[] = {
    TextureFormat::Depth16,
    TextureFormat::Depth24x8,
    TextureFormat::Depth32F,
    };

  for(auto& i:sfrm) {
    if(device.properties().hasDepthFormat(i) && device.properties().hasSamplerFormat(i)) {
      shadowFormat = i;
      break;
      }
    }

  static const TextureFormat zfrm[] = {
    TextureFormat::Depth32F,
    TextureFormat::Depth24x8,
    TextureFormat::Depth16,
    };
  for(auto& i:zfrm) {
    if(device.properties().hasDepthFormat(i) && device.properties().hasSamplerFormat(i)){
      zBufferFormat = i;
      break;
      }
    }

  // crappy rasbery-pi like hardware
  if(!device.properties().hasStorageFormat(sky.lutRGBAFormat))
    sky.lutRGBAFormat = Tempest::TextureFormat::RGBA8;
  if(!device.properties().hasStorageFormat(sky.lutRGBFormat))
    sky.lutRGBFormat = Tempest::TextureFormat::RGBA8;

  Log::i("GPU = ",device.properties().name);
  Log::i("Depth format = ", Tempest::formatName(zBufferFormat), " Shadow format = ", Tempest::formatName(shadowFormat));

  Gothic::inst().onSettingsChanged.bind(this,&Renderer::setupSettings);
  Gothic::inst().toggleGi  .bind(this, &Renderer::toggleGi);
  Gothic::inst().toggleVsm .bind(this, &Renderer::toggleVsm);

  Gothic::inst().togglePathtrace.bind(this, &Renderer::togglePathtrace);

  settings.giMethod         = Gothic::options().doGi;
  settings.vsmEnabled       = Shaders::options().doVirtualShadow;
  if(const char* stats = std::getenv("OPENGOTHIC_VSM_STATS"))
    settings.vsmStats = std::string_view(stats)=="1";
  // OPENGOTHIC_VSM_LAMP=hard|flat: the lamp shadow filter for a study of its cost
  // (reports/render-features/2026-11-04-lamp-pcss/step-2c.md): one gather at the centre, or the same taps against
  // the receiver's depth without its plane; unset or pcss: the PCSS ([INTERNAL] vsmLampSoft does not change the study)
  if(const char* mode = std::getenv("OPENGOTHIC_VSM_LAMP")) {
    const std::string_view m = mode;
    settings.vsmLampMode = m=="hard" ? 1 : (m=="flat" ? 2 : (m=="truth" ? 3 : 0));
    if(settings.vsmLampMode!=0)
      Log::i("vsm: lamp filter study mode = ", m);
    }
  // OPENGOTHIC_VSM_LAMP_NARROW=<texels>: the widest penumbra filtered by four taps (light.frag, OmniNarrow); 0 always twelve
  if(const char* narrow = std::getenv("OPENGOTHIC_VSM_LAMP_NARROW")) {
    settings.vsmLampNarrow = std::max(0.f, float(std::atof(narrow)));
    Log::i("vsm: lamp filter study narrow penumbra = ", settings.vsmLampNarrow, " texels");
    }
  // OPENGOTHIC_VSM_LAMP_TAA=1: under TAA the lamp taps turn from frame to frame, fewer of them. Only for the study: the
  // pattern lets light into contact shadows and its grain outlives TAA (reports/render-features/2026-11-04-lamp-pcss/
  // step-2d-why.md). The switches below imply it: OPENGOTHIC_VSM_LAMP_TAPS=<search 1>,<search 2>,<narrow>,<wide>;
  // OPENGOTHIC_VSM_LAMP_FLOOR=1 the search stops after its first tap when that one puts a shadow's edge at the
  // penumbra's floor; OPENGOTHIC_VSM_LAMP_CENTRE=1 the narrow penumbra's threshold in texels of the face's centre, not
  // where they are smallest
  if(const char* e = std::getenv("OPENGOTHIC_VSM_LAMP_TAA"); e!=nullptr && std::string_view(e)=="1")
    settings.vsmLampTaa = true;
  if(const char* taps = std::getenv("OPENGOTHIC_VSM_LAMP_TAPS")) {
    unsigned t[4] = {};
    if(std::sscanf(taps, "%u,%u,%u,%u", &t[0], &t[1], &t[2], &t[3])==4 && t[0]>=1 && t[2]>=1 && t[3]>=1) {
      for(int i=0; i<4; ++i)
        settings.vsmLampTaps[i] = std::min(t[i], 16u);
      settings.vsmLampStudy = true;
      Log::i("vsm: lamp filter study taps under taa = ", t[0], ",", t[1], ",", t[2], ",", t[3]);
      } else {
      Log::e("vsm: OPENGOTHIC_VSM_LAMP_TAPS wants <search 1>,<search 2>,<narrow>,<wide>, got ", taps);
      }
    }
  if(const char* e = std::getenv("OPENGOTHIC_VSM_LAMP_FLOOR"); e!=nullptr && std::string_view(e)=="1") {
    settings.vsmLampFlags |= 1;
    settings.vsmLampStudy  = true;
    Log::i("vsm: lamp filter study: the search stops at the penumbra's floor");
    }
  if(const char* e = std::getenv("OPENGOTHIC_VSM_LAMP_CENTRE"); e!=nullptr && std::string_view(e)=="1") {
    settings.vsmLampFlags |= 2;
    settings.vsmLampStudy  = true;
    Log::i("vsm: lamp filter study: the narrow penumbra in texels of the face's centre");
    }
  // OPENGOTHIC_VSM_LAMP_EXP=still,inner,litall,radial,expect,paths: the experiments of step-2d-why.md (light.frag, probeExp)
  if(const char* e = std::getenv("OPENGOTHIC_VSM_LAMP_EXP")) {
    const std::string_view s = e;
    const char* names[] = {"still", "inner", "litall", "radial", "expect", "paths"};
    for(uint32_t i=0; i<6; ++i)
      if(s.find(names[i])!=std::string_view::npos)
        settings.vsmLampExp |= 1u << i;
    settings.vsmLampStudy = true;
    Log::i("vsm: lamp filter study experiments = ", e, " (", settings.vsmLampExp, ")");
    }
  settings.vsmLampTaa   |= settings.vsmLampStudy;
  if(settings.vsmLampTaa)
    Log::i("vsm: lamp filter study: taps turned per frame under taa");
  settings.vsmLampStudy |= settings.vsmLampMode!=0 || settings.vsmStats || settings.vsmLampNarrow>=0;

  sky.sampler = Tempest::Sampler::bilinear();
  sky.sampler.vClamp = ClampMode::ClampToEdge;

  sky.cloudsLut     = device.image2d   (sky.lutRGBAFormat,  2,  1);
  sky.transLut      = device.attachment(sky.lutRGBFormat, 256, 64);
  sky.multiScatLut  = device.attachment(sky.lutRGBFormat,  32, 32);
  sky.viewLut       = device.attachment(Tempest::TextureFormat::RGBA32F, 128, 64);
  sky.viewCldLut    = device.attachment(Tempest::TextureFormat::RGBA32F, 512, 256);
  sky.irradianceLut = device.image2d(TextureFormat::RGBA32F, 3,2);

  setupSettings();
  }

Renderer::~Renderer() {
  Gothic::inst().onSettingsChanged.ubind(this,&Renderer::setupSettings);
  }

void Renderer::setupSettings() {
  settings.zEnvMappingEnabled = Gothic::settingsGetI("ENGINE","zEnvMappingEnabled")!=0;
  settings.zCloudShadowScale  = Gothic::settingsGetI("ENGINE","zCloudShadowScale") !=0;
  // SSAO and the fog shadow mask cost ~15 ms a frame at 3456x1918 on an M1 Pro (docs/performance.md):
  // both are traced at half resolution, the depth-aware blur brings SSAO back to full size
  settings.halfResAoFog       = Gothic::settingsGetI("INTERNAL","halfResAoFog")!=0;
  settings.zFogRadial         = Gothic::settingsGetI("RENDERER_D3D","zFogRadial")!=0;
  {
    // wind
    settings.zWindEnabled = Gothic::settingsGetI("ENGINE","zWindEnabled")!=0;

    const float period  = Gothic::settingsGetF("ENGINE","zWindCycleTime");
    const float periodV = Gothic::settingsGetF("ENGINE","zWindCycleTimeVar");
    settings.windPeriod = uint64_t((period+periodV)*1000.f);
    if(settings.windPeriod<=0) {
      settings.windPeriod   = 1;
      settings.zWindEnabled = false;
      }
  }

  // VSM lamps: blockers nearer than this to the light (its own basket, lantern frame or candle holder) cast no
  // shadow; 0 or unset: 30 cm. With the light in its flame a brazier's basket is within it; a man by a torch or a
  // candle on a table still casts a shadow (80 cm took that away)
  settings.vsmLampNearCm      = Gothic::settingsGetF("INTERNAL","vsmLampNearCm");
  if(settings.vsmLampNearCm<=0)
    settings.vsmLampNearCm = 30;
  // VSM lamps: the soft PCSS, or hard shadows in one gather (light.frag, VSM_LAMP_HARD). On Metal hard by default:
  // the PCSS costs ~11 ms GPU indoors on an M1 Pro (reports/render-features/2026-11-05-vsm-mac-hard/README.md).
  // OPENGOTHIC_VSM_LAMP_SOFT=0|1 (test benches) wins over the ini; the study switches (OPENGOTHIC_VSM_LAMP*,
  // OPENGOTHIC_VSM_STATS, read in the constructor) take the study shader and its PCSS whatever this says
  {
    const char* env = std::getenv("OPENGOTHIC_VSM_LAMP_SOFT");
    settings.vsmLampSoft = env!=nullptr ? std::string_view(env)!="0" : Gothic::settingsGetI("INTERNAL","vsmLampSoft")!=0;
    static int logged = -1;
    if(settings.vsmEnabled && logged!=int(settings.vsmLampSoft)) {
      logged = int(settings.vsmLampSoft);
      Log::i("vsm: lamp shadows = ", settings.vsmLampSoft ? "soft (pcss)" : "hard (one gather)",
             env!=nullptr ? " by OPENGOTHIC_VSM_LAMP_SOFT" : " by [INTERNAL] vsmLampSoft");
      }
  }
  settings.zVidBrightness     = Gothic::settingsGetF("VIDEO","zVidBrightness");
  settings.zVidContrast       = Gothic::settingsGetF("VIDEO","zVidContrast");
  settings.zVidGamma          = Gothic::settingsGetF("VIDEO","zVidGamma");

  settings.sunSize            = Gothic::settingsGetF("SKY_OUTDOOR","zSunSize");
  settings.moonSize           = Gothic::settingsGetF("SKY_OUTDOOR","zMoonSize");
  if(settings.sunSize<=1)
    settings.sunSize = 200;
  if(settings.moonSize<=1)
    settings.moonSize = 400;

  settings.vidResIndex    = Gothic::settingsGetF("INTERNAL","vidResIndex");
  // MetalFX spatial upscaling of reduced internal resolution; Lanczos in the tonemapping pass otherwise
  settings.spatialUpscale = Gothic::settingsGetI("INTERNAL","spatialUpscale")!=0 &&
                            Resources::device().properties().spatialUpscale;
  // temporal AA by the camera (docs/graphics-plan.md, step 1a), on by default: -taa 0 or [INTERNAL] taa=0 turn it
  // off and bring back CMAA2; OPENGOTHIC_TAA=0|1 (test benches) overrides both
  {
    bool taa = Gothic::options().taa && Gothic::settingsGetI("INTERNAL","taa")!=0;
    if(const char* e = std::getenv("OPENGOTHIC_TAA"))
      taa = std::string_view(e)!="0";
    settings.taaEnabled = taa && Resources::device().properties().hasAttachFormat(TextureFormat::RGBA16F);
    // the sharpening after the resolve (RCAS) and the mip bias of the main view's material textures:
    // the history averages the jittered samples, which softens; OPENGOTHIC_TAA_SHARPNESS, OPENGOTHIC_TAA_MIP_BIAS
    // (test benches) override
    settings.taaSharpness = Gothic::settingsGetF("INTERNAL","taaSharpness");
    if(const char* e = std::getenv("OPENGOTHIC_TAA_SHARPNESS"))
      settings.taaSharpness = std::strtof(e, nullptr);
    settings.taaSharpness = std::clamp(settings.taaSharpness, 0.f, 1.f);
    settings.taaMipBias   = -0.5f;
    if(const char* e = std::getenv("OPENGOTHIC_TAA_MIP_BIAS"))
      settings.taaMipBias = std::clamp(std::strtof(e, nullptr), -2.f, 0.f);
    static int logged = -1;
    if(logged!=int(settings.taaEnabled)) {
      logged = int(settings.taaEnabled);
      Log::i("render: taa = ", logged, ", sharpness = ", settings.taaSharpness, ", mip bias = ", settings.taaMipBias);
      }
  }
  // SSAO over frames (reports/engine/2026-11-08-ssao-temporal), on by default: OPENGOTHIC_SSAO_HISTORY=0 traces the
  // whole set of 32 every frame and keeps no history (the same look, at the former cost)
  {
    settings.ssaoHistory = true;
    if(const char* e = std::getenv("OPENGOTHIC_SSAO_HISTORY"))
      settings.ssaoHistory = std::string_view(e)!="0";
    static int logged = -1;
    if(logged!=int(settings.ssaoHistory)) {
      logged = int(settings.ssaoHistory);
      Log::i("render: ssao history = ", logged);
      }
  }
  // A study of the temporal upscale (graphics plan 1g, reports/render-features/2026-11-05-upscale), test benches only:
  // OPENGOTHIC_RENDER_SCALE=0.5..1 draws the scene at that share of the frame over vidResIndex;
  // OPENGOTHIC_UPSCALE=temporal puts MetalFX Temporal in place of the TAA resolve when the scene is drawn smaller than
  // the frame (Metal, with TAA); OPENGOTHIC_UPSCALE_MIP_BIAS overrides its mip bias, log2(in/out) - 1
  {
    settings.renderScale = 0;
    if(const char* e = std::getenv("OPENGOTHIC_RENDER_SCALE"))
      settings.renderScale = std::clamp(std::strtof(e, nullptr), 0.5f, 1.f);
    settings.temporalUpscale = false;
    if(const char* e = std::getenv("OPENGOTHIC_UPSCALE"); e!=nullptr && std::string_view(e)=="temporal")
      settings.temporalUpscale = settings.taaEnabled && Resources::device().properties().temporalUpscale;
    settings.upscaleMipBiasSet = false;
    if(const char* e = std::getenv("OPENGOTHIC_UPSCALE_MIP_BIAS")) {
      settings.upscaleMipBiasSet = true;
      settings.upscaleMipBias    = std::clamp(std::strtof(e, nullptr), -3.f, 0.f);
      }
    static bool logged = false;
    if(!logged && (settings.renderScale>0 || std::getenv("OPENGOTHIC_UPSCALE")!=nullptr)) {
      logged = true;
      Log::i("render: upscale = ", settings.temporalUpscale ? "temporal" : "off", ", render scale = ",
             settings.renderScale, settings.upscaleMipBiasSet ? ", mip bias set = " : "",
             settings.upscaleMipBiasSet ? std::to_string(settings.upscaleMipBias) : std::string());
      }
  }
  // CMAA2 runs at internal resolution, so it needs a same-size target: native or the upscaler input
  const bool nativeRes    = settings.vidResIndex==0 && (settings.renderScale==0 || settings.renderScale==1);
  settings.aaEnabled      = !settings.taaEnabled && (Gothic::options().aaPreset>0) &&
                            (nativeRes || settings.spatialUpscale);
  // bloom from the HDR scene before tonemapping; OPENGOTHIC_BLOOM=0 turns it off
  if(const char* bloom = std::getenv("OPENGOTHIC_BLOOM"))
    settings.bloomEnabled = std::string_view(bloom)!="0";

  // direct lighting
  if(settings.vsmEnabled)
    shadow.directLightPso = &shaders.vsmDirectLight;
  else if(Shaders::options().doRayQuery && settings.shadowResolution>0)
    shadow.directLightPso = &shaders.directLightRq;
  else if(settings.shadowResolution>0)
    shadow.directLightPso = &shaders.directLightSh;
  else
    shadow.directLightPso = &shaders.directLight;

  // point-lights
  if(settings.vsmEnabled && settings.vsmLampStudy)
    lights.directLightPso = &shaders.lightsVsmProbe;
  else if(settings.vsmEnabled && !settings.vsmLampSoft)
    lights.directLightPso = &shaders.lightsVsmHard;
  else if(settings.vsmEnabled)
    lights.directLightPso = &shaders.lightsVsm;
  else if(Shaders::options().doRayQuery)
    lights.directLightPso = &shaders.lightsRq;
  else
    lights.directLightPso = &shaders.lights;

  const auto gi = settings.giMethod;
  if(gi==GiMethod::Probes && Shaders::isGi1Supported() && Shaders::options().doRtScene) {
    settings.giMethod = GiMethod::Probes;
    }
  else if(gi==GiMethod::IrrC && Shaders::isGi2Supported() && Shaders::options().doRtScene) {
    settings.giMethod = GiMethod::IrrC;
    }
  else {
    settings.giMethod = GiMethod::None;
    }

  Log::i("render: ray query = ", int(Shaders::options().doRayQuery), ", meshlets = ", int(Shaders::options().doMeshShading),
         ", gi = ", int(settings.giMethod), ", aa = ", settings.aaEnabled ? int(Gothic::options().aaPreset) : 0,
         ", bloom = ", int(settings.bloomEnabled));
  Log::i("render: vsm = ", int(settings.vsmEnabled));

  if(settings.giMethod!=GiMethod::None) {
    // need a projective shadow, for gi
    resetShadowmap();
    }

  resetSkyFog();
  resetShadowmap();
  }

void Renderer::toggleVsm() {
  if(!Shaders::isVsmSupported())
    return;

  settings.vsmEnabled = !settings.vsmEnabled;

  auto& device = Resources::device();
  device.waitIdle();

  setupSettings();
  }

void Renderer::togglePathtrace() {
  settings.pathTraceEnabled = !settings.pathTraceEnabled;
  pt.numFrames = 0;
  setupSettings();
  }

void Renderer::onWorldChanged() {
  sky.lutIsInitialized = false;
  taa.valid            = false;
  ssao.valid           = false;
  gi.resetHistory      = true;
  resetSkyFog();
  }

void Renderer::resetTemporal() {
  taa.valid  = false;
  ssao.valid = false;
  taa.frame = 0;
  taa.cur   = 0;
  }

void Renderer::updateCamera(const WorldView& wview, const Camera& camera) {
  proj        = camera.projective();
  viewProj    = camera.viewProj();
  viewProjLwc = camera.viewProjLwc();

  // without jitter, as the TAA takes it: SSAO reprojects its history by the camera even with the TAA off
  ssao.view.proj    = proj;
  ssao.view.viewLwc = camera.viewLwc();
  ssao.view.origin  = camera.originLwc();
  ssao.camCuts      = camera.cuts();

  for(size_t i=0; i<Resources::ShadowLayers; ++i)
    shadowMatrix[i] = camera.viewShadow(wview.mainLight().dir(),i);
  shadowMatrixVsm = camera.viewShadowVsm(wview.mainLight().dir());

  auto zNear = camera.zNear();
  auto zFar  = camera.zFar();
  clipInfo.x = zNear*zFar;
  clipInfo.y = zNear-zFar;
  clipInfo.z = zFar;
  }

bool Renderer::requiresRtScene() const {
  if(!Shaders::options().doRtScene)
    return false;

  if(settings.giMethod!=GiMethod::None || settings.pathTraceEnabled)
    return true;
  if(!settings.vsmEnabled)
    return true;
  return false;
  }

bool Renderer::requiresLightsTree() const {
  if(!Shaders::isLightsTreeSupported())
    return false;
  if(settings.giMethod==GiMethod::IrrC || settings.giMethod==GiMethod::Probes || settings.pathTraceEnabled)
    return true;
  return false;
  }

StorageImage& Renderer::usesImage3d(Tempest::StorageImage& ret, Tempest::TextureFormat frm, uint32_t w, uint32_t h, uint32_t d, bool mips) {
  if(ret.format()==frm && uint32_t(ret.w())==w && uint32_t(ret.h())==h && uint32_t(ret.d())==d && bool(ret.mipCount()>1)==mips)
    return ret;
  Resources::recycle(std::move(ret));
  ret = Resources::device().image3d(frm, w, h, d, mips);
  return ret;
  }

StorageImage& Renderer::usesImage2d(Tempest::StorageImage& ret, Tempest::TextureFormat frm, uint32_t w, uint32_t h, bool mips) {
  if(ret.format()==frm && uint32_t(ret.w())==w && uint32_t(ret.h())==h && ret.d()==1 && bool(ret.mipCount()>1)==mips)
    return ret;
  Resources::recycle(std::move(ret));
  ret = Resources::device().image2d(frm, w, h, mips);
  return ret;
  }

StorageImage& Renderer::usesImage2d(Tempest::StorageImage& ret, Tempest::TextureFormat frm, Tempest::Size size, bool mips) {
  if(ret.format()==frm && ret.size()==size && ret.d()==1 && (ret.mipCount()>1)==mips)
    return ret;
  Resources::recycle(std::move(ret));
  ret = Resources::device().image2d(frm, size, mips);
  return ret;
  }

ZBuffer& Renderer::usesZBuffer(Tempest::ZBuffer& ret, Tempest::TextureFormat frm, uint32_t w, uint32_t h) {
  if(textureCast<Texture2d&>(ret).format()==frm && uint32_t(ret.w())==w && uint32_t(ret.h())==h)
    return ret;
  Resources::recycle(std::move(ret));
  ret = Resources::device().zbuffer(frm, w, h);
  return ret;
  }

Attachment& Renderer::usesAttachment(Tempest::Attachment& ret, Tempest::TextureFormat frm, Tempest::Size size) {
  if(textureCast<Texture2d&>(ret).format()==frm && ret.size()==size)
    return ret;
  Resources::recycle(std::move(ret));
  ret = Resources::device().attachment(frm, size);
  return ret;
  }

Attachment& Renderer::usesAttachment(Tempest::Attachment& ret, Tempest::TextureFormat frm, uint32_t w, uint32_t h) {
  if(textureCast<Texture2d&>(ret).format()==frm && uint32_t(ret.w())==w && uint32_t(ret.h())==h)
    return ret;
  Resources::recycle(std::move(ret));
  ret = Resources::device().attachment(frm, w, h);
  return ret;
  }

StorageBuffer& Renderer::usesSsbo(Tempest::StorageBuffer& ret, size_t size) {
  if(ret.byteSize()==size)
    return ret;
  Resources::recycle(std::move(ret));
  ret = Resources::device().ssbo(Uninitialized, size);
  return ret;
  }

StorageBuffer& Renderer::usesSsboInit(Tempest::StorageBuffer& ret, size_t size) {
  if(ret.byteSize()==size)
    return ret;
  Resources::recycle(std::move(ret));
  ret = Resources::device().ssbo(nullptr, size);
  return ret;
  }

void Renderer::prepareUniforms(WorldView& wview) {
  const Texture2d* sh[Resources::ShadowLayers] = {};
  for(size_t i=0; i<Resources::ShadowLayers; ++i)
    if(!shadowMap[i].isEmpty()) {
      sh[i] = &textureCast<const Texture2d&>(shadowMap[i]);
      }
  wview.setShadowMaps(sh);
  wview.setVirtualShadowMap(settings.vsmEnabled, vsm.pageData, vsm.pageTbl, vsm.pageHiZ, vsm.pageList);

  wview.setHiZ(textureCast<const Texture2d&>(hiz.hiZ));
  wview.setGbuffer(textureCast<const Texture2d&>(gbufDiffuse), textureCast<const Texture2d&>(gbufNormal));
  wview.setSceneImages(textureCast<const Texture2d&>(sceneOpaque), textureCast<const Texture2d&>(sceneDepth), zbuffer);
  wview.setFog(sky.fogLut3D, sky.fogLut3DMs);
  wview.setWindEnabled(settings.zWindEnabled, settings.windPeriod);
  if(auto* world = Gothic::inst().world())
    wview.setClothWind(world->physic()->clothEnabled(),world->physic()->cloth().windScale());
  }

void Renderer::resetViewport(Tempest::Size res, Tempest::Size fullRes) {
  auto& device = Resources::device();
  device.waitIdle();

  const uint32_t w = uint32_t(res.w);
  const uint32_t h = uint32_t(res.h);

  // the temporal upscale (graphics plan 1g): MetalFX makes the frame of the swapchain's size from the jittered one,
  // and bloom and tonemapping go on in that size
  const bool     temporalOn = useTemporalUpscale(res, fullRes);
  const uint32_t bw         = temporalOn ? uint32_t(fullRes.w) : w;
  const uint32_t bh         = temporalOn ? uint32_t(fullRes.h) : h;

  sceneLinear = device.attachment(TextureFormat::R11G11B10UF,w,h);
  for(size_t i=0; i<bloomChain.size(); ++i)
    bloomChain[i] = settings.bloomEnabled ? device.attachment(TextureFormat::R11G11B10UF,
                                                              std::max(bw>>(i+1),1u), std::max(bh>>(i+1),1u)) : Attachment();

  if(temporalOn) {
    temporal.motion  = device.attachment(TextureFormat::RG16F, w, h);
    temporal.output  = device.image2d(TextureFormat::R11G11B10UF, bw, bh);
    temporal.phases  = Taa::upscalePhases(w, h, bw, bh);
    temporal.mipBias = settings.upscaleMipBiasSet ? settings.upscaleMipBias : Taa::upscaleMipBias(w, bw);
    Log::i("render: temporal upscale ", w, "x", h, " -> ", bw, "x", bh, ", phases = ", temporal.phases,
           ", mip bias = ", temporal.mipBias);
    } else {
    temporal.motion  = Attachment();
    temporal.output  = StorageImage();
    }

  // TAA: the frame resolved with the history is written into `output`, which then swaps with sceneLinear
  if(settings.taaEnabled && !temporalOn) {
    taa.output     = device.attachment(TextureFormat::R11G11B10UF,w,h);
    taa.history[0] = device.attachment(TextureFormat::RGBA16F,w,h);
    taa.history[1] = device.attachment(TextureFormat::RGBA16F,w,h);
    } else {
    taa.output     = Attachment();
    taa.history[0] = Attachment();
    taa.history[1] = Attachment();
    }
  taa.valid = false;

  if(settings.aaEnabled) {
    cmaa2.workingEdges               = device.image2d(TextureFormat::R8, (w + 1) / 2, h);
    cmaa2.shapeCandidates            = device.ssbo(Tempest::Uninitialized, w * h / 4 * sizeof(uint32_t));
    cmaa2.deferredBlendLocationList  = device.ssbo(Tempest::Uninitialized, (w * h + 3) / 6 * sizeof(uint32_t));
    cmaa2.deferredBlendItemList      = device.ssbo(Tempest::Uninitialized, w * h * sizeof(uint32_t));
    cmaa2.deferredBlendItemListHeads = device.image2d(TextureFormat::R32U, (w + 1) / 2, (h + 1) / 2);
    cmaa2.controlBuffer              = device.ssbo(nullptr, 5 * sizeof(uint32_t));
    cmaa2.indirectBuffer             = device.ssbo(nullptr, sizeof(DispatchIndirectCommand) + sizeof(DrawIndirectCommand));
    }

  if(settings.spatialUpscale && res!=fullRes && !temporalOn) {
    upscale.input  = device.attachment(TextureFormat::RGBA8, w, h);
    upscale.output = device.image2d(TextureFormat::RGBA8, uint32_t(fullRes.w), uint32_t(fullRes.h));
    } else {
    upscale.input  = Attachment();
    upscale.output = StorageImage();
    }

  zbuffer = device.zbuffer(zBufferFormat,w,h);
  if(res!=fullRes)
    zbufferUi = device.zbuffer(zBufferFormat, fullRes); else
    zbufferUi = ZBuffer();

  uint32_t pw = nextPot(w);
  uint32_t ph = nextPot(h);

  uint32_t hw = pw;
  uint32_t hh = ph;
  while(hw>64 || hh>64) {
    hw = std::max(1u, (hw+1)/2u);
    hh = std::max(1u, (hh+1)/2u);
    }

  hiz.hiZ       = device.image2d(TextureFormat::R16,  hw, hh, true);

  sceneOpaque   = device.attachment(TextureFormat::R11G11B10UF,w,h);
  sceneDepth    = device.attachment(TextureFormat::R32F,       w,h);

  gbufDiffuse   = device.attachment(TextureFormat::RGBA8,w,h);
  gbufNormal    = device.attachment(TextureFormat::R32U, w,h);
  if(Shaders::options().doMaterialMaps)
    gbufOrm = device.attachment(TextureFormat::RGBA8, w,h);

  ssao     = decltype(ssao)();
  epipolar = decltype(epipolar)();
  vsm      = decltype(vsm)();

  // volumetric sky
  sky.occlusionLut = StorageImage();

  resetSkyFog();
  }

void Renderer::resetShadowmap() {
  auto& device = Resources::device();

  for(int i=0; i<Resources::ShadowLayers; ++i)
    Resources::recycle(std::move(shadowMap[i]));

  // the probes of -gi 1 light their hits with a shadow ray (probe_lighting.comp): no cascade for them under VSM
  const bool forceSm1 = (settings.giMethod==GiMethod::IrrC || settings.pathTraceEnabled);
  for(int i=0; i<Resources::ShadowLayers; ++i) {
    if(!(i==1 && forceSm1)) {
      if(settings.vsmEnabled)
        continue; //TODO: support vsm in gi code
      }
    shadowMap[i] = device.zbuffer(shadowFormat, settings.shadowResolution, settings.shadowResolution);
    }
  }

void Renderer::draw(Attachment& result, Encoder<CommandBuffer>& cmd, uint8_t fId,
                    VectorImage::Mesh& uiLayer, VectorImage::Mesh& numOverlay,
                    InventoryMenu& inventory, VideoWidget& video) {
  auto wview  = Gothic::inst().worldView();
  auto camera = Gothic::inst().camera();

  if(!video.isActive() && wview!=nullptr && camera!=nullptr) {
    draw(result, cmd, fId, *wview, *camera);
    cmd.setFramebuffer({{result, Tempest::Preserve, Tempest::Preserve}});
    } else {
    taa.valid = false; // a video or no world: the next world frame starts over
    cmd.setFramebuffer({{result, Vec4(), Tempest::Preserve}});
    }
  FrameProfile::Scope uiTiming(FrameProfile::Zone::EncodeUi);
  cmd.setDebugMarker("UI");
  uiLayer.draw(cmd);

  if(inventory.isOpen()!=InventoryMenu::State::Closed) {
    auto& zb = (zbufferUi.isEmpty() ? zbuffer : zbufferUi);
    cmd.setFramebuffer({{result, Tempest::Preserve, Tempest::Preserve}},{zb, 1.f, Tempest::Preserve});
    cmd.setDebugMarker("Inventory");
    inventory.draw(cmd);

    cmd.setFramebuffer({{result, Tempest::Preserve, Tempest::Preserve}});
    cmd.setDebugMarker("Inventory-counters");
    numOverlay.draw(cmd);
    }
  }

void Renderer::dbgDraw(Tempest::Painter& p) {
  static bool dbg = false;
  if(!dbg)
    return;

  std::vector<const Texture2d*> tex;
  //tex.push_back(&textureCast(hiz.hiZ));
  //tex.push_back(&textureCast(hiz.smProj));
  //tex.push_back(&textureCast(hiz.hiZSm1));
  //tex.push_back(&textureCast(shadowMap[1]));
  //tex.push_back(&textureCast<const Texture2d&>(shadowMap[0]));
  //tex.push_back(&textureCast<const Texture2d&>(vsm.pageData));
  tex.push_back(&textureCast<const Texture2d&>(surf.irrImage));

  static int size = 400;
  int left = 10;
  for(auto& t:tex) {
    p.setBrush(Brush(*t,Painter::NoBlend,ClampMode::ClampToBorder));
    auto sz = Size(p.brush().w(),p.brush().h());
    if(sz.isEmpty())
      continue;
    while(sz.w<size && sz.h<size) {
      sz.w *= 2;
      sz.h *= 2;
      }
    while(sz.w>size*2 || sz.h>size*2) {
      sz.w = (sz.w+1)/2;
      sz.h = (sz.h+1)/2;
      }
    p.drawRect(left,50,sz.w,sz.h,
               0,0,p.brush().w(),p.brush().h());
    left += (sz.w+10);
    }
  }

void Renderer::draw(Tempest::Attachment& result, Encoder<CommandBuffer>& cmd, uint8_t fId, WorldView& wview, const Camera& camera) {
  FrameProfile::Scope mainTiming(FrameProfile::Zone::EncodeMain);
  //FIXME: those also needed to be refactor away
  const auto world     = Gothic::inst().world();
  const auto gameTime  = world!=nullptr ? world->time() : gtime(8,0);
  const auto tickCount = world!=nullptr ? world->tickCount() : uint64_t();

  shaders.waitCompiler();

  auto res = internalResolution(result.size());
  // With TAA a screenshot (bench `shot`, every frame of `rec`) is drawn at the internal resolution it is the size of:
  // otherwise its smaller internal size would rebuild the viewport, and the history with it, at every capture
  const bool shotNative = taa.shot && settings.taaEnabled && result.size()==zbuffer.size();
  if(shotNative)
    res = zbuffer.size();
  // a switch between TAA and CMAA2 (settings) needs their buffers; the temporal upscale its own, of the frame's size
  const bool aaReady  = !settings.aaEnabled  || !cmaa2.workingEdges.isEmpty();
  const bool taaReady = !settings.taaEnabled ||
                        (useTemporalUpscale(res, result.size()) ? temporal.output.size()==result.size()
                                                                : !taa.output.isEmpty());
  if(res!=zbuffer.size() || !aaReady || !taaReady) {
    resetViewport(res, result.size());
    }

  if(requiresRtScene())
    wview.updateRtScene(cmd, fId);
  wview.updateLights(gameTime);
  gi.dayTime = gameTime.toInt();

  if(requiresLightsTree())
    prepareLightsBvh(cmd, wview);

  updateCamera(wview, camera);
  prepareTaa(wview, camera);

  static bool updFr = true;
  if(updFr){
    if(wview.mainLight().dir().y>Camera::minShadowY) {
      frustrum[SceneGlobals::V_Shadow0].make(shadowMatrix[0],shadowMap[0].w(),shadowMap[0].h());
      frustrum[SceneGlobals::V_Shadow1].make(shadowMatrix[1],shadowMap[1].w(),shadowMap[1].h());
      } else {
      frustrum[SceneGlobals::V_Shadow0].clear();
      frustrum[SceneGlobals::V_Shadow1].clear();
      }
    frustrum[SceneGlobals::V_Main].make(viewProj,zbuffer.w(),zbuffer.h());
    frustrum[SceneGlobals::V_HiZ] = frustrum[SceneGlobals::V_Main];
    frustrum[SceneGlobals::V_Vsm] = frustrum[SceneGlobals::V_Shadow1]; //TODO: remove
    wview.updateFrustrum(frustrum);
    }

  if(world!=nullptr) {
    // Keep a margin around the camera view so a turning camera resumes cloth
    // before it becomes visible. The physics tick owns a copy of these planes.
    world->physic()->cloth().setVisibilityTest([view = frustrum[SceneGlobals::V_Main]](const Vec3& center, float radius) {
      return view.testPoint(center,radius+200.f);
      });
    }

  prepareUniforms(wview);
  wview.preFrameUpdate(camera, tickCount, fId);
  wview.prepareGlobals(cmd,fId);

  if(settings.pathTraceEnabled) {
    drawPathtrace(cmd, wview, fId);
    mainTiming.finish();
    FrameProfile::Scope postTiming(FrameProfile::Zone::EncodePost);
    cmd.setDebugMarker("Tonemapping");
    drawTonemapping(result, cmd, wview);
    postTiming.finish();
    wview.postFrameupdate();
    return;
    }

  wview.visibilityPass(cmd, 0);
  prepareSky(cmd, wview);

  drawHiZ (cmd, wview);
  buildHiZ(cmd);

  wview.visibilityPass(cmd, 1);
  drawGBuffer(cmd, fId, wview);
  if(world!=nullptr && blood.prepare(fId, *world, wview.sceneGlobals(), camera.listenerPosition().pos))
    blood.draw(cmd, fId, wview.sceneGlobals(), gbufDiffuse, Shaders::options().doMaterialMaps ? &gbufOrm : nullptr,
               gbufNormal, zbuffer);
  // SSAO needs only the depth: ahead of the shadow maps its compute runs under their vertex work on Metal
  prepareSSAO(cmd, wview);
  mainTiming.finish();

  drawShadowMap(cmd, fId, wview);
  prepareEpipolar(cmd, wview);

  drawVsm(cmd, wview);

  { FrameProfile::Scope timing(FrameProfile::Zone::EncodeMain); prepareIrradiance(cmd, wview); }
  prepareExposure(cmd, wview);
  FrameProfile::Scope lightingTiming(FrameProfile::Zone::EncodeMain);
  prepareFog (cmd, wview);
  prepareGi  (cmd, fId, wview);
  prepareSurfels(cmd, wview);

  cmd.setFramebuffer({{sceneLinear, Tempest::Discard, Tempest::Preserve}}, {zbuffer, Tempest::Readonly});
  drawShadowResolve(cmd, wview);
  drawAmbient(cmd, wview);
  drawLights(cmd, wview);
  drawSky(cmd, wview);
  drawLightTreeDbg(sceneLinear, cmd, wview);

  stashSceneAux(cmd);
  lightingTiming.finish();

  FrameProfile::Scope waterTiming(FrameProfile::Zone::EncodeWater);
  drawGWater(cmd, wview);

  cmd.setFramebuffer({{sceneLinear, Tempest::Preserve, Tempest::Preserve}}, {zbuffer, Tempest::Preserve, Tempest::Preserve});
  cmd.setDebugMarker("Sun&Moon");
  drawSunMoon(cmd, wview);
  // Multiply darkens what lies behind it, mostly a surface it is a decal on: the fog pass of that depth goes over it.
  cmd.setDebugMarker("Multiply");
  wview.drawMultiply(cmd, fId);
  waterTiming.finish();

  // Reflections and fog go under the other translucent surfaces: those don't write depth, and the fog pass
  // reads the depth of what lies behind them. They take the fog of their own depth.
  cmd.setFramebuffer({{sceneLinear, Tempest::Preserve, Tempest::Preserve}});
  drawReflections(cmd, wview);
  if(!camera.isInWater()) {
    FrameProfile::Scope fogTiming(FrameProfile::Zone::EncodePost);
    cmd.setDebugMarker("Fog");
    drawFog(cmd, wview);
    }

  FrameProfile::Scope translucentTiming(FrameProfile::Zone::EncodeWater);
  cmd.setFramebuffer({{sceneLinear, Tempest::Preserve, Tempest::Preserve}}, {zbuffer, Tempest::Preserve, Tempest::Preserve});
  cmd.setDebugMarker("Translucent");
  wview.drawTranslucent(cmd, fId);
  translucentTiming.finish();

  //drawHashDbg(sceneLinear, cmd, wview);
  drawProbesDbg(cmd, wview);
  drawProbesHitDbg(cmd);
  drawSurfelsDbg(cmd, wview);
  drawVsmDbg(cmd, wview);
  drawRayQueryDbg(cmd, wview);
  //

  FrameProfile::Scope postTiming(FrameProfile::Zone::EncodePost);
  if(camera.isInWater()) {
    cmd.setFramebuffer({{sceneLinear, Tempest::Preserve, Tempest::Preserve}});
    cmd.setDebugMarker("Underwater");
    drawUnderwater(cmd, wview);
    }

  if(temporal.active) {
    // the frame of the swapchain's size from here on: bloom and tonemapping read temporal.output
    drawTemporalUpscale(cmd);
    drawBloom(cmd);
    cmd.setDebugMarker("Tonemapping");
    drawTonemappingTemporal(result, cmd, wview);
    postTiming.finish();
    wview.postFrameupdate();
    return;
    }

  auto& tonemapTarget = (upscale.input.isEmpty() || shotNative) ? result : upscale.input;
  // sharpening, bloom and tonemapping of the TAA frame are one pass; Lanczos at a smaller size still goes the long way
  const bool fused = taa.active && settings.bloomEnabled && !bloomChain[0].isEmpty() &&
                     tonemapTarget.size()==sceneLinear.size();
  if(taa.active)
    drawTaa(cmd, fused);
  drawBloom(cmd, fused);

  if(fused) {
    cmd.setDebugMarker("Tonemapping");
    drawTonemappingTaa(tonemapTarget, cmd, wview);
    }
  else if(settings.aaEnabled) {
    cmd.setDebugMarker("CMAA2 & Tonemapping");
    drawCMAA2(tonemapTarget, cmd, wview);
    } else {
    cmd.setDebugMarker("Tonemapping");
    drawTonemapping(tonemapTarget, cmd, wview);
    }
  if(!upscale.input.isEmpty() && !shotNative)
    drawUpscale(result, cmd);
  postTiming.finish();

  //drawRayQueryDbg(cmd, wview);
  //drawHashDbg(result, cmd, wview);

  wview.postFrameupdate();
  }

void Renderer::drawTaaSharpen(Tempest::Encoder<Tempest::CommandBuffer>& cmd) {
  // RCAS of AMD FidelityFX FSR 1 (MIT) on the resolved frame into `output` (shader/antialiasing/taa_sharpen.frag)
  struct Push {
    float sharpness = 0;
    } push;
  push.sharpness = settings.taaSharpness;
  cmd.setDebugMarker("TAA-sharpen");
  cmd.setFramebuffer({{taa.output, Tempest::Discard, Tempest::Preserve}});
  cmd.setBinding(0, taa.history[taa.cur], Sampler::nearest(ClampMode::ClampToEdge));
  cmd.setPushData(push);
  cmd.setPipeline(shaders.taaSharpen);
  cmd.draw(nullptr, 0, 3);
  }

bool Renderer::useTemporalUpscale(Tempest::Size res, Tempest::Size fullRes) const {
  return settings.temporalUpscale && settings.taaEnabled && res.w<fullRes.w && res.h<fullRes.h;
  }

void Renderer::drawUpscale(Tempest::Attachment& result, Tempest::Encoder<Tempest::CommandBuffer>& cmd) {
  cmd.setFramebuffer({});
  cmd.setDebugMarker("Upscale");
  cmd.upscale(upscale.input, upscale.output);

  cmd.setFramebuffer({{result, Tempest::Discard, Tempest::Preserve}});
  cmd.setBinding(0, upscale.output, Sampler::nearest());
  cmd.setPipeline(shaders.copy);
  cmd.draw(nullptr, 0, 3);
  }

void Renderer::stashSceneAux(Encoder<CommandBuffer>& cmd) {
  auto& device = Resources::device();
  if(!device.properties().hasSamplerFormat(zBufferFormat))
    return;
  cmd.setFramebuffer({{sceneOpaque, Tempest::Discard, Tempest::Preserve}, {sceneDepth, Tempest::Discard, Tempest::Preserve}});
  cmd.setDebugMarker("Stash scene");
  cmd.setBinding(0, sceneLinear,Sampler::nearest());
  cmd.setBinding(1, zbuffer,    Sampler::nearest());
  cmd.setPipeline(shaders.stash);
  cmd.draw(nullptr, 0, 3);
  }

void Renderer::drawHiZ(Encoder<CommandBuffer>& cmd, WorldView& view) {
  cmd.setDebugMarker("HiZ-occluders");
  cmd.setFramebuffer({}, {zbuffer, 1.f, Tempest::Preserve});
  view.drawHiZ(cmd);
  }

void Renderer::buildHiZ(Tempest::Encoder<Tempest::CommandBuffer>& cmd) {
  assert(hiz.hiZ.w()<=128 && hiz.hiZ.h()<=128); // shader limitation

  cmd.setDebugMarker("HiZ-mip");
  cmd.setFramebuffer({});
  cmd.setBinding(0, zbuffer, Sampler::nearest(ClampMode::ClampToEdge));
  cmd.setBinding(1, hiz.hiZ);
  cmd.setPipeline(shaders.hiZPot);
  cmd.dispatch(size_t(hiz.hiZ.w()), size_t(hiz.hiZ.h()));

  const uint32_t maxBind = 8, mip = hiz.hiZ.mipCount();
  const auto     wgSz = shaders.hiZMip.workGroupSize();
  const int      wgW  = std::max(std::max(hiz.hiZ.w()/2, 1) / wgSz.x, 1);
  const int      wgH  = std::max(std::max(hiz.hiZ.h()/2, 1) / wgSz.y, 1);
  struct Push {
    uint32_t baseMip;
    uint32_t mip;
    } push = {};
  push.baseMip = 0;
  push.mip     = mip;

  for(uint32_t i=0; i<maxBind; ++i)
    cmd.setBinding(1+i, hiz.hiZ, Sampler::nearest(), std::min(i, mip-1));
  cmd.setPushData(push);
  cmd.setPipeline(shaders.hiZMip);
  //cmd.dispatchThreads(std::max(uint32_t(hiz.hiZ.w())/2u, 1u), std::max(uint32_t(hiz.hiZ.h())/2u, 1u));
  cmd.dispatch(size_t(wgW), size_t(wgH));

  if(mip>5) {
    // the mips past 4 read mip 4 of all workgroups above: a dispatch of their own (see hiz_mip.comp)
    push.baseMip = 4;
    cmd.setPushData(push);
    cmd.dispatch(1, 1);
    }
  }

void Renderer::drawGBuffer(Encoder<CommandBuffer>& cmd, uint8_t fId, WorldView& view) {
  cmd.setDebugMarker("GBuffer");
  if(Shaders::options().doMaterialMaps) {
    cmd.setFramebuffer({{gbufDiffuse, Tempest::Vec4(), Tempest::Preserve},
                        {gbufNormal, Tempest::Vec4(), Tempest::Preserve},
                        {gbufOrm, Tempest::Vec4(1,1,0,0), Tempest::Preserve}},
                       {zbuffer, Tempest::Preserve, Tempest::Preserve});
    } else {
    cmd.setFramebuffer({{gbufDiffuse, Tempest::Vec4(), Tempest::Preserve},
                        {gbufNormal, Tempest::Vec4(), Tempest::Preserve}},
                       {zbuffer, Tempest::Preserve, Tempest::Preserve});
    }
  view.drawGBuffer(cmd,fId);
  }

void Renderer::drawGWater(Encoder<CommandBuffer>& cmd, WorldView& view) {
  static bool water = true;
  if(!water)
    return;

  cmd.setFramebuffer({{sceneLinear, Tempest::Preserve, Tempest::Preserve},
                      {gbufDiffuse, Vec4(0,0,0,0),     Tempest::Preserve},
                      {gbufNormal,  Vec4(0,0,0,0),     Tempest::Preserve}},
                     {zbuffer, Tempest::Preserve, Tempest::Preserve});
  // cmd.setFramebuffer({{sceneLinear, Tempest::Preserve, Tempest::Preserve}},
  //                    {zbuffer, Tempest::Preserve, Tempest::Preserve});
  cmd.setDebugMarker("GWater");
  view.drawWater(cmd);
  }

void Renderer::drawReflections(Encoder<CommandBuffer>& cmd, const WorldView& wview) {
  FrameProfile::Scope timing(FrameProfile::Zone::EncodeWater);
  auto& pso = settings.zEnvMappingEnabled ? shaders.waterReflectionSSR : shaders.waterReflection;

  cmd.setDebugMarker("Reflections");
  cmd.setBinding(0, wview.sceneGlobals().uboGlobal[SceneGlobals::V_Main]);
  cmd.setBinding(1, sceneOpaque, Sampler::bilinear(ClampMode::ClampToEdge));
  cmd.setBinding(2, gbufDiffuse, Sampler::nearest (ClampMode::ClampToEdge));
  cmd.setBinding(3, gbufNormal,  Sampler::nearest (ClampMode::ClampToEdge));
  cmd.setBinding(4, zbuffer,     Sampler::nearest (ClampMode::ClampToEdge));
  cmd.setBinding(5, sceneDepth,  Sampler::nearest (ClampMode::ClampToEdge));
  cmd.setBinding(6, sky.viewCldLut, sky.sampler);
  cmd.setPipeline(pso);
  if(Shaders::options().doMeshShading) {
    cmd.dispatchMeshThreads(gbufDiffuse.size());
    } else {
    cmd.draw(nullptr, 0, 3);
    }
  }

void Renderer::drawUnderwater(Encoder<CommandBuffer>& cmd, const WorldView& wview) {
  cmd.setBinding(0, wview.sceneGlobals().uboGlobal[SceneGlobals::V_Main]);
  cmd.setBinding(1, zbuffer);

  cmd.setPipeline(shaders.underwaterT);
  cmd.draw(nullptr, 0, 3);
  cmd.setPipeline(shaders.underwaterS);
  cmd.draw(nullptr, 0, 3);
  }

void Renderer::drawShadowMap(Encoder<CommandBuffer>& cmd, uint8_t fId, WorldView& view) {
  FrameProfile::Scope timing(FrameProfile::Zone::EncodeShadows);
  for(uint8_t i=0; i<Resources::ShadowLayers; ++i) {
    if(shadowMap[i].isEmpty())
      continue;
    cmd.setDebugMarker(string_frm("ShadowMap #",i));
    cmd.setFramebuffer({}, {shadowMap[i], 0.f, Tempest::Preserve});
    if(view.mainLight().dir().y > Camera::minShadowY)
      view.drawShadow(cmd,fId,i);
    }
  }

void Renderer::drawShadowResolve(Encoder<CommandBuffer>& cmd, const WorldView& wview) {
  static bool light = true;
  if(!light)
    return;

  auto& scene = wview.sceneGlobals();
  cmd.setDebugMarker(settings.vsmEnabled ? "DirectSunLight-VSM" : "DirectSunLight");

  struct Push { Vec3 originLwc; float vsmMipBias; } push = {scene.originLwc, settings.vsmMipBias};
  if(shadow.directLightPso==&shaders.vsmDirectLight)
    cmd.setPushData(&push, sizeof(push)); else
    cmd.setPushData(&push.originLwc, sizeof(push.originLwc));
  cmd.setBinding(0, scene.uboGlobal[SceneGlobals::V_Main]);
  cmd.setBinding(1, gbufDiffuse, Sampler::nearest());
  cmd.setBinding(2, gbufNormal,  Sampler::nearest());
  cmd.setBinding(3, zbuffer,     Sampler::nearest());
  if(Shaders::options().doMaterialMaps)
    cmd.setBinding(12, gbufOrm, Sampler::nearest());
  if(shadow.directLightPso==&shaders.vsmDirectLight) {
    cmd.setBinding(4, vsm.pageTbl);
    cmd.setBinding(6, vsm.pageData);
    }
  else if(shadow.directLightPso==&shaders.directLightRq) {
    for(size_t r=0; r<Resources::ShadowLayers; ++r) {
      if(shadowMap[r].isEmpty())
        continue;
      cmd.setBinding(4+r, shadowMap[r], Resources::shadowSampler());
      }
    cmd.setBinding(6, scene.rtScene.tlas);
    cmd.setBinding(7, Sampler::bilinear());
    cmd.setBinding(8, scene.rtScene.tex);
    cmd.setBinding(9, scene.rtScene.vbo);
    cmd.setBinding(10,scene.rtScene.ibo);
    cmd.setBinding(11,scene.rtScene.rtDesc);
    }
  else {
    for(size_t r=0; r<Resources::ShadowLayers; ++r) {
      if(shadowMap[r].isEmpty())
        continue;
      cmd.setBinding(4+r, shadowMap[r], Resources::shadowSampler());
      }
    }

  cmd.setPipeline(*shadow.directLightPso);
  cmd.draw(nullptr, 0, 3);
  }

void Renderer::drawLights(Encoder<CommandBuffer>& cmd, const WorldView& wview) {
  static bool light = true;
  if(!light)
    return;

  auto& scene   = wview.sceneGlobals();
  cmd.setDebugMarker("Point lights");

  cmd.setBinding(0, scene.uboGlobal[SceneGlobals::V_Main]);
  cmd.setBinding(1, gbufDiffuse, Sampler::nearest());
  cmd.setBinding(2, gbufNormal,  Sampler::nearest());
  cmd.setBinding(3, zbuffer,     Sampler::nearest());
  cmd.setBinding(4, wview.lights().lightsSsbo());
  if(Shaders::options().doMaterialMaps)
    cmd.setBinding(12, gbufOrm, Sampler::nearest());
  for(size_t i=0; i<LightGroup::MaxCookies; ++i)
    cmd.setBinding(13+i, wview.lights().cookie(i), Sampler::bilinear()); // u is the azimuth: it wraps
  if(lights.directLightPso==&shaders.lightsVsm || lights.directLightPso==&shaders.lightsVsmHard ||
     lights.directLightPso==&shaders.lightsVsmProbe) {
    cmd.setBinding(5, vsm.pageTblOmni);
    cmd.setBinding(6, vsm.pageData, Sampler::nearest(ClampMode::ClampToEdge)); // textureGather inside a face block
    }
  if(lights.directLightPso==&shaders.lightsRq) {
    cmd.setBinding(6, scene.rtScene.tlas);
    cmd.setBinding(7, Sampler::bilinear());
    cmd.setBinding(8, scene.rtScene.tex);
    cmd.setBinding(9, scene.rtScene.vbo);
    cmd.setBinding(10,scene.rtScene.ibo);
    cmd.setBinding(11,scene.rtScene.rtDesc);
    }

  auto& ibo  = Resources::cubeIbo();
  struct Push {
    Vec3 originLwc; float vsmLampNear; uint32_t probeMode, probeStamp; float probeNarrow; uint32_t probeTaps[4], probeFlags;
    uint32_t probeExp;
    } push = {scene.originLwc, settings.vsmLampNearCm, settings.vsmLampMode, 0, settings.vsmLampNarrow,
              {settings.vsmLampTaps[0], settings.vsmLampTaps[1], settings.vsmLampTaps[2], settings.vsmLampTaps[3]},
              settings.vsmLampFlags, settings.vsmLampExp};
  if(lights.directLightPso==&shaders.lightsVsmProbe) {
    // the study variant: its counters and a stamp per pixel (light.frag, LampProbe); one frame in 5 s counts
    auto& probe = usesSsboInit(vsm.lampProbe, (16 + size_t(zbuffer.w())*size_t(zbuffer.h()))*sizeof(uint32_t));
    cmd.setBinding(7, probe);
    if(vsm.lampArmed) {
      push.probeStamp = ++vsm.lampStamp==0 ? ++vsm.lampStamp : vsm.lampStamp;
      vsm.lampArmed   = false;
      vsm.lampCounted = true;
      }
    }
  if(lights.directLightPso==&shaders.lightsVsmProbe)
    cmd.setPushData(&push, sizeof(push));
  else if(lights.directLightPso==&shaders.lightsVsm || lights.directLightPso==&shaders.lightsVsmHard)
    cmd.setPushData(&push, offsetof(Push, probeMode)); else
    cmd.setPushData(&push.originLwc, sizeof(push.originLwc));
  cmd.setPipeline(*lights.directLightPso);
  cmd.draw(nullptr,ibo, 0,ibo.size(), 0,wview.lights().size());
  }

void Renderer::prepareSSAO(Encoder<CommandBuffer>& cmd, WorldView& wview) {
  // a frame without SSAO breaks the history
  if(!settings.zCloudShadowScale) {
    ssao.valid = false;
    return;
    }
  // the probes of -gi 1 trace the occlusion of their light themselves: their ambient takes no SSAO (probe_ambient.frag)
  if(settings.giMethod==GiMethod::Probes) {
    ssao.valid = false;
    return;
    }
  // ssao: the projection comes from the scene (jittered, as the depth); the push carries the history's reprojection
  struct PushSsao {
    Matrix4x4 reproject;
    uint32_t  frame   = 0;
    uint32_t  reset   = 0;
    uint32_t  fullSet = 0; // the whole set this frame, no history
    } push;
  if(ssao.camCuts!=ssao.cuts || Taa::isCameraJump(ssao.view, ssao.prev))
    ssao.valid = false;
  // both ends with this frame's jitter, as the TAA does: a camera that stands still reads the same texel
  const auto jitter = taa.active ? Taa::jitterMatrix(taa.jitter, uint32_t(zbuffer.w()), uint32_t(zbuffer.h()))
                                 : Matrix4x4::mkIdentity();
  push.reproject = ssao.valid ? Taa::reprojection(ssao.view, ssao.prev, jitter) : Matrix4x4::mkIdentity();
  push.frame     = ssao.frame;
  push.reset     = ssao.valid && settings.ssaoHistory ? 0 : 1;
  push.fullSet   = settings.ssaoHistory ? 0 : 1;

  auto& device = Resources::device();
  const auto aoSize = effectResolution();
  if(ssao.ssaoBuf.size()!=aoSize || ssao.ssaoBlur.size()!=zbuffer.size() || ssao.depthHalf.size()!=aoSize) {
    Resources::recycle(std::move(ssao.ssaoBuf));
    Resources::recycle(std::move(ssao.ssaoBlur));
    Resources::recycle(std::move(ssao.depthHalf));
    Resources::recycle(std::move(ssao.history[0]));
    Resources::recycle(std::move(ssao.history[1]));
    ssao.ssaoBuf    = device.image2d(ssao.aoFormat, aoSize);
    ssao.ssaoBlur   = device.image2d(ssao.aoFormat, zbuffer.size());
    ssao.depthHalf  = device.image2d(TextureFormat::R32F, aoSize);
    ssao.history[0] = device.image2d(TextureFormat::RGBA16F, aoSize);
    ssao.history[1] = device.image2d(TextureFormat::RGBA16F, aoSize);
    ssao.valid      = false;
    push.reset      = 1;
    }

  cmd.setFramebuffer({});
  // depth at the SSAO size for the samples of ssao.comp: the full buffer misses the cache under their scatter
  cmd.setDebugMarker("SSAO-depth");
  cmd.setBinding(0, zbuffer, Sampler::nearest(ClampMode::ClampToEdge));
  cmd.setBinding(1, ssao.depthHalf);
  cmd.setPipeline(shaders.ssaoDepth);
  cmd.dispatchThreads(ssao.depthHalf.size());

  cmd.setDebugMarker("SSAO");

  cmd.setBinding(0, ssao.ssaoBuf);
  cmd.setBinding(1, wview.sceneGlobals().uboGlobal[SceneGlobals::V_Main]);
  cmd.setBinding(4, zbuffer,     Sampler::nearest(ClampMode::ClampToEdge));
  cmd.setBinding(5, ssao.depthHalf, Sampler::nearest(ClampMode::ClampToEdge));
  cmd.setBinding(6, ssao.history[ssao.cur^1], Sampler::nearest(ClampMode::ClampToEdge));
  cmd.setBinding(7, ssao.history[ssao.cur]);
  cmd.setPushData(&push, sizeof(push));
  cmd.setPipeline(shaders.ssao);
  cmd.dispatchThreads(ssao.ssaoBuf.size());
  ssao.prev  = ssao.view;
  ssao.cuts  = ssao.camCuts;
  ssao.valid = true;
  ssao.cur  ^= 1;
  ssao.frame++;

  cmd.setDebugMarker("SSAO-blur");
  cmd.setBinding(0, ssao.ssaoBlur);
  cmd.setBinding(1, wview.sceneGlobals().uboGlobal[SceneGlobals::V_Main]);
  cmd.setBinding(2, ssao.ssaoBuf, Sampler::bilinear(ClampMode::ClampToEdge));
  cmd.setBinding(3, zbuffer, Sampler::nearest(ClampMode::ClampToEdge));
  cmd.setPipeline(shaders.ssaoBlur);
  cmd.dispatchThreads(ssao.ssaoBlur.size());
  }

void Renderer::prepareIrradiance(Encoder<CommandBuffer>& cmd, WorldView& wview) {
  auto& scene = wview.sceneGlobals();

  cmd.setDebugMarker("Irradiance");
  cmd.setFramebuffer({});
  cmd.setBinding(0, sky.irradianceLut);
  cmd.setBinding(1, scene.uboGlobal[SceneGlobals::V_Main]);
  cmd.setBinding(2, sky.viewCldLut, sky.sampler);
  cmd.setPipeline(shaders.irradiance);
  cmd.dispatch(1);
  }


void Renderer::readGiStats(uint8_t fId) {
  // the counters this frame slot wrote last time: its fence has been waited for, the buffer is host-visible
  if(!FrameProfile::enabled() || !gi.statsWritten[fId] || gi.stats[fId].isEmpty())
    return;
  uint32_t v[RendererInternal::giStatsCount] = {};
  Resources::device().readBytes(gi.stats[fId], v, sizeof(v));
  FrameProfile::add(FrameProfile::Amount::GiProbesLive,     v[0]);
  FrameProfile::add(FrameProfile::Amount::GiProbesTraced,   v[1]);
  FrameProfile::add(FrameProfile::Amount::GiProbesFromHits, v[4]);
  FrameProfile::add(FrameProfile::Amount::GiProbesFreed,    v[5]);
  gi.statsWritten[fId] = false;
  }

Tempest::Attachment Renderer::screenshoot(uint8_t frameId) {
  auto& device = Resources::device();
  device.waitIdle();

  uint32_t w    = uint32_t(zbuffer.w());
  uint32_t h    = uint32_t(zbuffer.h());
  if(!temporal.output.isEmpty()) {
    // the temporal upscale makes the frame of the swapchain's size; a smaller one would start it over
    w = uint32_t(temporal.output.w());
    h = uint32_t(temporal.output.h());
    }
  auto     img  = device.attachment(Tempest::TextureFormat::RGBA8,w,h);

  auto wview  = Gothic::inst().worldView();
  auto camera = Gothic::inst().camera();
  if(wview==nullptr || camera==nullptr)
    return Attachment();

  CommandBuffer cmd;
  {
  auto enc = cmd.startEncoding(device);
  taa.shot = true;
  draw(img,enc,frameId,*wview,*camera);
  taa.shot = false;
  }

  auto sync = device.submit(cmd);
  sync.wait();
  return img;

  // debug
  auto d16     = device.attachment(TextureFormat::R16,    w, h);
  auto normals = device.attachment(TextureFormat::RGBA16, w, h);

  {
  auto enc = cmd.startEncoding(device);
  enc.setFramebuffer({{normals,Tempest::Discard,Tempest::Preserve}});
  enc.setBinding(0, gbufNormal, Sampler::nearest());
  enc.setPipeline(shaders.copy);
  enc.draw(nullptr, 0, 3);

  enc.setFramebuffer({{d16,Tempest::Discard,Tempest::Preserve}});
  enc.setBinding(0,zbuffer,Sampler::nearest());
  enc.setPipeline(shaders.copy);
  enc.draw(nullptr, 0, 3);
  }
  sync = device.submit(cmd);
  sync.wait();

  auto pm  = device.readPixels(textureCast<const Texture2d&>(normals));
  pm.save("gbufNormal.png");

  pm  = device.readPixels(textureCast<const Texture2d&>(d16));
  pm.save("zbuffer.hdr");

  return img;
  }

float Renderer::internalResolutionScale() const {
  if(settings.renderScale>0)
    return settings.renderScale;
  if(settings.vidResIndex==0)
    return 1;
  if(settings.vidResIndex==1)
    return 0.75;
  return 0.5;
  }

Size Renderer::effectResolution() const {
  if(!settings.halfResAoFog)
    return zbuffer.size();
  return Size((zbuffer.w()+1)/2, (zbuffer.h()+1)/2);
  }

Size Renderer::internalResolution(Tempest::Size src) const {
  if(settings.renderScale>0)
    return Size(std::max(1, int(float(src.w)*settings.renderScale)), std::max(1, int(float(src.h)*settings.renderScale)));
  if(settings.vidResIndex==0)
     return src;
  if(settings.vidResIndex==1)
    return Size(3*src.w/4, 3*src.h/4);
  return Size(src.w/2, src.h/2);
  }

