#include "itemdefinitions.h"
#include "itemrecordfields.h"

#include <rapidjson/document.h>

#include <fstream>
#include <stdexcept>
#include <type_traits>

namespace {
using Json = rapidjson::Value;
void require(bool ok, const std::string& why) {
  if(!ok) throw std::runtime_error(why);
  }
const Json& at(const Json& value, const char* key) {
  require(value.IsObject() && value.HasMember(key),std::string("missing field ")+key);
  return value[key];
  }
std::string string(const Json& value) {
  require(value.IsString(),"expected a string");
  return {value.GetString(),value.GetStringLength()};
  }
std::string bytes(const Json& value) {
  const auto text=string(value);
  std::string out;
  for(size_t i=0;i<text.size();++i) {
    const auto c=uint8_t(text[i]);
    if(c<128) out+=char(c);
    else {
      require((c==0xc2 || c==0xc3) && i+1<text.size() && (uint8_t(text[i+1])&0xc0)==0x80,
              "invalid script byte string");
      out+=char(((c&3)<<6)|(uint8_t(text[++i])&63));
      }
    }
  return out;
  }
template<class T> void field(T& out, const Json& value, bool function, std::vector<std::string>& names) {
  if constexpr(std::is_array_v<T>) {
    require(value.IsArray() && value.Size()==std::extent_v<T>,"array extent differs");
    for(size_t i=0;i<std::extent_v<T>;++i) field(out[i],value[rapidjson::SizeType(i)],function,names);
    }
  else if constexpr(std::is_same_v<T,std::string>) out=bytes(value);
  else {
    if(function) {
      if(value.IsNull()) { out=T(0); names.emplace_back(); }
      else if(value.IsInt() && value.GetInt()==-1) { out=T(-1); names.emplace_back(); }
      else { names.push_back(string(value)); require(!names.back().empty(),"empty symbol name"); out=T(0); }
      }
    else {
      require(value.IsInt(),"expected int32");
      out=T(value.GetInt());
      }
    }
  }
}

ItemDefinitions::Library ItemDefinitions::read(const std::filesystem::path& base, std::string_view programSha256) {
  const auto file=base/"definitions"/programSha256/"items.json";
  try {
    std::ifstream input(file,std::ios::binary);
    require(bool(input),"cannot open file");
    const std::string text{std::istreambuf_iterator<char>(input),{}};
    rapidjson::Document doc;
    doc.Parse<rapidjson::kParseValidateEncodingFlag>(text.data(),text.size());
    require(!doc.HasParseError(),"invalid JSON");
    require(string(at(doc,"format"))=="gothic-item-definitions","unsupported format");
    const auto& version=at(doc,"version");
    require(version.IsInt() && version.GetInt()==1,"unsupported version");
    require(!string(at(doc,"program_name")).empty(),"empty program name");
    require(string(at(doc,"program_sha256"))==programSha256,"program SHA differs");
    require(string(at(doc,"string_encoding"))=="unicode-byte-mapping","unsupported string encoding");
    require(string(at(doc,"native_schema"))==ItemRecordSchema,"ItemRecord schema differs");
    const auto& records=at(doc,"records");
    require(records.IsArray(),"records must be an array");
    Library result;
    for(const auto& record:records.GetArray()) {
      const auto name=string(at(record,"symbol"));
      try {
        require(!name.empty() && result.items.count(name)==0 && result.uncaptured.count(name)==0,"duplicate or empty symbol");
        const auto status=string(at(record,"status"));
        if(status=="uncaptured") {
          require(!record.HasMember("fields"),"uncaptured item has fields");
          result.uncaptured.emplace(name,string(at(record,"reason"))+": "+string(at(record,"detail")));
          continue;
          }
        require(status=="captured","unsupported capture status");
        const auto& calls=at(record,"calls");
        require(calls.IsArray() && calls.Empty(),"item constructor calls are unsupported");
        const auto& fields=at(record,"fields");
        require(fields.IsObject(),"fields must be an object");
        Definition definition;
        size_t count=0;
        itemRecordFields(definition.record,[&](const char* key, auto& value, bool function) {
          try {
            std::vector<std::string> names;
            field(value,at(fields,key),function,names);
            if(function) definition.symbols.emplace(key,std::move(names));
            ++count;
            }
          catch(const std::exception& e) { throw std::runtime_error(std::string(key)+": "+e.what()); }
          });
        require(count==fields.MemberCount(),"unexpected ItemRecord field");
        result.items.emplace(name,std::move(definition));
        }
      catch(const std::exception& e) { throw std::runtime_error(name+": "+e.what()); }
      }
    return result;
    }
  catch(const std::exception& e) { throw std::runtime_error("item definitions: "+file.string()+": "+e.what()); }
  }
