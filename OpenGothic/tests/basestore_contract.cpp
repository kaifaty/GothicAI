#include "baseimport.h"
#include "basestoreimport.h"
#include "commandline.h"
#include "programdata.h"
#include "outputunits.h"
#include "resources.h"
#include <zenkit/Archive.hh>
#include <zenkit/CutsceneLibrary.hh>
#include <zenkit/Stream.hh>
#include <zenkit/Vfs.hh>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <chrono>
#include <cerrno>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <csignal>
#include <stdexcept>
#include <sys/wait.h>
#include <sys/file.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {
void require(bool ok, const std::string& message) {
  if(!ok) throw std::runtime_error(message);
  }

std::string read(const fs::path& file) {
  std::ifstream in(file,std::ios::binary);
  require(bool(in),"cannot read "+file.string());
  return {std::istreambuf_iterator<char>(in),std::istreambuf_iterator<char>()};
  }

std::map<std::string,std::string> hashes(const fs::path& root) {
  std::map<std::string,std::string> result;
  for(const auto& entry:fs::recursive_directory_iterator(root))
    if(entry.is_regular_file())
      result.emplace(entry.path().lexically_relative(root).generic_string(),BaseImport::sha256(read(entry.path())));
  return result;
  }

void equal(const fs::path& a, const fs::path& b) {
  const auto expected=hashes(a), actual=hashes(b);
  size_t mismatches=0;
  for(const auto& [path,hash]:expected) {
    const auto found=actual.find(path);
    if(found==actual.end() || found->second!=hash) {
      std::cerr << "SHA-256 mismatch: " << path << '\n';
      ++mismatches;
      }
    }
  for(const auto& [path,hash]:actual)
    if(!expected.count(path)) { std::cerr << "extra file: " << path << '\n'; ++mismatches; }
  require(mismatches==0,"base file SHA-256 mismatches: "+std::to_string(mismatches));
  std::cout << "equal SHA-256: " << expected.size() << " files, 0 mismatches\n";
  }

struct Temporary {
  fs::path path;
  Temporary() {
    const auto* cache=std::getenv("OPENGOTHIC_BASE_CACHE");
    const auto parent=cache && *cache ? fs::absolute(cache).parent_path() : fs::temp_directory_path();
    fs::create_directories(parent);
    path=fs::weakly_canonical(parent)/("gothic-base-store-"+std::to_string(getpid()));
    require(fs::create_directory(path),"temporary directory exists");
    }
  ~Temporary() { std::error_code error; fs::remove_all(path,error); }
  };

void write(const fs::path& file, const std::string& bytes) {
  fs::create_directories(file.parent_path());
  std::ofstream out(file,std::ios::binary|std::ios::trunc);
  out.write(bytes.data(),std::streamsize(bytes.size()));
  require(bool(out),"cannot write "+file.string());
  }

// A VDF written by ZenKit itself, with the header timestamp the engine sorts by.
void writeVdf(const fs::path& file, const std::vector<std::pair<std::string,std::string>>& files, time_t time) {
  zenkit::Vfs vfs;
  for(const auto& [path,bytes]:files) {
    const auto parent=fs::path(path).parent_path().generic_string();
    vfs.mkdir(parent);
    vfs.mount(zenkit::VfsNode::file(fs::path(path).filename().string(),
                                    {reinterpret_cast<const std::byte*>(bytes.data()),bytes.size(),false}),parent);
    }
  fs::create_directories(file.parent_path());
  auto disk=zenkit::Write::to(file);
  vfs.save(disk.get(),zenkit::GameVersion::GOTHIC_2,time);
  }

// The cache manifest and source snapshot can be exercised without reimporting the corpus.
void writeBaseFixture(const fs::path& directory, const BaseImport::Identity& identity) {
  write(directory/"sources/sources.json","{\"format\":\"gothic-sources\",\"version\":1}\n");
  write(directory/"fixture.json","{\"fixture\":true}\n");
  write(directory/"payload.bin","fixture payload");
  BaseImport::writeBaseMetadata(directory,identity);
  }

fs::path pointerFile(const fs::path& cache, const fs::path& game) {
  return cache/"current"/(BaseImport::sha256(fs::weakly_canonical(game).generic_string())+".json");
  }

void checkPointer(const fs::path& cache, const fs::path& game, const fs::path& base) {
  const auto bytes=read(pointerFile(cache,game));
  rapidjson::Document pointer;
  pointer.Parse(bytes.c_str());
  require(!pointer.HasParseError() && pointer.IsObject() && pointer.HasMember("base") &&
          pointer["base"].IsObject() && pointer["base"].HasMember("path") &&
          pointer["base"]["path"].IsString() && pointer["base"]["path"].GetString()==base.string(),
          "current base pointer must remain complete JSON with the selected path");
  }

void fingerprintSelection(const fs::path& root) {
  const auto game=root/"game", cache=root/"cache", store=root/"store";
  fs::create_directories(game);
  const auto source=root/"source";
  writeVdf(source/"Data/FIXTURE.VDF",{{"FIXTURE.TXT","fingerprint source"}},1000000000);
  const auto identity=BaseImport::identify(source);
  const auto base=cache/identity.fingerprint;
  writeBaseFixture(base,identity);
  setenv("OPENGOTHIC_BASE_CACHE",cache.c_str(),1);
  setenv("OPENGOTHIC_BASE_STORE",store.c_str(),1);
  setenv("OPENGOTHIC_BASE_FINGERPRINT",identity.fingerprint.c_str(),1);
  unsetenv("OPENGOTHIC_BASE");
  size_t identifies=0;
  const auto observe=[&](std::string_view message) {
    if(message=="base: identifying installation" || message=="base: importing installation") ++identifies;
    };
  require(BaseStore::resolve(game,{},observe).path==base && identifies==0,
          "fingerprint must select cache without an installation or identification");
  checkPointer(cache,game,base);
  fs::create_directories(store/"base");
  fs::rename(base,store/"base"/identity.fingerprint);
  require(BaseStore::resolve(game,{},observe).path==base && identifies==0,
          "fingerprint must copy store into cache without an installation");
  equal(store/"base"/identity.fingerprint,base);
  checkPointer(cache,game,base);
  fs::remove_all(store);
  const auto manifest=read(base/"base-files.json");
  write(base/"base-files.json",manifest+" ");
  std::string reason;
  try { (void)BaseStore::resolve(game,{},observe); }
  catch(const std::exception& error) { reason=error.what(); }
  require(reason.find("base-files.json")!=std::string::npos && identifies==0,
          "fingerprint selection must reject a corrupt manifest without importing: "+reason);
  fs::remove_all(base);
  reason.clear();
  try { (void)BaseStore::resolve(game,{},observe); }
  catch(const std::exception& error) { reason=error.what(); }
  require(reason.find(identity.fingerprint)!=std::string::npos && identifies==0,
          "missing fingerprint must fail clearly without importing: "+reason);
  writeBaseFixture(base,identity);
  const std::string absent(64,'0');
  setenv("OPENGOTHIC_BASE_FINGERPRINT",absent.c_str(),1);
  setenv("OPENGOTHIC_BASE",base.c_str(),1);
  require(BaseStore::resolve(game,{},observe).path==base,"explicit base must take priority over fingerprint");
  checkPointer(cache,game,base);
  // storygraph passes the base itself as -g; this must not create current or take its lock.
  fs::remove_all(cache/"current");
  require(BaseStore::resolve(base,{},observe).path==base && !fs::exists(cache/"current"),
          "explicit base used as game directory must not create a pointer or current lock");
  unsetenv("OPENGOTHIC_BASE_FINGERPRINT");
  unsetenv("OPENGOTHIC_BASE");
  std::cout << "fingerprint selection: cache/store, pointer, missing/corrupt refusal, explicit precedence; direct base has no current pointer/lock\n";
  }

void snapshotSelection(const fs::path& root) {
  const auto game=root/"game", cache=root/"cache";
  fs::create_directories(game);
  writeVdf(game/"Data/FIXTURE.VDF",{{"FIXTURE.TXT","fixture source"}},1000000000);
  write(game/"_work/Data/Scripts/_compiled/FIXTURE.DAT","loose fixture");
  setenv("OPENGOTHIC_BASE_CACHE",cache.c_str(),1);
  setenv("OPENGOTHIC_BASE_STORE","",1);
  unsetenv("OPENGOTHIC_BASE");
  auto mounted=BaseImport::Files{{"FIXTURE.VDF",fs::file_size(game/"Data/FIXTURE.VDF")}};
  auto identity=BaseImport::identify(game,mounted);
  auto base=cache/identity.fingerprint;
  size_t preparations=0;
  std::string missingReason;
  try {
    (void)BaseStore::resolve(game,mounted,[&](std::string_view message) {
      if(message=="base: identifying installation" || message=="base: importing installation") ++preparations;
      });
    }
  catch(const std::exception& error) { missingReason=error.what(); }
  require(missingReason.find("content-convert resolve")!=std::string::npos && preparations==0 && !fs::exists(base),
          "ready-only selection with empty roots must fail before hashing or importing the installation");
  writeBaseFixture(base,identity);
  size_t identifies=0;
  const auto observe=[&](std::string_view message) {
    if(message=="base: identifying installation") ++identifies;
    };
  require(BaseStore::resolve(game,mounted,observe).path==base && identifies>0,
          "first selection must identify the installation before writing its pointer");
  checkPointer(cache,game,base);
  auto pointer=read(pointerFile(cache,game));
  identifies=0;
  require(BaseStore::resolve(game,mounted,observe).path==base && identifies==0,
          "matching installation snapshot must skip full source identification");
  setenv("OPENGOTHIC_BASE",base.c_str(),1);
  require(BaseStore::resolve(game,mounted,observe).path==base && identifies==0,
          "explicit base with a matching snapshot must skip full source identification");
  {
    const int lock=::open((cache/".lock").c_str(),O_RDWR);
    require(lock>=0 && flock(lock,LOCK_EX)==0,"cannot hold cache import lock");
    int result[2]; require(pipe(result)==0,"explicit selection pipe failed");
    std::cout.flush(); std::cerr.flush();
    const auto child=fork(); require(child>=0,"explicit selection fork failed");
    if(child==0) {
      close(lock); close(result[0]);
      alarm(10);
      try {
        const auto selected=BaseStore::resolve(game,mounted,observe);
        if(selected.path!=base || identifies!=0 || ::write(result[1],"x",1)!=1) _exit(1);
        _exit(0);
        }
      catch(...) { _exit(1); }
      }
    close(result[1]);
    pollfd ready{result[0],POLLIN,0};
    char token=0;
    const bool completed=poll(&ready,1,5000)>0 && ::read(result[0],&token,1)==1 && token=='x';
    flock(lock,LOCK_UN); close(lock); close(result[0]);
    int status=0;
    require(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0 && completed,
            "explicit base selection must complete while another process holds the cache import lock");
    std::cout << "explicit base: completed while cache import lock remained held\n";
  }
  // An in-place rebuild can leave a current base behind an old importer's pointer.
  {
    rapidjson::Document stalePointer, staleIdentity;
    stalePointer.Parse(read(pointerFile(cache,game)).c_str());
    staleIdentity.Parse(stalePointer["base"]["identity"].GetString());
    const std::string oldFormat(64,'0');
    staleIdentity["format_sha256"].SetString(oldFormat.c_str(),64,staleIdentity.GetAllocator());
    staleIdentity.RemoveMember("fingerprint");
    rapidjson::StringBuffer source;
    rapidjson::Writer<rapidjson::StringBuffer> sourceWriter(source);
    staleIdentity.Accept(sourceWriter);
    const auto oldFingerprint=BaseImport::sha256({source.GetString(),source.GetSize()});
    staleIdentity.AddMember("fingerprint",rapidjson::Value(oldFingerprint.c_str(),staleIdentity.GetAllocator()),
                            staleIdentity.GetAllocator());
    rapidjson::StringBuffer identityBytes;
    rapidjson::Writer<rapidjson::StringBuffer> identityWriter(identityBytes);
    staleIdentity.Accept(identityWriter);
    stalePointer["base"]["fingerprint"].SetString(oldFingerprint.c_str(),stalePointer.GetAllocator());
    stalePointer["base"]["identity"].SetString(identityBytes.GetString(),rapidjson::SizeType(identityBytes.GetSize()),stalePointer.GetAllocator());
    rapidjson::StringBuffer pointerBytes;
    rapidjson::Writer<rapidjson::StringBuffer> pointerWriter(pointerBytes);
    stalePointer.Accept(pointerWriter);
    write(pointerFile(cache,game),{pointerBytes.GetString(),pointerBytes.GetSize()});
    writeBaseFixture(base,identity);
    identifies=0;
    require(BaseStore::resolve(game,mounted,observe).path==base && identifies==1,
            "explicit rebuilt base behind an old importer pointer must identify exactly once");
    rapidjson::Document refreshed;
    refreshed.Parse(read(pointerFile(cache,game)).c_str());
    require(refreshed["base"]["fingerprint"].GetString()==identity.fingerprint &&
            refreshed["base"]["identity"].GetString()==identity.json,
            "rebuilt base selection must refresh the pointer's importer identity");
    identifies=0;
    require(BaseStore::resolve(game,mounted,observe).path==base && identifies==0,
            "refreshed importer pointer must skip source identification on the next selection");
  }
  unsetenv("OPENGOTHIC_BASE");
  const auto archive=game/"Data/FIXTURE.VDF";
  fs::last_write_time(archive,fs::last_write_time(archive)+std::chrono::seconds(1));
  require(BaseStore::resolve(game,mounted,observe).path==base && identifies>0,
          "VDF mtime change must take the full identification path");
  require(read(pointerFile(cache,game))!=pointer,"mtime change must refresh the pointer snapshot");
  pointer=read(pointerFile(cache,game));
  { std::ofstream append(archive,std::ios::binary|std::ios::app); append.put('x'); }
  mounted.front().second=fs::file_size(archive);
  const auto previousBase=base;
  identity=BaseImport::identify(game,mounted);
  base=cache/identity.fingerprint;
  writeBaseFixture(base,identity);
  identifies=0;
  require(BaseStore::resolve(game,mounted,observe).path==base && identifies>0,
          "VDF size change must take the full path and select the new fingerprint");
  require(read(pointerFile(cache,game))!=pointer,"size change must write a new pointer");
  checkPointer(cache,game,base);
  fs::rename(game/"Data",game/"saved-Data");
  identifies=0;
  require(BaseStore::resolve(game,{},observe).path==base && identifies==0,
          "installation-free selection must use the pointer without source identification");
  setenv("OPENGOTHIC_BASE",previousBase.c_str(),1);
  require(previousBase!=base && BaseStore::resolve(game,{},observe).path==previousBase && identifies==0,
          "explicit offline base must use its own identity despite a pointer to a different base");
  checkPointer(cache,game,previousBase);
  const auto otherGame=root/"other-game";
  fs::create_directory(otherGame);
  setenv("OPENGOTHIC_BASE",base.c_str(),1);
  require(BaseStore::resolve(otherGame,{},observe).path==base && identifies==0,
          "explicit offline base must ignore another game directory's pointer in the same cache");
  checkPointer(cache,otherGame,base);
  checkPointer(cache,game,previousBase);
  require(BaseStore::resolve(game,{},observe).path==base && identifies==0,
          "explicit offline selection must replace the old pointer without source identification");
  unsetenv("OPENGOTHIC_BASE");
  checkPointer(cache,game,base);
  fs::remove_all(base);
  const auto missing=[&](const fs::path& directory) {
    std::string reason;
    try { (void)BaseStore::resolve(directory,{},observe); }
    catch(const std::exception& error) { reason=error.what(); }
    require(reason.find(directory.string())!=std::string::npos && reason.find("content-convert resolve")!=std::string::npos,
            "missing ready base must explain the game directory and conversion command: "+reason);
    };
  missing(game);
  const auto empty=root/"no-pointer";
  fs::create_directory(empty);
  missing(empty);
  // Restoring the installation allows ordinary cache resolution after pruning the pointer target.
  fs::rename(game/"saved-Data",game/"Data");
  writeBaseFixture(base,identity);
  require(BaseStore::resolve(game,mounted,observe).path==base,"restored installation must recover a deleted pointer target");
  int gate[2]; require(pipe(gate)==0,"snapshot gate failed");
  std::cout.flush(); std::cerr.flush();
  pid_t children[2];
  for(auto& child:children) {
    child=fork(); require(child>=0,"snapshot fork failed");
    if(child==0) {
      close(gate[1]); char token=0;
      if(::read(gate[0],&token,1)!=1) _exit(2);
      close(gate[0]);
      try { (void)BaseStore::resolve(game,mounted,observe); _exit(0); }
      catch(...) { _exit(1); }
      }
    }
  close(gate[0]); require(::write(gate[1],"xx",2)==2,"snapshot gate write failed"); close(gate[1]);
  for(const auto child:children) {
    int status=0;
    require(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0,
            "concurrent pointer writer failed");
    }
  checkPointer(cache,game,base);
  {
    const auto alias=root/"cache-link", store=root/"alias-store";
    fs::create_directory_symlink(fs::absolute(cache),alias);
    std::vector<fs::path> old;
    for(int i=0;i<3;++i) {
      old.push_back(cache/std::string(64,char('0'+i)));
      fs::create_directory(old.back());
      fs::last_write_time(old.back(),fs::file_time_type::clock::now()-std::chrono::hours(i+1));
      }
    setenv("OPENGOTHIC_BASE_CACHE",alias.c_str(),1);
    setenv("OPENGOTHIC_BASE_STORE",store.c_str(),1);
    identifies=0;
    std::string logs;
    const auto aliasLog=[&](std::string_view message) { observe(message); logs+=std::string(message)+"\n"; };
    {
      auto selected=BaseStore::resolve(game,mounted,aliasLog);
      require(selected.path==fs::weakly_canonical(base) && selected.lease && selected.publication.valid(),
              "symlinked cache selection must retain its lease and background publication");
      selected.publication.get();
      }
    require(!fs::exists(old.back()) && BaseImport::matches(store/"base"/identity.fingerprint,identity,true),
            "symlinked cache selection must prune old bases and publish the selected base");
    setenv("OPENGOTHIC_BASE_STORE","",1);
    logs.clear();
    auto selected=BaseStore::resolve(game,mounted,aliasLog);
    require(selected.path==fs::weakly_canonical(base) && selected.lease && identifies==0 &&
            logs.find("base: cache ")!=std::string::npos,
            "repeat selection through a cache symlink must remain a leased cache hit");
    std::cout.flush(); std::cerr.flush();
    const auto child=fork(); require(child>=0,"symlink lease fork failed");
    if(child==0) {
      selected.lease.reset();
      const int lock=::open((cache/(identity.fingerprint+".use")).c_str(),O_RDWR);
      if(lock<0) _exit(1);
      const bool busy=flock(lock,LOCK_EX|LOCK_NB)!=0 && (errno==EWOULDBLOCK || errno==EAGAIN);
      close(lock);
      _exit(busy ? 0 : 1);
      }
    int status=0;
    require(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0,
            "another process must not exclusively lock a base selected through a cache symlink");
    for(const auto& directory:old) fs::remove_all(directory);
    setenv("OPENGOTHIC_BASE_CACHE",cache.c_str(),1);
    std::cout << "symlink cache: repeated cache hit, lease exclusion, prune and publication verified\n";
  }
  std::cout << "snapshot selection: zero source identifies on repeat/explicit/offline; foreign offline pointers; VDF mtime/size and importer refresh; missing/pruned pointer; two atomic writers verified\n";
  }


void offlineResources(const fs::path& source, const fs::path& reference, const fs::path& root,
                      const BaseImport::Files& mounted) {
  std::cout.flush(); std::cerr.flush();
  const auto child=fork(); require(child>=0,"offline Resources fork failed");
  if(child==0) {
    try {
      const auto game=root/"game", cache=root/"cache";
      fs::create_directories(game);
      std::vector<fs::path> installationDirectories;
      for(const auto& directory:fs::directory_iterator(source)) {
        auto name=directory.path().filename().string();
        for(auto& c:name) c=char(std::toupper(static_cast<unsigned char>(c)));
        if(name!="DATA" && name!="_WORK") continue;
        const auto targetDirectory=game/directory.path().filename();
        // Preserve Data's directory order: tied VDF timestamps use it in the identity.
        fs::create_directory_symlink(fs::absolute(directory.path()),targetDirectory);
        installationDirectories.push_back(targetDirectory);
        }
      setenv("OPENGOTHIC_BASE_CACHE",cache.c_str(),1);
      setenv("OPENGOTHIC_BASE_STORE","",1);
      setenv("OPENGOTHIC_BASE",reference.c_str(),1);
      require(BaseStore::resolve(game,mounted,[](std::string_view) {}).path==reference,
              "offline Resources fixture did not write its pointer");
      const auto path=game.string();
      const char* arguments[]={"gothic-base-store-contract","-g",path.c_str()};
      for(const auto& directory:installationDirectories) fs::remove(directory);
      CommandLine commandLine(3,arguments);
      unsetenv("OPENGOTHIC_BASE");
      {
        Resources resources;
        Resources::loadBase();
        require(Resources::worldBaseDir("NEWWORLD.ZEN")==reference/"worlds/NEWWORLD",
                "offline Resources did not reach the native base after all family checks");
      }
      _exit(0);
      }
    catch(const std::exception& error) { std::cerr << "offline Resources: " << error.what() << std::endl; _exit(1); }
    }
  int status=0;
  require(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0,
          "Resources::loadBase must complete without Data or _work using the pointer identity");
  std::cout << "offline Resources: loadBase completed every native family check using base.json identity\n";
  }

std::string le32(uint32_t v) {
  std::string s(4,'\0');
  for(size_t i=0;i<4;++i) s[i]=char((v>>(8*i))&0xFF);
  return s;
  }

std::string chunk(const char* id, const std::string& data) {
  auto s=std::string(id,4)+le32(uint32_t(data.size()))+data;
  if(data.size()%2) s.push_back('\0');
  return s;
  }

std::string riff(const char* id, const char* type, const std::string& body) {
  return chunk(id,std::string(type,4)+body);
  }

std::string utf16z(const std::string& ascii) {
  std::string s;
  for(const char c:ascii) { s.push_back(c); s.push_back('\0'); }
  return s+std::string(2,'\0');
  }

std::string tinyDls(char guid) {
  return riff("RIFF","DLS ",chunk("dlid",std::string(16,guid))+chunk("colh",le32(1))+
              riff("LIST","lins",riff("LIST","ins ",chunk("insh",le32(0)+le32(0)+le32(0)))));
  }

// A band instrument with a valid patch and a DLS reference.
std::string instrument(char guid, const std::string& file) {
  const auto bins=le32(0)+le32(0)+std::string(16,'\0')+le32(0)+le32(1)+std::string(4,'\0')+le32(0);
  return riff("LIST","lbin",chunk("bins",bins)+
              riff("LIST","DMRF",chunk("guid",std::string(16,guid))+chunk("file",utf16z(file))));
  }

std::string cutsceneArchive(bool library, bool populated=false) {
  std::vector<std::byte> bytes;
  {
  auto stream=zenkit::Write::to(&bytes);
  auto archive=populated ? zenkit::WriteArchive::to_save(stream.get(),zenkit::ArchiveFormat::BINSAFE) :
                          zenkit::WriteArchive::to(stream.get(),zenkit::ArchiveFormat::ASCII);
  if(library) {
    auto lib=std::make_shared<zenkit::CutsceneLibrary>();
    if(populated) {
      auto nested=std::make_shared<zenkit::CutsceneBlock>();
      nested->name="NESTED";
      auto shared=std::get<0>(nested->block)->message;
      shared->type=0xfedcba98;
      shared->name="AUDIO_7.WAV";
      shared->text=std::string(" \x98\xa0raw\0tail ",12);
      shared->high_priority=true; shared->used=true; shared->deleted=true;
      for(const char* name:{"Z_BLOCK","A_BLOCK"}) {
        auto root=std::make_shared<zenkit::CutsceneBlock>();
        root->name=name; root->block=nested;
        lib->blocks.push_back(root);
        }
      for(const char* text:{"first duplicate","last duplicate"}) {
        auto root=std::make_shared<zenkit::CutsceneBlock>();
        root->name="DUP_BLOCK";
        root->set_message(std::make_shared<zenkit::ConversationMessageEvent>());
        root->get_message()->name="DUP_AUDIO_9.WAV";
        root->get_message()->text=text;
        lib->blocks.push_back(root);
        }
      auto empty=std::make_shared<zenkit::CutsceneBlock>();
      empty->name="EMPTY_BLOCK"; empty->set_message(nullptr);
      lib->blocks.push_back(empty);
      }
    archive->write_object(lib,zenkit::GameVersion::GOTHIC_2);
    }
  else        archive->write_object(std::make_shared<zenkit::CutsceneBlock>(),zenkit::GameVersion::GOTHIC_2);
  archive->write_header();
  }
  return std::string(reinterpret_cast<const char*>(bytes.data()),bytes.size());
  }

bool nodePath(const zenkit::VfsNode& parent, const zenkit::VfsNode* node, std::string& path) {
  if(&parent==node) return true;
  if(parent.type()!=zenkit::VfsNodeType::DIRECTORY) return false;
  for(const auto& child:parent.children()) {
    const auto size=path.size();
    path+=(path.empty() ? "" : "/")+child.name();
    if(nodePath(child,node,path)) return true;
    path.resize(size);
    }
  return false;
  }

rapidjson::Document manifest(const fs::path& game, const fs::path& out) {
  require(BaseImport::convertSources(game,out,false)==0,"synthetic source manifest failed");
  rapidjson::Document doc;
  const auto text=read(out/"sources.json");
  doc.Parse(text.c_str(),text.size());
  require(!doc.HasParseError() && doc.IsObject(),"synthetic sources.json is not JSON");
  return doc;
  }

// P10 regressions on a small installation: DFS basename winners with a shadowed loose copy,
// OU parse semantics, the GothicKit music trace, and content/order binding of the identity.
void syntheticSources(const fs::path& game) {
  const std::string sfx="RIFF-sfx", speech="RIFF-speech-copy", loose="RIFF-loose-sfx";
  const std::string dat("\x32\0\0\0\0\0\0\0\0",9); // version, no symbols, no code
  const auto archiveDat=std::string(1,'\x33')+le32(0)+le32(1)+std::string(1,'\x3c');
  std::string archiveOu;
  const auto archives=[&](time_t a, time_t b) {
    writeVdf(game/"Data/A.VDF",{{"/_WORK/DATA/SOUND/SFX/SVM_X.WAV",sfx},{"/_WORK/DATA/SOUND/SPEECH/SVM_X.WAV",speech},
                              {"/_WORK/DATA/SCRIPTS/_COMPILED/GOTHIC.DAT",archiveDat},{"/_WORK/DATA/SCRIPTS/_COMPILED/BAD.DAT","not a program"}},a);
    std::vector<std::pair<std::string,std::string>> extra{{"/_WORK/DATA/SOUND/SFX/OTHER.WAV",sfx}};
    if(!archiveOu.empty()) {
      extra.emplace_back("/OU.BIN",archiveOu);
      extra.emplace_back("/BROKEN.BIN","ZenGin Archive\nbroken body");
      extra.emplace_back("/NONZEN.BIN","not a zengin archive");
      }
    writeVdf(game/"Data/B.VDF",extra,b);
    };
  archives(1000000000,1100000000);
  write(game/"_work/Data/Sound/SFX/SVM_X.WAV",loose);
  for(const char* name:{"CAMERA","FIGHT","SFX","MUSIC","PARTICLEFX","VISUALFX","MENU","GOTHIC"})
    write(game/"_work/Data/Scripts/_compiled"/(std::string(name)+".DAT"),dat);
  const auto ou=game/"_work/Data/Scripts/CONTENT/CUTSCENE/OU.BIN";
  write(ou,cutsceneArchive(true));
  const auto band=riff("RIFF","DMBD",chunk("guid",std::string(16,'b'))+riff("LIST","lbil",
    instrument('m',"missing.dls")+instrument('g',"Real.DLS")+instrument('g',"alias.dls")));
  const auto segment=riff("RIFF","DMSG",chunk("segh",std::string(24,'\0'))+riff("LIST","trkl",
    riff("RIFF","DMTK",chunk("trkh",std::string(32,'\0'))+riff("RIFF","DMBT",riff("LIST","lbdl",
      riff("LIST","lbnd",chunk("bdih",le32(0))+band))))));
  write(game/"_work/Data/Music/newworld/TEST.sgt",segment);
  write(game/"_work/Data/Music/newworld/real.dls",tinyDls('g'));
  // A style track (both providers) whose style is spelled differently from the file on disk;
  // one of its DLS lies outside the Dx8 search directories but inside the VFS.
  write(game/"_work/Data/Music/newworld/STYLED.sgt",riff("RIFF","DMSG",chunk("segh",std::string(24,'\0'))+
    riff("LIST","trkl",riff("RIFF","DMTK",chunk("trkh",std::string(32,'\0'))+riff("LIST","sttr",
      riff("LIST","strf",chunk("stmp",le32(0))+
        riff("LIST","DMRF",chunk("guid",std::string(16,'s'))+chunk("file",utf16z("Style.STY")))))))));
  write(game/"_work/Data/Music/newworld/style.sty",riff("RIFF","DMST",chunk("guid",std::string(16,'s'))+
    riff("RIFF","DMBD",chunk("guid",std::string(16,'c'))+
      riff("LIST","lbil",instrument('g',"real.dls")+instrument('x',"extra.dls")))));
  write(game/"_work/Data/Sound/extra.dls",tinyDls('x'));
  // A missing style before an existing one: the download stops there, so the root is incomplete.
  const auto styleRef=[&](const std::string& file, char guid) {
    return riff("LIST","strf",chunk("stmp",le32(0))+riff("LIST","DMRF",chunk("guid",std::string(16,guid))+chunk("file",utf16z(file))));
    };
  write(game/"_work/Data/Music/newworld/EARLY.sgt",riff("RIFF","DMSG",chunk("segh",std::string(24,'\0'))+
    riff("LIST","trkl",riff("RIFF","DMTK",chunk("trkh",std::string(32,'\0'))+
      riff("LIST","sttr",styleRef("missing.sty",'n')+styleRef("Style.STY",'s'))))));

  const auto doc=manifest(game,game.parent_path()/"synthetic-sources");
  // The same installation spelled through "..": identical manifest, no absolute or ".." paths.
  {
  fs::create_directories(game.parent_path()/"spelling");
  const auto spelled=game.parent_path()/"spelling"/".."/game.filename();
  const auto other=manifest(spelled,game.parent_path()/"synthetic-spelled");
  const auto text=read(game.parent_path()/"synthetic-spelled/sources.json");
  require(other==doc && text.find(game.parent_path().string())==std::string::npos && text.find("../")==std::string::npos,
          "an equivalent installation spelling changed the manifest or leaked its root");
  }
  // The winner is whatever the engine's Vfs::find returns, with the engine's mount order.
  zenkit::Vfs oracle;
  for(const auto& a:BaseImport::runtimeArchives(game))
    oracle.mount_disk(a.path,zenkit::VfsOverwriteBehavior::OLDER);
  oracle.mkdir("/_work");
  oracle.mount_host(fs::absolute(game/"_work"),"/_work",zenkit::VfsOverwriteBehavior::NONE);
  std::string expected;
  require(nodePath(oracle.root(),oracle.find("SVM_X.WAV"),expected),"oracle path");
  bool checkedWav=false;
  for(const auto& f:doc["files"].GetArray()) {
    if(std::string(f["request"].GetString())!="SVM_X.WAV") continue;
    checkedWav=true;
    require(std::string(f["path"].GetString())==expected,"WAV winner differs from Vfs::find");
    require(f["candidates"].Size()==2,"both SFX and SPEECH paths must be candidates");
    size_t selected=0;
    for(const auto& c:f["candidates"].GetArray()) {
      if(!c["selected"].GetBool()) continue;
      ++selected;
      if(std::string(c["path"].GetString()).find("SFX")!=std::string::npos) {
        require(c["copies"].Size()==2 && std::string(f["source"].GetString())=="A.VDF",
                "archive copy must win over the shadowed _work copy");
        }
      }
    require(selected==1,"exactly one WAV candidate is selected");
    }
  require(checkedWav,"SVM_X.WAV missing from the manifest");
  std::cout << "synthetic WAV: DFS winner " << expected << ", shadowed _work copy recorded\n";

  const auto lower=[](std::string v) {
    for(auto& c:v) c=char(std::tolower(static_cast<unsigned char>(c)));
    return v;
    };
  const auto root=[&](const char* provider, const char* name) -> const rapidjson::Value& {
    for(const auto& r:doc["music"][provider]["roots"].GetArray())
      if(std::string(r["root"].GetString())==name) return r;
    throw std::runtime_error(std::string(provider)+" root missing: "+name);
    };
  const auto& test=root("gothickit","TEST.sgt");
  require(test["loaded"].GetBool() && test["result"].GetInt()==0,"GothicKit root must load");
  std::vector<std::string> requests;
  for(const auto& a:test["attempts"].GetArray()) {
    requests.emplace_back(a["request"].GetString());
    if(requests.back()=="missing.dls") require(a["found"].IsNull(),"missing.dls must be a miss");
    if(requests.back()=="Real.DLS")
      require(std::string(a["found"]["path"].GetString()).ends_with("real.dls"),"exact request Real.DLS must open real.dls");
    }
  require((requests==std::vector<std::string>{"TEST.sgt","missing.dls","Real.DLS"}),
          "GothicKit attempts must keep order and exact spelling, without a request for the cached alias");
  const auto& refs=test["references"];
  require(refs.Size()==3 && refs[0u]["object"].IsNull() && refs[1u]["object"].IsObject() &&
          refs[2u]["object"].IsObject() && refs[2u]["object"]["id"]==refs[1u]["object"]["id"] &&
          refs[2u].HasMember("cache_alias") && !refs[2u]["name_requested"].GetBool(),
          "missing early DLS, later success and the GUID-cache alias must all be recorded");
  std::cout << "synthetic music: embedded band->DLS, early miss then success, GUID alias recorded\n";

  // GothicKit follows the style and finds extra.dls anywhere in the VFS.
  requests.clear();
  for(const auto& a:root("gothickit","STYLED.sgt")["attempts"].GetArray()) {
    requests.emplace_back(a["request"].GetString());
    require(!a["found"].IsNull(),requests.back()+": GothicKit must resolve it through the VFS");
    }
  require((requests==std::vector<std::string>{"STYLED.sgt","Style.STY","real.dls","extra.dls"}),
          "GothicKit style-track attempts differ");
  // Dx8 ignores segment bands, keeps the exact style spelling and only searches the music directories.
  const auto& dx8Test=root("dx8","TEST.sgt");
  require(dx8Test["events"].Size()==1 && dx8Test["engine_failure"].IsNull(),"Dx8 must not follow segment bands");
  const auto& events=root("dx8","STYLED.sgt")["events"];
  require(events.Size()==4 &&
          std::string(events[1u]["request"].GetString())=="Style.STY" &&
          lower(events[1u]["found"]["path"].GetString()).ends_with("newworld/style.sty") &&
          std::string(events[2u]["request"].GetString())=="real.dls" && events[2u].HasMember("found") &&
          std::string(events[3u]["request"].GetString())=="extra.dls" && events[3u].HasMember("error") &&
          root("dx8","STYLED.sgt")["engine_failure"].GetUint()==3,
          "Dx8 style->band->DLS order, exact spelling and the unresolved DLS must be recorded");
  std::cout << "synthetic music: Dx8 and GothicKit provider differences recorded\n";
  require(root("gothickit","TEST.sgt")["dependency_closure"].GetBool() &&
          root("gothickit","STYLED.sgt")["dependency_closure"].GetBool() &&
          root("dx8","TEST.sgt")["dependency_closure"].GetBool() &&
          root("dx8","STYLED.sgt")["dependency_closure"].GetBool(),"complete roots must report closure");
  require(root("gothickit","EARLY.sgt")["result"].GetInt()!=0 &&
          !root("gothickit","EARLY.sgt")["dependency_closure"].GetBool() &&
          !doc["music"]["gothickit"]["dependency_closure"].GetBool() &&
          !root("dx8","EARLY.sgt")["dependency_closure"].GetBool() &&
          !doc["music"]["dx8"]["dependency_closure"].GetBool(),
          "a download stopped by a missing style must not report closure");
  std::cout << "synthetic music: early missing style reported as incomplete\n";

  // A VFS OU that does not load ends the game's dialog load; it must not be reported as selected.
  require(std::string(doc["output_units"]["selected"]["NONE"]["path"].GetString()).ends_with("OU.BIN"),"valid OU.BIN selected");
  // The game never reaches OU.DAT after a valid OU.BIN: a broken OU.DAT must not reject the import.
  {
  const auto unused=ou.parent_path()/"OU.DAT";
  write(unused,"garbage");
  const auto withDat=manifest(game,game.parent_path()/"synthetic-unused-dat");
  require(std::string(withDat["output_units"]["selected"]["NONE"]["path"].GetString()).ends_with("OU.BIN"),
          "valid OU.BIN must stay selected beside a broken, unreached OU.DAT");
  fs::remove(unused);
  }
  const auto valid=read(ou);
  auto badBody=valid;
  const auto field=badBody.find("NumOfItems=int:0");
  require(field!=std::string::npos,"ASCII cutscene library field not found");
  badBody.replace(field,16,"NumOfItems=string:x");
  for(const auto& [what,bytes]:std::vector<std::pair<std::string,std::string>>{
        {"wrong object type",cutsceneArchive(false)},{"bad body",badBody},{"bad header","garbage"}}) {
    write(ou,bytes);
    bool rejected=false;
    try { BaseImport::convertSources(game,game.parent_path()/"synthetic-rejected",false); }
    catch(const std::exception& e) { rejected=std::string(e.what()).find("OU.BIN")!=std::string::npos; }
    require(rejected,"OU.BIN with "+what+" must not be selected");
    }
  write(ou,valid);
  std::cout << "synthetic OU: wrong type, bad body and bad header rejected\n";

  // P14: the engine's archive winner and Python's physical host selection are distinct libraries.
  archiveOu=cutsceneArchive(true,true);
  archives(1000000000,1100000000);
  write(ou.parent_path()/"OU.DAT","unreached bad DAT");
  write(ou.parent_path()/"HOST_BAD.BIN","host skipped source");
  write(game/"_work/Data/Scripts_RU/LOCAL.DAT",cutsceneArchive(true));
  const auto output=game.parent_path()/"synthetic-outputunits";
  require(BaseImport::convertOutputUnits(game,output,false)==0,"synthetic output units must import");
  require(BaseImport::convertOutputUnits(game,output,true)==0,"synthetic output units must reproduce byte-for-byte");
  const auto units=OutputUnits::readIndex(output);
  std::string_view ouBranch;
  const auto* engineOu=units.findVfs("ou.bin \t",&ouBranch);
  const auto* pythonOu=units.serverOuPy();
  require(engineOu && engineOu->origin=="archive" && engineOu->archive=="B.VDF" &&
          engineOu->sha256==BaseImport::sha256(archiveOu) && ouBranch=="vfs_find_request",
          "engine OU must select the archive BIN with exact imported provenance");
  require(units.findVfs("missing\\OU.BIN \t",&ouBranch)==engineOu && ouBranch=="vfs_find_basename",
          "OU basename fallback must preserve its branch and source");
  require(pythonOu && pythonOu->origin=="host" && pythonOu->sha256==BaseImport::sha256(valid) &&
          units.findHost("_WORK/DATA/SCRIPTS/CONTENT/CUTSCENE/ou.bin")==pythonOu && pythonOu->record!=engineOu->record,
          "Python's exact-directory physical OU source must remain distinct from the engine winner");
  require(units.findHost("_work/Data/Scripts_RU/LOCAL.DAT") &&
          !units.findHost("_work/Data/Scripts_RU/LOCAL.DAT")->record.empty(),"localized direct-directory OU candidates must be indexed");
  for(const char* request:{"OU.DAT","BROKEN.BIN","NONZEN.BIN","GOTHIC.DAT"}) {
    const auto* bad=units.findVfs(request);
    require(bad && !bad->skipped.empty() && bad->record.empty(),std::string(request)+": skipped VFS source must remain present");
    }
  const auto* badHost=units.findHost("_work/Data/Scripts/CONTENT/CUTSCENE/HOST_BAD.BIN");
  require(badHost && !badHost->skipped.empty() && !units.findHost("_work/Data/Scripts/CONTENT/CUTSCENE/NOT_IMPORTED.BIN"),
          "an existing skipped host source must differ from a source absent from the index");
  require(units.selections.size()==9,"valid BIN must end every language selection before the unreached bad DAT");
  for(const auto& row:units.selections)
    require(row.candidate=="OU.BIN" && row.step=="vfs_find_request" && row.source &&
            &units.sources.at(*row.source)==engineOu,"BIN must win over DAT and host/localized sources for every language");
  std::string ouDigest;
  const auto library=OutputUnits::readLibrary(units.path(*engineOu),&ouDigest);
  require(ouDigest==engineOu->recordSha256 && ouDigest==BaseImport::sha256File(units.path(*engineOu)) &&
          library.blocks.size()==5 && library.blocks[0]->name=="Z_BLOCK" && library.blocks[1]->name=="A_BLOCK",
          "native OU must hash the actual decoded record and preserve archive root order");
  const auto nested=std::get<1>(library.blocks[0]->block);
  require(nested==std::get<1>(library.blocks[1]->block),"native OU must preserve shared nested references");
  const auto rawMessage=OutputUnits::message(*library.blocks[0]);
  require(rawMessage && rawMessage->type==0xfedcba98 && rawMessage->name=="AUDIO_7.WAV" &&
          rawMessage->text==std::string(" \x98\xa0raw\0tail ",12) && rawMessage->high_priority && rawMessage->used && rawMessage->deleted &&
          !OutputUnits::message(*library.blocks.back()),"native OU must preserve raw bytes, audio/block distinction, save flags and null messages");
  const auto ownMessages=OutputUnits::messagesInArchiveOrder(library);
  require(ownMessages.size()==3 && ownMessages[0]==rawMessage && ownMessages[1]->text=="first duplicate" &&
          ownMessages[2]->text=="last duplicate","native server messages must keep archive order and visit aliases once");
  const auto sortedOu=OutputUnits::sortedBlocks(library);
  require(OutputUnits::blockByName(sortedOu,"B_MISSING")->name=="DUP_BLOCK" &&
          OutputUnits::blockByName(sortedOu,"")->name=="A_BLOCK" && !OutputUnits::blockByName(sortedOu,"~AFTER"),
          "OU lookup must preserve absent-name successor behavior before, between and after roots");
  const auto rawLibraryBytes=std::vector<std::byte>(reinterpret_cast<const std::byte*>(archiveOu.data()),
                                                   reinterpret_cast<const std::byte*>(archiveOu.data())+archiveOu.size());
  auto legacyStream=zenkit::Read::from(&rawLibraryBytes);
  auto legacyArchive=zenkit::ReadArchive::from(legacyStream.get());
  const auto legacyLibrary=legacyArchive->read_object<zenkit::CutsceneLibrary>(zenkit::GameVersion::GOTHIC_2);
  require(legacyLibrary && legacyLibrary->blocks.size()==sortedOu.size(),"synthetic original OU sorted root count");
  for(size_t i=0;i<sortedOu.size();++i) {
    const auto a=legacyLibrary->blocks[i]->get_message();
    const auto b=OutputUnits::message(*sortedOu[i]);
    require(legacyLibrary->blocks[i]->name==sortedOu[i]->name && bool(a)==bool(b) && (!a || a->text==b->text),
            "synthetic duplicate roots must retain the original platform std::sort order");
    }
  const auto recordFile=units.path(*engineOu);
  const auto nativeBytes=read(recordFile);
  fs::remove(recordFile);
  bool ouRejected=false;
  try { (void)OutputUnits::readLibrary(recordFile); } catch(const std::runtime_error&) { ouRejected=true; }
  require(ouRejected,"missing native OU payload must fail without reading the intact original");
  write(recordFile,"{broken");
  ouRejected=false;
  try { (void)OutputUnits::readLibrary(recordFile); } catch(const std::runtime_error&) { ouRejected=true; }
  require(ouRejected,"corrupt native OU payload must fail without reading the intact original");
  write(recordFile,nativeBytes);
  const auto indexBytes=read(output/"outputunits.json");
  fs::remove(output/"outputunits.json");
  ouRejected=false;
  try { (void)OutputUnits::readIndex(output); } catch(const std::runtime_error&) { ouRejected=true; }
  require(ouRejected,"missing native OU index must fail");
  write(output/"outputunits.json",indexBytes);
  const auto emptyGame=game.parent_path()/"synthetic-empty-ou";
  fs::create_directories(emptyGame/"Data");
  const auto emptyOut=game.parent_path()/"synthetic-empty-outputunits";
  require(BaseImport::convertOutputUnits(emptyGame,emptyOut,false)==0,"installation without OU inputs must import");
  const auto emptyUnits=OutputUnits::readIndex(emptyOut);
  require(emptyUnits.sources.empty() && emptyUnits.records.empty() && emptyUnits.vfs.empty() && emptyUnits.host.empty() &&
          !emptyUnits.serverOuPy(),"installation without OU inputs must retain a valid empty index");
  // No BIN: the same selector must proceed to DAT; a present bad VFS BIN must stop it instead.
  fs::rename(ou,ou.parent_path()/"HOST_ONLY.BIN");
  write(ou.parent_path()/"OU.DAT",valid);
  archiveOu.clear();
  archives(1000000000,1100000000);
  const auto datOut=game.parent_path()/"synthetic-outputunits-dat";
  require(BaseImport::convertOutputUnits(game,datOut,false)==0,"DAT-only OU selection must import");
  const auto datUnits=OutputUnits::readIndex(datOut);
  size_t selectedDat=0;
  for(const auto& row:datUnits.selections) {
    if(!row.source) continue;
    ++selectedDat;
    require(row.candidate=="OU.DAT" && row.step=="vfs_find_request" &&
            datUnits.sources.at(*row.source).skipped.empty(),"a missing BIN must fall through to valid DAT");
    }
  require(selectedDat==9 && !datUnits.serverOuPy(),"DAT-only engine selection must not invent a Python OU.BIN source");
  const auto skippedOut=game.parent_path()/"synthetic-outputunits-skipped";
  for(const std::string badOu:{"not a zengin archive","ZenGin Archive\nbroken body"}) {
    archiveOu=badOu;
    archives(1000000000,1100000000);
    require(BaseImport::convertOutputUnits(game,skippedOut,false)==0,"selected malformed VFS source must remain explicit in the index");
    const auto skippedUnits=OutputUnits::readIndex(skippedOut);
    require(skippedUnits.selections.size()==9,"a present bad VFS BIN must stop before valid DAT");
    for(const auto& row:skippedUnits.selections)
      require(row.candidate=="OU.BIN" && row.source && !skippedUnits.sources.at(*row.source).skipped.empty(),
              "selected skipped VFS BIN must not become an absent row or fall through to DAT");
    }
  fs::rename(ou.parent_path()/"HOST_ONLY.BIN",ou);
  write(ou.parent_path()/"OU.DAT","unreached bad DAT");
  archiveOu=cutsceneArchive(true,true);
  archives(1000000000,1100000000);
  std::cout << "synthetic OU: archive/host/language selections, skipped-vs-absent rows, graph aliases, raw flags/text, native failures and empty index verified\n";

  const auto programs=game.parent_path()/"synthetic-programs";
  require(BaseImport::convertPrograms(game,programs,false)==0,"synthetic programs must import");
  require(BaseImport::convertPrograms(game,programs,true)==0,"synthetic program import must be reproducible");
  const auto index=ProgramData::readIndex(programs);
  std::string_view branch;
  const auto* engine=index.findVfs("gothic.dat \t",&branch);
  require(engine!=nullptr && branch=="vfs_find_request" && engine->origin=="archive" && engine->archive=="A.VDF" &&
          engine->path=="_WORK/DATA/SCRIPTS/_COMPILED/GOTHIC.DAT" && engine->sha256==BaseImport::sha256(archiveDat),
          "program index must retain the full archive path and actual selected provenance");
  require(index.findVfs("missing\\gothic.dat \t",&branch)==engine && branch=="vfs_find_basename",
          "program basename fallback must retain the selected source and branch");
  const auto* server=index.server();
  require(server!=nullptr && server->origin=="host" && server->archive.empty() &&
          server->path=="_work/Data/Scripts/_compiled/GOTHIC.DAT" && server->sha256==BaseImport::sha256(dat) &&
          server->program!=engine->program && index.findHost("_WORK/DATA/SCRIPTS/_COMPILED/gothic.dat")==server,
          "server physical GOTHIC.DAT must remain separate from the engine archive winner");
  std::string digest;
  const auto record=ProgramData::readProgram(index.path(*engine),&digest);
  auto script=zenkit::DaedalusScript::from_program(record);
  require(record.version==0x33 && record.symbols.empty() && record.bytecode.size()==1 &&
          uint8_t(record.bytecode[0])==0x3c && script.symbols().empty() && script.size()==1 &&
          script.instruction_at(0).op==zenkit::DaedalusOpcode::RSR && digest==engine->recordSha256,
          "native program must preserve the independent synthetic header and return opcode");
  bool rejected=false;
  try { (void)index.findVfs("BAD.DAT"); }
  catch(const std::runtime_error& error) { rejected=std::string(error.what()).find("not an importable program")!=std::string::npos; }
  require(rejected,"a selected nonprogram DAT must fail without falling through");
  auto binary=index.path(*engine); binary.replace_extension(".bin");
  const auto payload=read(binary);
  fs::remove(binary);
  rejected=false;
  try { (void)ProgramData::readProgram(index.path(*engine)); }
  catch(const std::runtime_error& error) { rejected=std::string(error.what()).find("program data:")!=std::string::npos; }
  require(rejected,"missing selected native program BIN must fail");
  write(binary,payload);
  std::cout << "synthetic programs: native roundtrip, archive/host provenance, basename selection and missing/nonprogram failures verified\n";

  const auto base=BaseImport::identify(game);
  auto music=read(game/"_work/Data/Music/newworld/TEST.sgt");
  music.back()=char(music.back()^1);
  write(game/"_work/Data/Music/newworld/TEST.sgt",music);
  require(BaseImport::identify(game).fingerprint!=base.fingerprint,"same-size loose SGT change kept the fingerprint");
  music.back()=char(music.back()^1);
  write(game/"_work/Data/Music/newworld/TEST.sgt",music);
  require(BaseImport::identify(game).json==base.json,"restored installation must restore the identity");
  // VDF stores DOS timestamps with a two-second resolution.
  archives(1000000000,1100000002);
  require(BaseImport::identify(game).fingerprint!=base.fingerprint,"VDF header timestamp change kept the fingerprint");
  archives(1100000000,1000000000);
  require(BaseImport::identify(game).fingerprint!=base.fingerprint,"VDF mount order change kept the fingerprint");
  std::cout << "synthetic identity: loose content, VDF timestamp and mount order change the fingerprint\n";
  }
}

int main() {
  try {
    const char* game=std::getenv("GOTHIC_DIR");
    const char* referenceEnv=std::getenv("OPENGOTHIC_BASE");
    require(game && *game && referenceEnv && *referenceEnv,"GOTHIC_DIR and OPENGOTHIC_BASE (build-base.sh output) required");
    const fs::path reference=referenceEnv;
    unsetenv("OPENGOTHIC_BASE_FINGERPRINT");
    unsetenv("OPENGOTHIC_VIDEOS_FINGERPRINT");
    const auto identity=BaseImport::identify(game);
    require(BaseImport::matches(reference,identity),"build-base.sh reference has a different base.json");
    require(BaseImport::sha256("abc")=="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
            "SHA-256 known vector");
    // P10: the manifest of selected remaining inputs is reproducible from the installation.
    require(BaseImport::convertSources(game,reference/"sources",true)==0,"reference source manifest differs");
    Temporary temporary;
    // Startup verifies the manifest and indices while payload SHA remains a publication/read check.
    {
    const auto copy=temporary.path/"manifest-binding";
    writeBaseFixture(copy,identity);
    std::string reason;
    require(BaseImport::matches(copy,identity,false,&reason),"fixture manifest must match: "+reason);
    const auto rejected=[&](const char* path) {
      reason.clear();
      require(!BaseImport::matches(copy,identity,false,&reason) && !reason.empty() && reason.find(path)!=std::string::npos,
              "damaged base must report its failed path: "+reason);
      };
    const auto index=read(copy/"fixture.json");
    write(copy/"fixture.json","{\"fixture\":fals}\n");
    rejected("fixture.json");
    write(copy/"fixture.json",index);
    const auto payload=read(copy/"payload.bin");
    fs::remove(copy/"payload.bin"); rejected("payload.bin");
    write(copy/"payload.bin",payload+"x"); rejected("payload.bin");
    write(copy/"payload.bin",payload);
    const auto manifest=read(copy/"base-files.json");
    write(copy/"base-files.json",manifest+" "); rejected("base-files.json");
    write(copy/"base-files.json",manifest);
    // Same-size payload damage is deferred at startup but checked for copies/publication.
    auto damaged=payload; damaged.back()=char(damaged.back()^1); write(copy/"payload.bin",damaged);
    require(BaseImport::matches(copy,identity,false,&reason),"startup unexpectedly hashed payload");
    require(!BaseImport::matches(copy,identity,true,&reason) && reason.find("payload.bin")!=std::string::npos,
            "full payload verification must report a same-size corruption");
    }
    std::cout << "base manifest: corrupt index, missing/resized payload and altered manifest rejected with reasons\n";
    fingerprintSelection(temporary.path/"fingerprints");
    snapshotSelection(temporary.path/"snapshots");
    syntheticSources(temporary.path/"synthetic-game");
    const auto cache=temporary.path/"cache", store=temporary.path/"store";
    const auto base=cache/identity.fingerprint;
    setenv("OPENGOTHIC_BASE_CACHE",cache.c_str(),1);
    setenv("OPENGOTHIC_BASE_STORE",store.c_str(),1);
    setenv("OPENGOTHIC_CONTENT","",1);
    setenv("OPENGOTHIC_TEXTURES","",1);
    unsetenv("OPENGOTHIC_BASE");
    // Obtain the engine's archive identity without opening any game resource.
    BaseImport::Files mounted;
    for(const auto& entry:fs::directory_iterator(fs::path(game)/"Data")) {
      auto name=entry.path().filename().string();
      for(auto& c:name) c=char(std::toupper(static_cast<unsigned char>(c)));
      if(entry.is_regular_file() && name.ends_with(".VDF")) mounted.emplace_back(name,entry.file_size());
      }
    require(BaseImport::identify(game,mounted).json==identity.json,"archive identity differs");
    offlineResources(game,reference,temporary.path/"offline-resources",mounted);
    const char* args[]={"gothic-base-store-contract","-g",game};
    CommandLine commandLine(3,args);
    zenkit::Logger::set(zenkit::LogLevel::ERROR,[](zenkit::LogLevel level, const char* component, const char* message) {
      Resources::zenkitLog(level,component,message);
      BaseImport::onZenkitLog(level,component,message);
      });
    // The real engine log callback must reject a class ZenKit otherwise skips.
    const auto unknownGame=temporary.path/"unknown-game";
    fs::create_directories(unknownGame/"Data");
    {
    std::vector<std::byte> bytes;
    auto stream=zenkit::Write::to(&bytes);
    auto archive=zenkit::WriteArchive::to(stream.get(),zenkit::ArchiveFormat::ASCII);
    archive->write_object_begin("world","oCWorld:zCWorld",0);
    archive->write_object_begin("VobTree","",0);
    archive->write_int("childs0",1);
    archive->write_object_begin("vob","zCUnknownImportVob:zCVob",0);
    archive->write_object_end();
    archive->write_int("childs1",0);
    archive->write_object_end();
    archive->write_object_begin("EndMarker","",0);
    archive->write_object_end();
    archive->write_object_end();
    archive->write_header();
    zenkit::Vfs vfs;
    vfs.mount(zenkit::VfsNode::file("NEWWORLD.ZEN",{bytes.data(),bytes.size(),false}),"/");
    auto disk=zenkit::Write::to(unknownGame/"Data/UNKNOWN.VDF");
    vfs.save(disk.get(),zenkit::GameVersion::GOTHIC_2);
    }
    const auto rejectUnknown=[&](auto run) {
      std::string failure;
      try { run(); }
      catch(const std::exception& e) { failure=e.what(); }
      require(failure.find("NEWWORLD.ZEN")!=std::string::npos &&
              failure.find("Unknown object type: zCUnknownImportVob:zCVob")!=std::string::npos,
              "import must reject unknown classes with file and class names: "+failure);
      };
    const auto unknownOut=temporary.path/"unknown-zen";
    rejectUnknown([&] { BaseImport::convertZen(unknownGame,unknownOut,false); });
    require(!fs::exists(unknownOut/"NEWWORLD.json") && !fs::exists(unknownOut/"zen.json"),
            "rejected ZEN must not be written or indexed");
    const auto unknownIdentity=BaseImport::identify(unknownGame);
    rejectUnknown([&] {
      BaseStore::ensure(unknownGame,{{"UNKNOWN.VDF",fs::file_size(unknownGame/"Data/UNKNOWN.VDF")}},
                         [](std::string_view) {});
      });
    require(!fs::exists(cache/unknownIdentity.fingerprint) && !fs::exists(store/"base"/unknownIdentity.fingerprint),
            "rejected import must not publish a base");
    // Neither a failed import nor an unrelated log may poison the next import.
    zenkit::Logger::log(zenkit::LogLevel::ERROR,"contract","Unknown object type: outside import");
    std::cout << "unknown class: ZEN and world import rejected, file/class reported, no publication\n";
    // SIGTERM skips RAII cleanup. The next lock owner must remove these partial bases.
    const auto interrupted=fork(); require(interrupted>=0,"fork failed");
    if(interrupted==0) {
      try {
        BaseStore::ensure(game,mounted,[&](std::string_view message) {
          if(message!="base: importing installation") return;
          for(const auto& entry:fs::directory_iterator(cache))
            if(entry.path().filename().string().starts_with(identity.fingerprint+".tmp-"))
              std::ofstream(entry.path()/"partial") << "interrupted import";
          const auto partialStore=store/"base"/(identity.fingerprint+".tmp-1");
          fs::create_directory(partialStore);
          std::ofstream(partialStore/"partial") << "interrupted publication";
          std::raise(SIGTERM);
          });
        }
      catch(...) { _exit(1); }
      _exit(2);
      }
    int interruptedStatus=0;
    require(waitpid(interrupted,&interruptedStatus,0)==interrupted && WIFSIGNALED(interruptedStatus) &&
            WTERMSIG(interruptedStatus)==SIGTERM,"import child must terminate with SIGTERM");
    const auto otherPartial=store/"base"/(std::string(64,'0')+".tmp-1");
    fs::create_directory(otherPartial);
    // Publication is optional for the game; this comparison needs it complete.
    BaseStore::ensure(game,mounted,[](std::string_view) {}).publication.get();
    equal(reference,base);
    equal(reference,store/"base"/identity.fingerprint);
    for(const auto& root:{cache,store/"base"})
      for(const auto& entry:fs::directory_iterator(root))
        require(!entry.path().filename().string().starts_with(identity.fingerprint+".tmp-"),
                "interrupted temporary directory survived: "+entry.path().string());
    require(fs::exists(otherPartial),"store cleanup must not touch another fingerprint");
    std::cout << "SIGTERM recovery: cache and store partial bases removed; other fingerprint untouched\n";

    std::string logs;
    const auto log=[&](std::string_view message) { logs+=std::string(message)+"\n"; };
    const auto select=[&](const char* expected) {
      logs.clear();
      auto selected=BaseStore::resolve(game,mounted,log);
      require(selected.path==base,"wrong base path");
      if(selected.publication.valid()) selected.publication.get();
      require(logs.find(std::string("base: ")+expected+" ")!=std::string::npos,"wrong source: "+logs);
      require(logs.find("base: importing installation")==std::string::npos,"unexpected import");
      std::cout << logs;
      };
    // A cache hit must return even while publication is blocked. It must also
    // clean an interrupted store copy, despite the cache already being complete.
    {
    const auto partial=store/"base"/(identity.fingerprint+".tmp-2");
    fs::create_directory(partial);
    const auto lockPath=store/"base"/(identity.fingerprint+".lock");
    const int lock=::open(lockPath.c_str(),O_RDWR);
    require(lock>=0 && flock(lock,LOCK_EX)==0,"cannot hold publication lock");
    auto selected=BaseStore::resolve(game,mounted,log);
    require(selected.path==base && BaseImport::matches(base,identity),"cache must be usable before publication");
    require(selected.publication.valid() && selected.publication.wait_for(std::chrono::milliseconds(50))==std::future_status::timeout,
            "publication must be asynchronous");
    flock(lock,LOCK_UN); close(lock);
    selected.publication.get();
    require(!fs::exists(partial),"cache hit must clean interrupted publication");
    std::cout << "background publication: cache ready while store locked; cache-hit recovery passed\n";
    }
    // Exercise real Resources destruction while the publisher cannot make
    // progress. The old async future blocks here until SIGALRM kills the child.
    {
    const auto lockPath=store/"base"/(identity.fingerprint+".lock");
    const int lock=::open(lockPath.c_str(),O_RDWR);
    require(lock>=0 && flock(lock,LOCK_EX)==0,"cannot hold shutdown publication lock");
    std::cout.flush(); std::cerr.flush();
    const auto child=fork(); require(child>=0,"fork failed");
    if(child==0) {
      close(lock);
      try {
        {
        Resources resources;
        Resources::loadBase();
        require(Resources::worldBaseDir("NEWWORLD.ZEN")==base/"worlds/NEWWORLD","engine did not select automatic base");
        // Bound only the destruction: selection hashes the installation and takes longer.
        alarm(5);
        }
        _exit(0);
        }
      catch(...) { _exit(1); }
      }
    int status=0;
    const auto waited=waitpid(child,&status,0);
    flock(lock,LOCK_UN); close(lock);
    require(waited==child && WIFEXITED(status) && WEXITSTATUS(status)==0,
            "resource shutdown must not wait for publication");
    std::cout << "resource shutdown: completed while publication lock remained held\n";
    }
    select("cache");
    fs::remove_all(base);
    select("store");
    checkPointer(cache,game,base);
    fs::remove_all(cache);
    select("store");
    equal(reference,base);
    { std::ofstream broken(base/"base.json"); broken << "{broken"; }
    select("store");
    require(logs.find("rejected cache directory")!=std::string::npos,"corrupt cache was accepted");
    equal(reference,base);

    // Both children start together with empty roots; the lock spans the import.
    fs::remove_all(cache); fs::remove_all(store);
    int gate[2]; require(pipe(gate)==0,"pipe failed");
    std::cout.flush(); std::cerr.flush();
    pid_t children[2];
    for(int i=0;i<2;++i) {
      children[i]=fork(); require(children[i]>=0,"fork failed");
      if(children[i]==0) {
        close(gate[1]);
        char token;
        if(::read(gate[0],&token,1)!=1) _exit(2);
        try {
          std::ofstream events(temporary.path/("child-"+std::to_string(i)+".log"));
          auto selected=BaseStore::ensure(game,mounted,[&](std::string_view message) { events << message << '\n'; });
          if(selected.publication.valid()) selected.publication.get();
          events.close();
          _exit(0);
          }
        catch(...) { _exit(1); }
        }
      }
    close(gate[0]); require(::write(gate[1],"xx",2)==2,"gate write failed"); close(gate[1]);
    for(auto child:children) {
      int status=0; require(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0,"child failed");
      }
    int imports=0, hits=0;
    for(int i=0;i<2;++i) {
      const auto events=read(temporary.path/("child-"+std::to_string(i)+".log"));
      if(events.find("base: importing installation")!=std::string::npos) ++imports;
      if(events.find("base: cache ")!=std::string::npos) ++hits;
      }
    require(imports==1 && hits==1,"two processes must produce exactly one import and one cache hit");
    equal(reference,base);
    checkPointer(cache,game,base);
    std::cout << "concurrent processes: 2, imports: " << imports << ", cache hits: " << hits << '\n';

    const auto changedGame=temporary.path/"game";
    fs::create_directories(changedGame/"Data");
    fs::path smallest;
    const auto sourceData=fs::path(game)/"Data";
    // Native scanning treats directory symlinks as files, so mirror real directories.
    for(const auto& entry:fs::recursive_directory_iterator(sourceData)) {
      const auto mirror=changedGame/"Data"/entry.path().lexically_relative(sourceData);
      if(entry.is_directory()) { fs::create_directories(mirror); continue; }
      fs::create_symlink(fs::absolute(entry.path()),mirror);
      if(entry.path().parent_path()!=sourceData) continue;
      auto extension=entry.path().extension().string();
      for(auto& c:extension) c=char(std::toupper(static_cast<unsigned char>(c)));
      if(extension==".VDF" && (smallest.empty() || entry.file_size()<fs::file_size(smallest))) smallest=entry.path();
      }
    for(const auto& entry:fs::directory_iterator(game)) {
      auto name=entry.path().filename().string();
      for(auto& c:name) c=char(std::toupper(static_cast<unsigned char>(c)));
      if(name=="_WORK") fs::create_directory_symlink(entry.path(),changedGame/entry.path().filename());
      }
    const auto changedArchive=changedGame/"Data"/smallest.filename();
    fs::remove(changedArchive);
    fs::copy_file(smallest,changedArchive);
    const auto beforeAppend=BaseImport::identify(changedGame);
    { std::ofstream append(changedArchive,std::ios::binary|std::ios::app); append.put('\0'); }
    require(BaseImport::identify(changedGame).fingerprint!=beforeAppend.fingerprint,"archive size +1 did not change fingerprint");
    std::cout << "archive size +1: different fingerprint\n";
    // A selected loose program changes the identity, also when its size stays the same.
    {
    const auto scriptsGame=temporary.path/"scripts-game";
    fs::create_directories(scriptsGame);
    fs::create_directory_symlink(fs::absolute(fs::path(game)/"Data"),scriptsGame/"Data");
    fs::path work, program;
    for(const auto& entry:fs::directory_iterator(game)) {
      auto name=entry.path().filename().string();
      for(auto& c:name) c=char(std::toupper(static_cast<unsigned char>(c)));
      if(name=="_WORK" && entry.is_directory()) work=entry.path();
      }
    require(!work.empty(),"installation has no _work");
    for(const auto& entry:fs::recursive_directory_iterator(work)) {
      const auto mirror=scriptsGame/work.filename()/entry.path().lexically_relative(work);
      if(entry.is_directory()) { fs::create_directories(mirror); continue; }
      auto name=entry.path().filename().string(), parent=entry.path().parent_path().filename().string();
      for(auto& c:name) c=char(std::toupper(static_cast<unsigned char>(c)));
      for(auto& c:parent) c=char(std::toupper(static_cast<unsigned char>(c)));
      if(name=="GOTHIC.DAT" && parent=="_COMPILED") { fs::copy_file(entry.path(),mirror); program=mirror; }
      else fs::create_symlink(fs::absolute(entry.path()),mirror);
      }
    require(!program.empty(),"installation has no _work/.../_compiled/GOTHIC.DAT");
    require(BaseImport::identify(scriptsGame).json==identity.json,"mirrored installation must keep the identity");
    {
    std::fstream flip(program,std::ios::binary|std::ios::in|std::ios::out);
    flip.seekg(-1,std::ios::end);
    const char last=char(flip.get());
    flip.seekp(-1,std::ios::end);
    flip.put(char(last^0x01));
    require(bool(flip),"cannot change GOTHIC.DAT copy");
    }
    require(BaseImport::identify(scriptsGame).fingerprint!=identity.fingerprint,"same-size GOTHIC.DAT change kept the fingerprint");
    }
    std::cout << "loose GOTHIC.DAT same-size change: different fingerprint\n";
    // Isolate retention from publication, which owns its own source lease.
    setenv("OPENGOTHIC_BASE_STORE","",1);
    // Keep a real Resources owner alive while another process selects B, C and D.
    auto otherMounted=mounted;
    fs::path latest;
    {
    Resources resources;
    Resources::loadBase();
    fs::last_write_time(base,fs::file_time_type::clock::now()-std::chrono::hours(1));
    const auto worldFile=Resources::worldBaseDir("NEWWORLD.ZEN")/"vobs.json";
    const auto expected=BaseImport::sha256(read(worldFile));
    for(int i=0;i<3;++i) {
      ++otherMounted.front().second;
      const auto other=BaseImport::identify(game,otherMounted);
      latest=cache/other.fingerprint;
      fs::create_directory(latest);
      write(latest/"sources/sources.json","{\"format\":\"gothic-sources\",\"version\":1,\"synthetic\":\"retention fixture\"}\n");
      BaseImport::writeBaseMetadata(latest,other);
      const auto child=fork(); require(child>=0,"fork failed");
      if(child==0) {
        try { BaseStore::resolve(game,otherMounted,log); _exit(0); }
        catch(...) { _exit(1); }
        }
      int status=0;
      require(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0,"cache selection child failed");
      require(BaseImport::sha256(read(worldFile))==expected,"active game lost its base world data");
      }
    require(fs::is_directory(base),"active base must survive three newer selections");
    }
    // Once the owner releases the lease, normal retention can remove A.
    require(BaseStore::resolve(game,otherMounted,log).path==latest,"wrong last fingerprint");
    require(!fs::exists(base),"released base should be pruned");
    setenv("OPENGOTHIC_BASE_STORE",store.c_str(),1);
    select("store");
    std::cout << "active base survives B/C/D; released base is pruned\n";
    // Retention only touches fingerprint directories, keeps the current base and two recent ones.
    for(int i=0;i<4;++i) {
      const auto old=cache/std::string(64,char('0'+i));
      fs::create_directory(old);
      fs::last_write_time(old,fs::file_time_type::clock::now()-std::chrono::hours(i+1));
      }
    select("cache");
    size_t directories=0;
    for(const auto& entry:fs::directory_iterator(cache))
      if(entry.is_directory() && entry.path().filename().string().size()==64) ++directories;
    require(directories==3,"cache must retain three fingerprints");
    setenv("OPENGOTHIC_BASE","",1);
    select("cache");
    setenv("OPENGOTHIC_BASE",reference.c_str(),1);
    require(BaseStore::resolve(game,mounted,log).path==reference,"explicit base must win");
    std::cout << "base-store contract passed: import, cache, store, corrupt metadata, 2 processes, fingerprint, active lease, retention, SIGTERM recovery, explicit base\n";
    return 0;
    }
  catch(const std::exception& e) { std::cerr << "base-store contract: " << e.what() << '\n'; return 1; }
  }
