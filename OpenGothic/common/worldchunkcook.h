#pragma once

// Cooking a world's chunks into .wcb files (worldchunkbinary.h) and assembling
// them back. readWorldChunks stays the truth: the cook runs it and stores each
// cell's part of its stitched result. The assembler is for the converter's
// checks, tests and measurements; the game does not read .wcb yet.

#include "worldchunkbinary.h"
#include "worldchunksruntime.h"

#include <filesystem>
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

struct CookedWorldStats {
  size_t   cells     = 0;
  uint64_t triangles = 0;
  uint64_t glbBytes  = 0; // of the GLBs read
  uint64_t wcbBytes  = 0; // of the .wcb written
  };

// Reads dir with overrideDir through readWorldChunks and writes one .wcb per
// cell of world.json (cookedChunkPath). Throws std::runtime_error.
CookedWorldStats cookWorldChunks(const std::filesystem::path& dir, const std::filesystem::path& overrideDir = {},
                                 const std::filesystem::path& outDir = {});

struct CookedWorld {
  std::filesystem::path    dir;      // the world directory, for unreferenced.bin
  WorldCellManifest        manifest;
  std::vector<CookedChunk> cells;    // in manifest order
  };

// One read and validation of every cell's .wcb, no glTF. Rejects a chunk cooked
// for another cell or world, or whose GLB no longer has the length it was cooked from.
CookedWorld readCookedWorld(const std::filesystem::path& dir, const std::filesystem::path& overrideDir = {},
                            const std::filesystem::path& outDir = {});
// Checks one cell's cooked chunk against world.json and its GLB (length only).
void checkCookedCell(const CookedChunk& chunk, const ChunkCell& cell, const WorldCellManifest& manifest,
                     const std::filesystem::path& glb);

// The stitched mesh and, if given, the chunk table rebuilt from the cooked
// chunks, world.json and unreferenced.bin alone: no glTF and no ID resolution.
// The table has no file-backed source.
asset::WorldMesh assembleCookedWorld(const CookedWorld& world, ChunkTable* table = nullptr);

// Empty when equal: entries (cell, file, meshes, count, bounds bit for bit),
// triangle ownership and GLB ordinals; otherwise the first difference.
std::string compareChunkTables(const ChunkTable& expected, const ChunkTable& actual);

// Assembles the cooked chunks of dir (cookedChunkPath) and compares them with
// readWorldChunks of the same files: empty when the mesh and the table are equal
// field by field, otherwise the first difference. With hash, every GLB's
// SHA-256 is checked against its chunk as well.
std::string verifyCookedWorld(const std::filesystem::path& dir, const std::filesystem::path& overrideDir = {},
                              const std::filesystem::path& outDir = {}, bool hash = false);
