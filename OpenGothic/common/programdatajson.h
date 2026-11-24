#pragma once

#include <rapidjson/document.h>
#include <rapidjson/error/en.h>

#include <cstdint>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace ProgramData::Detail {
using Json = rapidjson::Value;

inline void require(bool ok, const std::string& reason) {
  if(!ok)
    throw std::runtime_error(reason);
  }

inline const Json& member(const Json& obj, const char* name) {
  require(obj.HasMember(name),std::string("missing field ")+name);
  return obj[name];
  }

inline uint64_t unsignedValue(const Json& value, const char* name) {
  require(value.IsUint64(),std::string(name)+" must be an unsigned integer");
  return value.GetUint64();
  }

inline void exactMembers(const Json& obj, const char* what, std::initializer_list<const char*> names) {
  require(obj.IsObject(),std::string(what)+" must be an object");
  for(const char* name:names)
    member(obj,name);
  require(obj.MemberCount()==names.size(),std::string(what)+" has unknown members");
  }

inline rapidjson::Document parseJson(const std::vector<uint8_t>& bytes) {
  rapidjson::Document doc;
  doc.Parse<rapidjson::kParseValidateEncodingFlag>(reinterpret_cast<const char*>(bytes.data()),bytes.size());
  require(!doc.HasParseError(),rapidjson::GetParseError_En(doc.GetParseError()));
  return doc;
  }

inline bool isDigest(std::string_view digest) {
  return digest.size()==64 && digest.find_first_not_of("0123456789abcdef")==std::string_view::npos;
  }

}
