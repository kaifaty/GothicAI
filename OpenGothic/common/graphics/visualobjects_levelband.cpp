#include "visualobjects.h"

#include <algorithm>
#include <cstddef>
#include <cstring>

#include "graphics/foliagetree.h"
#include "graphics/mesh/submesh/staticmesh.h"
#include "graphics/treelevelband.h"
#include "frameprofile.h"

// The level bands of a tree with a bough hierarchy (treelevelband.h). The object's own cluster keeps the finer side of
// the main view's band and of the shadow's, a second cluster with an instance of its own draws the coarser side through
// the same command: the hierarchy mesh's command has the masked shadow pipelines (DrawCommands::commandId). Its room in
// the command is reserved once, for the largest level below the mesh itself; a view takes either its main range or its
// shadow range.

void VisualObjects::allocLevelBand(Object& obj) {
  const auto& mesh = *obj.bucketId->staticMesh;
  uint32_t reserve = 0;
  for(size_t i=1; i<=std::min(mesh.levels.size(), MeshLevels::MaxLevels); ++i)
    reserve = std::max(reserve, mesh.levelMeshlets(obj.iboOff, obj.iboLen, i).second);
  // the masked pipelines come with the impostor joint's (DrawCommands::commandId)
  if(reserve==0 || mesh.impostor.px<=0.f)
    return;
  obj.bandCmd      = obj.cmdId;
  obj.bandReserve  = reserve;
  // The VSM visibility and meshlet passes read this optional record after the ordinary instance.
  struct SunCaster {
    uint32_t firstMeshlet, meshletCount, commandId;
    float    cmPerTexel;
    uint32_t fullFirstMeshlet, lod1FirstMeshlet, lod1MeshletCount;
    float    lod1CmPerTexel;
    };
  static_assert(sizeof(InstanceDesc)==64 && sizeof(SunCaster)==32);
  const bool hasL4 = mesh.levels.size()==MeshLevels::MaxLevels && mesh.levels.back().px>0.f &&
                     mesh.levels.back().sub.size()==mesh.sub.size() && obj.impostorShadowCmd!=uint16_t(-1) &&
                     std::ranges::all_of(mesh.levels.back().sub, [](auto range) { return range.second!=0; }) &&
                     std::ranges::any_of(mesh.sub, [](const auto& sub) { return FoliageTree::isTree(sub.material.foliage); });
  const size_t bytes = sizeof(InstanceDesc) + (hasL4 ? sizeof(SunCaster) : 0);
  obj.bandInstance = instanceMem.alloc(bytes);
  if(hasL4) {
    instanceMem.realloc(obj.objInstance, bytes);
    clustersMem[obj.clusterId].instanceId = obj.objInstance.offsetId<InstanceDesc>();
    const auto [first, count] = mesh.levelMeshlets(obj.iboOff, obj.iboLen, MeshLevels::MaxLevels);
    // Every material uses the same diameter, including the largest authored displacement of any part of the tree.
    float grow = 0;
    for(const auto& sub : mesh.sub) {
      const auto& p = sub.material.foliage;
      const auto  bits = FoliageWind::pack(p, FoliageWind::Wind);
      grow = std::max(grow, FoliageTree::travel(bits, FoliageTree::reach(p, mesh.bbox.bbox[0], mesh.bbox.bbox[1])));
      }
    const Tempest::Vec3 margin(2*grow, 2*grow, 2*grow);
    const float diameter = (mesh.drawn.bbox[1] - mesh.drawn.bbox[0] + margin).length();
    SunCaster caster {first, count, obj.impostorShadowCmd | DrawClusters::ShadowCoarse, diameter / mesh.levels.back().px,
                      0, 0, 0, 0.f};
    if(mesh.treeBough && obj.levelSub==mesh.treeBough->sourceSub &&
       mesh.levels.front().sub.size()==mesh.sub.size()) {
      const auto [full, fullCount] = mesh.levelMeshlets(obj.iboOff, obj.iboLen, 0);
      const auto [lod1, lod1Count]  = mesh.levelMeshlets(obj.iboOff, obj.iboLen, 1);
      const float height = mesh.bbox.bbox[1].y - mesh.bbox.bbox[0].y + margin.y;
      if(fullCount>0 && lod1Count>0 && height>0.f) {
        caster.fullFirstMeshlet = full;
        caster.lod1FirstMeshlet = lod1;
        caster.lod1MeshletCount = lod1Count;
        caster.lod1CmPerTexel   = height / TreeLevelBand::ShadowPx[0];
        }
      }
    obj.objInstance.set(&caster, sizeof(InstanceDesc), sizeof(caster));
    caster.meshletCount = 0; // the owner supplies the whole caster on this mip
    caster.lod1MeshletCount = 0; // the coarse side never owns the full crown
    obj.bandInstance.set(&caster, sizeof(InstanceDesc), sizeof(caster));
    }
  obj.bandCluster  = clustersMem.alloc(*obj.bucketId, obj.iboOff/PackedMesh::MaxInd, 0, obj.bucketId.toInt(), obj.bandCmd);
  auto& band = clustersMem[obj.bandCluster];
  band.instanceId = obj.bandInstance.offsetId<InstanceDesc>();
  band.shadowCommandId = clustersMem[obj.clusterId].shadowCommandId & 0xFFFF0000u;
  band.historyGeneration |= clustersMem[obj.clusterId].historyGeneration & DrawClusters::HistoryStatic;
  drawCmd.addClusters(obj.bandCmd, reserve);
  }

void VisualObjects::freeLevelBand(Object& obj) {
  if(obj.bandCluster==uint32_t(-1))
    return;
  drawCmd.addClusters(obj.bandCmd, -obj.bandReserve);
  clustersMem.free(obj.bandCluster, 1);
  obj.bandCluster  = uint32_t(-1);
  obj.bandInstance = InstanceStorage::Id();
  obj.bandCmd      = uint16_t(-1);
  obj.bandReserve  = 0;
  obj.bandWord     = 0;
  }

// Replaces the hard picks of preFrameUpdateLevels: `level` and `shadowLevel` become the finer sides of the bands. The
// impostor and a ghost keep them; the shadow scale of a crown starts at the full GLB, as for a hierarchy crown there
bool VisualObjects::updateLevelBand(Object& obj, float height, float shadowHeight, bool shadowLevels, bool fullShadows,
                                    MeshLevels::Selection& level, uint8_t& shadowLevel, bool& shadowCoarse) {
  const auto&  mesh  = *obj.bucketId->staticMesh;
  const size_t count = std::min(mesh.levels.size(), MeshLevels::MaxLevels);
  const bool scale = FoliageTree::isTree(obj.bucketId->mat.foliage);
  const auto& own = clustersMem[obj.clusterId];
  const bool treeStats = (own.shadowCommandId & (DrawClusters::ShadowVisual | DrawClusters::ShadowCoarse))!=0;
  bool sunLod = false;
  TreeLevelBand::Pick main, shadow;
  if(!obj.isGhost && !level.impostor()) {
    float px[MeshLevels::MaxLevels] = {};
    for(size_t i=0; i<count; ++i)
      px[i] = mesh.levels[i].px;
    main  = TreeLevelBand::main(px, count, height);
    level = {main.fine, main.fine};
    const bool crown = obj.levelSub==mesh.treeBough->sourceSub;
    if(scale && fullShadows)
      shadow = {};
    else if(scale && shadowLevels)
      shadow = TreeLevelBand::shadow(count, shadowHeight, crown ? 0 : main.fine);
    else
      shadow = main; // the shadow takes the visual GLB
    sunLod = shadowLevels && !fullShadows && obj.bandInstance.size()>sizeof(InstanceDesc);
    shadowLevel = shadow.fine;
    if(treeStats)
      shadowCoarse = scale && shadowLevels && shadow.fine!=(crown ? 0 : main.fine);
    if(crown)
      main = {main.fine, main.fine, 0.f}; // its main view is the bough cut
    }
  if(main.active())
    FrameProfile::add(FrameProfile::Amount::TreeLevelBands, 1);
  if(shadow.active())
    FrameProfile::add(FrameProfile::Amount::TreeShadowBands, 1);

  const uint32_t word = TreeLevelBand::word(main, shadow, false);
  if(word!=obj.bandWord) {
    obj.bandWord = word;
    const uint32_t coarse = word==0 ? 0 : word | TreeLevelBand::Coarse;
    obj.objInstance.set(&word, offsetof(InstanceDesc,deformPtr), sizeof(word));
    obj.bandInstance.set(&coarse, offsetof(InstanceDesc,deformPtr), sizeof(coarse));
    }

  // The coarser side: in the main view the band's GLB, in the shadows the shadow band's. A side whose share is zero
  // is never drawn there: the main view skips the cluster (visibility_pass.comp), the shadows mask the last GLB away
  auto        next = clustersMem[obj.bandCluster];
  next.pos = own.pos;
  next.r   = own.r;
  next.meshletCount = next.shadowMeshletCount = 0;
  if(word!=0 && obj.visible) {
    const auto coarseShadow = mesh.levelMeshlets(obj.iboOff, obj.iboLen, shadow.active() ? shadow.coarse : count);
    const auto coarseMain   = main.active() ? mesh.levelMeshlets(obj.iboOff, obj.iboLen, main.coarse) : coarseShadow;
    next.firstMeshlet       = coarseMain.first;
    next.meshletCount       = coarseMain.second;
    next.shadowFirstMeshlet = coarseShadow.first;
    next.shadowMeshletCount = coarseShadow.second;
    const uint32_t treeClass = !treeStats ? 0 :
                               (scale && shadowLevels ? DrawClusters::ShadowCoarse : DrawClusters::ShadowVisual);
    next.shadowCommandId    = obj.bandCmd | treeClass | (sunLod ? DrawClusters::ShadowSunLod : 0u);
    }
  auto& band = clustersMem[obj.bandCluster];
  if(std::memcmp(&band, &next, sizeof(next))==0)
    return sunLod;
  if(band.firstMeshlet!=next.firstMeshlet || band.meshletCount!=next.meshletCount) {
    constexpr auto typeMask = DrawClusters::HistoryStatic;
    next.historyGeneration = (next.historyGeneration & typeMask) | ((next.historyGeneration+1) & ~typeMask);
    }
  band = next;
  clustersMem.markClusters(obj.bandCluster);
  return sunLod;
  }
