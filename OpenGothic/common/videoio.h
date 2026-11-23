#pragma once

#include "videodata.h"
#include "videoformat.h"
#include "sha256.h"
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <algorithm>
#include <stdexcept>

namespace VideoData::Detail {
namespace fs=std::filesystem;
constexpr uint64_t HeaderSize=128;
constexpr uint32_t KeyframeInterval=4;
constexpr size_t CancellationChunk=1<<16;
inline void require(bool ok,const std::string& reason) { if(!ok) throw std::runtime_error("video data: "+reason); }
inline void require(bool ok,const char* reason) { if(!ok) throw std::runtime_error(std::string("video data: ")+reason); }
inline void checkCancelled(const std::atomic_bool* cancelled) {
  require(cancelled==nullptr || !cancelled->load(),"preparation cancelled");
  }
inline uint32_t aligned(uint32_t n) { return ((n+15)/16)*16; }
inline std::string upper(std::string name) { for(auto& c:name) if(c>='a' && c<='z') c=char(c-'a'+'A'); return name; }
inline std::string quoted(const std::string& value) {
  rapidjson::StringBuffer b; rapidjson::Writer<rapidjson::StringBuffer> w(b); w.String(value.c_str(),rapidjson::SizeType(value.size())); return b.GetString();
  }
inline void writeBytes(std::ostream& out,const void* data,size_t size,const std::atomic_bool* cancelled=nullptr) {
  const auto* bytes=static_cast<const char*>(data);
  for(size_t offset=0;offset<size;offset+=CancellationChunk) {
    checkCancelled(cancelled);
    out.write(bytes+offset,std::streamsize(std::min(size-offset,CancellationChunk)));
    require(bool(out),"write failed");
    }
  }
inline void put32(std::ostream& out,uint32_t value) { uint8_t b[4]; for(size_t i=0;i<4;++i) b[i]=uint8_t(value>>(i*8)); writeBytes(out,b,4); }
inline void put64(std::ostream& out,uint64_t value) { uint8_t b[8]; for(size_t i=0;i<8;++i) b[i]=uint8_t(value>>(i*8)); writeBytes(out,b,8); }
inline void readBytes(std::istream& in,void* data,size_t size) { in.read(static_cast<char*>(data),std::streamsize(size)); require(bool(in),"truncated file"); }
inline uint32_t get32(std::istream& in) { uint8_t b[4]; readBytes(in,b,4); uint32_t v=0; for(size_t i=0;i<4;++i) v|=uint32_t(b[i])<<(i*8); return v; }
inline uint64_t get64(std::istream& in) { uint8_t b[8]; readBytes(in,b,8); uint64_t v=0; for(size_t i=0;i<8;++i) v|=uint64_t(b[i])<<(i*8); return v; }
inline std::string hashRange(std::istream& in,uint64_t size,const std::atomic_bool* cancelled=nullptr) {
  Sha256 hash; std::array<char,1<<16> buffer;
  while(size) { checkCancelled(cancelled); const auto n=size_t(std::min<uint64_t>(size,buffer.size())); readBytes(in,buffer.data(),n); hash.update(buffer.data(),n); size-=n; }
  return hash.finish();
  }
inline std::string readText(const fs::path& path) {
  std::ifstream in(path,std::ios::binary); require(bool(in),"cannot open "+path.string()); return {std::istreambuf_iterator<char>(in),{}};
  }
inline std::string fileHash(const fs::path& path,const std::atomic_bool* cancelled=nullptr) {
  std::ifstream in(path,std::ios::binary); require(bool(in),"cannot open "+path.string());
  return hashRange(in,fs::file_size(path),cancelled);
  }
inline void updateHash(Sha256& hash,std::string_view data,const std::atomic_bool* cancelled) {
  for(size_t offset=0;offset<data.size();offset+=CancellationChunk) {
    checkCancelled(cancelled);
    hash.update(data.data()+offset,std::min(data.size()-offset,CancellationChunk));
    }
  checkCancelled(cancelled);
  }
inline std::string dataHash(std::string_view data,const std::atomic_bool* cancelled) {
  Sha256 hash; updateHash(hash,data,cancelled); return hash.finish();
  }
inline std::string frameHash(uint32_t flags,std::string_view data,const std::atomic_bool* cancelled=nullptr) {
  uint8_t bytes[4]; for(size_t i=0;i<4;++i) bytes[i]=uint8_t(flags>>(i*8));
  Sha256 hash; hash.update(bytes,sizeof(bytes)); updateHash(hash,data,cancelled); return hash.finish();
  }
inline Identity identityOf(const std::vector<Source>& list) {
  std::string json="{\"format_sha256\":"+quoted(VideoFormatSha256)+",\"sources\":[";
  for(size_t i=0;i<list.size();++i) {
    const auto& s=list[i]; json+=std::string(i ? "," : "")+"{\"name\":"+quoted(s.name)+",\"size\":"+std::to_string(s.size)+",\"sha256\":"+quoted(s.sha256)+"}";
    }
  json+="]}"; return {Sha256::hash(json),json};
  }
}
