#include "drawcommands.h"

#include <Tempest/Log>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdlib>

#include "graphics/clusterlodrender.h"
#include "graphics/mesh/submesh/animmesh.h"
#include "graphics/mesh/submesh/staticmesh.h"
#include "graphics/mesh/submesh/packedmesh.h"
#include "graphics/visualobjects.h"
#include "shaders.h"

#include "benchprobe.h"
#include "frameprofile.h"

using namespace Tempest;

namespace {

enum class LodView { Default, Base, Normal, Levels };
LodView benchLodView = LodView::Default;

struct LodPush {
  float    thresholdPx = 0;
  float    projection  = 0;
  float    height      = 0;
  uint32_t debug       = 0;
  Vec4     camera;
  };

LodPush clusterLodView(const DrawClusters& clusters, const SceneGlobals& scene) {
  if(!clusters.hasLevels())
    return {};

  static const auto thresholdPx = [] {
    const char* value = std::getenv("OPENGOTHIC_CLUSTER_LOD_PX");
    if(value==nullptr || *value=='\0')
      return 1.f;
    char* end = nullptr;
    const float px = std::strtof(value, &end);
    if(end!=value && *end=='\0' && std::isfinite(px) && px>=0.f)
      return px;
    Log::e("Invalid OPENGOTHIC_CLUSTER_LOD_PX; using 1 pixel");
    return 1.f;
    }();
  static const auto envLevels = [] {
    const char* mode = std::getenv("OPENGOTHIC_CLUSTER_LOD");
    return mode!=nullptr && std::string_view(mode)=="levels";
    }();
  const bool debug = benchLodView==LodView::Levels || (benchLodView==LodView::Default && envLevels);
  const auto eye = scene.camPos();
  return {benchLodView==LodView::Base ? 0.f : thresholdPx, scene.verticalProjection(),
          float(scene.screenRes().y), uint32_t(debug), Vec4(eye.x, eye.y, eye.z, 0.f)};
  }

}

void ClusterLodRender::setBenchView(std::string_view mode) {
  if(mode=="base")
    benchLodView = LodView::Base;
  else if(mode=="normal")
    benchLodView = LodView::Normal;
  else if(mode=="levels")
    benchLodView = LodView::Levels;
  else
    benchLodView = LodView::Default;
  }

// OPENGOTHIC_VSM_PAIRS=<n>: another cap of the VSM meshlet-page pairs, for a study (the demand of a scene without the
// cap, an overflow without a forest); the buffers follow it
uint32_t DrawCommands::vsmPairCap() {
  static const uint32_t cap = [] {
    const char* e = std::getenv("OPENGOTHIC_VSM_PAIRS");
    const unsigned long v = (e!=nullptr && *e!='\0') ? std::strtoul(e, nullptr, 0) : 0;
    return v==0 ? VsmMaxMeshlets : uint32_t(std::clamp<unsigned long>(v, 1024u, 16u*1024u*1024u));
    }();
  return cap;
  }

static void usesSsbo(StorageBuffer& b, const size_t desiredSz) {
  if(b.byteSize()<desiredSz || b.byteSize()>=2*desiredSz) {
    Resources::recycle(std::move(b));
    b = Resources::device().ssbo(Tempest::Uninitialized, desiredSz);
    }
  }

bool DrawCommands::DrawCmd::isForwardShading() const {
  return Material::isForwardShading(alpha);
  }

bool DrawCommands::DrawCmd::isShadowmapRequired() const {
  return Material::isShadowmapRequired(alpha);
  }

bool DrawCommands::DrawCmd::isSceneInfoRequired() const {
  return Material::isSceneInfoRequired(alpha);
  }

bool DrawCommands::DrawCmd::isTextureInShadowPass() const {
  return Material::isTextureInShadowPass(alpha);
  }

bool DrawCommands::DrawCmd::isBindless() const {
  return bucketId==uint32_t(-1);
  }

bool DrawCommands::DrawCmd::isMeshShader() const {
  if(!Shaders::options().doMeshShading)
    return false;
  if(Material::isTesselated(alpha) && type==DrawCommands::Landscape && Resources::device().properties().tesselationShader)
    return false;
  return true;
  }


DrawCommands::DrawCommands(VisualObjects& owner, DrawBuckets& buckets, DrawClusters& clusters, const SceneGlobals& scene)
    : owner(owner), buckets(buckets), clusters(clusters), scene(scene), vsmSupported(Shaders::isVsmSupported()) {
  for(uint8_t v=0; v<SceneGlobals::V_Count; ++v) {
    views[v].viewport = SceneGlobals::VisCamera(v);
    }

  if(vsmSupported) {
    Tempest::DispatchIndirectCommand cmd = {2000,1,1};
    vsmIndirectCmd = Resources::device().ssbo(&cmd, sizeof(cmd));
    }
  // vsmSwrImage = Resources::device().image2d(TextureFormat::R16,  4096, 4096);
  // vsmSwrImage = Resources::device().image2d(TextureFormat::R32U, 4096, 4096);
  }

DrawCommands::~DrawCommands() {
  }

void DrawCommands::bindSunVsm(Tempest::Encoder<Tempest::CommandBuffer>& cmd, const SceneGlobals& scene,
                              uint32_t tblSlot, uint32_t atlasSlot) {
  // the forward pass's sun shadow (materials/main.frag sunShadow): VSM when its pages are there, else the cascades
  if(scene.vsmEnabled && scene.vsmPageTbl!=nullptr && !scene.vsmPageTbl->isEmpty() && scene.vsmPageData!=nullptr) {
    cmd.setBinding(tblSlot,   *scene.vsmPageTbl);
    cmd.setBinding(atlasSlot, *scene.vsmPageData);
    } else {
    cmd.setBinding(tblSlot,   Resources::fallbackPageTable());
    cmd.setBinding(atlasSlot, Resources::fallbackBlack());
    }
  }

bool DrawCommands::isViewEnabled(SceneGlobals::VisCamera viewport) const {
  if(viewport==SceneGlobals::V_Vsm && !(vsmSupported && scene.vsmEnabled))
    return false;
  if(viewport==SceneGlobals::V_Shadow0 && scene.shadowMap[0]->size()==Size(1,1))
    return false;
  if(viewport==SceneGlobals::V_Shadow1 && scene.shadowMap[1]->size()==Size(1,1))
    return false;
  return true;
  }

void DrawCommands::setBindings(Tempest::Encoder<CommandBuffer>& cmd, const DrawCmd& cx, SceneGlobals::VisCamera v) {
  static const DrawBuckets::Bucket nullBk;

  const auto  bId = cx.bucketId;
  const auto& bx  = cx.isBindless() ? nullBk : buckets.buckets()[bId];

  cmd.setBinding(L_Scene,    scene.uboGlobal[v]);
  cmd.setBinding(L_Payload,  views[v].visClusters);
  cmd.setBinding(L_Instance, owner.instanceSsbo());
  cmd.setBinding(L_Bucket,   buckets.ssbo());
  if(v==SceneGlobals::V_Main)
    cmd.setBinding(L_InstancePrev, owner.instanceSsboPrev()); // read by the G-buffer variants of objects only

  if(cx.isBindless()) {
    cmd.setBinding(L_Ibo, buckets.ibo());
    cmd.setBinding(L_Vbo, buckets.vbo());
    }
  else if(bx.staticMesh!=nullptr) {
    cmd.setBinding(L_Ibo, bx.staticMesh->ibo8Buffer());
    cmd.setBinding(L_Vbo, bx.staticMesh->vboBuffer());
    }
  else {
    cmd.setBinding(L_Ibo, bx.animMesh->ibo8);
    cmd.setBinding(L_Vbo, bx.animMesh->vbo);
    }

  if(v==SceneGlobals::V_Main || cx.isTextureInShadowPass()) {
    if(cx.isBindless()) {
      cmd.setBinding(L_Diffuse, buckets.textures());
      }
    else if(bx.mat.hasFrameAnimation()) {
      uint64_t timeShift = 0;
      auto     frame     = size_t((timeShift+scene.tickCount)/bx.mat.texAniFPSInv);
      auto*    t         = bx.mat.frames[frame%bx.mat.frames.size()];
      cmd.setBinding(L_Diffuse, *t);
      }
    else {
      cmd.setBinding(L_Diffuse, *bx.mat.tex);
      }
    auto smp = SceneGlobals::isShadowView(v) ? Sampler::trillinear() : Sampler::anisotrophy();
    cmd.setBinding(L_Sampler, smp);
    }

  if(Shaders::options().doMaterialMaps && v==SceneGlobals::V_Main &&
     (cx.alpha==Material::Solid || cx.alpha==Material::AlphaTest)) {
    if(cx.isBindless()) {
      cmd.setBinding(L_Normal, buckets.normals());
      cmd.setBinding(L_Orm, buckets.orm());
      if(cx.type==Landscape)
        cmd.setBinding(L_GrassBorder, scene.grassBorder!=nullptr ? *scene.grassBorder : scene.grassBorderNone);
      } else {
      cmd.setBinding(L_Normal, bx.mat.normal!=nullptr ? *bx.mat.normal : Resources::fallbackTexture());
      cmd.setBinding(L_Orm, bx.mat.orm!=nullptr ? *bx.mat.orm : Resources::fallbackTexture());
      const auto& f = bx.mat.fringe;
      const bool  on = bx.mat.hasFringe();
      cmd.setBinding(L_FringeMain,   on ? *f.albedo : Resources::fallbackTexture());
      cmd.setBinding(L_FringeNormal, on ? *f.normal : Resources::fallbackTexture());
      cmd.setBinding(L_FringeOrm,    on ? *f.orm    : Resources::fallbackTexture());
      }
    }

  if(v==SceneGlobals::V_Main && cx.isShadowmapRequired()) {
    cmd.setBinding(L_Shadow0, *scene.shadowMap[0], Resources::shadowSampler());
    cmd.setBinding(L_Shadow1, *scene.shadowMap[1], Resources::shadowSampler());
    bindSunVsm(cmd, scene, L_VsmPageTbl, L_VsmPageAtlas);
    }

  if(cx.type==Morph && cx.isBindless()) {
    cmd.setBinding(L_MorphId,  buckets.morphId());
    cmd.setBinding(L_Morph,    buckets.morph());
    }
  else if(cx.type==Morph && bx.staticMesh!=nullptr) {
    cmd.setBinding(L_MorphId,  *bx.staticMesh->morph.index);
    cmd.setBinding(L_Morph,    *bx.staticMesh->morph.samples);
    }

  if(v==SceneGlobals::V_Main && cx.isSceneInfoRequired()) {
    cmd.setBinding(L_SceneClr, *scene.sceneColor, Sampler::bilinear(ClampMode::MirroredRepeat));
    cmd.setBinding(L_GDepth,   *scene.sceneDepth, Sampler::nearest (ClampMode::MirroredRepeat));
    }

  if(v==SceneGlobals::V_Main && Material::isFogRequired(cx.alpha)) {
    cmd.setBinding(L_FogLut,   *scene.fogLut,   Sampler::bilinear(ClampMode::ClampToEdge));
    cmd.setBinding(L_FogLutMs, *scene.fogLutMs, Sampler::bilinear(ClampMode::ClampToEdge));
    cmd.setBinding(L_FogOcc,   *scene.fogOcclusion);
    }

  if(v==SceneGlobals::V_Vsm) {
    cmd.setBinding(L_CmdOffsets, views[v].indirectCmd);
    cmd.setBinding(L_VsmPages,   *scene.vsmPageList);
    cmd.setBinding(L_Lights,     *scene.lights);
    }
  }

uint16_t DrawCommands::commandId(const Material& m, Type type, uint32_t bucketId) {
  const bool bindlessSys = Shaders::options().doBindless;
  const bool bindless    = bindlessSys && (!m.hasFrameAnimation() || type==Landscape);

  auto pMain    = Shaders::inst().materialPipeline(m, type, Shaders::T_Main,   bindless);
  auto pShadow  = Shaders::inst().materialPipeline(m, type, Shaders::T_Shadow, bindless);
  auto pVsm     = Shaders::inst().materialPipeline(m, type, Shaders::T_Vsm,    bindless);
  auto pHiZ     = Shaders::inst().materialPipeline(m, type, Shaders::T_Depth,  bindless);
  if(pMain==nullptr && pShadow==nullptr && pHiZ==nullptr)
    return uint16_t(-1);

  for(size_t i=0; i<cmd.size(); ++i) {
    if(cmd[i].pMain!=pMain || cmd[i].pShadow!=pShadow || cmd[i].pHiZ!=pHiZ)
      continue;
    if(!bindless && cmd[i].bucketId!=bucketId)
      continue;
    return uint16_t(i);
    }

  auto ret = uint16_t(cmd.size());

  DrawCmd cx;
  cx.pMain       = pMain;
  cx.pShadow     = pShadow;
  cx.pVsm        = pVsm;
  cx.pHiZ        = pHiZ;
  cx.bucketId    = bindless ? 0xFFFFFFFF : bucketId;
  cx.type        = type;
  cx.alpha       = m.alpha;
  cmd.push_back(std::move(cx));
  cmdDurtyBit = true;
  return ret;
  }

void DrawCommands::addClusters(uint16_t cmdId, uint32_t meshletCount) {
  cmdDurtyBit = true;
  cmd[cmdId].maxPayload += meshletCount;
  }

void DrawCommands::commit(Encoder<CommandBuffer>& enc) {
  bool cmdChg = false;
  for(auto& v:views) {
    if(isViewEnabled(v.viewport)) {
      cmdChg |= (v.indirectCmd.byteSize() != sizeof(IndirectCmd)*cmd.size());
      } else {
      Resources::recycle(std::move(v.indirectCmd));
      Resources::recycle(std::move(v.visClusters));
      Resources::recycle(std::move(v.vsmClusters));
      }
    }

  if(!cmdDurtyBit && !cmdChg)
    return;
  cmdDurtyBit = false;

  ord.resize(cmd.size());
  for(size_t i=0; i<cmd.size(); ++i)
    ord[i] = &cmd[i];
  std::sort(ord.begin(), ord.end(), [](const DrawCmd* l, const DrawCmd* r){
    return l->alpha < r->alpha;
    });

  size_t totalPayload = 0;
  bool   layChg       = false;
  for(auto& i:cmd) {
    layChg |= (i.firstPayload != uint32_t(totalPayload));
    i.firstPayload = uint32_t(totalPayload);
    totalPayload  += i.maxPayload;
    }

  totalPayload = (totalPayload + 0xFF) & ~size_t(0xFF);
  this->maxPayload = totalPayload;

  if(!cmdChg && !layChg) {
    return;
    }

  std::vector<IndirectCmd> cx(cmd.size());
  for(size_t i=0; i<cmd.size(); ++i) {
    auto mesh = cmd[i].isMeshShader();

    cx[i].vertexCount   = PackedMesh::MaxInd;
    cx[i].writeOffset   = cmd[i].firstPayload;
    cx[i].firstVertex   = mesh ? 1 : 0;
    cx[i].firstInstance = mesh ? 1 : 0;
    }

  auto& device = Resources::device();
  for(auto& v:views) {
    if(!isViewEnabled(v.viewport))
      continue;
    if(v.indirectCmd.byteSize() != sizeof(IndirectCmd)*cmd.size()) {
      Resources::recycle(std::move(v.indirectCmd));
      v.indirectCmd = device.ssbo(Uninitialized,sizeof(IndirectCmd)*cx.size());
      v.indirectCmd.updateAsync(cx.data(),0,sizeof(IndirectCmd)*cx.size());
      }
    else if(layChg) {
      auto staging = device.ssbo(BufferHeap::Upload, cx.data(), sizeof(IndirectCmd)*cx.size());

      enc.setBinding(0, v.indirectCmd);
      enc.setBinding(1, staging);
      enc.setPipeline(Shaders::inst().copyBuf);
      enc.dispatchThreads(staging.byteSize()/sizeof(uint32_t));

      Resources::recycle(std::move(staging));
      }
    }
  }

void DrawCommands::visibilityPass(Encoder<CommandBuffer>& cmd, int pass) {
  static bool freeze = false;
  if(freeze)
    return;

  cmd.setFramebuffer({});
  if(pass==0) {
    for(auto& v:views) {
      if(this->cmd.empty())
        continue;
      if(!isViewEnabled(v.viewport))
        continue;
      const uint32_t isMeshShader = (Shaders::options().doMeshShading ? 1 : 0);
      cmd.setBinding(T_Indirect, v.indirectCmd);
      cmd.setPushData(&isMeshShader, sizeof(isMeshShader));
      cmd.setPipeline(Shaders::inst().clusterInit);
      cmd.dispatchThreads(this->cmd.size());
      }
    }

  for(uint8_t v=0; v<SceneGlobals::V_Count; ++v) {
    const auto viewport = SceneGlobals::VisCamera(v);
    if(viewport==SceneGlobals::V_HiZ && pass!=0)
      continue;
    if(viewport!=SceneGlobals::V_HiZ && pass==0)
      continue;
    if(viewport==SceneGlobals::V_Vsm)
      continue;
    if(!isViewEnabled(viewport))
      continue;

    auto& view = views[viewport];

    const size_t visClustersSz = maxPayload*sizeof(uint32_t)*4;
    usesSsbo(view.visClusters, visClustersSz);

    struct Push {
      uint32_t firstMeshlet, meshletCount;
      float znear;
      alignas(16) LodPush lod;
      } push = {};
    static_assert(offsetof(Push,lod)==16 && sizeof(Push)==48);
    push.firstMeshlet = 0;
    push.meshletCount = uint32_t(clusters.size());
    push.znear        = scene.znear;
    push.lod          = clusterLodView(clusters, scene);

    // bench `deterministic on`: one invocation appends the visible clusters in their canonical order (orderedClusters),
    // so the instance order of a draw (who wins equal depth, the order of blending) depends neither on how GPU threads
    // race for atomicAdd nor on the order the streaming threads published chunks and vobs in
    const auto* probe = BenchProbe::get();
    const bool ordered = probe!=nullptr && probe->orderedVisibility();
    const Tempest::StorageBuffer* order = nullptr;
    if(ordered) {
      order = &orderedClusters();
      push.meshletCount = uint32_t(orderIds.size());
      }
    auto* pso = ordered ? &Shaders::inst().visibilityPassShOrd : &Shaders::inst().visibilityPassSh;
    if(viewport==SceneGlobals::V_Main)
      pso = ordered ? &Shaders::inst().visibilityPassHiZOrd : &Shaders::inst().visibilityPassHiZ;
    else if(viewport==SceneGlobals::V_HiZ)
      pso = ordered ? &Shaders::inst().visibilityPassHiZCrOrd : &Shaders::inst().visibilityPassHiZCr;

    cmd.setBinding(T_Scene,    scene.uboGlobal[viewport]);
    cmd.setBinding(T_Payload,  view.visClusters);
    cmd.setBinding(T_Instance, owner.instanceSsbo());
    cmd.setBinding(T_Bucket,   buckets.ssbo());
    cmd.setBinding(T_Indirect, view.indirectCmd);
    cmd.setBinding(T_Clusters, clusters.ssbo());
    cmd.setBinding(T_HiZ,      *scene.hiZ);
    cmd.setBinding(8,         clusters.levelIndicesSsbo());
    cmd.setBinding(9,         clusters.levelsSsbo());
    if(order!=nullptr)
      cmd.setBinding(T_Order,  *order);
    cmd.setPushData(push);
    cmd.setPipeline(*pso);
    cmd.dispatchThreads(ordered ? 1 : push.meshletCount);
    }
  }

void DrawCommands::visibilityStats(Encoder<CommandBuffer>& cmd, uint8_t fId) {
  // the counters of the main view's culling for the frame profile (visibility_stats.comp): what this frame slot wrote
  // last time - its fence has been waited for, the buffer is host-visible - then this frame's. Without the profile
  // nothing runs
  if(!FrameProfile::enabled())
    return;
  if(statsWritten[fId]) {
    uint32_t v[4] = {};
    Resources::device().readBytes(statsRead[fId], v, sizeof(v));
    FrameProfile::add(FrameProfile::Amount::MainClusters,       v[0]);
    FrameProfile::add(FrameProfile::Amount::MainMeshlets,       v[1]);
    FrameProfile::add(FrameProfile::Amount::MainCanopyClusters, v[2]);
    FrameProfile::add(FrameProfile::Amount::MainCanopyMeshlets, v[3]);
    statsWritten[fId] = false;
    }

  auto& view = views[SceneGlobals::V_Main];
  if(this->cmd.empty() || maxPayload==0 || view.visClusters.isEmpty() || view.indirectCmd.isEmpty())
    return;
  auto& device = Resources::device();
  if(statsSum.isEmpty())
    statsSum = device.ssbo(Tempest::Uninitialized, 4*sizeof(uint32_t));
  if(statsRead[fId].isEmpty())
    statsRead[fId] = device.ssbo(Tempest::BufferHeap::Readback, Tempest::Uninitialized, 4*sizeof(uint32_t));

  struct Push { uint32_t phase; uint32_t count; } push = {};
  push.count = uint32_t(maxPayload);
  cmd.setDebugMarker("Visibility-stats");
  cmd.setFramebuffer({});
  cmd.setBinding(0, view.visClusters);
  cmd.setBinding(1, view.indirectCmd);
  cmd.setBinding(2, buckets.ssbo());
  cmd.setBinding(3, statsSum);
  cmd.setBinding(4, statsRead[fId]);
  cmd.setPipeline(Shaders::inst().visibilityStats);
  for(uint32_t phase=0; phase<3; ++phase) {
    push.phase = phase;
    cmd.setPushData(push);
    if(phase==1)
      cmd.dispatchThreads(maxPayload); else
      cmd.dispatch(1);
    }
  statsWritten[fId] = true;
  }

const Tempest::StorageBuffer& DrawCommands::orderedClusters() {
  orderNext.clear();
  for(size_t i=0; i<clusters.size(); ++i)
    if(clusters[i].r>0)
      orderNext.push_back(uint32_t(i));
  // a cluster is named by what it draws: its sphere, then the meshlets of its mesh; ids, buckets and commands are
  // handed out in the order of publication and differ between runs
  std::stable_sort(orderNext.begin(), orderNext.end(), [this](uint32_t a, uint32_t b) {
    const auto& l = clusters[a];
    const auto& r = clusters[b];
    if(l.pos.x!=r.pos.x) return l.pos.x<r.pos.x;
    if(l.pos.y!=r.pos.y) return l.pos.y<r.pos.y;
    if(l.pos.z!=r.pos.z) return l.pos.z<r.pos.z;
    if(l.r!=r.r)         return l.r<r.r;
    if(l.meshletCount!=r.meshletCount) return l.meshletCount<r.meshletCount;
    return l.firstMeshlet<r.firstMeshlet;
    });
  if(orderNext!=orderIds || orderSsbo.isEmpty()) {
    std::swap(orderIds, orderNext);
    Resources::recycle(std::move(orderSsbo));
    if(orderIds.empty())
      orderSsbo = Resources::device().ssbo(Tempest::BufferHeap::Upload, Tempest::Uninitialized, sizeof(uint32_t)); else
      orderSsbo = Resources::device().ssbo(Tempest::BufferHeap::Upload, orderIds);
    }
  return orderSsbo;
  }

void DrawCommands::visibilityVsm(Encoder<CommandBuffer>& cmd) {
  auto& shaders = Shaders::inst();

  auto& view = views[SceneGlobals::V_Vsm];
  const uint32_t cap = vsmPairCap();
  const size_t pairsSz = size_t(cap)*sizeof(uint32_t)*4;

  usesSsbo(vsmCandidates,    pairsSz); // a cluster and a page entry its sphere touches, the visibility pass's
  usesSsbo(view.vsmClusters, pairsSz); // the pairs of meshlet and entry, as the meshlets' pass writes them...
  usesSsbo(view.visClusters, pairsSz); // ...and by draw

  struct Push {
    uint32_t meshletCount, candidateCap;
    alignas(16) LodPush lod;
    } push = {};
  static_assert(offsetof(Push,lod)==16 && sizeof(Push)==48);
  push.meshletCount = uint32_t(clusters.size());
  push.candidateCap = cap;
  push.lod          = clusterLodView(clusters, scene);

  cmd.setBinding(T_Scene,    scene.uboGlobal[SceneGlobals::V_Vsm]);
  cmd.setBinding(T_Payload,  vsmCandidates);
  cmd.setBinding(T_Instance, owner.instanceSsbo());
  cmd.setBinding(T_Bucket,   buckets.ssbo());
  cmd.setBinding(T_Indirect, view.indirectCmd);
  cmd.setBinding(T_Clusters, clusters.ssbo());
  cmd.setBinding(T_Lights,   *scene.lights);
  cmd.setBinding(T_HiZ,      *scene.vsmPageHiZ);
  cmd.setBinding(T_VsmPages, *scene.vsmPageList);
  cmd.setBinding(9,          scene.vsmDbg);
  cmd.setBinding(10,         clusters.levelIndicesSsbo());
  cmd.setBinding(11,         clusters.levelsSsbo());
  cmd.setPushData(&push, sizeof(push));
  cmd.setPipeline(shaders.vsmVisibilityPass);
  cmd.dispatchThreads(push.meshletCount, size_t(scene.vsmPageTbl->d() + 1));

  // the candidates' meshlets (vsm_cull_meshlets.comp): an object's by their spheres; the first pass counts them by
  // classes, the second writes the classes that fit under the cap. Scene, candidates, instances, buckets, draws,
  // clusters and lights keep their slots
  struct CullPush { uint32_t pairCap; uint32_t emit; } cull = {cap, 0};
  cmd.setBinding(7, buckets.meshletSpheres());
  cmd.setBinding(8, *scene.vsmPageList);
  cmd.setBinding(9, view.vsmClusters);
  cmd.setPipeline(shaders.vsmCullMeshlets);
  for(cull.emit=0; cull.emit<2; ++cull.emit) {
    cmd.setPushData(cull);
    cmd.dispatch(VsmCullGroups);
    }

  cmd.setBinding(1, view.vsmClusters);
  cmd.setBinding(2, view.visClusters);
  cmd.setBinding(3, view.indirectCmd);
  cmd.setBinding(4, *scene.vsmPageList);
  cmd.setBinding(5, vsmIndirectCmd);
  cmd.setPipeline(shaders.vsmPackDraw0);
  cmd.dispatch(1);

  cmd.setBinding(1, view.vsmClusters);
  cmd.setBinding(2, view.visClusters);
  cmd.setBinding(3, view.indirectCmd);
  cmd.setBinding(4, *scene.vsmPageList);
  cmd.setPushData(&cap, sizeof(cap));
  cmd.setPipeline(shaders.vsmPackDraw1);
  cmd.dispatch((cap+127)/128); // vsm_pack_draws.comp: 128 threads; TODO: indirect
  // cmd.dispatchIndirect(vsmIndirectCmd, 0);
  }

uint32_t DrawCommands::hiddenTypes = 0;
bool     DrawCommands::hidePfxObjects = false;

void DrawCommands::drawVsm(Tempest::Encoder<Tempest::CommandBuffer>& cmd) {
  // return;
  struct Push { uint32_t commandId; } push = {};

  auto  viewId = SceneGlobals::V_Vsm;
  auto& view   = views[viewId];

  for(size_t i=0; i<ord.size(); ++i) {
    auto& cx = *ord[i];
    if(cx.maxPayload==0)
      continue;
    if(cx.alpha!=Material::Solid && cx.alpha!=Material::AlphaTest)
      continue;
    if(cx.pVsm==nullptr)
      continue;
    if((hiddenTypes>>cx.type) & 1u)
      continue;

    auto id  = size_t(std::distance(this->cmd.data(), &cx));
    push.commandId = uint32_t(id);

    // cmd.setUniforms(*cx.pVsm, desc[viewId], &push, sizeof(push));
    setBindings(cmd, cx, viewId);
    cmd.setPushData(push);
    cmd.setPipeline(*cx.pVsm);
    if(cx.isMeshShader())
      cmd.dispatchMeshIndirect(view.indirectCmd, sizeof(IndirectCmd)*id + sizeof(uint32_t)); else
      cmd.drawIndirect(view.indirectCmd, sizeof(IndirectCmd)*id);
    }

  if(false) {
    struct Push { uint32_t meshletCount; } push = {};
    push.meshletCount = uint32_t(clusters.size());

    cmd.setFramebuffer({});
    cmd.setBinding(0, *scene.vsmPageData);
    cmd.setBinding(1, scene.uboGlobal[SceneGlobals::V_Vsm]);
    cmd.setBinding(2, *scene.vsmPageList);
    cmd.setBinding(3, clusters.ssbo());
    cmd.setBinding(4, owner.instanceSsbo());
    cmd.setBinding(5, buckets.ibo());
    cmd.setBinding(6, buckets.vbo());
    cmd.setBinding(7, buckets.textures());
    cmd.setBinding(8, Sampler::bilinear());
    cmd.setPushData(&push, sizeof(push));
    cmd.setPipeline(Shaders::inst().vsmRendering);
    // const auto sz = Shaders::inst().vsmRendering.workGroupSize();
    cmd.dispatch(1024u);
    }
  }

void DrawCommands::drawHiZ(Tempest::Encoder<Tempest::CommandBuffer>& cmd) {
  // return;
  struct Push { uint32_t firstMeshlet; uint32_t meshletCount; } push = {};

  auto  viewId = SceneGlobals::V_HiZ;
  auto& view   = views[viewId];

  for(size_t i=0; i<ord.size(); ++i) {
    auto& cx = *ord[i];
    if(cx.maxPayload==0)
      continue;
    if(cx.alpha!=Material::Solid && cx.alpha!=Material::AlphaTest)
      continue;
    if(cx.type!=Landscape && cx.type!=Static)
      continue;
    if(cx.pHiZ==nullptr)
      continue;
    if((hiddenTypes>>cx.type) & 1u)
      continue;

    auto id  = size_t(std::distance(this->cmd.data(), &cx));
    push.firstMeshlet = cx.firstPayload;
    push.meshletCount = cx.maxPayload;

    setBindings(cmd, cx, viewId);
    cmd.setPushData(push);
    cmd.setPipeline(*cx.pHiZ);
    if(cx.isMeshShader())
      cmd.dispatchMeshIndirect(view.indirectCmd, sizeof(IndirectCmd)*id + sizeof(uint32_t)); else
      cmd.drawIndirect(view.indirectCmd, sizeof(IndirectCmd)*id);
    }
  }

void DrawCommands::drawCommon(Tempest::Encoder<Tempest::CommandBuffer>& cmd, SceneGlobals::VisCamera viewId, Material::AlphaFunc func) {
  struct Push { uint32_t firstMeshlet; uint32_t meshletCount; } push = {};

  auto b = std::lower_bound(ord.begin(), ord.end(), func, [](const DrawCmd* l, Material::AlphaFunc f){
    return l->alpha < f;
    });
  auto e = std::upper_bound(ord.begin(), ord.end(), func, [](Material::AlphaFunc f, const DrawCmd* r){
    return f < r->alpha;
    });

  auto& view = views[viewId];
  for(auto i=b; i!=e; ++i) {
    auto& cx = **i;
    if(cx.alpha!=func)
      continue;

    if(cx.maxPayload==0)
      continue;
    if((hiddenTypes>>cx.type) & 1u)
      continue;

    const RenderPipeline* pso = nullptr;
    switch(viewId) {
      case SceneGlobals::V_Shadow0:
      case SceneGlobals::V_Shadow1:
      case SceneGlobals::V_Vsm:
        pso = cx.pShadow;
        break;
      case SceneGlobals::V_Main:
        pso = cx.pMain;
        break;
      case SceneGlobals::V_HiZ:
      case SceneGlobals::V_Count:
        break;
      }
    if(pso==nullptr)
      continue;

    auto id  = size_t(std::distance(this->cmd.data(), &cx));
    push.firstMeshlet = cx.firstPayload;
    push.meshletCount = cx.maxPayload;

    setBindings(cmd, cx, viewId);
    cmd.setPushData(push);
    cmd.setPipeline(*pso);
    if(cx.isMeshShader())
      cmd.dispatchMeshIndirect(view.indirectCmd, sizeof(IndirectCmd)*id + sizeof(uint32_t)); else
      cmd.drawIndirect(view.indirectCmd, sizeof(IndirectCmd)*id);
    }
  }
