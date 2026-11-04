#if defined(TEMPEST_BUILD_METAL)

#include "mtshadercache.h"

#include <Tempest/Log>

#include "gapi/pipelinecache.h"

#include <chrono>
#include <cstring>

using namespace Tempest;
using namespace Tempest::Detail;

namespace fs = std::filesystem;

// Bump when the file layout or the meaning of an entry changes.
static constexpr char     MslCacheMagic[8] = {'T','M','S','L','C','C','H','1'};
static constexpr size_t   MslCacheHeader   = 8+8+8; // magic, body size, body hash
static constexpr size_t   MaxMslCacheSize  = 64u*1024u*1024u;
static constexpr uint64_t BodySeed         = 0x6d736c2d63616368ull;

namespace {

class Writer {
  public:
    std::vector<uint8_t> data;
    void u8 (uint8_t v)  { data.push_back(v); }
    void u32(uint32_t v) { raw(&v,4); }
    void u64(uint64_t v) { raw(&v,8); }
    void str(const std::string& s) { u32(uint32_t(s.size())); raw(s.data(),s.size()); }
    void raw(const void* p, size_t n) {
      auto* b = reinterpret_cast<const uint8_t*>(p);
      data.insert(data.end(),b,b+n);
      }
  };

class Reader {
  public:
    Reader(const uint8_t* p, size_t n):p(p),end(p+n) {}
    bool u8 (uint8_t& v)  { return raw(&v,1); }
    bool u32(uint32_t& v) { return raw(&v,4); }
    bool u64(uint64_t& v) { return raw(&v,8); }
    bool str(std::string& s) {
      uint32_t n = 0;
      if(!u32(n) || size_t(end-p)<n)
        return false;
      s.assign(reinterpret_cast<const char*>(p),n);
      p += n;
      return true;
      }
    bool raw(void* v, size_t n) {
      if(size_t(end-p)<n)
        return false;
      std::memcpy(v,p,n);
      p += n;
      return true;
      }
    bool done() const { return p==end; }
  private:
    const uint8_t* p;
    const uint8_t* end;
  };

}

MtShaderCache::~MtShaderCache() {
  if(saver.joinable())
    saver.join();
  save("exit");
  }

MtShaderCache::Key MtShaderCache::key(const void* spirv, size_t size) {
  Key k;
  k.h0   = PipelineCacheFile::hashWords(spirv,size,0x243f6a8885a308d3ull);
  k.h1   = PipelineCacheFile::hashWords(spirv,size,0x13198a2e03707344ull);
  k.size = uint32_t(size);
  return k;
  }

void MtShaderCache::init(const std::string& id) {
  const auto dir = PipelineCacheFile::shaderDirectory();
  if(dir.empty())
    return;
  identity  = id;
  verifyAll = PipelineCacheFile::shaderVerify();
  file      = dir/("msl-"+PipelineCacheFile::hex(PipelineCacheFile::hash(id.data(),id.size()))+".bin");

  const auto start = std::chrono::steady_clock::now();
  if(!load()) {
    entries.clear();
    Log::i("msl cache: empty, will write ",file.string(),verifyAll ? " (verify)" : "");
    return;
    }
  const auto ms = std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
  Log::i("msl cache: loaded ",file.string()," (",entries.size()," shaders in ",int(ms)," ms",verifyAll ? ", verify" : "",")");
  }

bool MtShaderCache::load() {
  std::error_code ec;
  if(!fs::exists(file,ec))
    return false;
  std::vector<uint8_t> data;
  if(!PipelineCacheFile::read(file,data,MaxMslCacheSize)) {
    Log::i("msl cache: ignored ",file.string(),": unreadable or too large");
    return false;
    }
  uint64_t bodySize = 0, bodyHash = 0;
  if(data.size()<MslCacheHeader || std::memcmp(data.data(),MslCacheMagic,8)!=0) {
    Log::i("msl cache: ignored ",file.string(),": bad header");
    return false;
    }
  std::memcpy(&bodySize,data.data()+8, 8);
  std::memcpy(&bodyHash,data.data()+16,8);
  const uint8_t* body = data.data()+MslCacheHeader;
  const size_t   size = data.size()-MslCacheHeader;
  if(bodySize!=size || PipelineCacheFile::hashWords(body,size,BodySeed)!=bodyHash) {
    Log::i("msl cache: ignored ",file.string(),": checksum mismatch");
    return false;
    }

  Reader      rd(body,size);
  std::string id;
  uint32_t    count = 0;
  if(!rd.str(id) || id!=identity || !rd.u32(count)) {
    Log::i("msl cache: ignored ",file.string(),": other translator options");
    return false;
    }
  for(uint32_t i=0; i<count; ++i) {
    Key     k;
    Slot    s;
    uint8_t bsb = 0;
    uint32_t nlay = 0;
    if(!rd.u64(k.h0) || !rd.u64(k.h1) || !rd.u32(k.size) || !rd.u8(bsb) || !rd.u32(s.entry.reflected) || !rd.u32(nlay) ||
       nlay>s.entry.reflected) {
      Log::i("msl cache: ignored ",file.string(),": damaged entry");
      return false;
      }
    s.entry.bufferSizeBuffer = bsb!=0;
    s.entry.lay.resize(nlay);
    for(auto& b:s.entry.lay) {
      if(!rd.u32(b.src) || !rd.u32(b.mslBinding) || !rd.u32(b.mslBinding2) || !rd.u32(b.mslSize)) {
        Log::i("msl cache: ignored ",file.string(),": damaged entry");
        return false;
        }
      }
    if(!rd.str(s.entry.msl)) {
      Log::i("msl cache: ignored ",file.string(),": damaged entry");
      return false;
      }
    entries[k] = std::move(s);
    }
  if(!rd.done()) {
    Log::i("msl cache: ignored ",file.string(),": trailing data");
    return false;
    }
  return true;
  }

bool MtShaderCache::find(const Key& k, Entry& out) {
  if(!enabled())
    return false;
  std::lock_guard<std::mutex> guard(sync);
  auto it = entries.find(k);
  if(it==entries.end()) {
    misses.fetch_add(1,std::memory_order_relaxed);
    return false;
    }
  it->second.used = true;
  hits.fetch_add(1,std::memory_order_relaxed);
  out = it->second.entry;
  return true;
  }

void MtShaderCache::put(const Key& k, Entry e) {
  if(!enabled())
    return;
  std::lock_guard<std::mutex> guard(sync);
  auto& s = entries[k];
  s.entry = std::move(e);
  s.used  = true;
  dirty   = true;
  }

void MtShaderCache::verified(bool ok) {
  if(ok)
    checked.fetch_add(1,std::memory_order_relaxed);
  }

void MtShaderCache::saveAsync() {
  if(!enabled())
    return;
  if(saver.joinable())
    saver.join();
  saver = std::thread([this](){ save("play"); });
  }

void MtShaderCache::save(const char* reason) {
  if(!enabled())
    return;
  std::lock_guard<std::mutex> saveGuard(saveSync);
  const auto start = std::chrono::steady_clock::now();
  Writer   wr;
  uint32_t count = 0;
  {
    std::lock_guard<std::mutex> guard(sync);
    if(!dirty) {
      Log::i("msl cache: unchanged (",reason,", hits=",hits.load()," misses=",misses.load(),
             verifyAll ? ", verified=" : "",verifyAll ? std::to_string(checked.load()) : std::string(),")");
      return;
      }
    // Only the shaders of this session go to the file, so it does not grow with every shader edit.
    wr.str(identity);
    const size_t countAt = wr.data.size();
    wr.u32(0);
    for(auto& [k,s]:entries) {
      if(!s.used)
        continue;
      wr.u64(k.h0);
      wr.u64(k.h1);
      wr.u32(k.size);
      wr.u8 (s.entry.bufferSizeBuffer ? 1 : 0);
      wr.u32(s.entry.reflected);
      wr.u32(uint32_t(s.entry.lay.size()));
      for(auto& b:s.entry.lay) {
        wr.u32(b.src);
        wr.u32(b.mslBinding);
        wr.u32(b.mslBinding2);
        wr.u32(b.mslSize);
        }
      wr.str(s.entry.msl);
      ++count;
      }
    std::memcpy(wr.data.data()+countAt,&count,4);
    dirty = false;
  }

  std::vector<uint8_t> out(MslCacheHeader+wr.data.size());
  const uint64_t bodySize = wr.data.size();
  const uint64_t bodyHash = PipelineCacheFile::hashWords(wr.data.data(),wr.data.size(),BodySeed);
  std::memcpy(out.data(),   MslCacheMagic,8);
  std::memcpy(out.data()+8, &bodySize,8);
  std::memcpy(out.data()+16,&bodyHash,8);
  std::memcpy(out.data()+MslCacheHeader,wr.data.data(),wr.data.size());
  if(!PipelineCacheFile::writeAtomic(file,out.data(),out.size())) {
    std::lock_guard<std::mutex> guard(sync);
    dirty = true;
    return;
    }
  const auto ms = std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
  Log::i("msl cache: saved ",out.size()," bytes in ",int(ms)," ms (",reason,", shaders=",count,
         " hits=",hits.load()," misses=",misses.load(),verifyAll ? ", verified=" : "",
         verifyAll ? std::to_string(checked.load()) : std::string(),")");
  }

#endif
