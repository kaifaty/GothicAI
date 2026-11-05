#include "facerig.h"

#include "gltfmesh.h"

#include <rapidjson/document.h>
#include <rapidjson/error/en.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <stdexcept>

using namespace Tempest;

FaceRig::Probe FaceRig::subjectProbe;
const void*    FaceRig::probeNpc = nullptr;

namespace {

[[noreturn]] void fail(const std::string& what) {
  throw std::runtime_error(what);
  }

std::string quoted(std::string_view s) {
  return "\""+std::string(s)+"\"";
  }

std::string_view str(const rapidjson::Value& v) {
  return std::string_view(v.GetString(),v.GetStringLength());
  }

double number(const rapidjson::Value& obj, const char* key, const std::string& where) {
  if(!obj.HasMember(key))
    fail(where+": no "+key);
  const auto& v = obj[key];
  if(!v.IsNumber() || !std::isfinite(v.GetDouble()))
    fail(where+": "+key+" must be a number");
  return v.GetDouble();
  }

void onlyFields(const rapidjson::Value& obj, std::initializer_list<std::string_view> keys, const std::string& where) {
  for(const auto& m:obj.GetObject()) {
    const auto key = str(m.name);
    if(std::find(keys.begin(),keys.end(),key)==keys.end())
      fail(where+": unknown field "+quoted(key));
    }
  }

uint32_t targetId(const GltfHeadMesh& head, std::string_view name) {
  for(size_t t=0; t<head.targets.size(); ++t)
    if(head.targets[t].name==name)
      return uint32_t(t);
  return uint32_t(-1);
  }

// the sphere through the vertices [first, end) by least squares: |p-c|^2 = r^2 as 2 p.c + (r^2 - c.c) = p.p
bool fitSphere(const std::vector<asset::Vec3>& pos, uint32_t first, uint32_t end, Vec3& center, float& radius) {
  if(end<first+4)
    return false;
  double mean[3] = {};
  for(uint32_t i=first; i<end; ++i) {
    mean[0] += pos[i].x;
    mean[1] += pos[i].y;
    mean[2] += pos[i].z;
    }
  for(auto& m:mean)
    m /= double(end-first);
  double a[4][5] = {};
  for(uint32_t i=first; i<end; ++i) {
    const double p[3] = {pos[i].x-mean[0], pos[i].y-mean[1], pos[i].z-mean[2]};
    const double row[5] = {2*p[0], 2*p[1], 2*p[2], 1, p[0]*p[0]+p[1]*p[1]+p[2]*p[2]};
    for(int r=0; r<4; ++r)
      for(int c=0; c<5; ++c)
        a[r][c] += row[r]*row[c];
    }
  for(int c=0; c<4; ++c) {
    int best = c;
    for(int r=c+1; r<4; ++r)
      if(std::abs(a[r][c])>std::abs(a[best][c]))
        best = r;
    if(std::abs(a[best][c])<1e-9)
      return false;
    std::swap(a[c],a[best]);
    for(int r=0; r<4; ++r) {
      if(r==c)
        continue;
      const double k = a[r][c]/a[c][c];
      for(int i=c; i<5; ++i)
        a[r][i] -= k*a[c][i];
      }
    }
  double x[4];
  for(int i=0; i<4; ++i)
    x[i] = a[i][4]/a[i][i];
  const double r2 = x[3] + x[0]*x[0] + x[1]*x[1] + x[2]*x[2];
  if(!(r2>0) || !std::isfinite(r2))
    return false;
  center = Vec3(float(x[0]+mean[0]),float(x[1]+mean[1]),float(x[2]+mean[2]));
  radius = float(std::sqrt(r2));
  return true;
  }

FaceProfile::Eyes parseEyes(const rapidjson::Value& v, const GltfHeadMesh& head) {
  using Eyes = FaceProfile::Eyes;
  if(!v.IsObject())
    fail("eyes must be an object");
  onlyFields(v,{"about","parts","limits_deg","lids"},"eyes");
  if(v.HasMember("about") && !v["about"].IsString())
    fail("eyes: about must be a string");
  Eyes ret;

  if(!v.HasMember("parts") || !v["parts"].IsObject())
    fail("eyes: parts must be an object of the left and the right eye's head parts");
  const auto& parts = v["parts"];
  onlyFields(parts,{"left","right"},"eyes: parts");
  const char* side[2] = {"left","right"};
  for(int s=0; s<2; ++s) {
    if(!parts.HasMember(side[s]) || !parts[side[s]].IsString())
      fail(std::string("eyes: parts: ")+side[s]+" must be the name of a head part");
    const auto name = str(parts[side[s]]);
    auto&      e    = ret.eye[s];
    e.part = head.parts.size();
    for(size_t i=0; i<head.parts.size(); ++i)
      if(head.parts[i].name==name)
        e.part = i;
    if(e.part==head.parts.size())
      fail("eyes: parts: no head part "+quoted(name));
    const auto& p = head.parts[e.part];
    e.first = uint32_t(p.firstVertex);
    e.end   = uint32_t(std::min(p.firstVertex+p.vertexCount,head.mesh.positions.size()));
    if(!fitSphere(head.mesh.positions,e.first,e.end,e.center,e.radius))
      fail("eyes: part "+quoted(name)+" is not a ball (no sphere fits its vertices)");
    }
  if(ret.eye[0].part==ret.eye[1].part)
    fail("eyes: parts: the left and the right eye must be different parts");
  const float apart = ret.eye[0].center.y - ret.eye[1].center.y;
  if(std::abs(apart)<ret.eye[0].radius)
    fail("eyes: the balls must stand apart across the head (y)");
  ret.eye[0].outSign = apart>0 ? 1.f : -1.f;
  ret.eye[1].outSign = -ret.eye[0].outSign;

  const char* dir[4] = {"down","up","in","out"};
  if(!v.HasMember("limits_deg") || !v["limits_deg"].IsObject())
    fail("eyes: limits_deg must be an object");
  const auto& lim = v["limits_deg"];
  onlyFields(lim,{"down","up","in","out"},"eyes: limits_deg");
  for(int d=0; d<4; ++d) {
    const double deg = number(lim,dir[d],"eyes: limits_deg");
    if(deg<=0 || deg>90)
      fail(std::string("eyes: limits_deg: ")+dir[d]+" must be above 0 and up to 90");
    ret.limit[d] = float(deg*M_PI/180.0);
    }

  if(!v.HasMember("lids") || !v["lids"].IsObject())
    fail("eyes: lids must be an object of gaze directions");
  const auto& lids = v["lids"];
  onlyFields(lids,{"down","up","in","out"},"eyes: lids");
  for(int d=0; d<4; ++d) {
    if(!lids.HasMember(dir[d]))
      continue;
    const auto& list = lids[dir[d]];
    if(!list.IsArray())
      fail(std::string("eyes: lids: ")+dir[d]+" must be a list");
    for(rapidjson::SizeType i=0; i<list.Size(); ++i) {
      const auto  at = std::string("eyes: lids ")+dir[d]+" "+std::to_string(i);
      const auto& l  = list[i];
      if(!l.IsObject())
        fail(at+": must be an object");
      onlyFields(l,{"target","from","to","max"},at);
      if(!l.HasMember("target") || !l["target"].IsString())
        fail(at+": target must be a target name ({side} is Left or Right)");
      const double from = number(l,"from",at), to = number(l,"to",at), max = number(l,"max",at);
      if(from<0 || to<=from)
        fail(at+": from must be 0 or more and to above it");
      if(max<0 || max>1)
        fail(at+": max must be a number 0..1");
      for(int s=0; s<2; ++s) {
        std::string name = std::string(str(l["target"]));
        for(size_t p; (p = name.find("{side}"))!=std::string::npos;)
          name.replace(p,6,s==0 ? "Left" : "Right");
        Eyes::Lid lid;
        lid.target = targetId(head,name);
        if(lid.target==uint32_t(-1))
          fail(at+": unknown target "+quoted(std::string_view(name)));
        lid.from = float(from);
        lid.to   = float(to);
        lid.max  = float(max);
        ret.eye[s].lids[d].push_back(lid);
        }
      }
    }
  return ret;
  }

}

const FaceProfile::Anim* FaceProfile::find(std::string_view name) const {
  const size_t i = index(name);
  return i<anims.size() ? &anims[i] : nullptr;
  }

size_t FaceProfile::index(std::string_view name) const {
  for(size_t i=0; i<anims.size(); ++i)
    if(anims[i].name==name)
      return i;
  return size_t(-1);
  }

std::unique_ptr<FaceProfile> FaceProfile::parse(std::string_view json, const GltfHeadMesh& head) {
  rapidjson::Document doc;
  doc.Parse<rapidjson::kParseValidateEncodingFlag>(json.data(),json.size());
  if(doc.HasParseError())
    fail("invalid JSON at offset "+std::to_string(doc.GetErrorOffset())+": "+rapidjson::GetParseError_En(doc.GetParseError()));
  if(!doc.IsObject())
    fail("the profile must be a JSON object");
  for(const auto& m:doc.GetObject()) {
    const auto key = str(m.name);
    if(key!="version" && key!="about" && key!="mouth" && key!="anims" && key!="eyes")
      fail("unknown field "+quoted(key));
    }
  if(!doc.HasMember("version") || !doc["version"].IsInt() || doc["version"].GetInt()!=1)
    fail("version must be 1");
  if(doc.HasMember("about") && !doc["about"].IsString())
    fail("about must be a string");

  auto ret = std::make_unique<FaceProfile>();
  const size_t targets = head.targets.size();

  // the mouth: targets by the prefix of their name
  if(!doc.HasMember("mouth") || !doc["mouth"].IsArray())
    fail("mouth must be a list of target name prefixes");
  std::vector<std::string> prefixes;
  for(const auto& p:doc["mouth"].GetArray()) {
    if(!p.IsString() || p.GetStringLength()==0)
      fail("mouth must be a list of non-empty strings");
    prefixes.emplace_back(str(p));
    }
  ret->mouth.assign(targets,0);
  for(size_t t=0; t<targets; ++t)
    for(auto& p:prefixes)
      if(head.targets[t].name.compare(0,p.size(),p)==0)
        ret->mouth[t] = 1;

  if(!doc.HasMember("anims") || !doc["anims"].IsObject() || doc["anims"].MemberCount()==0)
    fail("anims must be a non-empty object");

  std::vector<Vec3>    acc(head.mesh.positions.size());
  std::vector<uint8_t> mark(head.mesh.positions.size(),0);
  std::vector<uint32_t> used;
  for(const auto& entry:doc["anims"].GetObject()) {
    const auto  name  = str(entry.name);
    const auto  where = "anim "+quoted(name);
    const auto& v     = entry.value;
    if(name.empty())
      fail("an anim without a name");
    if(ret->index(name)!=size_t(-1))
      fail(where+": listed twice");
    if(!v.IsObject())
      fail(where+": must be an object");
    for(const auto& m:v.GetObject()) {
      const auto key = str(m.name);
      if(key!="layer" && key!="blend_in_ms" && key!="blend_out_ms" && key!="duration_ms" && key!="frame_ms" && key!="frames")
        fail(where+": unknown field "+quoted(key));
      }
    Anim a;
    a.name = std::string(name);
    if(!v.HasMember("layer") || !v["layer"].IsInt() || v["layer"].GetInt()<0)
      fail(where+": layer must be a whole number from 0");
    a.layer = v["layer"].GetInt();
    const double in  = number(v,"blend_in_ms",where);
    const double out = number(v,"blend_out_ms",where);
    if(in<0 || out<0)
      fail(where+": blend_in_ms and blend_out_ms must not be negative");
    a.blendIn  = in >0 ? float(1.0/in)  : 0.f;
    a.blendOut = out>0 ? float(1.0/out) : 0.f;
    if(!v.HasMember("duration_ms") || !v["duration_ms"].IsInt64() || v["duration_ms"].GetInt64()<-1)
      fail(where+": duration_ms must be -1, 0 or a time in ms");
    a.duration = uint64_t(std::max<int64_t>(0,v["duration_ms"].GetInt64()));
    const double frame = number(v,"frame_ms",where);
    if(frame<=0)
      fail(where+": frame_ms must be above 0");
    a.frameMs = std::max<uint64_t>(1,uint64_t(std::llround(frame)));
    if(!v.HasMember("frames") || !v["frames"].IsArray() || v["frames"].Empty())
      fail(where+": frames must be a non-empty list");
    for(const auto& f:v["frames"].GetArray()) {
      const auto at = where+" frame "+std::to_string(a.frames.size());
      if(!f.IsObject())
        fail(at+": must be an object of target weights");
      std::vector<std::pair<uint32_t,float>> weights;
      for(const auto& w:f.GetObject()) {
        const auto target = str(w.name);
        size_t id = size_t(-1);
        for(size_t t=0; t<targets; ++t)
          if(head.targets[t].name==target) {
            id = t;
            break;
            }
        if(id==size_t(-1))
          fail(at+": unknown target "+quoted(target));
        if(!w.value.IsNumber() || !std::isfinite(w.value.GetDouble()) || w.value.GetDouble()<0 || w.value.GetDouble()>1)
          fail(at+": the weight of "+quoted(target)+" must be a number 0..1");
        weights.emplace_back(uint32_t(id),float(w.value.GetDouble()));
        }
      a.frames.push_back(std::move(weights));
      }

    // how far each frame moves the vertices on average, over the vertices the anim moves at all
    // (as ProtoMesh::mkAnimation for the VISEME shapes of lip sync)
    used.clear();
    for(auto& f:a.frames)
      for(auto& [t,w]:f)
        for(auto& d:head.targets[t].vertices)
          if(d.vertex<mark.size() && mark[d.vertex]==0) {
            mark[d.vertex] = 1;
            used.push_back(d.vertex);
            }
    float strongest = 0;
    a.strength.resize(a.frames.size());
    for(size_t i=0; i<a.frames.size(); ++i) {
      for(auto id:used)
        acc[id] = Vec3();
      for(auto& [t,w]:a.frames[i])
        for(auto& d:head.targets[t].vertices)
          if(d.vertex<acc.size())
            acc[d.vertex] += Vec3(d.position.x,d.position.y,d.position.z)*w;
      float sum = 0;
      for(auto id:used)
        sum += acc[id].length();
      a.strength[i] = used.empty() ? 0.f : sum/float(used.size());
      strongest     = std::max(strongest,a.strength[i]);
      }
    for(auto& s:a.strength)
      s = strongest>0 ? s/strongest : 0.f;
    for(auto id:used)
      mark[id] = 0;

    ret->anims.push_back(std::move(a));
    }
  if(doc.HasMember("eyes"))
    ret->eyes = parseEyes(doc["eyes"],head);
  return ret;
  }


void FaceRig::setup(const GltfHeadMesh* h, const FaceProfile* p) {
  if(h==head && p==profile) {
    dirty = true;
    return;
    }
  head     = h;
  profile  = p;
  slots.clear();
  want.clear();
  applied.clear();
  pos.clear();
  nrm.clear();
  basePos.clear();
  baseNrm.clear();
  drawnFirst.clear();
  drawn.clear();
  moved.clear();
  mark.clear();
  deltas.clear();
  deform.clear();
  deformed = false;
  dirty    = true;
  speechLayer = -1;
  for(int i=0; i<2; ++i)
    eyeWant[i] = eyeShown[i] = EyeTurn();
  if(!isActive())
    return;
  if(auto v = profile->find("VISEME"))
    speechLayer = v->layer;
  want   .assign(head->targets.size(),0.f);
  applied.assign(head->targets.size(),0.f);
  }

float FaceRig::fade(const Slot& s, uint64_t now) const {
  if(!isOn(s,now))
    return 0;
  if(s.manual)
    return 1;
  const auto& m = profile->anims[s.anim];
  float w = 1;
  if(m.blendIn>0)
    w = std::min(w, float(now-s.timeStart)*m.blendIn);
  if(m.blendOut>0 && s.timeUntil!=uint64_t(-1))
    w = std::min(w, float(s.timeUntil-now)*m.blendOut);
  return std::clamp(w,0.f,1.f);
  }

void FaceRig::fadeOut(Slot& s, uint64_t now) {
  const auto& m = profile->anims[s.anim];
  if(s.manual || m.blendOut<=0) {
    s.on = false;
    return;
    }
  s.timeUntil = std::min(s.timeUntil, now + uint64_t(std::lround(fade(s,now)/m.blendOut)));
  }

FaceRig::Slot& FaceRig::freeSlot(uint64_t now) {
  for(auto& s:slots)
    if(!isOn(s,now))
      return s;
  slots.emplace_back();
  return slots.back();
  }

void FaceRig::startAnim(std::string_view name, float intensity, uint64_t timeUntil, uint64_t now) {
  if(!isActive())
    return;
  const size_t id = profile->index(name);
  if(id==size_t(-1))
    return;
  const auto& m    = profile->anims[id];
  const bool  stop = timeUntil<now;  // the anim is stopped (a time of 0)
  if(timeUntil==uint64_t(-1) && m.duration>0)
    timeUntil = now + m.duration;
  // held for its time, then fades out
  if(!stop && timeUntil!=uint64_t(-1) && m.blendOut>0)
    timeUntil += uint64_t(std::lround(1.f/m.blendOut));  // rounded: 1/(1/40 ms) is 39.99 in floats

  // another anim of the layer gives way: S_ANGRY replaces S_FRIENDLY
  if(!stop) {
    for(auto& s:slots) {
      if(s.anim==id || !isOn(s,now) || s.manual || profile->anims[s.anim].layer!=m.layer)
        continue;
      fadeOut(s,now);
      }
    }

  // the anim is on already (or still fading out): it goes on from its weight
  for(auto& s:slots) {
    if(s.anim!=id || !isOn(s,now))
      continue;
    if(stop) {
      fadeOut(s,now);
      return;
      }
    if(m.blendIn>0) {
      const float w = s.manual ? 1.f : fade(s,now);
      s.timeStart = now - std::min(now, uint64_t(std::ceil(w/m.blendIn)));
      }
    s.timeUntil = timeUntil;
    s.intensity = intensity;
    s.manual    = false;
    return;
    }
  if(stop)
    return;

  auto& s = freeSlot(now);
  s = Slot();
  s.anim      = id;
  s.timeStart = now;
  s.timeUntil = timeUntil;
  s.intensity = intensity;
  s.on        = true;
  }

void FaceRig::setShape(std::string_view name, uint32_t frame0, uint32_t frame1, float alpha, float intensity, uint64_t now) {
  if(!isActive())
    return;
  const size_t id = profile->index(name);
  if(id==size_t(-1))
    return;
  // the layer of the anim, or a free slot
  const auto& m    = profile->anims[id];
  Slot*       slot = nullptr;
  for(auto& s:slots)
    if(isOn(s,now) && profile->anims[s.anim].layer==m.layer) {
      slot = &s;
      break;
      }
  if(slot==nullptr) {
    slot = &freeSlot(now);
    slot->on = false;
    }
  const uint32_t last = uint32_t(m.frames.size()-1);
  if(slot->anim!=id || !isOn(*slot,now))
    slot->timeStart = now;
  slot->anim      = id;
  slot->timeUntil = now + ShapeHold;  // the owner sets the shape every frame; if it stops, the shape goes
  slot->intensity = std::clamp(intensity,0.f,1.f);
  slot->on        = true;
  slot->manual    = true;
  slot->frame0    = std::min(frame0,last);
  slot->frame1    = std::min(frame1,last);
  slot->alpha     = std::clamp(alpha,0.f,1.f);
  }

float FaceRig::weight(std::string_view name, uint64_t now) const {
  if(!isActive())
    return 0;
  float w = 0;
  for(auto& s:slots)
    if(isOn(s,now) && profile->anims[s.anim].name==name)
      w = std::max(w, s.intensity*fade(s,now));
  return w;
  }

std::vector<float> FaceRig::strength(std::string_view name) const {
  if(!isActive())
    return {};
  if(auto a = profile->find(name))
    return a->strength;
  return {};
  }

void FaceRig::setEyes(const EyeTurn& left, const EyeTurn& right) {
  if(!hasEyes())
    return;
  eyeWant[0] = left;
  eyeWant[1] = right;
  }

void FaceRig::mix(uint64_t now, std::vector<float>& out) const {
  std::fill(out.begin(),out.end(),0.f);
  // a voice takes the mouth: the mouth shapes of the other layers show only in part
  float speech = 0;
  if(speechLayer>=0)
    for(auto& s:slots)
      if(isOn(s,now) && profile->anims[s.anim].layer==speechLayer)
        speech = std::max(speech, s.intensity*fade(s,now));
  const float quiet = 1.f - SpeechMouth*std::clamp(speech,0.f,1.f);

  for(auto& s:slots) {
    const float k = s.intensity*fade(s,now);
    if(k<=0)
      continue;
    const auto& a = profile->anims[s.anim];
    const auto  n = a.frames.size();
    size_t f0 = 0, f1 = 0;
    float  alpha = 0;
    if(s.manual) {
      f0    = s.frame0;
      f1    = s.frame1;
      alpha = s.alpha;
      } else {
      const uint64_t time = now-s.timeStart;
      f0    = size_t((time/a.frameMs+0)%n);
      f1    = size_t((time/a.frameMs+1)%n);
      alpha = float(time%a.frameMs)/float(a.frameMs);
      }
    const bool damp = speechLayer>=0 && a.layer!=speechLayer;
    for(auto [frame,part]:{std::pair{f0,1.f-alpha},std::pair{f1,alpha}}) {
      if(part<=0)
        continue;
      for(auto& [t,w]:a.frames[frame])
        out[t] += w*k*part*(damp && profile->mouth[t]!=0 ? quiet : 1.f);
      }
    }
  // the lids follow the eyes: per direction of each eye the shapes of the profile's table
  if(profile->eyes) {
    using Eyes = FaceProfile::Eyes;
    constexpr float Deg = float(M_PI)/180.f;
    for(int s=0; s<2; ++s) {
      const auto& e     = profile->eyes->eye[s];
      const float yo    = eyeWant[s].yaw*e.outSign/Deg;  // + outwards
      const float pitch = eyeWant[s].pitch/Deg;
      float       a[4]  = {};
      a[Eyes::Down] = std::max(0.f,-pitch);
      a[Eyes::Up]   = std::max(0.f, pitch);
      a[Eyes::In]   = std::max(0.f,-yo);
      a[Eyes::Out]  = std::max(0.f, yo);
      for(int d=0; d<4; ++d)
        for(auto& l:e.lids[d])
          out[l.target] += l.max*std::clamp((a[d]-l.from)/(l.to-l.from),0.f,1.f);
      }
    }
  for(auto& w:out)
    w = std::clamp(w,0.f,1.f);
  }

FaceRig::Result FaceRig::update(uint64_t now) {
  if(!isActive())
    return Result::Same;
  mix(now,want);

  // the eyes turned since the head was deformed
  bool turned = false, straight = true;
  if(hasEyes())
    for(int i=0; i<2; ++i) {
      turned   = turned || std::abs(eyeWant[i].yaw-eyeShown[i].yaw)>EyeChanged ||
                           std::abs(eyeWant[i].pitch-eyeShown[i].pitch)>EyeChanged;
      straight = straight && eyeWant[i].yaw==0.f && eyeWant[i].pitch==0.f;
      }

  bool zero = straight;
  for(auto w:want)
    if(w>0) {
      zero = false;
      break;
      }
  if(zero) {
    if(!deformed && !dirty)
      return Result::Same;
    applied  = want;
    eyeShown[0] = eyeShown[1] = EyeTurn();
    deformed = false;
    dirty    = false;
    return Result::Rest;
    }

  bool changed = dirty || !deformed;
  for(size_t i=0; i<want.size() && !changed; ++i)
    changed = std::abs(want[i]-applied[i])>Changed;
  if(!changed && !turned)
    return Result::Same;
  const bool whole = dirty || !deformed;  // the view has none of it: a rest or a new view
  if(hasEyes()) {
    // the rotation of each ball: pitch around y (z to x), then yaw around x (z to y); forward (0,0,1) goes to
    // (sin pitch, cos pitch sin yaw, cos pitch cos yaw)
    for(int i=0; i<2; ++i) {
      eyeShown[i] = eyeWant[i];
      const float cy = std::cos(eyeShown[i].yaw),   sy = std::sin(eyeShown[i].yaw);
      const float cp = std::cos(eyeShown[i].pitch), sp = std::sin(eyeShown[i].pitch);
      const float r[3][3] = {
        {cp,      0,  sp   },
        {-sy*sp,  cy, sy*cp},
        {-cy*sp, -sy, cy*cp},
        };
      std::copy(&r[0][0],&r[0][0]+9,&eyeTurn[i][0][0]);
      }
    }
  if(changed) {
    applied = want;
    blend();
    } else {
    turnEyes();
    }
  if(whole) {
    changedFirst = 0;
    changedEnd   = uint32_t(head->renderVertexIds.size());
    }
  deformed = true;
  dirty    = false;
  return Result::Deformed;
  }

void FaceRig::prepare() {
  // the neutral head once: logical vertices with normalized normals, every drawn vertex laid out
  const auto&  mesh  = head->mesh;
  const size_t count = mesh.positions.size();
  basePos.resize(count);
  baseNrm.resize(count);
  for(size_t i=0; i<count; ++i) {
    const auto& p = mesh.positions[i];
    basePos[i] = Vec3(p.x,p.y,p.z);
    Vec3 n;
    if(i<mesh.normals.size())
      n = Vec3(mesh.normals[i].x,mesh.normals[i].y,mesh.normals[i].z);
    const float l = n.quadLength();
    baseNrm[i] = l>1e-12f ? n/std::sqrt(l) : n;
    }
  pos = basePos;
  nrm = baseNrm;
  mark.assign(count,0);
  moved.clear();

  deltas.assign(head->targets.size(),{});
  for(size_t t=0; t<head->targets.size(); ++t)
    for(auto& d:head->targets[t].vertices) {
      const auto& p = d.position;
      const auto& n = d.normal;
      if(d.vertex>=count)
        continue;
      if(std::max({std::abs(p.x),std::abs(p.y),std::abs(p.z)})<StillCm &&
         std::max({std::abs(n.x),std::abs(n.y),std::abs(n.z)})<StillNormal)
        continue;
      deltas[t].push_back({d.vertex,Vec3(p.x,p.y,p.z),Vec3(n.x,n.y,n.z)});
      }

  const auto& ids = head->renderVertexIds;
  drawnFirst.assign(count+1,0);
  for(auto id:ids)
    if(id<count)
      ++drawnFirst[id+1];
  for(size_t i=0; i<count; ++i)
    drawnFirst[i+1] += drawnFirst[i];
  drawn.resize(drawnFirst[count]);
  std::vector<uint32_t> at(drawnFirst.begin(),drawnFirst.end()-1);
  // the drawn vertices: position (w = 1, as cloth) and normal per vertex, zeros for the padding of a meshlet
  deform.assign(ids.size()*2,Vec4());
  for(size_t i=0; i<ids.size(); ++i) {
    const uint32_t id = ids[i];
    if(id>=count)
      continue;
    drawn[at[id]++] = uint32_t(i);
    deform[i*2+0] = Vec4(basePos[id].x,basePos[id].y,basePos[id].z,1.f);
    deform[i*2+1] = Vec4(baseNrm[id].x,baseNrm[id].y,baseNrm[id].z,0.f);
    }
  }

void FaceRig::write(uint32_t v) {
  Vec3 p = pos[v];
  Vec3 n = nrm[v];
  if(const auto* ey = eyes())
    for(int i=0; i<2; ++i) {
      const auto& e = ey->eye[i];
      if(v<e.first || v>=e.end)
        continue;
      // a rigid turn of the ball around its centre
      const auto& r = eyeTurn[i];
      const Vec3  d = p - e.center;
      p = e.center + Vec3(r[0][0]*d.x + r[0][1]*d.y + r[0][2]*d.z,
                          r[1][0]*d.x + r[1][1]*d.y + r[1][2]*d.z,
                          r[2][0]*d.x + r[2][1]*d.y + r[2][2]*d.z);
      n = Vec3(r[0][0]*n.x + r[0][1]*n.y + r[0][2]*n.z,
               r[1][0]*n.x + r[1][1]*n.y + r[1][2]*n.z,
               r[2][0]*n.x + r[2][1]*n.y + r[2][2]*n.z);
      break;
      }
  for(uint32_t i=drawnFirst[v]; i<drawnFirst[v+1]; ++i) {
    const uint32_t r = drawn[i];
    changedFirst = std::min(changedFirst,r);
    changedEnd   = std::max(changedEnd,r+1);
    deform[size_t(r)*2+0] = Vec4(p.x,p.y,p.z,1.f);
    deform[size_t(r)*2+1] = Vec4(n.x,n.y,n.z,0.f);
    }
  }

void FaceRig::blend() {
  // only the vertices the shapes move: those moved before go back to neutral, those moved now get the weighted
  // deltas, normals normalized after the sum, both written to their drawn vertices
  const auto start = std::chrono::steady_clock::now();
  if(basePos.size()!=head->mesh.positions.size() || deform.size()!=head->renderVertexIds.size()*2)
    prepare();
  changedFirst = uint32_t(-1);
  changedEnd   = 0;
  for(auto v:moved) {
    pos[v]  = basePos[v];
    nrm[v]  = baseNrm[v];
    mark[v] = 1;
    }
  for(size_t t=0; t<applied.size(); ++t) {
    const float w = applied[t];
    if(w<=0)
      continue;
    for(auto& d:deltas[t]) {
      const uint32_t v = d.vertex;
      if(mark[v]!=2) {
        if(mark[v]==0)
          moved.push_back(v);
        mark[v] = 2;
        }
      pos[v] += d.position*w;
      nrm[v] += d.normal*w;
      }
    }
  size_t keep = 0;
  for(size_t i=0; i<moved.size(); ++i) {
    const uint32_t v = moved[i];
    if(mark[v]==2) {
      auto&       n = nrm[v];
      const float l = n.quadLength();
      if(l>1e-12f)
        n = n/std::sqrt(l);
      moved[keep++] = v;
      }
    write(v);  // back to neutral when no shape moves it now
    mark[v] = 0;
    }
  moved.resize(keep);
  writeEyes();
  blendUs = float(std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-start).count());
  ++blends;
  }

void FaceRig::writeEyes() {
  if(const auto* ey = eyes())
    for(auto& e:ey->eye)
      for(uint32_t v=e.first; v<e.end; ++v)
        write(v);
  }

void FaceRig::turnEyes() {
  // only the balls turned: their vertices, the rest of the head as it is
  if(basePos.size()!=head->mesh.positions.size() || deform.size()!=head->renderVertexIds.size()*2)
    prepare();
  changedFirst = uint32_t(-1);
  changedEnd   = 0;
  writeEyes();
  }

void FaceRig::probe(Probe& p, uint64_t now) const {
  p = Probe();
  p.valid = isActive();
  if(!p.valid)
    return;
  for(auto& s:slots) {
    const float k = s.intensity*fade(s,now);
    if(k<=0)
      continue;
    ++p.anims;
    char buf[64];
    std::snprintf(buf,sizeof(buf),"%s%s %.2f",p.shown.empty() ? "" : " ",profile->anims[s.anim].name.c_str(),double(k));
    p.shown += buf;
    }
  for(auto w:applied)
    if(w>0)
      ++p.targets;
  p.blendUs = blendUs;
  p.blends  = blends;
  }
