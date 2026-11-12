#include "speechface.h"

#include <Tempest/Log>

#include <rapidjson/document.h>
#include <rapidjson/error/en.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <stdexcept>

using namespace Tempest;

namespace {

bool enabled = []() {
  const char* v = std::getenv("OPENGOTHIC_SPEECH_FACE");
  return v==nullptr || v[0]!='0';
  }();

[[noreturn]] void fail(const std::string& what) {
  throw std::runtime_error(what);
  }

std::string quoted(std::string_view s) {
  return "\""+std::string(s)+"\"";
  }

std::string_view str(const rapidjson::Value& v) {
  return std::string_view(v.GetString(),v.GetStringLength());
  }

std::string lower(std::string_view s) {
  std::string ret(s);
  for(auto& c:ret)
    c = char(std::tolower(uint8_t(c)));
  return ret;
  }

// strict: only the base64 alphabet and its padding at the end
std::vector<uint8_t> fromBase64(std::string_view in) {
  const auto value = [](char c) -> int {
    if(c>='A' && c<='Z') return c-'A';
    if(c>='a' && c<='z') return c-'a'+26;
    if(c>='0' && c<='9') return c-'0'+52;
    if(c=='+') return 62;
    if(c=='/') return 63;
    return -1;
    };
  if(in.size()%4!=0)
    fail("data: base64 of a length that is not a multiple of 4");
  std::vector<uint8_t> out;
  out.reserve(in.size()/4*3);
  for(size_t i=0; i<in.size(); i+=4) {
    int    v[4] = {};
    size_t pad  = 0;
    for(size_t k=0; k<4; ++k) {
      const char c = in[i+k];
      if(c=='=' && i+4==in.size() && k>=2) {
        ++pad;
        continue;
        }
      if(pad>0 || (v[k] = value(c))<0)
        fail("data: not base64 at "+std::to_string(i+k));
      }
    const uint32_t n = uint32_t(v[0])<<18 | uint32_t(v[1])<<12 | uint32_t(v[2])<<6 | uint32_t(v[3]);
    out.push_back(uint8_t(n>>16));
    if(pad<2)
      out.push_back(uint8_t(n>>8));
    if(pad<1)
      out.push_back(uint8_t(n));
    }
  return out;
  }

// "anger:0.8,disgust:0.3": the names of Audio2Face v3.0, strengths 0..1; true: a strength above 0
bool readEmotion(std::string_view label) {
  bool any = false;
  while(!label.empty()) {
    const size_t end  = std::min(label.find(','),label.size());
    const auto   item = label.substr(0,end);
    label.remove_prefix(std::min(end+1,label.size()));
    const size_t colon = item.find(':');
    if(colon==std::string_view::npos)
      fail("emotion: "+quoted(item)+" is not <name>:<strength>");
    const auto name = item.substr(0,colon);
    if(std::find(std::begin(SpeechFace::Emotions),std::end(SpeechFace::Emotions),name)==std::end(SpeechFace::Emotions))
      fail("emotion: unknown emotion "+quoted(name));
    const std::string num(item.substr(colon+1));
    char*        last = nullptr;
    const double k    = std::strtod(num.c_str(),&last);
    if(num.empty() || last!=num.c_str()+num.size() || !std::isfinite(k) || k<0 || k>1)
      fail("emotion: the strength of "+quoted(name)+" must be a number 0..1");
    any = any || k>0;
    }
  return any;
  }

float smooth(float x) {
  x = std::clamp(x,0.f,1.f);
  return x*x*(3.f-2.f*x);
  }

bool validPattern(std::string_view p) {
  if(p.empty() || p=="*")
    return false;
  const size_t star = p.find('*');
  return star==std::string_view::npos || star+1==p.size();
  }

}

bool FaceProfile::Speech::matches(std::string_view pattern, std::string_view name) {
  if(!pattern.empty() && pattern.back()=='*')
    return name.substr(0,pattern.size()-1)==pattern.substr(0,pattern.size()-1);
  return name==pattern;
  }

bool SpeechFace::isEnabled() {
  return enabled;
  }

void SpeechFace::setEnabled(bool e) {
  enabled = e;
  }

float SpeechFace::Track::durationMs() const {
  return frames>0 ? float(frames-1)*1000.f/fps : 0.f;
  }

std::shared_ptr<const SpeechFace::Track> SpeechFace::Track::read(const rapidjson::Value& obj) {
  if(!obj.IsObject())
    fail("the track must be a JSON object");
  auto ret = std::make_shared<Track>();
  if(!obj.HasMember("fps") || !obj["fps"].IsNumber() || !std::isfinite(obj["fps"].GetDouble()) ||
     obj["fps"].GetDouble()<1 || obj["fps"].GetDouble()>240)
    fail("fps must be a number 1..240");
  ret->fps = float(obj["fps"].GetDouble());
  if(!obj.HasMember("shapes") || !obj["shapes"].IsArray() || obj["shapes"].Empty())
    fail("shapes must be a non-empty list of names");
  for(const auto& s:obj["shapes"].GetArray()) {
    if(!s.IsString() || s.GetStringLength()==0)
      fail("shapes must be a list of non-empty strings");
    if(std::find(ret->shapes.begin(),ret->shapes.end(),str(s))!=ret->shapes.end())
      fail("shapes: "+quoted(str(s))+" listed twice");
    ret->shapes.emplace_back(str(s));
    }
  if(!obj.HasMember("data") || !obj["data"].IsString())
    fail("data must be a base64 string");
  ret->data = fromBase64(str(obj["data"]));
  if(ret->data.empty() || ret->data.size()%ret->shapes.size()!=0)
    fail("data: "+std::to_string(ret->data.size())+" bytes are not whole frames of "+
         std::to_string(ret->shapes.size())+" shapes");
  ret->frames = ret->data.size()/ret->shapes.size();
  if(obj.HasMember("emotion") && !obj["emotion"].IsNull()) {
    if(!obj["emotion"].IsString())
      fail("emotion must be a string");
    ret->emotion   = std::string(str(obj["emotion"]));
    ret->emotional = readEmotion(ret->emotion);
    }
  return ret;
  }

std::shared_ptr<const SpeechFace::Track> SpeechFace::Track::parse(std::string_view json) {
  rapidjson::Document doc;
  doc.Parse<rapidjson::kParseValidateEncodingFlag>(json.data(),json.size());
  if(doc.HasParseError())
    fail("invalid JSON at offset "+std::to_string(doc.GetErrorOffset())+": "+rapidjson::GetParseError_En(doc.GetParseError()));
  return read(doc);
  }

FaceProfile::Speech SpeechFace::parseRules(const rapidjson::Value& v, const FaceProfile& profile) {
  const std::string where = "speech_face";
  if(!v.IsObject())
    fail(where+": must be an object");
  for(const auto& m:v.GetObject()) {
    const auto key = str(m.name);
    if(key!="about" && key!="fade_in_ms" && key!="fade_out_ms" && key!="cap" && key!="scale" && key!="drop" &&
       key!="mood_faces")
      fail(where+": unknown field "+quoted(key));
    }
  if(v.HasMember("about") && !v["about"].IsString())
    fail(where+": about must be a string");
  FaceProfile::Speech ret;
  for(auto [key,out]:{std::pair{"fade_in_ms",&ret.fadeInMs},std::pair{"fade_out_ms",&ret.fadeOutMs}}) {
    if(!v.HasMember(key) || !v[key].IsNumber() || !std::isfinite(v[key].GetDouble()) || v[key].GetDouble()<0 ||
       v[key].GetDouble()>2000)
      fail(where+": "+key+" must be a time 0..2000 ms");
    *out = float(v[key].GetDouble());
    }
  if(v.HasMember("cap")) {
    if(!v["cap"].IsObject())
      fail(where+": cap must be an object of shape -> the shape it stays under");
    for(const auto& c:v["cap"].GetObject()) {
      if(!c.value.IsString() || c.name.GetStringLength()==0 || c.value.GetStringLength()==0 ||
         str(c.name).find('*')!=std::string_view::npos || str(c.value).find('*')!=std::string_view::npos)
        fail(where+": cap: "+quoted(str(c.name))+" must name a shape and the one it stays under");
      ret.caps.push_back({std::string(str(c.name)),std::string(str(c.value))});
      }
    }
  if(v.HasMember("scale")) {
    if(!v["scale"].IsObject())
      fail(where+": scale must be an object of shape (or prefix*) -> factor");
    for(const auto& s:v["scale"].GetObject()) {
      if(!validPattern(str(s.name)))
        fail(where+": scale: "+quoted(str(s.name))+" is not a shape or a prefix ending with *");
      if(!s.value.IsNumber() || !std::isfinite(s.value.GetDouble()) || s.value.GetDouble()<0 || s.value.GetDouble()>4)
        fail(where+": scale: the factor of "+quoted(str(s.name))+" must be a number 0..4");
      ret.scales.push_back({std::string(str(s.name)),float(s.value.GetDouble())});
      }
    }
  if(v.HasMember("drop")) {
    if(!v["drop"].IsArray())
      fail(where+": drop must be a list of shapes or prefixes ending with *");
    for(const auto& d:v["drop"].GetArray()) {
      if(!d.IsString() || !validPattern(str(d)))
        fail(where+": drop: each must be a shape or a prefix ending with *");
      ret.drop.emplace_back(str(d));
      }
    }
  if(v.HasMember("mood_faces")) {
    if(!v["mood_faces"].IsArray())
      fail(where+": mood_faces must be a list of anims of the profile");
    for(const auto& m:v["mood_faces"].GetArray()) {
      const size_t id = m.IsString() ? profile.index(str(m)) : size_t(-1);
      if(id==size_t(-1))
        fail(where+": mood_faces: no anim "+(m.IsString() ? quoted(str(m)) : std::string("(not a string)")));
      ret.moodFaces.push_back(id);
      }
    }
  return ret;
  }

std::shared_ptr<const SpeechFace::Track> SpeechFace::fromFile(std::string_view sound, std::string_view dir) {
  namespace fs = std::filesystem;
  std::string name = lower(sound);
  if(name.size()>4 && name.compare(name.size()-4,4,".wav")==0)
    name.resize(name.size()-4);
  if(name.empty())
    return nullptr;
  name += ".face.json";
  // the names in each directory, once (case aside: the game names its sounds in any case)
  static std::map<std::string,std::map<std::string,fs::path>> listed;
  const char* env = std::getenv("OPENGOTHIC_FACE_TRACKS");
  for(std::string_view d:{std::string_view(env!=nullptr ? env : ""),dir}) {
    if(d.empty())
      continue;
    auto it = listed.find(std::string(d));
    if(it==listed.end()) {
      it = listed.emplace(std::string(d),std::map<std::string,fs::path>()).first;
      std::error_code ec;
      for(fs::directory_iterator i(fs::path(std::string(d)),ec), end; !ec && i!=end; i.increment(ec)) {
        const auto n = lower(i->path().filename().string());
        if(n.size()>10 && n.compare(n.size()-10,10,".face.json")==0)
          it->second[n] = i->path();
        }
      }
    const auto f = it->second.find(name);
    if(f==it->second.end())
      continue;
    try {
      std::ifstream file(f->second,std::ios::binary);
      if(!file)
        fail("unable to open the file");
      const std::string text{std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>()};
      return Track::parse(text);
      }
    catch(const std::exception& e) {
      Log::e("[face] invalid face track \"",f->second.string(),"\": ",e.what(),"; the voice goes without it");
      return nullptr;
      }
    }
  return nullptr;
  }

void SpeechFace::bind(std::shared_ptr<const Track> t) {
  track       = std::move(t);
  going       = false;
  shownK      = 0;
  mappedTrack = nullptr;
  }

void SpeechFace::fadeOut(uint64_t now) {
  if(track==nullptr || going)
    return;
  going  = true;
  goneAt = now;
  goneK  = shownK;
  }

void SpeechFace::setTrack(std::shared_ptr<const Track> t, std::string_view line) {
  if(t==nullptr || line.empty())
    return;
  // its voice already plays (a full sound starts at once, the track comes right after it)
  if(voice!=nullptr && voice->isOf(line)) {
    bind(std::move(t));
    return;
    }
  pending.erase(std::remove_if(pending.begin(),pending.end(),[&](const Line& l){ return l.id==line; }),pending.end());
  if(pending.size()>=Pending)
    pending.erase(pending.begin());
  pending.push_back({std::string(line),std::move(t)});
  }

void SpeechFace::start(std::shared_ptr<LipSync::Track> v, std::string_view dir, uint64_t now) {
  // the face of the last voice goes as the mouth of a new one starts
  fadeOut(now);
  voice     = std::move(v);
  startTick = now;
  if(voice==nullptr)
    return;
  std::shared_ptr<const Track> t;
  for(size_t i=0; i<pending.size(); ++i)
    if(voice->isOf(pending[i].id)) {
      t = std::move(pending[i].track);
      pending.erase(pending.begin()+std::ptrdiff_t(i));
      break;
      }
  if(t==nullptr && !dir.empty() && enabled)
    t = fromFile(voice->name(),dir);
  if(t!=nullptr)
    bind(std::move(t));
  }

void SpeechFace::stop(uint64_t now) {
  fadeOut(now);
  voice = nullptr;
  }

uint64_t SpeechFace::lineTime(uint64_t now) const {
  // as LipSync: what the audio thread has played of a streamed voice, else the time since it started
  const int64_t played = voice->playedMs();
  if(played>=0)
    return uint64_t(played);
  return now>startTick ? now-startTick : 0;
  }

void SpeechFace::remap(const FaceRig& face) {
  const auto& rules = *face.faceProfile()->speech;
  const auto& t     = *track;
  map = Map();
  map.target.assign(t.shapes.size(),uint32_t(-1));
  map.scale .assign(t.shapes.size(),1.f);
  for(size_t i=0; i<t.shapes.size(); ++i) {
    const auto& n = t.shapes[i];
    bool drop = false;
    for(auto& d:rules.drop)
      drop = drop || FaceProfile::Speech::matches(d,n);
    for(auto& s:rules.scales)
      if(FaceProfile::Speech::matches(s.shape,n))
        map.scale[i] *= s.k;
    // a shape the head does not have is left out
    const size_t id = drop ? size_t(-1) : face.target(n);
    if(id!=size_t(-1))
      map.target[i] = uint32_t(id);
    }
  for(auto& c:rules.caps) {
    const auto a = std::find(t.shapes.begin(),t.shapes.end(),c.shape);
    const auto b = std::find(t.shapes.begin(),t.shapes.end(),c.under);
    if(a!=t.shapes.end() && b!=t.shapes.end())
      map.caps.emplace_back(size_t(a-t.shapes.begin()),size_t(b-t.shapes.begin()));
    }
  mappedTrack   = track.get();
  mappedProfile = face.faceProfile();
  }

void SpeechFace::sample(const FaceRig& face, float ms, std::vector<std::pair<uint32_t,float>>& out) {
  out.clear();
  if(track==nullptr || face.faceProfile()==nullptr || !face.faceProfile()->speech)
    return;
  if(mappedTrack!=track.get() || mappedProfile!=face.faceProfile())
    remap(face);
  const auto&  t  = *track;
  const float  f  = std::clamp(ms*t.fps/1000.f,0.f,float(t.frames-1));
  const size_t f0 = size_t(f);
  const size_t f1 = std::min(f0+1,t.frames-1);
  const float  a  = f-float(f0);
  raw.resize(t.shapes.size());
  for(size_t i=0; i<raw.size(); ++i)
    raw[i] = t.weight(f0,i)*(1.f-a) + t.weight(f1,i)*a;
  // the caps on the track's own weights (mouthClose over a nearly closed jaw rides the lower lip through the upper)
  for(auto [s,under]:map.caps)
    raw[s] = std::min(raw[s],raw[under]);
  for(size_t i=0; i<raw.size(); ++i)
    if(map.target[i]!=uint32_t(-1))
      out.emplace_back(map.target[i],raw[i]*map.scale[i]);
  }

float SpeechFace::play(FaceRig& face, uint64_t now) {
  const auto* profile = face.faceProfile();
  if(profile==nullptr || !profile->speech || !enabled || track==nullptr) {
    if(track!=nullptr && (profile==nullptr || !profile->speech || !enabled))
      bind(nullptr);
    face.setVoiceFace({},0,false);
    return 0;
    }
  const auto& rules = *profile->speech;
  if(!going && voice!=nullptr) {
    // the track ends at its last frame or at the end of the voice, whichever is first; then its last frame fades
    float end = track->durationMs();
    if(voice->isFinished())
      end = std::min(end,float(voice->durationMs()));
    const float t = float(lineTime(now));
    sample(face,std::min(t,end),shown);
    shownK = rules.fadeInMs>0 ? smooth(t/rules.fadeInMs) : 1.f;
    if(t>=end)
      fadeOut(now);
    }
  else if(!going) {
    fadeOut(now);
    }
  float k = shownK;
  if(going) {
    k = goneK*(1.f - (rules.fadeOutMs>0 ? smooth(float(now-goneAt)/rules.fadeOutMs) : 1.f));
    if(k<=0) {
      bind(nullptr);
      shown.clear();
      face.setVoiceFace({},0,false);
      return 0;
      }
    }
  face.setVoiceFace(shown,k,track->emotional);
  return k;
  }
