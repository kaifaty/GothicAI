#include "audiodata.h"
#include "gothicglb.h"
#include "sha256.h"

#include <rapidjson/error/en.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <initializer_list>
#include <stdexcept>

namespace AudioData {
namespace {
namespace fs = std::filesystem;
using Json = rapidjson::Value;

void require(bool ok, const std::string& reason) {
  if(!ok)
    throw std::runtime_error(reason);
  }

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

bool isHex64(const std::string& s) {
  return s.size()==64 && s.find_first_not_of("0123456789abcdef")==std::string::npos;
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

struct Payload {
  std::string file;
  uint64_t    size = 0;
  };
}

const Sound* Index::find(std::string_view request) const {
  const auto it = sounds.find(upperAscii(trimTrailing(request)));
  return it==sounds.end() ? nullptr : &it->second;
  }

Index readIndex(const fs::path& dir) {
  const auto file = dir/"audio.json";
  try {
    const auto bytes = gothicglb::readFile(file);
    rapidjson::Document doc;
    doc.Parse<rapidjson::kParseValidateEncodingFlag>(reinterpret_cast<const char*>(bytes.data()),bytes.size());
    require(!doc.HasParseError(),rapidjson::GetParseError_En(doc.GetParseError()));
    exactMembers(doc,"index",{"format","version","archives","loose_files","payloads","sounds"});
    require(gothicglb::jsonToString(doc["format"],"format")=="gothic-audio","unsupported format");
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

    std::map<std::string,Payload> payloads;
    require(doc["payloads"].IsObject(),"payloads must be an object");
    for(const auto& p:doc["payloads"].GetObject()) {
      const auto sha = gothicglb::jsonToString(p.name,"payload name");
      require(isHex64(sha),"invalid payload name");
      exactMembers(p.value,"payload",{"file","size"});
      Payload payload;
      payload.file = gothicglb::jsonToString(p.value["file"],"file");
      payload.size = unsignedValue(p.value["size"],"size");
      require(payload.file==sha.substr(0,2)+"/"+sha+".wav" || payload.file==sha.substr(0,2)+"/"+sha+".ogg",
              "invalid payload file "+payload.file);
      payloads[sha] = payload;
      }

    require(doc["sounds"].IsObject(),"sounds must be an object");
    for(const auto& e:doc["sounds"].GetObject()) {
      const auto name = gothicglb::jsonToString(e.name,"sound name");
      require(!name.empty() && name==upperAscii(name),"sound name must be ASCII upper case: "+name);
      const auto& v = e.value;
      exactMembers(v,"sound",{"payload","class","origin","path","archive","source_sha256","header"});
      Sound s;
      s.payload      = gothicglb::jsonToString(v["payload"],"payload");
      s.cls          = gothicglb::jsonToString(v["class"],"class");
      s.origin       = gothicglb::jsonToString(v["origin"],"origin");
      s.archive      = optionalString(v["archive"],"archive");
      s.sourceSha256 = gothicglb::jsonToString(v["source_sha256"],"source_sha256");
      require(s.cls=="speech" || s.cls=="sound","invalid class of "+name);
      require(s.origin=="host" || s.origin=="archive","invalid origin of "+name);
      require((s.origin=="archive")==!s.archive.empty(),"archive name must match the origin of "+name);
      s.path = s.origin=="host" ? relativePath(v["path"],"path") : gothicglb::jsonToString(v["path"],"path");
      require(isHex64(s.sourceSha256),"invalid source_sha256 of "+name);
      require(v["header"].IsObject(),"header must be an object");
      const auto p = payloads.find(s.payload);
      require(p!=payloads.end(),name+" references an unknown payload");
      s.file = p->second.file;
      s.size = p->second.size;
      index.sounds[name] = std::move(s);
      }
    return index;
    }
  catch(const std::exception& e) {
    throw std::runtime_error("audio data: "+file.string()+": "+e.what());
    }
  }

void readPayload(const Index& index, const Sound& s, std::string_view request, std::vector<uint8_t>& out) {
  out.clear();
  const auto file = index.dir/s.file;
  const auto fail = [&](const std::string& reason) {
    out.clear();
    out.shrink_to_fit();
    throw std::runtime_error("audio data: "+std::string(request)+" ("+file.string()+"): "+reason);
    };
  std::ifstream f(file,std::ios::binary|std::ios::ate);
  if(!f)
    fail("missing");
  const auto end = f.tellg();
  if(end<0)
    fail("read error");
  // One read of the whole file; the size checked below is what was actually read.
  out.resize(size_t(end));
  f.seekg(0,std::ios::beg);
  f.read(reinterpret_cast<char*>(out.data()),std::streamsize(out.size()));
  out.resize(size_t(f.gcount()));
  if(out.size()!=s.size)
    fail("size "+std::to_string(out.size())+", expected "+std::to_string(s.size));
  if(Sha256::hash(std::string_view(reinterpret_cast<const char*>(out.data()),out.size()))!=s.payload)
    fail("sha256 differs");
  }

}
