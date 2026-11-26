#include "visualobjects.h"
#include "gameservices.h"
#include "camera.h"

#include <Tempest/Log>
#include <Tempest/Painter>
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cassert>
#include <cstdlib>

#include "graphics/mesh/submesh/animmesh.h"
#include "frameprofile.h"
#include "graphics/meshlevels.h"
#include "graphics/treeboughasset.h"
#include "graphics/treelevelband.h"
#include "graphics/hizoccluders.h"
#include <ostream>
#include "loadprobe.h"
#include "physics/worldwind.h"
#include "shaders.h"
#include "utils/workers.h"
#include <atomic>

using namespace Tempest;

VisualObjects::BenchImpostors VisualObjects::benchImpostors = BenchImpostors::Auto;
VisualObjects::BenchShadowLevels VisualObjects::benchShadowLevels = BenchShadowLevels::Auto;
std::optional<float> VisualObjects::benchShadowNearPx;

static RtScene::Category toRtCategory(DrawCommands::Type t) {
  switch (t) {
    case DrawCommands::Landscape: return RtScene::Landscape;
    case DrawCommands::Static:    return RtScene::Static;
    case DrawCommands::Movable:   return RtScene::Movable;
    case DrawCommands::Animated:  return RtScene::None;
    case DrawCommands::Pfx:       return RtScene::None;
    case DrawCommands::Morph:     return RtScene::None;
    }
  return RtScene::None;
  }


struct VisualObjects::MorphDesc final {
  uint32_t indexOffset = 0;
  uint32_t sample0     = 0;
  uint32_t sample1     = 0;
  uint16_t alpha       = 0;
  uint16_t intensity   = 0;
  };

struct VisualObjects::MorphData {
  MorphDesc morph[Resources::MAX_MORPH_LAYERS];
  };


void VisualObjects::Item::setObjMatrix(const Tempest::Matrix4x4& pos) {
  setObjMatrix(pos,Vec3(pos[3][0],pos[3][1],pos[3][2]));
  }

void VisualObjects::Item::setObjMatrix(const Tempest::Matrix4x4& pos, const Vec3& texturePosition) {
  if(owner!=nullptr) {
    auto& obj = owner->objects[id];
    if(obj.pos!=pos)
      owner->updateRtAs(id);
    obj.pos = pos;
    if(obj.textureCategory==Resources::TextureCategory::Static)
      setTexturePosition(texturePosition);
    else if(obj.textureWorldOwner)
      owner->updateTextureScale(id);
    owner->updateInstance(id);
    }
  }

void VisualObjects::Item::setAsGhost(bool g) {
  if(owner!=nullptr) {
    owner->setAsGhost(id, g);
    }
  }

void VisualObjects::Item::setFatness(float f) {
  if(owner!=nullptr) {
    if(owner->objects[id].fatness == f)
      return;
    owner->objects[id].fatness = f;
    owner->updateInstance(id);
    }
  }

void VisualObjects::Item::setWind(asset::AnimationType m, float intensity) {
  if(owner==nullptr)
    return;

  if(intensity!=0 && m==asset::AnimationType::NONE)
    m = asset::AnimationType::WIND_ALT;

  if(intensity==0)
    m = asset::AnimationType::NONE;

  auto& obj = owner->objects[id];

  const auto prev = obj.wind;
  obj.wind          = m;
  obj.windIntensity = intensity;

  if(prev==m)
    return;

  if(prev!=asset::AnimationType::NONE)
    owner->objectsWind.erase(id);
  if(m!=asset::AnimationType::NONE)
    owner->objectsWind.insert(id);
  }

void VisualObjects::Item::setVisible(bool visible) {
  if(owner==nullptr)
    return;
  auto& obj = owner->objects[id];
  if(obj.visible==visible || obj.isEmpty())
    return;
  obj.visible = visible;
  const bool perMeshlet = obj.type==DrawCommands::Landscape || obj.clusterLod;
  const uint32_t meshletCount = visible ? (perMeshlet ? 1u : owner->drawnMeshlets(obj).second) : 0u;
  for(uint32_t i=0;i<obj.clusterCount;++i)
    owner->clustersMem[obj.clusterId+i].meshletCount = meshletCount;
  owner->clustersMem.markClusters(obj.clusterId,obj.clusterCount);
  owner->updateRtAs(id);
  }

void VisualObjects::Item::startMMAnim(std::string_view anim, float intensity, uint64_t timeUntil, bool freeOnly) {
  if(owner!=nullptr)
    owner->startMMAnim(id, anim, intensity, timeUntil, freeOnly);
  }

void VisualObjects::Item::setDeformation(const std::vector<Tempest::Vec4>& vertices) {
  setDeformation(vertices.data(),vertices.size()/2,0,0,uint32_t(-1));
  }

std::pair<uint32_t,uint32_t> VisualObjects::Item::vertexRange() const {
  if(owner==nullptr)
    return {0,0};
  const auto& obj = owner->objects[id];
  return {obj.iboOff/PackedMesh::MaxInd*PackedMesh::MaxVert, (obj.iboOff+obj.iboLen)/PackedMesh::MaxInd*PackedMesh::MaxVert};
  }

void VisualObjects::Item::setDeformation(const Tempest::Vec4* vertices, size_t pairs, uint32_t first,
                                         uint32_t changedFirst, uint32_t changedEnd) {
  if(owner==nullptr)
    return;
  auto& obj = owner->objects[id];
  const auto count = uint32_t(pairs);
  if(count==0) {
    if(obj.deformCount==0)
      return;
    obj.objDeform = InstanceStorage::Id();
    obj.deformCount = 0;
    obj.deformFirst = 0;
    if(auto mesh=obj.bucketId->staticMesh) {
      owner->clustersMem[obj.clusterId].r = mesh->drawn.rConservative+obj.bucketId->mat.waveMaxAmplitude+
                                            FoliageTree::travel(obj.bucketId->mat.foliage,mesh->bbox.bbox[0],mesh->bbox.bbox[1]);
      obj.treeFade = FoliageTree::fadeCode(FoliageTree::Fade());
      owner->clustersMem.markClusters(obj.clusterId);
      }
    owner->updateInstance(id);
    return;
    }
  const auto size = size_t(count)*2*sizeof(Tempest::Vec4);
  const bool same = obj.deformCount==count && obj.deformFirst==first;
  // the same range as before: only the vertices that changed are written
  size_t from = 0, to = count;
  if(same) {
    from = std::clamp<size_t>(changedFirst,first,size_t(first)+count)-first;
    to   = std::clamp<size_t>(changedEnd,  first,size_t(first)+count)-first;
    if(from>=to)
      return;
    // from a 64-byte block of InstanceStorage (two vertices): an unaligned write goes byte by byte
    from -= from % 2;
    }
  const bool changed = owner->instanceMem.realloc(obj.objDeform,size);
  if(changed)
    from = 0, to = count;
  const size_t pairSize = 2*sizeof(Tempest::Vec4);
  obj.objDeform.set(vertices+from*2,from*pairSize,(to-from)*pairSize);
  // The authored bounds are shared by every instance. Grow this instance's
  // sphere to retain cloth that folds beyond the original flat mesh bounds.
  float radius = 0;
  for(size_t i=from*2;i<to*2;i+=2) {
    const auto& p = vertices[i];
    Vec3 point(p.x,p.y,p.z);
    float zero = 0;
    obj.pos.project(point.x,point.y,point.z,zero);
    radius = std::max(radius,point.length());
    }
  if(owner->clustersMem[obj.clusterId].r<radius) {
    owner->clustersMem[obj.clusterId].r = radius;
    owner->clustersMem.markClusters(obj.clusterId);
    }
  if(changed || obj.deformCount!=count || obj.deformFirst!=first) {
    obj.deformCount = count;
    obj.deformFirst = first;
    owner->updateInstance(id);
    }
  }

void VisualObjects::Item::setMMShape(std::string_view anim, uint32_t frame0, uint32_t frame1, float alpha, float intensity) {
  if(owner!=nullptr)
    owner->setMMShape(id, anim, frame0, frame1, alpha, intensity);
  }

float VisualObjects::Item::morphWeight(std::string_view anim) {
  return owner!=nullptr ? owner->morphWeight(id, anim) : 0.f;
  }

VisualObjects::MorphSample VisualObjects::Item::morphSample(std::string_view name, uint64_t tick) const {
  if(owner==nullptr)
    return {};
  auto& obj = owner->objects[id];
  const auto* mesh = obj.bucketId->staticMesh;
  if(mesh==nullptr || mesh->morph.anim==nullptr)
    return {};
  for(const auto& ani:obj.morphAnim) {
    if(ani.id>=mesh->morph.anim->size() || ani.timeUntil<tick)
      continue;
    const auto& anim = (*mesh->morph.anim)[ani.id];
    if(anim.name!=name || anim.numFrames==0 || ani.intensity<=0.f)
      continue;
    const auto time = tick-ani.timeStart;
    float weight = 1.f;
    if(!ani.manual) {
      if(anim.blendIn>0.f)
        weight = std::min(weight,float(time)*anim.blendIn);
      if(anim.blendOut>0.f && ani.timeUntil!=uint64_t(-1))
        weight = std::min(weight,float(ani.timeUntil-tick)*anim.blendOut);
      }
    const auto quantize = [](float x) { return float(uint16_t(std::clamp(x,0.f,1.f)*uint16_t(-1)))/float(uint16_t(-1)); };
    MorphSample sample;
    sample.frame0 = ani.manual ? ani.frame0 : uint32_t((time/anim.tickPerFrame+0)%anim.numFrames);
    sample.frame1 = ani.manual ? ani.frame1 : uint32_t((time/anim.tickPerFrame+1)%anim.numFrames);
    sample.alpha = quantize(ani.manual ? ani.alpha : float(time%anim.tickPerFrame)/float(anim.tickPerFrame));
    sample.intensity = quantize(ani.intensity*std::clamp(weight,0.f,1.f));
    return sample;
    }
  return {};
  }

const Material& VisualObjects::Item::material() const {
  if(owner==nullptr) {
    static Material m;
    return m;
    }
  auto& bx = *owner->objects[id].bucketId;
  return bx.mat;
  }

const Bounds& VisualObjects::Item::bounds() const {
  static Bounds dummy;
  if(owner==nullptr)
    return dummy;
  auto& bx = *owner->objects[id].bucketId;
  if(bx.staticMesh!=nullptr)
    return bx.staticMesh->bbox;
  if(bx.animMesh!=nullptr)
    return bx.animMesh->bbox;
  return dummy;
  }

Matrix4x4 VisualObjects::Item::position() const {
  if(owner!=nullptr)
    return owner->objects[id].pos;
  return Matrix4x4();
  }

const StaticMesh* VisualObjects::Item::mesh() const {
  if(owner==nullptr)
    return nullptr;
  auto& bx = *owner->objects[id].bucketId;
  return bx.staticMesh;
  }

std::pair<uint32_t, uint32_t> VisualObjects::Item::meshSlice() const {
  if(owner==nullptr)
    return std::pair<uint32_t, uint32_t>(0,0);
  auto& obj = owner->objects[id];
  if(obj.clusterLod) {
    const auto [offset,length] = obj.bucketId->staticMesh->rtSlice(obj.iboOff,obj.iboLen);
    return {uint32_t(offset),uint32_t(length)};
    }
  return std::make_pair(obj.iboOff, obj.iboLen);
  }


VisualObjects::VisualObjects(const SceneGlobals& scene, const std::pair<Vec3, Vec3>& bbox)
    : scene(scene), drawCmd(*this, bucketsMem, clustersMem, scene) {
  objectsMorph.reserve(1024);
  textureCensus.enabled=std::getenv("OPENGOTHIC_TEXTURE_DUMP")!=nullptr;
  }

void VisualObjects::invalidate(const StaticMesh& mesh) {
  assert(std::none_of(objects.begin(),objects.end(),[&](const auto& object) {
    return !object.isEmpty() && object.bucketId->staticMesh==&mesh;
    }));
  bucketsMem.invalidate(mesh);
  }

void VisualObjects::updateInstance(size_t id, Matrix4x4* pos) {
  auto& obj = objects[id];
  if(obj.type==DrawCommands::Landscape)
    return;

  const auto& transform = pos==nullptr ? obj.pos : *pos;
  InstanceDesc d;
  d.setPosition(transform);
  d.animPtr = obj.animPtr;
  // strands have no fatness: the slot holds how much wider a strand is drawn (vertex_process.glsl, BK_STRAND)
  d.fatness = obj.strandMeshlets>0 ? obj.strandWider : obj.fatness*0.5f;
  // The shader reads deformPtr + vboOffset*8 for vboOffset < deformCount: an object deformed from a later packed
  // vertex (its own range) starts that much before its storage (uint arithmetic wraps on both sides).
  d.deformPtr = obj.objDeform.offsetId<uint32_t>() - obj.deformFirst*8u;
  d.deformCount = obj.deformCount==0 ? 0 : obj.deformFirst+obj.deformCount;
  if(obj.bandCluster!=uint32_t(-1) && obj.deformCount==0) {
    // a static tree's level band: the deform slot, then the second instance of the coarser side
    d.deformPtr = obj.bandWord;
    obj.objInstance.set(&d, 0, sizeof(d));
    d.deformPtr = obj.bandWord==0 ? 0 : obj.bandWord | TreeLevelBand::Coarse;
    obj.bandInstance.set(&d, 0, sizeof(d));
    } else {
    obj.objInstance.set(&d, 0, sizeof(d));
    }
  if(obj.type==DrawCommands::Static && obj.bucketId->staticMesh->treeBough && TreeBough::cpuEnabled())
    drawCmd.setBoughTransform(obj.clusterId,transform);

  if(obj.clusterLod) {
    // The same conservative scale as clusterLodScale in cluster_lod.glsl.
    const Vec3 x(transform[0][0],transform[0][1],transform[0][2]);
    const Vec3 y(transform[1][0],transform[1][1],transform[1][2]);
    const Vec3 z(transform[2][0],transform[2][1],transform[2][2]);
    const float xy = std::abs(Vec3::dotProduct(x,y));
    const float yz = std::abs(Vec3::dotProduct(y,z));
    const float zx = std::abs(Vec3::dotProduct(z,x));
    const float scale = std::sqrt(std::max({Vec3::dotProduct(x,x)+xy+zx,
                                           Vec3::dotProduct(y,y)+yz+xy,
                                           Vec3::dotProduct(z,z)+zx+yz}));
    const auto& spheres = obj.bucketId->staticMesh->meshletSpheres;
    const auto first = obj.iboOff/PackedMesh::MaxInd;
    for(uint32_t i=0; i<obj.clusterCount; ++i) {
      const auto& sphere = spheres[first+i];
      Vec4 point(sphere.x,sphere.y,sphere.z,1.f);
      transform.project(point);
      const Vec3 center(point.x,point.y,point.z);
      const float radius = (sphere.w+std::abs(d.fatness))*scale;
      auto& cluster = clustersMem[obj.clusterId+i];
      if(cluster.pos!=center || cluster.r!=radius) {
        cluster.pos = center;
        cluster.r   = radius;
        clustersMem.markClusters(obj.clusterId+i);
        }
      }
    return;
    }

  auto cId  = obj.clusterId;
  auto npos = Vec3(obj.pos[3][0], obj.pos[3][1], obj.pos[3][2]);
  if(clustersMem[cId].pos != npos) {
    clustersMem[cId].pos = npos;
    clustersMem.markClusters(cId);
    }
  if(obj.bandCluster!=uint32_t(-1) && clustersMem[obj.bandCluster].pos!=npos) {
    clustersMem[obj.bandCluster].pos = npos;
    clustersMem.markClusters(obj.bandCluster);
    }
  }

void VisualObjects::updateRtAs(size_t id) {
  auto& obj = objects[id];
  const auto cat = toRtCategory(obj.type);
  if(cat!=RtScene::Landscape && cat!=RtScene::Static)
    return; // never in the scene: NPC, movable vob, pfx, morph
  if(!Shaders::options().doRtScene)
    return;
  std::lock_guard<std::mutex> guard(rtSync);
  if(obj.rtDirty)
    return;
  obj.rtDirty = true;
  rtDirty.push_back(id);
  }

void VisualObjects::updateRtSlot(RtScene& out, Object& obj) {
  // What the frame-time walk of the old scene took: visible objects with a BLAS of a supported category and material.
  // The solid and alpha-tested landscape is in the BLAS of its Landscape (addRtLandscape) instead.
  const AccelerationStructure* blas = nullptr;
  const StaticMesh*            mesh = nullptr;
  const auto                   cat  = toRtCategory(obj.type);
  bool inScene = !obj.isEmpty();
  if(inScene) {
    mesh    = obj.bucketId->staticMesh;
    blas    = mesh!=nullptr ? mesh->blas(obj.iboOff, obj.iboLen) : nullptr;
    inScene = blas!=nullptr && RtScene::isSupported(obj.bucketId->mat, cat) &&
              !RtScene::isMergedLandscape(obj.bucketId->mat, cat);
    }
  if(!inScene) {
    if(obj.rtSlot!=RtScene::NoSlot)
      out.freeSlot(obj.rtSlot);
    obj.rtSlot = RtScene::NoSlot;
    return;
    }
  if(obj.rtSlot==RtScene::NoSlot)
    obj.rtSlot = out.allocSlot();
  // the triangles of the BLAS: of the coarsest level of detail when the mesh has levels (meshlevels.h)
  const auto slice = mesh->rtSlice(obj.iboOff, obj.iboLen);
  out.setSlot(obj.rtSlot, obj.pos, *blas, obj.bucketId->mat, *mesh, slice.first, slice.second, obj.visible);
  }

VisualObjects::Item VisualObjects::get(const StaticMesh& mesh, const Material& mat,
                                       size_t iboOff, size_t iboLen,
                                       bool staticDraw) {
  LoadProbe::Vobs::Scope probe("draw_instance");
  std::lock_guard<std::recursive_mutex> guard(textureSync);
  // return Item();
  // 64x64 meshlets
  assert(iboOff%PackedMesh::MaxInd==0);
  assert(iboLen%PackedMesh::MaxInd==0);

  if(mat.tex==nullptr) {
    Log::e("no texture?!");
    return VisualObjects::Item();
    }
  auto         type = (staticDraw ? DrawCommands::Static : DrawCommands::Movable);
  const size_t id     = implAlloc();

  Object& obj = objects[id];

  if(mesh.morph.anim!=nullptr) {
    type = DrawCommands::Morph;
    }

  obj.type      = type;
  obj.iboOff    = uint32_t(iboOff);
  obj.iboLen    = uint32_t(iboLen);
  obj.bucketId  = bucketsMem.alloc(mat, mesh);
  obj.cmdId     = drawCmd.commandId(mat, obj.type, obj.bucketId.toInt());
  obj.alpha     = mat.alpha;
  obj.timeShift = -scene.tickCount;

  if(obj.isEmpty())
    return Item(); // null command

  obj.objInstance = instanceMem.alloc(sizeof(InstanceDesc));
  const auto instanceId = obj.objInstance.offsetId<InstanceDesc>();
  const auto firstMeshlet = iboOff/PackedMesh::MaxInd;
  const auto meshletCount = iboLen/PackedMesh::MaxInd;
  if(meshletCount>0 && !mesh.clusterLevels.empty()) {
    assert(firstMeshlet+meshletCount<=mesh.clusterLevels.size());
    assert(mesh.clusterLevels.size()==mesh.meshletSpheres.size());
    obj.clusterLod = true;
    obj.clusterCount = uint32_t(meshletCount);
    const auto levels = std::span(mesh.clusterLevels).subspan(firstMeshlet,meshletCount);
    obj.clusterId = clusterId(nullptr, firstMeshlet, meshletCount, obj.bucketId.toInt(), obj.cmdId, levels, instanceId);
    } else {
    obj.clusterId = clusterId(*obj.bucketId, firstMeshlet, meshletCount, obj.bucketId.toInt(), obj.cmdId);
    clustersMem[obj.clusterId].instanceId = instanceId;
    }
  if(type==DrawCommands::Static && FoliageTree::isTree(mat.foliage)) {
    for(uint32_t i=0; i<obj.clusterCount; ++i)
      clustersMem[obj.clusterId+i].historyGeneration |= DrawClusters::HistoryStatic;
    }
  if(MeshLevels::vsmFrameStatsEnabled() &&
     std::ranges::any_of(mesh.sub, [](const auto& sub) { return FoliageTree::isTree(sub.material.foliage); })) {
    for(uint32_t i=0; i<obj.clusterCount; ++i)
      clustersMem[obj.clusterId+i].shadowCommandId |= DrawClusters::ShadowVisual;
    }

  obj.textureCategory=staticDraw ? Resources::TextureCategory::Static : Resources::TextureCategory::Dynamic;
  // The submesh's density, measured when the mesh was loaded; a slice that matches none keeps the whole chain.
  obj.textureMeasured=true;
  for(const auto& sub:mesh.sub)
    if(sub.iboOffset==iboOff && sub.iboLength==iboLen) {
      obj.textureLocal=sub.texel;
      // the shader samples uv*uvScale (materials.json "scale") or by world position ("projection")
      obj.textureLocal.uvPerMetre=mat.textureUvPerMetre(obj.textureLocal.uvPerMetre,mesh.morph.anim!=nullptr);
      break;
      }
  { FrameProfile::Scope timing(FrameProfile::Zone::TextureDemand); updateTextureDemand(id); }
  if(mat.strand) {
    obj.strandMeshlets = uint32_t(iboLen/PackedMesh::MaxInd);
    obj.strandWider    = 1;
    objectsStrand.insert(id);
    }
  if(FoliageTree::isTree(mat.foliage) && FoliageWind::moves(FoliageWind::pack(mat.foliage,FoliageWind::enabled()))) {
    obj.tree     = true;
    obj.treeFade = FoliageTree::fadeCode(FoliageTree::Fade());
    objectsTree.insert(id);
    }
  // levels of detail (meshlevels.h): the cluster draws its submesh's slice of the level the main view picks
  if(!obj.clusterLod && !mesh.levels.empty() && type!=DrawCommands::Morph)
    for(size_t i=0; i<mesh.sub.size() && i<NoLevels; ++i)
      if(mesh.sub[i].iboOffset==iboOff && mesh.sub[i].iboLength==iboLen) {
        obj.levelSub = uint8_t(i);
        objectsLevels.insert(id);
        if(type==DrawCommands::Static && mesh.impostor.px>0.f)
          setImpostorShadow(obj, true);
        break;
        }

  if(type==DrawCommands::Static && mesh.treeBough && !obj.clusterLod) {
    const auto& source = mesh.sub[mesh.treeBough->sourceSub];
    if(source.iboOffset==iboOff && source.iboLength==iboLen)
      drawCmd.addBough(mesh,obj.clusterId,obj.cmdId,obj.impostorShadowCmd,uint32_t(firstMeshlet),uint32_t(meshletCount));
    if(obj.levelSub!=NoLevels)
      allocLevelBand(obj);
    }

  if(type==DrawCommands::Morph) {
    obj.objMorphAnim = instanceMem.alloc(sizeof(MorphDesc)*Resources::MAX_MORPH_LAYERS);
    obj.animPtr      = obj.objMorphAnim.offsetId<MorphDesc>();
    const MorphData d = {};
    obj.objMorphAnim.set(&d, 0, sizeof(d));
    }

  updateInstance(id);
  updateRtAs(id);
  return Item(*this, id);
  }

VisualObjects::Item VisualObjects::get(const AnimMesh& mesh, const Material& mat,
                                       size_t iboOff, size_t iboLen,
                                       const InstanceStorage::Id& anim) {
  LoadProbe::Vobs::Scope probe("draw_instance");
  std::lock_guard<std::recursive_mutex> guard(textureSync);
  // return Item();
  // 64x64 meshlets
  assert(iboOff%PackedMesh::MaxInd==0);
  assert(iboLen%PackedMesh::MaxInd==0);

  if(mat.tex==nullptr) {
    Log::e("no texture?!");
    return VisualObjects::Item();
    }

  const size_t id = implAlloc();

  Object& obj = objects[id];

  obj.type      = DrawCommands::Type::Animated;
  obj.iboOff    = uint32_t(iboOff);
  obj.iboLen    = uint32_t(iboLen);
  obj.bucketId  = bucketsMem.alloc(mat, mesh);
  obj.cmdId     = drawCmd.commandId(mat, obj.type, obj.bucketId.toInt());
  obj.clusterId = clusterId(*obj.bucketId, iboOff/PackedMesh::MaxInd, iboLen/PackedMesh::MaxInd, obj.bucketId.toInt(), obj.cmdId);
  obj.alpha     = mat.alpha;
  obj.timeShift = -scene.tickCount;

  if(obj.isEmpty())
    return Item(); // null command
  // The submesh's density in the bind pose, measured when the mesh was loaded.
  obj.textureMeasured=true;
  for(const auto& sub:mesh.sub)
    if(sub.iboOffset==iboOff && sub.iboSize==iboLen) {
      obj.textureLocal=sub.texel;
      obj.textureLocal.uvPerMetre*=std::abs(mat.uvScale); // a body is not projected by world position
      break;
      }
  { FrameProfile::Scope timing(FrameProfile::Zone::TextureDemand); updateTextureDemand(id); }
  obj.animPtr     = anim.offsetId<Matrix4x4>();
  obj.objInstance = instanceMem.alloc(sizeof(InstanceDesc));
  clustersMem[obj.clusterId].instanceId = obj.objInstance.offsetId<InstanceDesc>();

  updateInstance(id);
  updateRtAs(id);
  return Item(*this, id);
  }

VisualObjects::Item VisualObjects::get(const StaticMesh& mesh, const Material& mat,
                                       size_t iboOff, size_t iboLen, const PackedMesh::Cluster* cluster,
                                       DrawCommands::Type type, const Vec3* texturePosition, const TexelDensity* texel,
                                       std::span<const PackedMesh::ClusterLevel> levels) {
  LoadProbe::Vobs::Scope probe("draw_instance");
  std::lock_guard<std::recursive_mutex> guard(textureSync);
  // return Item();
  // 64x64 meshlets
  assert(iboOff%PackedMesh::MaxInd==0);
  assert(iboLen%PackedMesh::MaxInd==0);

  const size_t id = implAlloc();

  Object& obj = objects[id];

  obj.type      = DrawCommands::Type::Landscape;
  obj.iboOff    = uint32_t(iboOff);
  obj.iboLen    = uint32_t(iboLen);
  obj.bucketId  = bucketsMem.alloc(mat, mesh);
  obj.cmdId     = drawCmd.commandId(mat, type, obj.bucketId.toInt());
  obj.clusterCount = uint32_t(iboLen/PackedMesh::MaxInd);
  obj.clusterId = clusterId(cluster, mesh.firstMeshlet()+iboOff/PackedMesh::MaxInd, obj.clusterCount,
                           obj.bucketId.toInt(), obj.cmdId, levels);
  obj.alpha     = mat.alpha;
  obj.timeShift = -scene.tickCount;

  if(obj.isEmpty())
    return Item(); // null command
  obj.textureCategory=Resources::TextureCategory::Landscape;
  obj.textureChunkedLandscape=texturePosition!=nullptr;
  if(texturePosition)
    obj.texturePosition=*texturePosition;
  if(texturePosition && texel) {
    obj.textureMeasured=true;
    obj.texturePlaced=true;
    obj.textureLocal=*texel;
    obj.textureLocal.uvPerMetre=mat.textureUvPerMetre(obj.textureLocal.uvPerMetre,mesh.morph.anim!=nullptr);
    obj.textureTexel=obj.textureLocal;
    }
  { FrameProfile::Scope timing(FrameProfile::Zone::TextureDemand); updateTextureDemand(id); }
  updateRtAs(id);
  return Item(*this, id);
  }

size_t VisualObjects::implAlloc() {
  if(!objectsFree.empty()) {
    auto id = *objectsFree.begin();
    objectsFree.erase(objectsFree.begin());
    return id;
    }

  objects.resize(objects.size()+1);
  return objects.size()-1;
  }

void VisualObjects::free(size_t id) {
  FrameProfile::Scope timing(FrameProfile::Zone::TextureDemand);
  std::lock_guard<std::recursive_mutex> guard(textureSync);
  Object& obj = objects[id];
  setTextureDemand(obj,false);
  unscheduleTexture(obj);
  if(obj.textureChunkRegistered) {
    if(auto found=textureChunks.find(obj.textureChunk);found!=textureChunks.end()) {
      found->second.ids.erase(id);
      if(found->second.ids.empty())
        textureChunks.erase(found);
      }
    }
  timing.finish();

  const uint32_t meshletCount = (obj.iboLen/PackedMesh::MaxInd);

  drawCmd.addClusters(obj.cmdId, -meshletCount);
  if(obj.impostorShadowCmd!=uint16_t(-1))
    setImpostorShadow(obj, false);
  drawCmd.removeBough(obj.clusterId);
  freeLevelBand(obj);
  clustersMem.free(obj.clusterId, obj.clusterCount);
  if(Shaders::options().doRtScene) {
    // the slot goes at the next frame; a reused id is marked again by its new object
    std::lock_guard<std::mutex> guard(rtSync);
    if(obj.rtSlot!=RtScene::NoSlot)
      rtFreed.push_back(obj.rtSlot);
    }

  if(obj.wind!=asset::AnimationType::NONE)
    objectsWind.erase(id);
  if(obj.type==DrawCommands::Morph)
    objectsMorph.erase(id);
  if(obj.strandMeshlets>0)
    objectsStrand.erase(id);
  if(obj.tree)
    objectsTree.erase(id);
  if(obj.levelSub!=NoLevels)
    objectsLevels.erase(id);

  obj = Object();
  while(objects.size()>0) {
    if(!objects.back().isEmpty())
      break;
    objects.pop_back();
    objectsFree.erase(objects.size());
    }
  if(id<objects.size())
    objectsFree.insert(id);
  }

InstanceStorage::Id VisualObjects::alloc(size_t size) {
  return instanceMem.alloc(size);
  }

bool VisualObjects::realloc(InstanceStorage::Id& id, size_t size) {
  return instanceMem.realloc(id, size);
  }

const Tempest::StorageBuffer& VisualObjects::instanceSsbo() const {
  return instanceMem.ssbo();
  }

const Tempest::StorageBuffer& VisualObjects::instanceSsboPrev() const {
  return instanceMem.ssboPrev();
  }

void VisualObjects::setInstanceHistory(bool enabled) {
  instanceMem.setHistory(enabled);
  }

uint32_t VisualObjects::clusterId(const PackedMesh::Cluster* cx, size_t firstMeshlet, size_t meshletCount, uint16_t bucketId,
                                  uint16_t commandId, std::span<const PackedMesh::ClusterLevel> levels, uint32_t instanceId) {
  if(commandId==uint16_t(-1))
    return uint32_t(-1);

  const auto ret = clustersMem.alloc(cx, firstMeshlet, meshletCount, bucketId, commandId, levels, instanceId);
  drawCmd.addClusters(commandId, uint32_t(meshletCount));
  return uint32_t(ret);
  }

uint32_t VisualObjects::clusterId(const DrawBuckets::Bucket& bucket, size_t firstMeshlet, size_t meshletCount, uint16_t bucketId, uint16_t commandId) {
  if(commandId==uint16_t(-1))
    return uint32_t(-1);

  const auto ret = clustersMem.alloc(bucket, firstMeshlet, meshletCount, bucketId, commandId);
  drawCmd.addClusters(commandId, uint32_t(meshletCount));
  return uint32_t(ret);
  }

void VisualObjects::startMMAnim(size_t i, std::string_view animName, float intensity, uint64_t timeUntil, bool freeOnly) {
  auto& obj        = objects[i];
  auto  staticMesh = obj.bucketId->staticMesh;
  if(staticMesh==nullptr || staticMesh->morph.anim==nullptr)
    return;

  auto&  anim = *staticMesh->morph.anim;
  size_t id   = size_t(-1);
  for(size_t i=0; i<anim.size(); ++i)
    if(anim[i].name==animName) {
      id = i;
      break;
      }

  if(id==size_t(-1))
    return;

  objectsMorph.insert(i);

  const uint64_t now  = scene.tickCount;
  auto&          m    = anim[id];
  const bool     stop = timeUntil<now;  // the anim is stopped (a time of 0)
  if(timeUntil==uint64_t(-1) && m.duration>0)
    timeUntil = now + m.duration;
  // held for its time, then fades out
  if(!stop && timeUntil!=uint64_t(-1) && m.blendOut>0)
    timeUntil += uint64_t(1.f/m.blendOut);

  // fades out from where it is now
  auto fadeOut = [&](MorphAnim& a) {
    const auto& am = anim[a.id];
    if(a.manual || am.blendOut<=0) {
      a.timeUntil = 0;
      return;
      }
    a.timeUntil = std::min(a.timeUntil, now + uint64_t(morphWeight(obj,a)/am.blendOut));
    };

  // another anim of the layer gives way: S_ANGRY replaces S_FRIENDLY
  if(!stop) {
    for(auto& a:obj.morphAnim) {
      if(a.id==id || a.timeUntil<now || a.manual || anim[a.id].layer!=m.layer)
        continue;
      fadeOut(a);
      }
    }

  // the anim is on already (or still fading out): it goes on from its weight
  for(auto& a:obj.morphAnim) {
    if(a.id!=id || a.timeUntil<now)
      continue;
    if(stop) {
      fadeOut(a);
      return;
      }
    if(m.blendIn>0) {
      const float w = a.manual ? 1.f : morphWeight(obj,a);
      a.timeStart = now - std::min(now, uint64_t(std::ceil(w/m.blendIn)));
      }
    a.timeUntil = timeUntil;
    a.intensity = intensity;
    a.manual    = false;
    return;
    }
  if(stop)
    return;
  if(freeOnly && std::none_of(std::begin(obj.morphAnim),std::end(obj.morphAnim),[&](const MorphAnim& a){ return a.timeUntil<now; }))
    return;

  auto& ani = morphSlot(obj);
  ani.id        = id;
  ani.timeStart = now;
  ani.timeUntil = timeUntil;
  ani.intensity = intensity;
  ani.manual    = false;
  }

float VisualObjects::morphWeight(Object& obj, const MorphAnim& a) {
  const uint64_t now = scene.tickCount;
  if(a.timeUntil<now)
    return 0;
  if(a.manual)
    return 1;
  const auto& m = (*obj.bucketId->staticMesh->morph.anim)[a.id];
  float w = 1;
  if(m.blendIn>0)
    w = std::min(w, float(now-a.timeStart)*m.blendIn);
  if(m.blendOut>0 && a.timeUntil!=uint64_t(-1))
    w = std::min(w, float(a.timeUntil-now)*m.blendOut);
  return std::clamp(w, 0.f, 1.f);
  }

float VisualObjects::morphWeight(size_t i, std::string_view animName) {
  auto& obj = objects[i];
  if(obj.isEmpty() || obj.bucketId->staticMesh==nullptr || obj.bucketId->staticMesh->morph.anim==nullptr)
    return 0;
  auto& anim = *obj.bucketId->staticMesh->morph.anim;
  float w    = 0;
  for(auto& a:obj.morphAnim)
    if(a.id<anim.size() && anim[a.id].name==animName)
      w = std::max(w, a.intensity*morphWeight(obj,a));
  return w;
  }

auto VisualObjects::morphSlot(Object& obj) -> MorphAnim& {
  // a free slot, else the faintest anim that is not set by its owner
  MorphAnim* slot = nullptr;
  float      best = 2;
  for(auto& a:obj.morphAnim) {
    if(a.timeUntil<scene.tickCount)
      return a;
    if(a.manual)
      continue;
    const float w = morphWeight(obj,a)*a.intensity;
    if(w<best) {
      best = w;
      slot = &a;
      }
    }
  return slot!=nullptr ? *slot : obj.morphAnim[0];
  }

void VisualObjects::setMMShape(size_t i, std::string_view animName, uint32_t frame0, uint32_t frame1, float alpha, float intensity) {
  auto& obj        = objects[i];
  auto  staticMesh = obj.bucketId->staticMesh;
  if(staticMesh==nullptr || staticMesh->morph.anim==nullptr)
    return;

  auto&  anim = *staticMesh->morph.anim;
  size_t id   = size_t(-1);
  for(size_t r=0; r<anim.size(); ++r)
    if(anim[r].name==animName) {
      id = r;
      break;
      }
  if(id==size_t(-1) || anim[id].numFrames==0)
    return;

  // the layer of the anim, or a free slot, or the faintest one
  auto&      m    = anim[id];
  MorphAnim* slot = nullptr;
  for(auto& a:obj.morphAnim)
    if(a.timeUntil>=scene.tickCount && anim[a.id].layer==m.layer) {
      slot = &a;
      break;
      }
  if(slot==nullptr)
    slot = &morphSlot(obj);

  const uint32_t last = uint32_t(m.numFrames-1);
  if(slot->id!=id || slot->timeUntil<scene.tickCount)
    slot->timeStart = scene.tickCount;
  slot->id        = id;
  slot->timeUntil = scene.tickCount + 250;  // the owner sets the shape every frame; if it stops, the shape goes
  slot->intensity = std::clamp(intensity, 0.f, 1.f);
  slot->manual    = true;
  slot->frame0    = std::min(frame0, last);
  slot->frame1    = std::min(frame1, last);
  slot->alpha     = std::clamp(alpha, 0.f, 1.f);
  objectsMorph.insert(i);
  }

void VisualObjects::setAsGhost(size_t id, bool g) {
  auto& obj= objects[id];
  if(obj.isGhost==g)
    return;

  auto& bx  = *obj.bucketId;
  auto& cx  = drawCmd[obj.cmdId];
  auto  mat = bx.mat;

  const uint32_t meshletCount = (obj.iboLen/PackedMesh::MaxInd);
  drawCmd.addClusters(obj.cmdId, -meshletCount);
  if(obj.impostorShadowCmd!=uint16_t(-1))
    setImpostorShadow(obj, false);

  mat.alpha   = g ? Material::Ghost : obj.alpha;
  obj.isGhost = g;

  if(bx.staticMesh!=nullptr)
    obj.bucketId = bucketsMem.alloc(mat, *bx.staticMesh); else
    obj.bucketId = bucketsMem.alloc(mat, *bx.animMesh);

  obj.cmdId = drawCmd.commandId(mat, cx.type, obj.bucketId.toInt());
  drawCmd.addClusters(obj.cmdId, +meshletCount);
  if(!g && obj.type==DrawCommands::Static && obj.levelSub!=NoLevels && obj.bucketId->staticMesh->impostor.px>0.f)
    setImpostorShadow(obj, true);

  for(size_t i=0; i<obj.clusterCount; ++i) {
    clustersMem[obj.clusterId + i].commandId = obj.cmdId;
    clustersMem[obj.clusterId + i].bucketId  = obj.bucketId.toInt();
    }
  clustersMem.markClusters(obj.clusterId,obj.clusterCount);
  updateRtAs(id); // a ghost leaves the ray scene
  }

void VisualObjects::prepareGlobals(Encoder<CommandBuffer>& enc, uint8_t fId) {
  instanceMem.commit(enc, fId);
  clustersMem.commit(enc, fId);
  drawCmd.commit(enc);
  bucketsMem.commit(enc, fId);
  }

void VisualObjects::preFrameUpdate(uint8_t fId) {
  FrameProfile::Scope all(FrameProfile::Zone::VisualsPrepare);
  { FrameProfile::Scope z(FrameProfile::Zone::WindPrepare);    preFrameUpdateWind();    }
  { FrameProfile::Scope z(FrameProfile::Zone::MorphPrepare);   preFrameUpdateMorph();   }
  { FrameProfile::Scope z(FrameProfile::Zone::StrandsPrepare); preFrameUpdateStrands(); }
  { FrameProfile::Scope z(FrameProfile::Zone::TreesPrepare);   preFrameUpdateTrees();   }
  { FrameProfile::Scope z(FrameProfile::Zone::LevelsPrepare);  preFrameUpdateLevels();  }
  prepareImpostors(fId);
  }

// The wind of a tree fades with the distance (FoliageTree::fade; tree-wind.md, section 4), and a tree with less
// wind needs less room for it. The sphere of a cluster is the instance's own, so it is set by the fade the tree
// has in this frame: the frustum test of every view and the pages of the virtual shadow map go by it. The camera
// and the scale are those the shaders of this frame get (SceneGlobals::commitUbo is ahead of this), the fade is
// taken with a margin and in steps (fadeBound), so the sphere is never less than the motion. The box of the
// bucket is one for all the instances of a mesh and keeps the whole of the travel
void VisualObjects::preFrameUpdateTrees() {
  if(objectsTree.empty())
    return;

  // The quantized fade depends on the full mesh and placement, which a tree's submeshes share.
  // The cache lives for this view update. A collision only costs a fresh evaluation.
  struct TreeFade {
    const StaticMesh* mesh = nullptr;
    Vec3              at, up;
    FoliageTree::Fade fade;
    float             reach = 0.f;
    uint16_t          code = 0;
    };
  std::array<TreeFade,64> fades;
  const Vec3  camera = scene.camPos();
  const float px     = scene.foliageFade();
  const auto  wind   = FoliageWind::enabled();
  constexpr uint32_t treeAnchor = uint32_t(FoliageWind::AnchorTree) << 24;
  for(auto id:objectsTree) {
    auto&       obj  = objects[id];
    const auto* mesh = obj.bucketId->staticMesh;
    if(mesh==nullptr || obj.deformCount>0)
      continue; // a deformed mesh sets its sphere by its points
    const auto& mat = obj.bucketId->mat;
    assert(FoliageTree::isTree(mat.foliage));
    const Vec3 at(obj.pos.at(3,0), obj.pos.at(3,1), obj.pos.at(3,2));
    const Vec3 up(obj.pos.at(1,0), obj.pos.at(1,1), obj.pos.at(1,2));
    const auto x = std::bit_cast<uint32_t>(at.x);
    const auto z = std::bit_cast<uint32_t>(at.z);
    const auto hash = (x ^ (x >> 17)) ^ std::rotl(z ^ (z >> 11), 11);
    auto& entry = fades[hash % fades.size()];
    if(entry.mesh!=mesh || entry.at!=at || entry.up!=up) {
      const float reach = FoliageTree::reach(mat.foliage, mesh->bbox.bbox[0], mesh->bbox.bbox[1]);
      const auto  fade  = FoliageTree::fadeBound(treeAnchor, reach, up.length(), (at-camera).length(), px);
      entry = {mesh, at, up, fade, reach, FoliageTree::fadeCode(fade)};
      }
    if(entry.code==obj.treeFade)
      continue;
    // Keep the material's own bend, flutter and radius growth for its cluster.
    const auto bits = FoliageWind::pack(mat.foliage, wind);
    obj.treeFade = entry.code;
    clustersMem[obj.clusterId].r = mesh->drawn.rConservative + mat.waveMaxAmplitude +
                                   FoliageTree::travel(bits, entry.reach, entry.fade);
    clustersMem.markClusters(obj.clusterId);
    }
  }

void VisualObjects::writeTreeOcclusionProbe(std::ostream& out, std::ostream& decisions) {
  drawCmd.writeTreeOcclusionDecisions(decisions);
  for(auto id : objectsLevels) {
    auto& obj = objects[id];
    const auto& bucket = *obj.bucketId;
    const auto& mat = bucket.mat;
    if(!FoliageTree::isTree(mat.foliage))
      continue;
    const auto& mesh = *bucket.staticMesh;
    const auto& cluster = clustersMem[obj.clusterId];
    const bool canopy = HiZOccluders::canopy(mat.foliage, mat.alpha==Material::AlphaTest);
    const float grow = mat.hasMaterialMaps() ? FoliageTree::travel(mat.foliage, mesh.bbox.bbox[0], mesh.bbox.bbox[1]) : 0.f;
    const float currentGrow = cluster.r - mesh.drawn.rConservative - mat.waveMaxAmplitude;
    out << cluster.instanceId << ' ' << unsigned(obj.level.stage) << ' ' << cluster.meshletCount << ' ' << canopy
        << ' ' << cluster.pos.x << ' ' << cluster.pos.y << ' ' << cluster.pos.z << ' ' << cluster.r;
    for(size_t i=0; i<16; ++i)
      out << ' ' << obj.pos.data()[i];
    for(const auto& v : mesh.drawn.bbox)
      out << ' ' << v.x << ' ' << v.y << ' ' << v.z;
    auto lo = mesh.drawn.bbox[0] - Vec3(grow);
    auto hi = mesh.drawn.bbox[1] + Vec3(grow);
    if(const auto* box = mesh.treeHiZBox(cluster.firstMeshlet, cluster.meshletCount)) {
      const auto bits = mat.hasMaterialMaps() ? FoliageWind::pack(mat.foliage, FoliageWind::enabled()) : 0u;
      const float reach = FoliageTree::reach(mat.foliage, mesh.bbox.bbox[0], mesh.bbox.bbox[1]);
      const Vec3 up(obj.pos.at(1,0), obj.pos.at(1,1), obj.pos.at(1,2));
      const Vec3 root(obj.pos.at(3,0), obj.pos.at(3,1), obj.pos.at(3,2));
      const auto fade = FoliageTree::fade(bits, reach, up.length(), (root-scene.camPos()).length(), scene.foliageFade());
      const float margin = FoliageTree::travel(bits, reach, fade);
      lo = Vec3(std::max(lo.x, box->min.x-margin), std::max(lo.y, box->min.y-margin), std::max(lo.z, box->min.z-margin));
      hi = Vec3(std::min(hi.x, box->max.x+margin), std::min(hi.y, box->max.y+margin), std::min(hi.z, box->max.z+margin));
      }
    out << ' ' << grow << ' ' << currentGrow << ' ' << obj.clusterId
        << ' ' << lo.x << ' ' << lo.y << ' ' << lo.z << ' ' << hi.x << ' ' << hi.y << ' ' << hi.z << '\n';
    }
  }

std::pair<uint32_t,uint32_t> VisualObjects::drawnMeshlets(Object& obj) {
  const uint32_t first = obj.iboOff/PackedMesh::MaxInd;
  if(obj.strandMeshlets>0)
    return {first, obj.strandMeshlets};
  if(const auto* mesh = obj.bucketId->staticMesh)
    return mesh->levelMeshlets(obj.iboOff, obj.iboLen, obj.level.glb);
  return {first, obj.iboLen/PackedMesh::MaxInd};
  }

void VisualObjects::setImpostorShadow(Object& obj, bool enable) {
  const auto& mesh = *obj.bucketId->staticMesh;
  const auto count = mesh.levelMeshlets(obj.iboOff, obj.iboLen, mesh.levels.size()).second;
  if(enable) {
    obj.impostorShadowCmd = drawCmd.commandId(obj.bucketId->mat, obj.type, obj.bucketId.toInt(), true);
    if(obj.impostorShadowCmd!=uint16_t(-1))
      drawCmd.addClusters(obj.impostorShadowCmd, count);
    } else {
    drawCmd.addClusters(obj.impostorShadowCmd, -count);
    obj.impostorShadowCmd = uint16_t(-1);
    }
  }

// Levels of detail of own meshes (meshlevels.h): once a frame each object of a mesh with levels is measured on the
// main view — the height of its mesh's box times its scale, at the distance of the box's middle, by the projection
// the shaders of this frame get. An impostor keeps the last 3D level as its fallback caster. Trees can send a
// different 3D slice to both types of shadow views. A deformed object keeps the mesh itself.
void VisualObjects::preFrameUpdateLevels() {
  uint64_t count[1+MeshLevels::MaxLevels] = {};
  impostorDraws.clear();
  const float py   = scene.proj.at(1,1);
  const int   resY = scene.screenRes().y;
  const bool  shadowLevels = benchShadowLevels==BenchShadowLevels::On ||
                            (benchShadowLevels==BenchShadowLevels::Auto && MeshLevels::shadowLevelsEnabled());
  const bool  fullShadows = benchShadowLevels==BenchShadowLevels::Full;
  const float nearPx = benchShadowNearPx.value_or(MeshLevels::shadowNearPx());
  const bool vsmStats = MeshLevels::vsmFrameStatsEnabled();
  const auto probePx = vsmStats ? MeshLevels::shadowProbePx() : std::nullopt;
  if(probePx)
    shadowProbe = {};
  const bool drawStatic = (DrawCommands::hiddenTypes & (1u << DrawCommands::Static))==0;
  const bool materialMaps = Shaders::options().doMaterialMaps;
  for(auto id:objectsLevels) {
    auto&       obj  = objects[id];
    const auto* mesh = obj.bucketId->staticMesh;
    if(mesh==nullptr)
      continue;
    const bool staticInstance = obj.type==DrawCommands::Static || obj.type==DrawCommands::Movable;
    const bool canImpostor = obj.impostorShadowCmd!=uint16_t(-1) && benchImpostors!=BenchImpostors::Off;
    MeshLevels::Selection level;
    uint8_t shadowLevel = 0;
    bool shadowCoarse = false;
    uint32_t sunFlags = 0;
    bool probeChanged = false;
    uint32_t joint = 0;
    if(obj.deformCount==0 && py!=0.f && resY>0) {
      const auto& box    = mesh->bbox.bbox;
      Vec3        middle = (box[0]+box[1])*0.5f;
      obj.pos.project(middle);
      const float scale  = Vec3(obj.pos.at(1,0), obj.pos.at(1,1), obj.pos.at(1,2)).length();
      const float distance = (middle-scene.camPos()).length();
      const float worldHeight = (box[1].y-box[0].y)*scale;
      const float height = MeshLevels::screenHeight(worldHeight, distance, py, resY);
      const bool forceImpostor = canImpostor && benchImpostors==BenchImpostors::Force && obj.visible;
      if(canImpostor && mesh->treeBough && benchImpostors==BenchImpostors::Auto) {
        const float weight = TreeBough::impostorJointWeight(mesh->impostor.px,height);
        joint = TreeBough::impostorJointFade(weight);
        level = mesh->pickLevel(obj.level.stage, weight==1.f ? 0.f : height, weight==1.f);
        } else {
        // The forced bench view selects below every threshold.
        level = mesh->pickLevel(obj.level.stage, forceImpostor ? 0.f : height, canImpostor);
        }
      shadowLevel = level.glb;
      if(!obj.isGhost && FoliageTree::isTree(obj.bucketId->mat.foliage)) {
        const float shadowHeight = MeshLevels::screenHeight(worldHeight, distance, py, 1080);
        const bool boughCrown = obj.type==DrawCommands::Static && mesh->treeBough && obj.levelSub==mesh->treeBough->sourceSub;
        const uint8_t shadowNear = boughCrown && shadowLevels ? 0 : level.glb;
        const uint8_t previousNear = boughCrown && shadowLevels ? 0 : obj.level.glb;
        const bool wasSeparate = obj.shadowLevel!=previousNear;
        shadowLevel = fullShadows ? 0 : MeshLevels::pickShadow(mesh->levels.size(), obj.shadowLevel, wasSeparate,
                                                             shadowHeight, shadowNear, shadowLevels, nearPx);
        if(vsmStats)
          shadowCoarse = shadowLevels && !MeshLevels::shadowUsesVisual(shadowHeight, wasSeparate, nearPx);
        if(probePx && obj.bandCluster==uint32_t(-1)) {
          const bool probeWasSeparate = obj.shadowProbeLevel!=previousNear;
          const uint8_t probeLevel = fullShadows ? 0 : MeshLevels::pickShadow(mesh->levels.size(), obj.shadowProbeLevel,
                                                                            probeWasSeparate, shadowHeight, shadowNear,
                                                                            shadowLevels, *probePx);
          obj.shadowProbeLevel = probeLevel;
          probeChanged = probeLevel!=shadowLevel;
          if(obj.visible && drawStatic) {
            const auto [first, meshlets] = mesh->levelMeshlets(obj.iboOff, obj.iboLen, probeLevel);
            const auto triangles = mesh->shadowProbeTriangles(first, meshlets);
            if(shadowLevels && !MeshLevels::shadowUsesVisual(shadowHeight, probeWasSeparate, *probePx)) {
              shadowProbe.coarseTrees += obj.levelSub==0;
              shadowProbe.coarseMeshlets += meshlets;
              shadowProbe.coarseTriangles += triangles;
              }
            if(probeChanged) {
              const auto [oldFirst, oldMeshlets] = mesh->levelMeshlets(obj.iboOff, obj.iboLen, shadowLevel);
              shadowProbe.changedTrees += obj.levelSub==0;
              shadowProbe.oldMeshlets += oldMeshlets;
              shadowProbe.oldTriangles += mesh->shadowProbeTriangles(oldFirst, oldMeshlets);
              shadowProbe.newMeshlets += meshlets;
              shadowProbe.newTriangles += triangles;
              }
            }
          }
        }
      if(obj.bandCluster!=uint32_t(-1))
        sunFlags = updateLevelBand(obj, height, height*1080.f/float(resY), shadowLevels, fullShadows, level, shadowLevel, shadowCoarse);
      if((level.impostor() || joint!=0u) && obj.levelSub==0 && obj.visible && drawStatic) {
        const Vec3 x(obj.pos.at(0,0), obj.pos.at(0,1), obj.pos.at(0,2));
        const Vec3 y(obj.pos.at(1,0), obj.pos.at(1,1), obj.pos.at(1,2));
        const Vec3 z(obj.pos.at(2,0), obj.pos.at(2,1), obj.pos.at(2,2));
        const float scaleBound = std::sqrt(Vec3::dotProduct(x,x) + Vec3::dotProduct(y,y) + Vec3::dotProduct(z,z));
        const float radius = (box[1]-box[0]).length()*0.5f*scaleBound;
        if(scene.frustrum[SceneGlobals::V_Main].testPoint(middle,radius))
          impostorDraws.push_back(id);
        }
      if(staticInstance && materialMaps && !level.impostor() &&
         HiZOccluders::canopy(obj.bucketId->mat.foliage, obj.alpha==Material::AlphaTest)) {
        const bool cut = obj.type==DrawCommands::Static && mesh->treeBough && obj.levelSub==mesh->treeBough->sourceSub;
        const float cutL2Px = cut && mesh->levels.size()>1 ? mesh->levels[1].px : 0.f;
        if((cut || obj.bucketId->mat.normal!=nullptr) && MeshLevels::canopyUsesVertexNormal(level.glb, height, cutL2Px))
          joint |= MeshLevels::CanopyVertexNormal;
        }
      }
    if(staticInstance && obj.animPtr!=joint) {
      // The otherwise idle animation slot holds the impostor joint and the crown's normal-map choice.
      obj.animPtr = joint;
      obj.objInstance.set(&joint, offsetof(InstanceDesc,animPtr), sizeof(joint));
      if(!obj.bandInstance.isEmpty())
        obj.bandInstance.set(&joint, offsetof(InstanceDesc,animPtr), sizeof(joint));
      }
    if(obj.levelSub==0 && !level.impostor())
      ++count[level.glb];
    auto& c = clustersMem[obj.clusterId];
    const auto command = level.impostor() ? obj.impostorShadowCmd : obj.cmdId;
    const bool separateShadow = shadowLevel!=level.glb;
    uint32_t shadowCommand = separateShadow ? obj.cmdId : (c.shadowCommandId & 0xFFFFu);
    shadowCommand |= sunFlags;
    if(vsmStats && (c.shadowCommandId & (DrawClusters::ShadowVisual | DrawClusters::ShadowCoarse))!=0) {
      shadowCommand |= shadowCoarse ? DrawClusters::ShadowCoarse : DrawClusters::ShadowVisual;
      if(probeChanged)
        shadowCommand |= DrawClusters::ShadowProbeChanged;
      }
    if(level==obj.level && c.commandId==command && shadowLevel==obj.shadowLevel &&
       c.shadowCommandId==shadowCommand)
      continue;
    obj.level = level;
    obj.shadowLevel = shadowLevel;
    const auto range = drawnMeshlets(obj);
    if(c.commandId!=command || c.firstMeshlet!=range.first || c.meshletCount!=(obj.visible ? range.second : 0u)) {
      constexpr auto typeMask = DrawClusters::HistoryStatic;
      c.historyGeneration = (c.historyGeneration & typeMask) | ((c.historyGeneration+1) & ~typeMask);
      }
    c.commandId    = command;
    c.firstMeshlet = range.first;
    c.meshletCount = obj.visible ? range.second : 0u;
    c.shadowCommandId = shadowCommand;
    if(separateShadow) {
      const auto shadow = mesh->levelMeshlets(obj.iboOff, obj.iboLen, shadowLevel);
      c.shadowFirstMeshlet = shadow.first;
      c.shadowMeshletCount = shadow.second;
      // The regular command has room for the full GLB, even when the impostor command only reserves the last one.
      } else {
      c.shadowMeshletCount = 0;
      }
    clustersMem.markClusters(obj.clusterId);
    }
  // the objects of the first submesh of their meshes: one an object with levels, a spruce of bark and boughs once
  FrameProfile::add(FrameProfile::Amount::MeshLevel0, count[0]);
  FrameProfile::add(FrameProfile::Amount::MeshLevel1, count[1]);
  FrameProfile::add(FrameProfile::Amount::MeshLevel2, count[2]);
  FrameProfile::add(FrameProfile::Amount::MeshLevel3, count[3]);
  FrameProfile::add(FrameProfile::Amount::MeshLevel4, count[4]);
  FrameProfile::add(FrameProfile::Amount::MeshImpostors, impostorDraws.size());
  }

void VisualObjects::logVsmShadowProbe(uint64_t frameNo) const {
  if(const auto px = MeshLevels::shadowProbePx()) {
    const auto& p = shadowProbe;
    Log::i("vsm shadow probe ", frameNo, " px=", *px, " coarse=", p.coarseTrees, "/", p.coarseMeshlets, "/", p.coarseTriangles,
           " changed=", p.changedTrees, " old=", p.oldMeshlets, "/", p.oldTriangles, " new=", p.newMeshlets, "/", p.newTriangles);
    }
  }

// Legacy wind (the original's WIND/WIND_ALT vegetation): the matrix is a pure function of the world clock and the
// placement, so an object outside every view keeps its last matrix until a view reaches it again — there is no debt
// to catch up. The test is the animation LOD's conservative one (world/animationlod.h): the main frustum, both
// shadow cascades and a camera radius, the sphere grown by the wave amplitude and a margin. A cleared shadow
// frustum rejects everything. OPENGOTHIC_WIND_LOD=0 computes every object every frame, as before.
// OPENGOTHIC_WIND_PARALLEL=0 keeps the loop on the caller; by default the objects are dealt to the worker pool:
// the matrix is pure, and the writes of updateInstance are disjoint per object (instance slots, cluster spheres)
// or atomic (the dirty bits, the bough transforms), so the result does not depend on the order; the join orders
// the writes before the storage commit.
void VisualObjects::preFrameUpdateWind() {
  if(objectsWind.empty())
    return;
  static const bool lod = []{
    const char* e = std::getenv("OPENGOTHIC_WIND_LOD");
    return e==nullptr || e[0]!='0';
    }();
  static const bool par = []{
    const char* e = std::getenv("OPENGOTHIC_WIND_PARALLEL");
    return e==nullptr || e[0]!='0';
    }();
  constexpr float Margin       = 100.f;   // cm, on top of the wave amplitude
  constexpr float CameraRadius = 3000.f;  // cm, the animation LOD's shadow reach
  const auto& main = scene.frustrum[SceneGlobals::V_Main];
  const auto& sh0  = scene.frustrum[SceneGlobals::V_Shadow0];
  const auto& sh1  = scene.frustrum[SceneGlobals::V_Shadow1];
  const Vec3  cam  = scene.camPos();
  std::atomic<uint64_t> computed{0}, skipped{0};
  auto work = [&](size_t id) {
    auto& object = objects[id];
    if(lod) {
      const auto* mesh = object.bucketId->staticMesh;
      if(mesh!=nullptr) {
        const auto& box = mesh->bbox.bbox;
        Vec3 middle = (box[0]+box[1])*0.5f;
        object.pos.project(middle);
        const Vec3 x(object.pos.at(0,0),object.pos.at(0,1),object.pos.at(0,2));
        const Vec3 y(object.pos.at(1,0),object.pos.at(1,1),object.pos.at(1,2));
        const Vec3 z(object.pos.at(2,0),object.pos.at(2,1),object.pos.at(2,2));
        const float scale = std::sqrt(Vec3::dotProduct(x,x)+Vec3::dotProduct(y,y)+Vec3::dotProduct(z,z));
        const float r = mesh->bbox.rConservative*scale + object.bucketId->mat.waveMaxAmplitude + Margin;
        const bool near = (middle-cam).quadLength() < CameraRadius*CameraRadius;
        if(!near && !main.testPoint(middle,r) && !sh0.testPoint(middle,r) && !sh1.testPoint(middle,r)) {
          skipped.fetch_add(1,std::memory_order_relaxed);
          return;
          }
        }
      }
    auto position = WorldWind::shaderMatrix(object.pos, scene.tickCount, scene.windPeriod,
                                            scene.zWindEnabled, object.wind, object.windIntensity,
                                            scene.windScale, scene.legacyWind);
    updateInstance(id, &position);
    computed.fetch_add(1,std::memory_order_relaxed);
    };
  if(par) {
    windIds.assign(objectsWind.begin(),objectsWind.end());
    Workers::parallelFor(windIds,work);
    } else {
    for(auto id:objectsWind)
      work(id);
    }
  FrameProfile::add(FrameProfile::Amount::WindComputed,computed.load());
  FrameProfile::add(FrameProfile::Amount::WindSkipped,skipped.load());
  }

// Levels of detail of strands (reports/content/2026-11-npc-hair/lod.md). A hair is thinner than a pixel, so the
// ribbon is drawn a pixel wide with the hair's share of it as its cover (strandRibbon, vertex_process.glsl). As
// long as it stays under a pixel, a few hairs can be drawn as one that is as wide as all of them: the cover of the
// hair on screen is the same, and the work of culling, vertices and pixels falls with the count. The meshlets of
// strands are dealt out evenly (groommesh.cpp), so a level is the start of the list: the cluster draws that many
// meshlets and the instance tells the vertex shader how much wider a strand is.
//   OPENGOTHIC_GROOM_LOD=<pixels>  hairs are merged until one is this wide on screen (0.5 by default); 0: no levels
void VisualObjects::preFrameUpdateStrands() {
  if(objectsStrand.empty())
    return;
  static const float merged = []{
    const char* e = std::getenv("OPENGOTHIC_GROOM_LOD");
    return e!=nullptr ? std::max(0.f,float(std::atof(e))) : 0.5f;
    }();
  static const int   Steps    = 8;                 // levels per halving of the count
  static const float ShareMin = 0.02f;             // a far head keeps this much: a few hundred hairs
  const float py   = std::abs(scene.proj.at(1,1));
  const float resY = float(scene.screenRes().y);
  if(py<=0 || resY<=0)
    return;

  for(auto id:objectsStrand) {
    auto&          obj      = objects[id];
    const uint32_t meshlets = obj.iboLen/PackedMesh::MaxInd;
    const auto*    mesh     = obj.bucketId->staticMesh;
    float          share    = 1;
    if(merged>0 && mesh!=nullptr && mesh->strandWidth>0) {
      const Vec3  at    = Vec3(obj.pos.at(3,0), obj.pos.at(3,1), obj.pos.at(3,2));
      const float scale = Vec3(obj.pos.at(0,0), obj.pos.at(0,1), obj.pos.at(0,2)).length();
      const float depth = std::max((at-scene.camPos()).length(), 1.f);
      const float pixel = 2.f*depth/(py*resY);     // a pixel at the head, in the world's units
      share = std::clamp(mesh->strandWidth*scale/(merged*pixel), ShareMin, 1.f);
      }
    // the level moves only when the wanted one is a whole step away: no flicker between two counts
    const float want  = -float(Steps)*std::log2(share);
    int         level = obj.strandLevel;
    if(std::abs(want-float(level))>=1.f)
      level = int(std::lround(want));
    level = std::clamp(level, 0, 255);
    const uint32_t count = std::clamp(uint32_t(std::ceil(float(meshlets)*std::exp2(-float(level)/float(Steps)))), 1u, meshlets);
    obj.strandLevel = uint8_t(level);
    if(count==obj.strandMeshlets)
      continue;
    obj.strandMeshlets = count;
    obj.strandWider    = float(meshlets)/float(count);
    if(obj.visible) {
      clustersMem[obj.clusterId].meshletCount = count;
      clustersMem.markClusters(obj.clusterId);
      }
    updateInstance(id);
    }
  }

void VisualObjects::preFrameUpdateMorph() {
  for(auto it=objectsMorph.begin(); it!=objectsMorph.end(); ) {
    auto& obj = objects[*it];

    bool reschedule = false;
    MorphData data = {};
    for(size_t i=0; i<Resources::MAX_MORPH_LAYERS; ++i) {
      auto&    ani  = obj.morphAnim[i];
      auto&    bk   = *obj.bucketId;
      auto&    anim = (*bk.staticMesh->morph.anim)[ani.id];
      uint64_t time = (scene.tickCount-ani.timeStart);

      float    alpha     = float(time%anim.tickPerFrame)/float(anim.tickPerFrame);
      float    intensity = ani.intensity*morphWeight(obj,ani);

      if(scene.tickCount>ani.timeUntil) {
        data.morph[i].intensity = 0;
        continue;
        }

      // a still face (S_ANGRY) is watched too while it fades in, or has an end time to go at
      const bool fadingIn = !ani.manual && anim.blendIn>0 && float(time)*anim.blendIn<1;
      if(anim.numFrames>1 || ani.manual || ani.timeUntil!=uint64_t(-1) || fadingIn)
        reschedule = true;

      const uint32_t samplesPerFrame = uint32_t(anim.samplesPerFrame);
      if(ani.manual) {
        data.morph[i].indexOffset = uint32_t(anim.index);
        data.morph[i].sample0     = ani.frame0*samplesPerFrame;
        data.morph[i].sample1     = ani.frame1*samplesPerFrame;
        data.morph[i].alpha       = uint16_t(ani.alpha*uint16_t(-1));
        data.morph[i].intensity   = uint16_t(intensity*uint16_t(-1));
        continue;
        }
      data.morph[i].indexOffset = uint32_t(anim.index);
      data.morph[i].sample0     = uint32_t((time/anim.tickPerFrame+0)%anim.numFrames)*samplesPerFrame;
      data.morph[i].sample1     = uint32_t((time/anim.tickPerFrame+1)%anim.numFrames)*samplesPerFrame;
      data.morph[i].alpha       = uint16_t(alpha*uint16_t(-1));
      data.morph[i].intensity   = uint16_t(intensity*uint16_t(-1));
      }

    obj.objMorphAnim.set(&data, 0, sizeof(data));
    if(!reschedule)
      it = objectsMorph.erase(it); else
      it++;
    }
  }

void VisualObjects::visibilityPass(Tempest::Encoder<Tempest::CommandBuffer> &cmd, int pass, bool history) {
  drawCmd.visibilityPass(cmd, pass, history);
  }

void VisualObjects::visibilityStats(Tempest::Encoder<Tempest::CommandBuffer>& cmd, uint8_t fId) {
  drawCmd.visibilityStats(cmd, fId);
  }

void VisualObjects::visibilityVsm(Tempest::Encoder<Tempest::CommandBuffer>& cmd, const Tempest::StorageBuffer* geometryStats) {
  drawCmd.visibilityVsm(cmd, geometryStats);
  }

void VisualObjects::drawMultiply(Tempest::Encoder<Tempest::CommandBuffer>& cmd) {
  drawCmd.drawCommon(cmd, SceneGlobals::V_Main, Material::Multiply);
  drawCmd.drawCommon(cmd, SceneGlobals::V_Main, Material::Multiply2);
  }

void VisualObjects::drawTranslucent(Tempest::Encoder<Tempest::CommandBuffer>& cmd) {
  // return;
  drawCmd.drawCommon(cmd, SceneGlobals::V_Main, Material::Ghost);
  drawCmd.drawCommon(cmd, SceneGlobals::V_Main, Material::AdditiveLight);
  drawCmd.drawCommon(cmd, SceneGlobals::V_Main, Material::Transparent);
  }

void VisualObjects::drawWater(Tempest::Encoder<Tempest::CommandBuffer>& cmd) {
  // return;
  drawCmd.drawCommon(cmd, SceneGlobals::V_Main, Material::Water);
  }

void VisualObjects::drawGBuffer(Tempest::Encoder<CommandBuffer>& cmd, uint8_t fId, DrawCommands::HistoryDraw history) {
  drawCmd.drawCommon(cmd, SceneGlobals::V_Main, Material::Solid, history);
  drawCmd.drawCommon(cmd, SceneGlobals::V_Main, Material::AlphaTest, history);
  drawImpostors(cmd, fId, history!=DrawCommands::HistoryDraw::Off);
  }

void VisualObjects::prepareImpostors(uint8_t fId) {
  auto& pub = impostorPub[fId];
  pub.clear();
  if(impostorDraws.empty())
    return;

  const auto meshOf = [this](size_t id) { return objects[id].bucketId->staticMesh; };
  std::sort(impostorDraws.begin(), impostorDraws.end(), [&](size_t a, size_t b) {
    return std::less<const StaticMesh*>()(meshOf(a), meshOf(b));
    });
  impostorInstances.resize(impostorDraws.size());
  const uint32_t lightFlags = (CrownLight::enabled() ? 0u : 1u) |
                              ((FoliageWind::enabled() & FoliageWind::Light)!=0 ? 0u : 2u);
  for(size_t i=0; i<impostorDraws.size(); ++i) {
    const auto& obj = objects[impostorDraws[i]];
    auto& instance  = impostorInstances[i];
    std::copy_n(obj.pos.data(), 16, instance.transform);
    instance.info[0] = clustersMem[obj.clusterId].instanceId;
    instance.info[1] = lightFlags;
    instance.info[2] = obj.animPtr & ~MeshLevels::CanopyVertexNormal;
    }

  auto& buffer = impostorBuffer[fId];
  const size_t bytes = impostorInstances.size()*sizeof(ImpostorInstance);
  if(buffer.byteSize()<bytes) {
    Resources::recycle(std::move(buffer));
    buffer = Resources::device().ssbo(Tempest::BufferHeap::Upload, Tempest::Uninitialized, std::max<size_t>(4096,bytes+bytes/2));
    }
  buffer.update(impostorInstances.data(), 0, bytes);

  for(size_t first=0; first<impostorDraws.size();) {
    const auto* mesh = meshOf(impostorDraws[first]);
    size_t end = first+1;
    while(end<impostorDraws.size() && meshOf(impostorDraws[end])==mesh)
      ++end;
    ImpostorPub run;
    run.joint = mesh->treeBough!=nullptr;
    const Vec3 lo = mesh->bbox.bbox[0], hi = mesh->bbox.bbox[1];
    const Vec3 center = (lo+hi)*0.5f;
    run.push  = Vec4(center.x, center.y, center.z, (hi-lo).length()*0.5f);
    for(size_t t=0; t<3; ++t)
      run.tex[t] = mesh->impostor.textures[t].shared();
    run.first = first;
    run.count = end-first;
    pub.push_back(std::move(run));
    first = end;
    }
  }

void VisualObjects::drawImpostors(Tempest::Encoder<CommandBuffer>& cmd, uint8_t fId, bool history) {
  const auto& pub = impostorPub[fId];
  if(pub.empty() || (DrawCommands::hiddenTypes & (1u << DrawCommands::Static))!=0)
    return;

  const auto sampler = Sampler::trillinear(ClampMode::ClampToEdge);
  const auto& shaders = Shaders::inst();
  bool joint = pub.front().joint;
  cmd.setPipeline(shaders.impostorPipeline(history, joint));
  cmd.setBinding(0, scene.uboGlobal[SceneGlobals::V_Main]);
  cmd.setBinding(1, impostorBuffer[fId]);
  for(const auto& run:pub) {
    if(joint!=run.joint) {
      joint = run.joint;
      cmd.setPipeline(shaders.impostorPipeline(history, joint));
      }
    cmd.setPushData(run.push);
    cmd.setBinding(2, run.tex[0], sampler);
    cmd.setBinding(3, run.tex[1], sampler);
    if(Shaders::options().doMaterialMaps)
      cmd.setBinding(4, run.tex[2], sampler);
    cmd.draw(nullptr, 0, 6, run.first, run.count);
    }
  }

void VisualObjects::drawShadow(Tempest::Encoder<Tempest::CommandBuffer>& cmd, int layer) {
  // return;
  auto view = SceneGlobals::VisCamera(SceneGlobals::V_Shadow0 + layer);
  drawCmd.drawCommon(cmd, view, Material::Solid);
  drawCmd.drawCommon(cmd, view, Material::AlphaTest);
  }

void VisualObjects::drawVsm(Tempest::Encoder<Tempest::CommandBuffer>& cmd) {
  drawCmd.drawVsm(cmd);
  }

void VisualObjects::drawHiZ(Tempest::Encoder<Tempest::CommandBuffer>& cmd) {
  drawCmd.drawHiZ(cmd);
  }

void VisualObjects::postFrameupdate() {
  instanceMem.join();
  }

void VisualObjects::updateRtScene(RtScene& out, Tempest::Encoder<Tempest::CommandBuffer>& cmd, uint8_t fId) {
  {
    std::lock_guard<std::mutex> guard(rtSync);
    std::swap(rtDirty, rtDirtyFrame);
    std::swap(rtFreed, rtFreedFrame);
    for(auto id:rtDirtyFrame)
      if(id<objects.size())
        objects[id].rtDirty = false;
  }
  for(auto slot:rtFreedFrame)
    out.freeSlot(slot);
  for(auto id:rtDirtyFrame)
    if(id<objects.size())
      updateRtSlot(out, objects[id]);
  rtFreedFrame.clear();
  rtDirtyFrame.clear();

  {
    // the BLAS of Landscapes: a chunk comes with its publication and goes when it is released
    std::lock_guard<std::mutex> guard(rtSync);
    std::swap(rtLandscapeDirty, rtLandscapeDirtyFrame);
    for(auto id:rtLandscapeDirtyFrame) {
      auto& l = rtLandscapes[id];
      l.dirty = false;
      if(!l.used)
        continue;
      if(l.slot==RtScene::NoSlot)
        l.slot = out.allocSlot();
      out.setLandscapeSlot(l.slot, *l.blas, l.alpha, l.geom.data(), l.geom.size(), l.visible);
      }
    rtLandscapeDirtyFrame.clear();
  }
  out.commit(cmd, fId);
  }

size_t VisualObjects::addRtLandscape(const AccelerationStructure& blas, Material::AlphaFunc alpha,
                                     std::vector<RtScene::Geometry> geom) {
  std::lock_guard<std::mutex> guard(rtSync);
  size_t id = rtLandscapes.size();
  if(!rtLandscapesFree.empty()) {
    id = rtLandscapesFree.back();
    rtLandscapesFree.pop_back();
    } else {
    rtLandscapes.emplace_back();
    }
  auto& l = rtLandscapes[id];
  l.blas    = &blas;
  l.geom    = std::move(geom);
  l.alpha   = alpha;
  l.slot    = RtScene::NoSlot;
  l.used    = true;
  l.visible = true;
  if(!l.dirty) {
    l.dirty = true;
    rtLandscapeDirty.push_back(id);
    }
  return id;
  }

void VisualObjects::setRtLandscapeVisible(size_t id, bool visible) {
  std::lock_guard<std::mutex> guard(rtSync);
  auto& l = rtLandscapes[id];
  if(l.visible==visible)
    return;
  l.visible = visible;
  if(!l.dirty) {
    l.dirty = true;
    rtLandscapeDirty.push_back(id);
    }
  }

void VisualObjects::freeRtLandscape(size_t id) {
  std::lock_guard<std::mutex> guard(rtSync);
  auto& l = rtLandscapes[id];
  // the slot goes at the next frame, as an object's; a dirty mark left behind finds the record unused or reused
  if(l.slot!=RtScene::NoSlot)
    rtFreed.push_back(l.slot);
  l.blas = nullptr;
  l.geom.clear();
  l.slot = RtScene::NoSlot;
  l.used = false;
  rtLandscapesFree.push_back(id);
  }

void VisualObjects::dbgClusters(Tempest::Painter& p, Vec2 wsz) {
  auto cam = GameServices::inst().camera();
  if(cam==nullptr)
    return;
  /*
  for(auto& c:clusters) {
    dbgDraw(p, wsz, *cam, c);
    }
  */

  /*
  if(auto pl = GameServices::inst().player()) {
    Cluster cx;
    cx.pos = pl->position();
    cx.r   = 100;
    dbgDraw(p, wsz, *cam, cx);
    }
  */
  }

void VisualObjects::dbgDraw(Tempest::Painter& p, Vec2 wsz, const Camera& cam, const DrawClusters::Cluster& cx) {
  auto  c       = cx.pos;
  auto  project = cam.projective();

  cam.view().project(c);
  //const vec3  c     = (scene.view * vec4(sphere.xyz, 1)).xyz;
  const float R     = cx.r;
  const float znear = cam.zNear();
  if(c.z - R < znear)
    ;//return;

  // depthMin = znear / (c.z + r);
  // float z = c.z + R;
  // depthMin  = scene.project[3][2]/z + scene.project[2][2];

  float P00 = project[0][0];
  float P11 = project[1][1];

  Vec3  cr   = c * R;
  float czr2 = c.z * c.z - R * R;

  float vx   = std::sqrt(c.x * c.x + czr2);
  float minx = (vx * c.x - cr.z) / (vx * c.z + cr.x);
  float maxx = (vx * c.x + cr.z) / (vx * c.z - cr.x);

  float vy   = std::sqrt(c.y * c.y + czr2);
  float miny = (vy * c.y - cr.z) / (vy * c.z + cr.y);
  float maxy = (vy * c.y + cr.z) / (vy * c.z - cr.y);

  Vec4 aabb;
  aabb = Vec4(minx * P00, miny * P11, maxx * P00, maxy * P11);
  aabb = aabb*0.5 + Vec4(0.5);

  aabb.x = aabb.x * wsz.x;
  aabb.z = aabb.z * wsz.x;

  aabb.y = aabb.y * wsz.y;
  aabb.w = aabb.w * wsz.y;

  if(aabb.x>=aabb.z)
    Log::d("");
  if(aabb.y>=aabb.w)
    Log::d("");

  p.setBrush(Color(0,0,1,0.1f));
  p.drawRect(int(aabb.x), int(aabb.y), int(aabb.z-aabb.x), int(aabb.w-aabb.y));
  }

void VisualObjects::dbgDrawBBox(Tempest::Painter& p, Tempest::Vec2 wsz, const Camera& cam, const DrawClusters::Cluster& c) {
  /*
  auto& b       = buckets[c.bucketId];
  auto  project = cam.viewProj();

  Vec4  aabb     = Vec4(1, 1, -1, -1);
  float depthMin = 1;
  for(uint32_t i=0; i<8; ++i) {
    float x = b[i&0x1 ? 1 : 0].x;
    float y = b[i&0x2 ? 1 : 0].y;
    float z = b[i&0x4 ? 1 : 0].z;

    const Vec3 pos = Vec3(x, y, z);
    Vec4 trPos = Vec4(pos,1.0);
    trPos = Vec4(obj.mat*trPos, 1.0);
    trPos = scene.viewProject*trPos;
    if(trPos.w<znear || false) {
      depthMin = 0;
      aabb     = vec4(0,0,1,1);
      return true;
      }

    vec3 bp = trPos.xyz/trPos.w;

    aabb.xy  = min(aabb.xy,  bp.xy);
    aabb.zw  = max(aabb.zw,  bp.xy);
    depthMin = min(depthMin, bp.z);
    }
  aabb = aabb*0.5 + Vec4(0.5);

  aabb.x = aabb.x * wsz.x;
  aabb.z = aabb.z * wsz.x;

  aabb.y = aabb.y * wsz.y;
  aabb.w = aabb.w * wsz.y;

  if(aabb.x>=aabb.z)
    Log::d("");
  if(aabb.y>=aabb.w)
    Log::d("");

  p.setBrush(Color(0,0,1,0.1f));
  p.drawRect(int(aabb.x), int(aabb.y), int(aabb.z-aabb.x), int(aabb.w-aabb.y));
  */
  }

