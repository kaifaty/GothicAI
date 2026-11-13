#pragma once

// Static collision's input from the cooked chunk records (docs/landscape-plan.md, Д3, task 5). DynamicWorld packs
// the landscape after WorldCloth::extract with PackedMesh PK_Physic, which reads only the vertices, the vertex and
// material indices and each material's name, group and disable_collision. The collision mesh is that slim
// asset::WorldMesh built from the cells' .wcb files, the set's index (worldchunkindex.h) and the world cloth
// bundle (worldclothbundle.h) alone, no glTF and no stitched mesh: the same vertices bit for bit at the same ids,
// the same triangles in the same order, the same materials table. The bodies stay as they were: one landscape
// body, one water body, the triangle order the navigation cache and the way heights depend on.
//
// Placing is assembleCookedWorld's (worldchunkcook.h, CookedParts): every cell's .wcb in world.json order, each
// triangle at its stitched rank (the index's missing orders and Added starts), stitched ids through
// CookedIndex::resolve, appended materials from the index; the bundle's exclusions are left out and the rest keep
// their order, as retainWorldTriangles leaves the landscape.
//
// The world load (World's constructor) uses it when all of these hold: the chunk table's source has a cooked index
// in use (CookedChunkSource::cookedSet), no GLB had another length at load (CookedSet::stale), WorldCloth::extract
// took the bundle for this load, the bundle beside the index reads and passes checkClothBundle, world.json still
// names the index's source world and cells, and every cell's .wcb opens and passes checkCookedCell. Otherwise the
// stitched landscape gives the collision, as before, with one log line "[world-collision] <file> not used, ...:
// <reason>"; nothing is logged without a cooked index (OPENGOTHIC_WCB=0 among others). A failure of the builder
// never fails the load. On success it logs "[world-collision] chunks <index>: N triangles, M vertices, T ms".
// Until the start without the stitched mesh (task 8) this reads the cells a second time at load.

#include "asset/worldmesh.h"

#include <optional>
#include <string>

struct ChunkTable;
struct CookedWorld;
class  ClothBundle;

// The collision mesh of a cooked world with the bundle's triangles left out. Throws std::runtime_error, naming
// the cell or the bundle, unless the bundle has the world's cells and its exclusions remove as many triangles as
// its original mesh has.
asset::WorldMesh assembleCollisionMesh(const CookedWorld& world, const ClothBundle& cloth);

struct WorldCollisionInput {
  std::optional<asset::WorldMesh> mesh; // from the chunk records; empty: the stitched landscape gives the collision
  double                          ms = 0; // the builder's wall time, also when it gave nothing

  const asset::WorldMesh& select(const asset::WorldMesh& landscape) const { return mesh ? *mesh : landscape; }
  // For the physics_static load mark: " input=chunks input_ms=…" or " input=landscape input_ms=…".
  std::string probe() const;
  };

// The world load's choice for the loaded chunk table, after WorldCloth::extract changed it: clothBundle is true
// when extract took the bundle for this load (WorldCloth::lastPath on the loading thread). Never throws.
WorldCollisionInput worldCollisionInput(const ChunkTable& table, bool clothBundle);
