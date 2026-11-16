#pragma once

// The catalogue start of a world (docs/landscape-plan.md, Д3, task 8): with streaming on and a cooked set, the world
// load reads no GLB, stitches nothing and builds no whole asset::WorldMesh. Everything the start needs without
// geometry is in the set's index (worldchunkindex.h: the cells, the stitched totals, the appended materials) and its
// world cloth bundle (physics/worldclothbundle.h, version 2: what the cloth leaves of each cell — triangle count and
// bounds — the landscape's bounds and its materials in first use, the surfaces and the original mesh). The geometry
// of a cell is read only when the residency asks for it, from its .wcb (ChunkMeshSource::readLandscape).
//
// The start is taken (startWorldChunks) when all of these hold; otherwise readWorldChunks runs as before, with the
// same result and one line "[world-catalogue] <file> not used, the world is read and stitched: <reason>":
//   - the caller asks for it (not the built-in benchmark, worldbase.cpp) and none of OPENGOTHIC_STREAMING,
//     OPENGOTHIC_WORLD_CATALOGUE and OPENGOTHIC_WCB is 0 (no line for these; the path without streaming is task 9);
//   - the index lies where cookedIndexPath (worldchunkcook.h) puts it without an output directory (no line when it
//     does not), opens, and names world.json's source world and cells (checkCookedIndex);
//   - every cell's GLB has the byte length of its index entry, and its .wcb is there;
//   - the bundle lies beside the index, opens, passes checkClothBundle (the same source world, the SHA-256 of the
//     index bytes just read, this build's rules and algorithm), its exclusions add up to its original mesh and its
//     landscape materials are inside the material table.
// On success it logs "[world-catalogue] <index>: N cells from the index and the world cloth bundle, no GLB or .wcb
// read".
//
// What the start gives, in place of the stitched one:
//   - the chunk table: each entry's count and bounds as WorldCloth::extract leaves them, no triangleChunks or
//     triangleSources, a source whose catalogue() is this;
//   - the landscape: name, bbox and the stitched material table only; WorldCloth::extract returns the bundle's
//     surfaces and original mesh and changes nothing (worldclothbundle.cpp);
//   - the streaming view's start from landscapeMaterials() and landscapeBounds() (graphics/landscapestart.h);
//   - static collision from the cooked cells (physics/worldcollisionmesh.h) when its cache misses. If that builder
//     gives nothing, stitchedLandscape() reads and stitches the world then, once, as the old start would have had it.
// A cell whose .wcb cannot be used reads its GLB against the ids of a stitched read of the set, which the first such
// read builds once (the old start's ids; a hitch of the old load time, logged). The game hashes no GLB: a GLB
// changed at the same length after the cook is not noticed, as on the cooked cell path.

#include "worldchunkcookedsource.h"
#include "physics/worldclothbundle.h"

#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

class WorldCatalogue {
  public:
    const CookedSet&           set() const { return cooked; }
    // The stitched mesh's triangles, before world cloth: the index's total.
    size_t                     triangles() const { return totalTriangles; }
    std::span<const uint32_t>  exclusions(size_t cell) const { return bundle.exclusions(cell); }
    // The triangles of the cell the bundle leaves, as the chunk table counts them.
    uint32_t                   kept(size_t cell) const { return bundle.cell(cell).triangles; }
    std::span<const uint32_t>  landscapeMaterials() const { return bundle.landscapeMaterials(); }
    std::pair<asset::Vec3,asset::Vec3> landscapeBounds() const;
    // WorldCloth::extract's result on this start: the bundle's surfaces and original mesh, every mesh named name.
    WorldCloth::Extracted      cloth(const std::string& name) const;
    const std::filesystem::path& clothPath() const { return clothFile; }
    // The bundle's bytes and, once built, the stitched fallback's source and lists.
    size_t                     memoryBytes() const;

    // The landscape the old start would have after WorldCloth::extract (readWorldChunks with the bundle's
    // exclusions, or the island search when they do not fit the read), read now: static collision's fallback.
    // Throws std::runtime_error when the read fails, as the old start would at load; a failure is not retried.
    asset::WorldMesh stitchedLandscape() const;
    // Cell i from its GLB against the ids of a stitched read of the set (built once, then kept): the triangles of
    // sources, or without sources those the bundle leaves, in stitched order. Throws std::runtime_error.
    WorldChunkMesh   readGlb(size_t cell, const std::span<const uint32_t>* sources, bool currentFile) const;

  private:
    struct Stitched {
      std::shared_ptr<const ChunkMeshSource> source;
      std::vector<std::vector<uint32_t>>     lists; // per cell, GLB ordinals in stitched order without the exclusions
      };

    std::filesystem::path  dir, overrideDir, clothFile;
    CookedSet              cooked;
    ClothBundle            bundle;
    size_t                 totalTriangles = 0;
    mutable std::mutex     sync;
    mutable std::shared_ptr<const Stitched> stitched;
    mutable std::string    failure; // a stitched read that threw: later fallbacks throw it again without a read

    // A stitched read of the set with the bundle's exclusions taken out, or the island search's when they do not fit
    // it (the set changed since the start); fills stitched when it is empty.
    asset::WorldMesh read() const;
    // read() at most once: a failure is kept and thrown again. Called with sync held.
    asset::WorldMesh readOnce() const;

    friend asset::WorldMesh startWorldChunks(const std::filesystem::path&, const std::filesystem::path&,
                                             const WorldCellManifest&, ChunkTable&, bool);
  };

// The world load's chunk reading: with catalogue set, the catalogue start when the set allows it (above), else
// readWorldChunks(dir, overrideDir, &chunks). Throws as readWorldChunks does.
asset::WorldMesh startWorldChunks(const std::filesystem::path& dir, const std::filesystem::path& overrideDir,
                                  const WorldCellManifest& manifest, ChunkTable& chunks, bool catalogue);

// The triangles the world load read: the index's total after a catalogue start, else the landscape's.
size_t loadedWorldTriangles(const asset::WorldMesh& landscape, const ChunkTable& chunks);
