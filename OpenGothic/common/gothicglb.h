#pragma once

// Shared pieces of our glTF formats for converted Gothic content
// (docs/asset-migration.md): the full asset::Material record as JSON in
// extras, exact float formatting, a minimal GLB writer and a strict GLB reader.

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <rapidjson/document.h>
#include "asset/material.h"

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#endif
#include "../lib/cgltf/cgltf.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace gothicglb {

using Json = rapidjson::Value;

[[noreturn]] void reject(const std::string& reason);
uint32_t bits(float v);

// Gothic strings are single-byte: each byte maps to the code point of the same value.
std::string jsonString(std::string_view s);
std::string jsonFloat(float v); // 9 significant digits: every float round-trips
std::string jsonVec2(const zenkit::Vec2& v);

const Json&  member(const Json& obj, const char* name);
float        jsonToFloat(const Json& v, const char* name);
int64_t      jsonToInt(const Json& v, const char* name);
bool         jsonToBool(const Json& v, const char* name);
std::string  jsonToString(const Json& v, const char* name);
zenkit::Vec2 jsonToVec2(const Json& v, const char* name);

template<class E>
E jsonToEnum(const Json& obj, const char* name) {
  return E(jsonToInt(member(obj,name),name));
  }


// extras.gothic of a material: every field of asset::Material plus its source index.
std::string      materialJson(const asset::Material& m, size_t index);
asset::Material materialFromJson(const Json& g);
bool             sameMaterial(const asset::Material& a, const asset::Material& b);

class GlbBuilder {
  public:
    std::vector<uint8_t> bin;
    std::vector<std::string> views, accessors;

    template<class T>
    size_t view(const std::vector<T>& data, int target) {
      while(bin.size()%4!=0)
        bin.push_back(0);
      const size_t offset = bin.size();
      const size_t size   = data.size()*sizeof(T);
      bin.resize(offset+size);
      if(size>0)
        std::memcpy(bin.data()+offset,data.data(),size);
      std::string v = "{\"buffer\":0,\"byteOffset\":" + std::to_string(offset) + ",\"byteLength\":" + std::to_string(size);
      if(target!=0)
        v += ",\"target\":" + std::to_string(target);
      v += "}";
      views.push_back(v);
      return views.size()-1;
      }

    template<class T>
    size_t accessor(const std::vector<T>& data, int componentType, const char* type, size_t components,
                    bool normalized, int target, const std::string& minMax = {}) {
      const size_t v = view(data,target);
      std::string a = "{\"bufferView\":" + std::to_string(v) + ",\"componentType\":" + std::to_string(componentType) +
                      ",\"count\":" + std::to_string(data.size()/components) + ",\"type\":\"" + type + "\"";
      if(normalized)
        a += ",\"normalized\":true";
      a += minMax;
      a += "}";
      accessors.push_back(a);
      return accessors.size()-1;
      }

    std::vector<uint8_t> finish(const std::string& jsonBody) const {
      std::string json = jsonBody;
      while(json.size()%4!=0)
        json.push_back(' ');
      std::vector<uint8_t> payload = bin;
      while(payload.size()%4!=0)
        payload.push_back(0);
      std::vector<uint8_t> out;
      auto u32 = [&out](size_t v) {
        if(v>std::numeric_limits<uint32_t>::max())
          reject("chunk larger than 4 GiB");
        for(unsigned i=0; i<4; ++i)
          out.push_back(uint8_t(v>>(8*i)));
        };
      const size_t total = 12 + 8 + json.size() + 8 + payload.size();
      u32(0x46546C67); // "glTF"
      u32(2);
      u32(total);
      u32(json.size());
      u32(0x4E4F534A); // "JSON"
      out.insert(out.end(),json.begin(),json.end());
      u32(payload.size());
      u32(0x004E4942); // "BIN\0"
      out.insert(out.end(),payload.begin(),payload.end());
      return out;
      }

    std::string viewsJson() const     { return join(views); }
    std::string accessorsJson() const { return join(accessors); }

  private:
    static std::string join(const std::vector<std::string>& items) {
      std::string s = "[";
      for(size_t i=0; i<items.size(); ++i) {
        if(i>0)
          s += ",";
        s += items[i];
        }
      return s + "]";
      }
  };


void                 writeFile(const std::filesystem::path& path, const void* data, size_t size);
std::vector<uint8_t> readFile(const std::filesystem::path& path);

struct ParsedGlb {
  std::vector<uint8_t> bytes;
  std::unique_ptr<cgltf_data,decltype(&cgltf_free)> data {nullptr,cgltf_free};
  };


// One embedded buffer, no URIs, no compression; validated; default scene required.
ParsedGlb parseGlb(const std::filesystem::path& path);

const cgltf_accessor* findAttribute(const cgltf_primitive& p, cgltf_attribute_type type, const char* name = nullptr);
// Null when there are no extras or no "gothic" member.
std::unique_ptr<rapidjson::Document> parseExtras(const cgltf_data& d, const cgltf_extras& extras);
// The accessor named by extras.gothic.<name>, which must hold count elements.
const cgltf_accessor* extrasAccessor(const cgltf_data& d, const rapidjson::Document* extras, const char* name, size_t count);

}
