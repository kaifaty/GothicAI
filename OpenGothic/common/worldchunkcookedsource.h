#pragma once

// The game's reader of a cooked cell (docs/landscape-plan.md, Д3, task 3).
// ChunkMeshSource::read (worldchunksruntime.h) builds a cell from its .wcb
// (worldchunkbinary.h) and the set's index (worldchunkindex.h), with no glTF,
// when all of these hold:
//   - at world load the index lay where cookedIndexPath (worldchunkcook.h)
//     puts it without an output directory, passed its own validation, names
//     the source world of world.json, lists the same cells in the same order,
//     and its totals (vertices, features, materials, triangles) are what
//     readWorldChunks has just produced;
//   - the cell's GLB had at load the byte length of the index's entry, and
//     still has the length and time it had then. In a set with edits (ids past
//     world.json's counts) one GLB of another length rejects the whole index:
//     a changed cell can renumber the edited copies of its neighbours;
//   - the cell's .wcb, beside that GLB, opens, is for this cell and world, and
//     carries the GLB length and SHA-256 of the index's entry.
// Otherwise the cell is read from its GLB, with the same result. The index logs
// one line per world load, none when it is missing; a cell whose .wcb is there
// but cannot be used logs one line and reads its GLB from then on. A missing
// .wcb is not logged. OPENGOTHIC_WCB=0 at world load turns the cooked path off.
// The game writes no cooked file and hashes no GLB: a GLB changed at the same
// length after the cook is not noticed.

#include "worldchunkindex.h"
#include "worldchunksruntime.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <vector>

// The GLB readWorldChunks read for a cell, with its length and time then.
struct WorldChunkFile {
  std::filesystem::path           path;
  uintmax_t                       size = 0;
  std::filesystem::file_time_type modified;
  // Throws std::runtime_error "world chunks: chunk changed after world load: <path>".
  void verify() const;
  };

class CookedChunkSource {
  public:
    // What a cell is read against: its GLB as loaded and the loaded world's tables.
    struct World {
      const WorldChunkFile&               glb;
      const std::vector<asset::Material>& materials;
      size_t                              vertices = 0, features = 0;
      };

    // Called once by readWorldChunks with its result: finds and validates the
    // index, opens no .wcb. files are the cells' GLBs in world.json order.
    void load(const std::filesystem::path& dir, const std::filesystem::path& overrideDir,
              const WorldCellManifest& manifest, std::span<const WorldChunkFile> files, const asset::WorldMesh& mesh);
    // Cell i from its .wcb, or from glb() when the cooked path cannot be taken
    // or currentFile is set. Throws, naming the file, for a source ordinal past
    // the cell's triangles. Safe to call from several threads at once.
    WorldChunkMesh read(size_t cell, std::span<const uint32_t> sources, bool currentFile, const World& world,
                        const std::function<WorldChunkMesh()>& glb) const;
    bool            active() const { return index.has_value(); }
    ChunkReadCounts counts() const;
    size_t          memoryBytes() const;

  private:
    // Per cell: try the .wcb; the GLB had another length at load than the index's entry; the GLB from now on.
    enum State : uint8_t { Cooked = 0, Stale = 1, Glb = 2 };

    std::optional<CookedIndex>                 index;
    WorldChunkSource                           source;
    std::vector<ChunkId>                       cells;
    std::unique_ptr<std::atomic<uint8_t>[]>    states;
    mutable std::atomic<uint64_t>              cookedReads {0}, glbReads {0}, cookedNs {0}, glbNs {0};

    std::optional<WorldChunkMesh> readCooked(size_t cell, std::span<const uint32_t> sources, const World& world) const;
  };
