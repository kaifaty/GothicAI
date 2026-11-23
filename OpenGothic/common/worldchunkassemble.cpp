// assembleCookedWorld (worldchunkcook.h): a stitched mesh from the index, the cooked chunks, world.json and
// unreferenced.bin alone. The converter's checks and the tests take the whole mesh with its chunk table; static
// collision takes its slim input with the world cloth bundle's triangles left out (physics/worldcollisionmesh.h).

#include "worldchunkcook.h"

#include "gothicglb.h"

#include <bit>
#include <stdexcept>

namespace {

[[noreturn]] void fail(const std::string& reason) {
  throw std::runtime_error("cooked world: " + reason);
  }

std::string cellName(ChunkId id) {
  return std::to_string(id.x)+","+std::to_string(id.z);
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

// What a rank holds while the cells are placed.
enum Rank : char { Empty = 0, Placed = 1, Left = 2 };

}

asset::WorldMesh assembleCookedWorld(const CookedWorld& world, ChunkTable* table) {
  return assembleCookedWorld(world,table,CookedParts{});
  }

asset::WorldMesh assembleCookedWorld(const CookedWorld& world, ChunkTable* table, const CookedParts& parts) {
  const auto& manifest = world.manifest;
  const auto& index = world.index;
  const auto& totals = index.header();
  const bool full = !parts.collision;
  if(world.cells.size()!=manifest.cells.size() || index.cells().size()!=manifest.cells.size())
    fail("the cooked world has "+std::to_string(world.cells.size())+" cells and "+
         std::to_string(index.cells().size())+" index entries, world.json "+std::to_string(manifest.cells.size()));
  if(totals.vertices<manifest.vertices || totals.features<manifest.features ||
     totals.materials!=manifest.materials.size()+index.appendedMaterials())
    fail(index.name()+": its totals do not fit world.json");
  if(!parts.exclusions.empty() && (table!=nullptr || parts.exclusions.size()!=world.cells.size()))
    fail(table!=nullptr ? std::string("triangles are left out only without a chunk table") :
         std::to_string(parts.exclusions.size())+" lists of left out triangles for "+
         std::to_string(world.cells.size())+" cells");
  const uint64_t triangles = totals.triangles;

  asset::WorldMesh out;
  // Unreferenced IDs first, at the manifest's sizes as readWorldChunks reads them; without features they are read past.
  out.vertices.resize(manifest.vertices);
  std::vector<char> vertexSet(out.vertices.size(),0), featureSet;
  if(full) {
    out.features.resize(manifest.features);
    featureSet.resize(out.features.size(),0);
    }
  readWorldUnreferenced(world.dir,out.vertices,out.features,vertexSet,featureSet,full);
  out.vertices.resize(totals.vertices);
  vertexSet.resize(totals.vertices,0);
  if(full) {
    out.features.resize(totals.features);
    featureSet.resize(totals.features,0);
    }
  out.materials = manifest.materials;
  for(size_t i=0;i<index.appendedMaterials();++i)
    out.materials.push_back(index.appended(i));

  auto& q = out.polygons;
  q.material_indices.resize(triangles);
  q.vertex_indices.resize(triangles*3);
  if(full) {
    q.lightmap_indices.resize(triangles);
    q.flags.resize(triangles);
    q.feature_indices.resize(triangles*3);
    }
  std::vector<char> filled(triangles,Empty);
  uint64_t placed = 0, left = 0;
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
    if(full)
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
    for(size_t i=0;full && i<cell.features().size();++i) {
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
    const auto leave = parts.exclusions.empty() ? std::span<const uint32_t>() : parts.exclusions[ci];
    size_t next = 0;
    uint64_t added = index.cells()[ci].addedStart;
    for(size_t i=0;i<list.size();++i) {
      const auto& t = list[i];
      const uint64_t rank = t.order==wcb::Added ? added++ : index.sourceRank(t.order);
      if(rank>=triangles || filled[rank]!=Empty)
        fail(where+": triangle "+std::to_string(i)+" of order "+std::to_string(t.order)+
             (rank>=triangles ? " has no place in the index" : " takes rank "+std::to_string(rank)+" twice"));
      ++placed;
      if(next<leave.size() && leave[next]==i) {
        // Its rank stays taken, so the others keep their stitched order when the left out ones go.
        filled[rank] = Left;
        ++next;
        ++left;
        continue;
        }
      filled[rank] = Placed;
      const size_t r = rank;
      q.material_indices[r] = materialIds[t.material];
      for(unsigned c=0;c<3;++c)
        q.vertex_indices[r*3+c] = vertexIds[t.vertex[c]];
      if(full) {
        q.lightmap_indices[r] = t.lightmap;
        q.flags[r] = wcb::unpackFlags(t.flags,t.sector);
        for(unsigned c=0;c<3;++c)
          q.feature_indices[r*3+c] = featureIds[t.feature[c]];
        }
      if(table!=nullptr) {
        result.triangleChunks[r]  = uint32_t(ci);
        result.triangleSources[r] = uint32_t(i);
        }
      }
    if(next!=leave.size())
      fail(where+": triangle "+std::to_string(leave[next])+" is to be left out, the cell holds "+
           std::to_string(list.size())+" in ascending order");
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
  if(left>0) {
    // The kept triangles move up in rank order, as retainWorldTriangles keeps them.
    size_t kept = 0;
    for(size_t r=0;r<triangles;++r) {
      if(filled[r]==Left)
        continue;
      q.material_indices[kept] = q.material_indices[r];
      for(unsigned c=0;c<3;++c)
        q.vertex_indices[kept*3+c] = q.vertex_indices[r*3+c];
      if(full) {
        q.lightmap_indices[kept] = q.lightmap_indices[r];
        q.flags[kept] = q.flags[r];
        for(unsigned c=0;c<3;++c)
          q.feature_indices[kept*3+c] = q.feature_indices[r*3+c];
        }
      ++kept;
      }
    q.material_indices.resize(kept);
    q.vertex_indices.resize(kept*3);
    if(full) {
      q.lightmap_indices.resize(kept);
      q.flags.resize(kept);
      q.feature_indices.resize(kept*3);
      }
    }
  if(table!=nullptr)
    *table = std::move(result);
  return out;
  }
