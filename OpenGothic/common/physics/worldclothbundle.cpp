#include "worldclothbundle.h"

#include "gothicglb.h"
#include "worldchunkcook.h"
#include "worldchunkcookedsource.h"
#include "worldchunksruntime.h"

#include <Tempest/Log>

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstring>
#include <stdexcept>

namespace fs = std::filesystem;

namespace {

static_assert(sizeof(wcl::Surface)==32 && offsetof(wcl::Surface,visual)==12 && offsetof(wcl::Surface,pins)==20 &&
              offsetof(wcl::Surface,zero)==28);
static_assert(sizeof(wcl::Mesh)==56 && offsetof(wcl::Mesh,vertices)==24 && offsetof(wcl::Mesh,features)==32 &&
              offsetof(wcl::Mesh,triangles)==40 && offsetof(wcl::Mesh,materials)==48);
static_assert(sizeof(wcl::Vertex)==12 && sizeof(wcl::Range)==8);
static_assert(sizeof(wcl::Feature)==24 && offsetof(wcl::Feature,light)==8 && offsetof(wcl::Feature,nx)==12);
static_assert(sizeof(wcl::Triangle)==36 && offsetof(wcl::Triangle,material)==24 && offsetof(wcl::Triangle,lightmap)==28 &&
              offsetof(wcl::Triangle,flags)==32 && offsetof(wcl::Triangle,sector)==34);

constexpr char magic[4] = {'G','W','C','L'};
constexpr char what[]   = "cloth bundle";

enum Section : uint32_t {
  Surfaces=1, Meshes=2, Pins=3, Vertices=4, Features=5, Triangles=6, Materials=7, Cells=8, Exclusions=9, Strings=10,
  SectionEnd=11
  };

constexpr wcb::SectionSpec specs[] = {
  {"surfaces",sizeof(wcl::Surface)},
  {"meshes",sizeof(wcl::Mesh)},
  {"pins",4},
  {"vertices",sizeof(wcl::Vertex)},
  {"features",sizeof(wcl::Feature)},
  {"triangles",sizeof(wcl::Triangle)},
  {"materials",sizeof(wcb::Material)},
  {"cells",sizeof(wcl::Range)},
  {"exclusions",4},
  {"strings",1},
  };
static_assert(std::size(specs)==SectionEnd-1);

[[noreturn]] void fail(const std::string& reason) {
  throw std::runtime_error("cloth bundle: "+reason);
  }

uint32_t bits(float v) { return std::bit_cast<uint32_t>(v); }

bool sameBits(const asset::Vec3& a, const asset::Vec3& b) {
  return bits(a.x)==bits(b.x) && bits(a.y)==bits(b.y) && bits(a.z)==bits(b.z);
  }

bool fits(const wcl::Range& r, size_t size) {
  return r.first<=size && r.count<=size-r.first;
  }

std::string hex(const std::array<uint8_t,32>& hash) {
  static constexpr char digits[] = "0123456789abcdef";
  std::string out;
  for(size_t i=0;i<8;++i) {
    out += digits[hash[i]>>4];
    out += digits[hash[i]&15];
    }
  return out+"...";
  }

std::string compareMesh(const asset::WorldMesh& e, const asset::WorldMesh& a) {
  if(e.name!=a.name)
    return "name '"+e.name+"' vs '"+a.name+"'";
  if(!sameBits(e.bbox.min,a.bbox.min) || !sameBits(e.bbox.max,a.bbox.max))
    return "bounds differ";
  return compareWorldMeshes(e,a);
  }

// What applyClothBundle checks before it changes anything: the result and the triangles that stay.
struct Prepared {
  WorldCloth::Extracted cloth;
  std::vector<uint32_t> remaining;
  size_t                removed = 0;
  };

Prepared prepare(const ClothBundle& bundle, const asset::WorldMesh& landscape, const ChunkTable& table) {
  const auto& entries = table.entries;
  const size_t n = landscape.polygons.material_indices.size();
  if(bundle.cells()!=entries.size())
    fail(bundle.name()+" lists "+std::to_string(bundle.cells())+" cells, the loaded chunk table "+
         std::to_string(entries.size()));
  if(table.triangleChunks.size()!=n || table.triangleSources.size()!=n)
    fail("the loaded chunk table does not cover the landscape's "+std::to_string(n)+" triangles");
  // One flag per cell-local ordinal, cell after cell.
  std::vector<size_t> start(entries.size()+1,0);
  for(size_t c=0;c<entries.size();++c)
    start[c+1] = start[c]+entries[c].triangleCount;
  std::vector<char> excluded(start.back(),0);
  size_t named = 0;
  for(size_t c=0;c<entries.size();++c)
    for(const auto ordinal:bundle.exclusions(c)) {
      if(ordinal>=entries[c].triangleCount)
        fail(bundle.name()+": cell "+std::to_string(entries[c].id.x)+","+std::to_string(entries[c].id.z)+
             " excludes triangle "+std::to_string(ordinal)+", past the cell's "+
             std::to_string(entries[c].triangleCount)+" triangles in the loaded chunk table");
      excluded[start[c]+ordinal] = 1;
      ++named;
      }
  Prepared out;
  out.remaining.reserve(n-std::min(n,named));
  for(size_t t=0;t<n;++t) {
    const auto c = table.triangleChunks[t], ordinal = table.triangleSources[t];
    if(c>=entries.size() || ordinal>=entries[c].triangleCount)
      fail("the loaded chunk table places triangle "+std::to_string(t)+" outside its cells");
    if(excluded[start[c]+ordinal])
      ++out.removed;
    else
      out.remaining.push_back(uint32_t(t));
    }
  const auto original = bundle.originalTriangles();
  if(out.removed!=named || out.removed!=original)
    fail(bundle.name()+": its exclusions name "+std::to_string(named)+" triangles, the loaded chunk table holds "+
         std::to_string(out.removed)+" of them, its original mesh has "+std::to_string(original));
  for(size_t i=0;i<bundle.surfaces();++i)
    out.cloth.surfaces.push_back(bundle.surface(i,landscape.name));
  out.cloth.original = bundle.original(landscape.name);
  return out;
  }

// searchIslands leaves the landscape and the table as they are when it takes no triangle.
void commit(const Prepared& prepared, asset::WorldMesh& landscape, ChunkTable& table) {
  if(prepared.removed>0)
    retainWorldTriangles(landscape,prepared.remaining,&table);
  }

}

std::vector<uint8_t> encodeClothBundle(const ClothBundleData& data) {
  using wcb::count32;
  const auto& h = data.header;
  const auto& surfaces = data.cloth.surfaces;
  std::vector<const asset::WorldMesh*> meshes;
  for(const auto& s:surfaces)
    meshes.push_back(&s.mesh);
  meshes.push_back(&data.cloth.original);
  for(size_t i=0;i<meshes.size();++i) {
    const auto& p = meshes[i]->polygons;
    const auto n = p.material_indices.size();
    if(p.lightmap_indices.size()!=n || p.flags.size()!=n || p.vertex_indices.size()!=n*3 || p.feature_indices.size()!=n*3)
      fail("mesh "+std::to_string(i)+": polygon arrays disagree in length");
    }

  wcb::Writer w;
  for(const auto c:magic)
    w.u8(uint8_t(c));
  w.u32(wcl::Version);
  w.u32(wcb::ByteOrder);
  w.u32(wcl::HeaderSize);
  const size_t fileSizeAt = w.out.size();
  w.u64(0);
  w.u64(h.worldSize);
  w.u32(h.worldCrc32);
  w.u32(count32(data.exclusions.size(),"cells"));
  w.u32(count32(surfaces.size(),"surfaces"));
  w.u32(h.algorithm);
  for(const auto* hash:{&h.indexSha256,&h.rulesSha256})
    for(const auto b:*hash)
      w.u8(b);
  w.u32(SectionEnd-1);
  w.u32(0);
  w.u64(wcl::HeaderSize);
  w.u64(0);
  if(w.out.size()!=wcl::HeaderSize)
    fail("header layout");

  w.directory(SectionEnd-1);
  std::string strings;
  w.section(Surfaces,sizeof(wcl::Surface),surfaces.size(),"surfaces",[&] {
    uint32_t pins = 0;
    for(const auto& s:surfaces) {
      w.u32(s.id);
      w.f32(s.stiffness);
      w.f32(s.linearDamping);
      w.u32(count32(strings.size(),"string bytes"));
      w.u32(count32(s.visual.size(),"string bytes"));
      strings += s.visual;
      w.u32(pins);
      w.u32(count32(s.pins.size(),"pins"));
      pins += uint32_t(s.pins.size());
      w.u32(0);
      }
    });
  w.section(Meshes,sizeof(wcl::Mesh),meshes.size(),"meshes",[&] {
    uint32_t next[4] = {0,0,0,0};
    for(const auto* m:meshes) {
      for(const auto* v:{&m->bbox.min,&m->bbox.max})
        for(unsigned axis=0;axis<3;++axis)
          w.f32((*v)[axis]);
      const size_t counts[4] = {m->vertices.size(),m->features.size(),m->polygons.material_indices.size(),
                                m->materials.size()};
      for(unsigned k=0;k<4;++k) {
        w.u32(next[k]);
        w.u32(count32(counts[k],"mesh records"));
        next[k] += uint32_t(counts[k]);
        }
      }
    });
  size_t pinCount = 0, vertexCount = 0, featureCount = 0, triangleCount = 0, materialCount = 0, exclusionCount = 0;
  for(const auto& s:surfaces)
    pinCount += s.pins.size();
  for(const auto* m:meshes) {
    vertexCount   += m->vertices.size();
    featureCount  += m->features.size();
    triangleCount += m->polygons.material_indices.size();
    materialCount += m->materials.size();
    }
  for(const auto& cell:data.exclusions)
    exclusionCount += cell.size();
  w.section(Pins,4,pinCount,"pins",[&] {
    for(const auto& s:surfaces)
      for(const auto pin:s.pins)
        w.u32(pin);
    });
  w.section(Vertices,sizeof(wcl::Vertex),vertexCount,"vertices",[&] {
    for(const auto* m:meshes)
      for(const auto& v:m->vertices) {
        w.f32(v.x); w.f32(v.y); w.f32(v.z);
        }
    });
  w.section(Features,sizeof(wcl::Feature),featureCount,"features",[&] {
    for(const auto* m:meshes)
      for(const auto& f:m->features) {
        w.f32(f.texture.x); w.f32(f.texture.y); w.u32(f.light);
        w.f32(f.normal.x); w.f32(f.normal.y); w.f32(f.normal.z);
        }
    });
  w.section(Triangles,sizeof(wcl::Triangle),triangleCount,"triangles",[&] {
    for(const auto* m:meshes) {
      const auto& p = m->polygons;
      for(size_t t=0;t<p.material_indices.size();++t) {
        for(unsigned c=0;c<3;++c)
          w.u32(p.vertex_indices[t*3+c]);
        for(unsigned c=0;c<3;++c)
          w.u32(p.feature_indices[t*3+c]);
        w.u32(p.material_indices[t]);
        w.i32(p.lightmap_indices[t]);
        w.u16(wcb::packFlags(p.flags[t]));
        w.u16(uint16_t(p.flags[t].sector_index));
        }
      }
    });
  w.section(Materials,sizeof(wcb::Material),materialCount,"materials",[&] {
    for(const auto* m:meshes)
      for(const auto& material:m->materials)
        w.material(wcb::None,material,strings);
    });
  w.section(Cells,sizeof(wcl::Range),data.exclusions.size(),"cells",[&] {
    uint32_t first = 0;
    for(const auto& cell:data.exclusions) {
      w.u32(first);
      w.u32(count32(cell.size(),"exclusions"));
      first += uint32_t(cell.size());
      }
    });
  w.section(Exclusions,4,exclusionCount,"exclusions",[&] {
    for(const auto& cell:data.exclusions)
      for(const auto ordinal:cell)
        w.u32(ordinal);
    });
  w.strings(Strings,strings);
  w.put64(fileSizeAt,w.out.size());
  return std::move(w.out);
  }

ClothBundle ClothBundle::open(std::span<const uint8_t> bytes, const std::string& name, std::shared_ptr<const void> owner) {
  using wcb::le32;
  using wcb::le64;
  wcb::checkPrologue(bytes,magic,wcl::Version,wcl::HeaderSize,alignof(uint32_t),what,name);
  const auto* b = bytes.data();
  ClothBundle x;
  x.owner = std::move(owner);
  x.bytes = bytes;
  x.label = name;
  auto& h = x.head;
  h.worldSize  = le64(b+24);
  h.worldCrc32 = le32(b+32);
  h.cells      = le32(b+36);
  h.surfaces   = le32(b+40);
  h.algorithm  = le32(b+44);
  std::memcpy(h.indexSha256.data(),b+48,32);
  std::memcpy(h.rulesSha256.data(),b+80,32);
  auto reject = [&](const char* part, const std::string& reason) {
    wcb::reject(what,name,part,reason);
    };
  if(le32(b+116)!=0 || le64(b+128)!=0)
    reject("header","the reserved fields are not zero");
  const auto sections = wcb::readDirectory(bytes,wcl::HeaderSize,le32(b+112),le64(b+120),specs,what,name);
  if(sections[Surfaces-1].count!=h.surfaces)
    reject("surfaces","the header counts "+std::to_string(h.surfaces)+", the section holds "+
           std::to_string(sections[Surfaces-1].count));
  if(uint64_t(sections[Meshes-1].count)!=uint64_t(h.surfaces)+1)
    reject("meshes",std::to_string(sections[Meshes-1].count)+" meshes for "+std::to_string(h.surfaces)+
           " surfaces and the original mesh");
  if(sections[Cells-1].count!=h.cells)
    reject("cells","the header counts "+std::to_string(h.cells)+", the section holds "+
           std::to_string(sections[Cells-1].count));
  x.surfaceSpan   = wcb::view<wcl::Surface>(bytes,sections[Surfaces-1]);
  x.meshSpan      = wcb::view<wcl::Mesh>(bytes,sections[Meshes-1]);
  x.pinSpan       = wcb::view<uint32_t>(bytes,sections[Pins-1]);
  x.vertexSpan    = wcb::view<wcl::Vertex>(bytes,sections[Vertices-1]);
  x.featureSpan   = wcb::view<wcl::Feature>(bytes,sections[Features-1]);
  x.triangleSpan  = wcb::view<wcl::Triangle>(bytes,sections[Triangles-1]);
  x.materialSpan  = wcb::view<wcb::Material>(bytes,sections[Materials-1]);
  x.cellSpan      = wcb::view<wcl::Range>(bytes,sections[Cells-1]);
  x.exclusionSpan = wcb::view<uint32_t>(bytes,sections[Exclusions-1]);
  x.stringSpan    = bytes.subspan(size_t(sections[Strings-1].offset),sections[Strings-1].count);

  // Mesh ranges back to back, in mesh order, filling their sections; every triangle inside its own mesh.
  const char* kinds[4] = {"vertices","features","triangles","materials"};
  const size_t totals[4] = {x.vertexSpan.size(),x.featureSpan.size(),x.triangleSpan.size(),x.materialSpan.size()};
  uint64_t next[4] = {0,0,0,0};
  for(size_t i=0;i<x.meshSpan.size();++i) {
    const auto& m = x.meshSpan[i];
    const wcl::Range* ranges[4] = {&m.vertices,&m.features,&m.triangles,&m.materials};
    const auto where = "mesh "+std::to_string(i);
    for(unsigned k=0;k<4;++k) {
      if(ranges[k]->first!=next[k] || !fits(*ranges[k],totals[k]))
        reject("meshes",where+": "+kinds[k]+" "+std::to_string(ranges[k]->first)+"+"+std::to_string(ranges[k]->count)+
               " are not the next ones of "+std::to_string(totals[k]));
      next[k] = uint64_t(ranges[k]->first)+ranges[k]->count;
      }
    for(uint32_t t=m.triangles.first;t<m.triangles.first+m.triangles.count;++t) {
      const auto& tri = x.triangleSpan[t];
      for(unsigned c=0;c<3;++c)
        if(tri.vertex[c]>=m.vertices.count || tri.feature[c]>=m.features.count)
          reject("triangles",where+": triangle "+std::to_string(t-m.triangles.first)+" uses vertex "+
                 std::to_string(tri.vertex[c])+" and feature "+std::to_string(tri.feature[c])+" of "+
                 std::to_string(m.vertices.count)+" and "+std::to_string(m.features.count));
      if(tri.material>=m.materials.count)
        reject("triangles",where+": triangle "+std::to_string(t-m.triangles.first)+" uses material "+
               std::to_string(tri.material)+" of "+std::to_string(m.materials.count));
      }
    }
  for(unsigned k=0;k<4;++k)
    if(next[k]!=totals[k])
      reject(kinds[k],std::to_string(totals[k]-next[k])+" records belong to no mesh");
  for(size_t i=0;i<x.materialSpan.size();++i) {
    if(x.materialSpan[i].source!=wcb::None)
      reject("materials","material "+std::to_string(i)+" carries a source index");
    if(!wcb::materialStringsFit(x.materialSpan[i],x.stringSpan.size()))
      reject("materials","material "+std::to_string(i)+" has a string past the string section");
    }

  // Pins back to back in surface order, each a vertex of its surface's mesh.
  uint64_t pins = 0;
  for(size_t i=0;i<x.surfaceSpan.size();++i) {
    const auto& s = x.surfaceSpan[i];
    const auto where = "surface "+std::to_string(i);
    if(s.zero!=0)
      reject("surfaces",where+": the reserved field is not zero");
    if(s.visual.offset>x.stringSpan.size() || s.visual.size>x.stringSpan.size()-s.visual.offset)
      reject("surfaces",where+": its visual is past the string section");
    if(s.pins.first!=pins || !fits(s.pins,x.pinSpan.size()))
      reject("pins",where+": entries "+std::to_string(s.pins.first)+"+"+std::to_string(s.pins.count)+
             " are not the next ones of "+std::to_string(x.pinSpan.size()));
    pins = uint64_t(s.pins.first)+s.pins.count;
    for(uint32_t p=s.pins.first;p<s.pins.first+s.pins.count;++p)
      if(x.pinSpan[p]>=x.meshSpan[i].vertices.count)
        reject("pins",where+": pin "+std::to_string(x.pinSpan[p])+" past its mesh's "+
               std::to_string(x.meshSpan[i].vertices.count)+" vertices");
    }
  if(pins!=x.pinSpan.size())
    reject("pins",std::to_string(x.pinSpan.size()-pins)+" entries belong to no surface");

  // Exclusions back to back in cell order, ascending within a cell.
  uint64_t exclusions = 0;
  for(size_t c=0;c<x.cellSpan.size();++c) {
    const auto& r = x.cellSpan[c];
    if(r.first!=exclusions || !fits(r,x.exclusionSpan.size()))
      reject("cells","cell "+std::to_string(c)+": exclusions "+std::to_string(r.first)+"+"+std::to_string(r.count)+
             " are not the next ones of "+std::to_string(x.exclusionSpan.size()));
    exclusions = uint64_t(r.first)+r.count;
    for(uint32_t e=r.first+1;e<r.first+r.count;++e)
      if(x.exclusionSpan[e]<=x.exclusionSpan[e-1])
        reject("exclusions","cell "+std::to_string(c)+": ordinals are not ascending at entry "+std::to_string(e));
    }
  if(exclusions!=x.exclusionSpan.size())
    reject("exclusions",std::to_string(x.exclusionSpan.size()-exclusions)+" entries belong to no cell");
  return x;
  }

ClothBundle ClothBundle::read(const fs::path& path) {
  auto bytes = std::make_shared<const std::vector<uint8_t>>(gothicglb::readFile(path));
  const std::span<const uint8_t> view(bytes->data(),bytes->size());
  return open(view,path.string(),std::move(bytes));
  }

asset::WorldMesh ClothBundle::mesh(size_t i, const std::string& name) const {
  const auto& m = meshSpan[i];
  asset::WorldMesh out;
  // searchIslands builds a mesh only from triangles; with none (no cloth) the original stays default.
  if(m.triangles.count>0)
    out.name = name;
  out.bbox.min = {m.min[0],m.min[1],m.min[2]};
  out.bbox.max = {m.max[0],m.max[1],m.max[2]};
  out.materials.reserve(m.materials.count);
  for(uint32_t k=0;k<m.materials.count;++k)
    out.materials.push_back(wcb::readMaterial(materialSpan[m.materials.first+k],stringSpan));
  out.vertices.reserve(m.vertices.count);
  for(uint32_t k=0;k<m.vertices.count;++k) {
    const auto& v = vertexSpan[m.vertices.first+k];
    out.vertices.push_back({v.x,v.y,v.z});
    }
  out.features.reserve(m.features.count);
  for(uint32_t k=0;k<m.features.count;++k) {
    const auto& f = featureSpan[m.features.first+k];
    asset::VertexFeature feature;
    feature.texture = {f.u,f.v};
    feature.light   = f.light;
    feature.normal  = {f.nx,f.ny,f.nz};
    out.features.push_back(feature);
    }
  auto& p = out.polygons;
  p.material_indices.reserve(m.triangles.count);
  p.lightmap_indices.reserve(m.triangles.count);
  p.flags.reserve(m.triangles.count);
  p.vertex_indices.reserve(size_t(m.triangles.count)*3);
  p.feature_indices.reserve(size_t(m.triangles.count)*3);
  for(uint32_t k=0;k<m.triangles.count;++k) {
    const auto& t = triangleSpan[m.triangles.first+k];
    p.material_indices.push_back(t.material);
    p.lightmap_indices.push_back(t.lightmap);
    p.flags.push_back(wcb::unpackFlags(t.flags,t.sector));
    for(unsigned c=0;c<3;++c) {
      p.vertex_indices.push_back(t.vertex[c]);
      p.feature_indices.push_back(t.feature[c]);
      }
    }
  return out;
  }

WorldCloth::Surface ClothBundle::surface(size_t i, const std::string& name) const {
  const auto& s = surfaceSpan[i];
  WorldCloth::Surface out;
  out.mesh = mesh(i,name);
  out.pins.assign(pinSpan.begin()+s.pins.first,pinSpan.begin()+s.pins.first+s.pins.count);
  out.visual.assign(reinterpret_cast<const char*>(stringSpan.data())+s.visual.offset,s.visual.size);
  out.id            = s.id;
  out.stiffness     = s.stiffness;
  out.linearDamping = s.linearDamping;
  return out;
  }

asset::WorldMesh ClothBundle::original(const std::string& name) const {
  return mesh(surfaceSpan.size(),name);
  }

size_t ClothBundle::originalTriangles() const {
  return meshSpan[surfaceSpan.size()].triangles.count;
  }

std::span<const uint32_t> ClothBundle::exclusions(size_t cell) const {
  return exclusionSpan.subspan(cellSpan[cell].first,cellSpan[cell].count);
  }

ClothBundleData ClothBundle::data() const {
  ClothBundleData out;
  out.header = head;
  for(size_t i=0;i<surfaces();++i)
    out.cloth.surfaces.push_back(surface(i));
  out.cloth.original = original();
  for(size_t c=0;c<cells();++c) {
    const auto list = exclusions(c);
    out.exclusions.emplace_back(list.begin(),list.end());
    }
  return out;
  }

fs::path cookedClothPath(const fs::path& dir, const fs::path& overrideDir, const fs::path& outDir) {
  return cookedIndexPath(dir,overrideDir,outDir).parent_path()/wcl::FileName;
  }

ClothBundleData cookClothBundle(asset::WorldMesh& mesh, ChunkTable& table, const WorldChunkSource& source,
                                const std::array<uint8_t,32>& indexSha256) {
  const size_t n = mesh.polygons.material_indices.size();
  if(table.triangleChunks.size()!=n || table.triangleSources.size()!=n)
    fail("the stitched mesh and its chunk table disagree");
  const auto chunks = table.triangleChunks, sources = table.triangleSources;
  ClothBundleData data;
  data.cloth = WorldCloth::searchIslands(mesh,&table);
  // The table after the search keeps the other triangles in their order: the ones it skipped are the cloth.
  data.exclusions.resize(table.entries.size());
  size_t kept = 0;
  for(size_t t=0;t<n;++t) {
    if(kept<table.triangleChunks.size() && table.triangleChunks[kept]==chunks[t] && table.triangleSources[kept]==sources[t]) {
      ++kept;
      continue;
      }
    data.exclusions.at(chunks[t]).push_back(sources[t]);
    }
  size_t excluded = 0;
  for(auto& cell:data.exclusions) {
    std::sort(cell.begin(),cell.end());
    excluded += cell.size();
    }
  if(kept!=table.triangleChunks.size() || excluded!=data.cloth.original.polygons.material_indices.size())
    fail("the island search took "+std::to_string(n-kept)+" triangles out of the chunk table, its original mesh has "+
         std::to_string(data.cloth.original.polygons.material_indices.size()));
  auto& h = data.header;
  h.worldSize   = source.size;
  h.worldCrc32  = source.crc32;
  h.cells       = wcb::count32(data.exclusions.size(),"cells");
  h.surfaces    = wcb::count32(data.cloth.surfaces.size(),"surfaces");
  h.algorithm   = WorldCloth::Algorithm;
  h.indexSha256 = indexSha256;
  h.rulesSha256 = WorldCloth::rulesSha256();
  return data;
  }

void checkClothBundle(const ClothBundle& bundle, const CookedSet& set) {
  const auto& h = bundle.header();
  if(h.worldSize!=set.source.size || h.worldCrc32!=set.source.crc32)
    fail(bundle.name()+" names another source world than "+set.source.world);
  if(h.indexSha256!=set.indexSha256)
    fail(bundle.name()+" was cooked with another index than "+set.index.string()+" (SHA-256 "+hex(h.indexSha256)+
         ", the index read at load has "+hex(set.indexSha256)+"): the set changed since the bundle was cooked");
  if(h.rulesSha256!=WorldCloth::rulesSha256())
    fail(bundle.name()+" was cooked with other world cloth rules than this build's (SHA-256 "+hex(h.rulesSha256)+
         ", data/cloth-world.csv has "+hex(WorldCloth::rulesSha256())+")");
  if(h.algorithm!=WorldCloth::Algorithm)
    fail(bundle.name()+" was cooked by world cloth algorithm "+std::to_string(h.algorithm)+", this build runs "+
         std::to_string(WorldCloth::Algorithm));
  if(bundle.cells()!=set.cells)
    fail(bundle.name()+" lists "+std::to_string(bundle.cells())+" cells, world.json "+std::to_string(set.cells));
  }

WorldCloth::Extracted applyClothBundle(const ClothBundle& bundle, asset::WorldMesh& landscape, ChunkTable& table) {
  auto prepared = prepare(bundle,landscape,table);
  commit(prepared,landscape,table);
  return std::move(prepared.cloth);
  }

std::optional<WorldCloth::Extracted> takeClothBundle(asset::WorldMesh& landscape, ChunkTable& table) {
  if(!table.source)
    return std::nullopt;
  const auto* set = table.source->cooked().cookedSet();
  if(set==nullptr)
    return std::nullopt;
  const auto path = set->index.parent_path()/wcl::FileName;
  std::error_code error;
  if(!fs::is_regular_file(path,error))
    return std::nullopt;
  std::optional<Prepared> prepared;
  try {
    if(!set->stale.empty())
      throw std::runtime_error(set->stale);
    const auto bundle = ClothBundle::read(path);
    checkClothBundle(bundle,*set);
    prepared = prepare(bundle,landscape,table);
    }
  catch(const std::exception& e) {
    Tempest::Log::e("[world-cloth] bundle ",path.string()," not used, the island search runs: ",e.what());
    return std::nullopt;
    }
  commit(*prepared,landscape,table);
  Tempest::Log::i("[world-cloth] bundle ",path.string(),": ",uint64_t(prepared->cloth.surfaces.size())," surfaces, ",
                  uint64_t(prepared->removed)," triangles");
  return std::move(prepared->cloth);
  }

std::string compareWorldCloth(const WorldCloth::Extracted& e, const WorldCloth::Extracted& a) {
  if(e.surfaces.size()!=a.surfaces.size())
    return std::to_string(e.surfaces.size())+" vs "+std::to_string(a.surfaces.size())+" surfaces";
  for(size_t i=0;i<e.surfaces.size();++i) {
    const auto& x = e.surfaces[i];
    const auto& y = a.surfaces[i];
    const auto where = "surface "+std::to_string(i)+" ("+x.visual+")";
    if(x.visual!=y.visual)
      return where+": visual '"+y.visual+"'";
    if(x.id!=y.id)
      return where+": id "+std::to_string(x.id)+" vs "+std::to_string(y.id);
    if(bits(x.stiffness)!=bits(y.stiffness) || bits(x.linearDamping)!=bits(y.linearDamping))
      return where+": stiffness or damping differ";
    if(x.pins!=y.pins)
      return where+": pins differ";
    const auto difference = compareMesh(x.mesh,y.mesh);
    if(!difference.empty())
      return where+": mesh: "+difference;
    }
  const auto difference = compareMesh(e.original,a.original);
  if(!difference.empty())
    return "original mesh: "+difference;
  return {};
  }

std::string compareClothBundles(const ClothBundleData& e, const ClothBundleData& a) {
  const auto& x = e.header;
  const auto& y = a.header;
  if(x.worldSize!=y.worldSize || x.worldCrc32!=y.worldCrc32)
    return "header: another source world";
  if(x.indexSha256!=y.indexSha256)
    return "header: index SHA-256 "+hex(x.indexSha256)+" vs "+hex(y.indexSha256);
  if(x.rulesSha256!=y.rulesSha256)
    return "header: rules SHA-256 "+hex(x.rulesSha256)+" vs "+hex(y.rulesSha256);
  if(x.algorithm!=y.algorithm)
    return "header: algorithm "+std::to_string(x.algorithm)+" vs "+std::to_string(y.algorithm);
  auto difference = compareWorldCloth(e.cloth,a.cloth);
  if(!difference.empty())
    return difference;
  if(e.exclusions.size()!=a.exclusions.size())
    return "exclusions: "+std::to_string(e.exclusions.size())+" vs "+std::to_string(a.exclusions.size())+" cells";
  for(size_t c=0;c<e.exclusions.size();++c)
    if(e.exclusions[c]!=a.exclusions[c])
      return "exclusions of cell "+std::to_string(c)+" differ";
  return {};
  }
