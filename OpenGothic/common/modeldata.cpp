#include "modeldata.h"
#include "gothicglb.h"

#include <rapidjson/error/en.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>

namespace ModelData {
namespace {
using namespace asset;
using Json = rapidjson::Value;
using Alloc = rapidjson::Document::AllocatorType;
using zenkit::Vec2;
using zenkit::Vec3;
using zenkit::Quat;
using zenkit::Mat4;
using zenkit::Color;
using zenkit::AxisAlignedBoundingBox;
using zenkit::OrientedBoundingBox;
namespace fs = std::filesystem;

[[noreturn]] void fail(const std::string& reason) {
  throw std::runtime_error("model data: "+reason);
  }
void require(bool ok, const std::string& reason) { if(!ok) fail(reason); }
const Json& member(const Json& obj, const char* name) {
  require(obj.IsObject() && obj.HasMember(name),std::string("missing field ")+name);
  return obj[name];
  }
void array(const Json& value) { require(value.IsArray(),"expected array"); }
void array(const Json& value, size_t count) {
  array(value);
  require(value.Size()==count,"expected array of "+std::to_string(count)+" elements");
  }
size_t sizeValue(const Json& value) {
  require(value.IsUint64() && value.GetUint64()<=std::numeric_limits<size_t>::max(),"invalid size");
  return size_t(value.GetUint64());
  }
template<class T> T integer(const Json& value) {
  require(value.IsInt64(),"expected integer");
  const auto n=value.GetInt64();
  require(n>=int64_t(std::numeric_limits<T>::min()) && n<=int64_t(std::numeric_limits<T>::max()),"integer out of range");
  return T(n);
  }
template<class E> E enumeration(const Json& value) {
  return E(integer<std::underlying_type_t<E>>(value));
  }
float real(const Json& value) {
  require(value.IsNumber(),"expected float");
  const auto f=float(value.GetDouble());
  require(std::isfinite(f),"non-finite float");
  return f;
  }
bool boolean(const Json& value) {
  require(value.IsBool(),"expected boolean");
  return value.GetBool();
  }
std::string text(const Json& value) {
  return gothicglb::jsonToString(value,"string");
  }

struct Encoder {
  Alloc alloc;
  std::vector<uint8_t> bin;

  void u16(uint16_t value) {
    for(unsigned i=0;i<2;++i) bin.push_back(uint8_t(value>>(8*i)));
    }
  void u32(uint32_t value) {
    for(unsigned i=0;i<4;++i) bin.push_back(uint8_t(value>>(8*i)));
    }
  void f32(float value) { u32(std::bit_cast<uint32_t>(value)); }
  void vec3(const Vec3& v) { f32(v.x); f32(v.y); f32(v.z); }
  };

Json number(float value) {
  require(std::isfinite(value),"non-finite metadata float");
  return Json(double(value));
  }
Json text(Encoder& w, std::string_view value) {
  std::string utf8;
  for(char ch:value) {
    const auto c=static_cast<unsigned char>(ch);
    if(c<0x80) utf8.push_back(char(c));
    else {
      utf8.push_back(char(0xc0|(c>>6)));
      utf8.push_back(char(0x80|(c&0x3f)));
      }
    }
  require(utf8.size()<=std::numeric_limits<rapidjson::SizeType>::max(),"string too long");
  return Json(utf8.data(),rapidjson::SizeType(utf8.size()),w.alloc);
  }
void put(Encoder& w, Json& obj, const char* name, Json value) {
  obj.AddMember(rapidjson::StringRef(name),value,w.alloc);
  }
std::string jsonText(const Json& value) {
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  require(value.Accept(writer),"cannot encode JSON");
  return std::string(buffer.GetString(),buffer.GetSize());
  }

struct Decoder {
  rapidjson::Document doc;
  std::vector<uint8_t> bin;

  uint16_t u16(size_t& at) const {
    require(at<=bin.size() && bin.size()-at>=2,"short binary integer");
    const auto value=uint16_t(uint16_t(bin[at])|uint16_t(uint16_t(bin[at+1])<<8));
    at+=2;
    return value;
    }
  uint32_t u32(size_t& at) const {
    require(at<=bin.size() && bin.size()-at>=4,"short binary integer");
    uint32_t value=0;
    for(unsigned i=0;i<4;++i) value|=uint32_t(bin[at++])<<(8*i);
    return value;
    }
  float f32(size_t& at) const { return std::bit_cast<float>(u32(at)); }
  Vec3 vec3(size_t& at) const { Vec3 v; v.x=f32(at); v.y=f32(at); v.z=f32(at); return v; }
  size_t block(const Json& value, size_t stride, size_t& count) const {
    size_t at=sizeValue(member(value,"offset"));
    count=sizeValue(member(value,"count"));
    require(at<=bin.size() && count<=(bin.size()-at)/stride,"binary array out of range");
    return at;
    }
  };

Json block(Encoder& w, size_t count) {
  Json value(rapidjson::kObjectType);
  put(w,value,"offset",Json(uint64_t(w.bin.size())));
  put(w,value,"count",Json(uint64_t(count)));
  return value;
  }
Json vec3s(Encoder& w, const std::vector<Vec3>& values) {
  auto value=block(w,values.size());
  for(const auto& v:values) w.vec3(v);
  return value;
  }
std::vector<Vec3> vec3s(const Decoder& r, const Json& value) {
  size_t count;
  auto at=r.block(value,12,count);
  std::vector<Vec3> out(count);
  for(auto& v:out) v=r.vec3(at);
  return out;
  }
Json indices(Encoder& w, const std::vector<uint32_t>& values) {
  auto value=block(w,values.size());
  for(auto v:values) w.u32(v);
  return value;
  }
std::vector<uint32_t> indices(const Decoder& r, const Json& value) {
  size_t count;
  auto at=r.block(value,4,count);
  std::vector<uint32_t> out(count);
  for(auto& v:out) v=r.u32(at);
  return out;
  }
Json samples(Encoder& w, const std::vector<AnimationSample>& values) {
  auto value=block(w,values.size());
  put(w,value,"encoding",text(w,"float32"));
  for(const auto& v:values) {
    w.vec3(v.position);
    w.f32(v.rotation.w); w.f32(v.rotation.x); w.f32(v.rotation.y); w.f32(v.rotation.z);
    }
  return value;
  }
Json samples(Encoder& w, const std::vector<QuantizedAnimationSample>& values) {
  auto value=block(w,values.size());
  put(w,value,"encoding",text(w,"quantized16"));
  for(const auto& v:values) {
    for(auto code:v.position) w.u16(code);
    for(auto code:v.rotation) w.u16(code);
    }
  return value;
  }
Vec3 samplePosition(const QuantizedAnimationSample& sample, float scale, float minimum) {
  Vec3 v {};
  v.x = static_cast<float>(sample.position[0]) * scale + minimum;
  v.y = static_cast<float>(sample.position[1]) * scale + minimum;
  v.z = static_cast<float>(sample.position[2]) * scale + minimum;
  return v;
  }
Quat sampleRotation(const QuantizedAnimationSample& sample) {
  constexpr float range = static_cast<float>(1 << 16) - 1.0f;
  constexpr float scale = (1.0f / range) * 2.1f;
  constexpr uint16_t mid = (1 << 15) - 1;
  Quat v {};
  v.x = (static_cast<float>(sample.rotation[0]) - mid) * scale;
  v.y = (static_cast<float>(sample.rotation[1]) - mid) * scale;
  v.z = (static_cast<float>(sample.rotation[2]) - mid) * scale;

  float len_q = v.x * v.x + v.y * v.y + v.z * v.z;
  if(len_q > 1.0f) {
    float l = 1.0f / sqrtf(len_q);
    v.x *= l;
    v.y *= l;
    v.z *= l;
    v.w = 0;
    }
  else {
    v.w = sqrtf(1.0f - len_q);
    }
  return v;
  }
std::vector<AnimationSample> samples(const Decoder& r, const Json& value, float scale, float minimum) {
  const auto encoding=text(member(value,"encoding"));
  require(encoding=="float32" || encoding=="quantized16","unsupported animation sample encoding");
  size_t count;
  auto at=r.block(value,encoding=="quantized16" ? 12 : 28,count);
  std::vector<AnimationSample> out(count);
  for(auto& v:out) {
    if(encoding=="quantized16") {
      QuantizedAnimationSample sample;
      for(auto& code:sample.position) code=r.u16(at);
      for(auto& code:sample.rotation) code=r.u16(at);
      v.rotation=sampleRotation(sample);
      v.position=samplePosition(sample,scale,minimum);
      }
    else {
      v.position=r.vec3(at);
      v.rotation.w=r.f32(at); v.rotation.x=r.f32(at); v.rotation.y=r.f32(at); v.rotation.z=r.f32(at);
      }
    }
  return out;
  }
Json wedges(Encoder& w, const std::vector<MeshWedge>& values) {
  auto value=block(w,values.size());
  for(const auto& v:values) {
    w.vec3(v.normal); w.f32(v.texture.x); w.f32(v.texture.y); w.u16(v.index);
    }
  return value;
  }
std::vector<MeshWedge> wedges(const Decoder& r, const Json& value) {
  size_t count;
  auto at=r.block(value,22,count);
  std::vector<MeshWedge> out(count);
  for(auto& v:out) {
    v.normal=r.vec3(at); v.texture.x=r.f32(at); v.texture.y=r.f32(at); v.index=r.u16(at);
    }
  return out;
  }
Json triangles(Encoder& w, const std::vector<MeshTriangle>& values) {
  auto value=block(w,values.size());
  for(const auto& v:values)
    for(auto i:v.wedges) w.u16(i);
  return value;
  }
std::vector<MeshTriangle> triangles(const Decoder& r, const Json& value) {
  size_t count;
  auto at=r.block(value,6,count);
  std::vector<MeshTriangle> out(count);
  for(auto& v:out)
    for(auto& i:v.wedges) i=r.u16(at);
  return out;
  }
Json weights(Encoder& w, const std::vector<std::vector<SkinWeight>>& values) {
  auto value=block(w,values.size());
  const auto start=w.bin.size();
  for(const auto& vertex:values) {
    require(vertex.size()<=std::numeric_limits<uint32_t>::max(),"too many skin weights");
    w.u32(uint32_t(vertex.size()));
    for(const auto& v:vertex) {
      w.f32(v.weight); w.vec3(v.position); w.bin.push_back(v.node_index);
      }
    }
  put(w,value,"byte_length",Json(uint64_t(w.bin.size()-start)));
  return value;
  }
std::vector<std::vector<SkinWeight>> weights(const Decoder& r, const Json& value) {
  size_t count;
  auto at=r.block(value,4,count);
  const auto bytes=sizeValue(member(value,"byte_length"));
  require(bytes<=r.bin.size()-at && count<=bytes/4,"skin weight array out of range");
  const auto end=at+bytes;
  std::vector<std::vector<SkinWeight>> out(count);
  for(auto& vertex:out) {
    require(at<=end && end-at>=4,"short skin weight count");
    const auto n=r.u32(at);
    require(n<=(end-at)/17,"skin weights out of range");
    vertex.resize(n);
    for(auto& v:vertex) {
      v.weight=r.f32(at); v.position=r.vec3(at); v.node_index=r.bin[at++];
      }
    }
  require(at==end,"skin weight byte length mismatch");
  return out;
  }

Json encode(Encoder& w, const Vec2& v) {
  Json out(rapidjson::kArrayType); out.PushBack(number(v.x),w.alloc); out.PushBack(number(v.y),w.alloc); return out;
  }
void decode(Decoder&, const Json& v, Vec2& out) { array(v,2); out={real(v[0]),real(v[1])}; }
Json encode(Encoder& w, const Vec3& v) {
  Json out(rapidjson::kArrayType); out.PushBack(number(v.x),w.alloc); out.PushBack(number(v.y),w.alloc); out.PushBack(number(v.z),w.alloc); return out;
  }
void decode(Decoder&, const Json& v, Vec3& out) { array(v,3); out={real(v[0]),real(v[1]),real(v[2])}; }
Json encode(Encoder& w, const Color& v) {
  Json out(rapidjson::kArrayType); for(auto c:{v.r,v.g,v.b,v.a}) out.PushBack(Json(unsigned(c)),w.alloc); return out;
  }
void decode(Decoder&, const Json& v, Color& out) {
  array(v,4); for(unsigned i=0;i<4;++i) out[i]=integer<uint8_t>(v[i]);
  }
Json encode(Encoder& w, const Mat4& v) {
  Json out(rapidjson::kArrayType);
  for(unsigned col=0;col<4;++col) for(unsigned row=0;row<4;++row) out.PushBack(number(v[col][row]),w.alloc);
  return out;
  }
void decode(Decoder&, const Json& v, Mat4& out) {
  array(v,16); for(unsigned col=0;col<4;++col) for(unsigned row=0;row<4;++row) out[col][row]=real(v[col*4+row]);
  }
Json encode(Encoder& w, const AxisAlignedBoundingBox& v) {
  Json out(rapidjson::kArrayType); out.PushBack(encode(w,v.min),w.alloc); out.PushBack(encode(w,v.max),w.alloc); return out;
  }
void decode(Decoder& r, const Json& v, AxisAlignedBoundingBox& out) {
  array(v,2); decode(r,v[0],out.min); decode(r,v[1],out.max);
  }
Json encode(Encoder& w, const OrientedBoundingBox& v) {
  Json out(rapidjson::kObjectType);
  put(w,out,"center",encode(w,v.center));
  Json axes(rapidjson::kArrayType); for(const auto& axis:v.axes) axes.PushBack(encode(w,axis),w.alloc);
  put(w,out,"axes",std::move(axes));
  put(w,out,"half_width",encode(w,v.half_width));
  Json children(rapidjson::kArrayType); for(const auto& child:v.children) children.PushBack(encode(w,child),w.alloc);
  put(w,out,"children",std::move(children));
  return out;
  }
void decode(Decoder& r, const Json& v, OrientedBoundingBox& out) {
  decode(r,member(v,"center"),out.center);
  const auto& axes=member(v,"axes"); array(axes,3);
  for(unsigned i=0;i<3;++i) decode(r,axes[i],out.axes[i]);
  decode(r,member(v,"half_width"),out.half_width);
  const auto& children=member(v,"children"); array(children); out.children.resize(children.Size());
  for(rapidjson::SizeType i=0;i<children.Size();++i) decode(r,children[i],out.children[i]);
  }
Json encode(Encoder& w, const ModelDate& v) {
  Json out(rapidjson::kObjectType);
  put(w,out,"year",Json(int64_t(v.year)));
  put(w,out,"month",Json(int64_t(v.month)));
  put(w,out,"day",Json(int64_t(v.day)));
  put(w,out,"hour",Json(int64_t(v.hour)));
  put(w,out,"minute",Json(int64_t(v.minute)));
  put(w,out,"second",Json(int64_t(v.second)));
  return out;
  }
void decode(Decoder& r, const Json& v, ModelDate& out) {
  (void)r;
  out.year=integer<uint32_t>(member(v,"year"));
  out.month=integer<uint16_t>(member(v,"month"));
  out.day=integer<uint16_t>(member(v,"day"));
  out.hour=integer<uint16_t>(member(v,"hour"));
  out.minute=integer<uint16_t>(member(v,"minute"));
  out.second=integer<uint16_t>(member(v,"second"));
  }

Json encode(Encoder& w, const ModelHierarchyNode& v) {
  Json out(rapidjson::kObjectType);
  put(w,out,"parent_index",Json(int64_t(v.parent_index)));
  put(w,out,"name",text(w,v.name));
  put(w,out,"transform",encode(w,v.transform));
  return out;
  }
void decode(Decoder& r, const Json& v, ModelHierarchyNode& out) {
  out.parent_index=integer<int16_t>(member(v,"parent_index"));
  out.name=text(member(v,"name"));
  decode(r,member(v,"transform"),out.transform);
  }

Json encode(Encoder& w, const ModelHierarchy& v) {
  Json out(rapidjson::kObjectType);
  {
  Json values(rapidjson::kArrayType);
  for(const auto& item:v.nodes) values.PushBack(encode(w,item),w.alloc);
  put(w,out,"nodes",std::move(values));
  }
  put(w,out,"bbox",encode(w,v.bbox));
  put(w,out,"collision_bbox",encode(w,v.collision_bbox));
  put(w,out,"root_translation",encode(w,v.root_translation));
  put(w,out,"checksum",Json(int64_t(v.checksum)));
  put(w,out,"source_date",encode(w,v.source_date));
  put(w,out,"source_path",text(w,v.source_path));
  return out;
  }
void decode(Decoder& r, const Json& v, ModelHierarchy& out) {
  {
  const auto& values=member(v,"nodes");
  array(values);
  out.nodes.resize(values.Size());
  for(rapidjson::SizeType i=0;i<values.Size();++i) {
    decode(r,values[i],out.nodes[i]);
    }
  }
  decode(r,member(v,"bbox"),out.bbox);
  decode(r,member(v,"collision_bbox"),out.collision_bbox);
  decode(r,member(v,"root_translation"),out.root_translation);
  out.checksum=integer<uint32_t>(member(v,"checksum"));
  decode(r,member(v,"source_date"),out.source_date);
  out.source_path=text(member(v,"source_path"));
  }

Json encode(Encoder& w, const Material& v) {
  Json out(rapidjson::kObjectType);
  put(w,out,"name",text(w,v.name));
  put(w,out,"group",Json(int64_t(v.group)));
  put(w,out,"color",encode(w,v.color));
  put(w,out,"smooth_angle",number(v.smooth_angle));
  put(w,out,"texture",text(w,v.texture));
  put(w,out,"texture_scale",encode(w,v.texture_scale));
  put(w,out,"texture_anim_fps",number(v.texture_anim_fps));
  put(w,out,"texture_anim_map_mode",Json(int64_t(v.texture_anim_map_mode)));
  put(w,out,"texture_anim_map_dir",encode(w,v.texture_anim_map_dir));
  put(w,out,"disable_collision",Json(v.disable_collision));
  put(w,out,"disable_lightmap",Json(v.disable_lightmap));
  put(w,out,"dont_collapse",Json(v.dont_collapse));
  put(w,out,"detail_object",text(w,v.detail_object));
  put(w,out,"detail_object_scale",number(v.detail_object_scale));
  put(w,out,"force_occluder",Json(v.force_occluder));
  put(w,out,"environment_mapping",Json(v.environment_mapping));
  put(w,out,"environment_mapping_strength",number(v.environment_mapping_strength));
  put(w,out,"wave_mode",Json(int64_t(v.wave_mode)));
  put(w,out,"wave_speed",Json(int64_t(v.wave_speed)));
  put(w,out,"wave_max_amplitude",number(v.wave_max_amplitude));
  put(w,out,"wave_grid_size",number(v.wave_grid_size));
  put(w,out,"ignore_sun",Json(v.ignore_sun));
  put(w,out,"alpha_func",Json(int64_t(v.alpha_func)));
  put(w,out,"default_mapping",encode(w,v.default_mapping));
  return out;
  }
void decode(Decoder& r, const Json& v, Material& out) {
  out.name=text(member(v,"name"));
  out.group=enumeration<MaterialGroup>(member(v,"group"));
  decode(r,member(v,"color"),out.color);
  out.smooth_angle=real(member(v,"smooth_angle"));
  out.texture=text(member(v,"texture"));
  decode(r,member(v,"texture_scale"),out.texture_scale);
  out.texture_anim_fps=real(member(v,"texture_anim_fps"));
  out.texture_anim_map_mode=enumeration<AnimationMapping>(member(v,"texture_anim_map_mode"));
  decode(r,member(v,"texture_anim_map_dir"),out.texture_anim_map_dir);
  out.disable_collision=boolean(member(v,"disable_collision"));
  out.disable_lightmap=boolean(member(v,"disable_lightmap"));
  out.dont_collapse=boolean(member(v,"dont_collapse"));
  out.detail_object=text(member(v,"detail_object"));
  out.detail_object_scale=real(member(v,"detail_object_scale"));
  out.force_occluder=boolean(member(v,"force_occluder"));
  out.environment_mapping=boolean(member(v,"environment_mapping"));
  out.environment_mapping_strength=real(member(v,"environment_mapping_strength"));
  out.wave_mode=enumeration<WaveMode>(member(v,"wave_mode"));
  out.wave_speed=enumeration<WaveSpeed>(member(v,"wave_speed"));
  out.wave_max_amplitude=real(member(v,"wave_max_amplitude"));
  out.wave_grid_size=real(member(v,"wave_grid_size"));
  out.ignore_sun=boolean(member(v,"ignore_sun"));
  out.alpha_func=enumeration<AlphaFunction>(member(v,"alpha_func"));
  decode(r,member(v,"default_mapping"),out.default_mapping);
  }

Json encode(Encoder& w, const SubMesh& v) {
  Json out(rapidjson::kObjectType);
  put(w,out,"mat",encode(w,v.mat));
  put(w,out,"triangles",triangles(w,v.triangles));
  put(w,out,"wedges",wedges(w,v.wedges));
  return out;
  }
void decode(Decoder& r, const Json& v, SubMesh& out) {
  decode(r,member(v,"mat"),out.mat);
  out.triangles=triangles(r,member(v,"triangles"));
  out.wedges=wedges(r,member(v,"wedges"));
  }

Json encode(Encoder& w, const StaticMesh& v) {
  Json out(rapidjson::kObjectType);
  put(w,out,"positions",vec3s(w,v.positions));
  put(w,out,"normals",vec3s(w,v.normals));
  {
  Json values(rapidjson::kArrayType);
  for(const auto& item:v.sub_meshes) values.PushBack(encode(w,item),w.alloc);
  put(w,out,"sub_meshes",std::move(values));
  }
  {
  Json values(rapidjson::kArrayType);
  for(const auto& item:v.materials) values.PushBack(encode(w,item),w.alloc);
  put(w,out,"materials",std::move(values));
  }
  put(w,out,"alpha_test",Json(int64_t(v.alpha_test)));
  put(w,out,"bbox",encode(w,v.bbox));
  put(w,out,"obbox",encode(w,v.obbox));
  return out;
  }
void decode(Decoder& r, const Json& v, StaticMesh& out) {
  out.positions=vec3s(r,member(v,"positions"));
  out.normals=vec3s(r,member(v,"normals"));
  {
  const auto& values=member(v,"sub_meshes");
  array(values);
  out.sub_meshes.resize(values.Size());
  for(rapidjson::SizeType i=0;i<values.Size();++i) {
    decode(r,values[i],out.sub_meshes[i]);
    }
  }
  {
  const auto& values=member(v,"materials");
  array(values);
  out.materials.resize(values.Size());
  for(rapidjson::SizeType i=0;i<values.Size();++i) {
    decode(r,values[i],out.materials[i]);
    }
  }
  out.alpha_test=integer<uint8_t>(member(v,"alpha_test"));
  decode(r,member(v,"bbox"),out.bbox);
  decode(r,member(v,"obbox"),out.obbox);
  }

Json encode(Encoder& w, const SoftSkinMesh& v) {
  Json out(rapidjson::kObjectType);
  put(w,out,"mesh",encode(w,v.mesh));
  {
  Json values(rapidjson::kArrayType);
  for(const auto& item:v.bboxes) values.PushBack(encode(w,item),w.alloc);
  put(w,out,"bboxes",std::move(values));
  }
  put(w,out,"weights",weights(w,v.weights));
  return out;
  }
void decode(Decoder& r, const Json& v, SoftSkinMesh& out) {
  decode(r,member(v,"mesh"),out.mesh);
  {
  const auto& values=member(v,"bboxes");
  array(values);
  out.bboxes.resize(values.Size());
  for(rapidjson::SizeType i=0;i<values.Size();++i) {
    decode(r,values[i],out.bboxes[i]);
    }
  }
  out.weights=weights(r,member(v,"weights"));
  }

Json encode(Encoder& w, const AnimationEvent& v) {
  Json out(rapidjson::kObjectType);
  put(w,out,"type",Json(int64_t(v.type)));
  put(w,out,"frame",Json(int64_t(v.frame)));
  put(w,out,"tag",text(w,v.tag));
  {
  Json values(rapidjson::kArrayType);
  for(const auto& item:v.content) values.PushBack(text(w,item),w.alloc);
  put(w,out,"content",std::move(values));
  }
  {
  Json values(rapidjson::kArrayType);
  for(const auto& item:v.values) values.PushBack(number(item),w.alloc);
  put(w,out,"values",std::move(values));
  }
  put(w,out,"probability",number(v.probability));
  return out;
  }
void decode(Decoder& r, const Json& v, AnimationEvent& out) {
  (void)r;
  out.type=enumeration<AnimationEventType>(member(v,"type"));
  out.frame=integer<uint32_t>(member(v,"frame"));
  out.tag=text(member(v,"tag"));
  {
  const auto& values=member(v,"content");
  array(values,4);
  for(rapidjson::SizeType i=0;i<values.Size();++i) {
    out.content[i]=text(values[i]);
    }
  }
  {
  const auto& values=member(v,"values");
  array(values,4);
  for(rapidjson::SizeType i=0;i<values.Size();++i) {
    out.values[i]=real(values[i]);
    }
  }
  out.probability=real(member(v,"probability"));
  }

Json encode(Encoder& w, const ModelAnimation& v, const std::vector<QuantizedAnimationSample>* encoded=nullptr) {
  Json out(rapidjson::kObjectType);
  put(w,out,"name",text(w,v.name));
  put(w,out,"next",text(w,v.next));
  put(w,out,"layer",Json(int64_t(v.layer)));
  put(w,out,"frame_count",Json(int64_t(v.frame_count)));
  put(w,out,"node_count",Json(int64_t(v.node_count)));
  put(w,out,"fps",number(v.fps));
  put(w,out,"fps_source",number(v.fps_source));
  put(w,out,"sample_position_min",number(v.sample_position_min));
  put(w,out,"sample_position_scale",number(v.sample_position_scale));
  put(w,out,"bbox",encode(w,v.bbox));
  put(w,out,"checksum",Json(int64_t(v.checksum)));
  put(w,out,"source_path",text(w,v.source_path));
  put(w,out,"source_date",encode(w,v.source_date));
  put(w,out,"source_script",text(w,v.source_script));
  put(w,out,"samples",encoded!=nullptr ? samples(w,*encoded) : samples(w,v.samples));
  {
  Json values(rapidjson::kArrayType);
  for(const auto& item:v.events) values.PushBack(encode(w,item),w.alloc);
  put(w,out,"events",std::move(values));
  }
  put(w,out,"node_indices",indices(w,v.node_indices));
  return out;
  }
void decode(Decoder& r, const Json& v, ModelAnimation& out) {
  out.name=text(member(v,"name"));
  out.next=text(member(v,"next"));
  out.layer=integer<uint32_t>(member(v,"layer"));
  out.frame_count=integer<uint32_t>(member(v,"frame_count"));
  out.node_count=integer<uint32_t>(member(v,"node_count"));
  out.fps=real(member(v,"fps"));
  out.fps_source=real(member(v,"fps_source"));
  out.sample_position_min=real(member(v,"sample_position_min"));
  out.sample_position_scale=real(member(v,"sample_position_scale"));
  decode(r,member(v,"bbox"),out.bbox);
  out.checksum=integer<uint32_t>(member(v,"checksum"));
  out.source_path=text(member(v,"source_path"));
  decode(r,member(v,"source_date"),out.source_date);
  out.source_script=text(member(v,"source_script"));
  out.samples=samples(r,member(v,"samples"),out.sample_position_scale,out.sample_position_min);
  {
  const auto& values=member(v,"events");
  array(values);
  out.events.resize(values.Size());
  for(rapidjson::SizeType i=0;i<values.Size();++i) {
    decode(r,values[i],out.events[i]);
    }
  }
  out.node_indices=indices(r,member(v,"node_indices"));
  }

Json encode(Encoder& w, const MdsSkeleton& v) {
  Json out(rapidjson::kObjectType);
  put(w,out,"name",text(w,v.name));
  put(w,out,"disable_mesh",Json(v.disable_mesh));
  return out;
  }
void decode(Decoder& r, const Json& v, MdsSkeleton& out) {
  (void)r;
  out.name=text(member(v,"name"));
  out.disable_mesh=boolean(member(v,"disable_mesh"));
  }

Json encode(Encoder& w, const MdsModelTag& v) {
  Json out(rapidjson::kObjectType);
  put(w,out,"bone",text(w,v.bone));
  return out;
  }
void decode(Decoder& r, const Json& v, MdsModelTag& out) {
  (void)r;
  out.bone=text(member(v,"bone"));
  }

Json encode(Encoder& w, const MdsEventTag& v) {
  Json out(rapidjson::kObjectType);
  put(w,out,"frame",Json(int64_t(v.frame)));
  put(w,out,"type",Json(int64_t(v.type)));
  put(w,out,"slot",text(w,v.slot));
  put(w,out,"slot2",text(w,v.slot2));
  put(w,out,"item",text(w,v.item));
  {
  Json values(rapidjson::kArrayType);
  for(const auto& item:v.frames) values.PushBack(Json(int64_t(item)),w.alloc);
  put(w,out,"frames",std::move(values));
  }
  put(w,out,"fight_mode",Json(int64_t(v.fight_mode)));
  put(w,out,"attached",Json(v.attached));
  return out;
  }
void decode(Decoder& r, const Json& v, MdsEventTag& out) {
  (void)r;
  out.frame=integer<int32_t>(member(v,"frame"));
  out.type=enumeration<MdsEventType>(member(v,"type"));
  out.slot=text(member(v,"slot"));
  out.slot2=text(member(v,"slot2"));
  out.item=text(member(v,"item"));
  {
  const auto& values=member(v,"frames");
  array(values);
  out.frames.resize(values.Size());
  for(rapidjson::SizeType i=0;i<values.Size();++i) {
    out.frames[i]=integer<int32_t>(values[i]);
    }
  }
  out.fight_mode=enumeration<MdsFightMode>(member(v,"fight_mode"));
  out.attached=boolean(member(v,"attached"));
  }

Json encode(Encoder& w, const MdsParticleEffect& v) {
  Json out(rapidjson::kObjectType);
  put(w,out,"frame",Json(int64_t(v.frame)));
  put(w,out,"index",Json(int64_t(v.index)));
  put(w,out,"name",text(w,v.name));
  put(w,out,"position",text(w,v.position));
  put(w,out,"attached",Json(v.attached));
  return out;
  }
void decode(Decoder& r, const Json& v, MdsParticleEffect& out) {
  (void)r;
  out.frame=integer<int32_t>(member(v,"frame"));
  out.index=integer<int32_t>(member(v,"index"));
  out.name=text(member(v,"name"));
  out.position=text(member(v,"position"));
  out.attached=boolean(member(v,"attached"));
  }

Json encode(Encoder& w, const MdsCameraTremor& v) {
  Json out(rapidjson::kObjectType);
  put(w,out,"frame",Json(int64_t(v.frame)));
  put(w,out,"field1",Json(int64_t(v.field1)));
  put(w,out,"field2",Json(int64_t(v.field2)));
  put(w,out,"field3",Json(int64_t(v.field3)));
  put(w,out,"field4",Json(int64_t(v.field4)));
  return out;
  }
void decode(Decoder& r, const Json& v, MdsCameraTremor& out) {
  (void)r;
  out.frame=integer<int32_t>(member(v,"frame"));
  out.field1=integer<int32_t>(member(v,"field1"));
  out.field2=integer<int32_t>(member(v,"field2"));
  out.field3=integer<int32_t>(member(v,"field3"));
  out.field4=integer<int32_t>(member(v,"field4"));
  }

Json encode(Encoder& w, const MdsParticleEffectStop& v) {
  Json out(rapidjson::kObjectType);
  put(w,out,"frame",Json(int64_t(v.frame)));
  put(w,out,"index",Json(int64_t(v.index)));
  return out;
  }
void decode(Decoder& r, const Json& v, MdsParticleEffectStop& out) {
  (void)r;
  out.frame=integer<int32_t>(member(v,"frame"));
  out.index=integer<int32_t>(member(v,"index"));
  }

Json encode(Encoder& w, const MdsSoundEffect& v) {
  Json out(rapidjson::kObjectType);
  put(w,out,"frame",Json(int64_t(v.frame)));
  put(w,out,"name",text(w,v.name));
  put(w,out,"range",number(v.range));
  put(w,out,"empty_slot",Json(v.empty_slot));
  return out;
  }
void decode(Decoder& r, const Json& v, MdsSoundEffect& out) {
  (void)r;
  out.frame=integer<int32_t>(member(v,"frame"));
  out.name=text(member(v,"name"));
  out.range=real(member(v,"range"));
  out.empty_slot=boolean(member(v,"empty_slot"));
  }

Json encode(Encoder& w, const MdsSoundEffectGround& v) {
  Json out(rapidjson::kObjectType);
  put(w,out,"frame",Json(int64_t(v.frame)));
  put(w,out,"name",text(w,v.name));
  put(w,out,"range",number(v.range));
  put(w,out,"empty_slot",Json(v.empty_slot));
  return out;
  }
void decode(Decoder& r, const Json& v, MdsSoundEffectGround& out) {
  (void)r;
  out.frame=integer<int32_t>(member(v,"frame"));
  out.name=text(member(v,"name"));
  out.range=real(member(v,"range"));
  out.empty_slot=boolean(member(v,"empty_slot"));
  }

Json encode(Encoder& w, const MdsMorphAnimation& v) {
  Json out(rapidjson::kObjectType);
  put(w,out,"frame",Json(int64_t(v.frame)));
  put(w,out,"animation",text(w,v.animation));
  put(w,out,"node",text(w,v.node));
  return out;
  }
void decode(Decoder& r, const Json& v, MdsMorphAnimation& out) {
  (void)r;
  out.frame=integer<int32_t>(member(v,"frame"));
  out.animation=text(member(v,"animation"));
  out.node=text(member(v,"node"));
  }

Json encode(Encoder& w, const MdsAnimation& v) {
  Json out(rapidjson::kObjectType);
  put(w,out,"name",text(w,v.name));
  put(w,out,"layer",Json(int64_t(v.layer)));
  put(w,out,"next",text(w,v.next));
  put(w,out,"blend_in",number(v.blend_in));
  put(w,out,"blend_out",number(v.blend_out));
  put(w,out,"flags",Json(int64_t(v.flags)));
  put(w,out,"model",text(w,v.model));
  put(w,out,"direction",Json(int64_t(v.direction)));
  put(w,out,"first_frame",Json(int64_t(v.first_frame)));
  put(w,out,"last_frame",Json(int64_t(v.last_frame)));
  put(w,out,"fps",number(v.fps));
  put(w,out,"speed",number(v.speed));
  put(w,out,"collision_volume_scale",number(v.collision_volume_scale));
  {
  Json values(rapidjson::kArrayType);
  for(const auto& item:v.events) values.PushBack(encode(w,item),w.alloc);
  put(w,out,"events",std::move(values));
  }
  {
  Json values(rapidjson::kArrayType);
  for(const auto& item:v.pfx) values.PushBack(encode(w,item),w.alloc);
  put(w,out,"pfx",std::move(values));
  }
  {
  Json values(rapidjson::kArrayType);
  for(const auto& item:v.pfx_stop) values.PushBack(encode(w,item),w.alloc);
  put(w,out,"pfx_stop",std::move(values));
  }
  {
  Json values(rapidjson::kArrayType);
  for(const auto& item:v.sfx) values.PushBack(encode(w,item),w.alloc);
  put(w,out,"sfx",std::move(values));
  }
  {
  Json values(rapidjson::kArrayType);
  for(const auto& item:v.sfx_ground) values.PushBack(encode(w,item),w.alloc);
  put(w,out,"sfx_ground",std::move(values));
  }
  {
  Json values(rapidjson::kArrayType);
  for(const auto& item:v.morph) values.PushBack(encode(w,item),w.alloc);
  put(w,out,"morph",std::move(values));
  }
  {
  Json values(rapidjson::kArrayType);
  for(const auto& item:v.tremors) values.PushBack(encode(w,item),w.alloc);
  put(w,out,"tremors",std::move(values));
  }
  return out;
  }
void decode(Decoder& r, const Json& v, MdsAnimation& out) {
  out.name=text(member(v,"name"));
  out.layer=integer<uint32_t>(member(v,"layer"));
  out.next=text(member(v,"next"));
  out.blend_in=real(member(v,"blend_in"));
  out.blend_out=real(member(v,"blend_out"));
  out.flags=enumeration<AnimationFlags>(member(v,"flags"));
  out.model=text(member(v,"model"));
  out.direction=enumeration<AnimationDirection>(member(v,"direction"));
  out.first_frame=integer<int32_t>(member(v,"first_frame"));
  out.last_frame=integer<int32_t>(member(v,"last_frame"));
  out.fps=real(member(v,"fps"));
  out.speed=real(member(v,"speed"));
  out.collision_volume_scale=real(member(v,"collision_volume_scale"));
  {
  const auto& values=member(v,"events");
  array(values);
  out.events.resize(values.Size());
  for(rapidjson::SizeType i=0;i<values.Size();++i) {
    decode(r,values[i],out.events[i]);
    }
  }
  {
  const auto& values=member(v,"pfx");
  array(values);
  out.pfx.resize(values.Size());
  for(rapidjson::SizeType i=0;i<values.Size();++i) {
    decode(r,values[i],out.pfx[i]);
    }
  }
  {
  const auto& values=member(v,"pfx_stop");
  array(values);
  out.pfx_stop.resize(values.Size());
  for(rapidjson::SizeType i=0;i<values.Size();++i) {
    decode(r,values[i],out.pfx_stop[i]);
    }
  }
  {
  const auto& values=member(v,"sfx");
  array(values);
  out.sfx.resize(values.Size());
  for(rapidjson::SizeType i=0;i<values.Size();++i) {
    decode(r,values[i],out.sfx[i]);
    }
  }
  {
  const auto& values=member(v,"sfx_ground");
  array(values);
  out.sfx_ground.resize(values.Size());
  for(rapidjson::SizeType i=0;i<values.Size();++i) {
    decode(r,values[i],out.sfx_ground[i]);
    }
  }
  {
  const auto& values=member(v,"morph");
  array(values);
  out.morph.resize(values.Size());
  for(rapidjson::SizeType i=0;i<values.Size();++i) {
    decode(r,values[i],out.morph[i]);
    }
  }
  {
  const auto& values=member(v,"tremors");
  array(values);
  out.tremors.resize(values.Size());
  for(rapidjson::SizeType i=0;i<values.Size();++i) {
    decode(r,values[i],out.tremors[i]);
    }
  }
  }

Json encode(Encoder& w, const MdsAnimationAlias& v) {
  Json out(rapidjson::kObjectType);
  put(w,out,"name",text(w,v.name));
  put(w,out,"layer",Json(int64_t(v.layer)));
  put(w,out,"next",text(w,v.next));
  put(w,out,"blend_in",number(v.blend_in));
  put(w,out,"blend_out",number(v.blend_out));
  put(w,out,"flags",Json(int64_t(v.flags)));
  put(w,out,"alias",text(w,v.alias));
  put(w,out,"direction",Json(int64_t(v.direction)));
  return out;
  }
void decode(Decoder& r, const Json& v, MdsAnimationAlias& out) {
  (void)r;
  out.name=text(member(v,"name"));
  out.layer=integer<uint32_t>(member(v,"layer"));
  out.next=text(member(v,"next"));
  out.blend_in=real(member(v,"blend_in"));
  out.blend_out=real(member(v,"blend_out"));
  out.flags=enumeration<AnimationFlags>(member(v,"flags"));
  out.alias=text(member(v,"alias"));
  out.direction=enumeration<AnimationDirection>(member(v,"direction"));
  }

Json encode(Encoder& w, const MdsAnimationBlend& v) {
  Json out(rapidjson::kObjectType);
  put(w,out,"name",text(w,v.name));
  put(w,out,"next",text(w,v.next));
  put(w,out,"blend_in",number(v.blend_in));
  put(w,out,"blend_out",number(v.blend_out));
  return out;
  }
void decode(Decoder& r, const Json& v, MdsAnimationBlend& out) {
  (void)r;
  out.name=text(member(v,"name"));
  out.next=text(member(v,"next"));
  out.blend_in=real(member(v,"blend_in"));
  out.blend_out=real(member(v,"blend_out"));
  }

Json encode(Encoder& w, const MdsAnimationCombine& v) {
  Json out(rapidjson::kObjectType);
  put(w,out,"name",text(w,v.name));
  put(w,out,"layer",Json(int64_t(v.layer)));
  put(w,out,"next",text(w,v.next));
  put(w,out,"blend_in",number(v.blend_in));
  put(w,out,"blend_out",number(v.blend_out));
  put(w,out,"flags",Json(int64_t(v.flags)));
  put(w,out,"model",text(w,v.model));
  put(w,out,"last_frame",Json(int64_t(v.last_frame)));
  return out;
  }
void decode(Decoder& r, const Json& v, MdsAnimationCombine& out) {
  (void)r;
  out.name=text(member(v,"name"));
  out.layer=integer<uint32_t>(member(v,"layer"));
  out.next=text(member(v,"next"));
  out.blend_in=real(member(v,"blend_in"));
  out.blend_out=real(member(v,"blend_out"));
  out.flags=enumeration<AnimationFlags>(member(v,"flags"));
  out.model=text(member(v,"model"));
  out.last_frame=integer<int32_t>(member(v,"last_frame"));
  }

Json encode(Encoder& w, const ModelScript& v) {
  Json out(rapidjson::kObjectType);
  put(w,out,"skeleton",encode(w,v.skeleton));
  {
  Json values(rapidjson::kArrayType);
  for(const auto& item:v.meshes) values.PushBack(text(w,item),w.alloc);
  put(w,out,"meshes",std::move(values));
  }
  {
  Json values(rapidjson::kArrayType);
  for(const auto& item:v.disabled_animations) values.PushBack(text(w,item),w.alloc);
  put(w,out,"disabled_animations",std::move(values));
  }
  {
  Json values(rapidjson::kArrayType);
  for(const auto& item:v.combinations) values.PushBack(encode(w,item),w.alloc);
  put(w,out,"combinations",std::move(values));
  }
  {
  Json values(rapidjson::kArrayType);
  for(const auto& item:v.blends) values.PushBack(encode(w,item),w.alloc);
  put(w,out,"blends",std::move(values));
  }
  {
  Json values(rapidjson::kArrayType);
  for(const auto& item:v.aliases) values.PushBack(encode(w,item),w.alloc);
  put(w,out,"aliases",std::move(values));
  }
  {
  Json values(rapidjson::kArrayType);
  for(const auto& item:v.model_tags) values.PushBack(encode(w,item),w.alloc);
  put(w,out,"model_tags",std::move(values));
  }
  {
  Json values(rapidjson::kArrayType);
  for(const auto& item:v.animations) values.PushBack(encode(w,item),w.alloc);
  put(w,out,"animations",std::move(values));
  }
  return out;
  }
void decode(Decoder& r, const Json& v, ModelScript& out) {
  decode(r,member(v,"skeleton"),out.skeleton);
  {
  const auto& values=member(v,"meshes");
  array(values);
  out.meshes.resize(values.Size());
  for(rapidjson::SizeType i=0;i<values.Size();++i) {
    out.meshes[i]=text(values[i]);
    }
  }
  {
  const auto& values=member(v,"disabled_animations");
  array(values);
  out.disabled_animations.resize(values.Size());
  for(rapidjson::SizeType i=0;i<values.Size();++i) {
    out.disabled_animations[i]=text(values[i]);
    }
  }
  {
  const auto& values=member(v,"combinations");
  array(values);
  out.combinations.resize(values.Size());
  for(rapidjson::SizeType i=0;i<values.Size();++i) {
    decode(r,values[i],out.combinations[i]);
    }
  }
  {
  const auto& values=member(v,"blends");
  array(values);
  out.blends.resize(values.Size());
  for(rapidjson::SizeType i=0;i<values.Size();++i) {
    decode(r,values[i],out.blends[i]);
    }
  }
  {
  const auto& values=member(v,"aliases");
  array(values);
  out.aliases.resize(values.Size());
  for(rapidjson::SizeType i=0;i<values.Size();++i) {
    decode(r,values[i],out.aliases[i]);
    }
  }
  {
  const auto& values=member(v,"model_tags");
  array(values);
  out.model_tags.resize(values.Size());
  for(rapidjson::SizeType i=0;i<values.Size();++i) {
    decode(r,values[i],out.model_tags[i]);
    }
  }
  {
  const auto& values=member(v,"animations");
  array(values);
  out.animations.resize(values.Size());
  for(rapidjson::SizeType i=0;i<values.Size();++i) {
    decode(r,values[i],out.animations[i]);
    }
  }
  }

Json encode(Encoder& w, const MorphAnimation& v) {
  Json out(rapidjson::kObjectType);
  put(w,out,"name",text(w,v.name));
  put(w,out,"layer",Json(int64_t(v.layer)));
  put(w,out,"blend_in",number(v.blend_in));
  put(w,out,"blend_out",number(v.blend_out));
  put(w,out,"duration",number(v.duration));
  put(w,out,"speed",number(v.speed));
  put(w,out,"flags",Json(int64_t(v.flags)));
  put(w,out,"frame_count",Json(int64_t(v.frame_count)));
  put(w,out,"vertices",indices(w,v.vertices));
  put(w,out,"samples",vec3s(w,v.samples));
  return out;
  }
void decode(Decoder& r, const Json& v, MorphAnimation& out) {
  out.name=text(member(v,"name"));
  out.layer=integer<int32_t>(member(v,"layer"));
  out.blend_in=real(member(v,"blend_in"));
  out.blend_out=real(member(v,"blend_out"));
  out.duration=real(member(v,"duration"));
  out.speed=real(member(v,"speed"));
  out.flags=integer<uint8_t>(member(v,"flags"));
  out.frame_count=integer<uint32_t>(member(v,"frame_count"));
  out.vertices=indices(r,member(v,"vertices"));
  out.samples=vec3s(r,member(v,"samples"));
  }

Json encode(Encoder& w, const MorphSource& v) {
  Json out(rapidjson::kObjectType);
  put(w,out,"file_date",encode(w,v.file_date));
  put(w,out,"file_name",text(w,v.file_name));
  return out;
  }
void decode(Decoder& r, const Json& v, MorphSource& out) {
  decode(r,member(v,"file_date"),out.file_date);
  out.file_name=text(member(v,"file_name"));
  }

Json encode(Encoder& w, const MorphMesh& v) {
  Json out(rapidjson::kObjectType);
  put(w,out,"name",text(w,v.name));
  put(w,out,"mesh",encode(w,v.mesh));
  put(w,out,"morph_positions",vec3s(w,v.morph_positions));
  {
  Json values(rapidjson::kArrayType);
  for(const auto& item:v.animations) values.PushBack(encode(w,item),w.alloc);
  put(w,out,"animations",std::move(values));
  }
  {
  Json values(rapidjson::kArrayType);
  for(const auto& item:v.sources) values.PushBack(encode(w,item),w.alloc);
  put(w,out,"sources",std::move(values));
  }
  return out;
  }
void decode(Decoder& r, const Json& v, MorphMesh& out) {
  out.name=text(member(v,"name"));
  decode(r,member(v,"mesh"),out.mesh);
  out.morph_positions=vec3s(r,member(v,"morph_positions"));
  {
  const auto& values=member(v,"animations");
  array(values);
  out.animations.resize(values.Size());
  for(rapidjson::SizeType i=0;i<values.Size();++i) {
    decode(r,values[i],out.animations[i]);
    }
  }
  {
  const auto& values=member(v,"sources");
  array(values);
  out.sources.resize(values.Size());
  for(rapidjson::SizeType i=0;i<values.Size();++i) {
    decode(r,values[i],out.sources[i]);
    }
  }
  }

Json encode(Encoder& w, const ModelMesh& v) {
  Json out(rapidjson::kObjectType);
  Json meshes(rapidjson::kArrayType);
  for(const auto& mesh:v.meshes) meshes.PushBack(encode(w,mesh),w.alloc);
  put(w,out,"meshes",std::move(meshes));
  Json attachments(rapidjson::kArrayType);
  for(const auto& attachment:v.attachments) {
    Json item(rapidjson::kObjectType);
    put(w,item,"name",text(w,attachment.first));
    put(w,item,"mesh",encode(w,attachment.second));
    attachments.PushBack(item,w.alloc);
    }
  put(w,out,"attachments",std::move(attachments));
  return out;
  }
void decode(Decoder& r, const Json& v, ModelMesh& out) {
  const auto& meshes=member(v,"meshes"); array(meshes);
  out.meshes.resize(meshes.Size());
  for(rapidjson::SizeType i=0;i<meshes.Size();++i) decode(r,meshes[i],out.meshes[i]);
  const auto& attachments=member(v,"attachments"); array(attachments);
  out.attachments.resize(attachments.Size());
  for(rapidjson::SizeType i=0;i<attachments.Size();++i) {
    out.attachments[i].first=text(member(attachments[i],"name"));
    decode(r,member(attachments[i],"mesh"),out.attachments[i].second);
    }
  }
Json encode(Encoder& w, const Model& v) {
  Json out(rapidjson::kObjectType);
  put(w,out,"hierarchy",encode(w,v.hierarchy));
  put(w,out,"mesh",encode(w,v.mesh));
  return out;
  }
void decode(Decoder& r, const Json& v, Model& out) {
  decode(r,member(v,"hierarchy"),out.hierarchy);
  decode(r,member(v,"mesh"),out.mesh);
  }

void validateOrder(const ModelMesh& mesh, const std::vector<std::string>& order) {
  std::unordered_set<std::string> names;
  for(const auto& attachment:mesh.attachments)
    require(names.insert(attachment.first).second,"duplicate attachment name");
  if(order.empty()) return;
  require(order.size()==mesh.attachments.size(),"attachment order count mismatch");
  std::unordered_set<std::string> seen;
  for(const auto& name:order) {
    require(names.count(name)!=0,"attachment order names do not match mesh");
    require(seen.insert(name).second,"duplicate attachment order name");
    }
  }
void restoreOrder(ModelMesh& mesh, const std::vector<std::string>& order) {
  validateOrder(mesh,order);
  if(order.empty()) return;
  // Repeat the original map insertions so each platform keeps its native IDs.
  std::unordered_map<std::string,size_t> names;
  for(const auto& name:order) {
    const auto it=std::find_if(mesh.attachments.begin(),mesh.attachments.end(),[&](const auto& attachment) {
      return attachment.first==name;
      });
    names.emplace(name,size_t(it-mesh.attachments.begin()));
    }
  std::vector<std::pair<std::string,StaticMesh>> attachments;
  attachments.reserve(mesh.attachments.size());
  for(const auto& name:names) attachments.push_back(std::move(mesh.attachments[name.second]));
  mesh.attachments=std::move(attachments);
  }

fs::path binaryPath(fs::path file) {
  file.replace_extension(".bin");
  return file;
  }
void write(Encoder& w, Json data, const char* kind, const fs::path& file,
           const std::vector<std::string>* order=nullptr) {
  require(file!=binaryPath(file),"JSON and binary paths must differ");
  Json root(rapidjson::kObjectType);
  put(w,root,"format",text(w,"gothic-model"));
  put(w,root,"version",Json(1));
  put(w,root,"kind",text(w,kind));
  put(w,root,"buffer",text(w,binaryPath(file).filename().string()));
  put(w,root,"byte_length",Json(uint64_t(w.bin.size())));
  if(order!=nullptr) {
    Json names(rapidjson::kArrayType);
    for(const auto& name:*order) names.PushBack(text(w,name),w.alloc);
    put(w,root,"attachment_order",std::move(names));
    }
  put(w,root,"data",std::move(data));
  const auto json=jsonText(root)+"\n";
  gothicglb::writeFile(binaryPath(file),w.bin.data(),w.bin.size());
  gothicglb::writeFile(file,json.data(),json.size());
  }
const Json& read(Decoder& r, const fs::path& file, const char* kind) {
  const auto bytes=gothicglb::readFile(file);
  r.doc.Parse<rapidjson::kParseValidateEncodingFlag>(reinterpret_cast<const char*>(bytes.data()),bytes.size());
  if(r.doc.HasParseError()) fail(file.string()+": "+rapidjson::GetParseError_En(r.doc.GetParseError()));
  require(text(member(r.doc,"format"))=="gothic-model" && integer<int>(member(r.doc,"version"))==1,"unsupported format or version");
  require(text(member(r.doc,"kind"))==kind,"wrong record kind");
  require(file!=binaryPath(file),"JSON and binary paths must differ");
  require(text(member(r.doc,"buffer"))==binaryPath(file).filename().string(),"binary filename must be the fixed sibling");
  r.bin=gothicglb::readFile(binaryPath(file));
  require(sizeValue(member(r.doc,"byte_length"))==r.bin.size(),"binary byte length mismatch");
  return member(r.doc,"data");
  }
std::vector<std::string> attachmentOrder(const Decoder& r) {
  const auto& names=member(r.doc,"attachment_order"); array(names);
  std::vector<std::string> out;
  out.reserve(names.Size());
  for(const auto& name:names.GetArray()) out.push_back(text(name));
  return out;
  }
std::string difference(const Encoder& expected, const Json& a, const Encoder& actual, const Json& b) {
  const auto left=jsonText(a),right=jsonText(b);
  if(left!=right) {
    size_t at=0;
    while(at<left.size() && at<right.size() && left[at]==right[at]) ++at;
    return "metadata differs at JSON byte "+std::to_string(at)+": "+left.substr(at,80)+" != "+right.substr(at,80);
    }
  if(expected.bin!=actual.bin) {
    size_t at=0;
    while(at<expected.bin.size() && at<actual.bin.size() && expected.bin[at]==actual.bin[at]) ++at;
    return "binary arrays differ at byte "+std::to_string(at);
    }
  return {};
  }
}

void writeHierarchy(const ModelHierarchy& data, const fs::path& file) {
  Encoder w; auto value=encode(w,data); write(w,std::move(value),"hierarchy",file);
  }
ModelHierarchy readHierarchy(const fs::path& file) {
  Decoder r; const auto& value=read(r,file,"hierarchy"); ModelHierarchy out {}; decode(r,value,out); return out;
  }
void writeMesh(const ModelMesh& data, const fs::path& file, const std::vector<std::string>& order) {
  validateOrder(data,order); Encoder w; auto value=encode(w,data); write(w,std::move(value),"mesh",file,&order);
  }
ModelMesh readMesh(const fs::path& file) {
  Decoder r; const auto& value=read(r,file,"mesh"); ModelMesh out {}; decode(r,value,out);
  restoreOrder(out,attachmentOrder(r)); return out;
  }
void writeModel(const Model& data, const fs::path& file, const std::vector<std::string>& order) {
  validateOrder(data.mesh,order); Encoder w; auto value=encode(w,data); write(w,std::move(value),"model",file,&order);
  }
Model readModel(const fs::path& file) {
  Decoder r; const auto& value=read(r,file,"model"); Model out {}; decode(r,value,out);
  restoreOrder(out.mesh,attachmentOrder(r)); return out;
  }
void writeAnimation(const ModelAnimation& data, const fs::path& file, const std::vector<QuantizedAnimationSample>& encoded) {
  require(encoded.empty() || encoded.size()==data.samples.size(),"quantized animation sample count mismatch");
  Encoder w; auto value=encode(w,data,encoded.empty() ? nullptr : &encoded); write(w,std::move(value),"animation",file);
  }
ModelAnimation readAnimation(const fs::path& file) {
  Decoder r; const auto& value=read(r,file,"animation"); ModelAnimation out {}; decode(r,value,out); return out;
  }
void writeScript(const ModelScript& data, const fs::path& file) {
  Encoder w; auto value=encode(w,data); write(w,std::move(value),"script",file);
  }
ModelScript readScript(const fs::path& file) {
  Decoder r; const auto& value=read(r,file,"script"); ModelScript out {}; decode(r,value,out); return out;
  }
void writeMorph(const MorphMesh& data, const fs::path& file) {
  Encoder w; auto value=encode(w,data); write(w,std::move(value),"morph",file);
  }
MorphMesh readMorph(const fs::path& file) {
  Decoder r; const auto& value=read(r,file,"morph"); MorphMesh out {}; decode(r,value,out); return out;
  }

std::string compare(const ModelHierarchy& expected, const ModelHierarchy& actual) {
  Encoder a,b; auto left=encode(a,expected); auto right=encode(b,actual);
  return difference(a,left,b,right);
  }

std::string compare(const ModelMesh& expected, const ModelMesh& actual) {
  Encoder a,b; auto left=encode(a,expected); auto right=encode(b,actual);
  return difference(a,left,b,right);
  }

std::string compare(const Model& expected, const Model& actual) {
  Encoder a,b; auto left=encode(a,expected); auto right=encode(b,actual);
  return difference(a,left,b,right);
  }

std::string compare(const ModelAnimation& expected, const ModelAnimation& actual) {
  Encoder a,b; auto left=encode(a,expected); auto right=encode(b,actual);
  return difference(a,left,b,right);
  }

std::string compare(const ModelScript& expected, const ModelScript& actual) {
  Encoder a,b; auto left=encode(a,expected); auto right=encode(b,actual);
  return difference(a,left,b,right);
  }

std::string compare(const MorphMesh& expected, const MorphMesh& actual) {
  Encoder a,b; auto left=encode(a,expected); auto right=encode(b,actual);
  return difference(a,left,b,right);
  }

}
