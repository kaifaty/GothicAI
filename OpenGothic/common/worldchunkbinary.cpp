#include "worldchunkbinary.h"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>

#include "gothicglb.h"
#include "sha256.h"

namespace {

static_assert(std::endian::native==std::endian::little,"cooked chunks are viewed in place: a little-endian host is required");

// The structs view the bytes of the file: their layouts must be the table of worldchunkbinary.h.
static_assert(sizeof(wcb::Position)==16 && offsetof(wcb::Position,id)==12);
static_assert(sizeof(wcb::Feature)==28 && offsetof(wcb::Feature,light)==8 && offsetof(wcb::Feature,nx)==12 &&
              offsetof(wcb::Feature,id)==24);
static_assert(sizeof(wcb::Triangle)==44 && offsetof(wcb::Triangle,feature)==12 && offsetof(wcb::Triangle,material)==24 &&
              offsetof(wcb::Triangle,order)==28 && offsetof(wcb::Triangle,rank)==32 && offsetof(wcb::Triangle,lightmap)==36 &&
              offsetof(wcb::Triangle,flags)==40 && offsetof(wcb::Triangle,sector)==42);
static_assert(sizeof(wcb::Material)==88 && offsetof(wcb::Material,name)==4 && offsetof(wcb::Material,texture)==12 &&
              offsetof(wcb::Material,detailObject)==20 && offsetof(wcb::Material,smoothAngle)==28 &&
              offsetof(wcb::Material,defaultMapping)==68 && offsetof(wcb::Material,group)==76 &&
              offsetof(wcb::Material,color)==77 && offsetof(wcb::Material,alphaFunc)==84 &&
              offsetof(wcb::Material,switches)==85);

constexpr char     magic[4]       = {'G','W','C','B'};
constexpr uint32_t byteOrder      = 0x01020304u;
constexpr uint32_t entrySize      = 32;
constexpr uint32_t maxSections    = 16;
constexpr uint32_t sectionAlign   = 16;

enum Section : uint32_t { Positions=1, Features=2, Triangles=3, Materials=4, Strings=5, SectionEnd=6 };

const char* sectionName(uint32_t type) {
  switch(type) {
    case Positions: return "positions";
    case Features:  return "features";
    case Triangles: return "triangles";
    case Materials: return "materials";
    case Strings:   return "strings";
    }
  return "?";
  }

uint32_t sectionStride(uint32_t type) {
  switch(type) {
    case Positions: return sizeof(wcb::Position);
    case Features:  return sizeof(wcb::Feature);
    case Triangles: return sizeof(wcb::Triangle);
    case Materials: return sizeof(wcb::Material);
    case Strings:   return 1;
    }
  return 0;
  }

[[noreturn]] void reject(const std::string& name, const std::string& section, const std::string& reason) {
  throw std::runtime_error("cooked chunk "+name+": "+section+": "+reason);
  }

class Writer {
  public:
    std::vector<uint8_t> out;

    void u8(uint8_t v) { out.push_back(v); }
    void u16(uint16_t v) { u8(uint8_t(v)); u8(uint8_t(v>>8)); }
    void u32(uint32_t v) { for(unsigned i=0;i<4;++i) u8(uint8_t(v>>(8*i))); }
    void u64(uint64_t v) { for(unsigned i=0;i<8;++i) u8(uint8_t(v>>(8*i))); }
    void i32(int32_t v)  { u32(uint32_t(v)); }
    void f32(float v)    { u32(std::bit_cast<uint32_t>(v)); }
    void align() { while(out.size()%sectionAlign!=0) u8(0); }
    void put32(size_t at, uint32_t v) { for(unsigned i=0;i<4;++i) out[at+i]=uint8_t(v>>(8*i)); }
    void put64(size_t at, uint64_t v) { for(unsigned i=0;i<8;++i) out[at+i]=uint8_t(v>>(8*i)); }
  };

uint32_t count32(size_t n, const char* what) {
  if(n>std::numeric_limits<uint32_t>::max())
    throw std::runtime_error(std::string("cooked chunk: too many ")+what);
  return uint32_t(n);
  }

uint8_t enum8(int64_t v, const char* what) {
  if(v<0 || v>0xFF)
    throw std::runtime_error(std::string("cooked chunk: material ")+what+" "+std::to_string(v)+" does not fit a byte");
  return uint8_t(v);
  }

uint32_t le32(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1])<<8) | (uint32_t(p[2])<<16) | (uint32_t(p[3])<<24); }
uint64_t le64(const uint8_t* p) { return uint64_t(le32(p)) | (uint64_t(le32(p+4))<<32); }
float    lef32(const uint8_t* p) { return std::bit_cast<float>(le32(p)); }

template<class T>
std::span<const T> view(std::span<const uint8_t> bytes, uint64_t offset, uint32_t count) {
  return {reinterpret_cast<const T*>(bytes.data()+offset),count};
  }

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

}

std::vector<uint8_t> encodeCookedChunk(const CookedChunkData& data) {
  const auto& h = data.header;
  Writer w;
  w.out.reserve(wcb::HeaderSize+entrySize*5+data.positions.size()*sizeof(wcb::Position)+
                data.features.size()*sizeof(wcb::Feature)+data.triangles.size()*sizeof(wcb::Triangle)+
                data.materials.size()*(sizeof(wcb::Material)+64)+16*6);
  for(const auto c:magic)
    w.u8(uint8_t(c));
  w.u32(wcb::Version);
  w.u32(byteOrder);
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
  w.u32(SectionEnd-1);
  w.u64(wcb::HeaderSize);
  w.u64(0);
  if(w.out.size()!=wcb::HeaderSize)
    throw std::runtime_error("cooked chunk: header layout");

  const size_t directory = w.out.size();
  w.out.resize(directory+entrySize*(SectionEnd-1),0);
  auto section = [&](uint32_t type, size_t count, auto&& body) {
    w.align();
    const size_t begin = w.out.size();
    body();
    const size_t at = directory+(type-1)*entrySize;
    w.put32(at,type);
    w.put32(at+4,sectionStride(type));
    w.put32(at+8,count32(count,sectionName(type)));
    w.put64(at+16,begin);
    w.put64(at+24,w.out.size()-begin);
    };

  section(Positions,data.positions.size(),[&] {
    for(const auto& p:data.positions) {
      w.f32(p.x); w.f32(p.y); w.f32(p.z); w.u32(p.id);
      }
    });
  section(Features,data.features.size(),[&] {
    for(const auto& f:data.features) {
      w.f32(f.u); w.f32(f.v); w.u32(f.light); w.f32(f.nx); w.f32(f.ny); w.f32(f.nz); w.u32(f.id);
      }
    });
  section(Triangles,data.triangles.size(),[&] {
    for(const auto& t:data.triangles) {
      for(const auto v:t.vertex)  w.u32(v);
      for(const auto f:t.feature) w.u32(f);
      w.u32(t.material); w.u32(t.order); w.u32(t.rank); w.i32(t.lightmap);
      w.u16(t.flags); w.u16(uint16_t(t.sector));
      }
    });
  std::string strings;
  auto ref = [&](const std::string& s) {
    const wcb::StringRef r {count32(strings.size(),"string bytes"),count32(s.size(),"string bytes")};
    strings += s;
    return r;
    };
  section(Materials,data.materials.size(),[&] {
    for(const auto& [id,m]:data.materials) {
      w.u32(id);
      for(const auto& s:{&m.name,&m.texture,&m.detail_object}) {
        const auto r = ref(*s);
        w.u32(r.offset);
        w.u32(r.size);
        }
      for(const float f:{m.smooth_angle,m.texture_scale.x,m.texture_scale.y,m.texture_anim_fps,
                         m.texture_anim_map_dir.x,m.texture_anim_map_dir.y,m.detail_object_scale,
                         m.environment_mapping_strength,m.wave_max_amplitude,m.wave_grid_size,
                         m.default_mapping.x,m.default_mapping.y})
        w.f32(f);
      w.u8(enum8(int64_t(m.group),"group"));
      for(unsigned c=0;c<4;++c)
        w.u8(m.color[c]);
      w.u8(enum8(int64_t(m.texture_anim_map_mode),"texture_anim_map_mode"));
      w.u8(enum8(int64_t(m.wave_mode),"wave_mode"));
      w.u8(enum8(int64_t(m.wave_speed),"wave_speed"));
      w.u8(enum8(int64_t(m.alpha_func),"alpha_func"));
      w.u8(uint8_t((m.disable_collision ? wcb::DisableCollision : 0) | (m.disable_lightmap ? wcb::DisableLightmap : 0) |
                   (m.dont_collapse ? wcb::DontCollapse : 0) | (m.force_occluder ? wcb::ForceOccluder : 0) |
                   (m.environment_mapping ? wcb::EnvironmentMapping : 0) | (m.ignore_sun ? wcb::IgnoreSun : 0)));
      w.u8(0);
      w.u8(0);
      }
    });
  section(Strings,strings.size(),[&] {
    w.out.insert(w.out.end(),strings.begin(),strings.end());
    });
  w.put64(fileSizeAt,w.out.size());
  return std::move(w.out);
  }

CookedChunk CookedChunk::open(std::span<const uint8_t> bytes, const std::string& name, std::shared_ptr<const void> owner) {
  if(bytes.size()<wcb::HeaderSize)
    reject(name,"header","truncated: "+std::to_string(bytes.size())+" bytes, the header alone is "+
           std::to_string(wcb::HeaderSize));
  const auto* b = bytes.data();
  if(std::memcmp(b,magic,4)!=0)
    reject(name,"header","wrong magic, not a cooked chunk");
  if(le32(b+4)!=wcb::Version)
    reject(name,"header","version "+std::to_string(le32(b+4))+", this reader reads "+std::to_string(wcb::Version));
  if(le32(b+8)!=byteOrder)
    reject(name,"header","byte order mark is not little-endian");
  if(le32(b+12)!=wcb::HeaderSize)
    reject(name,"header","header size "+std::to_string(le32(b+12))+", expected "+std::to_string(wcb::HeaderSize));
  const uint64_t size = le64(b+16);
  if(size>bytes.size())
    reject(name,"header","truncated: "+std::to_string(bytes.size())+" of "+std::to_string(size)+" bytes");
  if(size<bytes.size())
    reject(name,"header",std::to_string(bytes.size())+" bytes, the header says "+std::to_string(size));
  if(reinterpret_cast<uintptr_t>(b)%alignof(uint32_t)!=0)
    reject(name,"header","bytes are not 4-byte aligned in memory");

  CookedChunk c;
  c.owner = std::move(owner);
  c.bytes = bytes;
  auto& h = c.head;
  h.cell      = {int32_t(le32(b+24)),int32_t(le32(b+28))};
  h.vertices  = le32(b+32);
  h.features  = le32(b+36);
  h.triangles = le32(b+40);
  h.materials = le32(b+44);
  for(unsigned axis=0;axis<3;++axis) {
    h.min[axis] = lef32(b+48+axis*4);
    h.max[axis] = lef32(b+60+axis*4);
    }
  h.glbSize = le64(b+72);
  std::memcpy(h.glbSha256.data(),b+80,32);
  h.worldSize  = le64(b+112);
  h.worldCrc32 = le32(b+120);
  const uint32_t sections = le32(b+124);
  const uint64_t directory = le64(b+128);
  if(sections>maxSections)
    reject(name,"directory",std::to_string(sections)+" sections, at most "+std::to_string(maxSections));
  if(directory<wcb::HeaderSize || directory>size || (size-directory)/entrySize<sections)
    reject(name,"directory","past the end of the file");
  const uint64_t directoryEnd = directory+uint64_t(sections)*entrySize;

  struct Entry { uint32_t count = 0; uint64_t offset = 0; bool found = false; };
  Entry entries[SectionEnd] = {};
  for(uint32_t i=0;i<sections;++i) {
    const auto* e = b+directory+uint64_t(i)*entrySize;
    const uint32_t type = le32(e);
    if(type==0 || type>=SectionEnd)
      reject(name,"directory","unknown section type "+std::to_string(type));
    const std::string what = sectionName(type);
    auto& entry = entries[type];
    if(entry.found)
      reject(name,what,"listed twice");
    const uint32_t stride = le32(e+4), count = le32(e+8);
    const uint64_t offset = le64(e+16), length = le64(e+24);
    if(stride!=sectionStride(type))
      reject(name,what,"stride "+std::to_string(stride)+", expected "+std::to_string(sectionStride(type)));
    if(offset<directoryEnd || offset%sectionAlign!=0)
      reject(name,what,"offset "+std::to_string(offset)+" is inside the header or directory or not "+
             std::to_string(sectionAlign)+"-byte aligned");
    if(offset>size || length>size-offset)
      reject(name,what,"past the end of the file (offset "+std::to_string(offset)+", length "+std::to_string(length)+
             ", file "+std::to_string(size)+" bytes)");
    if(uint64_t(count)*stride!=length)
      reject(name,what,std::to_string(count)+" records of "+std::to_string(stride)+" bytes do not fill "+
             std::to_string(length)+" bytes");
    entry = {count,offset,true};
    }
  for(uint32_t type=1;type<SectionEnd;++type)
    if(!entries[type].found)
      reject(name,sectionName(type),"missing");
  const std::pair<uint32_t,uint32_t> counted[] = {{Positions,h.vertices},{Features,h.features},
                                                  {Triangles,h.triangles},{Materials,h.materials}};
  for(const auto& [type,count]:counted)
    if(entries[type].count!=count)
      reject(name,sectionName(type),"the header counts "+std::to_string(count)+", the section holds "+
             std::to_string(entries[type].count));

  c.positionSpan = view<wcb::Position>(bytes,entries[Positions].offset,entries[Positions].count);
  c.featureSpan  = view<wcb::Feature>(bytes,entries[Features].offset,entries[Features].count);
  c.triangleSpan = view<wcb::Triangle>(bytes,entries[Triangles].offset,entries[Triangles].count);
  c.materialSpan = view<wcb::Material>(bytes,entries[Materials].offset,entries[Materials].count);
  c.stringSpan   = bytes.subspan(size_t(entries[Strings].offset),entries[Strings].count);

  const auto nv = c.positionSpan.size(), nf = c.featureSpan.size();
  for(size_t i=0;i<c.triangleSpan.size();++i) {
    const auto& t = c.triangleSpan[i];
    for(unsigned k=0;k<3;++k) {
      if(t.vertex[k]>=nv)
        reject(name,"triangles","triangle "+std::to_string(i)+" uses vertex "+std::to_string(t.vertex[k])+" of "+
               std::to_string(nv));
      if(t.feature[k]>=nf)
        reject(name,"triangles","triangle "+std::to_string(i)+" uses feature "+std::to_string(t.feature[k])+" of "+
               std::to_string(nf));
      }
    }
  for(size_t i=0;i<c.materialSpan.size();++i) {
    const auto& m = c.materialSpan[i];
    for(const auto* r:{&m.name,&m.texture,&m.detailObject})
      if(r->offset>c.stringSpan.size() || r->size>c.stringSpan.size()-r->offset)
        reject(name,"materials","material "+std::to_string(i)+" has a string past the string section");
    if(i>0 && m.id<=c.materialSpan[i-1].id)
      reject(name,"materials","material ids must be ascending");
    }
  return c;
  }

CookedChunk CookedChunk::read(const std::filesystem::path& path) {
  auto bytes = std::make_shared<const std::vector<uint8_t>>(gothicglb::readFile(path));
  const std::span<const uint8_t> view(bytes->data(),bytes->size());
  return open(view,path.string(),std::move(bytes));
  }

std::string_view CookedChunk::string(const wcb::StringRef& ref) const {
  return {reinterpret_cast<const char*>(stringSpan.data())+ref.offset,ref.size};
  }

asset::Material CookedChunk::material(size_t i) const {
  const auto& r = materialSpan[i];
  asset::Material m;
  m.name          = std::string(string(r.name));
  m.texture       = std::string(string(r.texture));
  m.detail_object = std::string(string(r.detailObject));
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
  m.disable_collision     = (r.switches&wcb::DisableCollision)!=0;
  m.disable_lightmap      = (r.switches&wcb::DisableLightmap)!=0;
  m.dont_collapse         = (r.switches&wcb::DontCollapse)!=0;
  m.force_occluder        = (r.switches&wcb::ForceOccluder)!=0;
  m.environment_mapping   = (r.switches&wcb::EnvironmentMapping)!=0;
  m.ignore_sun            = (r.switches&wcb::IgnoreSun)!=0;
  return m;
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
