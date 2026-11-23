#include "worldchunkbinary.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>

#include "gothicglb.h"
#include "sha256.h"

namespace {

static_assert(std::endian::native==std::endian::little,"cooked chunks are viewed in place: a little-endian host is required");

// The structs view the bytes of the file: their layouts must be the table of worldchunkbinary.h.
static_assert(sizeof(wcb::Position)==16 && offsetof(wcb::Position,source)==12);
static_assert(sizeof(wcb::Feature)==28 && offsetof(wcb::Feature,light)==8 && offsetof(wcb::Feature,nx)==12 &&
              offsetof(wcb::Feature,source)==24);
static_assert(sizeof(wcb::Triangle)==40 && offsetof(wcb::Triangle,feature)==12 && offsetof(wcb::Triangle,material)==24 &&
              offsetof(wcb::Triangle,order)==28 && offsetof(wcb::Triangle,lightmap)==32 &&
              offsetof(wcb::Triangle,flags)==36 && offsetof(wcb::Triangle,sector)==38);
static_assert(sizeof(wcb::Material)==88 && offsetof(wcb::Material,name)==4 && offsetof(wcb::Material,texture)==12 &&
              offsetof(wcb::Material,detailObject)==20 && offsetof(wcb::Material,smoothAngle)==28 &&
              offsetof(wcb::Material,defaultMapping)==68 && offsetof(wcb::Material,group)==76 &&
              offsetof(wcb::Material,color)==77 && offsetof(wcb::Material,alphaFunc)==84 &&
              offsetof(wcb::Material,switches)==85);
static_assert(sizeof(ClusterLod::Sphere)==16 && offsetof(ClusterLod::Sphere,radius)==12);
static_assert(sizeof(ClusterLod::Cluster)==48 && offsetof(ClusterLod::Cluster,level)==16 &&
              offsetof(ClusterLod::Cluster,material)==20 && offsetof(ClusterLod::Cluster,refinedGroup)==24 &&
              offsetof(ClusterLod::Cluster,parentGroup)==28 && offsetof(ClusterLod::Cluster,vertexOffset)==32 &&
              offsetof(ClusterLod::Cluster,indexOffset)==36 && offsetof(ClusterLod::Cluster,vertexCount)==40 &&
              offsetof(ClusterLod::Cluster,triangleCount)==44);
static_assert(sizeof(ClusterLod::Group)==20 && offsetof(ClusterLod::Group,errorCm)==16);
static_assert(sizeof(wcb::VertexRef)==8 && offsetof(wcb::VertexRef,feature)==4);

constexpr char     magic[4]    = {'G','W','C','B'};
constexpr uint32_t maxSections = 16;
constexpr char     what[]      = "cooked chunk";

enum Section : uint32_t {
  Positions=1, Features=2, Triangles=3, Materials=4, Strings=5,
  Clusters=6, Groups=7, Indices=8, VertexIndices=9, VertexRefs=10, SectionEnd=11
  };
constexpr uint32_t baseSections = Strings;
constexpr uint32_t currentSections = SectionEnd-1;

constexpr wcb::SectionSpec specs[] = {
  {"positions",sizeof(wcb::Position)},
  {"features",sizeof(wcb::Feature)},
  {"triangles",sizeof(wcb::Triangle)},
  {"materials",sizeof(wcb::Material)},
  {"strings",1},
  {"clusters",sizeof(ClusterLod::Cluster)},
  {"groups",sizeof(ClusterLod::Group)},
  {"local indices",1},
  {"vertex indices",sizeof(uint32_t)},
  {"vertex refs",sizeof(wcb::VertexRef)},
  };
static_assert(std::size(specs)==currentSections);

uint8_t enum8(int64_t v, const char* field) {
  if(v<0 || v>0xFF)
    throw std::runtime_error(std::string("cooked chunk: material ")+field+" "+std::to_string(v)+" does not fit a byte");
  return uint8_t(v);
  }

float lef32(const uint8_t* p) { return std::bit_cast<float>(wcb::le32(p)); }

}

namespace wcb {

uint16_t packFlags(const asset::PolygonFlagSet& f) {
  const unsigned lo = (unsigned(f.is_portal)&3u) | (unsigned(f.is_occluder)<<2) | (unsigned(f.is_sector)<<3) |
                      (unsigned(f.should_relight)<<4) | (unsigned(f.is_outdoor)<<5) |
                      (unsigned(f.is_ghost_occluder)<<6) | (unsigned(f.is_dynamically_lit)<<7) |
                      (unsigned(f.is_lod)<<8) | ((unsigned(f.normal_axis)&3u)<<9);
  return uint16_t(lo);
  }

asset::PolygonFlagSet unpackFlags(uint16_t lo, int16_t sector) {
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
  f.sector_index       = sector;
  return f;
  }

void reject(const std::string& kind, const std::string& name, const std::string& part, const std::string& reason) {
  throw std::runtime_error(kind+" "+name+": "+part+": "+reason);
  }

uint32_t count32(size_t n, const char* field) {
  if(n>std::numeric_limits<uint32_t>::max())
    throw std::runtime_error(std::string("cooked file: too many ")+field);
  return uint32_t(n);
  }

void Writer::f32(float v) {
  u32(std::bit_cast<uint32_t>(v));
  }

void Writer::directory(uint32_t count) {
  directoryAt = out.size();
  out.resize(directoryAt+size_t(EntrySize)*count,0);
  }

void Writer::entry(uint32_t type, uint32_t stride, size_t count, const char* name, size_t begin) {
  const size_t at = directoryAt+size_t(type-1)*EntrySize;
  put32(at,type);
  put32(at+4,stride);
  put32(at+8,count32(count,name));
  put64(at+16,begin);
  put64(at+24,out.size()-begin);
  }

void Writer::material(uint32_t source, const asset::Material& m, std::string& strings) {
  u32(source);
  for(const auto* s:{&m.name,&m.texture,&m.detail_object}) {
    u32(count32(strings.size(),"string bytes"));
    u32(count32(s->size(),"string bytes"));
    strings += *s;
    }
  for(const float f:{m.smooth_angle,m.texture_scale.x,m.texture_scale.y,m.texture_anim_fps,
                     m.texture_anim_map_dir.x,m.texture_anim_map_dir.y,m.detail_object_scale,
                     m.environment_mapping_strength,m.wave_max_amplitude,m.wave_grid_size,
                     m.default_mapping.x,m.default_mapping.y})
    f32(f);
  u8(enum8(int64_t(m.group),"group"));
  for(unsigned c=0;c<4;++c)
    u8(m.color[c]);
  u8(enum8(int64_t(m.texture_anim_map_mode),"texture_anim_map_mode"));
  u8(enum8(int64_t(m.wave_mode),"wave_mode"));
  u8(enum8(int64_t(m.wave_speed),"wave_speed"));
  u8(enum8(int64_t(m.alpha_func),"alpha_func"));
  u8(uint8_t((m.disable_collision ? DisableCollision : 0) | (m.disable_lightmap ? DisableLightmap : 0) |
             (m.dont_collapse ? DontCollapse : 0) | (m.force_occluder ? ForceOccluder : 0) |
             (m.environment_mapping ? EnvironmentMapping : 0) | (m.ignore_sun ? IgnoreSun : 0)));
  u8(0);
  u8(0);
  }

void Writer::strings(uint32_t type, const std::string& bytes) {
  section(type,1,bytes.size(),"strings",[&] {
    out.insert(out.end(),bytes.begin(),bytes.end());
    });
  }

static uint64_t checkPrologueImpl(std::span<const uint8_t> bytes, uint64_t fileSize, const char* magicBytes,
                                  uint32_t version, uint32_t minVersion, uint32_t headerSize, size_t alignment,
                                  const std::string& kind, const std::string& name) {
  if(bytes.size()<headerSize)
    reject(kind,name,"header","truncated: "+std::to_string(bytes.size())+" bytes, the header alone is "+
           std::to_string(headerSize));
  const auto* b = bytes.data();
  if(std::memcmp(b,magicBytes,4)!=0)
    reject(kind,name,"header","wrong magic, not a "+kind);
  const auto actualVersion = le32(b+4);
  if(actualVersion<minVersion || actualVersion>version)
    reject(kind,name,"header","version "+std::to_string(actualVersion)+", this reader reads "+std::to_string(version)+
           (version==minVersion ? "" : " through "+std::to_string(minVersion)));
  if(le32(b+8)!=ByteOrder)
    reject(kind,name,"header","byte order mark is not little-endian");
  if(le32(b+12)!=headerSize)
    reject(kind,name,"header","header size "+std::to_string(le32(b+12))+", expected "+std::to_string(headerSize));
  const uint64_t size = le64(b+16);
  if(size>fileSize)
    reject(kind,name,"header","truncated: "+std::to_string(fileSize)+" of "+std::to_string(size)+" bytes");
  if(size<fileSize)
    reject(kind,name,"header",std::to_string(fileSize)+" bytes, the header says "+std::to_string(size));
  if(reinterpret_cast<uintptr_t>(b)%alignment!=0)
    reject(kind,name,"header","bytes are not "+std::to_string(alignment)+"-byte aligned in memory");
  return size;
  }

uint64_t checkPrologue(std::span<const uint8_t> bytes, const char* magicBytes, uint32_t version, uint32_t headerSize,
                       size_t alignment, const std::string& kind, const std::string& name) {
  return checkPrologueImpl(bytes,bytes.size(),magicBytes,version,version,headerSize,alignment,kind,name);
  }

static std::vector<SectionView> readDirectoryImpl(std::span<const uint8_t> bytes, uint64_t fileSize,
                                                  uint32_t headerSize, uint32_t sections, uint64_t directory,
                                                  std::span<const SectionSpec> sectionSpecs,
                                                  const std::string& kind, const std::string& name) {
  const auto* b = bytes.data();
  if(sections>maxSections)
    reject(kind,name,"directory",std::to_string(sections)+" sections, at most "+std::to_string(maxSections));
  if(directory<headerSize || directory>bytes.size() || (bytes.size()-directory)/EntrySize<sections)
    reject(kind,name,"directory","past the end of the file");
  const uint64_t directoryEnd = directory+uint64_t(sections)*EntrySize;

  std::vector<SectionView> out(sectionSpecs.size());
  std::vector<char> found(sectionSpecs.size(),0);
  for(uint32_t i=0;i<sections;++i) {
    const auto* e = b+directory+uint64_t(i)*EntrySize;
    const uint32_t type = le32(e);
    if(type==0 || type>sectionSpecs.size())
      reject(kind,name,"directory","unknown section type "+std::to_string(type));
    const auto& spec = sectionSpecs[type-1];
    const std::string part = spec.name;
    if(found[type-1])
      reject(kind,name,part,"listed twice");
    const uint32_t stride = le32(e+4), count = le32(e+8);
    const uint64_t offset = le64(e+16), length = le64(e+24);
    if(stride!=spec.stride)
      reject(kind,name,part,"stride "+std::to_string(stride)+", expected "+std::to_string(spec.stride));
    if(offset<directoryEnd || offset%SectionAlign!=0)
      reject(kind,name,part,"offset "+std::to_string(offset)+" is inside the header or directory or not "+
             std::to_string(SectionAlign)+"-byte aligned");
    if(offset>fileSize || length>fileSize-offset)
      reject(kind,name,part,"past the end of the file (offset "+std::to_string(offset)+", length "+
             std::to_string(length)+", file "+std::to_string(fileSize)+" bytes)");
    if(uint64_t(count)*stride!=length)
      reject(kind,name,part,std::to_string(count)+" records of "+std::to_string(stride)+" bytes do not fill "+
             std::to_string(length)+" bytes");
    out[type-1] = {offset,count};
    found[type-1] = 1;
    }
  for(size_t i=0;i<sectionSpecs.size();++i)
    if(!found[i])
      reject(kind,name,sectionSpecs[i].name,"missing");
  return out;
  }

std::vector<SectionView> readDirectory(std::span<const uint8_t> bytes, uint32_t headerSize, uint32_t sections,
                                       uint64_t directory, std::span<const SectionSpec> sectionSpecs,
                                       const std::string& kind, const std::string& name) {
  return readDirectoryImpl(bytes,bytes.size(),headerSize,sections,directory,sectionSpecs,kind,name);
  }

bool materialStringsFit(const Material& r, size_t stringBytes) {
  for(const auto* s:{&r.name,&r.texture,&r.detailObject})
    if(s->offset>stringBytes || s->size>stringBytes-s->offset)
      return false;
  return true;
  }

asset::Material readMaterial(const Material& r, std::span<const uint8_t> strings) {
  auto text = [&](const StringRef& ref) {
    return std::string(reinterpret_cast<const char*>(strings.data())+ref.offset,ref.size);
    };
  asset::Material m;
  m.name          = text(r.name);
  m.texture       = text(r.texture);
  m.detail_object = text(r.detailObject);
  m.smooth_angle                 = r.smoothAngle;
  m.texture_scale                = {r.textureScale[0],r.textureScale[1]};
  m.texture_anim_fps             = r.textureAnimFps;
  m.texture_anim_map_dir         = {r.textureAnimMapDir[0],r.textureAnimMapDir[1]};
  m.detail_object_scale          = r.detailObjectScale;
  m.environment_mapping_strength = r.environmentMappingStrength;
  m.wave_max_amplitude           = r.waveMaxAmplitude;
  m.wave_grid_size               = r.waveGridSize;
  m.default_mapping              = {r.defaultMapping[0],r.defaultMapping[1]};
  m.group                 = asset::MaterialGroup(r.group);
  m.color                 = {r.color[0],r.color[1],r.color[2],r.color[3]};
  m.texture_anim_map_mode = asset::AnimationMapping(r.textureAnimMapMode);
  m.wave_mode             = asset::WaveMode(r.waveMode);
  m.wave_speed            = asset::WaveSpeed(r.waveSpeed);
  m.alpha_func            = asset::AlphaFunction(r.alphaFunc);
  m.disable_collision     = (r.switches&DisableCollision)!=0;
  m.disable_lightmap      = (r.switches&DisableLightmap)!=0;
  m.dont_collapse         = (r.switches&DontCollapse)!=0;
  m.force_occluder        = (r.switches&ForceOccluder)!=0;
  m.environment_mapping   = (r.switches&EnvironmentMapping)!=0;
  m.ignore_sun            = (r.switches&IgnoreSun)!=0;
  return m;
  }

}

namespace {

void checkChunkPrologue(std::span<const uint8_t> bytes, uint64_t fileSize, const std::string& name) {
  wcb::checkPrologueImpl(bytes,fileSize,magic,wcb::Version,wcb::LegacyVersion,wcb::HeaderSize,alignof(uint32_t),what,name);
  }

std::vector<wcb::SectionView> chunkDirectory(std::span<const uint8_t> bytes, uint64_t fileSize, const std::string& name) {
  const auto* b = bytes.data();
  const bool hierarchyLayout = wcb::le32(b+4)>=wcb::OldHierarchyVersion;
  const auto offset   = wcb::le64(b+128);
  if(hierarchyLayout && offset!=wcb::HeaderSize)
    wcb::reject(what,name,"directory","the hierarchy layout requires the directory immediately after the header");
  const auto sectionSpecs = std::span<const wcb::SectionSpec>(specs,hierarchyLayout ? currentSections : baseSections);
  auto sections = wcb::readDirectoryImpl(bytes,fileSize,wcb::HeaderSize,wcb::le32(b+124),offset,sectionSpecs,what,name);
  if(hierarchyLayout) {
    uint64_t end = 0;
    for(size_t i=0;i<sections.size();++i) {
      const auto& section = sections[i];
      if(section.offset<end)
        wcb::reject(what,name,specs[i].name,"overlaps or precedes the previous section");
      end = section.offset+uint64_t(section.count)*specs[i].stride;
      }
    }
  return sections;
  }

bool validSphere(const ClusterLod::Sphere& s) {
  return std::isfinite(s.center[0]) && std::isfinite(s.center[1]) && std::isfinite(s.center[2]) &&
         std::isfinite(s.radius) && s.radius>=0.f;
  }

void checkHierarchy(const CookedChunk& chunk, const std::string& name) {
  const auto h = chunk.hierarchy();
  if(chunk.triangles().empty()!=h.clusters.empty())
    wcb::reject(what,name,"clusters","the hierarchy and the base mesh disagree about whether the chunk is empty");
  for(const auto& ref:h.vertices)
    if(ref.position>=chunk.positions().size() || ref.feature>=chunk.features().size())
      wcb::reject(what,name,"vertex refs","position or feature index out of range");
  for(const auto& g:h.groups)
    if(!validSphere(g.bounds) || !(g.errorCm>=0.f))
      wcb::reject(what,name,"groups","invalid bounds or geometric error");
  size_t vertexEnd = 0, indexEnd = 0;
  for(const auto& c:h.clusters) {
    if(!validSphere(c.bounds) || c.material>=chunk.materials().size())
      wcb::reject(what,name,"clusters","invalid bounds or material index");
    if(c.parentGroup>=h.groups.size() ||
       (c.level==0 ? c.refinedGroup!=ClusterLod::NoGroup :
                     c.refinedGroup>=c.parentGroup || !std::isfinite(h.groups[c.refinedGroup].errorCm) ||
                     h.groups[c.refinedGroup].errorCm>h.groups[c.parentGroup].errorCm))
      wcb::reject(what,name,"clusters","invalid refined or parent group");
    if(c.vertexOffset!=vertexEnd || c.vertexCount==0 || c.vertexCount>ClusterLod::MaxVertices ||
       c.vertexCount>h.vertexIndices.size()-vertexEnd)
      wcb::reject(what,name,"vertex indices","invalid cluster vertex range");
    const size_t indexCount = size_t(c.triangleCount)*3;
    if(c.indexOffset!=indexEnd || c.triangleCount==0 || c.triangleCount>ClusterLod::MaxTriangles ||
       indexCount>h.indices.size()-indexEnd)
      wcb::reject(what,name,"local indices","invalid cluster triangle range");
    for(const auto v:h.vertexIndices.subspan(vertexEnd,c.vertexCount))
      if(v>=h.vertices.size())
        wcb::reject(what,name,"vertex indices","graph vertex index out of range");
    for(const auto i:h.indices.subspan(indexEnd,indexCount))
      if(i>=c.vertexCount)
        wcb::reject(what,name,"local indices","meshlet-local vertex index out of range");
    vertexEnd += c.vertexCount;
    indexEnd  += indexCount;
    }
  if(vertexEnd!=h.vertexIndices.size())
    wcb::reject(what,name,"vertex indices","unused meshlet vertex indices");
  if(indexEnd!=h.indices.size())
    wcb::reject(what,name,"local indices","unused meshlet triangle indices");
  }

}

std::vector<uint8_t> encodeCookedChunk(const CookedChunkData& data, uint32_t version) {
  using wcb::count32;
  if(version!=wcb::Version && version!=wcb::LegacyVersion)
    throw std::runtime_error("cooked chunk: cannot write version "+std::to_string(version));
  const bool withHierarchy = version==wcb::Version;
  const uint32_t sectionCount = withHierarchy ? currentSections : baseSections;
  const auto& h = data.header;
  wcb::Writer w;
  w.out.reserve(wcb::HeaderSize+wcb::EntrySize*sectionCount+data.positions.size()*sizeof(wcb::Position)+
                data.features.size()*sizeof(wcb::Feature)+data.triangles.size()*sizeof(wcb::Triangle)+
                data.materials.size()*(sizeof(wcb::Material)+64)+wcb::SectionAlign*sectionCount+
                (withHierarchy ? data.hierarchy.byteSize() : 0));
  for(const auto c:magic)
    w.u8(uint8_t(c));
  w.u32(version);
  w.u32(wcb::ByteOrder);
  w.u32(wcb::HeaderSize);
  const size_t fileSizeAt = w.out.size();
  w.u64(0);
  w.i32(h.cell.x);
  w.i32(h.cell.z);
  w.u32(count32(data.positions.size(),"positions"));
  w.u32(count32(data.features.size(),"features"));
  w.u32(count32(data.triangles.size(),"triangles"));
  w.u32(count32(data.materials.size(),"materials"));
  for(const auto* v:{&h.min,&h.max})
    for(unsigned axis=0;axis<3;++axis)
      w.f32((*v)[axis]);
  w.u64(h.glbSize);
  for(const auto b:h.glbSha256)
    w.u8(b);
  w.u64(h.worldSize);
  w.u32(h.worldCrc32);
  w.u32(sectionCount);
  w.u64(wcb::HeaderSize);
  w.u64(0);
  if(w.out.size()!=wcb::HeaderSize)
    throw std::runtime_error("cooked chunk: header layout");

  w.directory(sectionCount);
  w.section(Positions,sizeof(wcb::Position),data.positions.size(),"positions",[&] {
    for(const auto& p:data.positions) {
      w.f32(p.x); w.f32(p.y); w.f32(p.z); w.u32(p.source);
      }
    });
  w.section(Features,sizeof(wcb::Feature),data.features.size(),"features",[&] {
    for(const auto& f:data.features) {
      w.f32(f.u); w.f32(f.v); w.u32(f.light); w.f32(f.nx); w.f32(f.ny); w.f32(f.nz); w.u32(f.source);
      }
    });
  w.section(Triangles,sizeof(wcb::Triangle),data.triangles.size(),"triangles",[&] {
    for(const auto& t:data.triangles) {
      for(const auto v:t.vertex)  w.u32(v);
      for(const auto f:t.feature) w.u32(f);
      w.u32(t.material); w.u32(t.order); w.i32(t.lightmap);
      w.u16(t.flags); w.u16(uint16_t(t.sector));
      }
    });
  std::string strings;
  w.section(Materials,sizeof(wcb::Material),data.materials.size(),"materials",[&] {
    for(const auto& [source,m]:data.materials)
      w.material(source,m,strings);
    });
  w.strings(Strings,strings);
  if(withHierarchy) {
    const auto& graph = data.hierarchy;
    const auto sphere = [&](const ClusterLod::Sphere& s) {
      for(const auto axis:s.center)
        w.f32(axis);
      w.f32(s.radius);
      };
    w.section(Clusters,sizeof(ClusterLod::Cluster),graph.clusters.size(),"clusters",[&] {
      for(const auto& c:graph.clusters) {
        sphere(c.bounds);
        w.u32(c.level); w.u32(c.material); w.u32(c.refinedGroup); w.u32(c.parentGroup);
        w.u32(c.vertexOffset); w.u32(c.indexOffset); w.u32(c.vertexCount); w.u32(c.triangleCount);
        }
      });
    w.section(Groups,sizeof(ClusterLod::Group),graph.groups.size(),"groups",[&] {
      for(const auto& g:graph.groups) {
        sphere(g.bounds);
        w.f32(g.errorCm);
        }
      });
    w.section(Indices,1,graph.indices.size(),"local indices",[&] {
      w.out.insert(w.out.end(),graph.indices.begin(),graph.indices.end());
      });
    w.section(VertexIndices,sizeof(uint32_t),graph.vertexIndices.size(),"vertex indices",[&] {
      for(const auto v:graph.vertexIndices)
        w.u32(v);
      });
    w.section(VertexRefs,sizeof(wcb::VertexRef),graph.vertices.size(),"vertex refs",[&] {
      for(const auto& ref:graph.vertices) {
        w.u32(ref.position);
        w.u32(ref.feature);
        }
      });
    }
  w.put64(fileSizeAt,w.out.size());
  return std::move(w.out);
  }

CookedChunk CookedChunk::open(std::span<const uint8_t> bytes, const std::string& name, std::shared_ptr<const void> owner,
                              bool readHierarchy) {
  checkChunkPrologue(bytes,bytes.size(),name);
  return openRecords(bytes,name,std::move(owner),chunkDirectory(bytes,bytes.size(),name),readHierarchy);
  }

CookedChunk CookedChunk::openRecords(std::span<const uint8_t> bytes, const std::string& name,
                                     std::shared_ptr<const void> owner, std::span<const wcb::SectionView> sections,
                                     bool readHierarchy) {
  const auto* b = bytes.data();
  CookedChunk c;
  c.owner = std::move(owner);
  c.bytes = bytes;
  c.formatVersion = wcb::le32(b+4);
  auto& h = c.head;
  h.cell      = {int32_t(wcb::le32(b+24)),int32_t(wcb::le32(b+28))};
  h.vertices  = wcb::le32(b+32);
  h.features  = wcb::le32(b+36);
  h.triangles = wcb::le32(b+40);
  h.materials = wcb::le32(b+44);
  for(unsigned axis=0;axis<3;++axis) {
    h.min[axis] = lef32(b+48+axis*4);
    h.max[axis] = lef32(b+60+axis*4);
    }
  h.glbSize = wcb::le64(b+72);
  std::memcpy(h.glbSha256.data(),b+80,32);
  h.worldSize  = wcb::le64(b+112);
  h.worldCrc32 = wcb::le32(b+120);
  const std::pair<uint32_t,uint32_t> counted[] = {{Positions,h.vertices},{Features,h.features},
                                                  {Triangles,h.triangles},{Materials,h.materials}};
  for(const auto& [type,count]:counted)
    if(sections[type-1].count!=count)
      wcb::reject(what,name,specs[type-1].name,"the header counts "+std::to_string(count)+", the section holds "+
                  std::to_string(sections[type-1].count));

  c.positionSpan = wcb::view<wcb::Position>(bytes,sections[Positions-1]);
  c.featureSpan  = wcb::view<wcb::Feature>(bytes,sections[Features-1]);
  c.triangleSpan = wcb::view<wcb::Triangle>(bytes,sections[Triangles-1]);
  c.materialSpan = wcb::view<wcb::Material>(bytes,sections[Materials-1]);
  c.stringSpan   = bytes.subspan(size_t(sections[Strings-1].offset),sections[Strings-1].count);

  const auto nv = c.positionSpan.size(), nf = c.featureSpan.size(), nm = c.materialSpan.size();
  for(size_t i=0;i<c.triangleSpan.size();++i) {
    const auto& t = c.triangleSpan[i];
    for(unsigned k=0;k<3;++k) {
      if(t.vertex[k]>=nv)
        wcb::reject(what,name,"triangles","triangle "+std::to_string(i)+" uses vertex "+std::to_string(t.vertex[k])+
                    " of "+std::to_string(nv));
      if(t.feature[k]>=nf)
        wcb::reject(what,name,"triangles","triangle "+std::to_string(i)+" uses feature "+std::to_string(t.feature[k])+
                    " of "+std::to_string(nf));
      }
    if(t.material>=nm)
      wcb::reject(what,name,"triangles","triangle "+std::to_string(i)+" uses material "+std::to_string(t.material)+
                  " of "+std::to_string(nm));
    if(t.order==wcb::Added)
      ++c.addedCount;
    }
  for(size_t i=0;i<nm;++i)
    if(!wcb::materialStringsFit(c.materialSpan[i],c.stringSpan.size()))
      wcb::reject(what,name,"materials","material "+std::to_string(i)+" has a string past the string section");
  if(readHierarchy && c.formatVersion==wcb::Version) {
    c.hierarchyView.vertices      = wcb::view<wcb::VertexRef>(bytes,sections[VertexRefs-1]);
    c.hierarchyView.clusters      = wcb::view<ClusterLod::Cluster>(bytes,sections[Clusters-1]);
    c.hierarchyView.groups        = wcb::view<ClusterLod::Group>(bytes,sections[Groups-1]);
    c.hierarchyView.vertexIndices = wcb::view<uint32_t>(bytes,sections[VertexIndices-1]);
    c.hierarchyView.indices       = wcb::view<uint8_t>(bytes,sections[Indices-1]);
    checkHierarchy(c,name);
    }
  return c;
  }

CookedChunk CookedChunk::read(const std::filesystem::path& path, bool readHierarchy) {
  const auto name = path.string();
#ifdef _WIN32
  const std::unique_ptr<std::FILE,decltype(&std::fclose)> file(_wfopen(path.c_str(),L"rb"),std::fclose);
#else
  const std::unique_ptr<std::FILE,decltype(&std::fclose)> file(std::fopen(path.c_str(),"rb"),std::fclose);
#endif
  if(!file)
    wcb::reject(what,name,"header","cannot open the file");
  // Use bulk reads without buffering past the end of the base section prefix.
  std::setvbuf(file.get(),nullptr,_IONBF,0);
#ifdef _WIN32
  const auto end = _fseeki64(file.get(),0,SEEK_END)==0 ? _ftelli64(file.get()) : -1;
#else
  const auto end = fseeko(file.get(),0,SEEK_END)==0 ? ftello(file.get()) : -1;
#endif
  if(end<0 || uint64_t(end)>std::numeric_limits<size_t>::max() || std::fseek(file.get(),0,SEEK_SET)!=0)
    wcb::reject(what,name,"header","cannot determine the file size");
  const auto fileSize = size_t(end);
  auto bytes = std::make_shared<std::vector<uint8_t>>();
  const auto extend = [&](size_t size, const char* part) {
    const auto start = bytes->size();
    bytes->resize(size);
    if(size>start && std::fread(bytes->data()+start,1,size-start,file.get())!=size-start)
      wcb::reject(what,name,part,"cannot read the requested bytes");
    };
  extend(std::min(fileSize,size_t(wcb::HeaderSize)),"header");
  checkChunkPrologue(*bytes,fileSize,name);
  const auto version = wcb::le32(bytes->data()+4);
  if(version==wcb::LegacyVersion || (version==wcb::Version && readHierarchy)) {
    extend(fileSize,"sections");
    const std::span<const uint8_t> view(*bytes);
    return openRecords(view,name,std::move(bytes),chunkDirectory(view,fileSize,name),readHierarchy);
    }

  const size_t directoryEnd = wcb::HeaderSize + currentSections*wcb::EntrySize;
  extend(std::min(fileSize,directoryEnd),"directory");
  const auto sections = chunkDirectory(*bytes,fileSize,name);
  const auto& strings = sections[Strings-1];
  extend(size_t(strings.offset)+strings.count,"base sections");
  const std::span<const uint8_t> view(bytes->data(),bytes->size());
  return openRecords(view,name,std::move(bytes),sections,false);
  }

std::string_view CookedChunk::string(const wcb::StringRef& ref) const {
  return {reinterpret_cast<const char*>(stringSpan.data())+ref.offset,ref.size};
  }

asset::Material CookedChunk::material(size_t i) const {
  return wcb::readMaterial(materialSpan[i],stringSpan);
  }

std::array<uint8_t,32> sha256Bytes(std::span<const uint8_t> bytes) {
  Sha256 hash;
  hash.update(bytes.data(),bytes.size());
  const auto hex = hash.finish();
  std::array<uint8_t,32> out {};
  auto nibble = [](char c) {
    return uint8_t(c<='9' ? c-'0' : (c|0x20)-'a'+10);
    };
  for(size_t i=0;i<out.size();++i)
    out[i] = uint8_t((nibble(hex[i*2])<<4) | nibble(hex[i*2+1]));
  return out;
  }

std::string cookedChunkStale(const CookedChunk& chunk, const std::filesystem::path& glb, bool hash) {
  std::error_code error;
  const auto size = std::filesystem::file_size(glb,error);
  if(error)
    return glb.string()+": "+error.message();
  if(size!=chunk.header().glbSize)
    return glb.string()+" is "+std::to_string(size)+" bytes, the chunk was cooked from "+
           std::to_string(chunk.header().glbSize)+" bytes";
  if(hash) {
    const auto bytes = gothicglb::readFile(glb);
    if(sha256Bytes(bytes)!=chunk.header().glbSha256)
      return glb.string()+" has another SHA-256 than the GLB the chunk was cooked from";
    }
  return {};
  }
