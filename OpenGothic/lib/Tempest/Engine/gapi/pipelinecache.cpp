#include "pipelinecache.h"

#include <Tempest/Log>

#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <mutex>
#include <random>

using namespace Tempest::Detail;

namespace fs = std::filesystem;

static std::mutex& directorySync() {
  static std::mutex m;
  return m;
  }

static fs::path& directoryValue() {
  static fs::path dir;
  return dir;
  }

void PipelineCacheFile::setDirectory(const fs::path& dir) {
  std::lock_guard<std::mutex> guard(directorySync());
  directoryValue() = dir;
  }

fs::path PipelineCacheFile::directory() {
  std::lock_guard<std::mutex> guard(directorySync());
  return directoryValue();
  }

static fs::path& shaderDirectoryValue() {
  static fs::path dir;
  return dir;
  }

static bool& shaderVerifyValue() {
  static bool v = false;
  return v;
  }

void PipelineCacheFile::setShaderDirectory(const fs::path& dir, bool verify) {
  std::lock_guard<std::mutex> guard(directorySync());
  shaderDirectoryValue() = dir;
  shaderVerifyValue()    = verify;
  }

fs::path PipelineCacheFile::shaderDirectory() {
  std::lock_guard<std::mutex> guard(directorySync());
  return shaderDirectoryValue();
  }

bool PipelineCacheFile::shaderVerify() {
  std::lock_guard<std::mutex> guard(directorySync());
  return shaderVerifyValue();
  }

uint64_t PipelineCacheFile::hashWords(const void* data, size_t size, uint64_t seed) {
  // Not cryptographic: a key and a corruption check. Eight bytes per step, so a few MB cost about a millisecond.
  auto*    p = reinterpret_cast<const uint8_t*>(data);
  uint64_t h = seed ^ (uint64_t(size)*0x9e3779b97f4a7c15ull);
  size_t   i = 0;
  for(; i+8<=size; i+=8) {
    uint64_t w;
    std::memcpy(&w,p+i,8);
    h ^= w;
    h *= 0xbf58476d1ce4e5b9ull;
    h ^= h>>31;
    }
  uint64_t tail = 0;
  for(size_t r=0; i+r<size; ++r)
    tail |= uint64_t(p[i+r])<<(8*r);
  h ^= tail;
  h *= 0x94d049bb133111ebull;
  h ^= h>>29;
  h *= 0xbf58476d1ce4e5b9ull;
  h ^= h>>32;
  return h;
  }

uint64_t PipelineCacheFile::hash(const void* data, size_t size, uint64_t seed) {
  // FNV-1a: corruption and key check only, not a security boundary.
  auto* p = reinterpret_cast<const uint8_t*>(data);
  uint64_t h = seed;
  for(size_t i=0; i<size; ++i) {
    h ^= p[i];
    h *= 0x100000001b3ull;
    }
  return h;
  }

std::string PipelineCacheFile::hex(uint64_t v) {
  static const char* digits = "0123456789abcdef";
  std::string ret(16,'0');
  for(size_t i=0; i<16; ++i)
    ret[15-i] = digits[(v>>(i*4))&0xF];
  return ret;
  }

bool PipelineCacheFile::read(const fs::path& file, std::vector<uint8_t>& out, size_t maxSize) {
  std::error_code ec;
  const auto size = fs::file_size(file,ec);
  if(ec || size==0 || size>maxSize)
    return false;
  std::ifstream in(file,std::ios::binary);
  if(!in)
    return false;
  out.resize(size_t(size));
  if(!in.read(reinterpret_cast<char*>(out.data()),std::streamsize(out.size()))) {
    out.clear();
    return false;
    }
  return true;
  }

fs::path PipelineCacheFile::temporaryPath(const fs::path& file) {
  static std::atomic<uint32_t> counter{0};
  fs::path tmp = file;
  tmp += ".tmp-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
         "-" + std::to_string(std::random_device{}()) + "-" + std::to_string(counter.fetch_add(1));
  return tmp;
  }

bool PipelineCacheFile::writeAtomic(const fs::path& file, const void* data, size_t size) {
  fs::path tmp;
  try {
    fs::create_directories(file.parent_path());
    tmp = temporaryPath(file);
    {
      std::ofstream out(tmp,std::ios::binary|std::ios::trunc);
      out.write(reinterpret_cast<const char*>(data),std::streamsize(size));
      out.close();
      if(!out)
        throw std::runtime_error("cannot write");
    }
    fs::rename(tmp,file);
    return true;
    }
  catch(const std::exception& e) {
    if(!tmp.empty()) {
      std::error_code ignored;
      fs::remove(tmp,ignored);
      }
    Log::e("pipeline cache: write failed: ",file.string()," (",e.what(),")");
    return false;
    }
  }
