#pragma once

#include "worldchunkmanifest.h"

#include <chrono>
#include <memory>
#include <span>
#include <utility>

class ChunkMeshSource;
class CookedChunk;
class CookedChunkSource;
class CookedIndex;
struct CookedSet;
struct WorldChunkFile;
class WorldCatalogue;

// What readWorldChunks computes on the way, for the cooked chunks
// (worldchunkcook.h) and their timing; the game passes none.
struct WorldChunkTrace {
  using Clock = std::chrono::steady_clock;
  std::vector<int64_t> orders; // per stitched triangle: its GLB "order", -1 for geometry an edit added
  // Per stitched corner (three per triangle): the _VERTEX and _FEATURE its GLB carries, 0xFFFFFFFF for none.
  std::vector<std::pair<uint32_t,uint32_t>> corners;
  Clock::time_point parsed, ordered, stitched; // ends of reading, source-order sort, and ID resolution with stitching
  };

struct ChunkTable {
  struct Entry {
    ChunkId      id;
    std::string  file; // the cell's draw level 0 mesh, relative to the world directory, even with an override
    asset::Vec3 min {};
    asset::Vec3 max {};
    uint32_t     triangleCount = 0;
    std::vector<ChunkMeshRef> meshes; // every mesh of the cell from world.json; only file is read
    };

  std::vector<Entry> entries;
  // One table index per triangle in readWorldChunks' stitched source-order
  // mesh, filtered alongside polygons by retainWorldTriangles. Ownership comes
  // from the chunk file, so edited triangles keep their file's identity.
  // Streaming releases both arrays after the visual consumer captures its IDs
  // and the stitched CPU mesh has been destroyed. A catalogue start
  // (worldchunkcatalogue.h) has neither: its cells take ChunkMeshSource::readLandscape.
  std::vector<uint32_t> triangleChunks;
  // Chunk-local GLB triangle ordinals, in the same stitched/filtered order.
  // Empty without a file-backed source or after its loading consumer finishes.
  std::vector<uint32_t> triangleSources;
  std::shared_ptr<const ChunkMeshSource> source;
  };

struct WorldChunkMesh {
  asset::WorldMesh       mesh;
  // Compact local indices -> the full reader's original or appended IDs.
  std::vector<uint32_t>  vertexIds;
  std::vector<uint32_t>  featureIds;
  // Local position, feature and material IDs in the graph address its cooked chunk.
  std::shared_ptr<const CookedChunk> clusterHierarchy;
  };

// How ChunkMeshSource::read took its cells since the world was loaded.
struct ChunkReadCounts {
  uint64_t cooked = 0, glb = 0;      // reads from the cell's .wcb, and from its GLB
  double   cookedMs = 0, glbMs = 0;  // their wall time, summed over the calling threads
  };

class ChunkMeshSource {
  public:
    // Reads only this chunk's selected file. Polygon order follows sources,
    // including cloth filtering. Calls on the same source are independent.
    // currentFile retries a changed file against the existing IDs/materials;
    // the file must remain unchanged during this read. The cell comes from its
    // cooked .wcb when the world was loaded with a valid index for it, else
    // from its GLB, with the same result (worldchunkcookedsource.h);
    // currentFile always reads the GLB.
    WorldChunkMesh read(size_t chunk, std::span<const uint32_t> sources, bool currentFile=false) const;
    // After a catalogue start (worldchunkcatalogue.h): the cell's triangles the world cloth bundle leaves, in
    // stitched order — what read takes from the ownership arrays of a stitched start. From the cell's .wcb, else
    // from its GLB through the catalogue. Throws after a stitched start.
    WorldChunkMesh readLandscape(size_t chunk, bool currentFile=false) const;
    // The catalogue the world was started from; null after a stitched start.
    const WorldCatalogue* catalogue() const;
    ChunkReadCounts reads() const;
    // The cooked set the world was loaded with, if any (worldchunkcookedsource.h).
    const CookedChunkSource& cooked() const;
    // Estimated retained container storage, including strings; excludes allocator metadata.
    size_t memoryBytes() const;

  private:
    struct Impl;
    std::shared_ptr<const Impl> impl;
    WorldChunkMesh readGlb(size_t chunk, std::span<const uint32_t> sources, bool currentFile) const;
    explicit ChunkMeshSource(std::shared_ptr<const Impl> data) : impl(std::move(data)) {}
    // The catalogue start's source: cells from their .wcb against the index's ids, else WorldCatalogue::readGlb.
    static std::shared_ptr<const ChunkMeshSource> fromCatalogue(std::shared_ptr<const WorldCatalogue> catalogue,
                                                                std::vector<WorldChunkFile> files,
                                                                std::vector<asset::Material> materials,
                                                                CookedIndex index, CookedSet set,
                                                                const WorldCellManifest& manifest);
    // readWorldChunks; cookedCells loads the cooked index for the cells, as the game does.
    static asset::WorldMesh readStitched(const std::filesystem::path& dir, const std::filesystem::path& overrideDir,
                                         ChunkTable* chunks, WorldChunkTrace* trace, bool cookedCells);
    friend asset::WorldMesh readWorldChunks(const std::filesystem::path&, const std::filesystem::path&, ChunkTable*,
                                            WorldChunkTrace*);
    friend asset::WorldMesh readWorldChunksGlb(const std::filesystem::path&, const std::filesystem::path&, ChunkTable&);
    friend asset::WorldMesh startWorldChunks(const std::filesystem::path&, const std::filesystem::path&,
                                             const WorldCellManifest&, ChunkTable&, bool);
  };

// Reads the draw level 0 chunk of every cell in dir/world.json; other meshes of
// a cell are carried in the table, not opened. A chunk with the same file name
// under overrideDir (if not empty) replaces the base one. Fills materials,
// vertices, features and polygons of a fresh mesh. Unedited chunks restore
// the source arrays exactly, with every source index (the engine's meshlet
// packing keys on them); geometry an edit added or moved goes after them.
asset::WorldMesh readWorldChunks(const std::filesystem::path& dir, const std::filesystem::path& overrideDir = {},
                                 ChunkTable* chunks = nullptr, WorldChunkTrace* trace = nullptr);
// The same with a table whose source reads every cell from its GLB: it loads no cooked index and logs nothing
// about one. The catalogue start's fallback (worldchunkcatalogue.h).
asset::WorldMesh readWorldChunksGlb(const std::filesystem::path& dir, const std::filesystem::path& overrideDir,
                                    ChunkTable& chunks);

// Rebuild a single cell's cooked arrays in memory, from the exact GLB bytes named by the index.
CookedChunk recoverIndexedChunk(const std::filesystem::path& path, const CookedIndex& index, size_t cell,
                                std::span<const asset::Material> baseMaterials);

// Keep an increasing, unique list of source triangle IDs in source order.
// Only polygon arrays and their corresponding triangleChunks/triangleSources are filtered;
// material, vertex and feature arrays and their source IDs remain unchanged.
// If chunks is supplied it must cover every input triangle; bounds/counts are
// rebuilt from the retained triangles. Reusable by visual and collision passes.
void retainWorldTriangles(asset::WorldMesh& mesh, std::span<const uint32_t> triangles,
                          ChunkTable* chunks = nullptr);

// Lists source polygon IDs per table entry without compacting the source mesh.
std::vector<std::vector<uint32_t>> chunkTriangleLists(const asset::WorldMesh& mesh, const ChunkTable& chunks);

// Each table entry's GLB ordinals (triangleSources) in table order: what ChunkMeshSource::read takes for the cells
// a stitched start left. Empty after a catalogue start, whose cells take ChunkMeshSource::readLandscape.
std::vector<std::vector<uint32_t>> chunkSourceLists(const ChunkTable& chunks);

// The fields readWorldChunks fills, copied from a source mesh: materials,
// vertices, features and polygons. readWorldChunks of unedited chunks equals
// canonicalWorldMesh of the source mesh bit for bit.
asset::WorldMesh canonicalWorldMesh(const asset::WorldMesh& mesh);

// Empty when equal; otherwise the first difference, for logs and tests.
std::string compareWorldMeshes(const asset::WorldMesh& expected, const asset::WorldMesh& actual);
