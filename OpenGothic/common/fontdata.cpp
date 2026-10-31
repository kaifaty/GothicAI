#include "fontdata.h"
#include "gothicglb.h"

#include <rapidjson/error/en.h>

#include <bit>
#include <cctype>
#include <cstdint>
#include <initializer_list>
#include <stdexcept>
#include <vector>

namespace FontData {
namespace {
namespace fs = std::filesystem;
using Json = rapidjson::Value;

// width u8, uv[0].x, uv[0].y, uv[1].x, uv[1].y as raw f32 bits.
constexpr size_t kGlyphStride = 17;

void require(bool ok, const std::string& reason) {
  if(!ok)
    throw std::runtime_error(reason);
  }

fs::path binaryPath(fs::path file) {
  file.replace_extension(".bin");
  return file;
  }

void u32(std::vector<uint8_t>& bin, uint32_t value) {
  for(unsigned i=0; i<4; ++i)
    bin.push_back(uint8_t(value>>(8*i)));
  }

uint32_t u32(const std::vector<uint8_t>& bin, size_t& at) {
  uint32_t value = 0;
  for(unsigned i=0; i<4; ++i)
    value |= uint32_t(bin[at++])<<(8*i);
  return value;
  }

std::vector<uint8_t> encodeGlyphs(const asset::Font& font) {
  std::vector<uint8_t> bin;
  bin.reserve(font.glyphs.size()*kGlyphStride);
  for(const auto& g:font.glyphs) {
    bin.push_back(g.width);
    for(const auto& uv:g.uv) {
      u32(bin,std::bit_cast<uint32_t>(uv.x));
      u32(bin,std::bit_cast<uint32_t>(uv.y));
      }
    }
  return bin;
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

std::string upper(std::string_view s) {
  std::string out(s);
  for(auto& c:out)
    c = char(std::toupper(static_cast<unsigned char>(c)));
  return out;
  }
}

std::string atlasName(std::string_view fnt) {
  require(fnt.size()>4 && upper(fnt.substr(fnt.size()-4))==".FNT","font name without .FNT: "+std::string(fnt));
  return std::string(fnt.substr(0,fnt.size()-4))+".TGA";
  }

void writeFont(const asset::Font& font, std::string_view atlas, const fs::path& file) {
  require(!atlas.empty(),"font data: empty atlas name");
  require(file!=binaryPath(file),"font data: JSON and binary paths must differ");
  const auto bin = encodeGlyphs(font);
  const auto json = "{\"format\":\"gothic-font\",\"version\":1,\"buffer\":"+
                    gothicglb::jsonString(binaryPath(file).filename().string())+
                    ",\"byte_length\":"+std::to_string(bin.size())+
                    ",\"data\":{\"name\":"+gothicglb::jsonString(font.name)+
                    ",\"height\":"+std::to_string(font.height)+
                    ",\"atlas\":"+gothicglb::jsonString(atlas)+
                    ",\"glyphs\":{\"offset\":0,\"count\":"+std::to_string(font.glyphs.size())+
                    ",\"stride\":"+std::to_string(kGlyphStride)+"}}}\n";
  gothicglb::writeFile(binaryPath(file),bin.data(),bin.size());
  gothicglb::writeFile(file,json.data(),json.size());
  }

asset::Font readFont(const fs::path& file, std::string* atlas) {
  try {
    require(file!=binaryPath(file),"JSON and binary paths must differ");
    const auto bytes = gothicglb::readFile(file);
    rapidjson::Document doc;
    doc.Parse<rapidjson::kParseValidateEncodingFlag>(reinterpret_cast<const char*>(bytes.data()),bytes.size());
    require(!doc.HasParseError(),rapidjson::GetParseError_En(doc.GetParseError()));
    exactMembers(doc,"record",{"format","version","buffer","byte_length","data"});
    require(gothicglb::jsonToString(doc["format"],"format")=="gothic-font","unsupported format");
    require(doc["version"].IsInt() && doc["version"].GetInt()==1,"unsupported version");
    require(gothicglb::jsonToString(doc["buffer"],"buffer")==binaryPath(file).filename().string(),
            "binary filename must be the fixed sibling");

    const auto& data = doc["data"];
    exactMembers(data,"data",{"name","height","atlas","glyphs"});
    const auto& glyphs = data["glyphs"];
    exactMembers(glyphs,"glyphs",{"offset","count","stride"});
    require(unsignedValue(glyphs["offset"],"offset")==0,"glyph offset must be 0");
    require(unsignedValue(glyphs["stride"],"stride")==kGlyphStride,"unsupported glyph stride");
    const auto height = unsignedValue(data["height"],"height");
    require(height<=UINT32_MAX,"height out of range");

    const auto bin = gothicglb::readFile(binaryPath(file));
    require(unsignedValue(doc["byte_length"],"byte_length")==bin.size(),"binary byte length mismatch");
    const auto count = unsignedValue(glyphs["count"],"count");
    require(bin.size()%kGlyphStride==0 && count==bin.size()/kGlyphStride,"glyph count does not match the binary size");

    asset::Font font;
    font.name   = gothicglb::jsonToString(data["name"],"name");
    font.height = uint32_t(height);
    font.glyphs.resize(size_t(count));
    size_t at = 0;
    for(auto& g:font.glyphs) {
      g.width = bin[at++];
      for(auto& uv:g.uv) {
        uv.x = std::bit_cast<float>(u32(bin,at));
        uv.y = std::bit_cast<float>(u32(bin,at));
        }
      }
    const auto atlasValue = gothicglb::jsonToString(data["atlas"],"atlas");
    require(!atlasValue.empty(),"empty atlas name");
    if(atlas!=nullptr)
      *atlas = atlasValue;
    return font;
    }
  catch(const std::exception& e) {
    throw std::runtime_error("font data: "+file.string()+": "+e.what());
    }
  }

std::string compare(const asset::Font& expected, const asset::Font& actual) {
  if(expected.name!=actual.name)
    return "font name differs";
  if(expected.height!=actual.height)
    return "font height differs: "+std::to_string(expected.height)+" != "+std::to_string(actual.height);
  if(expected.glyphs.size()!=actual.glyphs.size())
    return "glyph count differs: "+std::to_string(expected.glyphs.size())+" != "+std::to_string(actual.glyphs.size());
  const auto a = encodeGlyphs(expected), b = encodeGlyphs(actual);
  for(size_t i=0; i<a.size(); ++i)
    if(a[i]!=b[i])
      return "glyph "+std::to_string(i/kGlyphStride)+" differs at byte "+std::to_string(i%kGlyphStride);
  return {};
  }

}
