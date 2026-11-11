#include "placedcensuscache.h"

#include "sha256.h"
#include "worldchunks.h"
#include "placedcensusbuilder.h"

#include <Jolt/Jolt.h>
#include <Tempest/Log>

#include <algorithm>
#include <cctype>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <random>
#include <stdexcept>

namespace {
namespace fs = std::filesystem;
constexpr std::string_view Magic = "GOTHIC-PLACED-CENSUS-1\n";
constexpr size_t HeaderSize = Magic.size()+sizeof(uint32_t)+2*64+2*sizeof(uint32_t);
constexpr size_t MaxFileSize = 64*1024*1024;
constexpr size_t MaxEntries = 1000000;
constexpr auto JoltVersion=[] { using namespace JPH; return JPH_VERSION_ID; }();
#if defined(JPH_CPU_ARM)
constexpr std::string_view Architecture="arm";
#else
constexpr std::string_view Architecture="x86";
#endif

bool envIs(const char* name, std::string_view value) {
  const auto* found=std::getenv(name);
  return found!=nullptr && std::string_view(found)==value;
  }

void put(std::string& bytes, uint32_t value) {
  for(unsigned shift=0;shift<32;shift+=8)
    bytes.push_back(char((value>>shift)&0xff));
  }

void put(std::string& bytes, uint64_t value) {
  put(bytes,uint32_t(value));
  put(bytes,uint32_t(value>>32));
  }

void put(std::string& bytes, float value) { put(bytes,std::bit_cast<uint32_t>(value)); }

void put(std::string& bytes, const Tempest::Vec3& value) {
  put(bytes,value.x);put(bytes,value.y);put(bytes,value.z);
  }

void put(std::string& bytes, std::string_view value) {
  put(bytes,uint32_t(value.size()));
  bytes.append(value);
  }

class Reader final {
  public:
    explicit Reader(std::string_view bytes):bytes(bytes) {}
    uint32_t u32() {
      need(4);
      uint32_t value=0;
      for(unsigned i=0;i<4;++i)
        value|=uint32_t(uint8_t(bytes[i]))<<(i*8);
      bytes.remove_prefix(4);
      return value;
      }
    uint64_t u64() { const uint64_t low=u32(); return low|(uint64_t(u32())<<32); }
    float f32() { return std::bit_cast<float>(u32()); }
    Tempest::Vec3 vec3() { const float x=f32(),y=f32(); return {x,y,f32()}; }
    std::string str() {
      const auto size=u32();
      need(size);
      std::string value(bytes.substr(0,size));
      bytes.remove_prefix(size);
      return value;
      }
    bool empty() const { return bytes.empty(); }

  private:
    void need(size_t size) const {
      if(bytes.size()<size)
        throw std::runtime_error("truncated placed census cache");
      }
    std::string_view bytes;
  };

void putEntry(std::string& bytes, const PlacedCensusCache::Entry& entry) {
  put(bytes,entry.disposition);
  put(bytes,entry.supportGapCm);
  put(bytes,entry.staticDepthCm);
  put(bytes,entry.furnitureDepthCm);
  put(bytes,entry.itemDepthCm);
  put(bytes,uint32_t(entry.supportCategory));
  put(bytes,entry.supportLabel);
  put(bytes,entry.supportNormal);
  put(bytes,entry.hullLowPointCm);
  put(bytes,entry.supportFaceNormal);
  put(bytes,entry.supportSlopeDeg);
  put(bytes,entry.supportZenId);
  put(bytes,uint64_t(entry.supportItemIndex));
  put(bytes,uint32_t(entry.enabled ? 1 : 0));
  }

PlacedCensusCache::Entry takeEntry(Reader& in) {
  PlacedCensusCache::Entry entry;
  entry.disposition=in.str();
  entry.supportGapCm=in.f32();
  entry.staticDepthCm=in.f32();
  entry.furnitureDepthCm=in.f32();
  entry.itemDepthCm=in.f32();
  entry.supportCategory=int(in.u32());
  entry.supportLabel=in.str();
  entry.supportNormal=in.vec3();
  entry.hullLowPointCm=in.vec3();
  entry.supportFaceNormal=in.vec3();
  entry.supportSlopeDeg=in.f32();
  entry.supportZenId=in.u32();
  const auto index=in.u64();
  entry.supportItemIndex=index==std::numeric_limits<uint64_t>::max() ? size_t(-1) : size_t(index);
  const auto enabled=in.u32();
  if(enabled>1)
    throw std::runtime_error("invalid placed census flag");
  entry.enabled=enabled==1;
  return entry;
  }

bool same(float a, float b) { return std::bit_cast<uint32_t>(a)==std::bit_cast<uint32_t>(b); }
bool same(const Tempest::Vec3& a, const Tempest::Vec3& b) { return same(a.x,b.x) && same(a.y,b.y) && same(a.z,b.z); }
}

bool PlacedCensusCache::enabled() {
  return !envIs("OPENGOTHIC_PLACED_CENSUS_CACHE","0");
  }

bool PlacedCensusCache::checkMode() {
  return envIs("OPENGOTHIC_PLACED_CENSUS_CHECK","1");
  }

std::string_view PlacedCensusCache::builderFingerprint() {
  return OPENGOTHIC_PLACED_CENSUS_BUILDER;
  }

std::string_view PlacedCensusCache::statusName(Status status) {
  switch(status) {
    case Status::Disabled: return "off";
    case Status::Hit:      return "cache";
    case Status::Absent:   return "absent";
    case Status::Version:  return "version";
    case Status::Builder:  return "builder";
    case Status::Inputs:   return "inputs";
    case Status::Corrupt:  return "corrupt";
    }
  return "unknown";
  }

PlacedCensusCache::PlacedCensusCache(const fs::path& cacheRoot, const Key& source)
  :builder(source.builder), version(source.version) {
  if(!enabled() || cacheRoot.empty() || source.base.empty() || source.content.empty() || source.inputs.empty())
    return;
  if(builder.size()!=64 || builder.find_first_not_of("0123456789abcdef")!=std::string::npos)
    return;
  if(source.base.find_first_not_of("0123456789abcdef")!=std::string::npos ||
     source.world.empty() || source.world.find_first_of("/\\")!=std::string::npos || source.world=="..")
    return;
  // A save names its world "newworld.zen", a fresh start "NEWWORLD.ZEN": one
  // file per world, also on case-sensitive file systems.
  Key key=source;
  std::transform(key.world.begin(),key.world.end(),key.world.begin(),[](unsigned char c) { return char(std::toupper(c)); });
  // Jolt queries decide every float; keep differing Jolt builds apart as well.
  const std::string bytes=key.base+"\n"+key.world+"\n"+key.content+"\n"+key.inputs+"\n"+key.builder+"\n"+
      std::to_string(key.flags)+"\n"+std::to_string(key.version)+"\n"+std::to_string(JoltVersion)+"\n"+
      std::to_string(sizeof(size_t))+"\n"+std::string(Architecture)+"\n"+
      (std::endian::native==std::endian::little ? "le" : "be");
  identity=Sha256::hash(bytes);
  file=cacheRoot/"derived"/key.base/(key.world+".placed-census");
  }

PlacedCensusCache::Status PlacedCensusCache::read(size_t count, std::vector<Entry>& entries) const {
  entries.clear();
  if(file.empty())
    return Status::Disabled;
  try {
    std::error_code error;
    if(!fs::exists(file,error))
      return Status::Absent;
    const auto size=fs::file_size(file,error);
    if(error || size<HeaderSize || size>MaxFileSize)
      return Status::Corrupt;
    std::ifstream input(file,std::ios::binary);
    std::string bytes(size_t(size),'\0');
    if(!input.read(bytes.data(),std::streamsize(bytes.size())))
      return Status::Corrupt;
    std::string_view view=bytes;
    if(!view.starts_with(Magic))
      return Status::Corrupt;
    view.remove_prefix(Magic.size());
    Reader header(view.substr(0,sizeof(uint32_t)));
    if(header.u32()!=version)
      return Status::Version;
    view.remove_prefix(sizeof(uint32_t));
    if(view.substr(0,64)!=builder)
      return Status::Builder;
    view.remove_prefix(64);
    if(view.substr(0,64)!=identity)
      return Status::Inputs;
    view.remove_prefix(64);
    Reader fields(view.substr(0,2*sizeof(uint32_t)));
    const auto stored=fields.u32();
    const auto checksum=fields.u32();
    view.remove_prefix(2*sizeof(uint32_t));
    if(worldChunkCrc32(view.data(),view.size())!=checksum || stored!=count || stored>MaxEntries)
      return Status::Corrupt;
    Reader in(view);
    entries.reserve(stored);
    for(uint32_t i=0;i<stored;++i)
      entries.push_back(takeEntry(in));
    if(!in.empty()) {
      entries.clear();
      return Status::Corrupt;
      }
    return Status::Hit;
    }
  catch(const std::exception&) {
    // A derived file can always be recomputed from the live census.
    entries.clear();
    return Status::Corrupt;
    }
  }

void PlacedCensusCache::save(const std::vector<Entry>& entries) const {
  if(file.empty() || entries.size()>MaxEntries)
    return;
  fs::path temporary;
  try {
    std::string payload;
    for(const auto& entry:entries)
      putEntry(payload,entry);
    std::string bytes(Magic);
    put(bytes,version);
    bytes+=builder;
    bytes+=identity;
    put(bytes,uint32_t(entries.size()));
    put(bytes,worldChunkCrc32(payload.data(),payload.size()));
    bytes+=payload;
    fs::create_directories(file.parent_path());
    temporary=file;
    temporary+=".tmp-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+
               "-"+std::to_string(std::random_device{}());
    {
      std::ofstream output(temporary,std::ios::binary|std::ios::trunc);
      output.write(bytes.data(),std::streamsize(bytes.size()));
      output.close();
      if(!output)
        throw std::runtime_error("cannot write placed census cache");
    }
    fs::rename(temporary,file);
    }
  catch(const std::exception& error) {
    if(!temporary.empty()) {
      std::error_code ignored;
      fs::remove(temporary,ignored);
      }
    Tempest::Log::e("placed census cache write failed: ",error.what());
    }
  }

std::vector<PlacedCensusCache::Difference> PlacedCensusCache::differences(const Entry& a, const Entry& b) {
  std::vector<Difference> ret;
  const auto real=[](float v) {
    char text[48];
    std::snprintf(text,sizeof(text),"%.9g(0x%08x)",double(v),std::bit_cast<uint32_t>(v));
    return std::string(text);
    };
  const auto vec=[&](const Tempest::Vec3& v) { return real(v.x)+","+real(v.y)+","+real(v.z); };
  const auto text=[&](std::string_view field, bool equal, std::string x, std::string y) {
    if(!equal)
      ret.push_back({field,std::move(x),std::move(y)});
    };
  text("disposition",a.disposition==b.disposition,a.disposition,b.disposition);
  text("supportGapCm",same(a.supportGapCm,b.supportGapCm),real(a.supportGapCm),real(b.supportGapCm));
  text("staticDepthCm",same(a.staticDepthCm,b.staticDepthCm),real(a.staticDepthCm),real(b.staticDepthCm));
  text("furnitureDepthCm",same(a.furnitureDepthCm,b.furnitureDepthCm),real(a.furnitureDepthCm),real(b.furnitureDepthCm));
  text("itemDepthCm",same(a.itemDepthCm,b.itemDepthCm),real(a.itemDepthCm),real(b.itemDepthCm));
  text("supportCategory",a.supportCategory==b.supportCategory,std::to_string(a.supportCategory),std::to_string(b.supportCategory));
  text("supportLabel",a.supportLabel==b.supportLabel,a.supportLabel,b.supportLabel);
  text("supportNormal",same(a.supportNormal,b.supportNormal),vec(a.supportNormal),vec(b.supportNormal));
  text("hullLowPointCm",same(a.hullLowPointCm,b.hullLowPointCm),vec(a.hullLowPointCm),vec(b.hullLowPointCm));
  text("supportFaceNormal",same(a.supportFaceNormal,b.supportFaceNormal),vec(a.supportFaceNormal),vec(b.supportFaceNormal));
  text("supportSlopeDeg",same(a.supportSlopeDeg,b.supportSlopeDeg),real(a.supportSlopeDeg),real(b.supportSlopeDeg));
  text("supportZenId",a.supportZenId==b.supportZenId,std::to_string(a.supportZenId),std::to_string(b.supportZenId));
  text("supportItemIndex",a.supportItemIndex==b.supportItemIndex,std::to_string(int64_t(a.supportItemIndex)),
       std::to_string(int64_t(b.supportItemIndex)));
  text("enabled",a.enabled==b.enabled,a.enabled ? "1" : "0",b.enabled ? "1" : "0");
  return ret;
  }
