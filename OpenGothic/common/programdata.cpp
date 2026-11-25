#include "programdata.h"
#include "gothicglb.h"
#include "programdatajson.h"

#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace ProgramData {
namespace {
namespace fs = std::filesystem;
using namespace Detail;

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

const Source* selected(const Index& index, size_t source, std::string_view what) {
  const auto& s = index.sources.at(source);
  if(!s.skipped.empty())
    throw std::runtime_error("program data: "+std::string(what)+": selected file is not an importable program ("+
                             s.skipped+"): "+s.path);
  return &s;
  }

std::string optionalString(const Json& v, const char* name) {
  return v.IsNull() ? std::string() : gothicglb::jsonToString(v,name);
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

std::optional<size_t> sourceRef(const Json& v, size_t sources, const char* name) {
  if(v.IsNull())
    return std::nullopt;
  const auto i = unsignedValue(v,name);
  require(i<sources,std::string(name)+" out of range");
  return size_t(i);
  }
}

const Source* Index::findVfs(std::string_view request, std::string_view* step) const {
  if(auto it=vfs.find(upperAscii(trimTrailing(request))); it!=vfs.end()) {
    if(step!=nullptr) *step = "vfs_find_request";
    return selected(*this,it->second,request);
    }
  const auto segment = request.find_last_of("\\/");
  if(segment==std::string_view::npos)
    return nullptr;
  if(auto it=vfs.find(upperAscii(trimTrailing(request.substr(segment+1)))); it!=vfs.end()) {
    if(step!=nullptr) *step = "vfs_find_basename";
    return selected(*this,it->second,request);
    }
  return nullptr;
  }

const Source* Index::findHost(std::string_view relativePath) const {
  const auto key = fs::path(std::string(relativePath)).lexically_normal().generic_string();
  if(auto it=host.find(key); it!=host.end())
    return selected(*this,it->second,relativePath);
  const auto folded = upperAscii(key);
  const Source* found = nullptr;
  size_t        index = 0;
  for(const auto& [path,source]:host)
    if(upperAscii(path)==folded) {
      if(found!=nullptr)
        throw std::runtime_error("program data: "+key+": ambiguous host path (differs only in case)");
      found = &sources[source];
      index = source;
      }
  return found==nullptr ? nullptr : selected(*this,index,relativePath);
  }

const Source* Index::server() const {
  return serverGameDat ? selected(*this,*serverGameDat,"server GOTHIC.DAT") : nullptr;
  }

fs::path Index::path(const Source& source) const {
  if(source.program.empty())
    throw std::runtime_error("program data: "+source.path+": not an importable program ("+source.skipped+")");
  return dir/(source.program+".json");
  }

Index readIndex(const fs::path& dir) {
  const auto file = dir/"programs.json";
  try {
    const auto doc = parseJson(gothicglb::readFile(file));
    exactMembers(doc,"index",{"format","version","archives","loose_files","programs","sources","vfs","host",
                              "selections","server"});
    require(gothicglb::jsonToString(doc["format"],"format")=="gothic-programs","unsupported format");
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
    require(doc["programs"].IsObject(),"programs must be an object");
    for(const auto& p:doc["programs"].GetObject()) {
      const auto name = gothicglb::jsonToString(p.name,"program name");
      require(isDigest(name),"invalid program name");
      index.programs[name] = gothicglb::jsonToString(p.value,"program identity");
      }
    require(doc["sources"].IsArray(),"sources must be an array");
    for(const auto& s:doc["sources"].GetArray()) {
      exactMembers(s,"source",{"origin","path","archive","size","sha256","program","trailing_bytes","skipped"});
      Source src;
      src.origin        = gothicglb::jsonToString(s["origin"],"origin");
      src.path          = gothicglb::jsonToString(s["path"],"path");
      src.archive       = optionalString(s["archive"],"archive");
      src.size          = unsignedValue(s["size"],"size");
      src.sha256        = gothicglb::jsonToString(s["sha256"],"sha256");
      src.program       = optionalString(s["program"],"program");
      src.trailingBytes = unsignedValue(s["trailing_bytes"],"trailing_bytes");
      src.skipped       = optionalString(s["skipped"],"skipped");
      require(src.origin=="host" || src.origin=="archive","invalid origin");
      if(src.origin=="host")
        src.path = relativePath(s["path"],"path");
      require((src.origin=="archive")==!src.archive.empty(),"archive name must match the origin");
      require(src.program.empty()!=src.skipped.empty(),"a source is either a program or skipped");
      if(!src.program.empty()) {
        const auto it = index.programs.find(src.program);
        require(it!=index.programs.end(),"source references an unknown program");
        src.recordSha256 = it->second;
        }
      index.sources.push_back(std::move(src));
      }
    for(const char* table:{"vfs","host"}) {
      require(doc[table].IsObject(),std::string(table)+" must be an object");
      auto& out = std::string_view(table)=="vfs" ? index.vfs : index.host;
      for(const auto& e:doc[table].GetObject()) {
        const auto source = sourceRef(e.value,index.sources.size(),table);
        require(source.has_value(),std::string(table)+" entry without a source");
        out[&out==&index.host ? relativePath(e.name,table) : gothicglb::jsonToString(e.name,table)] = *source;
        }
      }
    require(doc["selections"].IsArray(),"selections must be an array");
    for(const auto& s:doc["selections"].GetArray()) {
      exactMembers(s,"selection",{"request","language","step","path","source"});
      Selection sel;
      sel.request = gothicglb::jsonToString(s["request"],"request");
      require(s["language"].IsInt(),"language must be an integer");
      sel.language = s["language"].GetInt();
      sel.step     = gothicglb::jsonToString(s["step"],"step");
      sel.path     = optionalString(s["path"],"path");
      sel.source   = sourceRef(s["source"],index.sources.size(),"source");
      index.selections.push_back(std::move(sel));
      }
    exactMembers(doc["server"],"server",{"storygraph_dat"});
    index.serverGameDat = sourceRef(doc["server"]["storygraph_dat"],index.sources.size(),"storygraph_dat");
    return index;
    }
  catch(const std::exception& e) {
    throw std::runtime_error("program data: "+file.string()+": "+e.what());
    }
  }

}

const ProgramData::Source* ProgramData::fightProgram(const Index& index) {
  if(auto* source = index.findVfs("Fight.dat"))
    return source;
  return index.findHost("_work/Data/Scripts/_compiled/Fight.dat");
  }

namespace {
const ProgramData::Source* optionalDefinitionProgram(const ProgramData::Index& index, const char* name) {
  try {
    if(auto* source = index.findVfs(name))
      return source;
    return index.findHost(std::string("_work/Data/Scripts/_compiled/")+name);
    }
  catch(const std::runtime_error&) {
    // The legacy loader still reports a skipped or ambiguous program on use.
    return nullptr;
    }
  }
}

const ProgramData::Source* ProgramData::cameraProgram(const Index& index) {
  return optionalDefinitionProgram(index,"Camera.dat");
  }

const ProgramData::Source* ProgramData::sfxProgram(const Index& index) {
  return optionalDefinitionProgram(index,"Sfx.dat");
  }

const ProgramData::Source* ProgramData::musicProgram(const Index& index) {
  return optionalDefinitionProgram(index,"Music.dat");
  }

const ProgramData::Source* ProgramData::particleFxProgram(const Index& index) {
  return optionalDefinitionProgram(index,"ParticleFx.dat");
  }

const ProgramData::Source* ProgramData::visualFxProgram(const Index& index) {
  return optionalDefinitionProgram(index,"VisualFx.dat");
  }
