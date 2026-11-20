#include "renderer.h"

#include <Tempest/Application>
#include <Tempest/Log>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

#include "graphics/drawcommands.h"
#include "frameprofile.h"

using namespace Tempest;

namespace {
// VsmHeader of vsm_common.glsl: the bins of pairs are the lamps by distance, then the sun's mips by ring
constexpr uint32_t VsmMips = 16, VsmLampBins = 16, VsmRingBins = 16, VsmPairBins = VsmLampBins+VsmMips*VsmRingBins;
constexpr uint32_t VsmBinWords = (VsmPairBins+31)/32;
struct VsmHeaderCpu {
  uint32_t pageCount, meshletCount, counterM, counterV;
  uint32_t pagePerMip[VsmMips];
  int32_t  pageBbox[VsmMips][4];
  uint32_t pageOmniCount, pageListCount, pagePlaced, omniTileOverflow, omniTileLightsMax;
  uint32_t pairSpill, candidateCount, clusterPairs;
  uint32_t admitBits[VsmBinWords], admitPrevBits[VsmBinWords];
  uint32_t pairDemand[VsmPairBins];
  };

// vsm_pack_draws.comp: each row counts the geometry of one selected class on one light source/mip.
struct VsmGeometryRow {
  uint32_t pairs, triangles, affectedPairs, affectedTriangles;
  };
using VsmGeometryCpu = VsmGeometryRow[VsmMips+1][3];

void logVsmGeometry(uint64_t frame, uint32_t drawn, const VsmGeometryCpu& geometry) {
  uint32_t pairs = 0, triangles = 0, affectedPairs = 0, affectedTriangles = 0;
  for(uint32_t i=0; i<=VsmMips; ++i) {
    const auto& [other, visual, coarse] = geometry[i];
    const uint32_t current = other.pairs + visual.pairs + coarse.pairs;
    if(current==0)
      continue;
    const uint32_t changedPairs = other.affectedPairs + visual.affectedPairs + coarse.affectedPairs;
    const uint32_t changedTriangles = other.affectedTriangles + visual.affectedTriangles + coarse.affectedTriangles;
    pairs += current;
    triangles += other.triangles + visual.triangles + coarse.triangles;
    affectedPairs += changedPairs;
    affectedTriangles += changedTriangles;
    Log::i("vsm geometry ", frame, " source=", (i==VsmMips ? "lamp" : "m" + std::to_string(i)),
           " other=", other.pairs, "/", other.triangles, " visual=", visual.pairs, "/", visual.triangles,
           " coarse=", coarse.pairs, "/", coarse.triangles, " affected=", changedPairs, "/", changedTriangles);
    }
  Log::i("vsm geometry ", frame, " source=total pairs=", pairs, " triangles=", triangles,
         " affected=", affectedPairs, "/", affectedTriangles, " drawn=", drawn);
  }

bool vsmBinSet(const uint32_t* bits, uint32_t bin) {
  return (bits[bin/32] & (1u << (bin%32)))!=0;
  }

std::string vsmBinName(uint32_t bin) {
  if(bin<VsmLampBins)
    return "L" + std::to_string(bin);
  if(bin>=VsmPairBins)
    return "none";
  return "m" + std::to_string((bin-VsmLampBins)/VsmRingBins) + "r" + std::to_string((bin-VsmLampBins)%VsmRingBins);
  }

std::string vsmBitsHex(const uint32_t* bits) {
  std::string ret;
  char buf[16] = {};
  for(uint32_t i=0; i<VsmBinWords; ++i) {
    std::snprintf(buf, sizeof(buf), i==0 ? "%08x" : ".%08x", bits[i]);
    ret += buf;
    }
  return ret;
  }
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

void Renderer::drawVsm(Tempest::Encoder<Tempest::CommandBuffer>& cmd, uint8_t fId, WorldView& wview) {
  if(!settings.vsmEnabled)
    return;
  FrameProfile::Scope timing(FrameProfile::Zone::EncodeShadows);
  logVsmFrame(fId);
  ++vsm.frameNo;

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
  // zeroed when made: vsm_clear.comp takes the bins drawn the frame before from it (admitBits), none on the first
  auto& pageList    = usesSsboInit(vsm.pageList, shaders.vsmClear.sizeofBuffer(0, pageCount));
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
  auto* geometryStats = settings.vsmStatsFrames ? &usesSsbo(vsm.geometryStats, sizeof(VsmGeometryCpu)) : nullptr;
  wview.visibilityVsm(cmd, geometryStats);
  copyVsmFrame(cmd, fId);
  if(settings.vsmStatsFrames)
    wview.logVsmShadowProbe(vsm.frameNo);

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

  VsmHeaderCpu h = {};
  uint32_t visibleLights = 0;
  auto& device = Resources::device();
  device.waitIdle();
  device.readBytes(vsm.pageList,      &h,             sizeof(h));
  device.readBytes(vsm.visibleLights, &visibleLights, sizeof(visibleLights));

  const uint32_t poolPages = uint32_t(vsm.pageData.w()/VSM_PAGE_SIZE) * uint32_t(vsm.pageData.h()/VSM_PAGE_SIZE);
  const uint32_t dropped   = h.pageListCount>h.pagePlaced ? h.pageListCount-h.pagePlaced : 0;
  const uint32_t cap       = DrawCommands::vsmPairCap();
  uint64_t demand = 0;
  for(auto d:h.pairDemand)
    demand += d;
  // meshletCount: the pairs drawn; demand: the pairs the meshlets' test keeps, whole clusters: before it (a cluster's
  // all meshlets in every entry its sphere touches); OVERFLOW: the demand is past the cap and classes are dropped
  Log::i("vsm stats: pageCount = ", h.pageCount, "/", poolPages,
         ", entries = ", h.pageListCount, " (sun ", h.pageListCount-h.pageOmniCount, ", pageOmniCount ", h.pageOmniCount, ")",
         ", placed = ", h.pagePlaced, ", dropped = ", dropped,
         ", meshletCount = ", h.meshletCount, "/", cap,
         ", demand = ", demand, (demand>cap ? " OVERFLOW" : ""), " (whole clusters ", h.clusterPairs, ")",
         ", candidates = ", h.candidateCount, (h.candidateCount>cap ? " LOST" : ""),
         ", spilled = ", h.pairSpill,
         ", lights = ", visibleLights, "/", uint32_t(wview.lights().size()),
         ", tile lights max = ", h.omniTileLightsMax, ", tiles over limit = ", h.omniTileOverflow);
  // the pairs of the lamps and of each sun's mip with any, and how many of them the bins left out this frame
  // (vsm_cull_meshlets.comp) did not draw
  const auto drawn = [&](uint32_t bin) { return vsmBinSet(h.admitBits, bin); };
  std::string classes;
  for(uint32_t c=0; c<17; ++c) {
    const uint32_t first = c==0 ? 0 : 16+(c-1)*16;
    uint32_t sum = 0, left = 0;
    for(uint32_t bin=first; bin<first+16; ++bin) {
      sum += h.pairDemand[bin];
      if(!drawn(bin))
        left += h.pairDemand[bin];
      }
    if(sum==0)
      continue;
    classes += (c==0 ? std::string(" lamps ") : " mip" + std::to_string(c-1) + " ") + std::to_string(sum);
    if(left>0)
      classes += " (left out " + std::to_string(left) + ")";
    }
  Log::i("vsm pairs by class:", classes);

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

void Renderer::copyVsmFrame(Tempest::Encoder<Tempest::CommandBuffer>& cmd, uint8_t fId) {
  // OPENGOTHIC_VSM_STATS=frames: VsmHeader after the meshlets' pass, into this frame slot's buffer the CPU can read
  if(!settings.vsmStatsFrames)
    return;
  auto& dst = vsm.frameStats[fId];
  if(dst.isEmpty())
    dst = Resources::device().ssbo(BufferHeap::Readback, Uninitialized, sizeof(VsmHeaderCpu));
  cmd.setFramebuffer({});
  cmd.setDebugMarker("VSM-frame-stats");
  cmd.setBinding(0, dst);
  cmd.setBinding(1, vsm.pageList);
  cmd.setPipeline(shaders.copyBuf);
  cmd.dispatchThreads(sizeof(VsmHeaderCpu)/sizeof(uint32_t));
  auto& geometry = vsm.geometryRead[fId];
  if(geometry.isEmpty())
    geometry = Resources::device().ssbo(BufferHeap::Readback, Uninitialized, sizeof(VsmGeometryCpu));
  cmd.setBinding(0, geometry);
  cmd.setBinding(1, vsm.geometryStats);
  cmd.dispatchThreads(sizeof(VsmGeometryCpu)/sizeof(uint32_t));
  vsm.frameStatsNo     [fId] = vsm.frameNo;
  vsm.frameStatsWritten[fId] = true;
  }

void Renderer::logVsmFrame(uint8_t fId) {
  // OPENGOTHIC_VSM_STATS=frames: a line for the frame this slot drew last time - its fence has been waited for, so
  // nothing waits for the GPU. The bins of pairs (vsm_cull_meshlets.comp, admit()): the demand of each, drawn (+) or
  // left out (-); the selection admit() makes of this demand and of the bins of the frame before, made again here
  // (gpu==cpu), and whether the bins it took for the frame before's are the ones that frame drew (prev==last)
  if(!settings.vsmStatsFrames || !vsm.frameStatsWritten[fId] || vsm.frameStats[fId].isEmpty())
    return;
  vsm.frameStatsWritten[fId] = false;
  VsmHeaderCpu h = {};
  Resources::device().readBytes(vsm.frameStats[fId], &h, sizeof(h));
  const uint64_t no  = vsm.frameStatsNo[fId];
  const uint32_t cap = DrawCommands::vsmPairCap();

  uint32_t cpu[VsmBinWords] = {};
  uint32_t sum = 0;
  for(uint32_t bin=0; bin<VsmPairBins; ++bin) {
    const uint32_t d     = h.pairDemand[bin];
    const uint32_t limit = vsmBinSet(h.admitPrevBits, bin) ? cap : cap - cap/8u;
    if(d>limit || sum>limit-d)
      break;
    sum += d;
    cpu[bin/32] |= 1u << (bin%32);
    }

  uint64_t demand = 0, admitted = 0;
  uint32_t cut    = VsmPairBins;
  std::string bins;
  for(uint32_t bin=0; bin<VsmPairBins; ++bin) {
    const uint32_t d  = h.pairDemand[bin];
    const bool     in = vsmBinSet(h.admitBits, bin);
    demand += d;
    if(in)
      admitted += d;
    if(!in && cut==VsmPairBins)
      cut = bin;
    if(d>0)
      bins += " " + vsmBinName(bin) + (in ? "+" : "-") + std::to_string(d);
    }
  // a bench's shot draws a second frame through the same slot, so a frame may come before the one ahead of it: the
  // pair is checked by whichever of the two comes second
  const size_t   last     = size_t(no-1)%4;
  const size_t   next     = size_t(no+1)%4;
  const bool     sameCpu  = std::equal(std::begin(cpu), std::end(cpu), std::begin(h.admitBits));
  const bool     hasLast  = no>1 && vsm.frameLoggedNo[last]==no-1;
  const bool     sameLast = std::equal(std::begin(vsm.frameLoggedAdmit[last]), std::end(vsm.frameLoggedAdmit[last]),
                                       std::begin(h.admitPrevBits));
  const bool     hasNext  = vsm.frameLoggedNo[next]==no+1;
  const bool     sameNext = std::equal(std::begin(vsm.frameLoggedPrev[next]), std::end(vsm.frameLoggedPrev[next]),
                                       std::begin(h.admitBits));
  std::string cutText = vsmBinName(cut);
  if(cut<VsmPairBins)
    cutText += " " + std::to_string(h.pairDemand[cut]) + (vsmBinSet(h.admitPrevBits, cut) ? " drawn before" : " left out before");
  Log::i("vsm frame ", no, ": candidates ", h.candidateCount, (h.candidateCount>cap ? " LOST" : ""),
         ", entries ", h.pageListCount, " (omni ", h.pageOmniCount, "), drawn ", h.meshletCount, ", demand ", demand,
         ", admitted ", admitted, ", cut ", cutText, ", ", (sameCpu ? "gpu==cpu" : "GPU!=CPU"),
         ", ", (!hasLast ? "prev ?" : (sameLast ? "prev==last" : "PREV!=LAST")),
         (!hasNext ? "" : (sameNext ? ", next's prev==this" : ", NEXT'S PREV!=THIS")), ", spilled ", h.pairSpill,
         ", admit ", vsmBitsHex(h.admitBits), ", prev ", vsmBitsHex(h.admitPrevBits), ", bins", bins);
  VsmGeometryCpu geometry = {};
  Resources::device().readBytes(vsm.geometryRead[fId], &geometry, sizeof(geometry));
  logVsmGeometry(no, h.meshletCount, geometry);
  vsm.frameLoggedNo[no%4] = no;
  std::copy(std::begin(h.admitBits),     std::end(h.admitBits),     std::begin(vsm.frameLoggedAdmit[no%4]));
  std::copy(std::begin(h.admitPrevBits), std::end(h.admitPrevBits), std::begin(vsm.frameLoggedPrev[no%4]));
  }
