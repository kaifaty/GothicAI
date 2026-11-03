#include "placedcensuscache.h"

#include "sha256.h"
#include "worldchunks.h"

#include <Jolt/Jolt.h>
#include <Tempest/Log>

#include <bit>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <random>
#include <stdexcept>

namespace {
namespace fs = std::filesystem;
constexpr std::string_view Magic = "GOTHIC-PLACED-CENSUS-1\n";
constexpr size_t HeaderSize = Magic.size()+sizeof(uint32_t)+64+2*sizeof(uint32_t);
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

std::string_view PlacedCensusCache::statusName(Status status) {
  switch(status) {
    case Status::Disabled: return "off";
    case Status::Hit:      return "cache";
    case Status::Absent:   return "absent";
    case Status::Version:  return "version";
    case Status::Inputs:   return "inputs";
    case Status::Corrupt:  return "corrupt";
    }
  return "unknown";
  }

PlacedCensusCache::PlacedCensusCache(const fs::path& cacheRoot, const Key& key):version(key.version) {
  if(!enabled() || cacheRoot.empty() || key.base.empty() || key.content.empty() || key.inputs.empty())
    return;
  if(key.base.find_first_not_of("0123456789abcdef")!=std::string::npos ||
     key.world.empty() || key.world.find_first_of("/\\")!=std::string::npos || key.world=="..")
    return;
  // Jolt queries decide every float; keep differing Jolt builds apart as well.
  const std::string bytes=key.base+"\n"+key.world+"\n"+key.content+"\n"+key.inputs+"\n"+
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

std::vector<std::string_view> PlacedCensusCache::differences(const Entry& a, const Entry& b) {
  std::vector<std::string_view> ret;
  if(a.disposition!=b.disposition)                  ret.push_back("disposition");
  if(!same(a.supportGapCm,b.supportGapCm))          ret.push_back("supportGapCm");
  if(!same(a.staticDepthCm,b.staticDepthCm))        ret.push_back("staticDepthCm");
  if(!same(a.furnitureDepthCm,b.furnitureDepthCm))  ret.push_back("furnitureDepthCm");
  if(!same(a.itemDepthCm,b.itemDepthCm))            ret.push_back("itemDepthCm");
  if(a.supportCategory!=b.supportCategory)          ret.push_back("supportCategory");
  if(a.supportLabel!=b.supportLabel)                ret.push_back("supportLabel");
  if(!same(a.supportNormal,b.supportNormal))        ret.push_back("supportNormal");
  if(!same(a.hullLowPointCm,b.hullLowPointCm))      ret.push_back("hullLowPointCm");
  if(!same(a.supportFaceNormal,b.supportFaceNormal)) ret.push_back("supportFaceNormal");
  if(!same(a.supportSlopeDeg,b.supportSlopeDeg))    ret.push_back("supportSlopeDeg");
  if(a.supportZenId!=b.supportZenId)                ret.push_back("supportZenId");
  if(a.supportItemIndex!=b.supportItemIndex)        ret.push_back("supportItemIndex");
  if(a.enabled!=b.enabled)                          ret.push_back("enabled");
  return ret;
  }

std::string PlacedCensusCache::describe(const Entry& e) {
  const auto vec=[](const Tempest::Vec3& v) {
    return std::to_string(v.x)+","+std::to_string(v.y)+","+std::to_string(v.z);
    };
  return e.disposition+" gap="+std::to_string(e.supportGapCm)+" static="+std::to_string(e.staticDepthCm)+
         " furniture="+std::to_string(e.furnitureDepthCm)+" item="+std::to_string(e.itemDepthCm)+
         " category="+std::to_string(e.supportCategory)+" label="+e.supportLabel+
         " normal="+vec(e.supportNormal)+" low="+vec(e.hullLowPointCm)+" face="+vec(e.supportFaceNormal)+
         " slope="+std::to_string(e.supportSlopeDeg)+" zen="+std::to_string(e.supportZenId)+
         " support_item="+(e.supportItemIndex==size_t(-1) ? std::string("-") : std::to_string(e.supportItemIndex))+
         " enabled="+(e.enabled ? "1" : "0");
  }
