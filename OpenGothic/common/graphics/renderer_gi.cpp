#include "renderer.h"
#include "renderer_internal.h"

#include <Tempest/Log>
#include <cmath>

#include "frameprofile.h"
#include "gothic.h"

using namespace Tempest;
using RendererInternal::nextPot;

static Size tileCount(Size sz, int s) {
  sz.w = (sz.w+s-1)/s;
  sz.h = (sz.h+s-1)/s;
  return sz;
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
    cmd.setPipeline(shaders.probeInit);
    cmd.dispatch(1);
    cmd.setPipeline(shaders.probeClearHash);
    cmd.dispatchThreads(maxHash);

    cmd.setBinding(0, probesLighting);
    cmd.setBinding(1, Resources::fallbackBlack());
    cmd.setPipeline(shaders.copyImg);
    cmd.dispatchThreads(probesLighting.size());
    gi.lastTick = scene.tickCount;
    }
  static bool alloc = true;
  cmd.setDebugMarker("GI-Alloc");
  cmd.setBinding(0, voteTable);
  cmd.setBinding(1, hashTable);
  cmd.setBinding(2, probes);
  cmd.setBinding(3, freeList);
  cmd.setBinding(5, newList);
  cmd.setBinding(6, voteTiles);
  cmd.setPipeline(shaders.probeClear);
  cmd.dispatchThreads(maxHash);

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
  const uint64_t tick      = scene.tickCount;
  const float    dt        = tick>gi.lastTick ? std::min(float(tick-gi.lastTick)*0.001f, 0.25f) : 0.f;
  gi.lastTick = tick;
  if(dt>0)
    gi.frameTime = gi.frameTime>0 ? gi.frameTime + (dt-gi.frameTime)*0.1f : dt;

  struct Push {
    float    alpha;
    uint32_t frameId;
    uint32_t relitPeriod;
    } push = {};
  push.alpha       = 1.f - std::exp(-dt/giTau);
  push.frameId     = gi.frame++;
  push.relitPeriod = 4;
  while(push.relitPeriod<32 && float(push.relitPeriod*2)*gi.frameTime<=giRefresh*1.5f)
    push.relitPeriod *= 2;  // 30 fps: every 8th sample a frame, 60 fps: 16th, 120 fps: 32nd

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
      stats = Resources::device().ssbo(BufferHeap::Readback, Uninitialized, 4*sizeof(uint32_t));
    cmd.setDebugMarker("GI-Stats");
    cmd.setBinding(0, voteTable);
    cmd.setBinding(1, hashTable);
    cmd.setBinding(2, probes);
    cmd.setBinding(3, freeList);
    cmd.setBinding(4, stats);
    cmd.setBinding(5, newList);
    cmd.setBinding(6, voteTiles);
    cmd.setPipeline(shaders.probeStats);
    cmd.dispatch(1);
    gi.statsWritten[fId] = true;
    }
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

  // after the translucent pass, without its second attachment (the TAA reactive mask)
  cmd.setFramebuffer({{sceneLinear, Tempest::Preserve, Tempest::Preserve}}, {zbuffer, Tempest::Preserve, Tempest::Preserve});
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

  // after the translucent pass, without its second attachment (the TAA reactive mask)
  cmd.setFramebuffer({{sceneLinear, Tempest::Preserve, Tempest::Preserve}}, {zbuffer, Tempest::Preserve, Tempest::Preserve});
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

  // after the translucent pass, without its second attachment (the TAA reactive mask)
  cmd.setFramebuffer({{sceneLinear, Tempest::Preserve, Tempest::Preserve}}, {zbuffer, Tempest::Preserve, Tempest::Preserve});
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
