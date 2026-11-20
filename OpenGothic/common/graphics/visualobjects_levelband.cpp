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
  obj.bandInstance = instanceMem.alloc(sizeof(InstanceDesc));
  obj.bandCluster  = clustersMem.alloc(*obj.bucketId, obj.iboOff/PackedMesh::MaxInd, 0, obj.bucketId.toInt(), obj.bandCmd);
  auto& band = clustersMem[obj.bandCluster];
  band.instanceId = obj.bandInstance.offsetId<InstanceDesc>();
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
void VisualObjects::updateLevelBand(Object& obj, float height, float shadowHeight, bool shadowLevels, bool fullShadows,
                                    MeshLevels::Selection& level, uint8_t& shadowLevel) {
  const auto&  mesh  = *obj.bucketId->staticMesh;
  const size_t count = std::min(mesh.levels.size(), MeshLevels::MaxLevels);
  TreeLevelBand::Pick main, shadow;
  if(!obj.isGhost && !level.impostor()) {
    float px[MeshLevels::MaxLevels] = {};
    for(size_t i=0; i<count; ++i)
      px[i] = mesh.levels[i].px;
    main  = TreeLevelBand::main(px, count, height);
    level = {main.fine, main.fine};
    const bool crown = obj.levelSub==mesh.treeBough->sourceSub;
    const bool scale = FoliageTree::isTree(obj.bucketId->mat.foliage);
    if(scale && fullShadows)
      shadow = {};
    else if(scale && shadowLevels)
      shadow = TreeLevelBand::shadow(count, shadowHeight, crown ? 0 : main.fine);
    else
      shadow = main; // the shadow takes the visual GLB
    shadowLevel = shadow.fine;
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
  const auto& own  = clustersMem[obj.clusterId];
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
    next.shadowCommandId    = obj.bandCmd;
    }
  auto& band = clustersMem[obj.bandCluster];
  if(std::memcmp(&band, &next, sizeof(next))==0)
    return;
  if(band.firstMeshlet!=next.firstMeshlet || band.meshletCount!=next.meshletCount) {
    constexpr auto typeMask = DrawClusters::HistoryStatic;
    next.historyGeneration = (next.historyGeneration & typeMask) | ((next.historyGeneration+1) & ~typeMask);
    }
  band = next;
  clustersMem.markClusters(obj.bandCluster);
  }
