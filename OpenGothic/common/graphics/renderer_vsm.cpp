#include "renderer.h"

#include <Tempest/Application>
#include <Tempest/Log>
#include <algorithm>
#include <cstring>

#include "graphics/drawcommands.h"
#include "frameprofile.h"
#include "resources.h"

using namespace Tempest;

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

void Renderer::drawVsm(Tempest::Encoder<Tempest::CommandBuffer>& cmd, WorldView& wview) {
  if(!settings.vsmEnabled)
    return;
  FrameProfile::Scope timing(FrameProfile::Zone::EncodeShadows);

  static bool omniLights = true;
  const bool doVirtualFog = sky.quality!=VolumetricLQ;

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
  // the nearest lamps' shadows keep their state from frame to frame: the buffer grows only, new lamps start unshadowed
  if(vsm.lampShadow.byteSize()<std::max<size_t>(lightsTotal, 1)*sizeof(uint32_t[4])) {
    Resources::recycle(std::move(vsm.lampShadow));
    vsm.lampShadow = Resources::device().ssbo(nullptr, (size_t(lightsTotal) + 256)*sizeof(uint32_t[4]));
    }

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

    if(settings.vsmLampShadows>0) {
      // only the nearest lamps keep their place in the visible list: the others get no pages
      const uint64_t now = Application::tickCount();
      const float    dt  = vsm.selectTime==0 ? 0.f : std::min(float(now-vsm.selectTime)*0.001f, 0.25f);
      vsm.selectTime  = now;
      vsm.selectStamp = vsm.selectStamp+1==0 ? 1 : vsm.selectStamp+1;
      struct Push { uint32_t lightsTotal; uint32_t limit; float dt; uint32_t stamp; } push = {};
      push.lightsTotal = lightsTotal;
      push.limit       = settings.vsmLampShadows;
      push.dt          = dt;
      push.stamp       = vsm.selectStamp;
      cmd.setBinding(0, sceneUbo);
      cmd.setBinding(1, lightsSsbo);
      cmd.setBinding(2, visibleLights);
      cmd.setBinding(3, vsm.lampShadow);
      cmd.setPushData(push);
      cmd.setPipeline(shaders.vsmSelectLamps);
      cmd.dispatch(1);
      }
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
