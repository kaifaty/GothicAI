#include "facerig.h"

#include "gltfmesh.h"

#include <rapidjson/document.h>
#include <rapidjson/error/en.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
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

size_t targetOf(const GltfHeadMesh& head, std::string_view name) {
  for(size_t t=0; t<head.targets.size(); ++t)
    if(head.targets[t].name==name)
      return t;
  return size_t(-1);
  }

// the name of the same shape on the other side: browDownLeft <-> browDownRight (as cues.py mirror)
std::string mirrorName(std::string_view name) {
  std::string ret;
  for(size_t i=0; i<name.size();) {
    if(name.compare(i,4,"Left")==0) {
      ret += "Right";
      i   += 4;
      }
    else if(name.compare(i,5,"Right")==0) {
      ret += "Left";
      i   += 5;
      }
    else {
      ret += name[i];
      ++i;
      }
    }
  return ret;
  }

// a smooth step 0..1 (cues.py ease)
float ease(float x) {
  x = std::clamp(x,0.f,1.f);
  return x*x*(3.f-2.f*x);
  }

void parseCues(const rapidjson::Value& list, const GltfHeadMesh& head, FaceProfile& ret) {
  if(!list.IsObject())
    fail("cues must be an object");
  for(const auto& entry:list.GetObject()) {
    const auto  name  = str(entry.name);
    const auto  where = "cue "+quoted(name);
    const auto& v     = entry.value;
    if(name.empty())
      fail("a cue without a name");
    if(ret.findCue(name)!=nullptr)
      fail(where+": listed twice");
    if(!v.IsObject())
      fail(where+": must be an object");
    for(const auto& m:v.GetObject()) {
      const auto key = str(m.name);
      if(key!="zone" && key!="side" && key!="parts")
        fail(where+": unknown field "+quoted(key));
      }
    FaceProfile::Cue c;
    c.name = std::string(name);
    const auto zone = v.HasMember("zone") && v["zone"].IsString() ? str(v["zone"]) : std::string_view();
    if(zone!="upper" && zone!="mouth")
      fail(where+": zone must be \"upper\" or \"mouth\"");
    c.mouth = zone=="mouth";
    if(v.HasMember("side")) {
      const auto side = v["side"].IsString() ? str(v["side"]) : std::string_view();
      if(side!="L" && side!="R")
        fail(where+": side must be \"L\" or \"R\"");
      c.side = side[0];
      }
    if(!v.HasMember("parts") || !v["parts"].IsArray() || v["parts"].Empty())
      fail(where+": parts must be a non-empty list");
    for(const auto& p:v["parts"].GetArray()) {
      const auto at = where+" part "+std::to_string(c.parts.size());
      if(!p.IsObject())
        fail(at+": must be an object");
      for(const auto& m:p.GetObject()) {
        const auto key = str(m.name);
        if(key!="weights" && key!="delay_ms" && key!="on_ms" && key!="hold_ms" && key!="off_ms")
          fail(at+": unknown field "+quoted(key));
        }
      FaceProfile::Cue::Part part;
      part.delayMs = float(number(p,"delay_ms",at));
      part.onMs    = float(number(p,"on_ms",at));
      part.holdMs  = float(number(p,"hold_ms",at));
      part.offMs   = float(number(p,"off_ms",at));
      if(part.delayMs<0 || part.holdMs<0)
        fail(at+": delay_ms and hold_ms must not be negative");
      if(part.onMs<=0 || part.offMs<=0)
        fail(at+": on_ms and off_ms must be above 0");
      if(!p.HasMember("weights") || !p["weights"].IsObject() || p["weights"].MemberCount()==0)
        fail(at+": weights must be a non-empty object of target weights");
      for(const auto& w:p["weights"].GetObject()) {
        const auto target = str(w.name);
        const auto id     = targetOf(head,target);
        if(id==size_t(-1))
          fail(at+": unknown target "+quoted(target));
        if(!w.value.IsNumber() || !std::isfinite(w.value.GetDouble()) || w.value.GetDouble()<0 || w.value.GetDouble()>1)
          fail(at+": the weight of "+quoted(target)+" must be a number 0..1");
        const float weight = float(w.value.GetDouble());
        part.weights.emplace_back(uint32_t(id),weight);
        // a one-sided gesture is played on either side: its shapes must have their other side on the head
        const auto other = c.side!=0 ? mirrorName(target) : std::string(target);
        const auto oid   = targetOf(head,other);
        if(oid==size_t(-1))
          fail(at+": unknown target "+quoted(std::string_view(other))+" (the other side of "+quoted(target)+")");
        part.mirrored.emplace_back(uint32_t(oid),weight);
        }
      c.parts.push_back(std::move(part));
      }
    ret.cues.push_back(std::move(c));
    }
  }

void parseLeaks(const rapidjson::Value& v, const GltfHeadMesh& head, FaceProfile& ret) {
  // {"on_ms": 120, "hold_ms": 120, "off_ms": 160, "felt": {"afraid": {"upper": {target: weight}, "lower": {...}}}}
  if(!v.IsObject())
    fail("leaks must be an object");
  for(const auto& m:v.GetObject()) {
    const auto key = str(m.name);
    if(key!="on_ms" && key!="hold_ms" && key!="off_ms" && key!="felt")
      fail("leaks: unknown field "+quoted(key));
    }
  auto& time = ret.leakTime;
  time.onMs   = float(number(v,"on_ms","leaks"));
  time.holdMs = float(number(v,"hold_ms","leaks"));
  time.offMs  = float(number(v,"off_ms","leaks"));
  if(time.holdMs<0)
    fail("leaks: hold_ms must not be negative");
  if(time.onMs<=0 || time.offMs<=0)
    fail("leaks: on_ms and off_ms must be above 0");
  if(!v.HasMember("felt") || !v["felt"].IsObject())
    fail("leaks: felt must be an object of the felt moods");
  for(const auto& f:v["felt"].GetObject()) {
    const auto felt  = str(f.name);
    const auto where = "leak "+quoted(felt);
    if(!FaceProfile::isFelt(felt))
      fail(where+": unknown feeling (friendly, angry, hostile, afraid, pain)");
    if(!f.value.IsObject())
      fail(where+": must be an object of the halves upper and lower");
    for(const auto& h:f.value.GetObject()) {
      const auto half = str(h.name);
      const auto at   = where+" "+std::string(half);
      if(half!="upper" && half!="lower")
        fail(where+": unknown half "+quoted(half)+" (upper or lower)");
      if(!h.value.IsObject() || h.value.MemberCount()==0)
        fail(at+": must be a non-empty object of target weights");
      FaceProfile::Leak l;
      l.felt  = std::string(felt);
      l.lower = half=="lower";
      for(const auto& w:h.value.GetObject()) {
        const auto target = str(w.name);
        const auto id     = targetOf(head,target);
        if(id==size_t(-1))
          fail(at+": unknown target "+quoted(target));
        if(!w.value.IsNumber() || !std::isfinite(w.value.GetDouble()) || w.value.GetDouble()<0 || w.value.GetDouble()>1)
          fail(at+": the weight of "+quoted(target)+" must be a number 0..1");
        l.weights.emplace_back(uint32_t(id),float(w.value.GetDouble()));
        l.peak = std::max(l.peak,float(w.value.GetDouble()));
        }
      ret.leaks.push_back(std::move(l));
      }
    }
  }

}

bool FaceProfile::isFelt(std::string_view felt) {
  for(auto f:Felt)
    if(f==felt)
      return true;
  return false;
  }

const FaceProfile::Leak* FaceProfile::findLeak(std::string_view felt, bool lower) const {
  for(auto& l:leaks)
    if(l.felt==felt && l.lower==lower)
      return &l;
  return nullptr;
  }

float FaceProfile::Cue::span(float hold) const {
  float ret = 0;
  for(size_t i=0; i<parts.size(); ++i) {
    const auto& p = parts[i];
    ret = std::max(ret, p.delayMs+p.onMs+(i==0 && hold>=0 ? hold : p.holdMs)+p.offMs);
    }
  return ret;
  }

const FaceProfile::Cue* FaceProfile::findCue(std::string_view name) const {
  for(auto& c:cues)
    if(c.name==name)
      return &c;
  return nullptr;
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
    if(key!="version" && key!="about" && key!="mouth" && key!="anims" && key!="cues" && key!="leaks")
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
        const auto id     = targetOf(head,target);
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
  if(doc.HasMember("cues"))
    parseCues(doc["cues"],head,*ret);
  if(doc.HasMember("leaks"))
    parseLeaks(doc["leaks"],head,*ret);
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
  runs.clear();
  leakRun  = LeakRun();
  posed    = false;
  posedEnd = 0;
  squint.clear();
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
  if(!isActive())
    return;
  if(auto v = profile->find("VISEME"))
    speechLayer = v->layer;
  want   .assign(head->targets.size(),0.f);
  applied.assign(head->targets.size(),0.f);
  squint .assign(head->targets.size(),0);
  for(size_t t=0; t<head->targets.size(); ++t) {
    const auto& n = head->targets[t].name;
    squint[t] = n.compare(0,9,"eyeSquint")==0 || n.compare(0,11,"cheekSquint")==0;
    }
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

bool FaceRig::startCue(std::string_view name, bool mirror, float hold, uint64_t start, uint64_t now) {
  if(!isActive())
    return false;
  const auto* c = profile->findCue(name);
  if(c==nullptr)
    return false;
  // several gestures, the same one too, may go at once; the ended ones go in update()
  CueRun r;
  r.cue    = size_t(c-profile->cues.data());
  r.mirror = mirror && c->side!=0;
  r.hold   = hold;
  r.start  = std::min(start,now);
  runs.push_back(r);
  return true;
  }

bool FaceRig::startLeak(std::string_view felt, bool lower, uint64_t start, uint64_t now) {
  if(!isActive())
    return false;
  const auto* l = profile->findLeak(felt,lower);
  if(l==nullptr)
    return false;
  leakRun.leak  = size_t(l-profile->leaks.data());
  leakRun.start = std::min(start,now);
  leakRun.on    = true;
  return true;
  }

float FaceRig::leakEnvelope(uint64_t now) const {
  // as a cue's part (cues.py env)
  if(!leakRun.on || now<=leakRun.start)
    return 0;
  const auto& lt = profile->leakTime;
  const float t  = float(now-leakRun.start);
  return ease(t/lt.onMs)*(1.f-ease((t-lt.onMs-lt.holdMs)/lt.offMs));
  }

float FaceRig::leakWeight(uint64_t now) const {
  if(!isActive() || !leakRun.on)
    return 0;
  return leakEnvelope(now)*profile->leaks[leakRun.leak].peak;
  }

void FaceRig::setPosed(bool on, uint64_t now) {
  if(on==posed)
    return;
  posed = on;
  if(!on)
    posedEnd = now;
  }

float FaceRig::posedCut(uint64_t now) const {
  if(posed)
    return 1;
  if(posedEnd==0 || now>=posedEnd+PosedFadeMs)
    return 0;
  return 1.f-float(now>posedEnd ? now-posedEnd : 0)/float(PosedFadeMs);
  }

float FaceRig::cueEnvelope(const CueRun& r, size_t part, uint64_t now) const {
  // cues.py env: a smooth step up over on_ms, the hold, a smooth step down over off_ms
  const auto& p    = profile->cues[r.cue].parts[part];
  const float hold = part==0 && r.hold>=0 ? r.hold : p.holdMs;
  const float t    = float(now>=r.start ? now-r.start : 0) - p.delayMs;
  if(now<r.start || t<=0)
    return 0;
  return ease(t/p.onMs)*(1.f-ease((t-p.onMs-hold)/p.offMs));
  }

float FaceRig::cueStrength(const CueRun& r, uint64_t now) const {
  float ret = 0;
  for(size_t i=0; i<profile->cues[r.cue].parts.size(); ++i)
    ret = std::max(ret,cueEnvelope(r,i,now));
  return ret;
  }

float FaceRig::cueWeight(std::string_view name, uint64_t now) const {
  if(!isActive())
    return 0;
  float w = 0;
  for(auto& r:runs)
    if(profile->cues[r.cue].name==name)
      w = std::max(w,cueStrength(r,now));
  return w;
  }

std::vector<float> FaceRig::strength(std::string_view name) const {
  if(!isActive())
    return {};
  if(auto a = profile->find(name))
    return a->strength;
  return {};
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
  // a posed smile: the squint of the expression cut
  const float keep  = 1.f - posedCut(now);

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
        out[t] += w*k*part*(damp && profile->mouth[t]!=0 ? quiet : 1.f)*(squint[t]!=0 ? keep : 1.f);
      }
    }
  // the gestures over all of it, not quieted by the voice (the mouth ones go into its pauses anyway)
  for(auto& r:runs) {
    const auto& c = profile->cues[r.cue];
    for(size_t i=0; i<c.parts.size(); ++i) {
      const float e = cueEnvelope(r,i,now);
      if(e<=0)
        continue;
      for(auto& [t,w]:(r.mirror ? c.parts[i].mirrored : c.parts[i].weights))
        out[t] += w*e;
      }
    }
  // a leak the same way: added to all of it, putting out nothing
  if(const float e = leakEnvelope(now); e>0)
    for(auto& [t,w]:profile->leaks[leakRun.leak].weights)
      out[t] += w*e;
  for(auto& w:out)
    w = std::clamp(w,0.f,1.f);
  }

FaceRig::Result FaceRig::update(uint64_t now) {
  if(!isActive())
    return Result::Same;
  runs.erase(std::remove_if(runs.begin(),runs.end(),[&](const CueRun& r) {
    return now>r.start && float(now-r.start)>profile->cues[r.cue].span(r.hold);
    }),runs.end());
  if(leakRun.on && now>leakRun.start && float(now-leakRun.start)>profile->leakTime.span())
    leakRun.on = false;
  mix(now,want);

  bool zero = true;
  for(auto w:want)
    if(w>0) {
      zero = false;
      break;
      }
  if(zero) {
    if(!deformed && !dirty)
      return Result::Same;
    applied  = want;
    deformed = false;
    dirty    = false;
    return Result::Rest;
    }

  bool changed = dirty || !deformed;
  for(size_t i=0; i<want.size() && !changed; ++i)
    changed = std::abs(want[i]-applied[i])>Changed;
  if(!changed)
    return Result::Same;
  const bool whole = dirty || !deformed;  // the view has none of it: a rest or a new view
  applied = want;
  blend();
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
  const auto& p = pos[v];
  const auto& n = nrm[v];
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
  blendUs = float(std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-start).count());
  ++blends;
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
  for(auto& r:runs) {
    const float k = cueStrength(r,now);
    if(k<=0)
      continue;
    ++p.cues;
    const auto& c = profile->cues[r.cue];
    // a one-sided one with the side it is shown on: "doubt:R 0.64"
    const char* side = c.side==0 ? "" : ((c.side=='L')!=r.mirror ? ":L" : ":R");
    char buf[64];
    std::snprintf(buf,sizeof(buf),"%s%s%s %.2f",p.shown.empty() ? "" : " ",c.name.c_str(),side,double(k));
    p.shown += buf;
    }
  if(const float k = leakWeight(now); k>0) {
    const auto& l = profile->leaks[leakRun.leak];
    char buf[64];
    std::snprintf(buf,sizeof(buf),"%sleak:%s:%s %.2f",p.shown.empty() ? "" : " ",l.felt.c_str(),l.lower ? "lower" : "upper",double(k));
    p.shown += buf;
    }
  for(auto w:applied)
    if(w>0)
      ++p.targets;
  p.blendUs = blendUs;
  p.blends  = blends;
  }
