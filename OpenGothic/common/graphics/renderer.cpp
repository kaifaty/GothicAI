#include "renderer.h"
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

static const bool skyPathTrace = false;

static uint32_t nextPot(uint32_t x) {
  x--;
  x |= x >> 1;
  x |= x >> 2;
  x |= x >> 4;
  x |= x >> 8;
  x |= x >> 16;
  x++;
  return x;
  }

static float smoothstep(float edge0, float edge1, float x) {
  float t = std::min(std::max((x - edge0) / (edge1 - edge0), 0.f), 1.f);
  return t * t * (3.f - 2.f * t);
  };

static float linearstep(float edge0, float edge1, float x) {
  float t = std::min(std::max((x - edge0) / (edge1 - edge0), 0.f), 1.f);
  return t;
  };

static Size tileCount(Size sz, int s) {
  sz.w = (sz.w+s-1)/s;
  sz.h = (sz.h+s-1)/s;
  return sz;
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

void Renderer::toggleGi() {
  auto& device = Resources::device();
  if(!Shaders::options().doRtScene)
    return;

  if(settings.giMethod==GiMethod::None && Gothic::options().doGi!=GiMethod::None)
    settings.giMethod = Gothic::options().doGi;
  else if(settings.giMethod==GiMethod::None && Shaders::isGi1Supported())
    settings.giMethod = GiMethod::Probes; // default for now
  else
    settings.giMethod = GiMethod::None;

  device.waitIdle();
  setupSettings();
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
  gi.resetHistory      = true;
  resetSkyFog();
  }

void Renderer::resetTemporal() {
  taa.valid = false;
  taa.frame = 0;
  taa.cur   = 0;
  }

void Renderer::updateCamera(const WorldView& wview, const Camera& camera) {
  proj        = camera.projective();
  viewProj    = camera.viewProj();
  viewProjLwc = camera.viewProjLwc();

  for(size_t i=0; i<Resources::ShadowLayers; ++i)
    shadowMatrix[i] = camera.viewShadow(wview.mainLight().dir(),i);
  shadowMatrixVsm = camera.viewShadowVsm(wview.mainLight().dir());

  auto zNear = camera.zNear();
  auto zFar  = camera.zFar();
  clipInfo.x = zNear*zFar;
  clipInfo.y = zNear-zFar;
  clipInfo.z = zFar;
  }

void Renderer::prepareTaa(WorldView& wview, const Camera& camera) {
  // the path tracer accumulates its own frames, with a camera that stands still
  // the temporal upscale (graphics plan 1g) takes the jitter and the history of the TAA in its own way
  temporal.active = settings.taaEnabled && !settings.pathTraceEnabled && !temporal.output.isEmpty();
  taa.active      = settings.taaEnabled && !settings.pathTraceEnabled && (!taa.output.isEmpty() || temporal.active);
  if(!taa.active) {
    taa.valid = false;
    wview.setJitter(nullptr);
    wview.setMipBias(0);
    wview.setTaaFrame(false, 0);
    return;
    }
  wview.setMipBias(temporal.active ? temporal.mipBias : settings.taaMipBias);

  // without jitter, as the camera gives it; the shadows (Camera::viewShadow*) are made from it too
  Taa::View cur;
  cur.proj    = proj;
  cur.viewLwc = camera.viewLwc();
  cur.origin  = camera.originLwc();
  if(camera.cuts()!=taa.cuts || Taa::isCameraJump(cur, taa.prev))
    taa.valid = false;
  taa.cuts = camera.cuts();

  wview.setTaaFrame(settings.vsmLampTaa, taa.frame);
  const Vec2 px = temporal.active ? Taa::jitterPx(taa.frame++, temporal.phases) : Taa::jitterPx(taa.frame++);
  const auto j  = Taa::jitterMatrix(px, uint32_t(zbuffer.w()), uint32_t(zbuffer.h()));
  taa.jitter    = px;
  if(taa.valid)
    taa.reproject = Taa::reprojection(cur, taa.prev, j);
  taa.prev = cur;

  // the main view only: the frustum, SSAO and everything in SceneGlobals; not the shadows, UI or inventory
  auto m = j; m.mul(proj);        proj        = m;
  m      = j; m.mul(viewProj);    viewProj    = m;
  m      = j; m.mul(viewProjLwc); viewProjLwc = m;
  wview.setJitter(&j);
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
  const bool forceSm1 = (settings.giMethod==GiMethod::IrrC || settings.pathTraceEnabled || sky.quality==PathTrace);
  for(int i=0; i<Resources::ShadowLayers; ++i) {
    if(!(i==1 && forceSm1)) {
      if(settings.vsmEnabled)
        continue; //TODO: support vsm in gi code
      }
    shadowMap[i] = device.zbuffer(shadowFormat, settings.shadowResolution, settings.shadowResolution);
    }
  }

void Renderer::resetSkyFog() {
  auto& device = Resources::device();

  {
    auto q = Quality::VolumetricLQ;
    if(!settings.zFogRadial) {
      q = Quality::VolumetricLQ;
      } else {
      q = Quality::VolumetricHQ;
      // q = Quality::Epipolar;
      }

    if(skyPathTrace)
      q = PathTrace;

    if(sky.quality==q) {
      return;
      }

    sky.quality = q;
  }

  Resources::recycle(std::move(sky.fogLut3D));
  Resources::recycle(std::move(sky.fogLut3DMs));

  sky.lutIsInitialized = false;

  switch(sky.quality) {
    case None:
    case VolumetricLQ:
      sky.fogLut3D   = device.image3d(sky.lutRGBAFormat, 160, 90, 64);
      break;
    case VolumetricHQ:
      // fogLut and oclussion are decupled
      sky.fogLut3D   = device.image3d(sky.lutRGBFormat,  128,64,32);
      sky.fogLut3DMs = device.image3d(sky.lutRGBAFormat, 128,64,32);
      break;
    case Epipolar:
      sky.fogLut3D   = device.image3d(sky.lutRGBFormat,  128,64,32);
      sky.fogLut3DMs = device.image3d(sky.lutRGBAFormat, 128,64,32);
      break;
    case PathTrace:
      break;
    }
  }

void Renderer::prepareSky(Tempest::Encoder<Tempest::CommandBuffer>& cmd, WorldView& wview) {
  auto& scene   = wview.sceneGlobals();

  cmd.setDebugMarker("Sky LUT");
  if(!sky.lutIsInitialized) {
    sky.lutIsInitialized = true;

    cmd.setFramebuffer({});
    cmd.setBinding(0, sky.cloudsLut);
    cmd.setBinding(5, *wview.sky().cloudsDay()  .lay[0], Sampler::trillinear());
    cmd.setBinding(6, *wview.sky().cloudsDay()  .lay[1], Sampler::trillinear());
    cmd.setBinding(7, *wview.sky().cloudsNight().lay[0], Sampler::trillinear());
    cmd.setBinding(8, *wview.sky().cloudsNight().lay[1], Sampler::trillinear());
    cmd.setPipeline(shaders.cloudsLut);
    cmd.dispatchThreads(size_t(sky.cloudsLut.w()), size_t(sky.cloudsLut.h()));

    auto sz = Vec2(float(sky.transLut.w()), float(sky.transLut.h()));
    cmd.setFramebuffer({{sky.transLut, Tempest::Discard, Tempest::Preserve}});
    cmd.setBinding(5, *wview.sky().cloudsDay()  .lay[0], Sampler::trillinear());
    cmd.setBinding(6, *wview.sky().cloudsDay()  .lay[1], Sampler::trillinear());
    cmd.setBinding(7, *wview.sky().cloudsNight().lay[0], Sampler::trillinear());
    cmd.setBinding(8, *wview.sky().cloudsNight().lay[1], Sampler::trillinear());
    cmd.setPushData(&sz, sizeof(sz));
    cmd.setPipeline(shaders.skyTransmittance);
    cmd.draw(nullptr, 0, 3);

    sz = Vec2(float(sky.multiScatLut.w()), float(sky.multiScatLut.h()));
    cmd.setFramebuffer({{sky.multiScatLut, Tempest::Discard, Tempest::Preserve}});
    cmd.setBinding(0, sky.transLut, sky.sampler);
    cmd.setPushData(&sz, sizeof(sz));
    cmd.setPipeline(shaders.skyMultiScattering);
    cmd.draw(nullptr, 0, 3);
    }

  auto sz = Vec2(float(sky.viewLut.w()), float(sky.viewLut.h()));
  cmd.setFramebuffer({{sky.viewLut, Tempest::Discard, Tempest::Preserve}});
  cmd.setBinding(0, scene.uboGlobal[SceneGlobals::V_Main]);
  cmd.setBinding(1, sky.transLut,     sky.sampler);
  cmd.setBinding(2, sky.multiScatLut, sky.sampler);
  cmd.setBinding(3, sky.cloudsLut,    Sampler::bilinear(ClampMode::ClampToEdge));
  cmd.setPushData(&sz, sizeof(sz));
  cmd.setPipeline(shaders.skyViewLut);
  cmd.draw(nullptr, 0, 3);

  sz = Vec2(float(sky.viewCldLut.w()), float(sky.viewCldLut.h()));
  cmd.setFramebuffer({{sky.viewCldLut, Tempest::Discard, Tempest::Preserve}});
  cmd.setBinding(0, scene.uboGlobal[SceneGlobals::V_Main]);
  cmd.setBinding(1, sky.viewLut, sky.sampler);
  cmd.setBinding(2, *wview.sky().cloudsDay()  .lay[0], Sampler::trillinear());
  cmd.setBinding(3, *wview.sky().cloudsDay()  .lay[1], Sampler::trillinear());
  cmd.setBinding(4, *wview.sky().cloudsNight().lay[0], Sampler::trillinear());
  cmd.setBinding(5, *wview.sky().cloudsNight().lay[1], Sampler::trillinear());
  cmd.setPushData(&sz, sizeof(sz));
  cmd.setPipeline(shaders.skyViewCldLut);
  cmd.draw(nullptr, 0, 3);
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

void Renderer::drawBloom(Encoder<CommandBuffer>& cmd, bool fused) {
  if(!settings.bloomEnabled || bloomChain[0].isEmpty())
    return;
  cmd.setDebugMarker("Bloom");
  // down: the scene to half resolution (Karis-weighted), then each level to the next
  for(size_t i=0; i<bloomChain.size(); ++i) {
    cmd.setFramebuffer({{bloomChain[i], Tempest::Discard, Tempest::Preserve}});
    if(i==0 && temporal.active)
      cmd.setBinding(0, temporal.output, Sampler::bilinear(ClampMode::ClampToEdge));
    else if(i==0 && fused)
      cmd.setBinding(0, taa.history[taa.cur^1], Sampler::bilinear(ClampMode::ClampToEdge));
    else
      cmd.setBinding(0, i==0 ? sceneLinear : bloomChain[i-1], Sampler::bilinear(ClampMode::ClampToEdge));
    cmd.setPipeline(i==0 ? shaders.bloomDownKaris : shaders.bloomDown);
    cmd.draw(nullptr, 0, 3);
    }
  // up: each level adds its tent-filtered smaller one, so the half level ends up with all of them
  for(size_t i=bloomChain.size()-1; i>0; --i) {
    cmd.setFramebuffer({{bloomChain[i-1], Tempest::Preserve, Tempest::Preserve}});
    cmd.setBinding(0, bloomChain[i], Sampler::bilinear(ClampMode::ClampToEdge));
    cmd.setPipeline(shaders.bloomUp);
    cmd.draw(nullptr, 0, 3);
    }
  if(temporal.active || fused)
    return; // the tonemapping mixes it in (drawTonemappingTemporal, drawTonemappingTaa)
  struct Push {
    float strength = 0.05f;  // the share of the scene's light a lens scatters into the glow
    float levels   = 0;
    } push;
  push.levels = float(bloomChain.size());
  cmd.setFramebuffer({{sceneLinear, Tempest::Preserve, Tempest::Preserve}});
  cmd.setBinding(0, bloomChain[0], Sampler::bilinear(ClampMode::ClampToEdge));
  cmd.setPushData(push);
  cmd.setPipeline(shaders.bloomMix);
  cmd.draw(nullptr, 0, 3);
  }

void Renderer::drawTonemapping(Attachment& result, Encoder<CommandBuffer>& cmd, const WorldView& wview) {
  struct Push {
    float brightness = 0;
    float contrast   = 1;
    float gamma      = 1.f/2.2f;
    float mul        = 1;
    };

  Push p;
  p.brightness = (settings.zVidBrightness - 0.5f)*0.1f;
  p.contrast   = std::max(1.5f - settings.zVidContrast, 0.01f);
  p.gamma      = p.gamma/std::max(2.0f*settings.zVidGamma,  0.01f);

  static float mul = 0.f;
  if(mul>0)
    p.mul = mul;

  const bool native = (result.size()==sceneLinear.size());
  auto& pso = native ? shaders.tonemapping : shaders.tonemappingUpscale;
  cmd.setFramebuffer({ {result, Tempest::Discard, Tempest::Preserve} });
  cmd.setBinding(0, wview.sceneGlobals().uboGlobal[SceneGlobals::V_Main]);
  // This Lanczos implementation pairs adjacent weights using bilinear sampling.
  // Keep nearest sampling at native resolution.
  const auto sampler = native ? Sampler::nearest(ClampMode::ClampToEdge) :
                                Sampler::bilinear(ClampMode::ClampToEdge);
  cmd.setBinding(1, sceneLinear, sampler);
  cmd.setPushData(p);
  cmd.setPipeline(pso);
  cmd.draw(nullptr, 0, 3);
  }

void Renderer::drawCMAA2(Tempest::Attachment& result, Tempest::Encoder<Tempest::CommandBuffer>& cmd, const WorldView& wview) {
  auto&          pso             = shaders.cmaa2EdgeColor2x2Presets[Gothic::options().aaPreset];
  const IVec3    inputGroupSize  = pso.workGroupSize();
  const IVec3    outputGroupSize = inputGroupSize - IVec3(2, 2, 0);
  const uint32_t groupCountX     = uint32_t((sceneLinear.w() + outputGroupSize.x * 2 - 1) / (outputGroupSize.x * 2));
  const uint32_t groupCountY     = uint32_t((sceneLinear.h() + outputGroupSize.y * 2 - 1) / (outputGroupSize.y * 2));

  cmd.setFramebuffer({});
  cmd.setBinding(0, sceneLinear, Sampler::bilinear(ClampMode::ClampToEdge));
  cmd.setBinding(1, cmaa2.workingEdges);
  cmd.setBinding(2, cmaa2.shapeCandidates);
  cmd.setBinding(3, cmaa2.deferredBlendLocationList);
  cmd.setBinding(4, cmaa2.deferredBlendItemList);
  cmd.setBinding(5, cmaa2.deferredBlendItemListHeads);
  cmd.setBinding(6, cmaa2.controlBuffer);
  cmd.setBinding(7, cmaa2.indirectBuffer);

  // detect edges
  cmd.setPipeline(pso);
  cmd.dispatch(groupCountX, groupCountY, 1);

  // process candidates pass
  cmd.setPipeline(shaders.cmaa2ProcessCandidates);
  cmd.dispatchIndirect(cmaa2.indirectBuffer, 0);

  // deferred color apply
  struct Push {
    float brightness = 0;
    float contrast   = 1;
    float gamma      = 1.f/2.2f;
    float mul        = 1;
    };

  Push p;
  p.brightness = (settings.zVidBrightness - 0.5f)*0.1f;
  p.contrast   = std::max(1.5f - settings.zVidContrast, 0.01f);
  p.gamma      = p.gamma/std::max(2.0f*settings.zVidGamma,  0.01f);

  static float mul = 0.f;
  if(mul>0)
    p.mul = mul;

  // CMAA2 writes pixels in sceneLinear coordinates: result must have the same size
  auto& psoTone = shaders.tonemapping;
  cmd.setFramebuffer({{result, Tempest::Discard, Tempest::Preserve}});
  cmd.setBinding(0, wview.sceneGlobals().uboGlobal[SceneGlobals::V_Main]);
  cmd.setBinding(1, sceneLinear, Sampler::nearest());
  cmd.setPushData(&p, sizeof(p));
  cmd.setPipeline(psoTone);
  cmd.draw(nullptr, 0, 3);

  cmd.setBinding(0, sceneLinear);
  cmd.setBinding(1, cmaa2.workingEdges);
  cmd.setBinding(2, cmaa2.shapeCandidates);
  cmd.setBinding(3, cmaa2.deferredBlendLocationList);
  cmd.setBinding(4, cmaa2.deferredBlendItemList);
  cmd.setBinding(5, cmaa2.deferredBlendItemListHeads);
  cmd.setBinding(6, cmaa2.controlBuffer);
  cmd.setPushData(&p, sizeof(p));
  cmd.setPipeline(shaders.cmaa2DeferredColorApply2x2);
  cmd.drawIndirect(cmaa2.indirectBuffer, 3*sizeof(uint32_t));
  }

void Renderer::drawTaa(Tempest::Encoder<Tempest::CommandBuffer>& cmd, bool fused) {
  // In linear HDR after the translucent and underwater passes, before bloom (shader/antialiasing/taa_resolve.frag)
  struct Push {
    Matrix4x4 reproject;
    float     alpha = 0.1f; // the share of the new frame
    uint32_t  reset = 0;    // no history: the frame as it is
    } push;
  push.reproject = taa.reproject;
  push.reset     = taa.valid ? 0 : 1;

  auto& next = taa.history[taa.cur];
  auto& prev = taa.history[taa.cur^1];
  cmd.setDebugMarker("TAA");
  cmd.setFramebuffer({{next, Tempest::Discard, Tempest::Preserve}});
  cmd.setBinding(0, sceneLinear, Sampler::nearest(ClampMode::ClampToEdge));
  cmd.setBinding(1, zbuffer,     Sampler::nearest(ClampMode::ClampToEdge));
  cmd.setBinding(2, prev,        Sampler::bilinear(ClampMode::ClampToEdge));
  cmd.setPushData(push);
  cmd.setPipeline(shaders.taaResolve);
  cmd.draw(nullptr, 0, 3);
  taa.valid = true;

  if(fused) {
    taa.cur ^= 1; // the frame stays in the history: drawTonemappingTaa sharpens it on the way out
    return;
    }

  // the history stays unsharpened: sharpening fed back would build up from frame to frame
  drawTaaSharpen(cmd);

  // bloom and tonemapping go on from the resolved frame
  std::swap(sceneLinear, taa.output);
  taa.cur ^= 1;
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

void Renderer::drawTemporalUpscale(Tempest::Encoder<Tempest::CommandBuffer>& cmd) {
  // In linear HDR after the translucent and underwater passes, where the TAA resolve would be (graphics plan 1g).
  // The camera's motion of each pixel first: from its depth to where it was a frame ago (motion_camera.frag)
  struct Push {
    Matrix4x4 reproject;
    } push;
  push.reproject = taa.valid ? taa.reproject : Matrix4x4::mkIdentity();
  cmd.setDebugMarker("Upscale-motion");
  cmd.setFramebuffer({{temporal.motion, Tempest::Discard, Tempest::Preserve}});
  cmd.setBinding(0, zbuffer, Sampler::nearest(ClampMode::ClampToEdge));
  cmd.setPushData(push);
  cmd.setPipeline(shaders.motionCamera);
  cmd.draw(nullptr, 0, 3);

  // Apple's conventions: the jitter is the pixel's centre minus where it was sampled, in input pixels: the picture
  // moved by +jitter (Taa::jitterMatrix), so the centre shows the point at centre - jitter. The vectors are in the
  // input's texture coordinates, the scale makes pixels of them. The depth is not reversed: the sky is 1.
  // OPENGOTHIC_UPSCALE_JITTER_SIGN=<x>,<y> (a study of the convention): multipliers of the jitter given to MetalFX
  static const Vec2 sign = [] {
    Vec2 s(1, 1);
    if(const char* e = std::getenv("OPENGOTHIC_UPSCALE_JITTER_SIGN"))
      if(std::sscanf(e, "%f,%f", &s.x, &s.y)==2)
        Log::i("render: temporal upscale jitter sign = ", s.x, ",", s.y);
    return s;
    }();
  TemporalUpscaleDesc desc;
  desc.jitterX       = taa.jitter.x*sign.x;
  desc.jitterY       = taa.jitter.y*sign.y;
  desc.motionScaleX  = float(temporal.motion.w());
  desc.motionScaleY  = float(temporal.motion.h());
  desc.inputW        = uint32_t(sceneLinear.w());
  desc.inputH        = uint32_t(sceneLinear.h());
  desc.reset         = !taa.valid;
  desc.depthReversed = false;
  cmd.setFramebuffer({});
  cmd.setDebugMarker("Upscale-temporal");
  cmd.upscaleTemporal(sceneLinear, zbuffer, temporal.motion, temporal.output, desc);
  taa.valid = true;
  }

void Renderer::drawTonemappingTaa(Attachment& result, Encoder<CommandBuffer>& cmd, const WorldView& wview) {
  // drawTaaSharpen, the mix of drawBloom and drawTonemapping in one pass (tonemapping.frag BLOOM, RCAS)
  struct Push {
    float brightness    = 0;
    float contrast      = 1;
    float gamma         = 1.f/2.2f;
    float mul           = 1;
    float bloomStrength = 0.05f; // as drawBloom's mix
    float bloomLevels   = 1;
    float sharpness     = 0;
    } p;
  p.brightness  = (settings.zVidBrightness - 0.5f)*0.1f;
  p.contrast    = std::max(1.5f - settings.zVidContrast, 0.01f);
  p.gamma       = p.gamma/std::max(2.0f*settings.zVidGamma,  0.01f);
  p.bloomLevels = float(bloomChain.size());
  p.sharpness   = settings.taaSharpness;

  cmd.setFramebuffer({ {result, Tempest::Discard, Tempest::Preserve} });
  cmd.setBinding(0, wview.sceneGlobals().uboGlobal[SceneGlobals::V_Main]);
  cmd.setBinding(1, taa.history[taa.cur^1], Sampler::nearest(ClampMode::ClampToEdge));
  cmd.setBinding(2, bloomChain[0], Sampler::bilinear(ClampMode::ClampToEdge));
  cmd.setPushData(p);
  cmd.setPipeline(shaders.tonemappingTaa);
  cmd.draw(nullptr, 0, 3);
  }

void Renderer::drawTonemappingTemporal(Attachment& result, Encoder<CommandBuffer>& cmd, const WorldView& wview) {
  // drawTonemapping at the output resolution, from the upscaled frame; the bloom goes in here (tonemapping.frag BLOOM)
  struct Push {
    float brightness    = 0;
    float contrast      = 1;
    float gamma         = 1.f/2.2f;
    float mul           = 1;
    float bloomStrength = 0.05f; // as drawBloom's mix
    float bloomLevels   = 1;
    } p;
  p.brightness = (settings.zVidBrightness - 0.5f)*0.1f;
  p.contrast   = std::max(1.5f - settings.zVidContrast, 0.01f);
  p.gamma      = p.gamma/std::max(2.0f*settings.zVidGamma,  0.01f);

  const bool bloom = settings.bloomEnabled && !bloomChain[0].isEmpty();
  cmd.setFramebuffer({ {result, Tempest::Discard, Tempest::Preserve} });
  cmd.setBinding(0, wview.sceneGlobals().uboGlobal[SceneGlobals::V_Main]);
  cmd.setBinding(1, temporal.output, Sampler::nearest(ClampMode::ClampToEdge));
  if(bloom) {
    p.bloomLevels = float(bloomChain.size());
    cmd.setBinding(2, bloomChain[0], Sampler::bilinear(ClampMode::ClampToEdge));
    cmd.setPushData(p);
    cmd.setPipeline(shaders.tonemappingBloom);
    } else {
    cmd.setPushData(&p, 4*sizeof(float));
    cmd.setPipeline(shaders.tonemapping);
    }
  cmd.draw(nullptr, 0, 3);
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

void Renderer::drawFog(Tempest::Encoder<Tempest::CommandBuffer>& cmd, const WorldView& wview) {
  auto& scene = wview.sceneGlobals();

  switch(sky.quality) {
    case None:
    case VolumetricLQ: {
      cmd.setBinding(0, sky.fogLut3D, Sampler::bilinear(ClampMode::ClampToEdge));
      cmd.setBinding(1, sky.fogLut3D, Sampler::bilinear(ClampMode::ClampToEdge));
      cmd.setBinding(2, zbuffer, Sampler::nearest()); // NOTE: wanna here depthFetch from gles2
      cmd.setBinding(3, scene.uboGlobal[SceneGlobals::V_Main]);
      cmd.setPipeline(shaders.fog);
      break;
      }
    case VolumetricHQ: {
      cmd.setBinding(0, sky.fogLut3D,   Sampler::bilinear(ClampMode::ClampToEdge));
      cmd.setBinding(1, sky.fogLut3DMs, Sampler::bilinear(ClampMode::ClampToEdge));
      cmd.setBinding(2, zbuffer,        Sampler::nearest());
      cmd.setBinding(3, scene.uboGlobal[SceneGlobals::V_Main]);
      cmd.setBinding(4, sky.occlusionLut);
      cmd.setPipeline(shaders.fog3dHQ);
      break;
      }
    case Epipolar: {
      //cmd.setBinding(0, sky.fogLut3D,   Sampler::bilinear(ClampMode::ClampToEdge));
      cmd.setBinding(0, scene.uboGlobal[SceneGlobals::V_Main]);
      cmd.setBinding(1, zbuffer,        Sampler::nearest());
      cmd.setBinding(2, vsm.fogDbg,     Sampler::bilinear(ClampMode::ClampToEdge));
      cmd.setBinding(3, epipolar.epipoles);
      cmd.setBinding(4, sky.fogLut3DMs, Sampler::bilinear(ClampMode::ClampToEdge));
      cmd.setPipeline(shaders.vsmFog);
      break;
      }
    case PathTrace:
      return;
    }
  cmd.draw(nullptr, 0, 3);
  }

void Renderer::drawSunMoon(Tempest::Encoder<Tempest::CommandBuffer>& cmd, const WorldView& wview) {
  drawSunMoon(cmd, wview, false);
  drawSunMoon(cmd, wview, true);
  }

void Renderer::drawSunMoon(Tempest::Encoder<Tempest::CommandBuffer>& cmd, const WorldView& wview, bool isSun) {
  auto& scene = wview.sceneGlobals();
  auto& sun   = wview.sky().sunLight();
  auto  m     = scene.viewProject();
  auto  d     = sun.dir();

  if(!isSun) {
    // fixed pos for now
    d = Vec3::normalize({-1,1,0});
    }

  auto  dx = d;
  float w  = 0;
  m.project(dx.x, dx.y, dx.z, w);

  if(dx.z<=0)
    return;

  struct Push {
    Tempest::Vec2      pos;
    Tempest::Vec2      size;
    Tempest::Vec3      sunDir;
    float              GSunIntensity = 0;
    Tempest::Matrix4x4 viewProjectInv;
    uint32_t           isSun = 0;
    } push;
  push.pos     = Vec2(dx.x,dx.y)/dx.z;
  push.size.x  = 2.f/float(zbuffer.w());
  push.size.y  = 2.f/float(zbuffer.h());

  const float GSunIntensity  = wview.sky().sunIntensity();
  const float GMoonIntensity = wview.sky().moonIntensity();

  const float scale          = internalResolutionScale();
  const float sunSize        = settings.sunSize  * scale;
  const float moonSize       = settings.moonSize * scale;
  const float intencity      = isSun ? 0.07f : 0.4f;

  push.size          *= isSun ? sunSize : (moonSize*0.25f);
  push.GSunIntensity  = isSun ? (GSunIntensity*intencity) : (GMoonIntensity*intencity);
  push.isSun          = isSun ? 1 : 0;
  push.sunDir         = d;
  push.viewProjectInv = scene.viewProjectLwcInv();

  // HACK
  if(isSun) {
    float day = sun.dir().y;
    float stp = linearstep(-0.07f, 0.03f, day);
    push.GSunIntensity *= stp*stp*4.f;
    } else {
    push.GSunIntensity *= wview.sky().isNight();
    }
  // push.GSunIntensity *= exposure;

  cmd.setBinding(0, scene.uboGlobal[SceneGlobals::V_Main]);
  cmd.setBinding(1, isSun ? wview.sky().sunImage() : wview.sky().moonImage());
  cmd.setBinding(2, sky.transLut, sky.sampler);
  cmd.setPushData(push);
  cmd.setPipeline(shaders.sun);
  cmd.draw(nullptr, 0, 6);
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

void Renderer::drawVsmDbg(Tempest::Encoder<Tempest::CommandBuffer>& cmd, const WorldView& wview) {
  static bool enable = false;
  if(!enable || !settings.vsmEnabled)
    return;

  cmd.setFramebuffer({{sceneLinear, Tempest::Preserve, Tempest::Preserve}});
  cmd.setDebugMarker("VSM-dbg");
  cmd.setBinding(0, wview.sceneGlobals().uboGlobal[SceneGlobals::V_Main]);
  cmd.setBinding(1, gbufDiffuse, Sampler::nearest());
  cmd.setBinding(2, gbufNormal,  Sampler::nearest());
  cmd.setBinding(3, zbuffer,     Sampler::nearest());
  cmd.setBinding(4, vsm.pageTbl);
  cmd.setBinding(5, vsm.pageList);
  cmd.setBinding(6, vsm.pageData);
  cmd.setBinding(8, wview.sceneGlobals().vsmDbg);
  cmd.setPushData(&settings.vsmMipBias, sizeof(settings.vsmMipBias));
  cmd.setPipeline(shaders.vsmDbg);
  cmd.draw(nullptr, 0, 3);
  }

void Renderer::drawHashDbg(Attachment& result, Tempest::Encoder<Tempest::CommandBuffer>& cmd, const WorldView& wview) {
  static bool enable = true;
  if(!enable)
    return;

  auto& scene = wview.sceneGlobals();

  struct Push {
    Vec3 originLwc;
    } push;
  push.originLwc = scene.originLwc;

  cmd.setDebugMarker("Hash-dbg");
  cmd.setPushData(push);
  cmd.setBinding(0, scene.uboGlobal[SceneGlobals::V_Main]);
  cmd.setBinding(2, gbufNormal);
  cmd.setBinding(3, zbuffer);

  cmd.setFramebuffer({{result, Tempest::Preserve, Tempest::Preserve}});
  cmd.setPipeline(shaders.hashDbg);
  cmd.draw(nullptr, 0, 3);
  }

void Renderer::drawLightTreeDbg(Attachment& result, Tempest::Encoder<Tempest::CommandBuffer>& cmd, const WorldView& wview) {
  static bool enable = false;
  if(!enable)
    return;

  auto& scene = wview.sceneGlobals();

  struct Push {
    Vec3 originLwc;
    } push;
  push.originLwc = scene.originLwc;

  cmd.setDebugMarker("LightTree-dbg");
  cmd.setPushData(push);
  cmd.setBinding(0, scene.uboGlobal[SceneGlobals::V_Main]);
  cmd.setBinding(1, gbufNormal);
  cmd.setBinding(2, zbuffer);
  cmd.setBinding(3, lightsTree.bvh);

  cmd.setFramebuffer({{result, Tempest::Preserve, Tempest::Preserve}});
  cmd.setPipeline(shaders.lightsTreeDbg);
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

void Renderer::drawVsm(Tempest::Encoder<Tempest::CommandBuffer>& cmd, WorldView& wview) {
  if(!settings.vsmEnabled)
    return;
  FrameProfile::Scope timing(FrameProfile::Zone::EncodeShadows);

  static bool omniLights = true;
  const bool doVirtualFog = sky.quality!=VolumetricLQ && sky.quality!=PathTrace;

  auto& scene      = wview.sceneGlobals();
  auto& sceneUbo   = scene.uboGlobal[SceneGlobals::V_Main];
  auto& lightsSsbo = wview.lights().lightsSsbo();

  logVsmStats(wview);

  auto& vsmDbg   = usesImage2d(vsm.vsmDbg,   TextureFormat::R32U, 1, 1); // vsm_mark_omni_pages: debug writes are off
  auto& pageTbl  = usesImage3d(vsm.pageTbl,  TextureFormat::R32U, 32, 32, 16);
  auto& pageHiZ  = usesImage3d(vsm.pageHiZ,  TextureFormat::R32U, 32, 32, 16);
  auto& pageData = usesZBuffer(vsm.pageData, shadowFormat,        8192, 8192);

  auto  pageCount   = uint32_t(vsm.pageData.w()/VSM_PAGE_SIZE) * uint32_t(vsm.pageData.h()/VSM_PAGE_SIZE);
  auto& pageList    = usesSsbo(vsm.pageList,    shaders.vsmClear.sizeofBuffer(0, pageCount));
  auto& pageListTmp = usesSsbo(vsm.pageListTmp, shaders.vsmAllocPages.sizeofBuffer(3, pageCount));

  const uint32_t lightsTotal  = omniLights ? uint32_t(wview.lights().size()) : 0;
  const size_t   numOmniPages = lightsTotal*6;
  auto& pageTblOmni   = usesSsbo(vsm.pageTblOmni,   shaders.vsmClearOmni.sizeofBuffer(0, numOmniPages));
  auto& visibleLights = usesSsbo(vsm.visibleLights, shaders.vsmClearOmni.sizeofBuffer(1, lightsTotal ));

  wview.setVirtualShadowMap(true, pageData, pageTbl, pageHiZ, pageList);

  cmd.setFramebuffer({});
  cmd.setDebugMarker("VSM-pages");
  cmd.setBinding(0, pageList);
  cmd.setBinding(1, pageTbl);
  cmd.setBinding(2, pageHiZ);
  cmd.setPipeline(shaders.vsmClear);
  cmd.dispatchThreads(size_t(pageTbl.w()), size_t(pageTbl.h()), size_t(pageTbl.d()));

  if(omniLights) {
    cmd.setBinding(0, pageTblOmni);
    cmd.setBinding(1, visibleLights);
    cmd.setPipeline(shaders.vsmClearOmni);
    cmd.dispatchThreads(numOmniPages);

    struct Push { float znear; uint32_t lightsTotal; } push = {};
    push.znear       = scene.znear;
    push.lightsTotal = lightsTotal;
    cmd.setBinding(0, sceneUbo);
    cmd.setBinding(1, lightsSsbo);
    cmd.setBinding(2, visibleLights);
    cmd.setBinding(3, *scene.hiZ);
    cmd.setPushData(push);
    cmd.setPipeline(shaders.vsmCullLights);
    cmd.dispatchThreads(wview.lights().size());
    }

  cmd.setBinding(0, sceneUbo);
  cmd.setBinding(1, gbufDiffuse, Sampler::nearest());
  cmd.setBinding(2, gbufNormal,  Sampler::nearest());
  cmd.setBinding(3, zbuffer,     Sampler::nearest());
  cmd.setBinding(4, pageTbl);
  cmd.setBinding(5, pageHiZ);
  cmd.setPushData(settings.vsmMipBias);
  cmd.setPipeline(shaders.vsmMarkPages);
  cmd.dispatchThreads(zbuffer.size());

  if(omniLights) {
    struct Push { Vec3 originLwc; float znear; float vsmMipBias; } push = {};
    push.originLwc  = scene.originLwc;
    push.znear      = scene.znear;
    push.vsmMipBias = settings.vsmMipBias;
    cmd.setBinding(0, sceneUbo);
    cmd.setBinding(1, gbufDiffuse, Sampler::nearest());
    cmd.setBinding(2, gbufNormal,  Sampler::nearest());
    cmd.setBinding(3, zbuffer,     Sampler::nearest());
    cmd.setBinding(4, lightsSsbo);
    cmd.setBinding(5, visibleLights);
    cmd.setBinding(6, pageTblOmni);
    cmd.setBinding(7, vsmDbg);
    cmd.setBinding(8, pageList);
    cmd.setPushData(&push, sizeof(push));
    cmd.setPipeline(shaders.vsmMarkOmniPages);
    cmd.dispatchThreads(zbuffer.size());

    cmd.setBinding(0, pageTblOmni);
    cmd.setPushData(&lightsTotal, sizeof(lightsTotal));
    cmd.setPipeline(shaders.vsmPostprocessOmni);
    cmd.dispatchThreads(wview.lights().size());
    }

  // sky&fog
  if(doVirtualFog) {
    cmd.setDebugMarker("VSM-pages-epipolar");
    cmd.setBinding(0, epipolar.epTrace);
    cmd.setBinding(1, sceneUbo);
    cmd.setBinding(2, epipolar.epipoles);
    cmd.setBinding(3, pageTbl);
    cmd.setBinding(4, pageHiZ);
    cmd.setPipeline(shaders.vsmFogPages);
    cmd.dispatchThreads(epipolar.epTrace.size());
    }

  cmd.setDebugMarker("VSM-pages-alloc");
  if(true) {
    // clump
    cmd.setBinding(0, pageList);
    cmd.setBinding(1, pageTbl);
    cmd.setPipeline(shaders.vsmClumpPages);
    cmd.dispatchThreads(size_t(pageTbl.w()), size_t(pageTbl.h()), size_t(pageTbl.d()));
    }

  cmd.setBinding(0, pageList);
  cmd.setBinding(1, pageTbl);
  cmd.setBinding(2, pageTblOmni);
  cmd.setBinding(3, pageListTmp);
  cmd.setBinding(4, scene.vsmDbg);
  // list
  cmd.setPipeline(shaders.vsmListPages);
  if(omniLights)
    cmd.dispatch(size_t(pageTbl.d() + 1)); else
    cmd.dispatch(size_t(pageTbl.d()));

  cmd.setPipeline(shaders.vsmAllocPages);
  cmd.dispatch(1);

  // hor-merge
  cmd.setPipeline(shaders.vsmMergePages);
  cmd.dispatch(1);

  cmd.setDebugMarker("VSM-visibility");
  wview.visibilityVsm(cmd);

  cmd.setDebugMarker("VSM-rendering");
  cmd.setFramebuffer({}, {pageData, 0.f, Tempest::Preserve});
  wview.drawVsm(cmd);
  }

void Renderer::logVsmStats(const WorldView& wview) {
  // OPENGOTHIC_VSM_STATS=1: the previous frames' counters (VsmHeader, vsm_common.glsl) once in 5 s; a read waits
  // for the GPU, so without the variable nothing is read back
  if(!settings.vsmStats || vsm.pageList.isEmpty() || vsm.visibleLights.isEmpty())
    return;
  const uint64_t now = Application::tickCount();
  if(vsm.statsTime!=0 && now<vsm.statsTime+5000)
    return;
  vsm.statsTime = now;

  struct Header {
    uint32_t pageCount, meshletCount, counterM, counterV;
    uint32_t pagePerMip[16];
    int32_t  pageBbox[16][4];
    uint32_t pageOmniCount, pageListCount, pagePlaced, omniTileOverflow, omniTileLightsMax;
    } h = {};
  uint32_t visibleLights = 0;
  auto& device = Resources::device();
  device.waitIdle();
  device.readBytes(vsm.pageList,      &h,             sizeof(h));
  device.readBytes(vsm.visibleLights, &visibleLights, sizeof(visibleLights));

  const uint32_t poolPages = uint32_t(vsm.pageData.w()/VSM_PAGE_SIZE) * uint32_t(vsm.pageData.h()/VSM_PAGE_SIZE);
  const uint32_t dropped   = h.pageListCount>h.pagePlaced ? h.pageListCount-h.pagePlaced : 0;
  Log::i("vsm stats: pageCount = ", h.pageCount, "/", poolPages,
         ", entries = ", h.pageListCount, " (sun ", h.pageListCount-h.pageOmniCount, ", pageOmniCount ", h.pageOmniCount, ")",
         ", placed = ", h.pagePlaced, ", dropped = ", dropped,
         ", meshletCount = ", h.meshletCount, "/", DrawCommands::VsmMaxMeshlets,
         (h.meshletCount>DrawCommands::VsmMaxMeshlets ? " OVERFLOW" : ""),
         ", lights = ", visibleLights, "/", uint32_t(wview.lights().size()),
         ", tile lights max = ", h.omniTileLightsMax, ", tiles over limit = ", h.omniTileOverflow);

  // the lamp filter's counters of one frame (light_vsm_probe), then the next frame is counted
  if(lights.directLightPso!=&Shaders::inst().lightsVsmProbe || vsm.lampProbe.isEmpty())
    return;
  uint32_t p[16] = {};
  if(vsm.lampCounted) {
    device.readBytes(vsm.lampProbe, p, sizeof(p));
    const auto share = [](uint32_t a, uint32_t b) { return b==0 ? 0.0 : double(a)/double(b); };
    const uint32_t filtered = p[8]+p[9]+p[10]+p[11]+p[12]+p[13];
    Log::i("vsm lamp stats: pairs = ", p[0], ", pixels = ", p[1], ", lamps per pixel = ", share(p[0], p[1]),
           ", past faint = ", p[2], ", out after 4 taps = ", share(p[3]+p[4], p[2]),
           " (lit ", share(p[3], p[2]), ", blocked ", share(p[4], p[2]), ")",
           ", taps per pixel = ", share(p[5], p[2]),
           ", neighbour checks = ", share(p[6], p[5]), ", neighbour taps = ", share(p[7], p[5]));
    Log::i("vsm lamp filter: filtered = ", filtered, ", penumbra texels <=1 ", share(p[8], filtered),
           " <=1.5 ", share(p[9], filtered), " <=2 ", share(p[10], filtered), " <=3 ", share(p[11], filtered),
           " <=6 ", share(p[12], filtered), " >6 ", share(p[13], filtered),
           ", search at floor = ", share(p[14], p[3]+p[4]+filtered), ", four-tap filter = ", share(p[15], filtered));
    }
  std::memset(p, 0, sizeof(p));
  vsm.lampProbe.update(p, 0, sizeof(p));
  vsm.lampArmed = true;
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

void Renderer::drawSky(Encoder<CommandBuffer>& cmd, const WorldView& wview) {
  auto& scene = wview.sceneGlobals();

  cmd.setDebugMarker("Sky");
  if(sky.quality==PathTrace) {
    cmd.setBinding(0, scene.uboGlobal[SceneGlobals::V_Main]);
    cmd.setBinding(1, sky.transLut,     sky.sampler);
    cmd.setBinding(2, sky.multiScatLut, sky.sampler);
    cmd.setBinding(3, sky.cloudsLut,    Sampler::bilinear(ClampMode::ClampToEdge));
    cmd.setBinding(4, zbuffer, Sampler::nearest());
    cmd.setBinding(5, shadowMap[1], Resources::shadowSampler());
    cmd.setPipeline(shaders.skyPathTrace);
    cmd.draw(nullptr, 0, 3);
    return;
    }

  auto& skyShader = sky.quality==VolumetricLQ ? shaders.sky : shaders.skySep;
  cmd.setBinding(0, scene.uboGlobal[SceneGlobals::V_Main]);
  cmd.setBinding(1, sky.transLut,     sky.sampler);
  cmd.setBinding(2, sky.multiScatLut, sky.sampler);
  cmd.setBinding(3, sky.viewLut,      sky.sampler);
  cmd.setBinding(4, sky.fogLut3D,     Sampler::bilinear(ClampMode::ClampToEdge));
  if(sky.quality!=VolumetricLQ)
    cmd.setBinding(5, sky.fogLut3DMs, Sampler::bilinear(ClampMode::ClampToEdge));
  cmd.setBinding(6, *wview.sky().cloudsDay()  .lay[0], Sampler::trillinear());
  cmd.setBinding(7, *wview.sky().cloudsDay()  .lay[1], Sampler::trillinear());
  cmd.setBinding(8, *wview.sky().cloudsNight().lay[0], Sampler::trillinear());
  cmd.setBinding(9, *wview.sky().cloudsNight().lay[1], Sampler::trillinear());
  cmd.setPipeline(skyShader);
  cmd.draw(nullptr, 0, 3);
  }

void Renderer::prepareSSAO(Encoder<CommandBuffer>& cmd, WorldView& wview) {
  if(!settings.zCloudShadowScale)
    return;
  // the probes of -gi 1 trace the occlusion of their light themselves: their ambient takes no SSAO (probe_ambient.frag)
  if(settings.giMethod==GiMethod::Probes)
    return;
  // ssao
  struct PushSsao {
    Matrix4x4 proj;
    Matrix4x4 projInv;
    } push;
  push.proj    = proj;
  push.projInv = proj;
  push.projInv.inverse();

  auto& device = Resources::device();
  const auto aoSize = effectResolution();
  if(ssao.ssaoBuf.size()!=aoSize || ssao.ssaoBlur.size()!=zbuffer.size() || ssao.depthHalf.size()!=aoSize) {
    Resources::recycle(std::move(ssao.ssaoBuf));
    Resources::recycle(std::move(ssao.ssaoBlur));
    Resources::recycle(std::move(ssao.depthHalf));
    ssao.ssaoBuf   = device.image2d(ssao.aoFormat, aoSize);
    ssao.ssaoBlur  = device.image2d(ssao.aoFormat, zbuffer.size());
    ssao.depthHalf = device.image2d(TextureFormat::R32F, aoSize);
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
  cmd.setBinding(2, gbufDiffuse, Sampler::nearest(ClampMode::ClampToEdge));
  cmd.setBinding(3, gbufNormal,  Sampler::nearest(ClampMode::ClampToEdge));
  cmd.setBinding(4, zbuffer,     Sampler::nearest(ClampMode::ClampToEdge));
  cmd.setBinding(5, ssao.depthHalf, Sampler::nearest(ClampMode::ClampToEdge));
  cmd.setPushData(&push, sizeof(push));
  cmd.setPipeline(shaders.ssao);
  cmd.dispatchThreads(ssao.ssaoBuf.size());

  cmd.setDebugMarker("SSAO-blur");
  cmd.setBinding(0, ssao.ssaoBlur);
  cmd.setBinding(1, wview.sceneGlobals().uboGlobal[SceneGlobals::V_Main]);
  cmd.setBinding(2, ssao.ssaoBuf, Sampler::bilinear(ClampMode::ClampToEdge));
  cmd.setBinding(3, zbuffer, Sampler::nearest(ClampMode::ClampToEdge));
  cmd.setPipeline(shaders.ssaoBlur);
  cmd.dispatchThreads(ssao.ssaoBlur.size());
  }

void Renderer::prepareFog(Encoder<Tempest::CommandBuffer>& cmd, WorldView& wview) {
  auto& scene  = wview.sceneGlobals();
  auto& device = Resources::device();

  cmd.setDebugMarker("Fog-LUTs");
  if(sky.quality!=PathTrace) {
    auto& shader = sky.quality==VolumetricLQ ? shaders.fogViewLut3d : shaders.fogViewLutSep;
    cmd.setFramebuffer({});
    cmd.setBinding(0, scene.uboGlobal[SceneGlobals::V_Main]);
    cmd.setBinding(1, sky.transLut,     sky.sampler);
    cmd.setBinding(2, sky.multiScatLut, sky.sampler);
    cmd.setBinding(3, sky.cloudsLut,    Sampler::bilinear(ClampMode::ClampToEdge));
    cmd.setBinding(4, sky.fogLut3D,     Sampler::bilinear(ClampMode::ClampToEdge));
    if(sky.quality==VolumetricHQ || sky.quality==Epipolar)
      cmd.setBinding(5, sky.fogLut3DMs);
    cmd.setPipeline(shader);
    cmd.dispatchThreads(uint32_t(sky.fogLut3D.w()), uint32_t(sky.fogLut3D.h()));
    }

  if(settings.vsmEnabled && !settings.pathTraceEnabled && (sky.quality==VolumetricHQ || sky.quality==Epipolar)) {
    cmd.setFramebuffer({});
    cmd.setDebugMarker("VSM-epipolar-fog");
    cmd.setBinding(0, epipolar.epTrace);
    cmd.setBinding(1, wview.sceneGlobals().uboGlobal[SceneGlobals::V_Main]);
    cmd.setBinding(2, epipolar.epipoles);
    cmd.setBinding(3, vsm.pageTbl);
    cmd.setBinding(4, vsm.pageData);
    cmd.setPipeline(shaders.vsmFogShadow);
    cmd.dispatchThreads(epipolar.epTrace.size());
    }

  switch(sky.quality) {
    case None:
    case VolumetricLQ:
      break;
    case VolumetricHQ: {
      auto& occlusionLut = usesImage2d(sky.occlusionLut, TextureFormat::R32U, effectResolution());
      cmd.setDebugMarker("Fog-occlusion");
      if(settings.vsmEnabled && !settings.pathTraceEnabled) {
        cmd.setFramebuffer({});
        cmd.setBinding(0, occlusionLut);
        cmd.setBinding(1, epipolar.epTrace);
        cmd.setBinding(2, wview.sceneGlobals().uboGlobal[SceneGlobals::V_Main]);
        cmd.setBinding(3, epipolar.epipoles);
        cmd.setBinding(4, zbuffer);
        cmd.setPipeline(shaders.fogEpipolarOcclusion);
        cmd.dispatchThreads(occlusionLut.size());
        } else {
        // drawShadowMap cleared the map and drew nothing: the mask follows from the clear value alone
        const bool emptyMap = !(wview.mainLight().dir().y > Camera::minShadowY);
        cmd.setFramebuffer({});
        cmd.setBinding(2, zbuffer, Sampler::nearest());
        cmd.setBinding(3, scene.uboGlobal[SceneGlobals::V_Main]);
        cmd.setBinding(4, occlusionLut);
        if(!emptyMap)
          cmd.setBinding(5, shadowMap[1], Resources::shadowSampler());
        cmd.setPipeline(emptyMap ? shaders.fogOcclusionEmpty : shaders.fogOcclusion);
        cmd.dispatchThreads(occlusionLut.size());
        }
      break;
      }
    case Epipolar:{
      // experimental
      if(vsm.fogDbg.isEmpty())
        vsm.fogDbg = device.image2d(sky.lutRGBFormat, 1024, 2*1024);
      cmd.setFramebuffer({});
      cmd.setDebugMarker("VSM-trace");
      cmd.setBinding(0, vsm.fogDbg);
      cmd.setBinding(1, epipolar.epTrace);
      cmd.setBinding(2, wview.sceneGlobals().uboGlobal[SceneGlobals::V_Main]);
      cmd.setBinding(3, epipolar.epipoles);
      cmd.setBinding(4, zbuffer);
      cmd.setBinding(5, sky.transLut,   sky.sampler);
      cmd.setBinding(6, sky.cloudsLut,  Sampler::bilinear(ClampMode::ClampToEdge));
      cmd.setBinding(7, sky.fogLut3DMs, Sampler::bilinear(ClampMode::ClampToEdge));
      cmd.setPipeline(shaders.vsmFogTrace);
      cmd.dispatchThreads(epipolar.epTrace.size());
      break;
      }
    case PathTrace: {
      break;
      }
    }
  }

void Renderer::prepareEpipolar(Tempest::Encoder<Tempest::CommandBuffer>& cmd, WorldView& wview) {
  const bool doVolumetricFog = sky.quality!=VolumetricLQ && sky.quality!=PathTrace;
  if(!doVolumetricFog || !settings.vsmEnabled)
    return;

  auto& scene  = wview.sceneGlobals();

  auto& epTrace      = usesImage2d(epipolar.epTrace,  TextureFormat::R16,  1024, 2*1024);
  auto& epipoles     = usesSsbo   (epipolar.epipoles, shaders.fogEpipolarVsm.sizeofBuffer(3, size_t(epipolar.epTrace.h())));
  auto& occlusionLut = usesImage2d(sky.occlusionLut, TextureFormat::R32U, effectResolution());

  cmd.setDebugMarker("Fog-epipolar");
  cmd.setFramebuffer({});
  cmd.setBinding(0, occlusionLut);
  cmd.setBinding(1, epTrace);
  cmd.setBinding(2, scene.uboGlobal[SceneGlobals::V_Main]);
  cmd.setBinding(3, epipoles);
  cmd.setBinding(4, zbuffer);
  cmd.setPipeline(shaders.fogEpipolarVsm);
  cmd.dispatch(uint32_t(epTrace.h()));
  }

void Renderer::prepareLightsBvh(Tempest::Encoder<Tempest::CommandBuffer>& cmd, WorldView& wview) {
  if(!Shaders::isLightsTreeSupported())
    return;

  struct Push {
    uint32_t numLightsPot;
    uint32_t numLights;
    } push = {};
  const uint32_t numLights = uint32_t(wview.lights().size());
  push.numLightsPot = nextPot(numLights);
  push.numLights    = numLights;

  auto& lightsSsbo = wview.lights().lightsSsbo();
  auto& bvhMorton  = usesSsbo(lightsTree.bvhMorton,   shaders.lightsBvh.sizeofBuffer(1, push.numLightsPot));
  auto& bvhAlux    = usesSsbo(lightsTree.bvhAlux,     shaders.lightsBvh.sizeofBuffer(2, numLights * 2));
  auto& ctrl       = usesSsboInit(lightsTree.bvhCtrl, shaders.lightsBvh.sizeofBuffer(3, (numLights + 31 )/32));

  // the stochastic tree of the path tracer; the bounding boxes of the lamps' ranges for the surfels and the probes of
  // -gi 1 (all lamps at a hit, probe_lighting.comp)
  const bool ltree = settings.pathTraceEnabled;
  const bool lbvh  = (settings.giMethod==GiMethod::IrrC || settings.giMethod==GiMethod::Probes);

  cmd.setDebugMarker("LightsTree");
  cmd.setFramebuffer({});
  cmd.setPushData(push);
  cmd.setBinding(0, lightsSsbo);
  cmd.setBinding(1, bvhMorton);
  cmd.setBinding(2, bvhAlux);
  cmd.setBinding(3, ctrl);

  cmd.setPipeline(shaders.lightsMorton);
  cmd.dispatch(1); // single pass, for simplicity

  if(ltree) {
    auto& tree = usesSsbo(lightsTree.tree, shaders.lightsTree.sizeofBuffer(4, numLights * 2));
    cmd.setBinding(4, tree);
    cmd.setPipeline(shaders.lightsTopology);
    cmd.dispatchThreads(numLights);

    cmd.setPipeline(shaders.lightsTree);
    cmd.dispatchThreads(numLights);
    }

  if(lbvh) {
    auto& bvh = usesSsbo(lightsTree.bvh, shaders.lightsBvh.sizeofBuffer(4, numLights * 2));
    cmd.setBinding(4, bvh);
    cmd.setPipeline(shaders.lightsTopology);
    cmd.dispatchThreads(numLights);

    cmd.setPipeline(shaders.lightsBvh);
    cmd.dispatchThreads(numLights);
    }
  }

void Renderer::prepareSurfels(Tempest::Encoder<Tempest::CommandBuffer>& cmd, WorldView& wview) {
  static bool enable = true;
  if(!enable)
    return;

  if(settings.giMethod!=GiMethod::IrrC || !settings.zCloudShadowScale)
    return;

  const uint32_t maxSurfels = surf.maxSurfels;
  const int32_t  TileSize   = 96;
  const int32_t  LargeTile  = 128;
  const auto     binCount   = tileCount(zbuffer.size(), TileSize);

  auto& scene    = wview.sceneGlobals();
  auto& irrImage = usesImage2d (surf.irrImage, TextureFormat::RGBA16F, zbuffer.size());
  auto& surfels  = usesSsboInit(surf.surfels,  shaders.surfAlloc.sizeofBuffer(4, maxSurfels));

  auto& surfCnts = usesImage2d(surf.surfCnts, TextureFormat::R32U, binCount);
  auto& surfBins = usesImage2d(surf.surfBins, TextureFormat::R32U, binCount);
  auto& surfList = usesSsbo   (surf.surfList, 16*maxSurfels*sizeof(uint32_t));

  struct Push {
    Vec3     originLwc;
    uint32_t tileSize;
    uint32_t pass;
    } push = {};
  push.originLwc = scene.originLwc;
  push.tileSize  = uint32_t(TileSize);
  push.pass      = 0;

  cmd.setDebugMarker("Surfels");

  if(surf.gbuffFree.isEmpty()) {
    auto& gbuffFree = usesSsbo(surf.gbuffFree, shaders.surfFList.sizeofBuffer(0, maxSurfels));
    cmd.setPushData(surf.gbufTilesX);
    cmd.setBinding(0, gbuffFree);
    cmd.setPipeline(shaders.surfFList);
    cmd.dispatchThreads(maxSurfels);
    }

  // prev-frame
  {
    cmd.setBinding(0, scene.uboGlobal[SceneGlobals::V_Main]);
    cmd.setBinding(1, zbuffer);
    cmd.setBinding(2, surfels);

    cmd.setPipeline(shaders.surfUpdate);
    cmd.dispatchThreads(maxSurfels);

    cmd.setBinding(0, scene.uboGlobal[SceneGlobals::V_Main]);
    cmd.setBinding(1, hiz.hiZ);
    cmd.setBinding(2, zbuffer);
    cmd.setBinding(3, surfels);
    cmd.setBinding(4, surf.gbuffFree);
    cmd.setPushData(scene.znear);

    cmd.setPipeline(shaders.surfCulling);
    cmd.dispatchThreads(maxSurfels);

    cmd.setPipeline(shaders.surfCompact);
    cmd.dispatch(1);
  }

  static bool rays = true;
  if(rays) {
    surfelsTrace(cmd, wview, surfels, false);
    }

  static bool apply = true;
  if(apply) {
    surfelsBinning(cmd, wview, TileSize, false);
    surfelsApply(cmd, wview, TileSize, false);
    }

  // current-frame
  static bool alloc = true;
  if(alloc) {
    cmd.setPushData(push);
    cmd.setBinding(0, scene.uboGlobal[SceneGlobals::V_Main]);
    cmd.setBinding(1, irrImage);
    cmd.setBinding(2, gbufNormal);
    cmd.setBinding(3, zbuffer);
    cmd.setBinding(4, surfels);
    cmd.setBinding(5, surf.gbuffFree);

    const auto tc = tileCount(zbuffer.size(), LargeTile);
    cmd.setPipeline(shaders.surfAlloc);
    cmd.dispatch(tc);
    }

  static bool gc = true;
  if(gc) {
    surfelsBinning(cmd, wview, TileSize, false);

    cmd.setBinding(0, scene.uboGlobal[SceneGlobals::V_Main]);
    cmd.setBinding(3, surfels);
    //
    cmd.setBinding(6, surfCnts);
    cmd.setBinding(7, surfBins);
    cmd.setBinding(8, surfList);

    //cmd.setPushData(push);
    //cmd.setPipeline(shaders.surfBinSort); // assist with stable GC
    //cmd.dispatch(surfBins.size());

    cmd.setPushData(push);
    cmd.setPipeline(shaders.surfDecimate);
    cmd.dispatchThreads(maxSurfels);
    }

  if(rays) {
    surfelsTrace(cmd, wview, surfels, true);
    }

  if(apply) {
    surfelsBinning(cmd, wview, TileSize, true);
    surfelsApply(cmd, wview, TileSize, true);
    }
  }

void Renderer::surfelsApply(Tempest::Encoder<Tempest::CommandBuffer>& cmd, WorldView& wview, int32_t tileSize, bool postPass) {
  auto& scene = wview.sceneGlobals();

  auto& irrImage  = surf.irrImage;
  auto& surfels   = surf.surfels;
  auto& surfCnts  = surf.surfCnts;
  auto& surfBins  = surf.surfBins;
  auto& surfList  = surf.surfList;

  // auto& irrImage2 = usesAttachment(surf.irrImage2, TextureFormat::RGBA16F, zbuffer.size());
  // (void)irrImage2;

  struct Push {
    Vec3     originLwc;
    uint32_t tileSize;
    uint32_t pass;
    } push = {};
  push.originLwc = scene.originLwc;
  push.tileSize  = uint32_t(tileSize);
  push.pass      = postPass ? 1 : 0;

  cmd.setPushData(push);
  cmd.setBinding(0, scene.uboGlobal[SceneGlobals::V_Main]);
  cmd.setBinding(1, irrImage);
  cmd.setBinding(2, gbufNormal);
  cmd.setBinding(3, zbuffer);
  cmd.setBinding(4, surfels);
  //
  cmd.setBinding(6, surfCnts);
  cmd.setBinding(7, surfBins);
  cmd.setBinding(8, surfList);

  cmd.setPipeline(shaders.surfApply);
  cmd.dispatchThreads(zbuffer.size());
  }

void Renderer::surfelsBinning(Tempest::Encoder<Tempest::CommandBuffer>& cmd, WorldView& wview, int32_t tileSize, bool postPass) {
  const  uint32_t maxSurfels = surf.maxSurfels;

  auto& scene       = wview.sceneGlobals();
  auto& surfels     = surf.surfels;
  auto& binCtrl     = usesSsboInit(surf.surfBinsCtrl, sizeof(uint32_t));
  auto& surfCnts    = surf.surfCnts;
  auto& surfBins    = surf.surfBins;
  auto& surfList    = surf.surfList;

  struct Push {
    int32_t tileSize;
    int32_t allocPass;
    int32_t postPass;
    } push = {};
  push.tileSize = tileSize;
  push.postPass = postPass ? 1 : 0;

  cmd.setPushData(push);
  cmd.setBinding(0, scene.uboGlobal[SceneGlobals::V_Main]);
  cmd.setBinding(1, binCtrl);
  cmd.setBinding(2, surfels);
  cmd.setBinding(3, surfCnts);
  cmd.setBinding(4, surfBins);
  cmd.setBinding(5, surfList);

  cmd.setPipeline(shaders.surfBinClear);
  cmd.dispatchThreads(surfBins.size());

  push.allocPass = 0;
  cmd.setPushData(push);
  cmd.setPipeline(shaders.surfBinPass);
  cmd.dispatchThreads(maxSurfels); //TODO: indirect

  cmd.setPipeline(shaders.surfBinAlloc);
  cmd.dispatchThreads(surfBins.size());

  push.allocPass = 1;
  cmd.setPushData(push);
  cmd.setPipeline(shaders.surfBinPass);
  cmd.dispatchThreads(maxSurfels);
  }

void Renderer::surfelsTrace(Tempest::Encoder<Tempest::CommandBuffer>& cmd, WorldView& wview, StorageBuffer& surfels, bool postPass) {
  static int giMethod = 0;

  auto& scene = wview.sceneGlobals();

  const Tempest::ComputePipeline* pso = &shaders.surfRaycast;
  if(giMethod==1)
    pso = &shaders.surfPathtrace;

  const uint32_t gbufX = surf.gbufTilesX * uint32_t(surf.gbufTile.x);
  const uint32_t gbufY = surf.gbufTilesY * uint32_t(surf.gbufTile.y);

  auto& gbuffDiff = usesImage2d(surf.gbuffDiff, TextureFormat::RGBA8, gbufX, gbufY);
  auto& gbuffNorm = usesImage2d(surf.gbuffNorm, TextureFormat::RGBA8, gbufX, gbufY);
  auto& gbuffHitT = usesImage2d(surf.gbuffHitT, TextureFormat::R16,   gbufX, gbufY);

  const uint32_t pass = postPass ? 1 : 0;
  cmd.setPushData(pass);
  cmd.setBinding(0, scene.uboGlobal[SceneGlobals::V_Main]);
  cmd.setBinding(1, surfels);
  cmd.setBinding(2, sky.viewCldLut, sky.sampler);
  cmd.setBinding(3, gbuffDiff);
  cmd.setBinding(4, gbuffNorm);
  cmd.setBinding(5, gbuffHitT);
  cmd.setBinding(6, scene.rtScene.tlas);
  cmd.setBinding(7, Sampler::trillinear());
  cmd.setBinding(8, scene.rtScene.tex);
  cmd.setBinding(9, scene.rtScene.vbo);
  cmd.setBinding(10,scene.rtScene.ibo);
  cmd.setBinding(11,scene.rtScene.rtDesc);
  cmd.setBinding(12,shadowMap[1]);
  cmd.setBinding(13,lightsTree.bvh);

  cmd.setPipeline(*pso);
  const uint32_t offset = postPass ? sizeof(uint32_t) : 0;
  if(!(giMethod==0 && postPass==false)) {
    cmd.dispatchIndirect(surfels, offset);
    // cmd.dispatchThreads(maxSurfels);
    }

  if(giMethod==0) {
    cmd.setPipeline(shaders.surLighting);
    cmd.dispatchIndirect(surfels, offset);
    }
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
  uint32_t v[gi.statsCount] = {};
  Resources::device().readBytes(gi.stats[fId], v, sizeof(v));
  FrameProfile::add(FrameProfile::Amount::GiProbesLive,     v[0]);
  FrameProfile::add(FrameProfile::Amount::GiProbesTraced,   v[1]);
  FrameProfile::add(FrameProfile::Amount::GiProbesFromHits, v[4]);
  FrameProfile::add(FrameProfile::Amount::GiProbesFreed,    v[5]);
  gi.statsWritten[fId] = false;
  }

// Probes of -gi 1 independent of the view (step 2b of the ray-tracing revisit, step-2b.md «2б-1: сделано»):
// - giExpire: s, a probe nobody uses (no pixel, no sample of another probe) for longer is freed;
// - giReserve: places kept free for a frame's allocations, the probes of the oldest use go first to keep them;
// - giHitRadius: cm, a hit of a probe's ray this close to the camera without a probe requests one;
// - giMaxRequests: requests a frame (the probes allocated at hits in the next frame);
// - giOffPeriod: a probe no pixel takes is relit and integrated once in so many frames.
static constexpr float    giExpire      = 5.f;
static constexpr uint32_t giReserve     = 8192;
static constexpr float    giHitRadius   = 2000.f;
static constexpr uint32_t giMaxRequests = 512;
static constexpr uint32_t giOffPeriod   = 2;

void Renderer::prepareGi(Encoder<CommandBuffer>& cmd, uint8_t fId, WorldView& wview) {
  if(settings.giMethod!=GiMethod::Probes || !settings.zCloudShadowScale) {
    return;
    }
  readGiStats(fId);

  const bool fisrtFrame = gi.voteTable.isEmpty();

  auto& hashTable          = usesSsbo(gi.hashTable, 2'097'152*sizeof(uint32_t)); // 8MB
  auto& voteTable          = usesSsbo(gi.voteTable, gi.hashTable.byteSize());
  auto& probes             = usesSsbo(gi.probes,    gi.maxProbes*32 + 64);       // probes and header
  auto& freeList           = usesSsbo(gi.freeList,  gi.maxProbes*sizeof(uint32_t) + sizeof(int32_t));
  auto& newList            = usesSsbo(gi.newList,   gi.maxProbes*sizeof(uint32_t)); // allocated in the frame: traced
  // probe_aux.comp, Rbo: the ages of the live probes (64 buckets) and the requests of probes at hits (16 bytes each)
  auto& requests           = usesSsbo(gi.requests,  (64 + giMaxRequests*4)*sizeof(uint32_t));
  // a flag per 16x8 tile of the screen (the allocation's workgroup): a missing probe was voted for there
  auto& voteTiles          = usesSsbo(gi.voteTiles, ((uint32_t(zbuffer.w())+15)/16)*((uint32_t(zbuffer.h())+7)/8)*sizeof(uint32_t));
  auto& probesGBuffDiff    = usesImage2d(gi.probesGBuffDiff,    TextureFormat::RGBA8, gi.atlasDim*16, gi.atlasDim*16); // 16x16 tile
  auto& probesGBuffNorm    = usesImage2d(gi.probesGBuffNorm,    TextureFormat::RGBA8, gi.atlasDim*16, gi.atlasDim*16);
  auto& probesGBuffRayT    = usesImage2d(gi.probesGBuffRayT,    TextureFormat::R16,   gi.atlasDim*16, gi.atlasDim*16);
  auto& probesRadiance     = usesImage2d(gi.probesRadiance,     TextureFormat::R32U,  gi.atlasDim*16, gi.atlasDim*16);
  // RGBA16F: R11G11B10 shifts the colour and has no pure white (step-2.md, «Формат»)
  auto& probesLighting     = usesImage2d(gi.probesLighting,     TextureFormat::RGBA16F, gi.atlasDim*3, gi.atlasDim*2);
  auto& probesLightingPrev = usesImage2d(gi.probesLightingPrev, TextureFormat::RGBA16F, uint32_t(gi.probesLighting.w()), uint32_t(gi.probesLighting.h()));

  const auto&  scene   = wview.sceneGlobals();
  const size_t maxHash = hashTable.byteSize()/sizeof(uint32_t);

  // The game's clock went back (a save from earlier in the game, a new game) or the world changed: the probes' hits
  // and light are of another place or time. With dt 0 their cubes would blend only at a sharp change until the clock
  // came back to the old tick. They start over as on the first frame: all probes new, traced and lit, no history.
  // A pause (the same tick) keeps them.
  if(scene.tickCount<gi.lastTick)
    gi.resetHistory = true;
  if(gi.resetHistory && !fisrtFrame)
    Log::i("render: gi probes start over (", scene.tickCount<gi.lastTick ? "the game's clock went back" : "another world", ")");
  const bool restart = fisrtFrame || gi.resetHistory;
  gi.resetHistory = false;

  cmd.setFramebuffer({});
  if(restart) {
    cmd.setDebugMarker("GI-Init");
    cmd.setBinding(0, voteTable);
    cmd.setBinding(1, hashTable);
    cmd.setBinding(2, probes);
    cmd.setBinding(3, freeList);
    cmd.setBinding(5, newList);
    cmd.setBinding(6, voteTiles);
    cmd.setBinding(7, requests);
    cmd.setPipeline(shaders.probeInit);
    cmd.dispatch(1);
    cmd.setPipeline(shaders.probeClearHash);
    cmd.dispatchThreads(maxHash);
    cmd.setPipeline(shaders.probeClearVotes);
    cmd.dispatchThreads(maxHash);

    cmd.setBinding(0, probesLighting);
    cmd.setBinding(1, Resources::fallbackBlack());
    cmd.setPipeline(shaders.copyImg);
    cmd.dispatchThreads(probesLighting.size());
    gi.lastTick = scene.tickCount;
    }
  // The world time of a frame (smoothed), s: the blend over time, the turns of the relight and the expiry of the probes
  const uint64_t tick = scene.tickCount;
  const float    dt   = tick>gi.lastTick ? std::min(float(tick-gi.lastTick)*0.001f, 0.25f) : 0.f;
  gi.lastTick = tick;
  if(dt>0)
    gi.frameTime = gi.frameTime>0 ? gi.frameTime + (dt-gi.frameTime)*0.1f : dt;
  const float frameTime = gi.frameTime>0 ? gi.frameTime : 1.f/60.f;

  struct PushClear {
    uint32_t frame;
    uint32_t expireAge;
    uint32_t reserve;
    } pushClear = {};
  pushClear.frame     = gi.frame;
  pushClear.expireAge = uint32_t(std::clamp(giExpire/frameTime, 2.f, 65535.f));
  pushClear.reserve   = giReserve;

  static bool alloc = true;
  cmd.setDebugMarker("GI-Alloc");
  cmd.setBinding(0, voteTable);
  cmd.setBinding(1, hashTable);
  cmd.setBinding(2, probes);
  cmd.setBinding(3, freeList);
  cmd.setBinding(5, newList);
  cmd.setBinding(6, voteTiles);
  cmd.setBinding(7, requests);
  cmd.setPushData(pushClear);
  cmd.setPipeline(shaders.probeClear);
  cmd.dispatchThreads(std::max<size_t>(voteTiles.byteSize()/sizeof(uint32_t), 64));

  if(alloc) {
    cmd.setBinding(0, wview.sceneGlobals().uboGlobal[SceneGlobals::V_Main]);
    cmd.setBinding(1, gbufDiffuse, Sampler::nearest());
    cmd.setBinding(2, gbufNormal,  Sampler::nearest());
    cmd.setBinding(3, zbuffer,     Sampler::nearest());
    cmd.setBinding(4, voteTable);
    cmd.setBinding(5, hashTable);
    cmd.setBinding(6, probes);
    cmd.setBinding(7, freeList);
    cmd.setBinding(8, newList);
    cmd.setBinding(9, voteTiles);
    cmd.setPipeline(shaders.probeVote);
    cmd.dispatchThreads(sceneDepth.size());

    cmd.setBinding(0, voteTable);
    cmd.setBinding(1, hashTable);
    cmd.setBinding(2, probes);
    cmd.setBinding(3, freeList);
    cmd.setBinding(5, newList);
    cmd.setBinding(6, voteTiles);
    cmd.setBinding(7, requests);
    cmd.setPipeline(shaders.probePrune);
    cmd.dispatchThreads(gi.maxProbes);

    cmd.setBinding(0, wview.sceneGlobals().uboGlobal[SceneGlobals::V_Main]);
    cmd.setBinding(1, gbufDiffuse, Sampler::nearest());
    cmd.setBinding(2, gbufNormal,  Sampler::nearest());
    cmd.setBinding(3, zbuffer,     Sampler::nearest());
    cmd.setBinding(4, voteTable);
    cmd.setBinding(5, hashTable);
    cmd.setBinding(6, probes);
    cmd.setBinding(7, freeList);
    cmd.setBinding(8, newList);
    cmd.setBinding(9, voteTiles);
    cmd.setPipeline(shaders.probeAlocation);
    cmd.dispatchThreads(sceneDepth.size());

    // the requests of probes at hits of the last frame's lighting, then the vote table is clean for this frame's
    cmd.setBinding(0, voteTable);
    cmd.setBinding(1, hashTable);
    cmd.setBinding(2, probes);
    cmd.setBinding(3, freeList);
    cmd.setBinding(5, newList);
    cmd.setBinding(6, voteTiles);
    cmd.setBinding(7, requests);
    cmd.setPipeline(shaders.probeHitAlloc);
    cmd.dispatchThreads(giMaxRequests);
    cmd.setPipeline(shaders.probeClearVotes);
    cmd.dispatchThreads(maxHash);
    }

  cmd.setDebugMarker("GI-Trace");
  cmd.setBinding(0, wview.sceneGlobals().uboGlobal[SceneGlobals::V_Main]);
  cmd.setBinding(1, probesGBuffDiff);
  cmd.setBinding(2, probesGBuffNorm);
  cmd.setBinding(3, probesGBuffRayT);
  cmd.setBinding(4, hashTable);
  cmd.setBinding(5, probes);
  cmd.setBinding(6, scene.rtScene.tlas);
  cmd.setBinding(7, Sampler::bilinear());
  cmd.setBinding(8, scene.rtScene.tex);
  cmd.setBinding(9, scene.rtScene.vbo);
  cmd.setBinding(10,scene.rtScene.ibo);
  cmd.setBinding(11,scene.rtScene.rtDesc);
  cmd.setBinding(12,newList);
  cmd.setPipeline(shaders.probeTrace);
  cmd.dispatch(1024); // TODO: dispath indirect?

  cmd.setDebugMarker("GI-HashMap");
  cmd.setBinding(0, voteTable);
  cmd.setBinding(1, hashTable);
  cmd.setBinding(2, probes);
  cmd.setBinding(3, freeList);
  cmd.setBinding(5, newList);
  cmd.setBinding(6, voteTiles);
  cmd.setPipeline(shaders.probeClearHash);
  cmd.dispatchThreads(maxHash);
  cmd.setPipeline(shaders.probeMakeHash);
  cmd.dispatchThreads(gi.maxProbes);

  // the blend of the probes' cubes over time: 90 % of a change in 2.3*tau (step-2.md, «2а»), by the world's clock,
  // so a slow frame or a recording at 30 frames per second responds in the same time
  const float    giTau     = 0.1f;
  // an old probe's samples are relit in turns, all of them in about this time: so many per frame, by the frame time
  const float    giRefresh = 0.25f;

  struct Push {
    float    alpha;
    uint32_t frameId;
    uint32_t relitPeriod;
    uint32_t offPeriod;
    float    hitRadius;
    uint32_t maxRequests;
    } push = {};
  push.alpha       = 1.f - std::exp(-dt/giTau);
  push.frameId     = gi.frame++;
  push.relitPeriod = 4;
  while(push.relitPeriod<32 && float(push.relitPeriod*2)*gi.frameTime<=giRefresh*1.5f)
    push.relitPeriod *= 2;  // 30 fps: every 8th sample a frame, 60 fps: 16th, 120 fps: 32nd
  push.offPeriod   = giOffPeriod;
  push.hitRadius   = giHitRadius;
  push.maxRequests = giMaxRequests;

  cmd.setDebugMarker("GI-Lighting");
  cmd.setBinding(0, probesLightingPrev);
  cmd.setBinding(1, probesLighting);
  cmd.setPipeline(shaders.copyImg);
  cmd.dispatchThreads(probesLighting.size());

  cmd.setPushData(push);
  cmd.setBinding(0, wview.sceneGlobals().uboGlobal[SceneGlobals::V_Main]);
  cmd.setBinding(1, probesLighting);
  cmd.setBinding(2, probesGBuffDiff, Sampler::nearest());
  cmd.setBinding(3, probesGBuffNorm, Sampler::nearest());
  cmd.setBinding(4, probesGBuffRayT, Sampler::nearest());
  cmd.setBinding(5, sky.viewCldLut,  sky.sampler);
  cmd.setBinding(6, scene.rtScene.tlas);
  cmd.setBinding(7, Sampler::bilinear());
  cmd.setBinding(8, scene.rtScene.tex);
  cmd.setBinding(9, scene.rtScene.vbo);
  cmd.setBinding(10,scene.rtScene.ibo);
  cmd.setBinding(11,scene.rtScene.rtDesc);
  cmd.setBinding(12,probesLightingPrev, Sampler::nearest());
  cmd.setBinding(13,hashTable);
  cmd.setBinding(14,probes);
  cmd.setBinding(15,lightsTree.bvh);
  cmd.setBinding(16,probesRadiance);
  cmd.setBinding(17,newList);
  cmd.setBinding(18,voteTable);
  cmd.setBinding(19,requests);
  // a thread per relit sample of every slot: the rays keep all lanes busy
  cmd.setPipeline(shaders.probeRelight);
  cmd.dispatchThreads(gi.maxProbes*(256/push.relitPeriod));
  // the new probes: all samples, a thread each
  cmd.setPipeline(shaders.probeRelightNew);
  cmd.dispatch(256);
  // a workgroup per probe: the integral of the samples
  cmd.setPipeline(shaders.probeLighting);
  cmd.dispatch(1024);

  if(FrameProfile::enabled()) {
    auto& stats = gi.stats[fId];
    if(stats.isEmpty())
      stats = Resources::device().ssbo(BufferHeap::Readback, Uninitialized, gi.statsCount*sizeof(uint32_t));
    cmd.setDebugMarker("GI-Stats");
    cmd.setBinding(0, voteTable);
    cmd.setBinding(1, hashTable);
    cmd.setBinding(2, probes);
    cmd.setBinding(3, freeList);
    cmd.setBinding(4, stats);
    cmd.setBinding(5, newList);
    cmd.setBinding(6, voteTiles);
    cmd.setBinding(7, requests);
    cmd.setPipeline(shaders.probeStats);
    cmd.dispatch(1);
    gi.statsWritten[fId] = true;
    }
  }

void Renderer::prepareExposure(Encoder<CommandBuffer>& cmd, WorldView& wview) {
  FrameProfile::Scope timing(FrameProfile::Zone::EncodePost);
  auto& scene = wview.sceneGlobals();

  auto sunDir = wview.sky().sunLight().dir();
  struct Push {
    float baseL        = 0.0;
    float sunOcclusion = 1.0;
    };
  Push push;
  push.sunOcclusion = smoothstep(0.0f, 0.01f, sunDir.y);

  static float override = 0;
  static float add      = 1.5f;
  if(override>0)
    push.baseL = override;
  push.baseL += add;

  cmd.setDebugMarker("Exposure");
  cmd.setFramebuffer({});
  cmd.setBinding(0, scene.uboGlobal[SceneGlobals::V_Main]);
  cmd.setBinding(1, sky.viewCldLut, sky.sampler);
  cmd.setBinding(2, sky.transLut,   Sampler::bilinear(ClampMode::ClampToEdge));
  cmd.setBinding(3, sky.cloudsLut,  Sampler::bilinear(ClampMode::ClampToEdge));
  cmd.setBinding(4, sky.irradianceLut);
  cmd.setPushData(&push, sizeof(push));
  cmd.setPipeline(shaders.skyExposure);
  cmd.dispatch(1);
  }

void Renderer::drawPathtrace(Tempest::Encoder<Tempest::CommandBuffer>& cmd, WorldView& wview, uint8_t fId) {
  if(wview.sceneGlobals().rtScene.tlas.isEmpty())
    return;

  if(!Resources::device().properties().hasAttachFormat(Tempest::RGBA16F))
    return;

  static bool enable = true;
  if(!enable)
    return;

  if(!settings.pathTraceEnabled)
    return;

  if(pt.frame.size()!=sceneLinear.size()) {
    Resources::recycle(std::move(pt.frame));
    pt.frame = Resources::device().attachment(Tempest::RGBA16F, sceneLinear.size());
    }

  wview.visibilityPass(cmd, 0);
  wview.visibilityPass(cmd, 1);

  cmd.setFramebuffer({});
  prepareSky(cmd, wview);
  prepareIrradiance(cmd,wview);
  prepareExposure(cmd,wview);

  drawShadowMap(cmd,fId,wview);

  static float eps = 2.f;
  for(int i=0; i<16; ++i) {
    float a = viewProj.data()[i];
    float b = pt.mvpLast.data()[i];
    if(std::abs(a-b)>eps) {
      pt.numFrames = 0;
      break;
      }
    }

  if(pt.numFrames==0)
    pt.mvpLast = viewProj;

  struct Push {
    uint32_t numFrames;
    };
  Push push;
  push.numFrames = pt.numFrames; ++pt.numFrames;

  if(push.numFrames==0)
    cmd.setFramebuffer({{pt.frame, Vec4(0), Tempest::Preserve}}, {zbuffer, 1, Tempest::Preserve}); else
    cmd.setFramebuffer({{pt.frame, Tempest::Preserve, Tempest::Preserve}}, {zbuffer, 1, Tempest::Preserve});
  auto& scene = wview.sceneGlobals();
  cmd.setDebugMarker("Pathtrace");
  cmd.setPushData(push);
  cmd.setBinding(0, scene.uboGlobal[SceneGlobals::V_Main]);
  //cmd.setBinding(1, zbuffer);
  cmd.setBinding(2, sky.irradianceLut);
  cmd.setBinding(3, sky.viewCldLut, sky.sampler);
  //cmd.setBinding(4, shadowMap[1], Sampler::bilinear());
  cmd.setBinding(4, Resources::fallbackBlack(), Sampler::bilinear());
  cmd.setBinding(5, lightsTree.tree);
  cmd.setBinding(6, scene.rtScene.tlas);
  cmd.setBinding(7, Sampler::trillinear());
  cmd.setBinding(8, scene.rtScene.tex);
  cmd.setBinding(9, scene.rtScene.vbo);
  cmd.setBinding(10,scene.rtScene.ibo);
  cmd.setBinding(11,scene.rtScene.rtDesc);

  cmd.setPipeline(shaders.rtPathtrace);
  cmd.draw(nullptr, 0, 3);

  cmd.setFramebuffer({{sceneLinear, Tempest::Discard, Tempest::Preserve}});
  cmd.setDebugMarker("Blit");
  cmd.setBinding(0, pt.frame, Sampler::nearest());
  cmd.setPipeline(shaders.copy);
  cmd.draw(nullptr, 0, 3);

  prepareFog(cmd,wview);

  cmd.setFramebuffer({{sceneLinear, Tempest::Discard, Tempest::Preserve}}, {zbuffer, Tempest::Readonly});
  cmd.setDebugMarker("Sky");
  drawSky(cmd,wview);
  drawSunMoon(cmd,wview);

  cmd.setDebugMarker("Fog");
  drawFog(cmd, wview);
  }

void Renderer::drawRayQueryDbg(Tempest::Encoder<Tempest::CommandBuffer>& cmd, const WorldView& wview) {
  if(wview.sceneGlobals().rtScene.tlas.isEmpty() || shadowMap[1].isEmpty())
    return;

  static bool enable = false;
  if(!enable)
    return;

  auto& scene = wview.sceneGlobals();
  cmd.setDebugMarker("RQ-dbg");
  cmd.setBinding(0, scene.uboGlobal[SceneGlobals::V_Main]);
  cmd.setBinding(1, zbuffer);
  cmd.setBinding(2, sky.irradianceLut);
  cmd.setBinding(3, shadowMap[1], Sampler::bilinear());
  //
  cmd.setBinding(6, scene.rtScene.tlas);
  cmd.setBinding(7, Sampler::trillinear());
  cmd.setBinding(8, scene.rtScene.tex);
  cmd.setBinding(9, scene.rtScene.vbo);
  cmd.setBinding(10,scene.rtScene.ibo);
  cmd.setBinding(11,scene.rtScene.rtDesc);

  cmd.setFramebuffer({{sceneLinear, Tempest::Preserve, Tempest::Preserve}}, {zbuffer, Tempest::Readonly});
  cmd.setPipeline(shaders.rtDbg);
  cmd.draw(nullptr, 0, 3);
  }

void Renderer::drawSurfelsDbg(Encoder<CommandBuffer>& cmd, const WorldView& wview) {
  if(settings.giMethod!=GiMethod::IrrC)
    return;

  static bool enable = false;
  if(!enable)
    return;

  cmd.setDebugMarker("GI-dbg");
  cmd.setBinding(0, wview.sceneGlobals().uboGlobal[SceneGlobals::V_Main]);
  cmd.setBinding(1, surf.surfels);
  cmd.setBinding(2, gbufNormal,  Sampler::nearest());
  cmd.setBinding(3, sceneDepth,  Sampler::nearest());
  cmd.setPipeline(shaders.surfDbg);
  cmd.draw(nullptr, 0, 36, 0, surf.maxSurfels);
  }

void Renderer::drawProbesDbg(Encoder<CommandBuffer>& cmd, const WorldView& wview) {
  if(settings.giMethod!=GiMethod::Probes)
    return;

  static bool enable = false;
  if(!enable)
    return;

  cmd.setDebugMarker("GI-dbg");
  cmd.setBinding(0, wview.sceneGlobals().uboGlobal[SceneGlobals::V_Main]);
  cmd.setBinding(1, gi.probesLighting);
  cmd.setBinding(2, gi.probes);
  cmd.setBinding(3, gi.hashTable);
  cmd.setPipeline(shaders.probeDbg);
  cmd.draw(nullptr, 0, 36, 0, gi.maxProbes);
  }

void Renderer::drawProbesHitDbg(Encoder<CommandBuffer>& cmd) {
  if(settings.giMethod!=GiMethod::Probes)
    return;

  static bool enable = false;
  if(!enable)
    return;

  cmd.setDebugMarker("GI-dbg");
  cmd.setBinding(1, gi.probesLighting);
  cmd.setBinding(2, gi.probesGBuffDiff);
  cmd.setBinding(3, gi.probesGBuffRayT);
  cmd.setBinding(4, gi.probes);
  cmd.setPipeline(shaders.probeHitDbg);
  cmd.draw(nullptr, 0, 36, 0, gi.maxProbes*256);
  //cmd.draw(nullptr, 0, 36, 0, 1024);
  }

void Renderer::drawAmbient(Encoder<CommandBuffer>& cmd, const WorldView& view) {
  static bool enable = true;
  if(!enable)
    return;

  cmd.setDebugMarker("AmbientLight");
  cmd.setBinding(0, view.sceneGlobals().uboGlobal[SceneGlobals::V_Main]);
  cmd.setBinding(1, gbufDiffuse, Sampler::nearest());
  cmd.setBinding(2, gbufNormal,  Sampler::nearest());
  if(Shaders::options().doMaterialMaps) {
    cmd.setBinding(12, gbufOrm, Sampler::nearest());
    cmd.setBinding(13, sky.viewLut, sky.sampler);
    cmd.setBinding(14, zbuffer, Sampler::nearest());
    }
  if(settings.giMethod==GiMethod::IrrC && settings.zCloudShadowScale) {
    cmd.setBinding(3, surf.irrImage, Sampler::nearest(ClampMode::ClampToEdge));
    cmd.setBinding(4, ssao.ssaoBlur, Sampler::nearest(ClampMode::ClampToEdge)); //TODO: remove once splatting is working
    cmd.setBinding(5, surf.surfels);
    cmd.setPipeline(shaders.ambientLightSurf);
    }
  else if(settings.giMethod==GiMethod::Probes && settings.zCloudShadowScale) {
    cmd.setBinding(4, zbuffer,       Sampler::nearest());
    cmd.setBinding(5, gi.hashTable);
    cmd.setBinding(6, gi.probes);
    cmd.setBinding(7, gi.probesLighting);
    cmd.setPipeline(shaders.probeAmbient);
    }
  else if(settings.zCloudShadowScale) {
    cmd.setBinding(3, sky.irradianceLut);
    cmd.setBinding(4, ssao.ssaoBlur, Sampler::nearest(ClampMode::ClampToEdge));
    cmd.setPipeline(shaders.ambientLightSsao);
    }
  else {
    cmd.setBinding(3, sky.irradianceLut);
    cmd.setPipeline(shaders.ambientLight);
    }
  cmd.draw(nullptr, 0, 3);
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

