#include "gothicglb.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <stdexcept>

namespace gothicglb {

namespace {

std::string bytesToUtf8(std::string_view s) {
  std::string out;
  for(const char ch:s) {
    const auto c = static_cast<unsigned char>(ch);
    if(c<0x80) {
      out.push_back(ch);
      } else {
      out.push_back(char(0xC0 | (c>>6)));
      out.push_back(char(0x80 | (c&0x3F)));
      }
    }
  return out;
  }

std::string utf8ToBytes(std::string_view s) {
  std::string out;
  for(size_t i=0; i<s.size(); ++i) {
    const auto c = static_cast<unsigned char>(s[i]);
    if(c<0x80) {
      out.push_back(char(c));
      continue;
      }
    if((c&0xE0)==0xC0 && i+1<s.size()) {
      const unsigned cp = ((c&0x1Fu)<<6) | (static_cast<unsigned char>(s[i+1])&0x3Fu);
      if(cp<0x100) {
        out.push_back(char(cp));
        ++i;
        continue;
        }
      }
    // A name typed in Blender may hold any UTF-8: keep it as is.
    return std::string(s);
    }
  return out;
  }

}

[[noreturn]] void reject(const std::string& reason) {
  throw std::runtime_error("glb: " + reason);
  }

uint32_t bits(float v) {
  uint32_t r;
  std::memcpy(&r,&v,sizeof(r));
  return r;
  }

// Gothic strings are single-byte (Windows-125x). JSON needs UTF-8: each byte
// becomes the code point of the same value, and back, so names survive byte for byte.
std::string jsonString(std::string_view s) {
  std::string out = "\"";
  for(const char ch:bytesToUtf8(s)) {
    const auto c = static_cast<unsigned char>(ch);
    if(c=='"' || c=='\\') {
      out.push_back('\\');
      out.push_back(ch);
      } else if(c<0x20) {
      char buf[8];
      std::snprintf(buf,sizeof(buf),"\\u%04x",c);
      out += buf;
      } else {
      out.push_back(ch);
      }
    }
  out.push_back('"');
  return out;
  }

std::string jsonFloat(float v) {
  if(!std::isfinite(v))
    reject("non-finite material value");
  char buf[32];
  // 9 significant digits round-trip every float.
  std::snprintf(buf,sizeof(buf),"%.9g",double(v));
  return buf;
  }

std::string jsonVec2(const zenkit::Vec2& v) {
  return "[" + jsonFloat(v.x) + "," + jsonFloat(v.y) + "]";
  }

std::string materialJson(const asset::Material& m, size_t index) {
  std::string s = "{";
  s += "\"index\":" + std::to_string(index);
  s += ",\"name\":" + jsonString(m.name);
  s += ",\"group\":" + std::to_string(int(m.group));
  s += ",\"color\":[" + std::to_string(m.color.r) + "," + std::to_string(m.color.g) + "," +
       std::to_string(m.color.b) + "," + std::to_string(m.color.a) + "]";
  s += ",\"smooth_angle\":" + jsonFloat(m.smooth_angle);
  s += ",\"texture\":" + jsonString(m.texture);
  s += ",\"texture_scale\":" + jsonVec2(m.texture_scale);
  s += ",\"texture_anim_fps\":" + jsonFloat(m.texture_anim_fps);
  s += ",\"texture_anim_map_mode\":" + std::to_string(int(m.texture_anim_map_mode));
  s += ",\"texture_anim_map_dir\":" + jsonVec2(m.texture_anim_map_dir);
  s += ",\"disable_collision\":" + std::string(m.disable_collision ? "true" : "false");
  s += ",\"disable_lightmap\":" + std::string(m.disable_lightmap ? "true" : "false");
  s += ",\"dont_collapse\":" + std::string(m.dont_collapse ? "true" : "false");
  s += ",\"detail_object\":" + jsonString(m.detail_object);
  s += ",\"detail_object_scale\":" + jsonFloat(m.detail_object_scale);
  s += ",\"force_occluder\":" + std::string(m.force_occluder ? "true" : "false");
  s += ",\"environment_mapping\":" + std::string(m.environment_mapping ? "true" : "false");
  s += ",\"environment_mapping_strength\":" + jsonFloat(m.environment_mapping_strength);
  s += ",\"wave_mode\":" + std::to_string(int(m.wave_mode));
  s += ",\"wave_speed\":" + std::to_string(int(m.wave_speed));
  s += ",\"wave_max_amplitude\":" + jsonFloat(m.wave_max_amplitude);
  s += ",\"wave_grid_size\":" + jsonFloat(m.wave_grid_size);
  s += ",\"ignore_sun\":" + std::string(m.ignore_sun ? "true" : "false");
  s += ",\"alpha_func\":" + std::to_string(int(m.alpha_func));
  s += ",\"default_mapping\":" + jsonVec2(m.default_mapping);
  s += "}";
  return s;
  }


const Json& member(const Json& obj, const char* name) {
  if(!obj.IsObject() || !obj.HasMember(name))
    reject(std::string("missing field ") + name);
  return obj[name];
  }

float jsonToFloat(const Json& v, const char* name) {
  if(!v.IsNumber())
    reject(std::string(name) + " must be a number");
  return float(v.GetDouble());
  }

int64_t jsonToInt(const Json& v, const char* name) {
  if(!v.IsInt64())
    reject(std::string(name) + " must be an integer");
  return v.GetInt64();
  }

bool jsonToBool(const Json& v, const char* name) {
  if(!v.IsBool())
    reject(std::string(name) + " must be a boolean");
  return v.GetBool();
  }

std::string jsonToString(const Json& v, const char* name) {
  if(!v.IsString())
    reject(std::string(name) + " must be a string");
  return utf8ToBytes(std::string_view(v.GetString(),v.GetStringLength()));
  }

zenkit::Vec2 jsonToVec2(const Json& v, const char* name) {
  if(!v.IsArray() || v.Size()!=2)
    reject(std::string(name) + " must be [x,y]");
  return {jsonToFloat(v[0],name),jsonToFloat(v[1],name)};
  }


asset::Material materialFromJson(const Json& g) {
  asset::Material m;
  m.name  = jsonToString(member(g,"name"),"name");
  m.group = jsonToEnum<asset::MaterialGroup>(g,"group");
  const auto& c = member(g,"color");
  if(!c.IsArray() || c.Size()!=4)
    reject("color must be [r,g,b,a]");
  for(unsigned i=0; i<4; ++i) {
    const auto v = jsonToInt(c[i],"color");
    if(v<0 || v>255)
      reject("color component outside 0..255");
    m.color[i] = static_cast<unsigned char>(v);
    }
  m.smooth_angle                 = jsonToFloat(member(g,"smooth_angle"),"smooth_angle");
  m.texture                      = jsonToString(member(g,"texture"),"texture");
  m.texture_scale                = jsonToVec2(member(g,"texture_scale"),"texture_scale");
  m.texture_anim_fps             = jsonToFloat(member(g,"texture_anim_fps"),"texture_anim_fps");
  m.texture_anim_map_mode        = jsonToEnum<asset::AnimationMapping>(g,"texture_anim_map_mode");
  m.texture_anim_map_dir         = jsonToVec2(member(g,"texture_anim_map_dir"),"texture_anim_map_dir");
  m.disable_collision            = jsonToBool(member(g,"disable_collision"),"disable_collision");
  m.disable_lightmap             = jsonToBool(member(g,"disable_lightmap"),"disable_lightmap");
  m.dont_collapse                = jsonToBool(member(g,"dont_collapse"),"dont_collapse");
  m.detail_object                = jsonToString(member(g,"detail_object"),"detail_object");
  m.detail_object_scale          = jsonToFloat(member(g,"detail_object_scale"),"detail_object_scale");
  m.force_occluder               = jsonToBool(member(g,"force_occluder"),"force_occluder");
  m.environment_mapping          = jsonToBool(member(g,"environment_mapping"),"environment_mapping");
  m.environment_mapping_strength = jsonToFloat(member(g,"environment_mapping_strength"),"environment_mapping_strength");
  m.wave_mode                    = jsonToEnum<asset::WaveMode>(g,"wave_mode");
  m.wave_speed                   = jsonToEnum<asset::WaveSpeed>(g,"wave_speed");
  m.wave_max_amplitude           = jsonToFloat(member(g,"wave_max_amplitude"),"wave_max_amplitude");
  m.wave_grid_size               = jsonToFloat(member(g,"wave_grid_size"),"wave_grid_size");
  m.ignore_sun                   = jsonToBool(member(g,"ignore_sun"),"ignore_sun");
  m.alpha_func                   = jsonToEnum<asset::AlphaFunction>(g,"alpha_func");
  m.default_mapping              = jsonToVec2(member(g,"default_mapping"),"default_mapping");
  return m;
  }

bool sameMaterial(const asset::Material& a, const asset::Material& b) {
  auto eq2 = [](const zenkit::Vec2& x, const zenkit::Vec2& y) {
    return bits(x.x)==bits(y.x) && bits(x.y)==bits(y.y);
    };
  return a.name==b.name && a.group==b.group &&
         a.color.r==b.color.r && a.color.g==b.color.g && a.color.b==b.color.b && a.color.a==b.color.a &&
         bits(a.smooth_angle)==bits(b.smooth_angle) && a.texture==b.texture &&
         eq2(a.texture_scale,b.texture_scale) && bits(a.texture_anim_fps)==bits(b.texture_anim_fps) &&
         a.texture_anim_map_mode==b.texture_anim_map_mode && eq2(a.texture_anim_map_dir,b.texture_anim_map_dir) &&
         a.disable_collision==b.disable_collision && a.disable_lightmap==b.disable_lightmap &&
         a.dont_collapse==b.dont_collapse && a.detail_object==b.detail_object &&
         bits(a.detail_object_scale)==bits(b.detail_object_scale) && a.force_occluder==b.force_occluder &&
         a.environment_mapping==b.environment_mapping &&
         bits(a.environment_mapping_strength)==bits(b.environment_mapping_strength) &&
         a.wave_mode==b.wave_mode && a.wave_speed==b.wave_speed &&
         bits(a.wave_max_amplitude)==bits(b.wave_max_amplitude) && bits(a.wave_grid_size)==bits(b.wave_grid_size) &&
         a.ignore_sun==b.ignore_sun && a.alpha_func==b.alpha_func && eq2(a.default_mapping,b.default_mapping);
  }


void writeFile(const std::filesystem::path& path, const void* data, size_t size) {
  const auto tmp = path.string() + ".tmp";
  {
  std::ofstream f(tmp,std::ios::binary | std::ios::trunc);
  if(!f)
    reject("cannot write " + tmp);
  f.write(static_cast<const char*>(data),std::streamsize(size));
  if(!f)
    reject("cannot write " + tmp);
  }
  std::filesystem::rename(tmp,path);
  }

std::vector<uint8_t> readFile(const std::filesystem::path& path) {
  std::ifstream f(path,std::ios::binary | std::ios::ate);
  if(!f)
    reject("cannot open " + path.string());
  const auto size = f.tellg();
  if(size<0)
    reject("cannot read " + path.string());
  std::vector<uint8_t> bytes(static_cast<size_t>(size));
  f.seekg(0);
  if(!bytes.empty() && !f.read(reinterpret_cast<char*>(bytes.data()),size))
    reject("cannot read " + path.string());
  return bytes;
  }


ParsedGlb parseGlb(const std::filesystem::path& path) {
  ParsedGlb r;
  r.bytes = readFile(path);
  cgltf_options options = {};
  options.type = cgltf_file_type_glb;
  cgltf_data* parsed = nullptr;
  if(cgltf_parse(&options,r.bytes.data(),r.bytes.size(),&parsed)!=cgltf_result_success)
    reject("cannot parse " + path.string());
  r.data.reset(parsed);
  auto& d = *r.data;
  if(d.buffers_count!=1 || d.buffers[0].uri!=nullptr || d.bin==nullptr || d.buffers[0].size>d.bin_size)
    reject(path.string() + ": expected one embedded buffer");
  if(d.extensions_required_count!=0)
    reject(path.string() + ": extensionsRequired is not supported");
  for(size_t i=0; i<d.accessors_count; ++i) {
    if(d.accessors[i].is_sparse)
      reject(path.string() + ": sparse accessors are not supported");
    if(d.accessors[i].buffer_view!=nullptr && d.accessors[i].buffer_view->has_meshopt_compression)
      reject(path.string() + ": compressed buffer views are not supported");
    }
  // Never call cgltf's URI loader: bind the BIN chunk only.
  d.buffers[0].data = const_cast<void*>(d.bin);
  if(cgltf_validate(&d)!=cgltf_result_success)
    reject(path.string() + ": glTF validation failed");
  if(d.scene==nullptr)
    reject(path.string() + ": a default scene is required");
  return r;
  }

const cgltf_accessor* findAttribute(const cgltf_primitive& p, cgltf_attribute_type type, const char* name) {
  for(size_t i=0; i<p.attributes_count; ++i) {
    const auto& a = p.attributes[i];
    if(a.type!=type || a.index!=0)
      continue;
    if(name!=nullptr && (a.name==nullptr || std::strcmp(a.name,name)!=0))
      continue;
    return a.data;
    }
  return nullptr;
  }

std::unique_ptr<rapidjson::Document> parseExtras(const cgltf_data& d, const cgltf_extras& extras) {
  // cgltf fills extras.data for most objects, but only the offsets for some
  // (meshes): then take the text from the JSON chunk.
  std::string text;
  if(extras.data!=nullptr)
    text = extras.data;
  else if(extras.end_offset>extras.start_offset && d.json!=nullptr && extras.end_offset<=d.json_size)
    text.assign(d.json+extras.start_offset,extras.end_offset-extras.start_offset);
  if(text.empty())
    return nullptr;
  auto doc = std::make_unique<rapidjson::Document>();
  doc->Parse<rapidjson::kParseFullPrecisionFlag>(text.c_str());
  if(doc->HasParseError() || !doc->IsObject())
    reject("invalid extras JSON");
  if(!doc->HasMember("gothic"))
    return nullptr;
  return doc;
  }

const cgltf_accessor* extrasAccessor(const cgltf_data& d, const rapidjson::Document* extras, const char* name,
                                     size_t count) {
  if(extras==nullptr)
    return nullptr;
  const auto& g = (*extras)["gothic"];
  if(!g.IsObject() || !g.HasMember(name))
    return nullptr;
  const auto index = jsonToInt(g[name],name);
  if(index<0 || size_t(index)>=d.accessors_count)
    reject(std::string("extras.gothic.") + name + " accessor out of range");
  const auto* a = &d.accessors[index];
  if(a->count!=count)
    reject(std::string("extras.gothic.") + name + " count does not match the triangles");
  return a;
  }

}
