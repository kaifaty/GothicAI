#include "worldchunkcatalogue.h"

#include "gothicglb.h"
#include "worldchunkcook.h"

#include <Tempest/Log>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string_view>

namespace fs = std::filesystem;

namespace {

using Clock = std::chrono::steady_clock;

[[noreturn]] void fail(const std::string& reason) {
  throw std::runtime_error(reason);
  }

bool switchedOff(const char* name) {
  const char* value = std::getenv(name);
  return value!=nullptr && std::string_view(value)=="0";
  }

}

std::pair<asset::Vec3,asset::Vec3> WorldCatalogue::landscapeBounds() const {
  const auto& h = bundle.header();
  return {h.landscapeMin,h.landscapeMax};
  }

WorldCloth::Extracted WorldCatalogue::cloth(const std::string& name) const {
  WorldCloth::Extracted out;
  for(size_t i=0;i<bundle.surfaces();++i)
    out.surfaces.push_back(bundle.surface(i,name));
  out.original = bundle.original(name);
  return out;
  }

asset::WorldMesh WorldCatalogue::read() const {
  const auto started = Clock::now();
  ChunkTable table;
  auto mesh = readWorldChunksGlb(dir,overrideDir,table);
  std::string mismatch;
  if(table.entries.size()!=bundle.cells())
    mismatch = "the stitched read has "+std::to_string(table.entries.size())+" cells, the world cloth bundle "+
               std::to_string(bundle.cells());
  // The cells' triangles in stitched order; the bundle's exclusions are taken out of both, as extract does.
  std::vector<std::vector<uint32_t>> lists(table.entries.size());
  std::vector<uint32_t> remaining;
  remaining.reserve(table.triangleChunks.size());
  std::vector<size_t> next(table.entries.size(),0);
  for(size_t t=0;mismatch.empty() && t<table.triangleChunks.size();++t) {
    const auto c = table.triangleChunks[t], ordinal = table.triangleSources[t];
    const auto leave = bundle.exclusions(c);
    // Within a cell, exclusions are ascending; the stitched order is not, so look the ordinal up.
    if(std::binary_search(leave.begin(),leave.end(),ordinal))
      ++next[c];
    else {
      lists[c].push_back(ordinal);
      remaining.push_back(uint32_t(t));
      }
    }
  for(size_t c=0;mismatch.empty() && c<lists.size();++c)
    if(next[c]!=bundle.exclusions(c).size() || lists[c].size()!=kept(c))
      mismatch = "cell "+table.entries[c].file+": the stitched read leaves "+std::to_string(lists[c].size())+
                 " triangles after "+std::to_string(next[c])+" exclusions, the world cloth bundle "+
                 std::to_string(kept(c))+" after "+std::to_string(bundle.exclusions(c).size());
  if(!mismatch.empty()) {
    // The set changed since the start (a GLB of the same length): the old start would have run the island search.
    Tempest::Log::e("[world-catalogue] ",clothFile.string()," does not fit the stitched read, the island search runs: ",
                    mismatch);
    (void)WorldCloth::searchIslands(mesh,&table);
    lists = chunkSourceLists(table);
    }
  else if(remaining.size()!=table.triangleChunks.size())
    retainWorldTriangles(mesh,remaining,&table);
  if(!stitched) {
    auto s = std::make_shared<Stitched>();
    s->source = table.source;
    s->lists  = std::move(lists);
    stitched  = std::move(s);
    }
  char ms[32];
  std::snprintf(ms,sizeof(ms),"%.1f",std::chrono::duration<double,std::milli>(Clock::now()-started).count());
  Tempest::Log::i("[world-catalogue] ",cooked.index.string(),": the world was read and stitched for a fallback, ",
                  uint64_t(mesh.polygons.material_indices.size())," triangles, ",ms," ms");
  return mesh;
  }

size_t WorldCatalogue::memoryBytes() const {
  std::lock_guard guard(sync);
  size_t bytes = sizeof(*this)+bundle.byteSize();
  if(stitched) {
    bytes += stitched->source->memoryBytes();
    for(const auto& list:stitched->lists)
      bytes += list.capacity()*sizeof(list[0]);
    }
  return bytes;
  }

asset::WorldMesh WorldCatalogue::readOnce() const {
  if(!failure.empty())
    fail(failure);
  try {
    return read();
    }
  catch(const std::exception& e) {
    failure = std::string("the stitched read of the fallback failed: ")+e.what();
    fail(failure);
    }
  }

asset::WorldMesh WorldCatalogue::stitchedLandscape() const {
  std::lock_guard guard(sync);
  return readOnce();
  }

WorldChunkMesh WorldCatalogue::readGlb(size_t cell, const std::span<const uint32_t>* sources, bool currentFile) const {
  std::shared_ptr<const Stitched> s;
  {
  std::lock_guard guard(sync);
  if(!stitched)
    (void)readOnce();
  s = stitched;
  }
  return s->source->read(cell,sources!=nullptr ? *sources : std::span<const uint32_t>(s->lists.at(cell)),currentFile);
  }

asset::WorldMesh startWorldChunks(const fs::path& dir, const fs::path& overrideDir, const WorldCellManifest& manifest,
                                  ChunkTable& chunks, bool catalogue) {
  // Without streaming the whole landscape is drawn from the stitched mesh (ChunkResidency::create gives none).
  if(!catalogue || switchedOff("OPENGOTHIC_STREAMING") || switchedOff("OPENGOTHIC_WORLD_CATALOGUE") ||
     switchedOff("OPENGOTHIC_WCB"))
    return readWorldChunks(dir,overrideDir,&chunks);
  const auto indexPath = cookedIndexPath(dir,overrideDir);
  std::error_code error;
  if(!fs::is_regular_file(indexPath,error))
    return readWorldChunks(dir,overrideDir,&chunks);
  auto file = indexPath;
  try {
    if(manifest.chunkSize!=ChunkId::Size)
      fail("runtime chunks require a 64 m grid");
    auto bytes = std::make_shared<const std::vector<uint8_t>>(gothicglb::readFile(indexPath));
    const std::span<const uint8_t> view(bytes->data(),bytes->size());
    const auto hash = sha256Bytes(view);
    auto index = CookedIndex::open(view,indexPath.string(),std::move(bytes));
    checkCookedIndex(index,manifest);
    const size_t cells = manifest.cells.size();

    // Every GLB at its index entry's length, every .wcb there: the old start's "no cell is stale".
    std::vector<WorldChunkFile> files;
    files.reserve(cells);
    for(size_t i=0;i<cells;++i) {
      const auto& name = manifest.cells[i].drawFile();
      const auto glb = chunkGlbPath(dir,overrideDir,name);
      files.push_back({glb,fs::file_size(glb),fs::last_write_time(glb)});
      if(files.back().size!=index.cells()[i].glbSize)
        fail(glb.string()+" has "+std::to_string(files.back().size)+" bytes, the index lists the cell with a GLB of "+
             std::to_string(index.cells()[i].glbSize)+" bytes");
      const auto wcb = cookedChunkPath(dir,overrideDir,name);
      if(!fs::is_regular_file(wcb,error))
        fail(wcb.string()+" is missing");
      }

    auto state = std::make_shared<WorldCatalogue>();
    state->dir         = dir;
    state->overrideDir = overrideDir;
    state->cooked      = {indexPath,hash,manifest.source,cells,{},dir,overrideDir};
    state->clothFile   = cookedClothPath(dir,overrideDir);
    file = state->clothFile;
    if(!fs::is_regular_file(state->clothFile,error))
      fail("the world cloth bundle is missing");
    state->bundle = ClothBundle::read(state->clothFile);
    checkClothBundle(state->bundle,state->cooked);
    const auto& bundle = state->bundle;
    size_t excluded = 0;
    for(size_t c=0;c<cells;++c)
      excluded += bundle.exclusions(c).size();
    if(excluded!=bundle.originalTriangles())
      fail("its exclusions name "+std::to_string(excluded)+" triangles, its original mesh has "+
           std::to_string(bundle.originalTriangles()));
    const auto& totals = index.header();
    for(const auto id:bundle.landscapeMaterials())
      if(id>=totals.materials)
        fail("the landscape material "+std::to_string(id)+" is past the index's "+std::to_string(totals.materials));
    state->totalTriangles = totals.triangles;

    asset::WorldMesh landscape;
    landscape.materials = manifest.materials;
    for(size_t i=0;i<index.appendedMaterials();++i)
      landscape.materials.push_back(index.appended(i));
    ChunkTable table;
    for(size_t c=0;c<cells;++c) {
      const auto left = bundle.cell(c);
      table.entries.push_back({manifest.cells[c].id,manifest.cells[c].drawFile(),left.min,left.max,left.triangles,
                               manifest.cells[c].meshes});
      }
    auto set = state->cooked;
    std::shared_ptr<const WorldCatalogue> shared = std::move(state);
    table.source = ChunkMeshSource::fromCatalogue(shared,std::move(files),landscape.materials,std::move(index),
                                                  std::move(set),manifest);
    chunks = std::move(table);
    Tempest::Log::i("[world-catalogue] ",indexPath.string(),": ",uint64_t(cells)," cells from the index and the world "
                    "cloth bundle, no GLB or .wcb read (OPENGOTHIC_WORLD_CATALOGUE=0 turns this off)");
    return landscape;
    }
  catch(const std::exception& e) {
    Tempest::Log::e("[world-catalogue] ",file.string()," not used, the world is read and stitched: ",e.what());
    }
  return readWorldChunks(dir,overrideDir,&chunks);
  }

size_t loadedWorldTriangles(const asset::WorldMesh& landscape, const ChunkTable& chunks) {
  if(chunks.source && chunks.source->catalogue())
    return chunks.source->catalogue()->triangles();
  return landscape.polygons.material_indices.size();
  }
