// Installation identity and ready-base validation use file I/O only.
#include "baseimport.h"
#include "baseformat.h"
#include "sha256.h"
#include "gothicglb.h"
#include "utils/fileutil.h"

#include <Tempest/Dir>
#include <Tempest/File>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <exception>
#include <fstream>
#include <map>
#include <mutex>
#include <thread>
#include <tuple>

namespace fs = std::filesystem;
namespace BaseImport {
namespace {
using Tempest::Dir;
void require(bool ok, const std::string& what) {
  if(!ok)
    throw std::runtime_error(what);
  }

std::string upper(std::string s) {
  for(auto& c:s)
    c = char(std::toupper(static_cast<unsigned char>(c)));
  return s;
  }

bool modelSource(const fs::path& path) {
  const auto ext=upper(path.extension().string());
  return ext==".MDH" || ext==".MDM" || ext==".MDL" || ext==".MAN" ||
         ext==".MSB" || ext==".MDS" || ext==".MMB";
  }

// Loose inputs of the not yet migrated families (P10): a superset of what any supported
// language or OUTPUTUNITS choice can select, so the identity does not depend on settings.
bool remainingSource(const fs::path& path) {
  const auto ext=upper(path.extension().string());
  return ext==".FNT" || ext==".WAV" || ext==".OGG" || ext==".BIK" || ext==".SGT" || ext==".STY" ||
         ext==".DLS" || ext==".DAT" || ext==".BIN";
  }

// SHA-256 of every file, a few files at a time: the archives alone are gigabytes.
std::vector<std::string> sha256Files(const std::vector<fs::path>& files) {
  std::vector<std::string> sums(files.size());
  std::atomic<size_t> next{0};
  std::mutex          sync;
  std::exception_ptr  error;
  const auto worker=[&] {
    for(size_t i=next++; i<files.size(); i=next++) {
      try { sums[i]=sha256File(files[i]); }
      catch(...) {
        std::lock_guard<std::mutex> guard(sync);
        if(!error) error=std::current_exception();
        }
      }
    };
  std::vector<std::thread> threads(std::clamp<size_t>(std::thread::hardware_concurrency(),1,8));
  for(auto& t:threads) t=std::thread(worker);
  for(auto& t:threads) t.join();
  if(error) std::rethrow_exception(error);
  return sums;
  }

std::vector<std::byte> readHost(const fs::path& path) {
  std::ifstream f(path,std::ios::binary|std::ios::ate);
  require(bool(f),"cannot open "+path.string());
  std::vector<std::byte> b(static_cast<size_t>(f.tellg()));
  f.seekg(0);
  f.read(reinterpret_cast<char*>(b.data()),std::streamsize(b.size()));
  require(bool(f),"cannot read "+path.string());
  return b;
  }

void writeHost(const fs::path& path, const void* data, size_t size) {
  const auto tmp = path.string()+".tmp";
  {
  std::ofstream f(tmp,std::ios::binary|std::ios::trunc);
  f.write(static_cast<const char*>(data),std::streamsize(size));
  require(bool(f),"cannot write "+tmp);
  }
  fs::rename(tmp,path);
  }

rapidjson::Document parseJson(const fs::path& file) {
  const auto bytes=readHost(file);
  rapidjson::Document doc;
  doc.Parse<rapidjson::kParseFullPrecisionFlag>(reinterpret_cast<const char*>(bytes.data()),bytes.size());
  require(!doc.HasParseError() && doc.IsObject(),"invalid JSON: "+file.string());
  return doc;
  }

// CommandLine's form of the installation path: forward slashes and a trailing '/'.
std::u16string installation(const fs::path& gothic) {
  auto path = gothic.u16string();
  for(auto& c:path)
    if(c==u'\\')
      c = u'/';
  if(!path.empty() && path.back()!=u'/')
    path.push_back(u'/');
  return path;
  }

struct Scanned {
  std::u16string name;
  int64_t        time = 0;
  uint16_t       ord = 0;
  bool           isMod = false;
  };

// Resources::vdfTimestamp, unchanged.
int64_t vdfTimestamp(const std::u16string& name) {
  enum {
    VDF_COMMENT_LENGTH   = 256,
    VDF_SIGNATURE_LENGTH = 16,
    };
  uint32_t count=0, timestamp=0;
  char sign[VDF_SIGNATURE_LENGTH]={};

  try {
    Tempest::RFile fin(name);
    fin.seek(VDF_COMMENT_LENGTH);
    fin.read(sign,VDF_SIGNATURE_LENGTH);
    fin.read(&count,sizeof(count));
    fin.seek(4);
    fin.read(&timestamp,sizeof(timestamp));

    return int64_t(timestamp);
    }
  catch(...) {
    return -1;
    }
  }

// Resources::detectVdf, unchanged apart from its result type.
void detectVdf(std::vector<Scanned>& ret, const std::u16string& root) {
  Dir::scan(root,[&root,&ret](const std::u16string& vdf, Dir::FileType t){
    if(t==Dir::FT_File) {
      Scanned ar;
      ar.name  = root+vdf;
      ar.time  = vdfTimestamp(ar.name);
      ar.ord   = uint16_t(ret.size());
      ar.isMod = vdf.rfind(u".mod")==vdf.size()-4;

      if(std::filesystem::file_size(ar.name)>0)
        ret.emplace_back(std::move(ar));
      return;
      }

    // switch uses NX extension for language dependant stuff
    if(vdf.find(u".NX.")!=std::string::npos)
      return;

    if(t==Dir::FT_Dir && vdf!=u".." && vdf!=u".") {
      auto dir = root + vdf + u"/";
      detectVdf(ret,dir);
      }
    });
  }

}

std::vector<RuntimeArchive> runtimeArchives(const fs::path& gothic) {
  std::vector<Scanned> archives;
  detectVdf(archives,FileUtil::nestedPath(installation(gothic),{u"Data"},Dir::FT_Dir));

  // Resources::loadVdfs: addon archives first!
  std::stable_sort(archives.begin(),archives.end(),[](const Scanned& a,const Scanned& b){
    int aIsMod = a.isMod ? 1 : -1;
    int bIsMod = b.isMod ? 1 : -1;
    return std::make_tuple(aIsMod,a.time,int(a.ord)) >
           std::make_tuple(bIsMod,b.time,int(b.ord));
    });

  std::vector<RuntimeArchive> out;
  for(auto& a:archives) {
    RuntimeArchive r;
    r.path      = fs::path(a.name);
    r.name      = upper(r.path.filename().string());
    r.size      = fs::file_size(r.path);
    r.timestamp = a.time;
    r.mod       = a.isMod;
    out.push_back(std::move(r));
    }
  return out;
  }

std::string sha256(std::string_view bytes) {
  return Sha256::hash(bytes);
  }

std::string sha256File(const fs::path& file) {
  std::ifstream f(file,std::ios::binary);
  if(!f) throw std::runtime_error("cannot open "+file.string());
  Sha256 sha;
  char buf[1<<16];
  while(f) {
    f.read(buf,sizeof(buf));
    sha.update(buf,size_t(f.gcount()));
    }
  require(f.eof(),"cannot read "+file.string());
  return sha.finish();
  }

namespace {
struct InstallationFiles {
  std::vector<RuntimeArchive> archives;
  std::vector<std::pair<std::string,fs::path>> loose;
  };

InstallationFiles installationFiles(const fs::path& gothic, Files mounted) {
  // The engine's mounted list in its real mount order. Archive header timestamps and the
  // order decide which copy of a path wins, so both are part of the identity.
  InstallationFiles result;
  auto& archives=result.archives;
  for(auto& archive:runtimeArchives(gothic)) {
    auto found=std::find(mounted.begin(),mounted.end(),std::make_pair(archive.name,archive.size));
    if(found==mounted.end())
      found=std::find_if(mounted.begin(),mounted.end(),[&](const auto& m) { return m.first==archive.name; });
    if(found==mounted.end()) continue;
    archive.size=found->second;
    archives.push_back(std::move(archive));
    mounted.erase(found);
    }
  require(mounted.empty(),"mounted archive not found under Data: "+(mounted.empty() ? std::string() : mounted.front().first));
  auto& loose=result.loose;
  for(const auto& dir:fs::directory_iterator(gothic))
    if(dir.is_directory() && upper(dir.path().filename().string())=="_WORK") {
      for(const auto& file:fs::recursive_directory_iterator(dir.path()))
        if(file.is_regular_file() && (upper(file.path().extension().string())==".ZEN" || modelSource(file.path()) ||
                                      remainingSource(file.path())))
          loose.emplace_back(file.path().lexically_relative(gothic).generic_string(),file.path());
      break;
      }
  std::sort(loose.begin(),loose.end());
  return result;
  }

Files defaultMounted(const fs::path& gothic) {
  Files mounted;
  for(const auto& archive:runtimeArchives(gothic))
    if(!archive.mod) mounted.emplace_back(archive.name,archive.size);
  return mounted;
  }
}

std::string snapshot(const fs::path& gothic, Files mounted) {
  const auto inputs=installationFiles(gothic,std::move(mounted));
  const auto metadata=[&](const fs::path& path, uintmax_t size) {
    return ",\"size\":"+std::to_string(size)+",\"mtime\":"+
           std::to_string(std::chrono::duration_cast<std::chrono::nanoseconds>(
             fs::last_write_time(path).time_since_epoch()).count());
    };
  std::string json="{\"archives\":[";
  for(size_t i=0;i<inputs.archives.size();++i) {
    const auto& a=inputs.archives[i];
    json+=std::string(i ? "," : "")+"{\"path\":"+gothicglb::jsonString(a.path.lexically_relative(gothic).generic_string())+
          ",\"name\":"+gothicglb::jsonString(a.name)+metadata(a.path,a.size)+",\"timestamp\":"+std::to_string(a.timestamp)+"}";
    }
  json+="],\"loose_files\":[";
  for(size_t i=0;i<inputs.loose.size();++i) {
    const auto& [name,path]=inputs.loose[i];
    json+=std::string(i ? "," : "")+"{\"path\":"+gothicglb::jsonString(name)+metadata(path,fs::file_size(path))+"}";
    }
  return json+"]}";
  }

std::string snapshot(const fs::path& gothic) {
  return snapshot(gothic,defaultMounted(gothic));
  }

Identity identify(const fs::path& gothic, Files mounted) {
  const auto inputs=installationFiles(gothic,std::move(mounted));
  const auto& archives=inputs.archives;
  const auto& loose=inputs.loose;
  std::vector<fs::path> files;
  for(const auto& a:archives) files.push_back(a.path);
  for(const auto& l:loose) files.push_back(l.second);
  const auto sums=sha256Files(files);
  std::string archiveJson="[";
  for(size_t i=0; i<archives.size(); ++i)
    archiveJson+=std::string(i ? "," : "")+"\n    {\"name\":"+gothicglb::jsonString(archives[i].name)+
                 ",\"size\":"+std::to_string(archives[i].size)+",\"timestamp\":"+std::to_string(archives[i].timestamp)+
                 ",\"sha256\":\""+sums[i]+"\"}";
  std::string looseJson="[";
  for(size_t i=0; i<loose.size(); ++i)
    looseJson+=std::string(i ? "," : "")+"\n    {\"path\":"+gothicglb::jsonString(loose[i].first)+
               ",\"size\":"+std::to_string(fs::file_size(loose[i].second))+",\"sha256\":\""+sums[archives.size()+i]+"\"}";
  // "archives" is in mount order; equal timestamps keep the engine's tie order.
  const auto source="\n  \"format\":\"gothic-base\",\n  \"version\":2,\n  \"format_sha256\":\""+std::string(BaseFormatSha256)+
                    "\",\n  \"archives\":"+archiveJson+"\n  ],\n  \"loose_files\":"+looseJson+"\n  ]\n}\n";
  Identity identity;
  identity.fingerprint=sha256("{"+source);
  identity.json="{\n  \"fingerprint\":\""+identity.fingerprint+"\","+source;
  return identity;
  }

Identity identify(const fs::path& gothic) {
  std::map<std::string,fs::path> archives;
  for(const auto& entry:fs::directory_iterator(gothic/"Data"))
    if(entry.is_regular_file() && upper(entry.path().extension().string())==".VDF")
      archives[upper(entry.path().filename().string())]=entry.path();
  require(!archives.empty(),"no archives in "+(gothic/"Data").string());
  Files mounted;
  for(const auto& [name,path]:archives) mounted.emplace_back(name,fs::file_size(path));
  return identify(gothic,std::move(mounted));
  }

namespace {
constexpr const char* nativeManifest="base-files.json";

bool digestString(const rapidjson::Value& value) {
  return value.IsString() && value.GetStringLength()==64 &&
         std::string_view(value.GetString(),64).find_first_not_of("0123456789abcdef")==std::string_view::npos;
  }

void checkIdentity(const rapidjson::Value& value) {
  require(value.IsObject() && value.HasMember("format") && value["format"]=="gothic-base" &&
          value.HasMember("version") && value["version"]==2 && value.HasMember("format_sha256") &&
          value["format_sha256"]==BaseFormatSha256,"base importer format differs");
  require(value.HasMember("fingerprint") && digestString(value["fingerprint"]),"invalid base fingerprint");
  require(value.HasMember("archives") && value["archives"].IsArray(),"invalid base archives");
  for(const auto& a:value["archives"].GetArray())
    require(a.IsObject() && a.HasMember("name") && a["name"].IsString() &&
            a.HasMember("size") && a["size"].IsUint64() && a.HasMember("timestamp") && a["timestamp"].IsInt64() &&
            a.HasMember("sha256") && digestString(a["sha256"]),"invalid base archive identity");
  require(value.HasMember("loose_files") && value["loose_files"].IsArray(),"invalid base loose files");
  for(const auto& file:value["loose_files"].GetArray())
    require(file.IsObject() && file.HasMember("path") && file["path"].IsString() &&
            file.HasMember("size") && file["size"].IsUint64() && file.HasMember("sha256") &&
            digestString(file["sha256"]),"invalid base loose identity");
  }

void checkBinding(const fs::path& dir, const rapidjson::Value& binding, const char* name, bool hash=true) {
  require(binding.IsObject() && binding.HasMember("path") && binding["path"].IsString() &&
          binding["path"]==name && binding.HasMember("size") && binding["size"].IsUint64() &&
          binding.HasMember("sha256") && digestString(binding["sha256"]),std::string("invalid base binding: ")+name);
  const auto file=dir/name;
  require(fs::is_regular_file(file),"missing base file: "+file.string());
  require(fs::file_size(file)==binding["size"].GetUint64(),"base file size differs: "+file.string());
  if(hash) require(sha256File(file)==binding["sha256"].GetString(),"base file SHA-256 differs: "+file.string());
  }
}

Identity readIdentity(const fs::path& dir) {
  auto json=parseJson(dir/"base.json");
  checkIdentity(json);
  Identity identity;
  identity.fingerprint=json["fingerprint"].GetString();
  json.RemoveMember("sources");
  json.RemoveMember("manifest");
  rapidjson::StringBuffer bytes;
  rapidjson::Writer<rapidjson::StringBuffer> writer(bytes);
  json.Accept(writer);
  identity.json.assign(bytes.GetString(),bytes.GetSize());
  return identity;
  }

void writeBaseMetadata(const fs::path& dir, const Identity& identity) {
  const auto sources=dir/"sources"/"sources.json";
  require(fs::is_regular_file(sources),"missing source manifest "+sources.string());
  std::vector<fs::path> files;
  for(const auto& entry:fs::recursive_directory_iterator(dir)) {
    const auto name=entry.path().lexically_relative(dir).generic_string();
    if(name=="base.json" || name==nativeManifest) continue;
    require(!entry.is_symlink(),"base contains a symbolic link: "+entry.path().string());
    if(entry.is_regular_file()) files.push_back(entry.path());
    }
  std::sort(files.begin(),files.end(),[&](const auto& a,const auto& b) {
    return a.lexically_relative(dir).generic_string()<b.lexically_relative(dir).generic_string();
    });
  const auto sums=sha256Files(files);
  std::string manifest="{\"format\":\"gothic-base-files\",\"version\":1,\"files\":[";
  for(size_t i=0;i<files.size();++i)
    manifest+=std::string(i ? "," : "")+"\n{\"path\":"+gothicglb::jsonString(files[i].lexically_relative(dir).generic_string())+
              ",\"size\":"+std::to_string(fs::file_size(files[i]))+",\"sha256\":\""+sums[i]+"\"}";
  manifest+="\n]}\n";
  writeHost(dir/nativeManifest,manifest.data(),manifest.size());
  const auto binding=[&](const fs::path& path) {
    return "{\"path\":"+gothicglb::jsonString(path.lexically_relative(dir).generic_string())+
           ",\"size\":"+std::to_string(fs::file_size(path))+",\"sha256\":\""+sha256File(path)+"\"}";
    };
  auto json=identity.json;
  const auto end=json.find_last_not_of(" \t\r\n");
  require(end!=std::string::npos && json[end]=='}',"invalid identity");
  json.replace(end,json.size()-end,",\n  \"sources\":"+binding(sources)+
               ",\n  \"manifest\":"+binding(dir/nativeManifest)+"\n}\n");
  writeHost(dir/"base.json",json.data(),json.size());
  }

bool matches(const fs::path& dir, const Identity& identity, bool payloadHashes, std::string* reason) {
  try {
    auto actual=parseJson(dir/"base.json");
    checkIdentity(actual);
    require(actual["fingerprint"]==identity.fingerprint.c_str(),"base fingerprint differs");
    require(actual.HasMember("sources"),"base.json has no source manifest binding");
    checkBinding(dir,actual["sources"],"sources/sources.json",false);
    const std::string sourceSum=actual["sources"]["sha256"].GetString();
    require(actual.HasMember("manifest"),"base.json has no native manifest binding");
    checkBinding(dir,actual["manifest"],nativeManifest);
    actual.RemoveMember("sources");
    actual.RemoveMember("manifest");
    rapidjson::Document expected;
    expected.Parse(identity.json.c_str());
    require(!expected.HasParseError() && actual==expected,"base identity differs from installation");
    const auto manifest=parseJson(dir/nativeManifest);
    require(manifest.HasMember("format") && manifest["format"]=="gothic-base-files" &&
            manifest.HasMember("version") && manifest["version"]==1 && manifest.HasMember("files") &&
            manifest["files"].IsArray(),"invalid base file manifest");
    std::string previous;
    bool sourceListed=false;
    std::vector<fs::path> checkedFiles;
    std::vector<std::string> expectedSums;
    for(const auto& entry:manifest["files"].GetArray()) {
      require(entry.IsObject() && entry.HasMember("path") && entry["path"].IsString() &&
              entry.HasMember("size") && entry["size"].IsUint64() && entry.HasMember("sha256") &&
              digestString(entry["sha256"]),"invalid base file manifest entry");
      const std::string name=entry["path"].GetString();
      const fs::path relative=name;
      require(!name.empty() && !relative.is_absolute() && relative.lexically_normal()==relative &&
              name!="base.json" && name!=nativeManifest && name>previous &&
              std::none_of(relative.begin(),relative.end(),[](const auto& part) { return part==".."; }),
              "invalid base manifest path: "+name);
      previous=name;
      if(name=="sources/sources.json") {
        sourceListed=true;
        require(entry["sha256"]==sourceSum.c_str(),"source manifest SHA-256 binding differs");
        }
      const auto file=dir/relative;
      std::error_code error;
      const auto size=fs::file_size(file,error);
      require(!error,"missing or unreadable base file: "+file.string());
      require(size==entry["size"].GetUint64(),"base file size differs: "+file.string());
      if(payloadHashes || relative.extension()==".json") {
        checkedFiles.push_back(file);
        expectedSums.emplace_back(entry["sha256"].GetString());
        }
      }
    const auto sums=sha256Files(checkedFiles);
    for(size_t i=0;i<sums.size();++i)
      require(sums[i]==expectedSums[i],"base file SHA-256 differs: "+checkedFiles[i].string());
    require(sourceListed,"base file manifest omits sources/sources.json");
    if(reason) reason->clear();
    return true;
    }
  catch(const std::exception& e) {
    if(reason) *reason=e.what();
    return false;
    }
  }

}
