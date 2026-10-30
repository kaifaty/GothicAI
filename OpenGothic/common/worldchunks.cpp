#include "worldchunks.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <stdexcept>

#include "gothicglb.h"

namespace {

using namespace gothicglb;

[[noreturn]] void fail(const std::string& reason) {
  throw std::runtime_error("world chunks: " + reason);
  }

constexpr int         formatVersion = 2;
constexpr const char* manifestName  = "world.json";
constexpr const char* chunkFormat   = "gothic-world-chunk";
constexpr const char* worldFormat   = "gothic-world";
constexpr const char* unreferencedName  = "unreferenced.bin";
constexpr char        unreferencedMagic[4] = {'G','W','U','R'};
// Float attributes carry source indices; floats are exact up to 2^24.
constexpr uint32_t    exactIndexLimit = 1u << 24;

// Polygon flags packed into two 16-bit words: bit fields, then sector index.
std::array<uint16_t,2> packFlags(const asset::PolygonFlagSet& f) {
  const unsigned lo = (unsigned(f.is_portal)&3u) | (unsigned(f.is_occluder)<<2) | (unsigned(f.is_sector)<<3) |
                      (unsigned(f.should_relight)<<4) | (unsigned(f.is_outdoor)<<5) |
                      (unsigned(f.is_ghost_occluder)<<6) | (unsigned(f.is_dynamically_lit)<<7) |
                      (unsigned(f.is_lod)<<8) | ((unsigned(f.normal_axis)&3u)<<9);
  return {uint16_t(lo),uint16_t(int16_t(f.sector_index))};
  }

// Feature light is ARGB in one word; glTF COLOR_0 is RGBA bytes.
std::array<uint8_t,4> lightToRgba(uint32_t light) {
  return {uint8_t(light>>16),uint8_t(light>>8),uint8_t(light),uint8_t(light>>24)};
  }

std::string chunkName(int32_t x, int32_t z) {
  return std::to_string(x) + "_" + std::to_string(z);
  }

int32_t chunkCoord(float v, int32_t size) {
  return int32_t(std::floor(double(v)/double(size)));
  }

void checkPolygons(const asset::WorldMesh& mesh) {
  const auto& p = mesh.polygons;
  const size_t n = p.material_indices.size();
  if(p.vertex_indices.size()!=n*3 || p.feature_indices.size()!=n*3 ||
     p.lightmap_indices.size()!=n || p.flags.size()!=n)
    fail("polygon arrays disagree in length");
  for(size_t i=0; i<n; ++i) {
    if(p.material_indices[i]>=mesh.materials.size())
      fail("material index out of range");
    for(unsigned c=0; c<3; ++c) {
      if(p.vertex_indices[i*3+c]>=mesh.vertices.size())
        fail("vertex index out of range");
      if(p.feature_indices[i*3+c]>=mesh.features.size())
        fail("feature index out of range");
      }
    }
  }

}

uint32_t worldChunkCrc32(const void* data, size_t size, uint32_t crc) {
  static const auto table = [] {
    std::array<uint32_t,256> t {};
    for(uint32_t i=0; i<256; ++i) {
      uint32_t c = i;
      for(int k=0; k<8; ++k)
        c = (c&1u) ? 0xEDB88320u ^ (c>>1) : c>>1;
      t[i] = c;
      }
    return t;
    }();
  crc = ~crc;
  const auto* p = static_cast<const uint8_t*>(data);
  for(size_t i=0; i<size; ++i)
    crc = table[(crc ^ p[i]) & 0xFFu] ^ (crc>>8);
  return ~crc;
  }

WorldChunkManifest writeWorldChunks(const asset::WorldMesh& mesh, const WorldChunkSource& source,
                                    const std::filesystem::path& dir, int32_t chunkSize) {
  checkPolygons(mesh);
  if(chunkSize<=0)
    fail("chunk size must be positive");
  const auto& p = mesh.polygons;
  const size_t n = p.material_indices.size();
  if(n>=exactIndexLimit || mesh.vertices.size()>=exactIndexLimit || mesh.features.size()>=exactIndexLimit)
    fail("mesh too large for exact float indices");

  // Triangle -> chunk by centroid in the ground plane (Gothic X and Z).
  std::map<std::pair<int32_t,int32_t>,std::vector<uint32_t>> chunks;
  for(size_t i=0; i<n; ++i) {
    double cx = 0, cz = 0;
    for(unsigned c=0; c<3; ++c) {
      const auto& v = mesh.vertices[p.vertex_indices[i*3+c]];
      cx += v.x;
      cz += v.z;
      }
    chunks[{chunkCoord(float(cx/3.0),chunkSize),chunkCoord(float(cz/3.0),chunkSize)}].push_back(uint32_t(i));
    }

  std::filesystem::create_directories(dir/"chunks");
  WorldChunkManifest manifest;
  manifest.source    = source;
  manifest.chunkSize = chunkSize;
  manifest.triangles = n;
  manifest.vertices  = mesh.vertices.size();
  manifest.features  = mesh.features.size();
  manifest.materials = mesh.materials;

  for(const auto& [coord,tris]:chunks) {
    // Chunk vertex = (source vertex, source feature) pair, first use order.
    std::map<std::pair<uint32_t,uint32_t>,uint32_t> local;
    std::vector<float>   position, normal, uv, vid, fid;
    std::vector<uint8_t> color;
    std::map<uint32_t,std::vector<uint32_t>> byMaterial;
    for(const auto t:tris)
      byMaterial[p.material_indices[t]].push_back(t);

    auto vertexOf = [&](uint32_t v, uint32_t f) {
      auto [it,inserted] = local.emplace(std::make_pair(v,f),uint32_t(local.size()));
      if(inserted) {
        const auto& pv = mesh.vertices[v];
        const auto& pf = mesh.features[f];
        const float gp[3] = {-pv.x,pv.y,pv.z};
        for(unsigned ax=0; ax<3; ++ax) {
          position.push_back(gp[ax]);
          }
        normal.push_back(-pf.normal.x);
        normal.push_back(pf.normal.y);
        normal.push_back(pf.normal.z);
        uv.push_back(pf.texture.x);
        uv.push_back(pf.texture.y);
        for(const auto ch:lightToRgba(pf.light))
          color.push_back(ch);
        vid.push_back(float(v));
        fid.push_back(float(f));
        }
      return it->second;
      };

    // Build indices first: the vertex table is shared by all primitives.
    struct Prim { uint32_t material; std::vector<uint32_t> idx; std::vector<float> order, lightmap; std::vector<uint16_t> flags; };
    std::vector<Prim> prims;
    for(const auto& [mat,list]:byMaterial) {
      Prim pr;
      pr.material = mat;
      for(const auto t:list) {
        for(unsigned c=0; c<3; ++c)
          pr.idx.push_back(vertexOf(p.vertex_indices[t*3+c],p.feature_indices[t*3+c]));
        pr.order.push_back(float(t));
        const auto f = packFlags(p.flags[t]);
        pr.flags.push_back(f[0]);
        pr.flags.push_back(f[1]);
        pr.lightmap.push_back(float(p.lightmap_indices[t]));
        }
      prims.push_back(std::move(pr));
      }

    GlbBuilder glb;
    // POSITION min/max are required by glTF: bounds of the stored values.
    std::string minMax;
    {
    float l[3] = {position[0],position[1],position[2]}, h[3] = {position[0],position[1],position[2]};
    for(size_t i=0; i<position.size(); i+=3)
      for(unsigned ax=0; ax<3; ++ax) {
        l[ax] = std::min(l[ax],position[i+ax]);
        h[ax] = std::max(h[ax],position[i+ax]);
        }
    minMax = ",\"min\":[" + jsonFloat(l[0]) + "," + jsonFloat(l[1]) + "," + jsonFloat(l[2]) +
             "],\"max\":[" + jsonFloat(h[0]) + "," + jsonFloat(h[1]) + "," + jsonFloat(h[2]) + "]";
    }
    const size_t aPos = glb.accessor(position,5126,"VEC3",3,false,34962,minMax);
    const size_t aNrm = glb.accessor(normal,5126,"VEC3",3,false,34962);
    const size_t aUv  = glb.accessor(uv,5126,"VEC2",2,false,34962);
    const size_t aCol = glb.accessor(color,5121,"VEC4",4,true,34962);
    const size_t aVid = glb.accessor(vid,5126,"SCALAR",1,false,34962);
    const size_t aFid = glb.accessor(fid,5126,"SCALAR",1,false,34962);
    const std::string attributes = "{\"POSITION\":" + std::to_string(aPos) + ",\"NORMAL\":" + std::to_string(aNrm) +
                                   ",\"TEXCOORD_0\":" + std::to_string(aUv) + ",\"COLOR_0\":" + std::to_string(aCol) +
                                   ",\"_VERTEX\":" + std::to_string(aVid) + ",\"_FEATURE\":" + std::to_string(aFid) + "}";
    std::string materials = "[", primitives = "[";
    for(size_t i=0; i<prims.size(); ++i) {
      const auto& pr = prims[i];
      const size_t aIdx = glb.accessor(pr.idx,5125,"SCALAR",1,false,34963);
      const size_t aOrd = glb.accessor(pr.order,5126,"SCALAR",1,false,0);
      const size_t aFlg = glb.accessor(pr.flags,5123,"VEC2",2,false,0);
      const size_t aLm  = glb.accessor(pr.lightmap,5126,"SCALAR",1,false,0);
      if(i>0) {
        materials += ",";
        primitives += ",";
        }
      const auto& m = mesh.materials[pr.material];
      materials += "{\"name\":" + jsonString(m.name) + ",\"extras\":{\"gothic\":" + materialJson(m,pr.material) + "}}";
      primitives += "{\"attributes\":" + attributes + ",\"indices\":" + std::to_string(aIdx) +
                    ",\"material\":" + std::to_string(i) + ",\"mode\":4,\"extras\":{\"gothic\":{\"order\":" +
                    std::to_string(aOrd) + ",\"flags\":" + std::to_string(aFlg) + ",\"lightmap\":" + std::to_string(aLm) + "}}}";
      }
    materials += "]";
    primitives += "]";

    const auto name = chunkName(coord.first,coord.second);
    const std::string json =
      "{\"asset\":{\"version\":\"2.0\",\"generator\":\"gothic-ai worldchunks\",\"extras\":{\"gothic\":{\"format\":\"" +
      std::string(chunkFormat) + "\",\"version\":" + std::to_string(formatVersion) + ",\"world\":" + jsonString(source.world) +
      ",\"chunk\":[" + std::to_string(coord.first) + "," + std::to_string(coord.second) + "]}}}," +
      "\"scene\":0,\"scenes\":[{\"nodes\":[0]}]," +
      "\"nodes\":[{\"name\":" + jsonString(name) + ",\"mesh\":0,\"scale\":[0.01,0.01,0.01]}]," +
      "\"meshes\":[{\"name\":" + jsonString(name) + ",\"primitives\":" + primitives + "}]," +
      "\"materials\":" + materials + "," +
      "\"accessors\":" + glb.accessorsJson() + "," +
      "\"bufferViews\":" + glb.viewsJson() + "," +
      "\"buffers\":[{\"byteLength\":" + std::to_string(glb.bin.size()) + "}]}";
    const auto bytes = glb.finish(json);
    const auto file = std::string("chunks/") + name + ".glb";
    writeFile(dir/file,bytes.data(),bytes.size());
    manifest.chunks.push_back(file);
    }

  // Vertices and features no triangle uses (portal and outdoor polygons):
  // kept so every source index is restored as it was.
  {
  std::vector<char> usedV(mesh.vertices.size(),0), usedF(mesh.features.size(),0);
  for(const auto v:p.vertex_indices)
    usedV[v] = 1;
  for(const auto f:p.feature_indices)
    usedF[f] = 1;
  std::vector<uint8_t> bin;
  auto put = [&bin](const void* v, size_t n) {
    const auto* b = static_cast<const uint8_t*>(v);
    bin.insert(bin.end(),b,b+n);
    };
  auto u32 = [&put](uint32_t v) {
    const uint8_t b[4] = {uint8_t(v),uint8_t(v>>8),uint8_t(v>>16),uint8_t(v>>24)};
    put(b,4);
    };
  auto f32 = [&u32](float v) { u32(bits(v)); };
  uint32_t nv = 0, nf = 0;
  for(const auto u:usedV)
    nv += u ? 0u : 1u;
  for(const auto u:usedF)
    nf += u ? 0u : 1u;
  put(unreferencedMagic,4);
  u32(uint32_t(formatVersion));
  u32(nv);
  u32(nf);
  for(size_t i=0; i<mesh.vertices.size(); ++i)
    if(!usedV[i]) {
      u32(uint32_t(i));
      f32(mesh.vertices[i].x);
      f32(mesh.vertices[i].y);
      f32(mesh.vertices[i].z);
      }
  for(size_t i=0; i<mesh.features.size(); ++i)
    if(!usedF[i]) {
      const auto& f = mesh.features[i];
      u32(uint32_t(i));
      f32(f.texture.x);
      f32(f.texture.y);
      u32(f.light);
      f32(f.normal.x);
      f32(f.normal.y);
      f32(f.normal.z);
      }
  writeFile(dir/unreferencedName,bin.data(),bin.size());
  manifest.unreferencedVertices = nv;
  manifest.unreferencedFeatures = nf;
  }

  std::string m = "{\n  \"format\": \"" + std::string(worldFormat) + "\",\n  \"version\": " + std::to_string(formatVersion) +
                  ",\n  \"source\": {\"world\": " + jsonString(source.world) + ", \"size\": " + std::to_string(source.size) +
                  ", \"crc32\": " + std::to_string(source.crc32) + "},\n  \"chunk_size_cm\": " + std::to_string(chunkSize) +
                  ",\n  \"triangles\": " + std::to_string(n) + ",\n  \"vertices\": " + std::to_string(mesh.vertices.size()) +
                  ",\n  \"features\": " + std::to_string(mesh.features.size()) +
                  ",\n  \"unreferenced\": \"" + unreferencedName + "\",\n  \"chunks\": [";
  for(size_t i=0; i<manifest.chunks.size(); ++i)
    m += std::string(i==0 ? "\n    " : ",\n    ") + jsonString(manifest.chunks[i]);
  m += "\n  ],\n  \"materials\": [";
  for(size_t i=0; i<mesh.materials.size(); ++i)
    m += std::string(i==0 ? "\n    " : ",\n    ") + materialJson(mesh.materials[i],i);
  m += "\n  ]\n}\n";
  writeFile(dir/manifestName,m.data(),m.size());
  return manifest;
  }
