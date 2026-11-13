#include "worldcollisionmesh.h"

#include "worldchunkcook.h"
#include "worldchunkcookedsource.h"
#include "worldclothbundle.h"

#include <Tempest/Log>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <system_error>

namespace fs = std::filesystem;

namespace {

using Clock = std::chrono::steady_clock;

[[noreturn]] void fail(const std::string& reason) {
  throw std::runtime_error(reason);
  }

std::string cellName(ChunkId id) {
  return std::to_string(id.x)+","+std::to_string(id.z);
  }

// The cooked world of the load: world.json read again, the index the load read, every cell's .wcb beside the GLB
// the load read, each checked as readCookedWorld checks it. file names what is being read, for the log line.
CookedWorld readLoaded(const CookedSet& set, const CookedIndex& index, std::string& file) {
  CookedWorld world;
  world.dir = set.dir;
  file = (set.dir/"world.json").string();
  world.manifest = readWorldChunkManifest(set.dir);
  const auto& source = world.manifest.source;
  if(source.world!=set.source.world || source.size!=set.source.size || source.crc32!=set.source.crc32)
    fail("world.json names another source world than at load");
  world.index = index;
  checkCookedIndex(world.index,world.manifest);
  world.cells.reserve(world.manifest.cells.size());
  for(size_t i=0;i<world.manifest.cells.size();++i) {
    const auto& name = world.manifest.cells[i].drawFile();
    const auto path = cookedChunkPath(set.dir,set.overrideDir,name);
    file = path.string();
    std::error_code error;
    if(!fs::is_regular_file(path,error))
      fail("the cell's .wcb is missing");
    auto chunk = CookedChunk::read(path);
    checkCookedCell(chunk,i,world.manifest,world.index,chunkGlbPath(set.dir,set.overrideDir,name));
    world.cells.push_back(std::move(chunk));
    }
  return world;
  }

}

asset::WorldMesh assembleCollisionMesh(const CookedWorld& world, const ClothBundle& cloth) {
  if(cloth.cells()!=world.cells.size())
    fail(cloth.name()+" lists "+std::to_string(cloth.cells())+" cells, the cooked world "+
         std::to_string(world.cells.size()));
  CookedParts parts;
  parts.collision = true;
  size_t named = 0;
  for(size_t c=0;c<cloth.cells();++c) {
    parts.exclusions.push_back(cloth.exclusions(c));
    named += parts.exclusions.back().size();
    }
  if(named!=cloth.originalTriangles())
    fail(cloth.name()+": its exclusions name "+std::to_string(named)+" triangles, its original mesh has "+
         std::to_string(cloth.originalTriangles()));
  return assembleCookedWorld(world,nullptr,parts);
  }

std::string WorldCollisionInput::probe() const {
  return std::string(" input=")+(mesh ? "chunks" : "landscape")+" input_ms="+std::to_string(ms);
  }

WorldCollisionInput worldCollisionInput(const ChunkTable& table, bool clothBundle) {
  const auto started = Clock::now();
  WorldCollisionInput out;
  const auto* set   = table.source ? table.source->cooked().cookedSet() : nullptr;
  const auto* index = table.source ? table.source->cooked().cookedIndex() : nullptr;
  if(set==nullptr || index==nullptr)
    return out;
  std::string file = set->index.string();
  try {
    if(!set->stale.empty())
      fail(set->stale);
    const auto bundlePath = set->index.parent_path()/wcl::FileName;
    file = bundlePath.string();
    std::error_code error;
    if(!fs::is_regular_file(bundlePath,error))
      fail("the world cloth bundle is missing");
    const auto bundle = ClothBundle::read(bundlePath);
    checkClothBundle(bundle,*set);
    if(!clothBundle)
      fail("WorldCloth::extract did not take the bundle for this load");
    const auto world = readLoaded(*set,*index,file);
    file = set->index.string();
    // What extract left of each cell in the loaded table is what the bundle leaves of its .wcb.
    if(table.entries.size()!=world.cells.size())
      fail("the loaded chunk table has "+std::to_string(table.entries.size())+" cells, the cooked world "+
           std::to_string(world.cells.size()));
    for(size_t c=0;c<world.cells.size();++c) {
      const auto kept = world.cells[c].triangles().size()-std::min(world.cells[c].triangles().size(),
                                                                  bundle.exclusions(c).size());
      if(kept!=table.entries[c].triangleCount)
        fail("cell "+cellName(world.manifest.cells[c].id)+": the bundle leaves "+std::to_string(kept)+
             " triangles, the loaded chunk table "+std::to_string(table.entries[c].triangleCount));
      }
    out.mesh = assembleCollisionMesh(world,bundle);
    }
  catch(const std::exception& e) {
    out.mesh.reset();
    Tempest::Log::e("[world-collision] ",file,": ",e.what(),"; the collision comes from the stitched landscape");
    }
  catch(...) {
    out.mesh.reset();
    Tempest::Log::e("[world-collision] ",file,": unknown error; the collision comes from the stitched landscape");
    }
  out.ms = std::chrono::duration<double,std::milli>(Clock::now()-started).count();
  if(out.mesh) {
    char ms[32];
    std::snprintf(ms,sizeof(ms),"%.1f",out.ms);
    Tempest::Log::i("[world-collision] chunks ",set->index.string(),": ",
                    uint64_t(out.mesh->polygons.material_indices.size())," triangles, ",
                    uint64_t(out.mesh->vertices.size())," vertices, ",ms," ms");
    }
  return out;
  }

const asset::WorldMesh& WorldCollisionSource::operator()() {
  ++count;
  if(!built)
    built = worldCollisionInput(table,clothBundle);
  return built->select(landscape);
  }

std::string WorldCollisionSource::probe() const {
  return built ? built->probe() : " input=cache input_ms=0";
  }
