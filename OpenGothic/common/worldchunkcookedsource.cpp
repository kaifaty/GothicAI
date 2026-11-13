#include "worldchunkcookedsource.h"

#include "gothicglb.h"
#include "worldchunkcook.h"

#include <Tempest/Log>

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
  const auto materialIds = stitchedIds(chunk.materials(),index.materialExceptions(cell),world.materials.size(),
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
    auto read = CookedIndex::read(path);
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
    states = std::make_unique<std::atomic<uint8_t>[]>(files.size());
    size_t stale = 0;
    for(size_t i=0;i<files.size();++i)
      if(read.cells()[i].glbSize!=files[i].size) {
        states[i] = Stale;
        ++stale;
        }
    cells.clear();
    for(const auto& cell:manifest.cells)
      cells.push_back(cell.id);
    source = manifest.source;
    index  = std::move(read);
    Tempest::Log::i("[world-chunks] index ",path.string(),": ",uint64_t(files.size()-stale)," of ",
                    uint64_t(files.size())," cells read from their .wcb when it is there (OPENGOTHIC_WCB=0 turns this off)");
    }
  catch(const std::exception& e) {
    Tempest::Log::e("[world-chunks] index ",path.string()," not used, every cell reads its GLB: ",e.what());
    }
  }

WorldChunkMesh CookedChunkSource::read(size_t cell, std::span<const uint32_t> sources, bool currentFile,
                                       const World& world, const std::function<WorldChunkMesh()>& glb) const {
  if(!currentFile && index) {
    const auto start = Clock::now();
    if(auto mesh = readCooked(cell,sources,world)) {
      cookedNs += nanoseconds(start);
      ++cookedReads;
      return std::move(*mesh);
      }
    }
  const auto start = Clock::now();
  ++glbReads;
  auto mesh = glb();
  glbNs += nanoseconds(start);
  return mesh;
  }

std::optional<WorldChunkMesh> CookedChunkSource::readCooked(size_t cell, std::span<const uint32_t> sources,
                                                            const World& world) const {
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
    const auto chunk = CookedChunk::read(path);
    const auto name = path.filename().string();
    checkCookedCell(chunk,cell,cells[cell],source,*index,name);
    world.glb.verify();
    return build(chunk,*index,cell,sources,world,name);
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

ChunkReadCounts CookedChunkSource::counts() const {
  ChunkReadCounts out;
  out.cooked   = cookedReads.load();
  out.glb      = glbReads.load();
  out.cookedMs = double(cookedNs.load())/1e6;
  out.glbMs    = double(glbNs.load())/1e6;
  return out;
  }

size_t CookedChunkSource::memoryBytes() const {
  return (index ? index->byteSize()+index->name().capacity() : 0)+source.world.capacity()+
         cells.capacity()*sizeof(ChunkId)+(states ? cells.size()*sizeof(std::atomic<uint8_t>) : 0);
  }
