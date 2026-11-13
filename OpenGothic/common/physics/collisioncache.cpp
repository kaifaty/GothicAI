#include "collisioncache.h"

#include "physicvbo.h"
#include "sha256.h"
#include "staticcollision.h"

#include <Jolt/Core/StreamIn.h>
#include <Jolt/Core/StreamOut.h>
#include <Jolt/Core/UnorderedMap.h>
#include <Jolt/Physics/Collision/PhysicsMaterial.h>
#include <Tempest/Log>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <random>
#include <stdexcept>
#include <unordered_map>

namespace {
namespace fs = std::filesystem;
constexpr std::string_view LegacyMagic = "GOTHIC-COLLISION-";
constexpr size_t IdentityAt = CollisionCache::Magic.size(), ChecksumAt = 88, SizesAt = 96, Sizes = 12;
constexpr uint32_t NoSector = ~uint32_t(0);
constexpr size_t SegmentBytes = 24;
constexpr auto JoltVersion=[] { using namespace JPH; return JPH_VERSION_ID; }();
#if defined(JPH_CPU_ARM)
constexpr std::string_view Architecture="arm";
#else
constexpr std::string_view Architecture="x86";
#endif
static_assert(IdentityAt+64<=ChecksumAt && SizesAt+Sizes*8==CollisionCache::HeaderSize);

// The section sizes of the header, as counts.
enum Size : size_t { NameBytes, Names, Vertices, LandIndices, LandSegments, LandGroups, LandMembers,
                     WaterIndices, WaterSegments, WaterGroups, WaterMembers, ShapeBytes };

struct Failure : std::runtime_error {
  using std::runtime_error::runtime_error;
  };

[[noreturn]] void fail(const std::string& reason) {
  throw Failure(reason);
  }

// XXH64 with seed 0, streamed; native reads (the identity names the endianness).
class Hash64 final {
  public:
    void update(const void* data, size_t size) {
      auto* p = static_cast<const uint8_t*>(data);
      total += size;
      if(buffered+size<32) {
        std::memcpy(buffer+buffered,p,size);
        buffered += size;
        return;
        }
      if(buffered>0) {
        const size_t fill = 32-buffered;
        std::memcpy(buffer+buffered,p,fill);
        stripe(buffer);
        p += fill;
        size -= fill;
        buffered = 0;
        }
      for(;size>=32;p+=32,size-=32)
        stripe(p);
      std::memcpy(buffer,p,size);
      buffered = size;
      }
    uint64_t finish() const {
      uint64_t h = total>=32 ? std::rotl(v[0],1)+std::rotl(v[1],7)+std::rotl(v[2],12)+std::rotl(v[3],18) : v[2]+P5;
      if(total>=32)
        for(const auto lane:v)
          h = (h^round(0,lane))*P1+P4;
      h += total;
      const uint8_t* p = buffer;
      size_t left = buffered;
      for(;left>=8;p+=8,left-=8)
        h = std::rotl(h^round(0,load<uint64_t>(p)),27)*P1+P4;
      if(left>=4) {
        h = std::rotl(h^(uint64_t(load<uint32_t>(p))*P1),23)*P2+P3;
        p += 4;
        left -= 4;
        }
      for(;left>0;++p,--left)
        h = std::rotl(h^(uint64_t(*p)*P5),11)*P1;
      h ^= h>>33;
      h *= P2;
      h ^= h>>29;
      h *= P3;
      return h^(h>>32);
      }

  private:
    static constexpr uint64_t P1=0x9E3779B185EBCA87ull, P2=0xC2B2AE3D27D4EB4Full, P3=0x165667B19E3779F9ull,
                              P4=0x85EBCA77C2B2AE63ull, P5=0x27D4EB2F165667C5ull;
    template<class T> static T load(const uint8_t* p) { T x; std::memcpy(&x,p,sizeof(x)); return x; }
    static uint64_t round(uint64_t acc, uint64_t input) { return std::rotl(acc+input*P2,31)*P1; }
    void stripe(const uint8_t* p) {
      for(size_t i=0;i<4;++i)
        v[i] = round(v[i],load<uint64_t>(p+8*i));
      }

    uint64_t v[4] = {P1+P2,P2,0,0-P1};
    uint8_t  buffer[32] = {};
    size_t   buffered = 0;
    uint64_t total = 0;
  };

class Reader final {
  public:
    Reader(const fs::path& file, Hash64& hash):input(file,std::ios::binary),hash(hash) {
      if(!input)
        fail("cannot open the file");
      }
    void raw(void* data, size_t size) {
      if(size>0 && !input.read(static_cast<char*>(data),std::streamsize(size)))
        fail("read failed");
      }
    void bytes(void* data, size_t size) {
      raw(data,size);
      hash.update(data,size);
      }
    template<class T> void vector(std::vector<T>& out, size_t count) {
      out.resize(count);
      bytes(out.data(),count*sizeof(T));
      }

  private:
    std::ifstream input;
    Hash64&       hash;
  };

class Writer final {
  public:
    explicit Writer(std::ofstream& file):file(file) {}
    void bytes(const void* data, size_t size) {
      file.write(static_cast<const char*>(data),std::streamsize(size));
      hash.update(data,size);
      }
    template<class T> void value(T v) { bytes(&v,sizeof(v)); }
    Hash64 hash;

  private:
    std::ofstream& file;
  };

class ShapeInput final : public JPH::StreamIn {
  public:
    explicit ShapeInput(std::string_view bytes):bytes(bytes) {}
    void ReadBytes(void* data, size_t size) override {
      if(size>bytes.size())
        fail("the shapes end early");
      std::memcpy(data,bytes.data(),size);
      bytes.remove_prefix(size);
      }
    bool IsEOF() const override { return false; }
    bool IsFailed() const override { return false; }
    bool empty() const { return bytes.empty(); }

  private:
    std::string_view bytes;
  };

class ShapeOutput final : public JPH::StreamOut {
  public:
    explicit ShapeOutput(Writer& out):out(out) {}
    void WriteBytes(const void* data, size_t size) override {
      out.bytes(data,size);
      written += size;
      }
    bool IsFailed() const override { return false; }
    uint64_t written = 0;

  private:
    Writer& out;
  };

bool validGroup(uint32_t group) {
  return group<=uint32_t(asset::MaterialGroup::CLAY) || group==uint32_t(asset::MaterialGroup::NONE);
  }

std::string count(uint64_t value) {
  return std::to_string(value);
  }

}

// What one mesh holds in the file, read and checked before it is handed to PhysicVbo.
struct CollisionCacheMesh {
  const char*                        name = "";
  std::vector<uint32_t>              indices, sectorIds, groupIds, groups, members;
  std::vector<PhysicVbo::Segment>    segments;
  std::vector<std::vector<uint32_t>> duplicates;

  // size: the counts of its indices, segments, duplicate groups and members.
  void read(Reader& in, const uint64_t* size) {
    in.vector(indices,size_t(size[0]));
    std::vector<uint8_t> raw;
    in.vector(raw,size_t(size[1])*SegmentBytes);
    in.vector(groups,size_t(size[2]));
    in.vector(members,size_t(size[3]));
    segments.resize(size_t(size[1]));
    sectorIds.resize(segments.size());
    groupIds.resize(segments.size());
    for(size_t i=0;i<segments.size();++i) {
      const auto* at = raw.data()+i*SegmentBytes;
      uint64_t offset = 0, triangles = 0;
      std::memcpy(&offset,at,8);
      std::memcpy(&triangles,at+8,8);
      std::memcpy(&groupIds[i],at+16,4);
      std::memcpy(&sectorIds[i],at+20,4);
      segments[i].off  = size_t(offset);
      segments[i].size = size_t(triangles);
      }
    }

  // After the checksum: the ranges that make a wrong file harmless.
  void check(size_t sectors, size_t vertices) {
    const std::string mesh = name;
    for(size_t i=0;i<indices.size();++i)
      if(indices[i]>=vertices)
        fail(mesh+" index "+count(i)+" is "+count(indices[i])+", the vertices are "+count(vertices));
    size_t end = 0;
    for(size_t i=0;i<segments.size();++i) {
      auto& s = segments[i];
      const auto at = mesh+" segment "+count(i);
      if(s.off>indices.size() || s.size>(indices.size()-s.off)/3)
        fail(at+" runs past its "+count(indices.size())+" indices (offset "+count(s.off)+", "+count(s.size)+
             " triangles)");
      if(s.off!=end)
        fail(at+" starts at index "+count(s.off)+", the one before ends at "+count(end));
      end = s.off+3*s.size;
      if(!validGroup(groupIds[i]))
        fail(at+" has material group "+count(groupIds[i]));
      s.mat = asset::MaterialGroup(groupIds[i]);
      if(sectorIds[i]!=NoSector && sectorIds[i]>=sectors)
        fail(at+" names sector "+count(sectorIds[i])+" of "+count(sectors));
      }
    if(end!=indices.size())
      fail(mesh+": the segments cover "+count(end)+" of its "+count(indices.size())+" indices");
    const size_t triangles = indices.size()/3;
    std::vector<bool> seen(triangles,false);
    size_t at = 0;
    for(size_t g=0;g<groups.size();++g) {
      if(groups[g]<2 || groups[g]>members.size()-at)
        fail(mesh+" duplicate group "+count(g)+" has "+count(groups[g])+" members, "+count(members.size()-at)+
             " are left");
      auto& group = duplicates.emplace_back(members.begin()+std::ptrdiff_t(at),
                                            members.begin()+std::ptrdiff_t(at+groups[g]));
      at += groups[g];
      for(const auto triangle:group) {
        if(triangle>=triangles || seen[triangle])
          fail(mesh+" duplicate group "+count(g)+" names triangle "+count(triangle)+
               (triangle>=triangles ? " of "+count(triangles) : " twice"));
        seen[triangle] = true;
        }
      }
    if(at!=members.size())
      fail(mesh+": the duplicate groups name "+count(at)+" of "+count(members.size())+" members");
    }

  // The sector pointers into the names out holds.
  void bind(const std::vector<std::string>& sectors) {
    for(size_t i=0;i<segments.size();++i)
      segments[i].sector = sectorIds[i]==NoSector ? nullptr : sectors[sectorIds[i]].c_str();
    }
  };

CollisionCache::CollisionCache(const fs::path& cacheRoot, const Key& key) {
  const auto* enabled=std::getenv("OPENGOTHIC_COLLISION_CACHE");
  if((enabled && std::string_view(enabled)=="0") || cacheRoot.empty() || key.base.empty() || key.content.empty())
    return;
  if(key.base.find_first_not_of("0123456789abcdef")!=std::string::npos ||
     key.world.empty() || key.world.find_first_of("/\\")!=std::string::npos || key.world=="..")
    return;
  // Jolt serializes native mesh blocks. Keep differing versions, feature flags,
  // endianness and word sizes out of the same derived file.
  const std::string bytes=key.base+"\n"+key.world+"\n"+key.content+"\n"+
      std::to_string(key.collisionFlags)+"\n"+key.clothRules+"\n"+std::to_string(key.version)+"\n"+
      std::to_string(JoltVersion)+"\n"+std::to_string(sizeof(size_t))+"\n"+
      std::to_string(sizeof(JPH::Vec3))+"\n"+std::string(Architecture)+"\n"+
      (std::endian::native==std::endian::little ? "le" : "be");
  identity=Sha256::hash(bytes);
  file=cacheRoot/"derived"/key.base/(key.world+".collision");
  }

uint64_t CollisionCache::checksum(const void* data, size_t size) {
  Hash64 hash;
  hash.update(data,size);
  return hash.finish();
  }

bool CollisionCache::read(StaticCollision& out) const {
  if(file.empty())
    return false;
  std::string reason;
  try {
    std::error_code error;
    const auto fileSize=fs::file_size(file,error);
    if(error) {
      // A miss as before, unless an interrupted write left its temporary file: name it, and remove it once it is
      // old enough not to belong to a writer of another process.
      const auto prefix=file.filename().string()+".tmp-";
      std::vector<std::string> left;
      for(const auto& entry:fs::directory_iterator(file.parent_path(),error)) {
        const auto name=entry.path().filename().string();
        if(!name.starts_with(prefix))
          continue;
        std::error_code ignored;
        const auto written=fs::last_write_time(entry.path(),ignored);
        const bool stale=!ignored && fs::file_time_type::clock::now()-written>std::chrono::minutes(10);
        if(stale)
          fs::remove(entry.path(),ignored);
        left.push_back(name+(stale ? " (removed)" : " (kept: younger than 10 minutes)"));
        }
      if(left.empty())
        return false;
      reason="missing; leftover temporary files of an interrupted write:";
      for(const auto& name:left)
        reason+=" "+name;
      fail(reason);
      }

    Hash64 hash;
    Reader in(file,hash);
    std::array<char,HeaderSize> header {};
    const auto head=std::min<uintmax_t>(fileSize,HeaderSize);
    in.raw(header.data(),size_t(head));
    const std::string_view magic(header.data(),std::min(head,uintmax_t(Magic.size())));
    if(magic.starts_with(LegacyMagic) && magic!=Magic)
      fail("format "+std::string(magic.substr(0,magic.find('\n')))+", this build reads "+
           std::string(Magic.substr(0,Magic.size()-1))+" (version 1 held the shapes alone)");
    if(magic!=Magic)
      fail("not a collision cache file");
    if(fileSize<HeaderSize)
      fail("truncated: "+count(fileSize)+" bytes, the header alone has "+count(HeaderSize));
    if(std::string_view(header.data()+IdentityAt,64)!=identity)
      fail("built for another key (base, world, content, collision flags, cloth rules, bake version or Jolt build)");
    uint64_t stored=0, size[Sizes]={};
    std::memcpy(&stored,header.data()+ChecksumAt,8);
    std::memcpy(size,header.data()+SizesAt,sizeof(size));
    // The sizes against the file size, each bounded before it is multiplied.
    // Bytes per entry; the names are counted in bytes, their number is checked when they are parsed.
    const uint64_t each[Sizes]={1,0,12,4,SegmentBytes,4,4,4,SegmentBytes,4,4,1};
    uint64_t expected=HeaderSize;
    for(size_t i=0;i<Sizes;++i) {
      if(each[i]!=0 && size[i]>fileSize/each[i])
        fail("truncated or damaged: section "+count(i)+" claims "+count(size[i])+" entries in a file of "+
             count(fileSize)+" bytes");
      expected+=size[i]*each[i];
      }
    if(size[Names]>size[NameBytes]/4)
      fail("the header lists "+count(size[Names])+" sector names in "+count(size[NameBytes])+" bytes");
    if(expected!=fileSize)
      fail("truncated or padded: "+count(fileSize)+" bytes, the sections need "+count(expected));
    if(size[Vertices]>std::numeric_limits<uint32_t>::max() || size[LandIndices]%3!=0 || size[WaterIndices]%3!=0)
      fail("the vertex or index counts are not those of triangle lists");

    std::vector<uint8_t> names;
    in.vector(names,size_t(size[NameBytes]));
    std::vector<std::string> sectors;
    std::vector<JPH::Vec3> vertices(size_t(size[Vertices]));
    {
    std::array<float,3*4096> block;
    for(size_t at=0;at<vertices.size();) {
      const size_t n=std::min<size_t>(4096,vertices.size()-at);
      in.bytes(block.data(),n*12);
      for(size_t i=0;i<n;++i)
        vertices[at+i]=JPH::Vec3(block[3*i],block[3*i+1],block[3*i+2]);
      at+=n;
      }
    }
    CollisionCacheMesh land, water;
    land.name="land";
    water.name="water";
    land .read(in,size+LandIndices);
    water.read(in,size+WaterIndices);
    std::string shapes(size_t(size[ShapeBytes]),'\0');
    in.bytes(shapes.data(),shapes.size());
    // Validate before any count is trusted and before Jolt interprets native shape data.
    if(hash.finish()!=stored)
      fail("checksum mismatch: the payload is damaged");

    size_t at=0;
    while(at<names.size()) {
      uint32_t length=0;
      if(names.size()-at<4)
        fail("the sector names end inside a length");
      std::memcpy(&length,names.data()+at,4);
      at+=4;
      if(length>names.size()-at)
        fail("sector name "+count(sectors.size())+" runs past the names");
      sectors.emplace_back(reinterpret_cast<const char*>(names.data()+at),length);
      at+=length;
      }
    if(sectors.size()!=size[Names])
      fail("the header lists "+count(size[Names])+" sector names, the file holds "+count(sectors.size()));
    land .check(sectors.size(),vertices.size());
    water.check(sectors.size(),vertices.size());

    Physics::initialize();
    ShapeInput stream(shapes);
    JPH::Shape::IDToShapeMap shapeIds;
    JPH::Shape::IDToMaterialMap materials;
    const auto landShape=JPH::Shape::sRestoreWithChildren(stream,shapeIds,materials);
    if(landShape.HasError())
      fail("Jolt cannot restore the land shape: "+std::string(landShape.GetError().c_str()));
    const auto waterShape=JPH::Shape::sRestoreWithChildren(stream,shapeIds,materials);
    if(waterShape.HasError())
      fail("Jolt cannot restore the water shape: "+std::string(waterShape.GetError().c_str()));
    if(!stream.empty())
      fail("bytes are left after the shapes");

    // Everything holds: hand it over. The meshes point into out.vertices and out.sectors, filled first.
    out.sectors =std::move(sectors);
    land .bind(out.sectors);
    water.bind(out.sectors);
    out.vertices=std::move(vertices);
    out.land =std::make_unique<PhysicVbo>(&out.vertices);
    out.water=std::make_unique<PhysicVbo>(&out.vertices);
    out.land ->adopt(std::move(land.indices), std::move(land.segments), std::move(land.duplicates));
    out.water->adopt(std::move(water.indices),std::move(water.segments),std::move(water.duplicates));
    out.landShape =landShape.Get();
    out.waterShape=waterShape.Get();
    return true;
    }
  catch(const Failure& failure) {
    reason=failure.what();
    }
  catch(const std::exception& error) {
    reason=std::string("read failed: ")+error.what();
    }
  Tempest::Log::i("[collision-cache] ",file.string(),": ",reason,"; rebuilding from the collision input");
  return false;
  }

void CollisionCache::save(const StaticCollision& in) const {
  if(file.empty())
    return;
  fs::path temporary;
  try {
    fs::create_directories(file.parent_path());
    temporary=file;
    temporary+=".tmp-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+
               "-"+std::to_string(std::random_device{}());
    std::unordered_map<const char*,uint32_t> sectorIds;
    for(size_t i=0;i<in.sectors.size();++i)
      sectorIds.emplace(in.sectors[i].c_str(),uint32_t(i));
    uint64_t size[Sizes]={0,in.sectors.size(),in.vertices.size()};
    for(const auto& name:in.sectors)
      size[NameBytes]+=4+name.size();
    const PhysicVbo* meshes[2]={in.land.get(),in.water.get()};
    for(size_t m=0;m<2;++m) {
      meshes[m]->prepareTriangleData();
      auto* s=size+LandIndices+4*m;
      s[0]=meshes[m]->id.size();
      s[1]=meshes[m]->segments.size();
      s[2]=meshes[m]->rayDuplicates.size();
      for(const auto& group:meshes[m]->rayDuplicates)
        s[3]+=group.size();
      }
    {
      std::ofstream output(temporary,std::ios::binary|std::ios::trunc);
      const std::array<char,HeaderSize> placeholder {};
      output.write(placeholder.data(),std::streamsize(placeholder.size()));
      Writer out(output);
      for(const auto& name:in.sectors) {
        out.value(uint32_t(name.size()));
        out.bytes(name.data(),name.size());
        }
      std::vector<float> block;
      block.reserve(3*4096);
      for(size_t at=0;at<in.vertices.size();at+=4096) {
        block.clear();
        for(size_t i=at;i<std::min(at+4096,in.vertices.size());++i)
          block.insert(block.end(),{in.vertices[i].GetX(),in.vertices[i].GetY(),in.vertices[i].GetZ()});
        out.bytes(block.data(),block.size()*sizeof(float));
        }
      for(const auto* mesh:meshes) {
        out.bytes(mesh->id.data(),mesh->id.size()*sizeof(uint32_t));
        for(const auto& segment:mesh->segments) {
          uint32_t sector=NoSector;
          if(segment.sector!=nullptr) {
            const auto found=sectorIds.find(segment.sector);
            if(found==sectorIds.end())
              throw std::runtime_error("a segment's sector is not one of the sector names");
            sector=found->second;
            }
          out.value(uint64_t(segment.off));
          out.value(uint64_t(segment.size));
          out.value(uint32_t(segment.mat));
          out.value(sector);
          }
        for(const auto& group:mesh->rayDuplicates)
          out.value(uint32_t(group.size()));
        for(const auto& group:mesh->rayDuplicates)
          out.bytes(group.data(),group.size()*sizeof(uint32_t));
        }
      ShapeOutput stream(out);
      JPH::Shape::ShapeToIDMap shapes;
      JPH::Shape::MaterialToIDMap materials;
      for(const auto* shape:{in.landShape.GetPtr(),in.waterShape.GetPtr()}) {
        if(shape!=nullptr)
          shape->SaveWithChildren(stream,shapes,materials);
        else
          stream.Write(~uint32_t(0));
        }
      size[ShapeBytes]=stream.written;
      std::array<char,HeaderSize> header {};
      std::memcpy(header.data(),Magic.data(),Magic.size());
      std::memcpy(header.data()+IdentityAt,identity.data(),identity.size());
      const uint64_t checksum=out.hash.finish();
      std::memcpy(header.data()+ChecksumAt,&checksum,8);
      std::memcpy(header.data()+SizesAt,size,sizeof(size));
      output.seekp(0);
      output.write(header.data(),std::streamsize(header.size()));
      output.close();
      if(!output)
        throw std::runtime_error("cannot write collision cache");
    }
    fs::rename(temporary,file);
    }
  catch(const std::exception& error) {
    if(!temporary.empty()) {
      std::error_code ignored;
      fs::remove(temporary,ignored);
      }
    Tempest::Log::e("collision cache write failed: ",error.what());
    }
  }
