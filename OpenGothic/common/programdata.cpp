#include "programdata.h"
#include "gothicglb.h"
#include "sha256.h"

#include <rapidjson/error/en.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <stdexcept>

namespace ProgramData {
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

void u32(std::vector<uint8_t>& bin, uint32_t value) {
  for(unsigned i=0; i<4; ++i)
    bin.push_back(uint8_t(value>>(8*i)));
  }

void bytes(std::vector<uint8_t>& bin, std::string_view s) {
  require(s.size()<=std::numeric_limits<uint32_t>::max(),"program data: string too long");
  u32(bin,uint32_t(s.size()));
  bin.insert(bin.end(),s.begin(),s.end());
  }

void count(std::vector<uint8_t>& bin, size_t n) {
  require(n<=std::numeric_limits<uint32_t>::max(),"program data: array too long");
  u32(bin,uint32_t(n));
  }

std::vector<uint8_t> encodeSymbols(const zenkit::DaedalusProgramRecord& program) {
  std::vector<uint8_t> bin;
  for(const auto& s:program.symbols) {
    bytes(bin,s.name);
    bin.push_back(s.generated ? 1 : 0);
    u32(bin,uint32_t(s.type));
    u32(bin,s.count);
    u32(bin,s.flags);
    u32(bin,uint32_t(s.address));
    u32(bin,uint32_t(s.parent));
    u32(bin,uint32_t(s.class_offset));
    u32(bin,s.member_offset);
    u32(bin,s.class_size);
    u32(bin,uint32_t(s.return_type));
    u32(bin,s.file_index);
    u32(bin,s.line_start);
    u32(bin,s.line_count);
    u32(bin,s.char_start);
    u32(bin,s.char_count);
    count(bin,s.int_values.size());
    for(const auto v:s.int_values)
      u32(bin,uint32_t(v));
    count(bin,s.float_bits.size());
    for(const auto v:s.float_bits)
      u32(bin,v);
    count(bin,s.string_values.size());
    for(const auto& v:s.string_values)
      bytes(bin,v);
    }
  return bin;
  }

class Cursor {
  public:
    Cursor(const std::vector<uint8_t>& bin, size_t end) : bin(bin), end(end) {}

    size_t left() const { return end-at; }

    uint8_t u8() {
      require(left()>=1,"symbol section truncated");
      return bin[at++];
      }

    uint32_t u32() {
      require(left()>=4,"symbol section truncated");
      uint32_t value = 0;
      for(unsigned i=0; i<4; ++i)
        value |= uint32_t(bin[at++])<<(8*i);
      return value;
      }

    // Array length whose elements take at least `elementSize` bytes each.
    uint32_t length(size_t elementSize) {
      const auto n = u32();
      require(uint64_t(n)*elementSize<=left(),"array length exceeds the symbol section");
      return n;
      }

    std::string str() {
      const auto n = length(1);
      std::string s(reinterpret_cast<const char*>(bin.data()+at),n);
      at += n;
      return s;
      }

    size_t at = 0;

  private:
    const std::vector<uint8_t>& bin;
    size_t                      end;
  };

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

rapidjson::Document parseJson(const std::vector<uint8_t>& bytes) {
  rapidjson::Document doc;
  doc.Parse<rapidjson::kParseValidateEncodingFlag>(reinterpret_cast<const char*>(bytes.data()),bytes.size());
  require(!doc.HasParseError(),rapidjson::GetParseError_En(doc.GetParseError()));
  return doc;
  }

fs::path binaryPath(fs::path file) {
  file.replace_extension(".bin");
  return file;
  }

std::string identity(const void* json, size_t jsonSize, const void* bin, size_t binSize) {
  Sha256 sha;
  sha.update(json,jsonSize);
  sha.update(bin,binSize);
  return sha.finish();
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
      require(name.size()==64 && name.find_first_not_of("0123456789abcdef")==std::string::npos,"invalid program name");
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

Encoded encodeProgram(const zenkit::DaedalusProgramRecord& program) {
  Encoded e;
  e.bin = encodeSymbols(program);
  const auto symbols = e.bin.size();
  e.bin.insert(e.bin.end(),reinterpret_cast<const uint8_t*>(program.bytecode.data()),
               reinterpret_cast<const uint8_t*>(program.bytecode.data())+program.bytecode.size());
  Sha256 name;
  name.update(&program.version,1);
  name.update(e.bin.data(),e.bin.size());
  e.name = name.finish();
  e.json = "{\"format\":\"gothic-program\",\"version\":1,\"buffer\":\""+e.name+".bin\",\"byte_length\":"+
           std::to_string(e.bin.size())+",\"data\":{\"version\":"+std::to_string(unsigned(program.version))+
           ",\"symbols\":{\"offset\":0,\"count\":"+std::to_string(program.symbols.size())+",\"length\":"+
           std::to_string(symbols)+"},\"bytecode\":{\"offset\":"+std::to_string(symbols)+",\"length\":"+
           std::to_string(program.bytecode.size())+"}}}\n";
  e.sha256 = identity(e.json.data(),e.json.size(),e.bin.data(),e.bin.size());
  return e;
  }

std::string writeProgram(const zenkit::DaedalusProgramRecord& program, const fs::path& dir, std::string* name) {
  const auto e = encodeProgram(program);
  gothicglb::writeFile(dir/(e.name+".bin"),e.bin.data(),e.bin.size());
  gothicglb::writeFile(dir/(e.name+".json"),e.json.data(),e.json.size());
  if(name!=nullptr)
    *name = e.name;
  return e.sha256;
  }

zenkit::DaedalusProgramRecord readProgram(const fs::path& file, std::string* actualSha256) {
  try {
    require(file!=binaryPath(file),"JSON and binary paths must differ");
    const auto json = gothicglb::readFile(file);
    const auto doc  = parseJson(json);
    exactMembers(doc,"record",{"format","version","buffer","byte_length","data"});
    require(gothicglb::jsonToString(doc["format"],"format")=="gothic-program","unsupported format");
    require(doc["version"].IsInt() && doc["version"].GetInt()==1,"unsupported version");
    require(gothicglb::jsonToString(doc["buffer"],"buffer")==binaryPath(file).filename().string(),
            "binary filename must be the fixed sibling");
    const auto& data = doc["data"];
    exactMembers(data,"data",{"version","symbols","bytecode"});
    const auto& symbols = data["symbols"];
    const auto& code    = data["bytecode"];
    exactMembers(symbols,"symbols",{"offset","count","length"});
    exactMembers(code,"bytecode",{"offset","length"});
    const auto version = unsignedValue(data["version"],"version");
    require(version<=std::numeric_limits<uint8_t>::max(),"program version out of range");

    const auto bin = gothicglb::readFile(binaryPath(file));
    const auto symbolCount  = unsignedValue(symbols["count"],"count");
    const auto symbolLength = unsignedValue(symbols["length"],"length");
    const auto codeLength   = unsignedValue(code["length"],"length");
    require(unsignedValue(doc["byte_length"],"byte_length")==bin.size(),"binary byte length mismatch");
    require(unsignedValue(symbols["offset"],"offset")==0,"symbol offset must be 0");
    require(unsignedValue(code["offset"],"offset")==symbolLength,"bytecode must follow the symbols");
    require(symbolLength<=bin.size() && bin.size()-symbolLength==codeLength,"section lengths do not match the binary");

    zenkit::DaedalusProgramRecord program;
    program.version = uint8_t(version);
    Cursor in(bin,size_t(symbolLength));
    // Each symbol takes at least 73 bytes, so a corrupt count cannot allocate past the file.
    require(symbolCount<=symbolLength/73,"symbol count exceeds the symbol section");
    program.symbols.resize(size_t(symbolCount));
    for(auto& s:program.symbols) {
      s.name = in.str();
      const auto generated = in.u8();
      require(generated<=1,"invalid generated flag");
      s.generated     = generated==1;
      s.type          = zenkit::DaedalusDataType(in.u32());
      s.count         = in.u32();
      s.flags         = in.u32();
      s.address       = int32_t(in.u32());
      s.parent        = int32_t(in.u32());
      s.class_offset  = int32_t(in.u32());
      s.member_offset = in.u32();
      s.class_size    = in.u32();
      s.return_type   = zenkit::DaedalusDataType(in.u32());
      s.file_index    = in.u32();
      s.line_start    = in.u32();
      s.line_count    = in.u32();
      s.char_start    = in.u32();
      s.char_count    = in.u32();
      s.int_values.resize(in.length(4));
      for(auto& v:s.int_values)
        v = int32_t(in.u32());
      s.float_bits.resize(in.length(4));
      for(auto& v:s.float_bits)
        v = in.u32();
      s.string_values.resize(in.length(4));
      for(auto& v:s.string_values)
        v = in.str();
      }
    require(in.at==symbolLength,"symbol section has trailing bytes");
    program.bytecode.resize(size_t(codeLength));
    if(codeLength>0)
      std::memcpy(program.bytecode.data(),bin.data()+symbolLength,size_t(codeLength));
    if(actualSha256!=nullptr)
      *actualSha256 = identity(json.data(),json.size(),bin.data(),bin.size());
    return program;
    }
  catch(const std::exception& e) {
    throw std::runtime_error("program data: "+file.string()+": "+e.what());
    }
  }

std::string recordSha256(const fs::path& file) {
  const auto json = gothicglb::readFile(file);
  const auto bin  = gothicglb::readFile(binaryPath(file));
  return identity(json.data(),json.size(),bin.data(),bin.size());
  }

std::string compare(const zenkit::DaedalusProgramRecord& expected, const zenkit::DaedalusProgramRecord& actual) {
  if(expected.version!=actual.version)
    return "program version differs";
  if(expected.symbols.size()!=actual.symbols.size())
    return "symbol count differs: "+std::to_string(expected.symbols.size())+" != "+std::to_string(actual.symbols.size());
  for(size_t i=0; i<expected.symbols.size(); ++i) {
    const auto& a = expected.symbols[i];
    const auto& b = actual.symbols[i];
    const char* field =
      a.name!=b.name                   ? "name" :
      a.generated!=b.generated         ? "generated" :
      a.type!=b.type                   ? "type" :
      a.count!=b.count                 ? "count" :
      a.flags!=b.flags                 ? "flags" :
      a.address!=b.address             ? "address" :
      a.parent!=b.parent               ? "parent" :
      a.class_offset!=b.class_offset   ? "class_offset" :
      a.member_offset!=b.member_offset ? "member_offset" :
      a.class_size!=b.class_size       ? "class_size" :
      a.return_type!=b.return_type     ? "return_type" :
      a.file_index!=b.file_index       ? "file_index" :
      a.line_start!=b.line_start       ? "line_start" :
      a.line_count!=b.line_count       ? "line_count" :
      a.char_start!=b.char_start       ? "char_start" :
      a.char_count!=b.char_count       ? "char_count" :
      a.int_values!=b.int_values       ? "int_values" :
      a.float_bits!=b.float_bits       ? "float_bits" :
      a.string_values!=b.string_values ? "string_values" : nullptr;
    if(field!=nullptr)
      return "symbol "+std::to_string(i)+" "+field+" differs";
    }
  if(expected.bytecode!=actual.bytecode)
    return "bytecode differs";
  return {};
  }

}
