#include "renderer.h"

#include <Tempest/Log>
#include <Tempest/RenderState>

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "camera.h"
#include "frameprofile.h"
#include "gameservices.h"
#include "graphics/mesh/submesh/staticmesh.h"
#include "graphics/treenanitegpu.h"
#include "graphics/renderer_internal.h"
#include "shader.h"

using namespace Tempest;

namespace {

constexpr uint32_t SelectWidth = 128;
constexpr uint32_t MaxMeshGroups = 32768;
constexpr uint32_t StatsCount = 7;
constexpr uint32_t VerifyCount = 16;
constexpr uint32_t ShadowChunk = 65536;
constexpr uint32_t ShadowCapacity = 64*ShadowChunk;

uint32_t divUp(uint32_t n, uint32_t d) {
  return (n+d-1)/d;
  }

void dispatchGroups(Encoder<CommandBuffer>& cmd, uint32_t groups) {
  const uint32_t width = std::min(groups,uint32_t(Resources::device().properties().compute.maxGroups.x));
  cmd.dispatch(width,divUp(groups,width));
  }

Shader shader(std::string_view name) {
  const auto src = GothicShader::get(name);
  return Resources::device().shader(src.data,src.len);
  }

ComputePipeline compute(std::string_view name) {
  return Resources::device().pipeline(shader(name));
  }

}

int Renderer::treeNaniteVerifyMode() {
  static const int mode = [] {
    const auto value = std::getenv("OPENGOTHIC_TREE_NANITE_VERIFY");
    return value==nullptr ? 0 : std::clamp(std::atoi(value),0,2);
    }();
  return mode;
  }

namespace {

void bindGeometry(Encoder<CommandBuffer>& cmd, const WorldView& view, const TreeNanite::GpuAsset& asset,
                  const StorageBuffer& poses, const VisualObjects& visuals) {
  cmd.setBinding(0,asset.vertices);
  cmd.setBinding(1,asset.meshlets);
  cmd.setBinding(2,asset.words);
  cmd.setBinding(3,asset.source);
  cmd.setBinding(4,visuals.treeNaniteInstances());
  cmd.setBinding(10,view.drawBuckets().ssbo());
  cmd.setBinding(11,asset.skin);
  cmd.setBinding(12,poses);
  cmd.setBinding(18,visuals.instanceSsbo());
  cmd.setBinding(19,visuals.instanceSsboPrev());
  }

}

void Renderer::readTreeNaniteVerification(uint8_t fId) {
  auto& ns = treeNanite;
  if(ns.verifyTick[fId]==0)
    return;
  uint32_t values[VerifyCount] = {};
  Resources::device().readBytes(ns.verifyRead[fId],values,sizeof(values));
  const bool passed = values[7]==0 && values[0]==uint32_t(ns.size.w)*uint32_t(ns.size.h);
  Log::i("{\"event\":\"tree_nanite_verify\",\"pairs\":1,\"tick\":",ns.verifyTick[fId]-1,
         ",\"segment\":\"",ns.verifySegment[fId],"\",\"tested_pixels\":",values[0],",\"lost_pixels\":",values[1],
         ",\"depth_mismatches\":",values[3],",\"id_mismatches\":",values[2],",\"flags_mismatches\":",values[4],
         ",\"tie_mismatches\":",values[5],",\"gained_pixels\":",values[6],",\"first_bad_pixel\":",values[7],
         ",\"reference_id\":[",values[8],",",values[9],"],\"actual_id\":[",values[10],",",values[11],"]",
         ",\"depth_bits\":[",values[12],",",values[13],"],\"material_flags\":[",values[14],",",values[15],"]",
         ",\"passed\":",passed ? "true}" : "false}");
  ns.verifyTick[fId] = 0;
  }

void Renderer::drawTreeNanite(Encoder<CommandBuffer>& cmd, uint8_t fId, WorldView& view, const Camera& camera,
                              const BenchProbe::HistoryVerifyRequest& request) {
  auto& ns = treeNanite;
  auto& device = Resources::device();
  auto& visuals = view.treeVisuals();
  const auto& asset = *visuals.treeNaniteMesh()->treeNanite;
  const auto& scene = view.sceneGlobals();
  const bool maps = Shaders::options().doMaterialMaps;
  const bool profile = FrameProfile::enabled();
  const bool verify = bool(request);
  const uint32_t instanceCount = visuals.treeNaniteCount();
  const uint32_t recordCount = instanceCount*asset.sourceCount;
  const uint32_t blockCount = divUp(recordCount,SelectWidth);
  const uint32_t blocksPerChunk = std::min(MaxMeshGroups,uint32_t(device.properties().meshlets.maxGroups.x))/SelectWidth;
  const uint32_t chunkCount = divUp(blockCount,blocksPerChunk);
  const uint32_t groupCount = uint32_t(asset.groups.byteSize()/32);
  const uint32_t maxFaces = asset.contourOffsets[1];
  const uint32_t contourGroups = divUp(maxFaces,32)*instanceCount;
  const uint32_t taskBatch = std::min(MaxMeshGroups,uint32_t(device.properties().meshlets.maxGroups.x));
  const Size size = zbuffer.size();
  const bool reset = !ns.valid || !camHistory.continuous || ns.revision!=visuals.treeNaniteRevision() ||
                     ns.size!=size || ns.serial==0x3fffffffu;
  if(reset)
    ns.serial = 0;
  ++ns.serial;
  ns.revision = visuals.treeNaniteRevision();
  ns.size = size;
  ns.active = true;

  FrameProfile::Scope encode(FrameProfile::Zone::TreeNaniteEncode);
  if(ns.raster.isEmpty()) {
    RenderState state;
    state.setCullFaceMode(RenderState::CullMode::NoCull);
    state.setZWriteEnabled(true);
    state.setZTestMode(RenderState::ZTestMode::Less);
    ns.raster = device.pipeline(state,shader("tree_nanite_contour.task.sprv"),shader("tree_nanite_adaptive.mesh.sprv"),
                                shader("tree_nanite_game_visibility.frag.sprv"));
    state.setZWriteEnabled(false);
    state.setZTestMode(RenderState::ZTestMode::Always);
    const auto fullscreen = shader("tree_nanite_game_resolve.vert.sprv");
    ns.resolve = device.pipeline(Topology::Triangles,state,fullscreen,
                                 shader(maps ? "tree_nanite_game_resolve.frag.sprv" : "tree_nanite_game_resolve_nomaps.frag.sprv"));
    ns.groups = compute("tree_nanite_groups.comp.sprv");
    ns.select = compute("tree_nanite_select.comp.sprv");
    ns.compact = compute("tree_nanite_compact.comp.sprv");
    ns.history = compute("tree_nanite_history.comp.sprv");
    ns.wind = compute("tree_nanite_game_pose.comp.sprv");
    ns.contourPrepare = compute("tree_nanite_contour_prepare.comp.sprv");
    if(treeNaniteVerifyMode()!=0) {
      state.setZWriteEnabled(true);
      ns.copyDepth = device.pipeline(Topology::Triangles,state,fullscreen,shader("tree_nanite_depth_copy.frag.sprv"));
      ns.verify = compute("tree_nanite_verify.comp.sprv");
      }
    Log::i("Tree Nanite Vulkan path active");
    }

  if(ns.statsPending[fId]) {
    uint32_t values[StatsCount] = {};
    device.readBytes(ns.statsRead[fId],values,sizeof(values));
    ns.statsPending[fId] = false;
    FrameProfile::add(FrameProfile::Amount::TreeNaniteTrianglesCandidate,values[1]);
    FrameProfile::add(FrameProfile::Amount::TreeNaniteTrianglesEarly,values[3]);
    FrameProfile::add(FrameProfile::Amount::TreeNaniteTrianglesLate,values[1]-values[3]-values[6]);
    FrameProfile::add(FrameProfile::Amount::TreeNaniteMeshletsTested,values[4]);
    FrameProfile::add(FrameProfile::Amount::TreeNaniteMeshletsRejected,values[5]);
    }
  readTreeNaniteVerification(fId);

  usesAttachment(ns.winners,TextureFormat::RG32U,size);
  usesSsbo(ns.state,size_t(recordCount)*2*sizeof(uint32_t));
  usesSsbo(ns.cut,size_t(instanceCount)*(groupCount+1)*sizeof(uint32_t));
  usesSsbo(ns.scratch,size_t(blockCount)*5*sizeof(uint32_t));
  usesSsbo(ns.records,size_t(recordCount)*sizeof(uint32_t));
  usesSsbo(ns.indirect,size_t(chunkCount)*3*sizeof(uint32_t));
  usesSsbo(ns.poses,size_t(2)*instanceCount*asset.boneCount*48);
  usesSsbo(ns.stats,StatsCount*sizeof(uint32_t));
  const auto contourBytes = size_t(instanceCount)*maxFaces*2*sizeof(uint32_t);
  usesSsbo(ns.contourCuts,contourBytes);
  usesSsbo(ns.contourShapes,contourBytes);
  usesSsbo(ns.contourWins,contourBytes);

  const auto cameraPosition = scene.camPos();
  const auto previousEye = reset ? Vec4() : ns.eye;
  ns.eye = Vec4(cameraPosition.x,cameraPosition.y,cameraPosition.z,0.5f*float(size.h)*std::abs(scene.verticalProjection()));
  const TreeNaniteFrame frame = {{8,asset.sourceCount,asset.boneCount,instanceCount},
    Vec4(camera.zNear(),camera.zFar(),11.f,asset.windHeight),previousEye,asset.boundsMin,asset.boundsMax,
    {visuals.treeNaniteBucket(0),visuals.treeNaniteBucket(1),uint32_t(settings.taaReactive),0u},
    Vec4(asset.levelPx[0],asset.levelPx[1],asset.levelPx[2],asset.levelPx[3]),asset.boundsCenterHeight,
    {asset.contourOffsets[0],asset.contourOffsets[1],asset.contourOffsets[2],asset.contourOffsets[3]},
    {asset.contourOffsets.back(),maxFaces,ns.serial,uint32_t(!reset)}};
  auto& frameUbo = ns.frame[fId];
  if(frameUbo.isEmpty())
    frameUbo = device.ubo(frame);
  else
    frameUbo.update(&frame);

  auto pyramidW = RendererInternal::nextPot(uint32_t(size.w));
  auto pyramidH = RendererInternal::nextPot(uint32_t(size.h));
  while(pyramidW>128 || pyramidH>128) {
    pyramidW = std::max(pyramidW/2,1u);
    pyramidH = std::max(pyramidH/2,1u);
    }
  auto& pyramid = usesImage2d(ns.pyramid,TextureFormat::R32F,pyramidW,pyramidH,true);
  const float roundoff = zBufferFormat==TextureFormat::Depth16 ? 1.f/65535.f : 2.f/16777215.f;

  cmd.setFramebuffer({});
  cmd.setDebugMarker("TreeNanite-wind");
  cmd.setBinding(4,visuals.treeNaniteInstances());
  cmd.setBinding(7,frameUbo);
  cmd.setBinding(12,ns.poses);
  cmd.setBinding(13,asset.bones);
  cmd.setBinding(16,scene.uboGlobal[SceneGlobals::V_Main]);
  cmd.setBinding(18,visuals.instanceSsbo());
  cmd.setBinding(19,visuals.instanceSsboPrev());
  cmd.setPipeline(ns.wind);
  cmd.dispatchThreads(asset.boneCount,instanceCount,2);

  const auto history = [&](uint32_t phase) {
    cmd.setFramebuffer({});
    cmd.setDebugMarker("TreeNanite-history");
    cmd.setBinding(0,ns.winners,Sampler::nearest());
    cmd.setBinding(1,ns.state);
    cmd.setBinding(2,ns.stats);
    cmd.setBinding(3,ns.contourWins);
    const uint32_t push[] = {recordCount,ns.serial,phase,0,asset.contourOffsets[0],asset.contourOffsets[1],
                             asset.contourOffsets[2],asset.contourOffsets[3],asset.contourOffsets.back(),maxFaces,instanceCount,0};
    cmd.setPushData(push);
    cmd.setPipeline(ns.history);
    if(phase==2)
      cmd.dispatchThreads(divUp(uint32_t(size.w),4),divUp(uint32_t(size.h),4));
    else
      dispatchGroups(cmd,phase==0 ? divUp(std::max(recordCount,StatsCount),64) : 1u);
    };
  if(reset || profile)
    history(reset ? 0u : 1u);

  cmd.setDebugMarker("TreeNanite-publish");
  cmd.setBinding(4,visuals.treeNaniteInstances());
  cmd.setBinding(7,frameUbo);
  cmd.setBinding(8,asset.groups);
  cmd.setBinding(16,scene.uboGlobal[SceneGlobals::V_Main]);
  cmd.setBinding(18,visuals.instanceSsbo());
  cmd.setBinding(21,ns.cut);
  cmd.setPipeline(ns.groups);
  cmd.dispatch(instanceCount);

  const auto contourPrepare = [&](uint32_t phase) {
    cmd.setFramebuffer({});
    cmd.setDebugMarker(phase==0 ? "TreeNanite-publish" : "TreeNanite-history");
    bindGeometry(cmd,view,asset,ns.poses,visuals);
    cmd.setBinding(7,frameUbo);
    cmd.setBinding(13,ns.stats);
    cmd.setBinding(16,scene.uboGlobal[SceneGlobals::V_Main]);
    cmd.setBinding(20,ns.contourCuts);
    cmd.setBinding(21,ns.cut);
    cmd.setBinding(22,asset.contourFaces);
    cmd.setBinding(23,asset.contourShapes);
    cmd.setBinding(24,ns.contourShapes);
    cmd.setBinding(25,ns.contourWins);
    const uint32_t push[] = {groupCount,phase,uint32_t(profile),0};
    cmd.setPushData(push);
    cmd.setPipeline(ns.contourPrepare);
    dispatchGroups(cmd,contourGroups);
    };
  if(reset)
    contourPrepare(2);
  contourPrepare(0);

  const auto select = [&](uint32_t phase, bool occlusion, bool reference) {
    cmd.setFramebuffer({});
    cmd.setDebugMarker(phase==0 ? "TreeNanite-publish" : "TreeNanite-cull");
    cmd.setBinding(1,asset.meshlets);
    cmd.setBinding(3,asset.source);
    cmd.setBinding(4,visuals.treeNaniteInstances());
    cmd.setBinding(7,frameUbo);
    cmd.setBinding(9,textureCast<const Texture2d&>(pyramid),Sampler::nearest(ClampMode::ClampToEdge));
    cmd.setBinding(10,ns.scratch);
    cmd.setBinding(13,ns.stats);
    cmd.setBinding(15,ns.state);
    cmd.setBinding(16,scene.uboGlobal[SceneGlobals::V_Main]);
    cmd.setBinding(18,visuals.instanceSsbo());
    cmd.setBinding(19,visuals.instanceSsboPrev());
    cmd.setBinding(21,ns.cut);
    struct Push {
      uint32_t serial, phase, historyValid, allowOcclusion;
      float roundoff;
      uint32_t profile, groupCount, reference;
      } push = {ns.serial,phase,uint32_t(!reset),uint32_t(occlusion),roundoff,uint32_t(profile && !reference),groupCount,uint32_t(reference)};
    cmd.setPushData(push);
    cmd.setPipeline(ns.select);
    dispatchGroups(cmd,blockCount);

    cmd.setBinding(0,ns.scratch);
    cmd.setBinding(1,ns.records);
    cmd.setBinding(2,ns.indirect);
    uint32_t compactPush[] = {0,blockCount,blocksPerChunk,0};
    cmd.setPushData(compactPush);
    cmd.setPipeline(ns.compact);
    cmd.dispatch(chunkCount);
    compactPush[0] = 1;
    cmd.setPushData(compactPush);
    dispatchGroups(cmd,blockCount);
    };
  const auto raster = [&](Attachment& winners, ZBuffer& depth, bool clear,
                          uint32_t phase, bool occlusion, bool reference) {
    const AttachmentDesc color = clear ? AttachmentDesc(winners,Vec4(),Preserve) : AttachmentDesc(winners,Preserve,Preserve);
    cmd.setFramebuffer({color},{depth,Preserve,Preserve});
    cmd.setDebugMarker(clear ? "TreeNanite-VB-early" : "TreeNanite-VB-late");
    bindGeometry(cmd,view,asset,ns.poses,visuals);
    cmd.setBinding(7,frameUbo);
    cmd.setBinding(14,ns.records);
    cmd.setBinding(16,scene.uboGlobal[SceneGlobals::V_Main]);
    cmd.setBinding(9,textureCast<const Texture2d&>(pyramid),Sampler::nearest(ClampMode::ClampToEdge));
    cmd.setBinding(13,ns.stats);
    cmd.setBinding(20,ns.contourCuts);
    cmd.setBinding(21,ns.cut);
    cmd.setBinding(22,asset.contourFaces);
    cmd.setBinding(24,ns.contourShapes);
    cmd.setBinding(25,ns.contourWins);
    cmd.setBinding(26,ns.indirect);
    cmd.setPipeline(ns.raster);
    struct Push {
      uint32_t first, phase, allowOcclusion, groupCount;
      float roundoff;
      uint32_t profile;
      };
    const auto draw = [&](uint32_t groups, Push push) {
      for(uint32_t first=0; first<groups; first+=taskBatch) {
        push.first = first;
        cmd.setPushData(push);
        cmd.dispatchMesh(std::min(taskBatch,groups-first));
        }
      };
    draw(divUp(recordCount,512),{0,3,0,blocksPerChunk*SelectWidth,0.f,0});
    draw(contourGroups,{0,phase,uint32_t(occlusion),groupCount,roundoff,uint32_t(profile && !reference)});
    };
  const auto resolve = [&](Attachment& winners, Attachment& diffuse, Attachment& normal, Attachment& orm,
                           Attachment& motion, Attachment& reactive) {
    if(maps)
      cmd.setFramebuffer({{diffuse,Preserve,Preserve},{normal,Preserve,Preserve},{orm,Preserve,Preserve},
                          {motion,Preserve,Preserve},{reactive,Vec4(),Preserve}});
    else
      cmd.setFramebuffer({{diffuse,Preserve,Preserve},{normal,Preserve,Preserve},
                          {motion,Preserve,Preserve},{reactive,Vec4(),Preserve}});
    cmd.setDebugMarker("TreeNanite-resolve");
    bindGeometry(cmd,view,asset,ns.poses,visuals);
    cmd.setBinding(5,view.drawBuckets().textures());
    cmd.setBinding(6,winners,Sampler::nearest());
    cmd.setBinding(7,frameUbo);
    if(maps) {
      cmd.setBinding(8,view.drawBuckets().normals());
      cmd.setBinding(9,view.drawBuckets().orm());
      }
    cmd.setBinding(15,ns.state);
    cmd.setBinding(16,scene.uboGlobal[SceneGlobals::V_Main]);
    cmd.setBinding(17,Sampler::anisotrophy());
    cmd.setBinding(20,ns.contourCuts);
    cmd.setBinding(22,asset.contourFaces);
    cmd.setBinding(24,ns.contourShapes);
    cmd.setPipeline(ns.resolve);
    cmd.draw(nullptr,0,3);
    };

  if(verify) {
    usesAttachment(ns.refWinners,TextureFormat::RG32U,size);
    usesZBuffer(ns.refDepth,zBufferFormat,uint32_t(size.w),uint32_t(size.h));
    cmd.setFramebuffer({}, {ns.refDepth,Discard,Preserve});
    cmd.setDebugMarker("TreeNanite-verify-depth");
    cmd.setBinding(0,zbuffer,Sampler::nearest());
    cmd.setPipeline(ns.copyDepth);
    cmd.draw(nullptr,0,3);
    }
  select(0,false,false);
  raster(ns.winners,zbuffer,true,0,false,false);
  if(verify && treeNaniteVerifyMode()==1)
    raster(ns.refWinners,ns.refDepth,true,0,false,true);
  if(!reset) {
    buildHiZ(cmd,&pyramid,"TreeNanite-HZB");
    select(1,true,false);
    raster(ns.winners,zbuffer,false,1,true,false);
    }
  resolve(ns.winners,gbufDiffuse,gbufNormal,gbufOrm,gbufMotion,taa.reactive);

  if(verify) {
    if(treeNaniteVerifyMode()==2) {
      select(2,false,true);
      raster(ns.refWinners,ns.refDepth,true,2,false,true);
      } else if(!reset) {
      select(1,false,true);
      raster(ns.refWinners,ns.refDepth,false,1,false,true);
      }
    usesAttachment(ns.refDiffuse,TextureFormat::RGBA8,size);
    usesAttachment(ns.refNormal,TextureFormat::R32U,size);
    if(maps)
      usesAttachment(ns.refOrm,TextureFormat::RGBA8,size);
    usesAttachment(ns.refMotion,textureCast<const Texture2d&>(gbufMotion).format(),size);
    usesAttachment(ns.refReactive,TextureFormat::R8,size);
    resolve(ns.refWinners,ns.refDiffuse,ns.refNormal,ns.refOrm,ns.refMotion,ns.refReactive);
    cmd.setFramebuffer({});
    cmd.setDebugMarker("TreeNanite-verify");
    auto& read = ns.verifyRead[fId];
    if(read.isEmpty())
      read = device.ssbo(BufferHeap::Readback,nullptr,VerifyCount*sizeof(uint32_t));
    else {
      const uint32_t zero[VerifyCount] = {};
      read.update(zero,0,sizeof(zero));
      }
    cmd.setBinding(0,ns.refWinners,Sampler::nearest());
    cmd.setBinding(1,ns.winners,Sampler::nearest());
    cmd.setBinding(2,ns.refDepth,Sampler::nearest());
    cmd.setBinding(3,zbuffer,Sampler::nearest());
    cmd.setBinding(4,ns.refDiffuse,Sampler::nearest());
    cmd.setBinding(5,gbufDiffuse,Sampler::nearest());
    cmd.setBinding(6,read);
    cmd.setPipeline(ns.verify);
    cmd.dispatchThreads(size);
    ns.verifyTick[fId] = GameServices::inst().worldTickCount()+1;
    ns.verifySegment[fId] = request.segment;
    }
  contourPrepare(1);
  history(2);
  if(profile) {
    auto& read = ns.statsRead[fId];
    if(read.isEmpty())
      read = device.ssbo(BufferHeap::Readback,Uninitialized,StatsCount*sizeof(uint32_t));
    cmd.setBinding(0,read);
    cmd.setBinding(1,ns.stats);
    cmd.setPipeline(shaders.copyBuf);
    cmd.dispatchThreads(StatsCount);
    ns.statsPending[fId] = true;
    }
  ns.valid = true;
  }

void Renderer::prepareTreeNaniteVsm(Encoder<CommandBuffer>& cmd, uint8_t fId, WorldView& view, const StorageBuffer& pageList) {
  auto& ns = treeNanite;
  if(!ns.active)
    return;
  auto& device = Resources::device();
  if(ns.shadow.isEmpty()) {
    RenderState state;
    state.setCullFaceMode(RenderState::CullMode::NoCull);
    state.setZWriteEnabled(true);
    state.setZTestMode(RenderState::ZTestMode::Greater);
    ns.shadow = device.pipeline(state,{},shader("tree_nanite_vsm.mesh.sprv"),{});
    ns.shadowPrepare = compute("tree_nanite_vsm_prepare.comp.sprv");
    ns.shadowPairs = compute("tree_nanite_vsm_pairs.comp.sprv");
    }
  constexpr uint32_t statsWords = 4;
  if(ns.shadowPending[fId]) {
    uint32_t values[statsWords] = {};
    device.readBytes(ns.shadowRead[fId],values,sizeof(values));
    FrameProfile::add(FrameProfile::Amount::TreeNaniteVsmPairs,values[0]);
    FrameProfile::add(FrameProfile::Amount::TreeNaniteVsmTriangles,values[1]);
    FrameProfile::add(FrameProfile::Amount::TreeNaniteVsmOverflow,values[2]);
    if(values[2]!=0)
      Log::e("Tree Nanite VSM pair capacity exceeded: ",values[2]);
    ns.shadowPending[fId] = false;
    }
  const auto& visuals = view.treeVisuals();
  const auto& asset = *visuals.treeNaniteMesh()->treeNanite;
  usesSsbo(ns.shadowList,size_t(ShadowCapacity)*2*sizeof(uint32_t));
  usesSsbo(ns.shadowIndirect,(1+ShadowCapacity/ShadowChunk)*4*sizeof(uint32_t));
  usesSsbo(ns.shadowStats,statsWords*sizeof(uint32_t));

  cmd.setFramebuffer({});
  cmd.setDebugMarker("TreeNanite-VSM-pairs");
  cmd.setBinding(1,asset.meshlets);
  cmd.setBinding(4,visuals.treeNaniteInstances());
  cmd.setBinding(7,ns.frame[fId]);
  cmd.setBinding(16,view.sceneGlobals().uboGlobal[SceneGlobals::V_Main]);
  cmd.setBinding(18,visuals.instanceSsbo());
  cmd.setBinding(19,visuals.instanceSsboPrev());
  cmd.setBinding(20,asset.shadow);
  cmd.setBinding(21,pageList);
  cmd.setBinding(22,view.lights().lightsSsbo());
  cmd.setBinding(23,ns.shadowList);
  cmd.setBinding(24,ns.shadowStats);
  cmd.setBinding(25,ns.shadowIndirect);
  uint32_t push[] = {asset.shadowCount,ShadowCapacity,0u};
  cmd.setPushData(push);
  cmd.setPipeline(ns.shadowPrepare);
  cmd.dispatch(1);
  cmd.setPipeline(ns.shadowPairs);
  cmd.dispatchIndirect(ns.shadowIndirect,0);
  push[2] = 1;
  cmd.setPushData(push);
  cmd.setPipeline(ns.shadowPrepare);
  cmd.dispatch(1);

  auto& read = ns.shadowRead[fId];
  if(read.isEmpty())
    read = device.ssbo(BufferHeap::Readback,Uninitialized,statsWords*sizeof(uint32_t));
  cmd.setBinding(0,read);
  cmd.setBinding(1,ns.shadowStats);
  cmd.setPipeline(shaders.copyBuf);
  cmd.dispatchThreads(statsWords);
  ns.shadowPending[fId] = true;
  }

void Renderer::drawTreeNaniteVsm(Encoder<CommandBuffer>& cmd, uint8_t fId, WorldView& view, const StorageBuffer& pageList) {
  const auto& ns = treeNanite;
  if(!ns.active)
    return;
  const auto& visuals = view.treeVisuals();
  const auto& asset = *visuals.treeNaniteMesh()->treeNanite;
  cmd.setDebugMarker("TreeNanite-VSM");
  bindGeometry(cmd,view,asset,ns.poses,visuals);
  cmd.setBinding(7,ns.frame[fId]);
  cmd.setBinding(16,view.sceneGlobals().uboGlobal[SceneGlobals::V_Main]);
  cmd.setBinding(20,asset.shadow);
  cmd.setBinding(21,pageList);
  cmd.setBinding(22,view.lights().lightsSsbo());
  cmd.setBinding(23,ns.shadowList);
  cmd.setBinding(24,ns.shadowStats);
  cmd.setPipeline(ns.shadow);
  for(uint32_t chunk=0; chunk<ShadowCapacity/ShadowChunk; ++chunk) {
    const uint32_t push[] = {chunk*ShadowChunk,ShadowCapacity,asset.shadowCount};
    cmd.setPushData(push);
    cmd.dispatchMeshIndirect(ns.shadowIndirect,size_t(chunk+1)*4*sizeof(uint32_t));
    }
  }
