#pragma once

#include <rapidjson/document.h>
#include <rapidjson/prettywriter.h>
#include <rapidjson/stringbuffer.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>

// The definitions sections share their reversible byte-string mapping and JSON I/O.
// This code has no dependency on the script VM.
namespace DefinitionJson {
using Json = rapidjson::Value;
using Allocator = rapidjson::Document::AllocatorType;

inline void require(bool ok, const std::string& why) {
  if(!ok) throw std::runtime_error(why);
  }
inline const Json& at(const Json& value, const char* key) {
  require(value.IsObject() && value.HasMember(key),std::string("missing field ")+key);
  return value[key];
  }
inline std::string string(const Json& value) {
  require(value.IsString(),"expected a string");
  return {value.GetString(),value.GetStringLength()};
  }
inline int32_t integer(const Json& value) {
  require(value.IsInt(),"expected int32 value");
  return value.GetInt();
  }
inline void object(const Json& value, std::initializer_list<std::string_view> fields) {
  require(value.IsObject(),"expected an object");
  std::set<std::string_view> seen;
  for(auto it=value.MemberBegin();it!=value.MemberEnd();++it) {
    const std::string_view name(it->name.GetString(),it->name.GetStringLength());
    require(std::find(fields.begin(),fields.end(),name)!=fields.end(),"unexpected field "+std::string(name));
    require(seen.insert(name).second,"duplicate field "+std::string(name));
    }
  for(auto name:fields) require(seen.contains(name),"missing field "+std::string(name));
  }

// Script strings are bytes, including undefined code-page bytes. Encoding each byte
// as its Unicode value preserves them without guessing the program's language.
inline std::string encode(std::string_view value) {
  std::string out;
  for(auto byte:value) {
    const auto c=uint8_t(byte);
    if(c<128) out+=char(c);
    else { out+=char(0xc0|(c>>6)); out+=char(0x80|(c&63)); }
    }
  return out;
  }
inline std::string bytes(const Json& value) {
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
inline Json text(std::string_view value, Allocator& allocator) {
  return Json(value.data(),rapidjson::SizeType(value.size()),allocator);
  }
inline void put(Json& json, const char* name, Json value, Allocator& allocator) {
  json.AddMember(text(name,allocator),value,allocator);
  }
inline rapidjson::Document read(const std::filesystem::path& path) {
  std::ifstream input(path,std::ios::binary);
  require(bool(input),"cannot read "+path.string());
  const std::string bytes{std::istreambuf_iterator<char>(input),{}};
  rapidjson::Document document;
  document.Parse<rapidjson::kParseFullPrecisionFlag | rapidjson::kParseValidateEncodingFlag>(bytes.data(),bytes.size());
  require(!document.HasParseError(),"invalid JSON: "+path.string());
  return document;
  }
inline void write(const std::filesystem::path& path, const Json& value) {
  rapidjson::StringBuffer buffer;
  rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buffer);
  require(value.Accept(writer),"cannot encode "+path.string());
  require(!std::filesystem::is_symlink(path),"definitions output is a symlink: "+path.string());
  std::ofstream output(path,std::ios::binary);
  output.write(buffer.GetString(),std::streamsize(buffer.GetSize())); output.put('\n');
  require(bool(output),"cannot write "+path.string());
  }
inline void difference(const Json& expected, const Json& actual, const std::string& path, int& errors) {
  if(expected==actual) return;
  if(expected.IsObject() && actual.IsObject()) {
    if(expected.MemberCount()!=actual.MemberCount()) { ++errors; std::cerr<<path<<": field count differs\n"; }
    for(auto it=expected.MemberBegin();it!=expected.MemberEnd();++it) {
      const auto found=actual.FindMember(it->name);
      if(found==actual.MemberEnd()) { ++errors; std::cerr<<path<<'.'<<it->name.GetString()<<": missing field\n"; }
      else difference(it->value,found->value,path+"."+it->name.GetString(),errors);
      }
    for(auto it=actual.MemberBegin();it!=actual.MemberEnd();++it)
      if(!expected.HasMember(it->name)) { ++errors; std::cerr<<path<<'.'<<it->name.GetString()<<": unexpected field\n"; }
    }
  else if(expected.IsArray() && actual.IsArray()) {
    if(expected.Size()!=actual.Size()) { ++errors; std::cerr<<path<<": count differs\n"; }
    for(rapidjson::SizeType i=0;i<std::min(expected.Size(),actual.Size());++i)
      difference(expected[i],actual[i],path+"["+std::to_string(i)+"]",errors);
    }
  else { ++errors; std::cerr<<path<<": value differs from fresh VM\n"; }
  }
}
