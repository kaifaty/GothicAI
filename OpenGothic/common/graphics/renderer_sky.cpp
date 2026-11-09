#include "renderer.h"

#include "camera.h"
#include "frameprofile.h"

using namespace Tempest;

static float smoothstep(float edge0, float edge1, float x) {
  float t = std::min(std::max((x - edge0) / (edge1 - edge0), 0.f), 1.f);
  return t * t * (3.f - 2.f * t);
  };

static float linearstep(float edge0, float edge1, float x) {
  float t = std::min(std::max((x - edge0) / (edge1 - edge0), 0.f), 1.f);
  return t;
  };

void Renderer::resetSkyFog() {
  auto& device = Resources::device();

  {
    auto q = Quality::VolumetricLQ;
    if(!settings.zFogRadial) {
      q = Quality::VolumetricLQ;
      } else {
      q = Quality::VolumetricHQ;
      }

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

void Renderer::drawSky(Encoder<CommandBuffer>& cmd, const WorldView& wview) {
  auto& scene = wview.sceneGlobals();

  cmd.setDebugMarker("Sky");
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

void Renderer::prepareFog(Encoder<Tempest::CommandBuffer>& cmd, WorldView& wview) {
  auto& scene  = wview.sceneGlobals();

  cmd.setDebugMarker("Fog-LUTs");
  {
    auto& shader = sky.quality==VolumetricLQ ? shaders.fogViewLut3d : shaders.fogViewLutSep;
    cmd.setFramebuffer({});
    cmd.setBinding(0, scene.uboGlobal[SceneGlobals::V_Main]);
    cmd.setBinding(1, sky.transLut,     sky.sampler);
    cmd.setBinding(2, sky.multiScatLut, sky.sampler);
    cmd.setBinding(3, sky.cloudsLut,    Sampler::bilinear(ClampMode::ClampToEdge));
    cmd.setBinding(4, sky.fogLut3D,     Sampler::bilinear(ClampMode::ClampToEdge));
    if(sky.quality==VolumetricHQ)
      cmd.setBinding(5, sky.fogLut3DMs);
    cmd.setPipeline(shader);
    cmd.dispatchThreads(uint32_t(sky.fogLut3D.w()), uint32_t(sky.fogLut3D.h()));
    }

  if(settings.vsmEnabled && !settings.pathTraceEnabled && sky.quality==VolumetricHQ) {
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
    }
  }

void Renderer::prepareEpipolar(Tempest::Encoder<Tempest::CommandBuffer>& cmd, WorldView& wview) {
  const bool doVolumetricFog = sky.quality!=VolumetricLQ;
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
