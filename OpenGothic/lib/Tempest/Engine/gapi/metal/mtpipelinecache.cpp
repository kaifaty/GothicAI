#if defined(TEMPEST_BUILD_METAL)

#include "mtpipelinecache.h"

#include <Tempest/Log>

#include "gapi/pipelinecache.h"

#include <chrono>
#include <mutex>

using namespace Tempest;
using namespace Tempest::Detail;

namespace fs = std::filesystem;

// Bump when the way pipelines are put into the archive changes.
static constexpr const char* MetalArchiveFormat = "tempest-metal-archive-1";
// Re-serializing an archive loaded from disk can grow it far beyond its content (5 MB -> 27 MB after
// one added pipeline on macOS 27); a larger file is dropped and the archive starts over.
static constexpr uintmax_t   MaxArchiveSize     = 64u*1024u*1024u;

static NsPtr<NS::URL> fileUrl(const fs::path& p) {
  auto str = NsPtr<NS::String>(NS::String::string(p.c_str(),NS::UTF8StringEncoding));
  str->retain();
  auto url = NsPtr<NS::URL>(NS::URL::fileURLWithPath(str.get()));
  url->retain();
  return url;
  }

static const char* errorText(NS::Error* err) {
  if(err==nullptr || err->localizedDescription()==nullptr)
    return "unknown error";
  return err->localizedDescription()->utf8String();
  }

MtPipelineCache::~MtPipelineCache() {
  if(saver.joinable())
    saver.join();
  save("exit");
  }

void MtPipelineCache::saveAsync() {
  if(archive==nullptr)
    return;
  if(saver.joinable())
    saver.join();
  saver = std::thread([this](){ save("play"); });
  }

void MtPipelineCache::init(MTL::Device& dev, const std::string& identity) {
  auto pool = NsPtr<NS::AutoreleasePool>::init();
  const auto dir = PipelineCacheFile::directory();
  if(dir.empty()) {
    Log::i("pipeline cache: off");
    return;
    }

  const std::string key = identity+"\n"+MetalArchiveFormat;
  file = dir/("metal-"+PipelineCacheFile::hex(PipelineCacheFile::hash(key.data(),key.size()))+".bin");

  std::error_code ec;
  fs::create_directories(dir,ec);
  const uintmax_t size   = fs::is_regular_file(file,ec) ? fs::file_size(file,ec) : 0;
  const bool      exists = !ec && size>0 && size<=MaxArchiveSize;
  if(!ec && size>MaxArchiveSize)
    Log::i("pipeline cache: ignored ",file.string(),": too large (",size_t(size)," bytes)");

  auto desc = NsPtr<MTL::BinaryArchiveDescriptor>::init();
  if(exists) {
    auto url = fileUrl(file);
    desc->setUrl(url.get());
    NS::Error* err = nullptr;
    archive = NsPtr<MTL::BinaryArchive>(dev.newBinaryArchive(desc.get(),&err));
    if(archive==nullptr || err!=nullptr) {
      Log::i("pipeline cache: ignored ",file.string(),": ",errorText(err));
      archive = nullptr;
      }
    }
  if(archive==nullptr) {
    desc->setUrl(nullptr);
    NS::Error* err = nullptr;
    archive = NsPtr<MTL::BinaryArchive>(dev.newBinaryArchive(desc.get(),&err));
    if(archive==nullptr || err!=nullptr) {
      Log::i("pipeline cache: unavailable: ",errorText(err));
      archive = nullptr;
      return;
      }
    Log::i("pipeline cache: empty, will write ",file.string());
    } else {
    Log::i("pipeline cache: loaded ",file.string());
    }

  list = NsPtr<NS::Array>(NS::Array::array(archive.get()));
  list->retain();
  }

template<class Desc, class State, class Make, class Add>
State* MtPipelineCache::create(Desc& desc, NS::Error** err, Make make, Add add) {
  if(archive==nullptr)
    return make(MTL::PipelineOptionNone,err);

  // No local autorelease pool: *err must stay valid for the caller.
  {
    std::shared_lock<std::shared_mutex> guard(sync);
    desc.setBinaryArchives(list.get());
    NS::Error* missErr = nullptr;
    State* ret = make(MTL::PipelineOptionFailOnBinaryArchiveMiss,&missErr);
    desc.setBinaryArchives(nullptr);
    if(ret!=nullptr) {
      hits.fetch_add(1,std::memory_order_relaxed);
      return ret;
      }
  }

  misses.fetch_add(1,std::memory_order_relaxed);
  NS::Error* addErr = nullptr;
  bool       added  = false;
  {
    std::unique_lock<std::shared_mutex> guard(sync);
    // Compiles the pipeline functions into the archive; the state below then loads them from it.
    added = add(&addErr);
    if(added)
      dirty = true;
  }
  if(!added) {
    failures.fetch_add(1,std::memory_order_relaxed);
    return make(MTL::PipelineOptionNone,err);
    }

  std::shared_lock<std::shared_mutex> guard(sync);
  desc.setBinaryArchives(list.get());
  State* ret = make(MTL::PipelineOptionNone,err);
  desc.setBinaryArchives(nullptr);
  return ret;
  }

MTL::RenderPipelineState* MtPipelineCache::render(MTL::Device& dev, MTL::RenderPipelineDescriptor& desc, NS::Error** err) {
  return create<MTL::RenderPipelineDescriptor,MTL::RenderPipelineState>(desc,err,
    [&](MTL::PipelineOption opt, NS::Error** e) { return dev.newRenderPipelineState(&desc,opt,nullptr,e); },
    [&](NS::Error** e) { return archive->addRenderPipelineFunctions(&desc,e); });
  }

MTL::ComputePipelineState* MtPipelineCache::compute(MTL::Device& dev, MTL::ComputePipelineDescriptor& desc, NS::Error** err) {
  return create<MTL::ComputePipelineDescriptor,MTL::ComputePipelineState>(desc,err,
    [&](MTL::PipelineOption opt, NS::Error** e) { return dev.newComputePipelineState(&desc,opt,nullptr,e); },
    [&](NS::Error** e) { return archive->addComputePipelineFunctions(&desc,e); });
  }

void MtPipelineCache::save(const char* reason) {
  if(archive==nullptr)
    return;
  auto pool = NsPtr<NS::AutoreleasePool>::init();
  std::lock_guard<std::mutex> saveGuard(saveSync);
  // Shared: serializing only reads the archive, so pipeline lookups go on; additions wait.
  std::shared_lock<std::shared_mutex> guard(sync);
  if(!dirty) {
    Log::i("pipeline cache: unchanged (",reason,", hits=",hits.load()," misses=",misses.load()," failures=",failures.load(),")");
    return;
    }
  const auto start = std::chrono::steady_clock::now();
  const auto tmp   = PipelineCacheFile::temporaryPath(file);
  auto       url   = fileUrl(tmp);
  NS::Error* err   = nullptr;
  if(!archive->serializeToURL(url.get(),&err) || err!=nullptr) {
    std::error_code ignored;
    fs::remove(tmp,ignored);
    Log::e("pipeline cache: write failed: ",file.string()," (",errorText(err),")");
    return;
    }
  std::error_code ec;
  fs::rename(tmp,file,ec);
  if(ec) {
    fs::remove(tmp,ec);
    Log::e("pipeline cache: write failed: ",file.string()," (rename)");
    return;
    }
  dirty = false;
  const auto ms   = std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
  const auto size = fs::file_size(file,ec);
  Log::i("pipeline cache: saved ",size_t(ec ? 0 : size)," bytes in ",int(ms)," ms (",reason,", hits=",hits.load(),
         " misses=",misses.load()," failures=",failures.load(),")");
  }

#endif
