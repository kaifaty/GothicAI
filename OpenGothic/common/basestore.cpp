#include "basestore.h"
#include "videodata.h"
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <system_error>
#include <thread>
#ifndef _WIN32
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

class BaseStore::Lock {
  public:
    enum Mode { Exclusive, Shared, TryExclusive };
    explicit Lock(const fs::path& path, Mode mode=Exclusive, const std::atomic_bool* cancelled=nullptr) {
#ifndef _WIN32
      if(cancelled && cancelled->load()) throw std::runtime_error("video preparation cancelled");
      fd=::open(path.c_str(),O_CREAT|O_RDWR|O_CLOEXEC,0600);
      if(fd<0) throw std::system_error(errno,std::generic_category(),"open base lock");
      const int operation=(mode==Shared ? LOCK_SH : LOCK_EX) | ((mode==TryExclusive || cancelled!=nullptr) ? LOCK_NB : 0);
      while(::flock(fd,operation)!=0) {
        const int error=errno;
        if(cancelled && !cancelled->load() && mode!=TryExclusive && (error==EWOULDBLOCK || error==EAGAIN)) {
          std::this_thread::sleep_for(std::chrono::milliseconds(20));
          continue;
          }
        if(error==EINTR && (!cancelled || !cancelled->load())) continue;
        ::close(fd);
        fd=-1;
        if(cancelled && cancelled->load()) throw std::runtime_error("video preparation cancelled");
        if(mode==TryExclusive && (error==EWOULDBLOCK || error==EAGAIN || error==EACCES)) return;
        throw std::system_error(error,std::generic_category(),"lock base");
        }
#else
      (void)path;
      (void)mode;
      (void)cancelled;
      throw std::runtime_error("automatic base import requires Unix file locks");
#endif
      }
    ~Lock() {
#ifndef _WIN32
      ::close(fd);
#endif
      }
    Lock(const Lock&)=delete;
    Lock& operator=(const Lock&)=delete;
    bool acquired() const { return fd>=0; }
  private:
    int fd=-1;
  };

namespace {
using BaseStore::Lock;

std::atomic_bool publicationEnabled{true};

bool fingerprintName(std::string_view name) {
  return name.size()==64 && name.find_first_not_of("0123456789abcdef")==std::string_view::npos;
  }

fs::path storeRoot() {
  if(const auto* env=std::getenv("OPENGOTHIC_BASE_STORE")) return env;
#ifdef __APPLE__
  const fs::path shared="/Volumes/shared/gothic-ai";
#else
  const fs::path shared="/mnt/shared/gothic-ai";
#endif
  std::error_code error;
  return fs::is_directory(shared,error) ? shared : fs::path();
  }

bool hasInstallation(const fs::path& gothic) {
  if(!fs::is_directory(gothic)) return false;
  const auto archives=BaseImport::runtimeArchives(gothic);
  return std::any_of(archives.begin(),archives.end(),[](const auto& archive) {
    return !archive.mod && archive.name.size()>=4 && archive.name.ends_with(".VDF");
    });
  }

// Only pointer I/O holds this lock; imports keep their independent cache locks.
class Current {
  public:
    Current(const fs::path& cache, const fs::path& gothic, const std::atomic_bool* cancelled=nullptr)
      :gothic(fs::weakly_canonical(gothic).generic_string()),cancelled(cancelled) {
      file=cache/"current"/(BaseImport::sha256(this->gothic)+".json");
      fs::create_directories(file.parent_path());
      Lock lock(file.parent_path()/".lock",Lock::Exclusive,cancelled);
      read();
      }

    template<class Identity>
    bool part(const char* name, fs::path& path, Identity& identity, const std::string* snapshot) const {
      if(!json.HasMember(name) || !json[name].IsObject()) return false;
      const auto& part=json[name];
      for(const char* key:{"path","fingerprint","identity","snapshot"})
        if(!part.HasMember(key) || !part[key].IsString()) return false;
      if(snapshot && *snapshot!=part["snapshot"].GetString()) return false;
      identity={part["fingerprint"].GetString(),part["identity"].GetString()};
      if(!fingerprintName(identity.fingerprint)) return false;
      path=fs::path(part["path"].GetString());
      return path.is_absolute();
      }

    template<class Identity>
    void save(const char* name, const fs::path& path, const Identity& identity, const std::string& snapshot) {
      Lock lock(file.parent_path()/".lock",Lock::Exclusive,cancelled);
      // The other part may have finished its selection since this object was read.
      read();
      std::string savedSnapshot=snapshot;
      if(snapshot.empty() && json.HasMember(name) && json[name].IsObject()) {
        const auto& previous=json[name];
        if(previous.HasMember("fingerprint") && previous["fingerprint"].IsString() &&
           previous["fingerprint"].GetString()==identity.fingerprint &&
           previous.HasMember("snapshot") && previous["snapshot"].IsString())
          savedSnapshot=previous["snapshot"].GetString();
        }
      rapidjson::Value part(rapidjson::kObjectType);
      auto& allocator=json.GetAllocator();
      part.AddMember("path",string(fs::weakly_canonical(path).generic_string()),allocator);
      part.AddMember("fingerprint",string(identity.fingerprint),allocator);
      part.AddMember("identity",string(identity.json),allocator);
      part.AddMember("snapshot",string(savedSnapshot),allocator);
      json.RemoveMember(name);
      json.AddMember(string(name),part,allocator);
      rapidjson::StringBuffer bytes;
      rapidjson::Writer<rapidjson::StringBuffer> writer(bytes);
      json.Accept(writer);
      const fs::path temporary=file.string()+".tmp-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
      try {
        std::ofstream output(temporary,std::ios::binary|std::ios::trunc);
        output.write(bytes.GetString(),std::streamsize(bytes.GetSize()));
        output.close();
        if(!output) throw std::runtime_error("cannot write current base pointer: "+temporary.string());
        fs::rename(temporary,file);
        }
      catch(...) { std::error_code error; fs::remove(temporary,error); throw; }
      }
  private:
    void read() {
      std::ifstream input(file,std::ios::binary);
      const std::string bytes{std::istreambuf_iterator<char>(input),{}};
      json.Parse(bytes.c_str());
      if(json.HasParseError() || !json.IsObject() || !json.HasMember("version") || json["version"]!=1 ||
         !json.HasMember("gothic") || !json["gothic"].IsString() || json["gothic"].GetString()!=gothic) {
        json.SetObject();
        json.AddMember("version",1,json.GetAllocator());
        json.AddMember("gothic",string(gothic),json.GetAllocator());
        }
      }
    rapidjson::Value string(const std::string& text) {
      return rapidjson::Value(text.data(),rapidjson::SizeType(text.size()),json.GetAllocator());
      }
    std::string gothic;
    fs::path file;
    rapidjson::Document json;
    const std::atomic_bool* cancelled;
  };

std::runtime_error missingBase(const fs::path& gothic, const std::string& reason={}) {
  return std::runtime_error("no ready base for "+gothic.string()+
    "; start through the launcher or run content-convert resolve <game directory>"+(reason.empty() ? "" : ": "+reason));
  }

VideoData::Identity readVideoIdentity(const fs::path& directory) {
  std::ifstream input(directory/"videos.json",std::ios::binary);
  const std::string bytes{std::istreambuf_iterator<char>(input),{}};
  rapidjson::Document json;
  json.Parse(bytes.c_str());
  if(json.HasParseError() || !json.IsObject() || !json.HasMember("fingerprint") ||
     !json["fingerprint"].IsString() || !json.HasMember("identity") || !json["identity"].IsString())
    throw std::runtime_error("invalid video base identity: "+(directory/"videos.json").string());
  return {json["fingerprint"].GetString(),json["identity"].GetString()};
  }

bool videoMatches(const fs::path& directory, const VideoData::Identity& identity,
                  const std::atomic_bool* cancelled, std::string& reason) {
  rapidjson::Document source;
  source.Parse(identity.json.c_str());
  if(source.HasParseError() || !source.IsObject() || !source.HasMember("format_sha256") ||
     !source["format_sha256"].IsString() || source["format_sha256"].GetString()!=VideoData::formatSha256()) {
    reason="video importer format differs";
    return false;
    }
  try {
    // readIndex preserves the existing index checksum/structure error for rejection diagnostics.
    VideoData::readIndex(directory);
    if(VideoData::matches(directory,identity,false,cancelled)) return true;
    reason="video identity or payload presence/size differs: "+directory.string();
    }
  catch(const std::exception& error) { reason=error.what(); }
  return false;
  }

void checkVideoCancelled(const std::atomic_bool* cancelled) {
  if(cancelled && cancelled->load()) throw std::runtime_error("video preparation cancelled");
  }

void removeVideoDirectory(const fs::path& path, const std::atomic_bool* cancelled) {
  checkVideoCancelled(cancelled);
  if(!fs::exists(path)) return;
  for(const auto& entry:fs::directory_iterator(path)) {
    checkVideoCancelled(cancelled);
    if(entry.is_directory() && !entry.is_symlink()) removeVideoDirectory(entry.path(),cancelled);
    else fs::remove(entry.path());
    }
  checkVideoCancelled(cancelled);
  fs::remove(path);
  }

// Called only while holding the cache lock or this store fingerprint's lock.
void removeInterrupted(const fs::path& root, const std::string& fingerprint={}, const std::atomic_bool* cancelled=nullptr) {
  for(const auto& entry:fs::directory_iterator(root)) {
    checkVideoCancelled(cancelled);
    const auto name=entry.path().filename().string();
    if(name.size()>69 && fingerprintName(std::string_view(name).substr(0,64)) &&
       name.compare(64,5,".tmp-")==0 && name.find_first_not_of("0123456789",69)==std::string::npos &&
       (fingerprint.empty() || name.compare(0,64,fingerprint)==0) && entry.is_directory()) {
      if(cancelled) removeVideoDirectory(entry.path(),cancelled);
      else fs::remove_all(entry.path());
      }
    }
  }

struct Temporary {
  fs::path path;
  const std::atomic_bool* cancelled=nullptr;
  explicit Temporary(const fs::path& destination, const std::atomic_bool* cancelled=nullptr):cancelled(cancelled) {
    path=destination.string()+".tmp-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    if(!fs::create_directory(path)) throw std::runtime_error("base temporary directory already exists");
    }
  ~Temporary() {
    if(cancelled) {
      // Cancellation leaves the partial directory for the next startup.
      try { removeVideoDirectory(path,cancelled); } catch(...) {}
      return;
      }
    std::error_code error;
    fs::remove_all(path,error);
    }
  };

bool publicationMatches(const fs::path& directory,const BaseImport::Identity& identity) {
  return BaseImport::matches(directory,identity,true);
  }

template<class Identity>
void publish(const fs::path& temporary, const fs::path& destination, const Identity& identity) {
  if(!publicationMatches(temporary,identity)) throw std::runtime_error("base metadata or payload does not match installation");
  // The caller holds the destination lock. Valid published directories are immutable.
  if(fs::exists(destination)) fs::remove_all(destination);
  fs::rename(temporary,destination);
  }

template<class Identity>
void copy(const fs::path& source, const fs::path& destination, const Identity& identity) {
  Temporary temporary(destination);
  fs::copy(source,temporary.path,fs::copy_options::recursive);
  publish(temporary.path,destination,identity);
  }

void publishVideo(const fs::path& temporary, const fs::path& destination,
                  const VideoData::Identity& identity, const std::atomic_bool* cancelled) {
  checkVideoCancelled(cancelled);
  if(!VideoData::matches(temporary,identity,true,cancelled)) {
    checkVideoCancelled(cancelled);
    throw std::runtime_error("video metadata or payload does not match installation");
    }
  checkVideoCancelled(cancelled);
  removeVideoDirectory(destination,cancelled);
  fs::rename(temporary,destination);
  }

void copyVideo(const fs::path& source, const fs::path& destination,
               const VideoData::Identity& identity, const std::atomic_bool* cancelled) {
  Temporary temporary(destination,cancelled);
  std::vector<char> buffer(1<<16);
  for(const auto& entry:fs::recursive_directory_iterator(source)) {
    checkVideoCancelled(cancelled);
    const auto target=temporary.path/fs::relative(entry.path(),source);
    if(entry.is_directory()) { fs::create_directory(target); continue; }
    std::ifstream input(entry.path(),std::ios::binary);
    std::ofstream output(target,std::ios::binary);
    if(!input || !output) throw std::runtime_error("cannot copy video part");
    for(;;) {
      checkVideoCancelled(cancelled);
      input.read(buffer.data(),std::streamsize(buffer.size()));
      const auto count=input.gcount();
      checkVideoCancelled(cancelled);
      output.write(buffer.data(),count);
      if(!output || (!input && !input.eof())) throw std::runtime_error("cannot copy video part");
      if(input.eof()) break;
      }
    }
  publishVideo(temporary.path,destination,identity,cancelled);
  }

void prune(const fs::path& cache, const fs::path& current, const std::atomic_bool* cancelled=nullptr) {
  checkVideoCancelled(cancelled);
  fs::last_write_time(current,fs::file_time_type::clock::now());
  std::vector<fs::directory_entry> older;
  for(const auto& entry:fs::directory_iterator(cache)) {
    checkVideoCancelled(cancelled);
    const auto name=entry.path().filename().string();
    if(entry.path()==current || !entry.is_directory() || !fingerprintName(name)) continue;
    older.push_back(entry);
    }
  std::sort(older.begin(),older.end(),[](const auto& a,const auto& b) { return a.last_write_time()>b.last_write_time(); });
  for(size_t i=2;i<older.size();++i) {
    Lock unused(cache/(older[i].path().filename().string()+".use"),Lock::TryExclusive);
    if(unused.acquired()) {
      if(cancelled) removeVideoDirectory(older[i].path(),cancelled);
      else fs::remove_all(older[i].path());
      }
    }
  }
}

void BaseStore::setPublication(bool enabled) { publicationEnabled=enabled; }

fs::path BaseStore::cacheRoot() {
  fs::path cache;
  if(const auto* env=std::getenv("OPENGOTHIC_BASE_CACHE")) cache=env;
  else if(const auto* home=std::getenv("HOME")) cache=fs::path(home)/".cache/gothic-ai/base";
  if(cache.empty()) throw std::runtime_error("base cache path is empty");
  return fs::weakly_canonical(fs::absolute(cache));
  }

BaseStore::Selection BaseStore::resolve(const fs::path& gothic, const BaseImport::Files& mounted, const BaseImport::Progress& log, const Import& import) {
  const auto start=std::chrono::steady_clock::now();
  const auto cache=cacheRoot();
  fs::create_directories(cache);
  const auto* explicitBase=std::getenv("OPENGOTHIC_BASE");
  std::unique_ptr<Lock> cacheLock;
  if(!explicitBase || !*explicitBase) {
    cacheLock=std::make_unique<Lock>(cache/".lock");
    removeInterrupted(cache);
    }
  const auto* fingerprint=std::getenv("OPENGOTHIC_BASE_FINGERPRINT");
  if((!explicitBase || !*explicitBase) && fingerprint && *fingerprint) {
    if(!fingerprintName(fingerprint)) throw std::runtime_error("invalid OPENGOTHIC_BASE_FINGERPRINT");
    const auto destination=cache/fingerprint;
    const auto store=storeRoot();
    const auto stored=store.empty() ? fs::path() : store/"base"/fingerprint;
    std::string rejection;
    for(const auto& source:{destination,stored}) {
      if(source.empty() || !fs::exists(source)) continue;
      try {
        const auto identity=BaseImport::readIdentity(source);
        if(identity.fingerprint!=fingerprint) throw std::runtime_error("base fingerprint differs");
        if(!BaseImport::matches(source,identity,false,&rejection)) throw std::runtime_error(rejection);
        if(source!=destination) {
          Lock replacement(cache/(identity.fingerprint+".use"),Lock::TryExclusive);
          if(!replacement.acquired()) throw std::runtime_error("cannot replace a base cache part in use");
          copy(source,destination,identity);
          }
        Current(cache,gothic).save("base",destination,identity,"");
        Selection selected{destination,std::make_shared<Lock>(cache/(identity.fingerprint+".use"),Lock::Shared),{}};
        try { prune(cache,destination); }
        catch(const std::exception& error) { log(std::string("base cache cleanup: ")+error.what()); }
        log(std::string("base: ")+(source==destination ? "cache " : "store ")+destination.string());
        return selected;
        }
      catch(const std::exception& error) { rejection=error.what(); }
      }
    throw std::runtime_error(std::string("no ready base for OPENGOTHIC_BASE_FINGERPRINT=")+fingerprint+
                             (rejection.empty() ? "; not found in cache or store" : ": "+rejection));
    }
  const bool directBase=explicitBase && *explicitBase && fs::weakly_canonical(explicitBase)==fs::weakly_canonical(gothic);
  std::unique_ptr<Current> current;
  if(!directBase) current=std::make_unique<Current>(cache,gothic);
  const bool installed=!directBase && hasInstallation(gothic);
  const auto snapshot=installed ? BaseImport::snapshot(gothic,mounted) : std::string();
  BaseImport::Identity identity;
  fs::path pointed;
  bool reused=current && current->part("base",pointed,identity,installed ? &snapshot : nullptr);
  const auto identify=[&] {
    log("base: identifying installation");
    return BaseImport::identify(gothic,mounted);
    };
  if(explicitBase && *explicitBase) {
    if(installed && reused) {
      rapidjson::Document previous, selected;
      previous.Parse(identity.json.c_str());
      selected.Parse(BaseImport::readIdentity(explicitBase).json.c_str());
      reused=!previous.HasParseError() && previous.IsObject() && previous.HasMember("format_sha256") &&
             previous["format_sha256"]==selected["format_sha256"];
      }
    if(!installed) identity=BaseImport::readIdentity(explicitBase);
    else if(!reused) identity=identify();
    Selection selected{explicitBase,{},{}};
    if(fs::weakly_canonical(fs::path(explicitBase).parent_path())==cache)
      selected.lease=std::make_shared<Lock>(cache/(identity.fingerprint+".use"),Lock::Shared);
    std::string reason;
    if(!BaseImport::matches(explicitBase,identity,false,&reason))
      throw std::runtime_error(std::string("OPENGOTHIC_BASE does not match this installation or importer: ")+explicitBase+": "+reason);
    if(current) current->save("base",explicitBase,identity,snapshot);
    return selected;
    }
  std::string rejection;
  if(reused && !BaseImport::matches(pointed,identity,false,&rejection)) reused=false;
  if(!installed && !reused) throw missingBase(gothic,rejection);
  if(!reused && !import) {
    const auto ready=[](const fs::path& root) {
      std::error_code error;
      for(const auto& entry:fs::directory_iterator(root,error))
        if(fingerprintName(entry.path().filename().string()) && fs::is_regular_file(entry.path()/"base.json",error))
          return true;
      return false;
      };
    const auto store=storeRoot();
    if(!ready(cache) && (store.empty() || !ready(store/"base"))) throw missingBase(gothic,rejection);
    }
  if(!reused) identity=identify();
  const auto destination=reused ? pointed : cache/identity.fingerprint;
  auto store=storeRoot();
  const auto finish=[&](const char* source, bool upload=true) {
    Selection selected{destination,{},{}};
    current->save("base",destination,identity,snapshot);
    if(destination.parent_path()==cache) {
      selected.lease=std::make_shared<Lock>(cache/(identity.fingerprint+".use"),Lock::Shared);
      try { prune(cache,destination); }
      catch(const std::exception& e) { log(std::string("base cache cleanup: ")+e.what()); }
      }
    else upload=false;
    const auto ms=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-start).count();
    log(std::string("base: ")+source+" "+destination.string()+" ("+std::to_string(ms)+" ms)");
    if(upload && publicationEnabled && !store.empty()) {
      // The worker owns a lease too: pruning cannot remove its source while copying.
      std::packaged_task<void()> publication([store,destination,identity,lease=selected.lease] {
        const auto started=std::chrono::steady_clock::now();
        try {
          const auto root=store/"base";
          fs::create_directories(root);
          Lock lock(root/(identity.fingerprint+".lock"));
          removeInterrupted(root,identity.fingerprint);
          const auto stored=root/identity.fingerprint;
          if(BaseImport::matches(stored,identity)) return;
          copy(destination,stored,identity);
          const auto ms=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-started).count();
          // The worker may outlive Resources and Tempest's logging callback.
          std::fprintf(stderr,"base: published to store %s (%lld ms, background)\n",
                       stored.string().c_str(),static_cast<long long>(ms));
          }
        catch(const std::exception& e) { std::fprintf(stderr,"base: store publication failed: %s\n",e.what()); }
        });
      selected.publication=publication.get_future();
      // A packaged-task future does not join on destruction. Process exit can
      // interrupt the copy; the next publisher removes its temporary directory.
      std::thread(std::move(publication)).detach();
      }
    return selected;
    };
  if(reused) return finish(destination.parent_path()==cache ? "cache" : "pointer");
  if(BaseImport::matches(destination,identity,false,&rejection)) return finish("cache");
  if(fs::exists(destination)) log("base: rejected cache directory: "+rejection);

  std::unique_ptr<Lock> storeLock;
  fs::path stored;
  if(!store.empty()) {
    stored=store/"base"/identity.fingerprint;
    try {
      fs::create_directories(store/"base");
      log("base: waiting for store lock");
      storeLock=std::make_unique<Lock>(store/"base"/(identity.fingerprint+".lock"));
      removeInterrupted(store/"base",identity.fingerprint);
      }
    catch(const std::exception& e) {
      log(std::string("base: store unavailable: ")+e.what());
      storeLock.reset();
      }
    // Also allow copying from a read-only store when opening its lock failed.
    try {
      if(BaseImport::matches(stored,identity,false,&rejection)) {
        copy(stored,destination,identity);
        return finish("store",false);
        }
      if(fs::exists(stored)) log("base: rejected store directory: "+rejection);
      }
    catch(const std::exception& e) {
      log(std::string("base: store unavailable: ")+e.what());
      storeLock.reset();
      }
    }
  if(!import) throw missingBase(gothic,rejection);
  // The current importer supports the installed Gothic II Data/*.vdf set. Never
  // label a base built from a different set (e.g. selected mods) with this identity.
  log("base: identifying installation");
  if(BaseImport::identify(gothic).json!=identity.json)
    throw std::runtime_error("mounted archives differ from the supported installation import");
  Temporary temporary(destination);
  log("base: importing installation");
  import(temporary.path);
  storeLock.reset();
  publish(temporary.path,destination,identity);
  return finish("import");
  }

BaseStore::Selection BaseStore::resolveVideos(const fs::path& gothic, const fs::path& mainBase,
                                             const BaseImport::Progress& log,
                                             std::shared_ptr<std::atomic_bool> cancelled, const Import& import,
                                             const IdentifyVideos& identify) {
  const auto start=std::chrono::steady_clock::now();
  const auto* fingerprint=std::getenv("OPENGOTHIC_VIDEOS_FINGERPRINT");
  if(fingerprint && !*fingerprint)
    throw std::runtime_error("video selection disabled: OPENGOTHIC_VIDEOS_FINGERPRINT is empty");
  const auto root=cacheRoot();
  fs::create_directories(root);
  const auto cache=root/"videos";
  fs::create_directories(cache);
  Lock cacheLock(cache/".lock",Lock::Exclusive,cancelled.get());
  if(fingerprint) {
    if(!fingerprintName(fingerprint)) throw std::runtime_error("invalid OPENGOTHIC_VIDEOS_FINGERPRINT");
    removeInterrupted(cache,{},cancelled.get());
    const auto destination=cache/fingerprint;
    const auto store=storeRoot();
    const auto stored=store.empty() ? fs::path() : store/"base/videos"/fingerprint;
    std::string rejection;
    for(const auto& source:{destination,stored}) {
      checkVideoCancelled(cancelled.get());
      if(source.empty() || !fs::exists(source)) continue;
      try {
        const auto identity=readVideoIdentity(source);
        if(identity.fingerprint!=fingerprint) throw std::runtime_error("video fingerprint differs");
        if(!videoMatches(source,identity,cancelled.get(),rejection)) throw std::runtime_error(rejection);
        if(source!=destination) {
          Lock replacement(cache/(identity.fingerprint+".use"),Lock::TryExclusive);
          if(!replacement.acquired()) throw std::runtime_error("cannot replace a video cache part in use");
          copyVideo(source,destination,identity,cancelled.get());
          }
        Current(root,gothic,cancelled.get()).save("videos",destination,identity,"");
        Selection selected{destination,std::make_shared<Lock>(cache/(identity.fingerprint+".use"),Lock::Shared,cancelled.get()),{}};
        try { prune(cache,destination,cancelled.get()); }
        catch(const std::exception& error) { log(std::string("video base cache cleanup: ")+error.what()); }
        checkVideoCancelled(cancelled.get());
        log(std::string("video base: ")+(source==destination ? "cache " : "store ")+destination.string());
        return selected;
        }
      catch(const std::exception& error) { rejection=error.what(); }
      }
    checkVideoCancelled(cancelled.get());
    throw std::runtime_error(std::string("no ready video base for OPENGOTHIC_VIDEOS_FINGERPRINT=")+fingerprint+
                             (rejection.empty() ? "; not found in cache or store" : ": "+rejection));
    }
  Current current(root,gothic,cancelled.get());
  const bool installed=hasInstallation(gothic);
  const auto snapshot=installed ? BaseImport::snapshot(gothic) : std::string();
  VideoData::Identity identity;
  fs::path pointed;
  bool reused=current.part("videos",pointed,identity,installed ? &snapshot : nullptr);
  std::string rejection;
  if(reused && !videoMatches(pointed,identity,cancelled.get(),rejection)) reused=false;
  checkVideoCancelled(cancelled.get());
  if(!installed && !reused && (identify || mainBase.empty()))
    throw std::runtime_error("no ready video base for "+gothic.string()+": "+
                             (rejection.empty() ? "missing from video base" : rejection));
  if(!reused) {
    log("video base: identifying installation");
    if(identify) identity=identify();
    else {
      if(mainBase.empty()) throw std::runtime_error("video selection requires a selected main base");
      identity=VideoData::identifyFromBase(mainBase,cancelled.get());
      }
    log("video base: identified installation ("+std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now()-start).count())+" ms)");
    }
  checkVideoCancelled(cancelled.get());
  removeInterrupted(cache,{},cancelled.get());
  const auto destination=reused ? pointed : cache/identity.fingerprint;
  auto store=storeRoot();
  if(!store.empty()) store/="base/videos";
  const auto finish=[&](const char* source, bool upload=true) {
    checkVideoCancelled(cancelled.get());
    Selection selected{destination,std::make_shared<Lock>(cache/(identity.fingerprint+".use"),Lock::Shared,cancelled.get()),{}};
    current.save("videos",destination,identity,snapshot);
    try { prune(cache,destination,cancelled.get()); }
    catch(const std::exception& e) { log(std::string("video base cache cleanup: ")+e.what()); }
    const auto ms=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-start).count();
    log(std::string("video base: ")+source+" "+destination.string()+" ("+std::to_string(ms)+" ms)");
    if(upload && !store.empty()) {
      std::packaged_task<void()> publication([store,destination,identity,cancelled,lease=selected.lease] {
        const auto started=std::chrono::steady_clock::now();
        try {
          fs::create_directories(store);
          std::shared_ptr<Lock> storedLease;
          {
          Lock cleanup(store/".lock",Lock::Exclusive,cancelled.get());
          storedLease=std::make_shared<Lock>(store/(identity.fingerprint+".use"),Lock::Shared,cancelled.get());
          }
          Lock lock(store/(identity.fingerprint+".lock"),Lock::Exclusive,cancelled.get());
          removeInterrupted(store,identity.fingerprint,cancelled.get());
          const auto stored=store/identity.fingerprint;
          const bool present=VideoData::matches(stored,identity,false,cancelled.get());
          checkVideoCancelled(cancelled.get());
          if(!present) {
            if(fs::exists(stored)) {
            Lock cleanup(store/".lock",Lock::Exclusive,cancelled.get());
            storedLease.reset();
            storedLease=std::make_shared<Lock>(store/(identity.fingerprint+".use"),Lock::TryExclusive);
            if(!storedLease->acquired()) throw std::runtime_error("cannot replace a video store part in use");
            removeVideoDirectory(stored,cancelled.get());
            storedLease.reset();
            storedLease=std::make_shared<Lock>(store/(identity.fingerprint+".use"),Lock::Shared,cancelled.get());
            }
            copyVideo(destination,stored,identity,cancelled.get());
            }
          {
          Lock cleanup(store/".lock",Lock::Exclusive,cancelled.get());
          prune(store,stored,cancelled.get());
          }
          const auto ms=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-started).count();
          std::fprintf(stderr,"video base: %s %s (%lld ms, background)\n",
                       present ? "verified store" : "published to store",stored.string().c_str(),static_cast<long long>(ms));
          }
        catch(const std::exception& e) { std::fprintf(stderr,"video base: store publication failed: %s\n",e.what()); }
        });
      selected.publication=publication.get_future();
      std::thread(std::move(publication)).detach();
      }
    return selected;
    };
  const bool cached=reused || videoMatches(destination,identity,cancelled.get(),rejection);
  checkVideoCancelled(cancelled.get());
  if(cached) return finish("cache");
  if(fs::exists(destination)) {
    log("video base: rejected cache directory: "+rejection);
    Lock replacement(cache/(identity.fingerprint+".use"),Lock::TryExclusive);
    if(!replacement.acquired()) throw std::runtime_error("cannot replace a video cache part in use");
    removeVideoDirectory(destination,cancelled.get());
    }

  std::unique_ptr<Lock> storeLock;
  std::shared_ptr<Lock> storedLease;
  fs::path stored;
  if(!store.empty()) {
    stored=store/identity.fingerprint;
    try {
      fs::create_directories(store);
      {
      Lock cleanup(store/".lock",Lock::Exclusive,cancelled.get());
      storedLease=std::make_shared<Lock>(store/(identity.fingerprint+".use"),Lock::Shared,cancelled.get());
      }
      log("video base: waiting for store lock");
      storeLock=std::make_unique<Lock>(store/(identity.fingerprint+".lock"),Lock::Exclusive,cancelled.get());
      removeInterrupted(store,identity.fingerprint,cancelled.get());
      }
    catch(const std::exception& e) {
      log(std::string("video base: store unavailable: ")+e.what());
      storeLock.reset();
      }
    try {
      const bool present=videoMatches(stored,identity,cancelled.get(),rejection);
      checkVideoCancelled(cancelled.get());
      if(present) {
        copyVideo(stored,destination,identity,cancelled.get());
        try {
          Lock cleanup(store/".lock",Lock::Exclusive,cancelled.get());
          prune(store,stored,cancelled.get());
          }
        catch(const std::exception& e) { log(std::string("video base store cleanup: ")+e.what()); }
        return finish("store",false);
        }
      if(fs::exists(stored)) log("video base: rejected store directory: "+rejection);
      }
    catch(const std::exception& e) {
      log(std::string("video base: store unavailable: ")+e.what());
      storeLock.reset();
      }
    }
  checkVideoCancelled(cancelled.get());
  if(!import) throw std::runtime_error("no ready video base for "+gothic.string()+"; run content-convert resolve <game directory>");
  Temporary temporary(destination,cancelled.get());
  log("video base: importing installation");
  import(temporary.path);
  storeLock.reset();
  storedLease.reset();
  publishVideo(temporary.path,destination,identity,cancelled.get());
  return finish("import");
  }
