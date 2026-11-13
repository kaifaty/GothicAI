#pragma once

// Cooking a world's chunks into .wcb files (worldchunkbinary.h), the set's
// index (worldchunkindex.h) and its world cloth bundle (physics/worldclothbundle.h),
// and assembling them back. readWorldChunks stays the truth: the cook runs it
// once and splits its stitched result into what each cell's GLB and world.json
// alone decide (the .wcb) and what the whole world and override set decide (the
// index), then runs the island search of world cloth on it (the bundle). The
// assembler is for the converter's checks, tests and measurements; the game
// reads a cell through worldchunkcookedsource.h.

#include "worldchunkbinary.h"
#include "worldchunkindex.h"
#include "worldchunksruntime.h"

#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <vector>

// The GLB readWorldChunks reads for a draw level 0 file of world.json: the
// override directory's when it has a file of that name, else the world's.
std::filesystem::path chunkGlbPath(const std::filesystem::path& dir, const std::filesystem::path& overrideDir,
                                   const std::string& file);
// Its cooked chunk: that GLB's path with the extension .wcb, or, with outDir,
// outDir/<file> with the extension .wcb for base and override cells alike.
std::filesystem::path cookedChunkPath(const std::filesystem::path& dir, const std::filesystem::path& overrideDir,
                                      const std::string& file, const std::filesystem::path& outDir = {});
// The set's index: outDir/chunks.wci with outDir, else in the override
// directory when there is one, else in the world directory.
std::filesystem::path cookedIndexPath(const std::filesystem::path& dir, const std::filesystem::path& overrideDir,
                                      const std::filesystem::path& outDir = {});

struct CookedWorldStats {
  size_t   cells        = 0;
  size_t   written      = 0;     // cells whose .wcb was written
  size_t   left         = 0;     // cells whose .wcb on disk already held the bytes: not rewritten
  bool     indexWritten = false;
  bool     clothWritten = false; // the world cloth bundle
  uint64_t triangles    = 0;
  uint64_t glbBytes     = 0;     // of the GLBs read
  uint64_t wcbBytes     = 0;     // of every cell's .wcb, written or left
  uint64_t indexBytes   = 0;
  uint64_t clothBytes   = 0;
  size_t   clothSurfaces  = 0;
  uint64_t clothTriangles = 0;   // taken out of the cells as cloth
  };

// Reads dir with overrideDir through readWorldChunks, then writes one .wcb per
// cell of world.json (cookedChunkPath), the world cloth bundle (cookedClothPath,
// physics/worldclothbundle.h) and the index (cookedIndexPath). A file that
// already holds the encoded bytes is left as it is. Write order: the index is
// removed before the first cell or bundle file is created or replaced, and
// written last, so a cook stopped at any point leaves either no index or a
// consistent set; every writer of cooked files keeps this order. A cook whose
// GLBs changed while it ran writes no index (something other than a cook may
// have changed them). One cook per directory at a time: the whole cook runs
// under an exclusive lock on the directory of the index (flock on POSIX, a named
// mutex on Windows; both go with the process, no file is left), a second cook
// waits; a cook nested in another's hook on the same thread takes no second
// lock. Where no lock can be taken (a file system without locks) the cook logs
// one line and runs without it. The hooks are for the tests: beforeIndex runs
// after the cells and the bundle, before the GLB check; afterCheck after the
// check, before the index is written.
// Throws std::runtime_error.
CookedWorldStats cookWorldChunks(const std::filesystem::path& dir, const std::filesystem::path& overrideDir = {},
                                 const std::filesystem::path& outDir = {},
                                 const std::function<void()>& beforeIndex = {},
                                 const std::function<void()>& afterCheck = {});

// Writes bytes to path through a temporary name unique to the process and the
// call, then renames it over path; leaves a file that already holds these bytes
// untouched. True when it wrote. beforeWrite, when given, runs once a write is
// needed and before the temporary is created. Throws std::runtime_error.
bool writeCookedFile(const std::filesystem::path& path, std::span<const uint8_t> bytes,
                     const std::function<void()>& beforeWrite = {});
// The temporary name writeCookedFile uses: path.<process id>-<call number>.tmp.
std::filesystem::path cookedTemporaryPath(const std::filesystem::path& path);

struct CookedWorld {
  std::filesystem::path    dir;      // the world directory, for unreferenced.bin
  WorldCellManifest        manifest;
  CookedIndex              index;
  std::vector<CookedChunk> cells;    // in manifest order
  };

// One read and validation of the index and every cell's .wcb, no glTF. Rejects
// a missing index, an index of another world or with other cells than
// world.json, a chunk cooked for another cell or world, one the index lists
// with another GLB, and one whose GLB no longer has the length it was cooked from.
CookedWorld readCookedWorld(const std::filesystem::path& dir, const std::filesystem::path& overrideDir = {},
                            const std::filesystem::path& outDir = {});
// Checks the index against world.json: source world, cell count and order, totals.
void checkCookedIndex(const CookedIndex& index, const WorldCellManifest& manifest);
// Checks cell i's cooked chunk against world.json, the index and its GLB (length only).
void checkCookedCell(const CookedChunk& chunk, size_t i, const WorldCellManifest& manifest, const CookedIndex& index,
                     const std::filesystem::path& glb);
// The same without the GLB: against cell i's id, the source world and the index; name names the chunk in errors.
void checkCookedCell(const CookedChunk& chunk, size_t i, ChunkId id, const WorldChunkSource& source,
                     const CookedIndex& index, const std::string& name);

// The stitched mesh and, if given, the chunk table rebuilt from the index, the
// cooked chunks, world.json and unreferenced.bin alone: no glTF and none of
// readWorldChunks' ID resolution. The table has no file-backed source.
// Defined in worldchunkassemble.cpp.
asset::WorldMesh assembleCookedWorld(const CookedWorld& world, ChunkTable* table = nullptr);

// What assembleCookedWorld builds when not everything is wanted. collision:
// only what static collision reads (PackedMesh PK_Physic): vertices, vertex
// and material indices, materials; features, feature indices, lightmaps and
// flags stay empty. exclusions: empty, or one list per cell of ascending
// cell-local triangle ordinals (the world cloth bundle's) that are left out;
// the other triangles keep their stitched order, as retainWorldTriangles keeps
// them. Exclusions take no chunk table.
struct CookedParts {
  bool                                   collision = false;
  std::vector<std::span<const uint32_t>> exclusions;
  };
asset::WorldMesh assembleCookedWorld(const CookedWorld& world, ChunkTable* table, const CookedParts& parts);

// Empty when equal: entries (cell, file, meshes, count, bounds bit for bit),
// triangle ownership and GLB ordinals; otherwise the first difference.
std::string compareChunkTables(const ChunkTable& expected, const ChunkTable& actual);

// Assembles the cooked chunks of dir (cookedChunkPath, cookedIndexPath) and
// compares them with readWorldChunks of the same files: empty when the mesh and
// the table are equal field by field and the world cloth bundle equals a fresh
// island search on that mesh (with the index's SHA-256, this build's rules and
// algorithm), otherwise the first difference. With hash, every GLB's SHA-256 is
// checked against its chunk as well.
std::string verifyCookedWorld(const std::filesystem::path& dir, const std::filesystem::path& overrideDir = {},
                              const std::filesystem::path& outDir = {}, bool hash = false);
