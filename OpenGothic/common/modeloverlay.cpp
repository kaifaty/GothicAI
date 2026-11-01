#include "modeldata.h"
#include "gothicglb.h"

#include <rapidjson/error/en.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace ModelData {
namespace {
using namespace asset;
using Json = rapidjson::Value;
using gothicglb::member;
namespace fs = std::filesystem;

void require(bool ok, const std::string& reason) {
  if(!ok)
    throw std::runtime_error("model overlay: "+reason);
  }

template<class T> T integer(const Json& value) {
  require(value.IsInt64(),"expected integer");
  const auto n=value.GetInt64();
  require(n>=int64_t(std::numeric_limits<T>::min()) && n<=int64_t(std::numeric_limits<T>::max()),
          "integer out of range");
  return T(n);
  }
template<class T> T integer(const Json& obj, const char* name) {
  return integer<T>(member(obj,name));
  }
template<class E> E enumeration(const Json& obj, const char* name) {
  return E(integer<std::underlying_type_t<E>>(obj,name));
  }
float real(const Json& obj, const char* name) {
  const auto value=gothicglb::jsonToFloat(member(obj,name),name);
  require(std::isfinite(value),std::string("non-finite float ")+name);
  return value;
  }
std::string text(const Json& obj, const char* name) {
  return gothicglb::jsonToString(member(obj,name),name);
  }
bool boolean(const Json& obj, const char* name) {
  return gothicglb::jsonToBool(member(obj,name),name);
  }

void decode(const Json& v, MdsEventTag& out) {
  out.frame=integer<int32_t>(v,"frame");
  out.type=enumeration<MdsEventType>(v,"type");
  out.slot=text(v,"slot");
  out.slot2=text(v,"slot2");
  out.item=text(v,"item");
  const auto& frames=member(v,"frames");
  require(frames.IsArray(),"event frames must be an array");
  for(const auto& frame:frames.GetArray()) out.frames.push_back(integer<int32_t>(frame));
  out.fight_mode=enumeration<MdsFightMode>(v,"fight_mode");
  out.attached=boolean(v,"attached");
  require(out.type!=MdsEventType::OPTIMAL_FRAME || !out.frames.empty(),"empty OPTIMAL_FRAME");
  if(out.type==MdsEventType::HIT_END || out.type==MdsEventType::PARRY_FRAME || out.type==MdsEventType::COMBO_WINDOW)
    require(std::all_of(out.frames.begin(),out.frames.end(),[](int32_t frame) { return frame>=0; }),
            "negative event frame");
  }
void decode(const Json& v, MdsParticleEffect& out) {
  out.frame=integer<int32_t>(v,"frame");
  out.index=integer<int32_t>(v,"index");
  out.name=text(v,"name");
  out.position=text(v,"position");
  out.attached=boolean(v,"attached");
  }
void decode(const Json& v, MdsParticleEffectStop& out) {
  out.frame=integer<int32_t>(v,"frame");
  out.index=integer<int32_t>(v,"index");
  }
template<class T> void sound(const Json& v, T& out) {
  out.frame=integer<int32_t>(v,"frame");
  out.name=text(v,"name");
  out.range=real(v,"range");
  out.empty_slot=boolean(v,"empty_slot");
  }
void decode(const Json& v, MdsSoundEffect& out) { sound(v,out); }
void decode(const Json& v, MdsSoundEffectGround& out) { sound(v,out); }
void decode(const Json& v, MdsMorphAnimation& out) {
  out.frame=integer<int32_t>(v,"frame");
  out.animation=text(v,"animation");
  out.node=text(v,"node");
  }
void decode(const Json& v, MdsCameraTremor& out) {
  out.frame=integer<int32_t>(v,"frame");
  out.field1=integer<int32_t>(v,"field1");
  out.field2=integer<int32_t>(v,"field2");
  out.field3=integer<int32_t>(v,"field3");
  out.field4=integer<int32_t>(v,"field4");
  }

template<class T> void header(const Json& v, T& out) {
  out.name=text(v,"name");
  out.layer=integer<uint32_t>(v,"layer");
  out.next=text(v,"next");
  out.blend_in=real(v,"blend_in");
  out.blend_out=real(v,"blend_out");
  out.flags=enumeration<AnimationFlags>(v,"flags");
  out.direction=enumeration<AnimationDirection>(v,"direction");
  const auto valid=[](float seconds) {
    return seconds>=0 && double(1000.f*seconds)<double(std::numeric_limits<uint64_t>::max());
    };
  require(valid(out.blend_in) && valid(out.blend_out),"invalid blend times for "+out.name);
  }
void decode(const Json& v, MdsAnimationAlias& out) {
  header(v,out);
  out.alias=text(v,"alias");
  }
void decode(const Json& v, MdsAnimation& out);
template<class T> void array(const Json& obj, const char* name, std::vector<T>& out) {
  const auto& values=member(obj,name);
  require(values.IsArray(),std::string(name)+" must be an array");
  out.resize(values.Size());
  for(rapidjson::SizeType i=0;i<values.Size();++i) decode(values[i],out[i]);
  }
void decode(const Json& v, MdsAnimation& out) {
  header(v,out);
  out.model=text(v,"model");
  out.first_frame=integer<int32_t>(v,"first_frame");
  out.last_frame=integer<int32_t>(v,"last_frame");
  out.fps=real(v,"fps");
  out.speed=real(v,"speed");
  out.collision_volume_scale=real(v,"collision_volume_scale");
  array(v,"events",out.events);
  array(v,"pfx",out.pfx);
  array(v,"pfx_stop",out.pfx_stop);
  array(v,"sfx",out.sfx);
  array(v,"sfx_ground",out.sfx_ground);
  array(v,"morph",out.morph);
  array(v,"tremors",out.tremors);
  }
}

// Keep runtime content decoding outside the base importer fingerprint.
asset::ModelScript readScriptOverlay(const std::filesystem::path& file) {
  const auto bytes=gothicglb::readFile(file);
  rapidjson::Document record;
  record.Parse<rapidjson::kParseValidateEncodingFlag>(reinterpret_cast<const char*>(bytes.data()),bytes.size());
  require(!record.HasParseError(),record.HasParseError() ? rapidjson::GetParseError_En(record.GetParseError()) : "");
  require(text(record,"format")=="gothic-model" && integer<int>(record,"version")==1,"unsupported format or version");
  require(text(record,"kind")=="script-overlay","wrong record kind");
  auto binary=file;
  binary.replace_extension(".bin");
  require(file!=binary && text(record,"buffer")==binary.filename().string(),"binary filename must be the fixed sibling");
  require(integer<int64_t>(record,"byte_length")==0 && gothicglb::readFile(binary).empty(),"overlay binary must be empty");
  asset::ModelScript out;
  const auto& data=member(record,"data");
  array(data,"animations",out.animations);
  array(data,"aliases",out.aliases);
  return out;
  }
}
