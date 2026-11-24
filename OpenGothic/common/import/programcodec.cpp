#include "programdata.h"
#include "gothicglb.h"
#include "programdatajson.h"
#include "sha256.h"

#include <cstring>
#include <limits>
#include <stdexcept>

namespace ProgramData {
namespace {
namespace fs = std::filesystem;
using namespace Detail;

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

std::vector<uint8_t> encodeSymbols(const asset::ProgramRecord& program) {
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

fs::path binaryPath(fs::path file) {
  file.replace_extension(".bin");
  return file;
  }

uint8_t programVersion(const Json& value) {
  const auto version=unsignedValue(value,"version");
  require(version<=std::numeric_limits<uint8_t>::max(),"program version out of range");
  return uint8_t(version);
  }

struct ProgramLayout {
  uint8_t version;
  uint64_t symbolCount;
  uint64_t symbolLength;
  uint64_t codeLength;
  uint64_t byteLength;
  };

ProgramLayout readLayout(const Json& doc, const fs::path& payload) {
  exactMembers(doc,"record",{"format","version","buffer","byte_length","data"});
  require(gothicglb::jsonToString(doc["format"],"format")=="gothic-program","unsupported format");
  require(doc["version"].IsInt() && doc["version"].GetInt()==1,"unsupported version");
  require(gothicglb::jsonToString(doc["buffer"],"buffer")==payload.filename().string(),
          "binary filename must be the fixed sibling");
  const auto& data=doc["data"];
  exactMembers(data,"data",{"version","symbols","bytecode"});
  const auto& symbols=data["symbols"];
  const auto& code=data["bytecode"];
  exactMembers(symbols,"symbols",{"offset","count","length"});
  exactMembers(code,"bytecode",{"offset","length"});
  const auto symbolLength=unsignedValue(symbols["length"],"length");
  const auto byteLength=unsignedValue(doc["byte_length"],"byte_length");
  const auto codeLength=unsignedValue(code["length"],"length");
  require(unsignedValue(symbols["offset"],"offset")==0,"symbol offset must be 0");
  require(unsignedValue(code["offset"],"offset")==symbolLength,"bytecode must follow the symbols");
  require(symbolLength<=byteLength && byteLength-symbolLength==codeLength,"section lengths do not match the binary");
  return {programVersion(data["version"]),unsignedValue(symbols["count"],"count"),symbolLength,codeLength,byteLength};
  }

std::vector<asset::ProgramSymbol> decodeSymbols(const std::vector<uint8_t>& bytes, size_t length, uint64_t count) {
  // Each symbol takes at least 73 bytes, so a corrupt count cannot allocate past the section.
  require(length<=bytes.size() && count<=length/73,"symbol count exceeds the symbol section");
  std::vector<asset::ProgramSymbol> symbols(static_cast<size_t>(count));
  Cursor in(bytes,length);
  for(auto& s:symbols) {
    s.name = in.str();
    const auto generated = in.u8();
    require(generated<=1,"invalid generated flag");
    s.generated     = generated==1;
    s.type          = in.u32();
    s.count         = in.u32();
    s.flags         = in.u32();
    s.address       = int32_t(in.u32());
    s.parent        = int32_t(in.u32());
    s.class_offset  = int32_t(in.u32());
    s.member_offset = in.u32();
    s.class_size    = in.u32();
    s.return_type   = in.u32();
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
  require(in.at==length,"symbol section has trailing bytes");
  return symbols;
  }

std::string sha256(const std::vector<uint8_t>& bytes) {
  Sha256 hash;
  hash.update(bytes.data(),bytes.size());
  return hash.finish();
  }

std::string identity(const void* json, size_t jsonSize, const void* bin, size_t binSize) {
  Sha256 sha;
  sha.update(json,jsonSize);
  sha.update(bin,binSize);
  return sha.finish();
  }
}

Encoded encodeProgram(const asset::ProgramRecord& program) {
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

std::string writeProgram(const asset::ProgramRecord& program, const fs::path& dir, std::string* name) {
  const auto e = encodeProgram(program);
  gothicglb::writeFile(dir/(e.name+".bin"),e.bin.data(),e.bin.size());
  gothicglb::writeFile(dir/(e.name+".json"),e.json.data(),e.json.size());
  if(name!=nullptr)
    *name = e.name;
  return e.sha256;
  }

asset::ProgramRecord readProgram(const fs::path& file, std::string* actualSha256) {
  try {
    const auto payload=binaryPath(file);
    require(file!=payload,"JSON and binary paths must differ");
    const auto json=gothicglb::readFile(file);
    const auto layout=readLayout(parseJson(json),payload);
    const auto bin=gothicglb::readFile(payload);
    require(layout.byteLength==bin.size(),"binary byte length mismatch");

    asset::ProgramRecord program;
    program.version=layout.version;
    program.symbols=decodeSymbols(bin,size_t(layout.symbolLength),layout.symbolCount);
    program.bytecode.resize(size_t(layout.codeLength));
    if(layout.codeLength>0)
      std::memcpy(program.bytecode.data(),bin.data()+layout.symbolLength,size_t(layout.codeLength));
    if(actualSha256!=nullptr)
      *actualSha256=identity(json.data(),json.size(),bin.data(),bin.size());
    return program;
    }
  catch(const std::exception& error) {
    throw std::runtime_error("program data: "+file.string()+": "+error.what());
    }
  }

std::filesystem::path symbolMetadataPath(const fs::path& base, std::string_view sourceProgramSha256) {
  require(isDigest(sourceProgramSha256),"program symbol metadata: invalid source program identity");
  return base/"definitions"/sourceProgramSha256/"program-symbols.json";
  }

fs::path symbolMetadataPayloadPath(fs::path file) {
  file.replace_extension(".symbols");
  return file;
  }

asset::ProgramRecord readSymbolMetadata(const fs::path& file, std::string_view sourceProgramSha256) {
  try {
    require(isDigest(sourceProgramSha256),"invalid source program identity");
    const auto doc=parseJson(gothicglb::readFile(file));
    exactMembers(doc,"symbol metadata",{"format","version","source_program_sha256","program_version",
                                        "symbol_count","symbol_byte_length","buffer","buffer_sha256"});
    require(gothicglb::jsonToString(doc["format"],"format")=="gothic-program-symbols","unsupported format");
    require(doc["version"].IsInt() && doc["version"].GetInt()==1,"unsupported version");
    require(gothicglb::jsonToString(doc["source_program_sha256"],"source_program_sha256")==sourceProgramSha256,
            "source program identity differs");
    const auto payload=symbolMetadataPayloadPath(file);
    require(file!=payload && gothicglb::jsonToString(doc["buffer"],"buffer")==payload.filename().string(),
            "symbol payload must be the fixed sibling");
    const auto payloadSha=gothicglb::jsonToString(doc["buffer_sha256"],"buffer_sha256");
    require(isDigest(payloadSha),"invalid symbol payload SHA-256");
    const auto bytes=gothicglb::readFile(payload);
    require(unsignedValue(doc["symbol_byte_length"],"symbol_byte_length")==bytes.size(),"symbol byte length differs");
    require(sha256(bytes)==payloadSha,"symbol payload SHA-256 differs");
    asset::ProgramRecord program;
    program.version=programVersion(doc["program_version"]);
    program.symbols=decodeSymbols(bytes,bytes.size(),unsignedValue(doc["symbol_count"],"symbol_count"));
    return program;
    }
  catch(const std::exception& error) {
    throw std::runtime_error("program symbol metadata: "+file.string()+": "+error.what());
    }
  }

fs::path writeSymbolMetadata(const fs::path& file, std::string_view sourceProgramSha256,
                             const asset::ProgramRecord& program) {
  require(isDigest(sourceProgramSha256),"program symbol metadata: invalid source program identity");
  const auto payload=symbolMetadataPayloadPath(file);
  require(file!=payload,"program symbol metadata: JSON and payload paths must differ");
  const auto bytes=encodeSymbols(program);
  const std::string json="{\"format\":\"gothic-program-symbols\",\"version\":1,\"source_program_sha256\":"+
                         gothicglb::jsonString(sourceProgramSha256)+",\"program_version\":"+
                         std::to_string(unsigned(program.version))+",\"symbol_count\":"+
                         std::to_string(program.symbols.size())+",\"symbol_byte_length\":"+
                         std::to_string(bytes.size())+",\"buffer\":"+gothicglb::jsonString(payload.filename().string())+
                         ",\"buffer_sha256\":\""+sha256(bytes)+"\"}\n";
  gothicglb::writeFile(payload,bytes.data(),bytes.size());
  gothicglb::writeFile(file,json.data(),json.size());
  return payload;
  }

std::string recordSha256(const fs::path& file) {
  const auto json = gothicglb::readFile(file);
  const auto bin  = gothicglb::readFile(binaryPath(file));
  return identity(json.data(),json.size(),bin.data(),bin.size());
  }

std::string compare(const asset::ProgramRecord& expected, const asset::ProgramRecord& actual) {
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
