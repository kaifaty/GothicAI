#include "videodata.h"
#include "videoio.h"
#include "bink/video.h"
#include <zenkit/Vfs.hh>
#include <zenkit/Stream.hh>
#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#include "miniz.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <exception>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>

namespace fs=std::filesystem;
namespace VideoData {
using namespace Detail;
namespace {
constexpr int CompressionLevel=6;
void collect(const zenkit::VfsNode& node,const std::string& parent,std::map<const zenkit::VfsNode*,std::string>& paths,std::set<std::string>& names,const std::atomic_bool* cancelled) {
  for(const auto& child:node.children()) {
    checkCancelled(cancelled);
    const auto path=parent+child.name();
    if(child.type()==zenkit::VfsNodeType::DIRECTORY) collect(child,path+"/",paths,names,cancelled);
    else if(upper(fs::path(child.name()).extension().string())==".BIK") { paths[&child]=path; names.insert(upper(child.name())); }
    }
  }
std::vector<uint8_t> bytes(const zenkit::VfsNode& node,const std::atomic_bool* cancelled=nullptr) {
  checkCancelled(cancelled);
  auto in=node.open_read(); std::vector<uint8_t> data; std::array<uint8_t,1<<16> buffer;
  for(;;) { checkCancelled(cancelled); const size_t n=in->read(buffer.data(),buffer.size()); if(!n) break; data.insert(data.end(),buffer.begin(),buffer.begin()+ptrdiff_t(n)); }
  return data;
  }
struct Selected {
  zenkit::Vfs vfs;
  std::vector<Source> list;
  std::vector<const zenkit::VfsNode*> nodes;
  explicit Selected(const fs::path& gothic,const std::atomic_bool* cancelled=nullptr) {
    checkCancelled(cancelled);
    fs::path work; BaseImport::mountRuntime(vfs,gothic,work);
    checkCancelled(cancelled);
    std::map<const zenkit::VfsNode*,std::string> paths; std::set<std::string> names;
    collect(vfs.root(),"",paths,names,cancelled);
    for(const auto& name:names) {
      checkCancelled(cancelled);
      const auto* node=vfs.find(name); require(node!=nullptr,"unresolved BIK "+name);
      auto in=node->open_read(); Sha256 hash; uint64_t size=0; std::array<char,1<<16> buffer;
      for(;;) { checkCancelled(cancelled); const auto n=in->read(buffer.data(),buffer.size()); if(!n) break; hash.update(buffer.data(),n); size+=n; }
      list.push_back({name,paths.at(node),size,hash.finish()}); nodes.push_back(node);
      }
    }
  };
struct Input final:Bink::Video::Input {
  const std::vector<uint8_t>& data; size_t at=0;
  const std::atomic_bool* cancelled=nullptr;
  explicit Input(const std::vector<uint8_t>& d,const std::atomic_bool* cancelled=nullptr):data(d),cancelled(cancelled) {}
  void read(void* out,size_t n) override { checkCancelled(cancelled); require(at<=data.size() && n<=data.size()-at,"truncated BIK"); std::memcpy(out,data.data()+at,n); at+=n; }
  void seek(size_t n) override { checkCancelled(cancelled); require(n<=data.size(),"BIK seek past end"); at=n; }
  void skip(size_t n) override { checkCancelled(cancelled); require(at<=data.size() && n<=data.size()-at,"BIK skip past end"); at+=n; }
  };
int16_t pcm(float v) { return v < -1.00004566f ? int16_t(-32768) : (v > 1.00001514f ? int16_t(32767) : int16_t(v*32767.5f)); }
Info infoOf(const Bink::Video& video) {
  Info ret; const auto& frame=video.frame(0);
  ret.width=frame.width(); ret.height=frame.height(); ret.frames=uint32_t(video.frameCount()); ret.fpsNum=video.fps().num; ret.fpsDen=video.fps().den;
  for(uint8_t p=0;p<4;++p) ret.planes[p]={frame.plane(p).stride(),aligned((p==1 || p==2) ? ret.height/2 : ret.height)};
  for(size_t t=0;t<video.audioCount();++t) { const auto& a=video.audio(uint8_t(t)); ret.tracks.push_back({a.sampleRate,a.isMono ? 1u : 2u,0}); }
  return ret;
  }
struct Position { uint64_t offset=0, size=0; uint32_t flags=0; std::string sha256=std::string(64,'0'); };
void writeInfo(std::ostream& out,const Info& info,const std::vector<Position>& index,uint64_t fileSize,const std::string& hash) {
  out.seekp(0); writeBytes(out,"GVIDEO2\0",8); put32(out,2); put32(out,uint32_t(HeaderSize+32+16*info.tracks.size()+84*index.size())); put64(out,fileSize);
  writeBytes(out,hash.data(),64); put32(out,info.width); put32(out,info.height); put32(out,info.frames); put32(out,info.fpsNum); put32(out,info.fpsDen); put32(out,uint32_t(info.tracks.size()));
  const uint8_t reserved[16]={}; writeBytes(out,reserved,16);
  for(const auto& p:info.planes) { put32(out,p.stride); put32(out,p.rows); }
  for(const auto& t:info.tracks) { put32(out,t.sampleRate); put32(out,t.channels); put64(out,t.samples); }
  for(const auto& f:index) { put64(out,f.offset); put64(out,f.size); put32(out,f.flags); writeBytes(out,f.sha256.data(),64); }
  }
bool encodePlane(std::ostream& out,const uint8_t* data,size_t size,uint32_t stride,const std::vector<uint8_t>* previous,const std::atomic_bool* cancelled) {
  checkCancelled(cancelled);
  std::vector<uint8_t> predicted(size);
  for(size_t i=0;i<size;++i) { if(i%CancellationChunk==0) checkCancelled(cancelled); predicted[i]=uint8_t(data[i]-(i>=stride ? data[i-stride] : 0)); }
  mz_ulong length=mz_compressBound(mz_ulong(size)); std::vector<uint8_t> compressed(length);
  checkCancelled(cancelled);
  require(mz_compress2(compressed.data(),&length,predicted.data(),mz_ulong(size),CompressionLevel)==MZ_OK,"deflate failed"); compressed.resize(length);
  checkCancelled(cancelled);
  bool temporal=false;
  if(previous) {
    require(previous->size()==size,"previous plane size differs");
    for(size_t i=0;i<size;++i) { if(i%CancellationChunk==0) checkCancelled(cancelled); predicted[i]=uint8_t(data[i]-(*previous)[i]); }
    mz_ulong deltaSize=mz_compressBound(mz_ulong(size)); std::vector<uint8_t> delta(deltaSize);
    checkCancelled(cancelled);
    require(mz_compress2(delta.data(),&deltaSize,predicted.data(),mz_ulong(size),CompressionLevel)==MZ_OK,"deflate failed");
    checkCancelled(cancelled);
    if(deltaSize<compressed.size()) { delta.resize(deltaSize); compressed=std::move(delta); temporal=true; }
    }
  std::vector<uint8_t> restored(size); mz_ulong restoredSize=mz_ulong(size);
  checkCancelled(cancelled);
  require(mz_uncompress(restored.data(),&restoredSize,compressed.data(),mz_ulong(compressed.size()))==MZ_OK && restoredSize==size,"deflate verification failed");
  checkCancelled(cancelled);
  if(temporal) {
    for(size_t i=0;i<size;++i) { if(i%CancellationChunk==0) checkCancelled(cancelled); restored[i]=uint8_t(restored[i]+(*previous)[i]); }
    } else {
    for(size_t i=stride;i<size;++i) { if(i%CancellationChunk==0) checkCancelled(cancelled); restored[i]=uint8_t(restored[i]+restored[i-stride]); }
    }
  require(std::memcmp(restored.data(),data,size)==0,"decoded plane differs after compression");
  checkCancelled(cancelled);
  put32(out,uint32_t(compressed.size())); writeBytes(out,compressed.data(),compressed.size(),cancelled); return temporal;
  }
Entry encode(const Source& source,Bink::Video& video,const fs::path& out,const std::atomic_bool* cancelled) {
  Entry entry; entry.source=source; entry.file=source.name+".gvideo"; entry.info=infoOf(video);
  const auto file=out/entry.file; std::fstream output(file,std::ios::binary|std::ios::in|std::ios::out|std::ios::trunc); require(bool(output),"cannot write "+file.string());
  std::vector<Position> positions(entry.info.frames);
  writeInfo(output,entry.info,positions,0,std::string(64,'0'));
  Sha256 payloadHash; std::array<std::vector<uint8_t>,4> previousPlanes;
  for(uint32_t f=0;f<entry.info.frames;++f) {
    checkCancelled(cancelled);
    const Bink::Frame* decoded=nullptr;
    try { decoded=&video.nextFrame(); }
    catch(const std::exception& e) {
      throw std::runtime_error("video data: "+source.name+" frame "+std::to_string(f)+" (zero-based): "+e.what());
      }
    checkCancelled(cancelled);
    const auto& frame=*decoded; const uint64_t start=uint64_t(output.tellp());
    std::ostringstream encodedFrame(std::ios::binary|std::ios::out);
    uint32_t flags=0;
    for(uint8_t p=0;p<4;++p) {
      const auto& pi=entry.info.planes[p]; const auto* data=frame.plane(p).data(); const size_t size=size_t(pi.stride)*pi.rows;
      if(encodePlane(encodedFrame,data,size,pi.stride,f%KeyframeInterval ? &previousPlanes[p] : nullptr,cancelled)) flags|=1u<<p;
      previousPlanes[p].assign(data,data+size);
      }
    for(size_t t=0;t<entry.info.tracks.size();++t) {
      const auto& samples=frame.audio(uint8_t(t)).samples; put32(encodedFrame,uint32_t(samples.size()));
      std::vector<uint8_t> encoded(samples.size()*2);
      for(size_t i=0;i<samples.size();++i) { if(i%CancellationChunk==0) checkCancelled(cancelled); const uint16_t v=uint16_t(pcm(samples[i])); encoded[2*i]=uint8_t(v); encoded[2*i+1]=uint8_t(v>>8); }
      writeBytes(encodedFrame,encoded.data(),encoded.size(),cancelled); entry.info.tracks[t].samples+=samples.size();
      }
    const auto encoded=encodedFrame.str(); updateHash(payloadHash,encoded,cancelled);
    positions[f]={start,encoded.size(),flags,frameHash(flags,encoded,cancelled)}; writeBytes(output,encoded.data(),encoded.size(),cancelled);
    }
  entry.size=uint64_t(output.tellp()); const auto hash=payloadHash.finish();
  writeInfo(output,entry.info,positions,entry.size,hash); output.close(); entry.sha256=fileHash(file,cancelled); return entry;
  }
std::string infoJson(const Info& info) {
  std::string json="{\"width\":"+std::to_string(info.width)+",\"height\":"+std::to_string(info.height)+",\"frames\":"+std::to_string(info.frames)+",\"fps_num\":"+std::to_string(info.fpsNum)+",\"fps_den\":"+std::to_string(info.fpsDen)+",\"planes\":[";
  for(size_t i=0;i<4;++i) json+=std::string(i ? "," : "")+"["+std::to_string(info.planes[i].stride)+","+std::to_string(info.planes[i].rows)+"]";
  json+="],\"tracks\":[";
  for(size_t i=0;i<info.tracks.size();++i) { const auto& t=info.tracks[i]; json+=std::string(i ? "," : "")+"{\"sample_rate\":"+std::to_string(t.sampleRate)+",\"channels\":"+std::to_string(t.channels)+",\"samples\":"+std::to_string(t.samples)+"}"; }
  return json+"]}";
  }
void writeIndex(const fs::path& out,const Identity& id,const std::vector<Entry>& entries) {
  std::string json="{\"version\":1,\"fingerprint\":"+quoted(id.fingerprint)+",\"identity\":"+quoted(id.json)+",\"videos\":[";
  for(size_t i=0;i<entries.size();++i) {
    const auto& e=entries[i]; json+=std::string(i ? "," : "")+"{\"name\":"+quoted(e.source.name)+",\"path\":"+quoted(e.source.path.generic_string())+",\"source_size\":"+std::to_string(e.source.size)+",\"source_sha256\":"+quoted(e.source.sha256)+",\"file\":"+quoted(e.file)+",\"size\":"+std::to_string(e.size)+",\"sha256\":"+quoted(e.sha256)+",\"info\":"+infoJson(e.info)+"}";
    }
  json+="]";
  const auto checksum=Sha256::hash(json); json+=",\"index_sha256\":\""+checksum+"\"}\n";
  std::ofstream file(out/"videos.json",std::ios::binary); writeBytes(file,json.data(),json.size());
  }
void compare(const fs::path& file,Bink::Video& video) {
  Reader reader(file); reader.verifyPayload(); const auto expected=infoOf(video); const auto& info=reader.info();
  require(info.width==expected.width && info.height==expected.height && info.frames==expected.frames && info.fpsNum==expected.fpsNum && info.fpsDen==expected.fpsDen && info.tracks.size()==expected.tracks.size(),"BIK metadata differs");
  std::vector<uint64_t> samples(info.tracks.size());
  for(size_t t=0;t<samples.size();++t) require(info.tracks[t].sampleRate==expected.tracks[t].sampleRate && info.tracks[t].channels==expected.tracks[t].channels,"BIK audio metadata differs");
  for(uint32_t f=0;f<info.frames;++f) {
    const auto& original=video.nextFrame(); const auto native=reader.nextFrame();
    for(uint8_t p=0;p<4;++p) { require(info.planes[p].stride==expected.planes[p].stride && info.planes[p].rows==expected.planes[p].rows,"BIK plane layout differs"); require(std::memcmp(native.planes[p].data(),original.plane(p).data(),native.planes[p].size())==0,"BIK plane differs at frame "+std::to_string(f)); }
    for(size_t t=0;t<samples.size();++t) {
      const auto& a=original.audio(uint8_t(t)).samples; require(a.size()==native.audio[t].size(),"BIK sample count differs");
      for(size_t s=0;s<a.size();++s) require(pcm(a[s])==native.audio[t][s],"BIK PCM differs");
      samples[t]+=a.size();
      }
    }
  for(size_t t=0;t<samples.size();++t) require(samples[t]==info.tracks[t].samples,"BIK total samples differ");
  }
}

std::vector<Source> sources(const fs::path& gothic) { return Selected(gothic).list; }
Identity identify(const fs::path& gothic,const std::atomic_bool* cancelled) { return identityOf(Selected(gothic,cancelled).list); }
void build(const fs::path& gothic,const fs::path& out,const BaseImport::Progress& progress,const std::atomic_bool* cancelled,size_t workerLimit) {
  const auto started=std::chrono::steady_clock::now(); Selected selected(gothic,cancelled); const auto identity=identityOf(selected.list); fs::create_directories(out);
  struct Job { std::vector<uint8_t> bytes; std::unique_ptr<Input> input; std::unique_ptr<Bink::Video> video; };
  std::vector<Job> jobs(selected.list.size());
  // Bink constructors write global VLC and cosine tables. Complete all constructors before decoding
  // on worker threads.
  for(size_t i=0;i<jobs.size();++i) { checkCancelled(cancelled); auto& job=jobs[i]; job.bytes=bytes(*selected.nodes[i],cancelled); require(dataHash({reinterpret_cast<const char*>(job.bytes.data()),job.bytes.size()},cancelled)==selected.list[i].sha256,"BIK changed while importing"); job.input=std::make_unique<Input>(job.bytes,cancelled); job.video=std::make_unique<Bink::Video>(job.input.get(),selected.list[i].name); }
  const size_t threads=std::max<size_t>(1,std::min(workerLimit,jobs.size())); std::vector<Entry> entries(jobs.size()); std::atomic<size_t> cursor=0; std::exception_ptr error; std::mutex sync;
  const auto run=[&] {
    try { for(;;) { const size_t i=cursor.fetch_add(1); if(i>=jobs.size()) break; entries[i]=encode(selected.list[i],*jobs[i].video,out,cancelled); } }
    catch(...) { std::lock_guard<std::mutex> guard(sync); if(!error) error=std::current_exception(); cursor=jobs.size(); }
    };
  std::vector<std::thread> workers; for(size_t i=0;i<threads;++i) workers.emplace_back(run); for(auto& worker:workers) worker.join(); if(error) std::rethrow_exception(error);
  checkCancelled(cancelled);
  writeIndex(out,identity,entries);
  uint64_t total=fs::file_size(out/"videos.json");
  for(const auto& e:entries) { total+=e.size; std::cout<<"video "<<e.source.name<<" frames="<<e.info.frames<<" fps="<<e.info.fpsNum<<"/"<<e.info.fpsDen<<" dimensions="<<e.info.width<<"x"<<e.info.height<<" duration_s="<<(double(e.info.frames)*e.info.fpsDen/e.info.fpsNum)<<" bytes="<<e.size; for(size_t t=0;t<e.info.tracks.size();++t) std::cout<<" track"<<t<<"="<<e.info.tracks[t].samples<<"@"<<e.info.tracks[t].sampleRate<<"x"<<e.info.tracks[t].channels; std::cout<<"\n"; }
  const auto elapsed=std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count();
  const auto report="videos: "+std::to_string(entries.size())+" files, "+std::to_string(total)+" bytes, "+std::to_string(elapsed)+" s, "+std::to_string(threads)+" threads"; std::cout<<report<<"\n"; if(progress) progress(report);
  }
int convert(const fs::path& gothic,const fs::path& out,bool verifyOnly) {
  if(!verifyOnly) { build(gothic,out); return 0; }
  Selected selected(gothic); require(matches(out,identityOf(selected.list)),"video part does not match installation"); const auto entries=readIndex(out); require(entries.size()==selected.list.size(),"video corpus size differs");
  for(size_t i=0;i<entries.size();++i) { auto data=bytes(*selected.nodes[i]); Input input(data); Bink::Video video(&input,selected.list[i].name); compare(out/entries[i].file,video); }
  std::cout<<"videos: verified "<<entries.size()<<" files, every plane and PCM sample\n"; return 0;
  }
}
