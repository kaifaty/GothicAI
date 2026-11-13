#include "musicdata.h"
#include "gothicglb.h"
#include "sha256.h"

#include <rapidjson/error/en.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <stdexcept>

namespace MusicData {

void require(bool ok, const std::string& reason) {
  if(!ok)
    throw std::runtime_error(reason);
  }

namespace {
namespace fs = std::filesystem;
using Json = rapidjson::Value;

std::string upperAscii(std::string_view s) {
  std::string out(s);
  for(auto& c:out)
    if(c>='a' && c<='z')
      c = char(c-'a'+'A');
  return out;
  }

// zenkit::VfsNode::child trims the requested name with std::isspace.
std::string_view trimTrailing(std::string_view s) {
  while(!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
    s.remove_suffix(1);
  return s;
  }

const Json& member(const Json& obj, const char* name) {
  require(obj.HasMember(name),std::string("missing field ")+name);
  return obj[name];
  }

uint64_t unsignedValue(const Json& value, const char* name) {
  require(value.IsUint64(),std::string(name)+" must be an unsigned integer");
  return value.GetUint64();
  }

void exactMembers(const Json& obj, const char* what, std::initializer_list<const char*> names) {
  require(obj.IsObject(),std::string(what)+" must be an object");
  for(const char* name:names)
    member(obj,name);
  require(obj.MemberCount()==names.size(),std::string(what)+" has unknown members");
  }

std::string optionalString(const Json& v, const char* name) {
  return v.IsNull() ? std::string() : gothicglb::jsonToString(v,name);
  }

bool isHex64(std::string_view s) {
  return s.size()==64 && s.find_first_not_of("0123456789abcdef")==std::string::npos;
  }

bool isKind(std::string_view k) {
  return k=="segment" || k=="style" || k=="dls";
  }

// Host paths stay inside the installation, as Resources::BaseIndex::load requires of loose files.
std::string relativePath(const Json& v, const char* name) {
  auto path = gothicglb::jsonToString(v,name);
  const fs::path p(path);
  require(!path.empty() && !p.is_absolute() && path.find('\0')==std::string::npos &&
          std::none_of(p.begin(),p.end(),[](const fs::path& part) { return part==".."; }),
          std::string("invalid relative ")+name);
  return path;
  }

struct Key {
  std::string projection, kind, sha;
  };

// "<projection>/<kind>/<BIN_SHA>"; false when malformed.
bool splitKey(std::string_view key, Key& out) {
  const auto a = key.find('/');
  if(a==std::string_view::npos)
    return false;
  const auto b = key.find('/',a+1);
  if(b==std::string_view::npos)
    return false;
  out.projection = std::string(key.substr(0,a));
  out.kind       = std::string(key.substr(a+1,b-a-1));
  out.sha        = std::string(key.substr(b+1));
  return (out.projection=="gk" || out.projection=="dx8") && isKind(out.kind) && isHex64(out.sha);
  }

Outcome readOutcome(const Json& v, const char* projection, const std::string& kind,
                    const std::map<std::string,std::string>& records) {
  require(v.IsObject() && v.MemberCount()==1,std::string(projection)+" outcome must have exactly one member");
  Outcome o;
  if(v.HasMember("record")) {
    o.record = gothicglb::jsonToString(v["record"],"record");
    Key k;
    require(splitKey(o.record,k) && k.projection==projection && k.kind==kind,
            "record "+o.record+" does not match "+projection+"/"+kind);
    const auto r = records.find(o.record);
    require(r!=records.end(),"unknown record "+o.record);
    o.identity = r->second;
    return o;
    }
  require(v.HasMember("failure"),std::string(projection)+" outcome needs record or failure");
  const auto& f = v["failure"];
  if(std::string_view(projection)=="gk") {
    exactMembers(f,"gk failure",{"result"});
    o.failure = gothicglb::jsonToString(f["result"],"result");
    // DmResult names without the prefix; only results a parse of a file can end with.
    require(*o.failure=="INVALID_ARGUMENT" || *o.failure=="INVALID_STATE" || *o.failure=="NOT_FOUND" ||
            *o.failure=="FILE_CORRUPT","gk failure must name a failing parse result");
    } else {
    exactMembers(f,"dx8 failure",{"error"});
    o.failure = gothicglb::jsonToString(f["error"],"error");
    }
  return o;
  }

bool equalsAsciiCase(std::string_view a, std::string_view b) {
  if(a.size()!=b.size())
    return false;
  for(size_t i=0; i<a.size(); ++i) {
    char x = a[i], y = b[i];
    if(x>='a' && x<='z')
      x = char(x-'a'+'A');
    if(y>='a' && y<='z')
      y = char(y-'a'+'A');
    if(x!=y)
      return false;
    }
  return true;
  }

std::vector<uint8_t> readWhole(const fs::path& file, const std::function<void(const std::string&)>& fail) {
  std::ifstream f(file,std::ios::binary|std::ios::ate);
  if(!f)
    fail("missing "+file.filename().string());
  const auto end = f.tellg();
  if(end<0)
    fail("read error "+file.filename().string());
  std::vector<uint8_t> out(static_cast<size_t>(end));
  f.seekg(0,std::ios::beg);
  f.read(reinterpret_cast<char*>(out.data()),std::streamsize(out.size()));
  out.resize(size_t(f.gcount()));
  return out;
  }


}

const Source* Index::findGk(std::string_view name) const {
  const auto it = gk.find(upperAscii(trimTrailing(name)));
  return it==gk.end() ? nullptr : &sources[it->second];
  }

const Source* Index::findDx8(std::string_view relativePath) const {
  const auto exact = dx8.find(std::string(relativePath));
  if(exact!=dx8.end())
    return &sources[exact->second];
  const Source* found = nullptr;
  for(auto& [path,i]:dx8) {
    if(!equalsAsciiCase(path,relativePath))
      continue;
    if(found!=nullptr)
      throw std::runtime_error("music data: ambiguous host path "+std::string(relativePath));
    found = &sources[i];
    }
  return found;
  }

Index readIndex(const fs::path& dir) {
  const auto file = dir/"music.json";
  try {
    const auto bytes = gothicglb::readFile(file);
    rapidjson::Document doc;
    doc.Parse<rapidjson::kParseValidateEncodingFlag>(reinterpret_cast<const char*>(bytes.data()),bytes.size());
    require(!doc.HasParseError(),rapidjson::GetParseError_En(doc.GetParseError()));
    exactMembers(doc,"index",{"format","version","archives","loose_files","records","sources","gk","dx8"});
    require(gothicglb::jsonToString(doc["format"],"format")=="gothic-music","unsupported format");
    require(doc["version"].IsInt() && doc["version"].GetInt()==1,"unsupported version");

    Index index;
    index.dir = dir;
    require(doc["archives"].IsArray(),"archives must be an array");
    for(const auto& a:doc["archives"].GetArray()) {
      exactMembers(a,"archive",{"name","size"});
      index.archives.emplace_back(gothicglb::jsonToString(a["name"],"name"),unsignedValue(a["size"],"size"));
      }
    require(doc["loose_files"].IsArray(),"loose_files must be an array");
    for(const auto& l:doc["loose_files"].GetArray()) {
      exactMembers(l,"loose file",{"path","size"});
      index.looseFiles.emplace_back(relativePath(l["path"],"path"),unsignedValue(l["size"],"size"));
      }

    require(doc["records"].IsObject(),"records must be an object");
    for(const auto& e:doc["records"].GetObject()) {
      const auto key = gothicglb::jsonToString(e.name,"record key");
      Key k;
      require(splitKey(key,k),"invalid record key "+key);
      const auto identity = gothicglb::jsonToString(e.value,"record identity");
      require(isHex64(identity),"invalid identity of "+key);
      require(index.records.emplace(key,identity).second,"duplicate record "+key);
      }

    require(doc["sources"].IsArray(),"sources must be an array");
    for(const auto& v:doc["sources"].GetArray()) {
      exactMembers(v,"source",{"origin","path","archive","size","sha256","kind","gk","dx8"});
      Source s;
      s.origin  = gothicglb::jsonToString(v["origin"],"origin");
      s.archive = optionalString(v["archive"],"archive");
      s.size    = unsignedValue(v["size"],"size");
      s.sha256  = gothicglb::jsonToString(v["sha256"],"sha256");
      s.kind    = gothicglb::jsonToString(v["kind"],"kind");
      require(s.origin=="host" || s.origin=="archive","invalid origin");
      require((s.origin=="archive")==!s.archive.empty(),"archive name must match the origin");
      s.path = s.origin=="host" ? relativePath(v["path"],"path") : gothicglb::jsonToString(v["path"],"path");
      require(isHex64(s.sha256),"invalid sha256 of "+s.path);
      require(isKind(s.kind),"invalid kind of "+s.path);
      s.gk  = readOutcome(v["gk"],"gk",s.kind,index.records);
      s.dx8 = readOutcome(v["dx8"],"dx8",s.kind,index.records);
      index.sources.push_back(std::move(s));
      }

    require(doc["gk"].IsObject(),"gk must be an object");
    for(const auto& e:doc["gk"].GetObject()) {
      const auto name = gothicglb::jsonToString(e.name,"gk name");
      require(!name.empty() && name==upperAscii(name),"gk name must be ASCII upper case: "+name);
      const auto i = unsignedValue(e.value,"gk source");
      require(i<index.sources.size(),"gk source out of range: "+name);
      index.gk[name] = size_t(i);
      }
    require(doc["dx8"].IsObject(),"dx8 must be an object");
    for(const auto& e:doc["dx8"].GetObject()) {
      const auto path = relativePath(e.name,"dx8 path");
      const auto i = unsignedValue(e.value,"dx8 source");
      require(i<index.sources.size(),"dx8 source out of range: "+path);
      require(index.sources[size_t(i)].origin=="host" && index.sources[size_t(i)].path==path,
              "dx8 path must name its host source: "+path);
      require(index.dx8.emplace(path,size_t(i)).second,"duplicate dx8 path "+path);
      }
    return index;
    }
  catch(const std::exception& e) {
    throw std::runtime_error("music data: "+file.string()+": "+e.what());
    }
  }

std::vector<uint8_t> readRecord(const Index& index, std::string_view key) {
  const auto fail = [&](const std::string& reason) {
    throw std::runtime_error("music data: "+std::string(key)+": "+reason);
    };
  Key k;
  if(!splitKey(key,k))
    fail("invalid record key");
  const auto id = index.records.find(std::string(key));
  if(id==index.records.end())
    fail("not in the index");

  const auto base = index.dir/k.projection/k.kind;
  const auto json = readWhole(base/(k.sha+".json"),fail);
  auto       bin  = readWhole(base/(k.sha+".bin"),fail);

  // The writer's exact single-line JSON: any other bytes (member order, spacing, values) are rejected.
  const std::string expected = std::string("{\"format\":\"gothic-music-record\",\"version\":1,\"projection\":\"")+
                               k.projection+"\",\"kind\":\""+k.kind+"\",\"buffer\":\""+k.sha+".bin\",\"byte_length\":"+
                               std::to_string(bin.size())+"}";
  if(std::string_view(reinterpret_cast<const char*>(json.data()),json.size())!=expected)
    fail("record JSON does not match its key and buffer");
  if(Sha256::hash(std::string_view(reinterpret_cast<const char*>(bin.data()),bin.size()))!=k.sha)
    fail("buffer sha256 differs");
  std::string whole(reinterpret_cast<const char*>(json.data()),json.size());
  whole.append(reinterpret_cast<const char*>(bin.data()),bin.size());
  if(Sha256::hash(whole)!=id->second)
    fail("record identity differs");
  return bin;
  }


}
