#include "worldchunkcookedsource.h"

#include "gothicglb.h"
#include "worldchunkcatalogue.h"
#include "worldchunkcook.h"

#include <Tempest/Log>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <string_view>

namespace fs = std::filesystem;

namespace {

using Clock = std::chrono::steady_clock;
constexpr uint32_t Unset = 0xFFFFFFFFu;

// A source list the caller got wrong: the GLB path would fail on it as well, so it is not a reason to fall back.
struct SourceError : std::runtime_error {
  using std::runtime_error::runtime_error;
  };

uint64_t nanoseconds(Clock::time_point start) {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-start).count());
  }

// The stitched id of every local index of one array, each checked against the loaded world's count.
template<class Record>
std::vector<uint32_t> stitchedIds(std::span<const Record> records, std::span<const wci::Exception> exceptions,
                                  size_t limit, const char* kind, const std::string& name) {
  std::vector<uint32_t> ids(records.size());
  for(size_t local=0;local<records.size();++local) {
    const auto id = CookedIndex::resolve(exceptions,uint32_t(local),records[local].source);
    if(id>=limit)
      throw std::runtime_error(name+": "+kind+" "+std::to_string(local)+" has the stitched id "+
                               (id==wcb::None ? std::string("none") : std::to_string(id))+
                               ", outside the loaded world's "+std::to_string(limit));
    ids[local] = id;
    }
  return ids;
  }

// The cell as ChunkMeshSource's GLB path builds it: the triangles of sources in their order, then
// positions and features compacted in first use, each with its stitched id.
WorldChunkMesh build(const CookedChunk& chunk, const CookedIndex& index, size_t cell, std::span<const uint32_t> sources,
                     const CookedChunkSource::World& world, const std::string& name) {
  const auto positions = chunk.positions();
  const auto features  = chunk.features();
  const auto triangles = chunk.triangles();
  const auto vertexIds   = stitchedIds(positions,index.vertexExceptions(cell),world.vertices,"position",name);
  const auto featureIds  = stitchedIds(features,index.featureExceptions(cell),world.features,"feature",name);
  auto materialIds = stitchedIds(chunk.materials(),index.materialExceptions(cell),world.materials.size(),
                                 "material",name);
  for(size_t local=0;local<materialIds.size();++local) {
    const auto record = chunk.material(local);
    if(!gothicglb::sameMaterial(world.materials[materialIds[local]],record))
      throw std::runtime_error(name+": material "+std::to_string(local)+" ("+record.name+
                               ") differs from the loaded world's material "+std::to_string(materialIds[local]));
    }

  WorldChunkMesh out;
  out.mesh.materials = world.materials;
  auto& q = out.mesh.polygons;
  q.material_indices.reserve(sources.size());
  q.lightmap_indices.reserve(sources.size());
  q.flags.reserve(sources.size());
  q.vertex_indices.reserve(sources.size()*3);
  q.feature_indices.reserve(sources.size()*3);
  std::vector<uint32_t> vertexOut(positions.size(),Unset), featureOut(features.size(),Unset);
  for(const auto source:sources) {
    if(source>=triangles.size())
      throw SourceError("world chunks: chunk source triangle "+std::to_string(source)+" past the "+
                        std::to_string(triangles.size())+" triangles of "+name);
    const auto& t = triangles[source];
    q.material_indices.push_back(materialIds[t.material]);
    q.lightmap_indices.push_back(t.lightmap);
    q.flags.push_back(wcb::unpackFlags(t.flags,t.sector));
    for(unsigned c=0;c<3;++c) {
      auto& v = vertexOut[t.vertex[c]];
      if(v==Unset) {
        const auto& p = positions[t.vertex[c]];
        v = uint32_t(out.mesh.vertices.size());
        out.mesh.vertices.push_back({p.x,p.y,p.z});
        out.vertexIds.push_back(vertexIds[t.vertex[c]]);
        }
      auto& f = featureOut[t.feature[c]];
      if(f==Unset) {
        const auto& r = features[t.feature[c]];
        asset::VertexFeature feature;
        feature.texture = {r.u,r.v};
        feature.light   = r.light;
        feature.normal  = {r.nx,r.ny,r.nz};
        f = uint32_t(out.mesh.features.size());
        out.mesh.features.push_back(feature);
        out.featureIds.push_back(featureIds[t.feature[c]]);
        }
      q.vertex_indices.push_back(v);
      q.feature_indices.push_back(f);
      }
    }
  if(!chunk.hierarchy().clusters.empty()) {
    out.clusterMaterialIds = std::move(materialIds);
    if(sources.size()!=triangles.size())
      out.clusterTriangleSources.assign(sources.begin(),sources.end());
    }
  return out;
  }

// The cell's triangles in stitched order — by rank: a source triangle's from its order, the k-th Added one's
// addedStart + k — without the ascending exclusions. Throws, naming the .wcb, unless the index places every
// triangle once, every exclusion is a triangle of the cell and kept triangles are left.
std::vector<uint32_t> landscapeSources(const CookedChunk& chunk, const CookedIndex& index, size_t cell,
                                       std::span<const uint32_t> exclusions, uint32_t kept, const std::string& name) {
  const auto triangles = chunk.triangles();
  std::vector<uint64_t> ranked;
  ranked.reserve(triangles.size());
  uint64_t added = index.cells()[cell].addedStart;
  size_t next = 0;
  for(size_t i=0;i<triangles.size();++i) {
    const auto& t = triangles[i];
    const uint64_t rank = t.order==wcb::Added ? added++ : index.sourceRank(t.order);
    if(rank>=index.header().triangles)
      throw std::runtime_error(name+": triangle "+std::to_string(i)+" of order "+std::to_string(t.order)+
                               " has no place in the index");
    if(next<exclusions.size() && exclusions[next]==i) {
      ++next;
      continue;
      }
    ranked.push_back(rank<<32 | i);
    }
  if(next!=exclusions.size())
    throw std::runtime_error(name+": the world cloth bundle leaves out triangle "+std::to_string(exclusions[next])+
                             ", the cell holds "+std::to_string(triangles.size()));
  if(ranked.size()!=kept)
    throw std::runtime_error(name+": the world cloth bundle leaves "+std::to_string(kept)+" triangles of the cell, "+
                             std::to_string(ranked.size())+" are left of its "+std::to_string(triangles.size()));
  // A GLB holds a cell's triangles by material, each primitive in source order: merge those ascending runs.
  std::vector<size_t> runs {0};
  for(size_t k=1;k<ranked.size();++k)
    if(ranked[k]<ranked[k-1])
      runs.push_back(k);
  runs.push_back(ranked.size());
  while(runs.size()>2) {
    std::vector<size_t> merged {0};
    for(size_t r=0;r+1<runs.size();r+=2) {
      const auto end = r+2<runs.size() ? runs[r+2] : runs[r+1];
      if(r+2<runs.size())
        std::inplace_merge(ranked.begin()+ptrdiff_t(runs[r]),ranked.begin()+ptrdiff_t(runs[r+1]),
                           ranked.begin()+ptrdiff_t(end));
      merged.push_back(end);
      }
    runs = std::move(merged);
    }
  std::vector<uint32_t> out(ranked.size());
  for(size_t k=0;k<ranked.size();++k) {
    if(k>0 && ranked[k]>>32==ranked[k-1]>>32)
      throw std::runtime_error(name+": two triangles take rank "+std::to_string(ranked[k]>>32));
    out[k] = uint32_t(ranked[k]);
    }
  return out;
  }

}

void WorldChunkFile::verify() const {
  if(fs::file_size(path)!=size || fs::last_write_time(path)!=modified)
    throw std::runtime_error("world chunks: chunk changed after world load: "+path.string());
  }

void CookedChunkSource::load(const fs::path& dir, const fs::path& overrideDir, const WorldCellManifest& manifest,
                             std::span<const WorldChunkFile> files, const asset::WorldMesh& mesh) {
  const char* flag = std::getenv("OPENGOTHIC_WCB");
  if(flag!=nullptr && std::string_view(flag)=="0")
    return;
  const auto path = cookedIndexPath(dir,overrideDir);
  std::error_code error;
  if(!fs::is_regular_file(path,error))
    return;
  try {
    auto bytes = std::make_shared<const std::vector<uint8_t>>(gothicglb::readFile(path));
    const std::span<const uint8_t> view(bytes->data(),bytes->size());
    const auto hash = sha256Bytes(view);
    auto read = CookedIndex::open(view,path.string(),std::move(bytes));
    checkCookedIndex(read,manifest);
    const auto& h = read.header();
    const uint64_t loaded[] = {mesh.vertices.size(),mesh.features.size(),mesh.materials.size(),
                               mesh.polygons.material_indices.size()};
    if(h.vertices!=loaded[0] || h.features!=loaded[1] || h.materials!=loaded[2] || h.triangles!=loaded[3])
      throw std::runtime_error("its totals of "+std::to_string(h.vertices)+" vertices, "+std::to_string(h.features)+
                               " features, "+std::to_string(h.materials)+" materials and "+std::to_string(h.triangles)+
                               " triangles differ from the loaded world's "+std::to_string(loaded[0])+", "+
                               std::to_string(loaded[1])+", "+std::to_string(loaded[2])+" and "+std::to_string(loaded[3]));
    if(files.size()!=manifest.cells.size())
      throw std::runtime_error("the world has "+std::to_string(files.size())+" chunk files for "+
                               std::to_string(manifest.cells.size())+" cells");
    // Ids past world.json's counts number edited geometry across the whole set: a cell whose GLB changed can
    // renumber its neighbours' copies, so then no cell may use the index. Without them every id is its source key.
    const bool edited = h.vertices>manifest.vertices || h.features>manifest.features || read.appendedMaterials()>0;
    states = std::make_unique<std::atomic<uint8_t>[]>(files.size());
    size_t stale = 0;
    std::string first;
    for(size_t i=0;i<files.size();++i)
      if(read.cells()[i].glbSize!=files[i].size) {
        const auto lengths = files[i].path.string()+" had "+std::to_string(files[i].size)+
                             " bytes at world load, the index lists the cell with a GLB of "+
                             std::to_string(read.cells()[i].glbSize)+" bytes";
        if(edited)
          throw std::runtime_error(lengths+", and the set's edited ids depend on every cell");
        if(stale==0)
          first = lengths;
        states[i] = Stale;
        ++stale;
        }
    cells.clear();
    for(const auto& cell:manifest.cells)
      cells.push_back(cell.id);
    set = {path,hash,manifest.source,files.size(),std::move(first),dir,overrideDir};
    index = std::move(read);
    configureClusterLod(true);
    Tempest::Log::i("[world-chunks] index ",path.string(),": ",uint64_t(files.size()-stale)," of ",
                    uint64_t(files.size())," cells read from their .wcb when it is there (OPENGOTHIC_WCB=0 turns this off)");
    }
  catch(const std::exception& e) {
    Tempest::Log::e("[world-chunks] index ",path.string()," not used, every cell reads its GLB: ",e.what());
    }
  }

void CookedChunkSource::loadCatalogue(CookedIndex validated, CookedSet loaded, const WorldCellManifest& manifest) {
  states = std::make_unique<std::atomic<uint8_t>[]>(manifest.cells.size());
  cells.clear();
  for(const auto& cell:manifest.cells)
    cells.push_back(cell.id);
  set = std::move(loaded);
  index = std::move(validated);
  configureClusterLod(true);
  }

void CookedChunkSource::configureClusterLod(bool enabled) const {
  const auto* value = std::getenv("OPENGOTHIC_CLUSTER_LOD");
  const std::string_view flag = value!=nullptr ? value : "";
  readHierarchy = flag=="1" || flag=="levels" || (flag!="0" && enabled);
  residentHierarchies.resize(readHierarchy ? cells.size() : 0);
  }

void CookedChunkSource::setResident(size_t cell, bool resident) const {
  if(residentHierarchies.empty())
    return;
  std::lock_guard lock(residencyMutex);
  auto& state = residentHierarchies[cell];
  if(bool(state.generation%2)!=resident) {
    ++state.generation;
    state.chunk.reset();
    }
  }

std::shared_ptr<const CookedChunk> CookedChunkSource::clusterHierarchy(size_t cell) const {
  if(residentHierarchies.empty())
    return {};
  std::lock_guard lock(residencyMutex);
  return residentHierarchies[cell].chunk;
  }

WorldChunkMesh CookedChunkSource::read(size_t cell, std::span<const uint32_t> sources, bool currentFile,
                                       const World& world, const std::function<WorldChunkMesh()>& glb) const {
  return readCell(cell,sources,nullptr,currentFile,world,glb);
  }

WorldChunkMesh CookedChunkSource::readLandscape(size_t cell, std::span<const uint32_t> exclusions, uint32_t kept,
                                                bool currentFile, const World& world,
                                                const std::function<WorldChunkMesh()>& glb) const {
  const Landscape landscape {exclusions,kept};
  return readCell(cell,{},&landscape,currentFile,world,glb);
  }

WorldChunkMesh CookedChunkSource::readCell(size_t cell, std::span<const uint32_t> sources, const Landscape* landscape,
                                           bool currentFile, const World& world,
                                           const std::function<WorldChunkMesh()>& glb) const {
  uint64_t generation = 0;
  if(!residentHierarchies.empty()) {
    std::lock_guard lock(residencyMutex);
    auto& state = residentHierarchies[cell];
    if(currentFile) {
      state.generation += 2;
      state.chunk.reset();
      }
    generation = state.generation;
    }
  const auto retain = [&](std::shared_ptr<const CookedChunk> chunk) {
    if(generation%2==0)
      return;
    std::lock_guard lock(residencyMutex);
    auto& state = residentHierarchies[cell];
    if(state.generation==generation && (!chunk || states[cell].load()!=Glb))
      state.chunk = std::move(chunk);
    };
  if(!currentFile && index) {
    const auto start = Clock::now();
    if(auto mesh = readCooked(cell,sources,landscape,world)) {
      cookedNs += nanoseconds(start);
      ++cookedReads;
      retain(mesh->clusterHierarchy);
      if(mesh->clusterHierarchy) {
        if(!loggedHierarchy.test_and_set()) {
          auto path = world.glb.path;
          path.replace_extension(".wcb");
          const auto graph = mesh->clusterHierarchy->hierarchy();
          Tempest::Log::i("[cluster-lod] read ",path.string()," (v3): ",uint64_t(graph.clusters.size()),
                          " clusters, ",uint64_t(graph.groups.size())," groups");
          }
        }
      return std::move(*mesh);
      }
    }
  retain({});
  const auto start = Clock::now();
  ++glbReads;
  std::optional<WorldChunkMesh> mesh;
  if(world.catalogue!=nullptr && !currentFile && world.catalogue->canRecoverIndexedChunk()) {
    try {
      world.glb.verify();
      const auto materials = std::span<const asset::Material>(world.materials);
      const auto chunk = recoverIndexedChunk(world.glb.path,*index,cell,
                                             materials.first(materials.size()-index->appendedMaterials()));
      const auto name = world.glb.path.filename().string();
      checkCookedCell(chunk,cell,cells[cell],set.source,*index,name);
      mesh = readMesh(chunk,cell,sources,landscape,world,name);
      }
    catch(const SourceError&) {
      throw;
      }
    catch(const std::exception&) {
      // If the recovered cell cannot use the loaded index, fall back to the stitched world.
      }
    }
  if(!mesh)
    mesh = glb();
  glbNs += nanoseconds(start);
  return std::move(*mesh);
  }

WorldChunkMesh CookedChunkSource::readMesh(const CookedChunk& chunk, size_t cell, std::span<const uint32_t> sources,
                                          const Landscape* landscape, const World& world, const std::string& name) const {
  if(landscape!=nullptr) {
    const auto list = landscapeSources(chunk,*index,cell,landscape->exclusions,landscape->kept,name);
    return build(chunk,*index,cell,list,world,name);
    }
  return build(chunk,*index,cell,sources,world,name);
  }

std::optional<WorldChunkMesh> CookedChunkSource::readCooked(size_t cell, std::span<const uint32_t> sources,
                                                            const Landscape* landscape, const World& world) const {
  auto& state = states[cell];
  const auto mode = state.load(std::memory_order_relaxed);
  if(mode==Glb)
    return std::nullopt;
  auto path = world.glb.path;
  path.replace_extension(".wcb");
  std::error_code error;
  if(!fs::is_regular_file(path,error)) {
    state.store(Glb,std::memory_order_relaxed);
    return std::nullopt;
    }
  try {
    const auto& entry = index->cells()[cell];
    if(mode==Stale)
      throw std::runtime_error(world.glb.path.string()+" had "+std::to_string(world.glb.size)+
                               " bytes at world load, the index lists the cell with a GLB of "+
                               std::to_string(entry.glbSize)+" bytes");
    auto chunk = CookedChunk::read(path,readHierarchy);
    const auto name = path.filename().string();
    checkCookedCell(chunk,cell,cells[cell],set.source,*index,name);
    world.glb.verify();
    auto mesh = readMesh(chunk,cell,sources,landscape,world,name);
    if(readHierarchy && chunk.version()==wcb::Version)
      mesh.clusterHierarchy = std::make_shared<const CookedChunk>(std::move(chunk));
    return mesh;
    }
  catch(const SourceError&) {
    throw;
    }
  catch(const std::exception& e) {
    if(state.exchange(Glb)!=Glb)
      Tempest::Log::e("[world-chunks] ",path.string()," not used, the cell reads its GLB: ",e.what());
    return std::nullopt;
    }
  }

ChunkReadCounts ChunkMeshSource::reads() const { return cooked().counts(); }

ChunkReadCounts CookedChunkSource::counts() const {
  ChunkReadCounts out;
  out.cooked   = cookedReads.load();
  out.glb      = glbReads.load();
  out.cookedMs = double(cookedNs.load())/1e6;
  out.glbMs    = double(glbNs.load())/1e6;
  return out;
  }

size_t CookedChunkSource::memoryBytes() const {
  std::lock_guard lock(residencyMutex);
  size_t hierarchyBytes = residentHierarchies.capacity()*sizeof(ResidentHierarchy);
  for(const auto& state:residentHierarchies)
    if(state.chunk)
      hierarchyBytes += sizeof(CookedChunk)+state.chunk->byteSize();
  return (index ? index->byteSize()+index->name().capacity() : 0)+set.source.world.capacity()+
         (set.index.native().capacity()+set.dir.native().capacity()+set.overrideDir.native().capacity())*
         sizeof(fs::path::value_type)+set.stale.capacity()+
         cells.capacity()*sizeof(ChunkId)+(states ? cells.size()*sizeof(std::atomic<uint8_t>) : 0)+hierarchyBytes;
  }
