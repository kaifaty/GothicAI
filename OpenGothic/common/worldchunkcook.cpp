#include "worldchunkcook.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <fstream>
#include <stdexcept>
#include <tuple>

#include "gothicglb.h"

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

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

uint64_t processId() {
#ifdef _WIN32
  return uint64_t(_getpid());
#else
  return uint64_t(getpid());
#endif
  }

// The stitched rank readWorldChunks gives a source triangle of this order: its
// orders are sorted, so the order less the missing orders below it.
uint32_t sourceRank(const std::vector<uint32_t>& missing, uint32_t order) {
  return order-uint32_t(std::lower_bound(missing.begin(),missing.end(),order)-missing.begin());
  }

// Appends the local indices of one cell whose stitched id is not their source key.
wci::Range exceptions(std::vector<wci::Exception>& out, const std::vector<uint32_t>& ids,
                      const std::vector<uint32_t>& sources) {
  wci::Range r {uint32_t(out.size()),0};
  for(size_t local=0;local<ids.size();++local)
    if(ids[local]!=sources[local])
      out.push_back({uint32_t(local),ids[local]});
  r.count = uint32_t(out.size()-r.first);
  return r;
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

fs::path cookedIndexPath(const fs::path& dir, const fs::path& overrideDir, const fs::path& outDir) {
  if(!outDir.empty())
    return outDir/wci::FileName;
  return (overrideDir.empty() ? dir : overrideDir)/wci::FileName;
  }

bool writeCookedFile(const fs::path& path, std::span<const uint8_t> bytes) {
  std::error_code error;
  const auto size = fs::file_size(path,error);
  if(!error && size==bytes.size()) {
    const auto old = gothicglb::readFile(path);
    if(std::equal(old.begin(),old.end(),bytes.begin(),bytes.end()))
      return false;
    }
  // Unique to the process and the call: two cooks into the same files never share a temporary.
  static std::atomic<uint64_t> calls {0};
  auto temporary = path;
  temporary += "."+std::to_string(processId())+"-"+std::to_string(++calls)+".tmp";
  if(!path.parent_path().empty())
    fs::create_directories(path.parent_path());
  {
  std::ofstream file(temporary,std::ios::binary | std::ios::trunc);
  file.write(reinterpret_cast<const char*>(bytes.data()),std::streamsize(bytes.size()));
  file.close();
  if(!file) {
    fs::remove(temporary,error);
    fail("cannot write "+temporary.string());
    }
  }
  fs::rename(temporary,path,error);
  if(error) {
    std::error_code ignored;
    fs::remove(temporary,ignored);
    fail("cannot rename "+temporary.string()+" to "+path.string()+": "+error.message());
    }
  return true;
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
     table.triangleSources.size()!=n || trace.orders.size()!=n || trace.corners.size()!=n*3)
    fail("the stitched mesh of "+dir.string()+" and its chunk table disagree");
  const auto lists = chunkTriangleLists(mesh,table);

  // Source triangles come first, in ascending order: the orders they skip are the missing ones.
  CookedIndexData index;
  uint32_t sourceTriangles = 0;
  for(uint32_t expected=0;sourceTriangles<n && trace.orders[sourceTriangles]>=0;++sourceTriangles) {
    const auto order = uint32_t(trace.orders[sourceTriangles]);
    if(order<expected)
      fail("the source triangles of the stitched mesh are not in ascending order");
    while(expected<order)
      index.missing.push_back(expected++);
    expected = order+1;
    }
  for(size_t rank=sourceTriangles;rank<n;++rank)
    if(trace.orders[rank]>=0)
      fail("a source triangle follows an added one in the stitched mesh");

  const size_t baseMaterials = manifest.materials.size();
  // Stitched index -> index in the current cell's arrays; reset after each cell.
  std::vector<uint32_t> vertexLocal(mesh.vertices.size(),Unset), featureLocal(mesh.features.size(),Unset),
                        materialLocal(mesh.materials.size(),Unset);
  uint32_t addedNext = sourceTriangles;
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

    // The cell's arrays in first use; per local index its stitched id and its source key.
    CookedChunkData data;
    data.triangles.reserve(ranks.size());
    std::vector<uint32_t> vertexIds, vertexSources, featureIds, featureSources, materialIds, materialSources;
    auto local = [&](std::vector<uint32_t>& map, std::vector<uint32_t>& ids, std::vector<uint32_t>& sources,
                     uint32_t id, uint32_t source, size_t sourceLimit, const char* kind) {
      auto& slot = map[id];
      if(slot==Unset) {
        // Below world.json's count a stitched id is its own source key; appended ids are past it.
        if(id<sourceLimit && source!=id)
          fail(entry.file+": "+kind+" "+std::to_string(id)+" carries the source key "+std::to_string(source));
        slot = uint32_t(ids.size());
        ids.push_back(id);
        sources.push_back(source);
        return std::make_pair(slot,true);
        }
      if(sources[slot]!=source)
        fail(entry.file+": "+kind+" "+std::to_string(id)+" carries two source keys");
      return std::make_pair(slot,false);
      };
    uint32_t added = 0;
    for(const auto rank:byOrdinal) {
      wcb::Triangle t {};
      for(unsigned c=0;c<3;++c) {
        const auto [vertexSource,featureSource] = trace.corners[size_t(rank)*3+c];
        const auto v = p.vertex_indices[size_t(rank)*3+c];
        const auto [vl,newVertex] = local(vertexLocal,vertexIds,vertexSources,v,vertexSource,manifest.vertices,"vertex");
        if(newVertex) {
          const auto& x = mesh.vertices[v];
          data.positions.push_back({x.x,x.y,x.z,vertexSource});
          }
        t.vertex[c] = vl;
        const auto f = p.feature_indices[size_t(rank)*3+c];
        const auto [fl,newFeature] = local(featureLocal,featureIds,featureSources,f,featureSource,manifest.features,"feature");
        if(newFeature) {
          const auto& x = mesh.features[f];
          data.features.push_back({x.texture.x,x.texture.y,x.light,x.normal.x,x.normal.y,x.normal.z,featureSource});
          }
        t.feature[c] = fl;
        }
      // A material below world.json's count is its record there (readWorldChunks keeps the index only then).
      const auto m = p.material_indices[rank];
      const auto materialSource = m<baseMaterials ? m : wcb::None;
      const auto [ml,newMaterial] = local(materialLocal,materialIds,materialSources,m,materialSource,baseMaterials,"material");
      if(newMaterial)
        data.materials.emplace_back(materialSource,mesh.materials[m]);
      t.material = ml;
      // The index must place every triangle where readWorldChunks put it.
      const auto order = trace.orders[rank];
      const auto expected = order<0 ? addedNext+added : sourceRank(index.missing,uint32_t(order));
      if(rank!=expected)
        fail(entry.file+": a triangle of rank "+std::to_string(rank)+" would be placed at "+std::to_string(expected));
      if(order<0)
        ++added;
      t.order    = order<0 ? wcb::Added : uint32_t(order);
      t.lightmap = p.lightmap_indices[rank];
      t.flags    = wcb::packFlags(p.flags[rank]);
      t.sector   = p.flags[rank].sector_index;
      data.triangles.push_back(t);
      }
    for(const auto id:vertexIds)
      vertexLocal[id] = Unset;
    for(const auto id:featureIds)
      featureLocal[id] = Unset;
    for(const auto id:materialIds)
      materialLocal[id] = Unset;

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

    wci::Cell ic {};
    ic.x          = entry.id.x;
    ic.z          = entry.id.z;
    ic.glbSize    = h.glbSize;
    std::copy(h.glbSha256.begin(),h.glbSha256.end(),ic.glbSha256);
    ic.addedStart = addedNext;
    ic.vertices   = exceptions(index.vertices,vertexIds,vertexSources);
    ic.features   = exceptions(index.features,featureIds,featureSources);
    ic.materials  = exceptions(index.materials,materialIds,materialSources);
    index.cells.push_back(ic);
    addedNext += added;

    const auto encoded = encodeCookedChunk(data);
    if(writeCookedFile(cookedChunkPath(dir,overrideDir,entry.file,outDir),encoded))
      ++stats.written;
    else
      ++stats.left;
    ++stats.cells;
    stats.triangles += data.triangles.size();
    stats.glbBytes  += bytes.size();
    stats.wcbBytes  += encoded.size();
    }
  if(addedNext!=n)
    fail("the cells hold "+std::to_string(addedNext)+" triangles, the stitched mesh "+std::to_string(n));

  auto& h = index.header;
  h.worldSize       = manifest.source.size;
  h.worldCrc32      = manifest.source.crc32;
  h.vertices        = wcb::count32(mesh.vertices.size(),"vertices");
  h.features        = wcb::count32(mesh.features.size(),"features");
  h.materials       = wcb::count32(mesh.materials.size(),"materials");
  h.triangles       = wcb::count32(n,"triangles");
  h.sourceTriangles = sourceTriangles;
  index.appended.assign(mesh.materials.begin()+ptrdiff_t(baseMaterials),mesh.materials.end());
  const auto encoded = encodeCookedIndex(index);
  stats.indexWritten = writeCookedFile(cookedIndexPath(dir,overrideDir,outDir),encoded);
  stats.indexBytes   = encoded.size();
  return stats;
  }

void checkCookedIndex(const CookedIndex& index, const WorldCellManifest& manifest) {
  const auto& h = index.header();
  const auto& name = index.name();
  if(h.worldSize!=manifest.source.size || h.worldCrc32!=manifest.source.crc32)
    fail(name+" names another source world than "+manifest.source.world);
  if(index.cells().size()!=manifest.cells.size())
    fail(name+" lists "+std::to_string(index.cells().size())+" cells, world.json "+
         std::to_string(manifest.cells.size()));
  for(size_t i=0;i<manifest.cells.size();++i) {
    const auto& c = index.cells()[i];
    if(!(ChunkId{c.x,c.z}==manifest.cells[i].id))
      fail(name+": entry "+std::to_string(i)+" is cell "+cellName({c.x,c.z})+", world.json lists cell "+
           cellName(manifest.cells[i].id)+" there: another cell order");
    }
  if(h.vertices<manifest.vertices || h.features<manifest.features ||
     h.materials!=manifest.materials.size()+index.appendedMaterials())
    fail(name+": the stitched mesh's "+std::to_string(h.vertices)+" vertices, "+std::to_string(h.features)+
         " features and "+std::to_string(h.materials)+" materials do not fit world.json's "+
         std::to_string(manifest.vertices)+", "+std::to_string(manifest.features)+" and "+
         std::to_string(manifest.materials.size())+" with "+std::to_string(index.appendedMaterials())+" appended");
  // An exception names an appended id: below world.json's count a stitched id is its own source key.
  for(size_t i=0;i<index.cells().size();++i) {
    const std::pair<std::span<const wci::Exception>,size_t> lists[] = {
      {index.vertexExceptions(i),manifest.vertices},{index.featureExceptions(i),manifest.features},
      {index.materialExceptions(i),manifest.materials.size()}};
    for(const auto& [list,base]:lists)
      for(const auto& e:list)
        if(e.id<base)
          fail(name+": cell "+cellName(manifest.cells[i].id)+": an exception gives local "+std::to_string(e.local)+
               " the source id "+std::to_string(e.id));
    }
  }

void checkCookedCell(const CookedChunk& chunk, size_t i, const WorldCellManifest& manifest, const CookedIndex& index,
                     const fs::path& glb) {
  const auto& h = chunk.header();
  const auto& cell = manifest.cells.at(i);
  const auto name = glb.filename().replace_extension(".wcb").string();
  if(!(h.cell==cell.id))
    fail(name+" was cooked for cell "+cellName(h.cell)+", world.json lists it for cell "+cellName(cell.id));
  if(h.worldSize!=manifest.source.size || h.worldCrc32!=manifest.source.crc32)
    fail(name+" was cooked for another source world than "+manifest.source.world);
  const auto& entry = index.cells()[i];
  if(entry.glbSize!=h.glbSize || !std::equal(h.glbSha256.begin(),h.glbSha256.end(),entry.glbSha256))
    fail(index.name()+" lists cell "+cellName(cell.id)+" with another GLB hash than "+name+
         ": the cell was re-cooked without the index");
  const std::tuple<std::span<const wci::Exception>,size_t,const char*> lists[] = {
    {index.vertexExceptions(i),chunk.positions().size(),"positions"},
    {index.featureExceptions(i),chunk.features().size(),"features"},
    {index.materialExceptions(i),chunk.materials().size(),"materials"}};
  for(const auto& [list,count,kind]:lists)
    if(!list.empty() && list.back().local>=count)
      fail(index.name()+": cell "+cellName(cell.id)+": an exception points to local "+
           std::to_string(list.back().local)+", past the "+std::to_string(count)+" "+kind+" of "+name);
  if(uint64_t(entry.addedStart)+chunk.added()!=index.addedEnd(i))
    fail(index.name()+": cell "+cellName(cell.id)+" places "+std::to_string(index.addedEnd(i)-entry.addedStart)+
         " added triangles, "+name+" holds "+std::to_string(chunk.added()));
  const auto stale = cookedChunkStale(chunk,glb,false);
  if(!stale.empty())
    fail(name+" is stale: "+stale);
  }

CookedWorld readCookedWorld(const fs::path& dir, const fs::path& overrideDir, const fs::path& outDir) {
  CookedWorld world;
  world.dir = dir;
  world.manifest = readWorldChunkManifest(dir);
  const auto indexPath = cookedIndexPath(dir,overrideDir,outDir);
  if(!fs::exists(indexPath))
    fail("the index "+indexPath.string()+" is missing: cook the world again");
  world.index = CookedIndex::read(indexPath);
  checkCookedIndex(world.index,world.manifest);
  world.cells.reserve(world.manifest.cells.size());
  for(size_t i=0;i<world.manifest.cells.size();++i) {
    const auto& file = world.manifest.cells[i].drawFile();
    auto chunk = CookedChunk::read(cookedChunkPath(dir,overrideDir,file,outDir));
    checkCookedCell(chunk,i,world.manifest,world.index,chunkGlbPath(dir,overrideDir,file));
    world.cells.push_back(std::move(chunk));
    }
  return world;
  }

asset::WorldMesh assembleCookedWorld(const CookedWorld& world, ChunkTable* table) {
  const auto& manifest = world.manifest;
  const auto& index = world.index;
  const auto& totals = index.header();
  if(world.cells.size()!=manifest.cells.size() || index.cells().size()!=manifest.cells.size())
    fail("the cooked world has "+std::to_string(world.cells.size())+" cells and "+
         std::to_string(index.cells().size())+" index entries, world.json "+std::to_string(manifest.cells.size()));
  if(totals.vertices<manifest.vertices || totals.features<manifest.features ||
     totals.materials!=manifest.materials.size()+index.appendedMaterials())
    fail(index.name()+": its totals do not fit world.json");
  const uint64_t triangles = totals.triangles;

  asset::WorldMesh out;
  // Unreferenced IDs first, at the manifest's sizes as readWorldChunks reads them.
  out.vertices.resize(manifest.vertices);
  out.features.resize(manifest.features);
  std::vector<char> vertexSet(out.vertices.size(),0), featureSet(out.features.size(),0);
  readWorldUnreferenced(world.dir,out.vertices,out.features,vertexSet,featureSet);
  out.vertices.resize(totals.vertices);
  out.features.resize(totals.features);
  vertexSet.resize(totals.vertices,0);
  featureSet.resize(totals.features,0);
  out.materials = manifest.materials;
  for(size_t i=0;i<index.appendedMaterials();++i)
    out.materials.push_back(index.appended(i));

  auto& q = out.polygons;
  q.material_indices.resize(triangles);
  q.lightmap_indices.resize(triangles);
  q.flags.resize(triangles);
  q.vertex_indices.resize(triangles*3);
  q.feature_indices.resize(triangles*3);
  std::vector<char> filled(triangles,0);
  uint64_t placed = 0;
  ChunkTable result;
  if(table!=nullptr) {
    result.triangleChunks.resize(triangles);
    result.triangleSources.resize(triangles);
    }

  std::vector<uint32_t> vertexIds, featureIds, materialIds;
  for(size_t ci=0;ci<world.cells.size();++ci) {
    const auto& cell = world.cells[ci];
    const auto& entry = manifest.cells[ci];
    const auto& h = cell.header();
    const auto where = "cell "+cellName(entry.id)+" ("+entry.drawFile()+")";
    if(!(h.cell==entry.id))
      fail(where+": cooked for cell "+cellName(h.cell));
    // Local index -> stitched id: the exception's, else the source key.
    auto resolve = [&](std::vector<uint32_t>& ids, std::span<const wci::Exception> exceptions, size_t count,
                       auto&& source, uint64_t limit, const char* kind) {
      ids.resize(count);
      for(size_t i=0;i<count;++i) {
        const auto id = CookedIndex::resolve(exceptions,uint32_t(i),source(i));
        if(id>=limit)
          fail(where+": "+kind+" "+std::to_string(i)+(id==wcb::None ? " has neither a source key nor an exception" :
               " has the stitched id "+std::to_string(id)+", past the "+std::to_string(limit)+" of the index"));
        ids[i] = id;
        }
      };
    resolve(vertexIds,index.vertexExceptions(ci),cell.positions().size(),
            [&](size_t i) { return cell.positions()[i].source; },totals.vertices,"position");
    resolve(featureIds,index.featureExceptions(ci),cell.features().size(),
            [&](size_t i) { return cell.features()[i].source; },totals.features,"feature");
    resolve(materialIds,index.materialExceptions(ci),cell.materials().size(),
            [&](size_t i) { return cell.materials()[i].source; },totals.materials,"material");

    for(size_t i=0;i<cell.materials().size();++i) {
      const auto id = materialIds[i];
      const auto record = cell.material(i);
      if(!gothicglb::sameMaterial(out.materials[id],record))
        fail(where+": material "+std::to_string(i)+" ("+record.name+") differs from "+
             (id<manifest.materials.size() ? "world.json's" : "the index's")+" material "+std::to_string(id));
      }
    for(size_t i=0;i<cell.positions().size();++i) {
      const auto& v = cell.positions()[i];
      const auto id = vertexIds[i];
      const asset::Vec3 position {v.x,v.y,v.z};
      if(vertexSet[id] && !sameBits(out.vertices[id],position))
        fail(where+": vertex "+std::to_string(id)+" has another position than in another cell or unreferenced.bin");
      out.vertices[id] = position;
      vertexSet[id] = 1;
      }
    for(size_t i=0;i<cell.features().size();++i) {
      const auto& f = cell.features()[i];
      const auto id = featureIds[i];
      asset::VertexFeature feature;
      feature.texture = {f.u,f.v};
      feature.light   = f.light;
      feature.normal  = {f.nx,f.ny,f.nz};
      if(featureSet[id] && !sameFeature(out.features[id],feature))
        fail(where+": feature "+std::to_string(id)+" has other values than in another cell or unreferenced.bin");
      out.features[id] = feature;
      featureSet[id] = 1;
      }

    const auto list = cell.triangles();
    uint64_t added = index.cells()[ci].addedStart;
    for(size_t i=0;i<list.size();++i) {
      const auto& t = list[i];
      const uint64_t rank = t.order==wcb::Added ? added++ : index.sourceRank(t.order);
      if(rank>=triangles || filled[rank])
        fail(where+": triangle "+std::to_string(i)+" of order "+std::to_string(t.order)+
             (rank>=triangles ? " has no place in the index" : " takes rank "+std::to_string(rank)+" twice"));
      filled[rank] = 1;
      ++placed;
      const size_t r = rank;
      q.material_indices[r] = materialIds[t.material];
      q.lightmap_indices[r] = t.lightmap;
      q.flags[r] = wcb::unpackFlags(t.flags,t.sector);
      for(unsigned c=0;c<3;++c) {
        q.vertex_indices[r*3+c]  = vertexIds[t.vertex[c]];
        q.feature_indices[r*3+c] = featureIds[t.feature[c]];
        }
      if(table!=nullptr) {
        result.triangleChunks[r]  = uint32_t(ci);
        result.triangleSources[r] = uint32_t(i);
        }
      }
    if(added!=index.addedEnd(ci))
      fail(where+": the index places "+std::to_string(index.addedEnd(ci)-index.cells()[ci].addedStart)+
           " added triangles, the cell holds "+std::to_string(cell.added()));
    if(table!=nullptr)
      result.entries.push_back({entry.id,entry.drawFile(),h.min,h.max,h.triangles,entry.meshes});
    }
  // Every rank is in range and taken once: when they add up, all are filled.
  if(placed!=triangles)
    fail("the cells hold "+std::to_string(placed)+" triangles, the index "+std::to_string(triangles));
  // Geometry past world.json's counts exists only because some cell uses it.
  for(size_t v=manifest.vertices;v<vertexSet.size();++v)
    if(!vertexSet[v])
      fail("vertex "+std::to_string(v)+" of the index is in no cooked chunk");
  for(size_t f=manifest.features;f<featureSet.size();++f)
    if(!featureSet[f])
      fail("feature "+std::to_string(f)+" of the index is in no cooked chunk");
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
