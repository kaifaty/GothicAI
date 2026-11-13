#pragma once

// Cooking a world's chunks into .wcb files (worldchunkbinary.h) and the set's
// index (worldchunkindex.h), and assembling them back. readWorldChunks stays
// the truth: the cook runs it once and splits its stitched result into what
// each cell's GLB and world.json alone decide (the .wcb) and what the whole
// world and override set decide (the index). The assembler is for the
// converter's checks, tests and measurements; the game reads a cell through
// worldchunkcookedsource.h.

#include "worldchunkbinary.h"
#include "worldchunkindex.h"
#include "worldchunksruntime.h"

#include <filesystem>
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
  uint64_t triangles    = 0;
  uint64_t glbBytes     = 0;     // of the GLBs read
  uint64_t wcbBytes     = 0;     // of every cell's .wcb, written or left
  uint64_t indexBytes   = 0;
  };

// Reads dir with overrideDir through readWorldChunks, then writes one .wcb per
// cell of world.json (cookedChunkPath) and the index (cookedIndexPath). A file
// that already holds the encoded bytes is left as it is. Throws std::runtime_error.
CookedWorldStats cookWorldChunks(const std::filesystem::path& dir, const std::filesystem::path& overrideDir = {},
                                 const std::filesystem::path& outDir = {});

// Writes bytes to path through a temporary name unique to the process and the
// call, then renames it over path; leaves a file that already holds these bytes
// untouched. True when it wrote. Throws std::runtime_error.
bool writeCookedFile(const std::filesystem::path& path, std::span<const uint8_t> bytes);
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
asset::WorldMesh assembleCookedWorld(const CookedWorld& world, ChunkTable* table = nullptr);

// Empty when equal: entries (cell, file, meshes, count, bounds bit for bit),
// triangle ownership and GLB ordinals; otherwise the first difference.
std::string compareChunkTables(const ChunkTable& expected, const ChunkTable& actual);

// Assembles the cooked chunks of dir (cookedChunkPath, cookedIndexPath) and
// compares them with readWorldChunks of the same files: empty when the mesh and
// the table are equal field by field, otherwise the first difference. With
// hash, every GLB's SHA-256 is checked against its chunk as well.
std::string verifyCookedWorld(const std::filesystem::path& dir, const std::filesystem::path& overrideDir = {},
                              const std::filesystem::path& outDir = {}, bool hash = false);
