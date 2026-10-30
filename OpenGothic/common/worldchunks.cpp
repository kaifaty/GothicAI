#include "worldchunks.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <unordered_map>

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
constexpr float       baseScale = 0.01f;

// Polygon flags packed into two 16-bit words: bit fields, then sector index.
std::array<uint16_t,2> packFlags(const asset::PolygonFlagSet& f) {
  const unsigned lo = (unsigned(f.is_portal)&3u) | (unsigned(f.is_occluder)<<2) | (unsigned(f.is_sector)<<3) |
                      (unsigned(f.should_relight)<<4) | (unsigned(f.is_outdoor)<<5) |
                      (unsigned(f.is_ghost_occluder)<<6) | (unsigned(f.is_dynamically_lit)<<7) |
                      (unsigned(f.is_lod)<<8) | ((unsigned(f.normal_axis)&3u)<<9);
  return {uint16_t(lo),uint16_t(int16_t(f.sector_index))};
  }

asset::PolygonFlagSet unpackFlags(uint16_t lo, uint16_t hi) {
  asset::PolygonFlagSet f {};
  f.is_portal          = uint8_t(lo&3u);
  f.is_occluder        = uint8_t((lo>>2)&1u);
  f.is_sector          = uint8_t((lo>>3)&1u);
  f.should_relight     = uint8_t((lo>>4)&1u);
  f.is_outdoor         = uint8_t((lo>>5)&1u);
  f.is_ghost_occluder  = uint8_t((lo>>6)&1u);
  f.is_dynamically_lit = uint8_t((lo>>7)&1u);
  f.is_lod             = uint8_t((lo>>8)&1u);
  f.normal_axis        = uint8_t((lo>>9)&3u);
  f.sector_index       = int16_t(hi);
  return f;
  }

// Feature light is ARGB in one word; glTF COLOR_0 is RGBA bytes.
std::array<uint8_t,4> lightToRgba(uint32_t light) {
  return {uint8_t(light>>16),uint8_t(light>>8),uint8_t(light),uint8_t(light>>24)};
  }

uint32_t rgbaToLight(const uint8_t* c) {
  return (uint32_t(c[3])<<24) | (uint32_t(c[0])<<16) | (uint32_t(c[1])<<8) | uint32_t(c[2]);
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

// Vertex and feature identity across chunks: the source index carried in
// _VERTEX/_FEATURE plus the exact values, so an edit that moves one copy of
// a shared vertex splits it instead of moving the other chunk's copy.
struct VertexKey {
  int64_t  id;
  uint32_t x,y,z;
  bool operator<(const VertexKey& o) const { return std::tie(id,x,y,z)<std::tie(o.id,o.x,o.y,o.z); }
  };

struct FeatureKey {
  int64_t  id;
  uint32_t u,v,light,nx,ny,nz;
  bool operator<(const FeatureKey& o) const {
    return std::tie(id,u,v,light,nx,ny,nz)<std::tie(o.id,o.u,o.v,o.light,o.nx,o.ny,o.nz);
    }
  };

struct Corner {
  VertexKey         vertex;
  zenkit::Vec3      position;
  FeatureKey        feature;
  asset::VertexFeature value;
  };

struct Triangle {
  int64_t                 order;     // source triangle index, or -1 for edited geometry
  size_t                  material;  // index into the reader's material list
  std::array<Corner,3>    corners;
  int32_t                 lightmap;
  asset::PolygonFlagSet  flags;
  uint32_t                chunk;     // manifest index, retained through source-order stitching
  };

struct ReadMaterial {
  int64_t          index; // source material index, or -1
  asset::Material material;
  };

class ChunkReader {
  public:
    std::vector<ReadMaterial> materials;
    std::vector<Triangle>     triangles;

    void read(const std::filesystem::path& path) {
      auto glb = parseGlb(path);
      const auto& d = *glb.data;
      std::vector<const cgltf_node*> pending;
      for(size_t i=0; i<d.scene->nodes_count; ++i)
        pending.push_back(d.scene->nodes[i]);
      while(!pending.empty()) {
        const auto* node = pending.back();
        pending.pop_back();
        if(node->mesh!=nullptr)
          readNode(d,*node);
        for(size_t i=0; i<node->children_count; ++i)
          pending.push_back(node->children[i]);
        }
      ++chunk;
      }

  private:
    size_t  chunk = 0;
    std::map<int64_t,size_t>     byIndex;
    std::map<std::string,size_t> byRecord;

    size_t material(const cgltf_data& d, const cgltf_material* m) {
      if(m==nullptr)
        fail("primitive without material");
      const auto extras = parseExtras(d,m->extras);
      if(extras==nullptr)
        fail(std::string("material ") + (m->name ? m->name : "?") + " has no extras.gothic");
      const auto& g = (*extras)["gothic"];
      auto rec = materialFromJson(g);
      if(g.HasMember("index")) {
        const auto index = jsonToInt(g["index"],"index");
        auto found = byIndex.find(index);
        if(found!=byIndex.end()) {
          if(!sameMaterial(materials[found->second].material,rec))
            fail("material index " + std::to_string(index) + " differs between chunks");
          return found->second;
          }
        materials.push_back({index,std::move(rec)});
        byIndex.emplace(index,materials.size()-1);
        return materials.size()-1;
        }
      const auto key = materialJson(rec,0);
      auto found = byRecord.find(key);
      if(found!=byRecord.end())
        return found->second;
      materials.push_back({-1,std::move(rec)});
      byRecord.emplace(key,materials.size()-1);
      return materials.size()-1;
      }

    void readNode(const cgltf_data& d, const cgltf_node& node) {
      float m[16];
      cgltf_node_transform_world(&node,m);
      // The converter writes centimetres under a 0.01 node scale: read them
      // without arithmetic so the round trip is exact. Anything else (an edit
      // exported from Blender) goes through the full transform.
      const bool raw = m[0]==baseScale && m[5]==baseScale && m[10]==baseScale && m[15]==1.f &&
                       m[1]==0 && m[2]==0 && m[3]==0 && m[4]==0 && m[6]==0 && m[7]==0 &&
                       m[8]==0 && m[9]==0 && m[11]==0 && m[12]==0 && m[13]==0 && m[14]==0;
      double cof[9];
      const double a=m[0], b=m[4], c=m[8], dd=m[1], e=m[5], f=m[9], g=m[2], h=m[6], k=m[10];
      cof[0]=e*k-f*h; cof[1]=f*g-dd*k; cof[2]=dd*h-e*g;
      cof[3]=c*h-b*k; cof[4]=a*k-c*g;  cof[5]=b*g-a*h;
      cof[6]=b*f-c*e; cof[7]=c*dd-a*f; cof[8]=a*e-b*dd;
      const double det = a*cof[0]+b*cof[1]+c*cof[2];
      if(!std::isfinite(det) || det==0)
        fail("singular node transform");

      const auto& mesh = *node.mesh;
      for(size_t pi=0; pi<mesh.primitives_count; ++pi) {
        const auto& p = mesh.primitives[pi];
        if(p.type!=cgltf_primitive_type_triangles)
          fail("only triangle primitives are supported");
        const auto* pos = findAttribute(p,cgltf_attribute_type_position);
        const auto* nrm = findAttribute(p,cgltf_attribute_type_normal);
        if(pos==nullptr || nrm==nullptr)
          fail("POSITION and NORMAL are required");
        const auto* uv    = findAttribute(p,cgltf_attribute_type_texcoord);
        const auto* col   = findAttribute(p,cgltf_attribute_type_color);
        const auto* vid   = findAttribute(p,cgltf_attribute_type_custom,"_VERTEX");
        const auto* fid   = findAttribute(p,cgltf_attribute_type_custom,"_FEATURE");
        const size_t count = p.indices!=nullptr ? p.indices->count : pos->count;
        if(count%3!=0)
          fail("triangle index count is not a multiple of 3");
        const size_t tris = count/3;
        const size_t mat  = material(d,p.material);
        const auto extras = parseExtras(d,p.extras);
        const auto* order = extrasAccessor(d,extras.get(),"order",tris);
        const auto* flags = extrasAccessor(d,extras.get(),"flags",tris);
        const auto* light = extrasAccessor(d,extras.get(),"lightmap",tris);

        auto corner = [&](size_t i) {
          Corner r {};
          float v[3], n[3], t[2] = {0,0};
          if(!cgltf_accessor_read_float(pos,i,v,3) || !cgltf_accessor_read_float(nrm,i,n,3))
            fail("cannot read POSITION or NORMAL");
          if(uv!=nullptr && !cgltf_accessor_read_float(uv,i,t,2))
            fail("cannot read TEXCOORD_0");
          if(raw) {
            r.position = {-v[0],v[1],v[2]};
            r.value.normal = {-n[0],n[1],n[2]};
            } else {
            for(unsigned ax=0; ax<3; ++ax) {
              const double w = double(m[ax])*v[0] + double(m[ax+4])*v[1] + double(m[ax+8])*v[2] + m[ax+12];
              r.position[ax] = float(w * (ax==0 ? -100.0 : 100.0));
              }
            double tn[3];
            for(unsigned ax=0; ax<3; ++ax)
              tn[ax] = (cof[ax*3]*n[0] + cof[ax*3+1]*n[1] + cof[ax*3+2]*n[2])/det;
            const double len = std::hypot(tn[0],tn[1],tn[2]);
            if(!std::isfinite(len) || len==0)
              fail("NORMAL must be finite and non-zero");
            r.value.normal = {float(-tn[0]/len),float(tn[1]/len),float(tn[2]/len)};
            }
          r.value.texture = {t[0],t[1]};
          r.value.light = 0xFFFFFFFFu;
          if(col!=nullptr) {
            if(col->component_type==cgltf_component_type_r_8u && col->type==cgltf_type_vec4 && col->normalized) {
              uint8_t rgba[4];
              cgltf_uint u[4];
              if(!cgltf_accessor_read_uint(col,i,u,4))
                fail("cannot read COLOR_0");
              for(unsigned ch=0; ch<4; ++ch)
                rgba[ch] = uint8_t(u[ch]);
              r.value.light = rgbaToLight(rgba);
              } else {
              float cf[4] = {1,1,1,1};
              const size_t comps = col->type==cgltf_type_vec3 ? 3 : 4;
              if(!cgltf_accessor_read_float(col,i,cf,comps))
                fail("cannot read COLOR_0");
              uint8_t rgba[4];
              for(unsigned ch=0; ch<4; ++ch)
                rgba[ch] = uint8_t(std::lround(std::clamp(double(cf[ch]),0.0,1.0)*255.0));
              r.value.light = rgbaToLight(rgba);
              }
            }
          auto sourceId = [&](const cgltf_accessor* a) -> int64_t {
            if(a==nullptr)
              return -1;
            float id;
            if(!cgltf_accessor_read_float(a,i,&id,1) || !std::isfinite(id) || id>=float(exactIndexLimit) || id!=std::floor(id))
              fail("invalid _VERTEX/_FEATURE value");
            // A negative id marks geometry an edit added (Blender scripts write -1).
            if(id<0)
              return -1;
            return int64_t(id);
            };
          // Edited geometry has no source id (-1): it never merges with a source
          // vertex, and copies the exporter split by normal or UV merge again
          // by their exact values.
          r.vertex  = {sourceId(vid),bits(r.position.x),bits(r.position.y),bits(r.position.z)};
          r.feature = {sourceId(fid),bits(r.value.texture.x),bits(r.value.texture.y),r.value.light,
                       bits(r.value.normal.x),bits(r.value.normal.y),bits(r.value.normal.z)};
          return r;
          };

        for(size_t t=0; t<tris; ++t) {
          Triangle tri {};
          tri.material = mat;
          tri.order    = -1;
          tri.lightmap = -1;
          for(unsigned c=0; c<3; ++c) {
            const size_t i = p.indices!=nullptr ? cgltf_accessor_read_index(p.indices,t*3+c) : t*3+c;
            if(i>=pos->count)
              fail("triangle index outside POSITION");
            tri.corners[c] = corner(i);
            }
          if(!raw && det<0)
            std::swap(tri.corners[1],tri.corners[2]);
          if(order!=nullptr) {
            float o;
            if(!cgltf_accessor_read_float(order,t,&o,1) || !(o>=0) || o>=float(exactIndexLimit) || o!=std::floor(o))
              fail("invalid order value");
            tri.order = int64_t(o);
            }
          if(flags!=nullptr) {
            cgltf_uint f[2];
            if(!cgltf_accessor_read_uint(flags,t,f,2))
              fail("cannot read flags");
            tri.flags = unpackFlags(uint16_t(f[0]),uint16_t(f[1]));
            }
          if(light!=nullptr) {
            float l;
            if(!cgltf_accessor_read_float(light,t,&l,1) || l!=std::floor(l))
              fail("invalid lightmap value");
            tri.lightmap = int32_t(l);
            }
          tri.chunk=uint32_t(chunk);
          triangles.push_back(tri);
          }
        }
      }
  };

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

asset::WorldMesh canonicalWorldMesh(const asset::WorldMesh& mesh) {
  checkPolygons(mesh);
  asset::WorldMesh out;
  out.materials = mesh.materials;
  out.vertices  = mesh.vertices;
  out.features  = mesh.features;
  out.polygons  = mesh.polygons;
  return out;
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

WorldChunkManifest readWorldChunkManifest(const std::filesystem::path& dir) {
  const auto bytes = readFile(dir/manifestName);
  rapidjson::Document doc;
  doc.Parse<rapidjson::kParseFullPrecisionFlag>(reinterpret_cast<const char*>(bytes.data()),bytes.size());
  if(doc.HasParseError() || !doc.IsObject())
    fail("invalid " + (dir/manifestName).string());
  if(jsonToString(member(doc,"format"),"format")!=worldFormat)
    fail("not a converted world: " + dir.string());
  if(jsonToInt(member(doc,"version"),"version")!=formatVersion)
    fail("unsupported world format version in " + dir.string());
  WorldChunkManifest m;
  const auto& s = member(doc,"source");
  m.source.world = jsonToString(member(s,"world"),"world");
  m.source.size  = uint64_t(jsonToInt(member(s,"size"),"size"));
  m.source.crc32 = uint32_t(jsonToInt(member(s,"crc32"),"crc32"));
  m.chunkSize    = int32_t(jsonToInt(member(doc,"chunk_size_cm"),"chunk_size_cm"));
  m.triangles    = uint64_t(jsonToInt(member(doc,"triangles"),"triangles"));
  const auto& c  = member(doc,"chunks");
  if(!c.IsArray())
    fail("chunks must be an array");
  for(const auto& v:c.GetArray()) {
    auto name = jsonToString(v,"chunks");
    const std::filesystem::path rel(name);
    if(rel.is_absolute() || rel.empty())
      fail("chunk path must be relative: " + name);
    for(const auto& part:rel)
      if(part=="..")
        fail("chunk path must stay inside the world directory: " + name);
    m.chunks.push_back(std::move(name));
    }
  m.vertices = uint64_t(jsonToInt(member(doc,"vertices"),"vertices"));
  m.features = uint64_t(jsonToInt(member(doc,"features"),"features"));
  if(jsonToString(member(doc,"unreferenced"),"unreferenced")!=unreferencedName)
    fail("unexpected unreferenced file name");
  const auto& mats = member(doc,"materials");
  if(!mats.IsArray())
    fail("materials must be an array");
  for(rapidjson::SizeType i=0; i<mats.Size(); ++i) {
    if(jsonToInt(member(mats[i],"index"),"index")!=int64_t(i))
      fail("materials must be listed in source order");
    m.materials.push_back(materialFromJson(mats[i]));
    }
  return m;
  }

asset::WorldMesh readWorldChunks(const std::filesystem::path& dir, const std::filesystem::path& overrideDir,
                                 ChunkTable* chunks) {
  const auto manifest = readWorldChunkManifest(dir);
  ChunkTable table;
  if(chunks!=nullptr) {
    if(manifest.chunkSize!=ChunkId::Size)
      fail("runtime chunks require a 64 m grid");
    for(const auto& file:manifest.chunks) {
      const auto name=std::filesystem::path(file).stem().string();
      const auto separator=name.find('_');
      ChunkId id;
      if(separator==std::string::npos)
        fail("invalid runtime chunk name: "+file);
      const auto x=std::from_chars(name.data(),name.data()+separator,id.x);
      const auto z=std::from_chars(name.data()+separator+1,name.data()+name.size(),id.z);
      if(x.ec!=std::errc{} || x.ptr!=name.data()+separator ||
         z.ec!=std::errc{} || z.ptr!=name.data()+name.size())
        fail("invalid runtime chunk name: "+file);
      table.entries.push_back({id,file});
      }
    }
  ChunkReader reader;
  for(const auto& chunk:manifest.chunks) {
    auto path = dir/chunk;
    if(!overrideDir.empty() && std::filesystem::exists(overrideDir/chunk))
      path = overrideDir/chunk;
    reader.read(path);
    }

  // Source triangles in source order, then edited ones in chunk order.
  std::vector<size_t> ordered, edited;
  std::set<int64_t> seen;
  for(size_t i=0; i<reader.triangles.size(); ++i) {
    const auto o = reader.triangles[i].order;
    if(o<0) {
      edited.push_back(i);
      continue;
      }
    if(!seen.insert(o).second)
      fail("triangle order " + std::to_string(o) + " appears twice");
    ordered.push_back(i);
    }
  std::stable_sort(ordered.begin(),ordered.end(),[&](size_t a, size_t b) {
    return reader.triangles[a].order<reader.triangles[b].order;
    });
  ordered.insert(ordered.end(),edited.begin(),edited.end());

  asset::WorldMesh out;
  out.materials = manifest.materials;
  out.vertices.resize(manifest.vertices);
  out.features.resize(manifest.features);
  std::vector<char> vertexSet(out.vertices.size(),0), featureSet(out.features.size(),0);

  // Vertices and features no triangle uses come from unreferenced.bin.
  {
  const auto bin = readFile(dir/unreferencedName);
  size_t o = 0;
  auto u32 = [&]() {
    if(o+4>bin.size())
      fail("unreferenced.bin is truncated");
    const uint32_t v = uint32_t(bin[o]) | (uint32_t(bin[o+1])<<8) | (uint32_t(bin[o+2])<<16) | (uint32_t(bin[o+3])<<24);
    o += 4;
    return v;
    };
  auto f32 = [&]() {
    const uint32_t v = u32();
    float f;
    std::memcpy(&f,&v,sizeof(f));
    return f;
    };
  if(bin.size()<4 || std::memcmp(bin.data(),unreferencedMagic,4)!=0)
    fail("unreferenced.bin has a wrong header");
  o = 4;
  if(u32()!=uint32_t(formatVersion))
    fail("unreferenced.bin has another version");
  const uint32_t nv = u32(), nf = u32();
  for(uint32_t i=0; i<nv; ++i) {
    const auto id = u32();
    if(id>=out.vertices.size())
      fail("unreferenced vertex out of range");
    const float x = f32(), y = f32(), z = f32();
    out.vertices[id] = {x,y,z};
    vertexSet[id] = 1;
    }
  for(uint32_t i=0; i<nf; ++i) {
    const auto id = u32();
    if(id>=out.features.size())
      fail("unreferenced feature out of range");
    asset::VertexFeature f;
    f.texture.x = f32();
    f.texture.y = f32();
    f.light     = u32();
    f.normal.x  = f32();
    f.normal.y  = f32();
    f.normal.z  = f32();
    out.features[id] = f;
    featureSet[id] = 1;
    }
  if(o!=bin.size())
    fail("unreferenced.bin has trailing bytes");
  }

  // A source material comes back at its index when the chunk still carries
  // the same record; a changed or new one is appended.
  std::vector<uint32_t> matMap(reader.materials.size());
  std::map<std::string,uint32_t> added;
  for(size_t i=0; i<reader.materials.size(); ++i) {
    const auto& rm = reader.materials[i];
    if(rm.index>=0 && size_t(rm.index)<out.materials.size() && sameMaterial(out.materials[size_t(rm.index)],rm.material)) {
      matMap[i] = uint32_t(rm.index);
      continue;
      }
    const auto key = materialJson(rm.material,0);
    auto [it,inserted] = added.emplace(key,uint32_t(out.materials.size()));
    if(inserted)
      out.materials.push_back(rm.material);
    matMap[i] = it->second;
    }

  std::map<VertexKey,uint32_t>  newVertices;
  std::map<FeatureKey,uint32_t> newFeatures;
  auto vertexIndex = [&](const Corner& c) {
    const auto id = c.vertex.id;
    if(id>=0 && size_t(id)<out.vertices.size()) {
      auto& v = out.vertices[size_t(id)];
      if(!vertexSet[size_t(id)]) {
        v = c.position;
        vertexSet[size_t(id)] = 1;
        return uint32_t(id);
        }
      if(bits(v.x)==c.vertex.x && bits(v.y)==c.vertex.y && bits(v.z)==c.vertex.z)
        return uint32_t(id);
      }
    auto [it,inserted] = newVertices.emplace(c.vertex,uint32_t(out.vertices.size()));
    if(inserted)
      out.vertices.push_back(c.position);
    return it->second;
    };
  auto featureIndex = [&](const Corner& c) {
    const auto id = c.feature.id;
    if(id>=0 && size_t(id)<out.features.size()) {
      auto& f = out.features[size_t(id)];
      if(!featureSet[size_t(id)]) {
        f = c.value;
        featureSet[size_t(id)] = 1;
        return uint32_t(id);
        }
      if(bits(f.texture.x)==c.feature.u && bits(f.texture.y)==c.feature.v && f.light==c.feature.light &&
         bits(f.normal.x)==c.feature.nx && bits(f.normal.y)==c.feature.ny && bits(f.normal.z)==c.feature.nz)
        return uint32_t(id);
      }
    auto [it,inserted] = newFeatures.emplace(c.feature,uint32_t(out.features.size()));
    if(inserted)
      out.features.push_back(c.value);
    return it->second;
    };

  auto& q = out.polygons;
  q.material_indices.reserve(ordered.size());
  q.lightmap_indices.reserve(ordered.size());
  q.flags.reserve(ordered.size());
  q.vertex_indices.reserve(ordered.size()*3);
  q.feature_indices.reserve(ordered.size()*3);
  if(chunks!=nullptr)
    table.triangleChunks.reserve(ordered.size());
  for(const auto t:ordered) {
    const auto& tri = reader.triangles[t];
    q.material_indices.push_back(matMap[tri.material]);
    q.lightmap_indices.push_back(tri.lightmap);
    q.flags.push_back(tri.flags);
    for(const auto& c:tri.corners) {
      q.vertex_indices.push_back(vertexIndex(c));
      q.feature_indices.push_back(featureIndex(c));
      }
    if(chunks!=nullptr) {
      table.triangleChunks.push_back(tri.chunk);
      auto& entry=table.entries[tri.chunk];
      if(entry.triangleCount==0)
        entry.min=entry.max=tri.corners.front().position;
      for(const auto& c:tri.corners) {
        for(unsigned axis=0;axis<3;++axis) {
          entry.min[axis]=std::min(entry.min[axis],c.position[axis]);
          entry.max[axis]=std::max(entry.max[axis],c.position[axis]);
          }
        }
      ++entry.triangleCount;
      }
    }
  if(chunks!=nullptr)
    *chunks=std::move(table);
  return out;
  }

std::string compareWorldMeshes(const asset::WorldMesh& e, const asset::WorldMesh& a) {
  if(e.materials.size()!=a.materials.size())
    return "materials: " + std::to_string(e.materials.size()) + " vs " + std::to_string(a.materials.size());
  for(size_t i=0; i<e.materials.size(); ++i)
    if(!sameMaterial(e.materials[i],a.materials[i]))
      return "material " + std::to_string(i) + " (" + e.materials[i].name + ") differs";
  if(e.vertices.size()!=a.vertices.size())
    return "vertices: " + std::to_string(e.vertices.size()) + " vs " + std::to_string(a.vertices.size());
  for(size_t i=0; i<e.vertices.size(); ++i)
    for(unsigned ax=0; ax<3; ++ax)
      if(bits(e.vertices[i][ax])!=bits(a.vertices[i][ax]))
        return "vertex " + std::to_string(i) + " differs";
  if(e.features.size()!=a.features.size())
    return "features: " + std::to_string(e.features.size()) + " vs " + std::to_string(a.features.size());
  for(size_t i=0; i<e.features.size(); ++i) {
    const auto& x = e.features[i];
    const auto& y = a.features[i];
    if(bits(x.texture.x)!=bits(y.texture.x) || bits(x.texture.y)!=bits(y.texture.y) || x.light!=y.light ||
       bits(x.normal.x)!=bits(y.normal.x) || bits(x.normal.y)!=bits(y.normal.y) || bits(x.normal.z)!=bits(y.normal.z))
      return "feature " + std::to_string(i) + " differs";
    }
  const auto& p = e.polygons;
  const auto& q = a.polygons;
  if(p.material_indices!=q.material_indices)
    return "polygon materials differ";
  if(p.vertex_indices!=q.vertex_indices)
    return "polygon vertex indices differ";
  if(p.feature_indices!=q.feature_indices)
    return "polygon feature indices differ";
  if(p.lightmap_indices!=q.lightmap_indices)
    return "polygon lightmap indices differ";
  if(p.flags.size()!=q.flags.size())
    return "polygon flags differ in count";
  for(size_t i=0; i<p.flags.size(); ++i)
    if(!(p.flags[i]==q.flags[i]))
      return "polygon flags " + std::to_string(i) + " differ";
  return {};
  }
