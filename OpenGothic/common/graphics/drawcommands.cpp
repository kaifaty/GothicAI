#include "drawcommands.h"

#include <Tempest/Log>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <limits>
#include <ostream>
#include <unordered_map>

#include "graphics/clusterlodrender.h"
#include "graphics/mesh/submesh/animmesh.h"
#include "graphics/mesh/submesh/staticmesh.h"
#include "graphics/mesh/submesh/packedmesh.h"
#include "graphics/treeboughrender.h"
#include "graphics/visualobjects.h"
#include "shaders.h"

#include "benchprobe.h"
#include "frameprofile.h"

using namespace Tempest;

namespace {

enum class LodView { Default, Base, Normal, Levels };
LodView benchLodView = LodView::Default;
bool ownMeshMask = false;

struct LodPush {
  float    thresholdPx = 0;
  float    projection  = 0;
  float    height      = 0;
  uint32_t debug       = 0;
  Vec4     camera;
  Vec4     forwardAndGuard;
  };

LodPush clusterLodView(bool hasLevels, const SceneGlobals& scene) {
  if(!hasLevels)
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
  const float farDepth = scene.clipInfo().z;
  const auto& view = scene.view;
  // Convert four float epsilon of projected depth to a conservative view-space band at the far plane.
  const float guard = 4.f * std::numeric_limits<float>::epsilon() * farDepth * farDepth / std::max(scene.znear, 1.f);
  return {benchLodView==LodView::Base ? 0.f : thresholdPx, scene.verticalProjection(),
          float(scene.screenRes().y), uint32_t(debug), Vec4(eye.x, eye.y, eye.z, farDepth),
          Vec4(view[0][2], view[1][2], view[2][2], guard)};
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

void ClusterLodRender::setBenchOwnMeshMask(bool enabled) {
  ownMeshMask = enabled;
  }

bool ClusterLodRender::benchOwnMeshMask() {
  return ownMeshMask;
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
  static const DrawBuckets::BucketPub nullBk;

  const auto  bId = cx.bucketId;
  const auto& bx  = cx.isBindless() ? nullBk : buckets.published()[bId];

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
  else {
    // the published handles cover the static mesh, its bough page and the anim mesh alike
    cmd.setBinding(L_Ibo, bx.iboBuffer());
    cmd.setBinding(L_Vbo, bx.vboBuffer());
    }

  if(v==SceneGlobals::V_Main || cx.isTextureInShadowPass()) {
    if(cx.isBindless()) {
      cmd.setBinding(L_Diffuse, buckets.textures());
      }
    else if(bx.frameAni) {
      uint64_t timeShift = 0;
      auto     frame     = size_t((timeShift+scene.tickCount)/bx.texAniFPSInv);
      cmd.setBinding(L_Diffuse, bx.frames[frame%bx.frames.size()]);
      }
    else {
      cmd.setBinding(L_Diffuse, bx.tex);
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
      cmd.setBinding(L_Normal, !bx.normal.isEmpty() ? bx.normal : Resources::fallbackTexture());
      cmd.setBinding(L_Orm, !bx.orm.isEmpty() ? bx.orm : Resources::fallbackTexture());
      const bool  on = bx.fringeOn;
      cmd.setBinding(L_FringeMain,   on ? bx.frAlbedo : Resources::fallbackTexture());
      cmd.setBinding(L_FringeNormal, on ? bx.frNormal : Resources::fallbackTexture());
      cmd.setBinding(L_FringeOrm,    on ? bx.frOrm    : Resources::fallbackTexture());
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
  else if(cx.type==Morph && !bx.morphId.isEmpty()) {
    cmd.setBinding(L_MorphId,  bx.morphId);
    cmd.setBinding(L_Morph,    bx.morph);
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

uint16_t DrawCommands::commandId(const Material& m, Type type, uint32_t bucketId, bool shadowOnly) {
  const bool bindlessSys = Shaders::options().doBindless;
  const bool bindless    = bindlessSys && (!m.hasFrameAnimation() || type==Landscape);

  auto bough = Shaders::Bough::None;
  if(!shadowOnly && type==Static && !m.isGhost && (m.alpha==Material::Solid || m.alpha==Material::AlphaTest)) {
    const auto* mesh = buckets[bucketId].staticMesh;
    if(mesh!=nullptr && mesh->treeBough && mesh->impostor.px>0.f)
      bough = Shaders::Bough::Joint;
    }
  auto pMain    = shadowOnly ? nullptr : Shaders::inst().materialPipeline(m, type, Shaders::T_Main, bindless, bough);
  // the hierarchy mesh's shadows mask the sides of its level bands (graphics/treelevelband.h)
  auto pShadow  = Shaders::inst().materialPipeline(m, type, Shaders::T_Shadow, bindless, bough);
  auto pVsm     = Shaders::inst().materialPipeline(m, type, Shaders::T_Vsm,    bindless, bough);
  auto pHiZ     = shadowOnly ? nullptr : Shaders::inst().materialPipeline(m, type, Shaders::T_Depth, bindless);
  if(pMain==nullptr && pShadow==nullptr && pVsm==nullptr && pHiZ==nullptr)
    return uint16_t(-1);

  for(size_t i=0; i<cmd.size(); ++i) {
    if(cmd[i].pMain!=pMain || cmd[i].pShadow!=pShadow || cmd[i].pVsm!=pVsm || cmd[i].pHiZ!=pHiZ)
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
  if(pMain!=nullptr && (m.alpha==Material::Solid || m.alpha==Material::AlphaTest) && treeHistoryEnabled()) {
    cx.pHistoryGBuffer = Shaders::inst().materialPipeline(m, type, Shaders::T_HistoryGBuffer, bindless, bough);
    cx.pHistoryDepth   = Shaders::inst().materialPipeline(m, type, Shaders::T_HistoryDepth, bindless, bough);
    cx.pHistoryVerify  = Shaders::inst().materialPipeline(m, type, Shaders::T_HistoryDepthVerify, bindless, bough);
    }
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
  boughs = liveBoughs.get();
  if(boughs)
    boughs->commit();
  const auto* probe = BenchProbe::get();
  pubOrdered = probe!=nullptr && probe->orderedVisibility();
  if(pubOrdered)
    prepareOrderedClusters();
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

  if(!cmdDurtyBit && !cmdChg) {
    publish();
    return;
    }
  pubDirty |= cmdDurtyBit || cmdChg;
  cmdDurtyBit = false;
  history.dirty = true;

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
  pubDirty |= layChg;

  if(!cmdChg && !layChg) {
    publish();
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
  publish();
  }

void DrawCommands::publish() {
  const size_t cc = clusters.size();
  if(pub!=nullptr && !pubDirty && pub->clusterCount==cc && pub->hasLevels==clusters.hasLevels()) {
    cur = pub.get();
    return;
    }
  auto p = std::make_shared<Pub>();
  p->cmd = cmd;
  p->ord.resize(ord.size());
  if(cmd.empty())
    p->ord.clear();
  for(size_t i=0; i<p->ord.size(); ++i)
    p->ord[i] = &p->cmd[size_t(ord[i]-cmd.data())];
  p->maxPayload   = maxPayload;
  p->clusterCount = cc;
  p->hasLevels    = clusters.hasLevels();
  pub      = std::move(p);
  cur      = pub.get();
  pubDirty = false;
  }

void DrawCommands::visibilityPass(Encoder<CommandBuffer>& cmd, int pass, bool useHistory) {
  static bool freeze = false;
  if(freeze)
    return;

  cmd.setFramebuffer({});
  if(pass==0) {
    for(auto& v:views) {
      if(cur->cmd.empty())
        continue;
      if(!isViewEnabled(v.viewport))
        continue;
      const uint32_t isMeshShader = (Shaders::options().doMeshShading ? 1 : 0);
      cmd.setBinding(T_Indirect, v.indirectCmd);
      cmd.setPushData(&isMeshShader, sizeof(isMeshShader));
      cmd.setPipeline(Shaders::inst().clusterInit);
      cmd.dispatchThreads(cur->cmd.size());
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

    const size_t visClustersSz = cur->maxPayload*sizeof(uint32_t)*4;
    usesSsbo(view.visClusters, visClustersSz);

    struct Push {
      uint32_t firstMeshlet, meshletCount;
      float znear;
      uint32_t ownMeshMask;
      alignas(16) LodPush lod;
      } push = {};
    static_assert(offsetof(Push,lod)==16 && sizeof(Push)==64);
    push.firstMeshlet = 0;
    push.meshletCount = uint32_t(cur->clusterCount);
    push.znear        = scene.znear;
    push.ownMeshMask  = uint32_t(scene.isLandscapeMask() && ownMeshMask);
    push.lod          = clusterLodView(cur->hasLevels, scene);

    // bench `deterministic on`: one invocation appends the visible clusters in their published canonical order,
    // so the instance order of a draw (who wins equal depth, the order of blending) depends neither on how GPU threads
    // race for atomicAdd nor on the order the streaming threads published chunks and vobs in
    const bool ordered = pubOrdered;
    if(ordered)
      push.meshletCount = uint32_t(orderIds.size());
    const auto* pso = ordered ? &Shaders::inst().visibilityPassShOrd : &Shaders::inst().visibilityPassSh;
    if(viewport==SceneGlobals::V_Main) {
      pso = ordered ? &Shaders::inst().visibilityPassHiZOrd : &Shaders::inst().visibilityPassHiZ;
      cmd.setBinding(10, buckets.treeHiZBounds());
      if(scene.isTreeOcclusionProbe()) {
        pso = ordered ? &Shaders::inst().visibilityPassHiZProbeOrd : &Shaders::inst().visibilityPassHiZProbe;
        const std::vector<uint32_t> empty(cur->clusterCount);
        treeOcclusionDecisions = Resources::device().ssbo(Tempest::BufferHeap::Readback, empty);
        cmd.setBinding(11, treeOcclusionDecisions);
        } else if(useHistory) {
        pso = ordered ? &Shaders::inst().visibilityPassHistoryOrd : &Shaders::inst().visibilityPassHistory;
        cmd.setBinding(11, history.status);
        cmd.setBinding(12, history.commands);
        }
      } else if(viewport==SceneGlobals::V_HiZ) {
      pso = ordered ? &Shaders::inst().visibilityPassHiZCrOrd : &Shaders::inst().visibilityPassHiZCr;
      }

    cmd.setBinding(T_Scene,    scene.uboGlobal[viewport]);
    cmd.setBinding(T_Payload,  view.visClusters);
    cmd.setBinding(T_Instance, owner.instanceSsbo());
    cmd.setBinding(T_Bucket,   buckets.ssbo());
    cmd.setBinding(T_Indirect, view.indirectCmd);
    cmd.setBinding(T_Clusters, clusters.ssbo());
    cmd.setBinding(T_HiZ,      *scene.hiZ);
    cmd.setBinding(8,         clusters.levelIndicesSsbo());
    cmd.setBinding(9,         clusters.levelsSsbo());
    if(ordered)
      cmd.setBinding(T_Order,  orderSsbo);
    if(boughs && viewport==SceneGlobals::V_Main) {
      boughExpanded = false;
      boughHistoryToken = 0;
      }
    const bool boughCull = boughs && boughs->ready() && ((hiddenTypes >> Static) & 1u)==0u &&
                          (viewport==SceneGlobals::V_Main || viewport==SceneGlobals::V_HiZ);
    if(!boughCull) {
      cmd.setPushData(push);
      cmd.setPipeline(*pso);
      cmd.dispatchThreads(ordered ? 1 : push.meshletCount);
      continue;
      }

    struct BoughPush {
      Push     lod;
      uint32_t token;
      } boughPush = {push, 0u};
    pso = &boughs->cull(cmd, viewport==SceneGlobals::V_HiZ, scene.isTreeOcclusionProbe(), useHistory, ordered, boughPush.token);
    cmd.setPushData(&boughPush, offsetof(BoughPush,token)+sizeof(boughPush.token));
    cmd.setPipeline(*pso);
    cmd.dispatchThreads(ordered ? 1 : push.meshletCount);
    if(boughPush.token!=0u) {
      if(useHistory) {
        boughHistoryToken = boughPush.token;
        } else {
        boughs->expand(cmd,boughPush.token,false);
        boughExpanded = true;
        }
      }
    }
  }

void DrawCommands::writeTreeOcclusionDecisions(std::ostream& out) {
  auto& device = Resources::device();
  std::vector<uint32_t> flags(cur->clusterCount);
  device.readBytes(treeOcclusionDecisions, flags.data(), flags.size()*sizeof(uint32_t));
  treeOcclusionDecisions = {};

  // Count real primitives of the rejected meshlets from the same packed indices as main.vert.
  std::unordered_map<const StorageBuffer*, std::vector<uint32_t>> indices;
  for(size_t i=0; i<flags.size(); ++i) {
    const auto& cluster = clusters[i];
    uint32_t triangles = 0;
    if((flags[i] & 3u)==1u) { // legacy passes and precise rejects
      const auto& bucket = buckets[cluster.bucketId];
      const auto& ibo = bucket.staticMesh!=nullptr ? bucket.staticMesh->ibo8Buffer() : bucket.animMesh->ibo8;
      auto [at, firstUse] = indices.try_emplace(&ibo);
      auto& words = at->second;
      if(firstUse) {
        words.resize(ibo.byteSize()/sizeof(uint32_t));
        device.readBytes(ibo, words.data(), ibo.byteSize());
        }
      for(size_t m=0; m<cluster.meshletCount; ++m) {
        const uint32_t bits = words.at((cluster.firstMeshlet+m+1)*PackedMesh::MaxPrim-1);
        triangles += (bits & 0xffu)==((bits>>8u) & 0xffu) ? (bits>>16u) & 0xffu : PackedMesh::MaxPrim;
        }
      }
    const uint32_t row[] = {flags[i], cluster.meshletCount, triangles};
    out.write(reinterpret_cast<const char*>(row), sizeof(row));
    }
  }

void DrawCommands::visibilityStats(Encoder<CommandBuffer>& cmd, uint8_t fId) {
  // the counters of the main view's culling for the frame profile (visibility_stats.comp): what this frame slot wrote
  // last time - its fence has been waited for, the buffer is host-visible - then this frame's. Without the profile
  // nothing runs
  if(!FrameProfile::enabled())
    return;
  if(boughs && boughHistoryToken==0)
    boughs->readStats(cmd, fId, boughExpanded);
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
  push.count = uint32_t(cur->maxPayload);
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
      cmd.dispatchThreads(cur->maxPayload); else
      cmd.dispatch(1);
    }
  statsWritten[fId] = true;
  }

void DrawCommands::prepareOrderedClusters() {
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
  }

void DrawCommands::visibilityVsm(Encoder<CommandBuffer>& cmd, const StorageBuffer* geometryStats) {
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
  static_assert(offsetof(Push,lod)==16 && sizeof(Push)==64);
  push.meshletCount = uint32_t(cur->clusterCount);
  push.candidateCap = cap;
  push.lod          = clusterLodView(cur->hasLevels, scene);

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
  cmd.setPipeline(geometryStats!=nullptr ? shaders.vsmCullMeshletsStats : shaders.vsmCullMeshlets);
  for(cull.emit=0; cull.emit<2; ++cull.emit) {
    cmd.setPushData(cull);
    cmd.dispatch(VsmCullGroups);
    }

  cmd.setBinding(1, view.vsmClusters);
  cmd.setBinding(2, view.visClusters);
  cmd.setBinding(3, view.indirectCmd);
  cmd.setBinding(4, *scene.vsmPageList);
  cmd.setBinding(5, vsmIndirectCmd);
  if(geometryStats!=nullptr)
    cmd.setBinding(6, *geometryStats);
  cmd.setPipeline(geometryStats!=nullptr ? shaders.vsmPackDraw0Stats : shaders.vsmPackDraw0);
  cmd.dispatch(1);

  cmd.setBinding(1, view.vsmClusters);
  cmd.setBinding(2, view.visClusters);
  cmd.setBinding(3, view.indirectCmd);
  cmd.setBinding(4, *scene.vsmPageList);
  cmd.setPushData(&cap, sizeof(cap));
  if(geometryStats!=nullptr) {
    cmd.setBinding(6, *geometryStats);
    cmd.setBinding(7, buckets.primitiveOffsetTable());
    cmd.setBinding(8, buckets.primitiveCountTable());
    }
  cmd.setPipeline(geometryStats!=nullptr ? shaders.vsmPackDraw1Stats : shaders.vsmPackDraw1);
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

  for(size_t i=0; i<cur->ord.size(); ++i) {
    auto& cx = *cur->ord[i];
    if(cx.maxPayload==0)
      continue;
    if(cx.alpha!=Material::Solid && cx.alpha!=Material::AlphaTest)
      continue;
    if(cx.pVsm==nullptr)
      continue;
    if((hiddenTypes>>cx.type) & 1u)
      continue;

    auto id  = size_t(std::distance(cur->cmd.data(), &cx));
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
    push.meshletCount = uint32_t(cur->clusterCount);

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
  const auto hidden = scene.isLandscapeMask() && ownMeshMask ? ~(1u << Static) : hiddenTypes;

  for(size_t i=0; i<cur->ord.size(); ++i) {
    auto& cx = *cur->ord[i];
    if(cx.maxPayload==0)
      continue;
    if(cx.alpha!=Material::Solid && cx.alpha!=Material::AlphaTest)
      continue;
    if(cx.type!=Landscape && cx.type!=Static)
      continue;
    if(cx.pHiZ==nullptr)
      continue;
    if((hidden>>cx.type) & 1u)
      continue;

    auto id  = size_t(std::distance(cur->cmd.data(), &cx));
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

void DrawCommands::drawCommon(Tempest::Encoder<Tempest::CommandBuffer>& cmd, SceneGlobals::VisCamera viewId,
                              Material::AlphaFunc func, HistoryDraw historyDraw) {
  struct Push { uint32_t firstMeshlet; uint32_t meshletCount; } push = {};

  auto b = std::lower_bound(cur->ord.begin(), cur->ord.end(), func, [](const DrawCmd* l, Material::AlphaFunc f){
    return l->alpha < f;
    });
  auto e = std::upper_bound(cur->ord.begin(), cur->ord.end(), func, [](Material::AlphaFunc f, const DrawCmd* r){
    return f < r->alpha;
    });

  const auto hidden = scene.isLandscapeMask() && ownMeshMask ? ~((1u << Static) | (1u << Movable)) : hiddenTypes;
  for(auto i=b; i!=e; ++i) {
    auto& cx = **i;
    if(cx.alpha!=func)
      continue;

    if(cx.maxPayload==0)
      continue;
    if((hidden>>cx.type) & 1u)
      continue;

    const RenderPipeline* pso = nullptr;
    switch(viewId) {
      case SceneGlobals::V_Shadow0:
      case SceneGlobals::V_Shadow1:
      case SceneGlobals::V_Vsm:
        pso = cx.pShadow;
        break;
      case SceneGlobals::V_Main:
        switch(historyDraw) {
          case HistoryDraw::Off:         pso = cx.pMain; break;
          case HistoryDraw::Early:       pso = cx.pHistoryDepth; break;
          case HistoryDraw::EarlyVerify: pso = cx.pHistoryVerify; break;
          case HistoryDraw::Unfiltered:
          case HistoryDraw::Filtered:    pso = cx.pHistoryGBuffer; break;
          }
        break;
      case SceneGlobals::V_HiZ:
      case SceneGlobals::V_Count:
        break;
      }
    if(pso==nullptr)
      continue;

    auto id  = size_t(std::distance(cur->cmd.data(), &cx));
    push.firstMeshlet = cx.firstPayload;
    push.meshletCount = cx.maxPayload;

    const bool compacted = historyDraw==HistoryDraw::Filtered || historyDraw==HistoryDraw::Early ||
                           historyDraw==HistoryDraw::EarlyVerify;
    const auto& view = compacted ? history.view : views[viewId];
    setBindings(cmd, cx, viewId);
    if(compacted)
      cmd.setBinding(L_Payload, view.visClusters);
    cmd.setPushData(push);
    cmd.setPipeline(*pso);
    if(cx.isMeshShader())
      cmd.dispatchMeshIndirect(view.indirectCmd, sizeof(IndirectCmd)*id + sizeof(uint32_t)); else
      cmd.drawIndirect(view.indirectCmd, sizeof(IndirectCmd)*id);
    }
  if(boughExpanded && viewId==SceneGlobals::V_Main && func==Material::AlphaTest &&
     historyDraw!=HistoryDraw::Early && historyDraw!=HistoryDraw::EarlyVerify && ((hidden>>Static) & 1u)==0u)
    boughs->draw(cmd,historyDraw);
  }
