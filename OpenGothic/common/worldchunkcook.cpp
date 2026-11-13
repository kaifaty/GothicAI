#include "worldchunkcook.h"

#include <algorithm>
#include <bit>
#include <stdexcept>

#include "gothicglb.h"

namespace fs = std::filesystem;

namespace {

constexpr uint32_t Unset = 0xFFFFFFFFu;

[[noreturn]] void fail(const std::string& reason) {
  throw std::runtime_error("cooked world: " + reason);
  }

std::string cellName(ChunkId id) {
  return std::to_string(id.x) + "," + std::to_string(id.z);
  }

bool sameBits(const asset::Vec3& a, const asset::Vec3& b) {
  for(unsigned axis=0;axis<3;++axis)
    if(std::bit_cast<uint32_t>(a[axis])!=std::bit_cast<uint32_t>(b[axis]))
      return false;
  return true;
  }

bool sameFeature(const asset::VertexFeature& a, const asset::VertexFeature& b) {
  return std::bit_cast<uint32_t>(a.texture.x)==std::bit_cast<uint32_t>(b.texture.x) &&
         std::bit_cast<uint32_t>(a.texture.y)==std::bit_cast<uint32_t>(b.texture.y) && a.light==b.light &&
         sameBits(a.normal,b.normal);
  }

}

fs::path chunkGlbPath(const fs::path& dir, const fs::path& overrideDir, const std::string& file) {
  // The same rule as readWorldChunks.
  if(!overrideDir.empty() && fs::exists(overrideDir/file))
    return overrideDir/file;
  return dir/file;
  }

fs::path cookedChunkPath(const fs::path& dir, const fs::path& overrideDir, const std::string& file, const fs::path& outDir) {
  auto path = outDir.empty() ? chunkGlbPath(dir,overrideDir,file) : outDir/file;
  path.replace_extension(".wcb");
  return path;
  }

CookedWorldStats cookWorldChunks(const fs::path& dir, const fs::path& overrideDir, const fs::path& outDir) {
  const auto manifest = readWorldChunkManifest(dir);
  // Each GLB's length and time before the read: the bytes hashed below must be the ones the mesh came from.
  std::vector<std::pair<uintmax_t,fs::file_time_type>> inputs;
  for(const auto& cell:manifest.cells) {
    const auto glb = chunkGlbPath(dir,overrideDir,cell.drawFile());
    inputs.emplace_back(fs::file_size(glb),fs::last_write_time(glb));
    }
  ChunkTable table;
  WorldChunkTrace trace;
  const auto mesh = readWorldChunks(dir,overrideDir,&table,&trace);
  const auto& p = mesh.polygons;
  const size_t n = p.material_indices.size();
  if(table.entries.size()!=manifest.cells.size() || table.triangleChunks.size()!=n ||
     table.triangleSources.size()!=n || trace.orders.size()!=n)
    fail("the stitched mesh of "+dir.string()+" and its chunk table disagree");
  const auto lists = chunkTriangleLists(mesh,table);

  // Stitched index -> index in the current cell's arrays; reset after each cell.
  std::vector<uint32_t> vertexLocal(mesh.vertices.size(),Unset), featureLocal(mesh.features.size(),Unset);
  std::vector<char> materialUsed(mesh.materials.size(),0);
  CookedWorldStats stats;
  for(size_t cell=0;cell<table.entries.size();++cell) {
    const auto& entry = table.entries[cell];
    const auto& ranks = lists[cell];
    std::vector<uint32_t> byOrdinal(ranks.size(),Unset);
    for(const auto rank:ranks) {
      const auto ordinal = table.triangleSources[rank];
      if(ordinal>=ranks.size() || byOrdinal[ordinal]!=Unset)
        fail(entry.file+": its GLB triangle ordinals are not 0.."+std::to_string(ranks.size())+" once each");
      byOrdinal[ordinal] = rank;
      }

    CookedChunkData data;
    data.triangles.reserve(ranks.size());
    std::vector<uint32_t> materials;
    for(const auto rank:byOrdinal) {
      wcb::Triangle t {};
      for(unsigned c=0;c<3;++c) {
        const auto v = p.vertex_indices[size_t(rank)*3+c];
        if(vertexLocal[v]==Unset) {
          vertexLocal[v] = uint32_t(data.positions.size());
          const auto& x = mesh.vertices[v];
          data.positions.push_back({x.x,x.y,x.z,v});
          }
        t.vertex[c] = vertexLocal[v];
        const auto f = p.feature_indices[size_t(rank)*3+c];
        if(featureLocal[f]==Unset) {
          featureLocal[f] = uint32_t(data.features.size());
          const auto& x = mesh.features[f];
          data.features.push_back({x.texture.x,x.texture.y,x.light,x.normal.x,x.normal.y,x.normal.z,f});
          }
        t.feature[c] = featureLocal[f];
        }
      t.material = p.material_indices[rank];
      if(!materialUsed[t.material]) {
        materialUsed[t.material] = 1;
        materials.push_back(t.material);
        }
      const auto order = trace.orders[rank];
      t.order    = order<0 ? wcb::Added : uint32_t(order);
      t.rank     = rank;
      t.lightmap = p.lightmap_indices[rank];
      t.flags    = wcb::packFlags(p.flags[rank]);
      t.sector   = p.flags[rank].sector_index;
      data.triangles.push_back(t);
      }
    for(const auto& v:data.positions)
      vertexLocal[v.id] = Unset;
    for(const auto& f:data.features)
      featureLocal[f.id] = Unset;
    std::sort(materials.begin(),materials.end());
    for(const auto m:materials) {
      materialUsed[m] = 0;
      data.materials.emplace_back(m,mesh.materials[m]);
      }

    const auto glb = chunkGlbPath(dir,overrideDir,entry.file);
    const auto bytes = gothicglb::readFile(glb);
    if(bytes.size()!=inputs[cell].first || fs::file_size(glb)!=inputs[cell].first ||
       fs::last_write_time(glb)!=inputs[cell].second)
      fail(glb.string()+" changed while it was cooked");
    auto& h = data.header;
    h.cell       = entry.id;
    h.min        = entry.min;
    h.max        = entry.max;
    h.glbSize    = bytes.size();
    h.glbSha256  = sha256Bytes(bytes);
    h.worldSize  = manifest.source.size;
    h.worldCrc32 = manifest.source.crc32;
    if(entry.triangleCount!=data.triangles.size())
      fail(entry.file+": the chunk table counts "+std::to_string(entry.triangleCount)+" triangles, the mesh has "+
           std::to_string(data.triangles.size()));
    const auto encoded = encodeCookedChunk(data);
    const auto path = cookedChunkPath(dir,overrideDir,entry.file,outDir);
    fs::create_directories(path.parent_path());
    gothicglb::writeFile(path,encoded.data(),encoded.size());
    ++stats.cells;
    stats.triangles += data.triangles.size();
    stats.glbBytes  += bytes.size();
    stats.wcbBytes  += encoded.size();
    }
  return stats;
  }

void checkCookedCell(const CookedChunk& chunk, const ChunkCell& cell, const WorldCellManifest& manifest,
                     const fs::path& glb) {
  const auto& h = chunk.header();
  const auto name = glb.filename().replace_extension(".wcb").string();
  if(!(h.cell==cell.id))
    fail(name+" was cooked for cell "+cellName(h.cell)+", world.json lists it for cell "+cellName(cell.id));
  if(h.worldSize!=manifest.source.size || h.worldCrc32!=manifest.source.crc32)
    fail(name+" was cooked for another source world than "+manifest.source.world);
  const auto stale = cookedChunkStale(chunk,glb,false);
  if(!stale.empty())
    fail(name+" is stale: "+stale);
  }

CookedWorld readCookedWorld(const fs::path& dir, const fs::path& overrideDir, const fs::path& outDir) {
  CookedWorld world;
  world.dir = dir;
  world.manifest = readWorldChunkManifest(dir);
  world.cells.reserve(world.manifest.cells.size());
  for(const auto& cell:world.manifest.cells) {
    const auto& file = cell.drawFile();
    auto chunk = CookedChunk::read(cookedChunkPath(dir,overrideDir,file,outDir));
    checkCookedCell(chunk,cell,world.manifest,chunkGlbPath(dir,overrideDir,file));
    world.cells.push_back(std::move(chunk));
    }
  return world;
  }

asset::WorldMesh assembleCookedWorld(const CookedWorld& world, ChunkTable* table) {
  const auto& manifest = world.manifest;
  if(world.cells.size()!=manifest.cells.size())
    fail("the cooked world has "+std::to_string(world.cells.size())+" cells, world.json "+
         std::to_string(manifest.cells.size()));
  uint64_t vertices = manifest.vertices, features = manifest.features, triangles = 0;
  size_t materials = manifest.materials.size();
  for(const auto& cell:world.cells) {
    for(const auto& v:cell.positions())
      vertices = std::max(vertices,uint64_t(v.id)+1);
    for(const auto& f:cell.features())
      features = std::max(features,uint64_t(f.id)+1);
    for(const auto& m:cell.materials())
      materials = std::max(materials,size_t(m.id)+1);
    triangles += cell.triangles().size();
    }

  asset::WorldMesh out;
  // Unreferenced IDs first, at the manifest's sizes as readWorldChunks reads them.
  out.vertices.resize(manifest.vertices);
  out.features.resize(manifest.features);
  std::vector<char> vertexSet(out.vertices.size(),0), featureSet(out.features.size(),0);
  readWorldUnreferenced(world.dir,out.vertices,out.features,vertexSet,featureSet);
  out.vertices.resize(vertices);
  out.features.resize(features);
  vertexSet.resize(vertices,0);
  featureSet.resize(features,0);

  out.materials = manifest.materials;
  out.materials.resize(materials);
  std::vector<char> materialSet(materials,0);
  std::fill(materialSet.begin(),materialSet.begin()+ptrdiff_t(manifest.materials.size()),char(1));
  std::vector<char> materialFromCell(materials,0);

  auto& q = out.polygons;
  q.material_indices.resize(triangles);
  q.lightmap_indices.resize(triangles);
  q.flags.resize(triangles);
  q.vertex_indices.resize(triangles*3);
  q.feature_indices.resize(triangles*3);
  std::vector<char> filled(triangles,0);
  ChunkTable result;
  if(table!=nullptr) {
    result.triangleChunks.resize(triangles);
    result.triangleSources.resize(triangles);
    }

  for(size_t ci=0;ci<world.cells.size();++ci) {
    const auto& cell = world.cells[ci];
    const auto& entry = manifest.cells[ci];
    const auto& h = cell.header();
    const auto where = "cell "+cellName(entry.id)+" ("+entry.drawFile()+")";
    if(!(h.cell==entry.id))
      fail(where+": cooked for cell "+cellName(h.cell));
    for(size_t i=0;i<cell.materials().size();++i) {
      const auto id = cell.materials()[i].id;
      const auto record = cell.material(i);
      if(materialSet[id] && !gothicglb::sameMaterial(out.materials[id],record))
        fail(where+": material "+std::to_string(id)+" ("+record.name+") differs from "+
             (materialFromCell[id] ? "another cell's" : "world.json's"));
      out.materials[id] = record;
      materialSet[id] = 1;
      materialFromCell[id] = 1;
      }
    for(const auto& v:cell.positions()) {
      const asset::Vec3 position {v.x,v.y,v.z};
      if(vertexSet[v.id] && !sameBits(out.vertices[v.id],position))
        fail(where+": vertex "+std::to_string(v.id)+" has another position than in another cell or unreferenced.bin");
      out.vertices[v.id] = position;
      vertexSet[v.id] = 1;
      }
    for(const auto& f:cell.features()) {
      asset::VertexFeature feature;
      feature.texture = {f.u,f.v};
      feature.light   = f.light;
      feature.normal  = {f.nx,f.ny,f.nz};
      if(featureSet[f.id] && !sameFeature(out.features[f.id],feature))
        fail(where+": feature "+std::to_string(f.id)+" has other values than in another cell or unreferenced.bin");
      out.features[f.id] = feature;
      featureSet[f.id] = 1;
      }
    const auto positions = cell.positions();
    const auto featureList = cell.features();
    const auto list = cell.triangles();
    for(size_t i=0;i<list.size();++i) {
      const auto& t = list[i];
      if(t.rank>=triangles || filled[t.rank])
        fail(where+": triangle "+std::to_string(i)+" has rank "+std::to_string(t.rank)+
             (t.rank>=triangles ? ", past the world's "+std::to_string(triangles)+" triangles" : ", taken twice"));
      if(t.material>=materials)
        fail(where+": triangle "+std::to_string(i)+" uses material "+std::to_string(t.material)+" of "+
             std::to_string(materials));
      filled[t.rank] = 1;
      const size_t r = t.rank;
      q.material_indices[r] = t.material;
      q.lightmap_indices[r] = t.lightmap;
      q.flags[r] = wcb::unpackFlags(t.flags,t.sector);
      for(unsigned c=0;c<3;++c) {
        q.vertex_indices[r*3+c]  = positions[t.vertex[c]].id;
        q.feature_indices[r*3+c] = featureList[t.feature[c]].id;
        }
      if(table!=nullptr) {
        result.triangleChunks[r]  = uint32_t(ci);
        result.triangleSources[r] = uint32_t(i);
        }
      }
    if(table!=nullptr)
      result.entries.push_back({entry.id,entry.drawFile(),h.min,h.max,h.triangles,entry.meshes});
    }
  // Every rank is in range and taken once, and they add up: all are filled.
  for(size_t m=manifest.materials.size();m<materials;++m)
    if(!materialSet[m])
      fail("material "+std::to_string(m)+" is in no cooked chunk");
  if(table!=nullptr)
    *table = std::move(result);
  return out;
  }

std::string compareChunkTables(const ChunkTable& e, const ChunkTable& a) {
  if(e.entries.size()!=a.entries.size())
    return "entries: "+std::to_string(e.entries.size())+" vs "+std::to_string(a.entries.size());
  for(size_t i=0;i<e.entries.size();++i) {
    const auto& x = e.entries[i];
    const auto& y = a.entries[i];
    const auto where = "entry "+std::to_string(i)+" (cell "+cellName(x.id)+")";
    if(!(x.id==y.id) || x.file!=y.file || !(x.meshes==y.meshes))
      return where+": cell, file or meshes differ";
    if(x.triangleCount!=y.triangleCount)
      return where+": "+std::to_string(x.triangleCount)+" vs "+std::to_string(y.triangleCount)+" triangles";
    if(!sameBits(x.min,y.min) || !sameBits(x.max,y.max))
      return where+": bounds differ";
    }
  if(e.triangleChunks.size()!=a.triangleChunks.size())
    return "triangle ownership: "+std::to_string(e.triangleChunks.size())+" vs "+std::to_string(a.triangleChunks.size());
  for(size_t t=0;t<e.triangleChunks.size();++t)
    if(e.triangleChunks[t]!=a.triangleChunks[t])
      return "triangle "+std::to_string(t)+" is owned by entry "+std::to_string(e.triangleChunks[t])+" vs "+
             std::to_string(a.triangleChunks[t]);
  if(e.triangleSources.size()!=a.triangleSources.size())
    return "GLB ordinals: "+std::to_string(e.triangleSources.size())+" vs "+std::to_string(a.triangleSources.size());
  for(size_t t=0;t<e.triangleSources.size();++t)
    if(e.triangleSources[t]!=a.triangleSources[t])
      return "triangle "+std::to_string(t)+" has GLB ordinal "+std::to_string(e.triangleSources[t])+" vs "+
             std::to_string(a.triangleSources[t]);
  return {};
  }

std::string verifyCookedWorld(const fs::path& dir, const fs::path& overrideDir, const fs::path& outDir, bool hash) {
  ChunkTable expectedTable, actualTable;
  const auto expected = readWorldChunks(dir,overrideDir,&expectedTable);
  const auto world = readCookedWorld(dir,overrideDir,outDir);
  if(hash) {
    for(size_t i=0;i<world.cells.size();++i) {
      const auto& file = world.manifest.cells[i].drawFile();
      const auto stale = cookedChunkStale(world.cells[i],chunkGlbPath(dir,overrideDir,file),true);
      if(!stale.empty())
        return file+": "+stale;
      }
    }
  const auto actual = assembleCookedWorld(world,&actualTable);
  auto difference = compareWorldMeshes(expected,actual);
  if(!difference.empty())
    return "mesh: "+difference;
  difference = compareChunkTables(expectedTable,actualTable);
  if(!difference.empty())
    return "chunk table: "+difference;
  return {};
  }
