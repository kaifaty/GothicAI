#include "facerig.h"

#include "eyes.h"
#include "gltfmesh.h"
#include "graphics/eyeshader.h"

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

FaceProfile::Cue::Look parseLook(const rapidjson::Value& v, const std::string& where);

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
      if(key!="zone" && key!="side" && key!="parts" && key!="look")
        fail(where+": unknown field "+quoted(key));
      }
    FaceProfile::Cue c;
    c.name = std::string(name);
    const auto zone = v.HasMember("zone") && v["zone"].IsString() ? str(v["zone"]) : std::string_view();
    if(zone!="upper" && zone!="mouth" && zone!="eyes")
      fail(where+": zone must be \"upper\", \"mouth\" or \"eyes\"");
    c.mouth = zone=="mouth";
    if(zone=="eyes") {
      if(!v.HasMember("look"))
        fail(where+": a gesture of the eyes must have look");
      c.look = parseLook(v["look"],where+": look");
      }
    else if(v.HasMember("look")) {
      fail(where+": only a gesture of the eyes (zone \"eyes\") has look");
      }
    if(v.HasMember("side")) {
      const auto side = v["side"].IsString() ? str(v["side"]) : std::string_view();
      if(side!="L" && side!="R")
        fail(where+": side must be \"L\" or \"R\"");
      c.side = side[0];
      }
    if(c.look.has_value() && !v.HasMember("parts")) {
      ret.cues.push_back(std::move(c));
      continue;  // the eyes alone
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

void onlyFields(const rapidjson::Value& obj, std::initializer_list<std::string_view> keys, const std::string& where) {
  for(const auto& m:obj.GetObject()) {
    const auto key = str(m.name);
    if(std::find(keys.begin(),keys.end(),key)==keys.end())
      fail(where+": unknown field "+quoted(key));
    }
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

FaceProfile::Cue::Look parseLook(const rapidjson::Value& v, const std::string& where) {
  // {"move": "jump"|"arc", "side_deg": 22, "up_deg": -4, "on_ms": 80, "hold_ms": 600, "off_ms": 120}
  using Look = FaceProfile::Cue::Look;
  if(!v.IsObject())
    fail(where+" must be an object");
  onlyFields(v,{"move","side_deg","up_deg","on_ms","hold_ms","off_ms"},where);
  Look ret;
  const auto move = v.HasMember("move") && v["move"].IsString() ? str(v["move"]) : std::string_view();
  if(move!="jump" && move!="arc")
    fail(where+": move must be \"jump\" or \"arc\"");
  ret.move    = move=="jump" ? Look::Move::Jump : Look::Move::Arc;
  ret.sideDeg = float(number(v,"side_deg",where));
  ret.upDeg   = float(number(v,"up_deg",where));
  ret.onMs    = float(number(v,"on_ms",where));
  ret.holdMs  = float(number(v,"hold_ms",where));
  ret.offMs   = float(number(v,"off_ms",where));
  if(ret.sideDeg<0 || ret.sideDeg>90 || std::abs(ret.upDeg)>90)
    fail(where+": side_deg must be 0..90 and up_deg -90..90");
  if(std::sqrt(ret.sideDeg*ret.sideDeg + ret.upDeg*ret.upDeg)<Eyes::Saccade)
    fail(where+": the eyes must turn at least "+std::to_string(int(Eyes::Saccade))+" degree");
  if(ret.onMs<=0 || ret.offMs<=0 || ret.holdMs<0)
    fail(where+": on_ms and off_ms must be above 0, hold_ms not negative");
  if(ret.move==Look::Move::Jump) {
    // the saccades of the main sequence must fit their time
    const float ms = Eyes::duration(std::sqrt(ret.sideDeg*ret.sideDeg + ret.upDeg*ret.upDeg));
    if(ms>ret.onMs || ms>ret.offMs)
      fail(where+": a jump that far takes "+std::to_string(int(std::ceil(ms)))+" ms, longer than on_ms or off_ms");
    }
  return ret;
  }

// the gestures of the eyes within the limits of the eyeballs; without them such gestures are left out
void checkLooks(FaceProfile& p) {
  using E = FaceProfile::Eyes;
  if(!p.eyes.has_value()) {
    p.cues.erase(std::remove_if(p.cues.begin(),p.cues.end(),[](const FaceProfile::Cue& c) { return c.look.has_value(); }),
                 p.cues.end());
    return;
    }
  constexpr float Deg = float(M_PI)/180.f;
  const auto& lim  = p.eyes->limit;
  const float side = std::min(lim[E::In],lim[E::Out])/Deg;  // one eye goes out, the other one in
  for(auto& c:p.cues) {
    if(!c.look.has_value())
      continue;
    const auto& l = *c.look;
    if(l.sideDeg>side || l.upDeg>lim[E::Up]/Deg || -l.upDeg>lim[E::Down]/Deg)
      fail("cue "+quoted(std::string_view(c.name))+": look: the turn must be within limits_deg of eyes");
    }
  }

std::vector<std::pair<uint32_t,float>> parseWeights(const rapidjson::Value& v, const GltfHeadMesh& head, const std::string& where) {
  if(!v.IsObject() || v.MemberCount()==0)
    fail(where+" must be a non-empty object of target weights");
  std::vector<std::pair<uint32_t,float>> ret;
  for(const auto& w:v.GetObject()) {
    const auto target = str(w.name);
    const auto id     = targetOf(head,target);
    if(id==size_t(-1))
      fail(where+": unknown target "+quoted(target));
    if(!w.value.IsNumber() || !std::isfinite(w.value.GetDouble()) || w.value.GetDouble()<0 || w.value.GetDouble()>1)
      fail(where+": the weight of "+quoted(target)+" must be a number 0..1");
    ret.emplace_back(uint32_t(id),float(w.value.GetDouble()));
    }
  return ret;
  }

// {"rise_st": 2.5, "rise_ms": 200, "syllable_ms": 150, "loud": 0.4, "lead_ms": 100, "on_ms": 120, "hold_ms": 120,
//  "off_ms": 300, "gap_ms": 700, "chance": 0.7, "up": {...}, "down": {...}, "quiet_cues": ["flash", ...]}
FaceProfile::Brows parseBrows(const rapidjson::Value& v, const GltfHeadMesh& head, std::vector<std::string>& quiet) {
  const std::string where = "speech_brows";
  if(!v.IsObject())
    fail(where+" must be an object");
  onlyFields(v,{"about","rise_st","rise_ms","syllable_ms","loud","lead_ms","on_ms","hold_ms","off_ms","gap_ms","chance",
                "up","down","quiet_cues"},where);
  if(v.HasMember("about") && !v["about"].IsString())
    fail(where+": about must be a string");
  FaceProfile::Brows ret;
  ret.riseSt     = float(number(v,"rise_st",where));
  ret.riseMs     = float(number(v,"rise_ms",where));
  ret.syllableMs = float(number(v,"syllable_ms",where));
  ret.loud       = float(number(v,"loud",where));
  ret.leadMs     = float(number(v,"lead_ms",where));
  ret.onMs       = float(number(v,"on_ms",where));
  ret.holdMs     = float(number(v,"hold_ms",where));
  ret.offMs      = float(number(v,"off_ms",where));
  ret.gapMs      = float(number(v,"gap_ms",where));
  ret.chance     = float(number(v,"chance",where));
  if(ret.riseSt<=0 || ret.riseSt>24)
    fail(where+": rise_st must be above 0 and up to 24 semitones");
  if(ret.riseMs<10 || ret.syllableMs<10)
    fail(where+": rise_ms and syllable_ms must be 10 ms or more");
  if(ret.loud<0 || ret.loud>1 || ret.chance<0 || ret.chance>1)
    fail(where+": loud and chance must be numbers 0..1");
  if(ret.leadMs<0 || ret.holdMs<0 || ret.gapMs<0)
    fail(where+": lead_ms, hold_ms and gap_ms must not be negative");
  if(ret.onMs<=0 || ret.offMs<=0)
    fail(where+": on_ms and off_ms must be above 0");
  if(!v.HasMember("up") || !v.HasMember("down"))
    fail(where+": up and down must be objects of target weights");
  ret.up   = parseWeights(v["up"],head,where+": up");
  ret.down = parseWeights(v["down"],head,where+": down");
  if(v.HasMember("quiet_cues")) {
    const auto& q = v["quiet_cues"];
    if(!q.IsArray())
      fail(where+": quiet_cues must be a list of cue names");
    for(const auto& n:q.GetArray()) {
      if(!n.IsString())
        fail(where+": quiet_cues must be a list of cue names");
      quiet.emplace_back(str(n));
      }
    }
  return ret;
  }

// {"chance": 0.45, "gap_ms": 4000, "flash": {"cue": "flash", "weight": 0.55, "time": 0.8},
//  "frown": {"cue": "frown", "weight": 0.6, "time": 0.4},
//  "smile": {"weight": 0.5, "on_ms": 400, "hold_ms": 700, "off_ms": 500, "chance": 0.5, "flash": 0.3}}
// after the cues and the anims: they are found by name
FaceProfile::Listen parseListen(const rapidjson::Value& v, const FaceProfile& p) {
  const std::string where = "listen_face";
  if(!v.IsObject())
    fail(where+" must be an object");
  onlyFields(v,{"about","chance","gap_ms","flash","frown","smile"},where);
  if(v.HasMember("about") && !v["about"].IsString())
    fail(where+": about must be a string");
  FaceProfile::Listen ret;
  ret.chance = float(number(v,"chance",where));
  ret.gapMs  = float(number(v,"gap_ms",where));
  if(ret.chance<0 || ret.chance>1)
    fail(where+": chance must be a number 0..1");
  if(ret.gapMs<0)
    fail(where+": gap_ms must not be negative");
  const auto gesture = [&](const char* key, FaceProfile::Listen::Gesture& g) {
    const std::string at = where+": "+key;
    if(!v.HasMember(key) || !v[key].IsObject())
      fail(at+" must be an object");
    const auto& o = v[key];
    onlyFields(o,{"cue","weight","time"},at);
    if(!o.HasMember("cue") || !o["cue"].IsString())
      fail(at+": cue must be the name of a cue");
    const auto  name = str(o["cue"]);
    const auto* c    = p.findCue(name);
    if(c==nullptr)
      fail(at+": no cue "+quoted(name));
    if(c->parts.empty())
      fail(at+": cue "+quoted(name)+" has no shapes");
    g.cue    = size_t(c-p.cues.data());
    g.weight = float(number(o,"weight",at));
    g.time   = float(number(o,"time",at));
    if(g.weight<=0 || g.weight>1 || g.time<=0 || g.time>1)
      fail(at+": weight and time must be above 0 and up to 1 (of the cue's)");
    };
  gesture("flash",ret.flash);
  gesture("frown",ret.frown);
  const std::string at = where+": smile";
  if(!v.HasMember("smile") || !v["smile"].IsObject())
    fail(at+" must be an object");
  const auto& sm = v["smile"];
  onlyFields(sm,{"weight","on_ms","hold_ms","off_ms","chance","flash"},at);
  ret.smileWeight = float(number(sm,"weight",at));
  ret.smileOnMs   = float(number(sm,"on_ms",at));
  ret.smileHoldMs = float(number(sm,"hold_ms",at));
  ret.smileOffMs  = float(number(sm,"off_ms",at));
  ret.smileChance = float(number(sm,"chance",at));
  ret.smileFlash  = float(number(sm,"flash",at));
  if(ret.smileWeight<=0 || ret.smileWeight>1)
    fail(at+": weight must be above 0 and up to 1 (of S_FRIENDLY's)");
  if(ret.smileOnMs<=0 || ret.smileOffMs<=0 || ret.smileHoldMs<0)
    fail(at+": on_ms and off_ms must be above 0, hold_ms not negative");
  if(ret.smileChance<0 || ret.smileChance>1 || ret.smileFlash<0 || ret.smileFlash>1)
    fail(at+": chance and flash must be numbers 0..1");
  const auto* friendly = p.find("S_FRIENDLY");
  if(friendly==nullptr)
    fail(at+": the profile has no S_FRIENDLY to smile by");
  for(auto& [t,w]:friendly->frames[0])
    ret.smile.emplace_back(t,w*ret.smileWeight);
  return ret;
  }

FaceProfile::Eyes::Shade parseLidShade(const rapidjson::Value& v, const GltfHeadMesh& head, const FaceProfile::Eyes& eyes) {
  const std::string at = "eyes: lid_shade";
  if(!v.IsObject())
    fail(at+" must be an object");
  onlyFields(v,{"about","part","upper_cm","lower_cm","upper_edge","lower_edge"},at);
  if(v.HasMember("about") && !v["about"].IsString())
    fail(at+": about must be a string");
  if(!v.HasMember("part") || !v["part"].IsString())
    fail(at+": part must be the name of the head part of the lids");
  FaceProfile::Eyes::Shade ret;
  const auto name = str(v["part"]);
  ret.part = head.parts.size();
  for(size_t i=0; i<head.parts.size(); ++i)
    if(head.parts[i].name==name)
      ret.part = i;
  if(ret.part==head.parts.size())
    fail(at+": no head part "+quoted(name));
  if(ret.part==eyes.eye[0].part || ret.part==eyes.eye[1].part)
    fail(at+": part must be the lids', not an eye");
  const auto cm = [&](const char* key, float& out) {
    const double x = number(v,key,at);
    if(x<=0 || x>double(eyes.eye[0].radius))
      fail(at+": "+key+" must be above 0 and up to the ball's radius");
    out = float(x);
    };
  const auto share = [&](const char* key, float& out) {
    const double x = number(v,key,at);
    if(x<0 || x>1)
      fail(at+": "+key+" must be a number 0..1");
    out = float(x);
    };
  cm   ("upper_cm",  ret.params.upperCm);
  cm   ("lower_cm",  ret.params.lowerCm);
  share("upper_edge",ret.params.upperEdge);
  share("lower_edge",ret.params.lowerEdge);

  // the part's triangles once, from the neutral head: those around each ball are its lids
  const auto& p     = head.parts[ret.part];
  const auto& mesh  = head.mesh;
  const size_t first = p.firstVertex, end = std::min(p.firstVertex+p.vertexCount,mesh.positions.size());
  std::vector<std::array<uint32_t,3>> tris;
  for(auto& s:mesh.sub_meshes)
    for(auto& t:s.triangles) {
      std::array<uint32_t,3> id;
      bool inside = true;
      for(size_t k=0; k<3; ++k) {
        const size_t w = t.wedges[k];
        id[k] = w<s.wedges.size() ? s.wedges[w].index : uint32_t(-1);
        inside = inside && id[k]>=first && id[k]<end;
        }
      if(inside)
        tris.push_back(id);
      }
  std::vector<Vec3> pos(mesh.positions.size());
  for(size_t i=0; i<pos.size(); ++i)
    pos[i] = Vec3(mesh.positions[i].x,mesh.positions[i].y,mesh.positions[i].z);
  const char* side[2] = {"left","right"};
  for(int s=0; s<2; ++s) {
    ret.lids[s] = LidShade::lids(tris,pos,eyes.eye[s].center,eyes.eye[s].radius);
    if(ret.lids[s].empty())
      fail(at+": part "+quoted(name)+" has no lids around the "+side[s]+" eye");
    }
  return ret;
  }

FaceProfile::Eyes parseEyes(const rapidjson::Value& v, const GltfHeadMesh& head) {
  using Eyes = FaceProfile::Eyes;
  if(!v.IsObject())
    fail("eyes must be an object");
  onlyFields(v,{"about","parts","limits_deg","lids","lid_shade"},"eyes");
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
        const size_t id = targetOf(head,name);
        if(id==size_t(-1))
          fail(at+": unknown target "+quoted(std::string_view(name)));
        lid.target = uint32_t(id);
        lid.from = float(from);
        lid.to   = float(to);
        lid.max  = float(max);
        ret.eye[s].lids[d].push_back(lid);
        }
      }
    }
  if(v.HasMember("lid_shade"))
    ret.shade = parseLidShade(v["lid_shade"],head,ret);
  return ret;
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
  float ret = look.has_value() ? look->span() : 0.f;
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
    if(key!="version" && key!="about" && key!="mouth" && key!="anims" && key!="cues" && key!="leaks" &&
       key!="eyes" && key!="speech_brows" && key!="listen_face")
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
  if(doc.HasMember("eyes"))
    ret->eyes = parseEyes(doc["eyes"],head);
  checkLooks(*ret);
  if(doc.HasMember("speech_brows")) {
    std::vector<std::string> quiet;
    ret->brows = parseBrows(doc["speech_brows"],head,quiet);
    for(auto& n:quiet) {
      const auto* c = ret->findCue(n);
      if(c==nullptr)
        fail("speech_brows: quiet_cues: no cue "+quoted(std::string_view(n)));
      ret->brows->quiet.push_back(size_t(c-ret->cues.data()));
      }
    }
  if(doc.HasMember("listen_face"))
    ret->listen = parseListen(doc["listen_face"],*ret);
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
  browRuns.clear();
  listenRuns.clear();
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
  for(int i=0; i<2; ++i) {
    eyeWant[i] = eyeShown[i] = EyeTurn();
    lidEdges[i] = LidShade();
    }
  lidShadeOn = eyeShaderEnabled();
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

void FaceRig::startBrows(bool down, float strength, uint64_t start, uint64_t now) {
  if(!isActive() || !profile->brows.has_value())
    return;
  BrowRun r;
  r.start    = std::min(start,now);
  r.down     = down;
  r.strength = std::clamp(strength,0.f,1.f);
  browRuns.push_back(r);
  }

float FaceRig::browEnvelope(const BrowRun& r, uint64_t now) const {
  // as a cue's part (cues.py env)
  if(now<=r.start)
    return 0;
  const auto& b = *profile->brows;
  const float t = float(now-r.start);
  return ease(t/b.onMs)*(1.f-ease((t-b.onMs-b.holdMs)/b.offMs));
  }

float FaceRig::browQuiet(uint64_t now) const {
  // a brow gesture or a leak of the upper face has the brows: silent from half of its rise on
  float other = 0;
  for(auto& r:runs)
    for(auto q:profile->brows->quiet)
      if(r.cue==q)
        other = std::max(other,cueStrength(r,now));
  if(leakRun.on && !profile->leaks[leakRun.leak].lower)
    other = std::max(other,leakEnvelope(now));
  return 1.f - std::min(1.f,2.f*other);
  }

float FaceRig::browWeight(uint64_t now) const {
  if(!isActive() || !profile->brows.has_value() || browRuns.empty())
    return 0;
  const float quiet = browQuiet(now);
  float       ret   = 0;
  for(auto& r:browRuns) {
    const float e = browEnvelope(r,now)*r.strength*quiet;
    if(e>std::abs(ret))
      ret = r.down ? -e : e;
    }
  return ret;
  }

void FaceRig::startListen(ListenKind kind, uint64_t start, uint64_t now) {
  if(!isActive() || !profile->listen.has_value())
    return;
  ListenRun r;
  r.kind  = kind;
  r.start = std::min(start,now);
  listenRuns.push_back(r);
  }

float FaceRig::listenEnvelope(const ListenRun& r, size_t part, uint64_t now) const {
  // as a cue's part (cues.py env): the flash and the frown on the times of their cue made shorter
  if(now<=r.start)
    return 0;
  const auto& l = *profile->listen;
  const float t = float(now-r.start);
  if(r.kind==ListenKind::Smile)
    return ease(t/l.smileOnMs)*(1.f-ease((t-l.smileOnMs-l.smileHoldMs)/l.smileOffMs));
  const auto& g = r.kind==ListenKind::Flash ? l.flash : l.frown;
  const auto& p = profile->cues[g.cue].parts[part];
  const float x = t - p.delayMs*g.time;
  if(x<=0)
    return 0;
  return ease(x/(p.onMs*g.time))*(1.f-ease((x-(p.onMs+p.holdMs)*g.time)/(p.offMs*g.time)));
  }

float FaceRig::listenSpan(const ListenRun& r) const {
  const auto& l = *profile->listen;
  if(r.kind==ListenKind::Smile)
    return l.smileSpan();
  const auto& g = r.kind==ListenKind::Flash ? l.flash : l.frown;
  return profile->cues[g.cue].span()*g.time;
  }

float FaceRig::speechWeight(uint64_t now) const {
  float ret = 0;
  if(speechLayer>=0)
    for(auto& s:slots)
      if(isOn(s,now) && profile->anims[s.anim].layer==speechLayer)
        ret = std::max(ret, s.intensity*fade(s,now));
  return std::clamp(ret,0.f,1.f);
  }

float FaceRig::listenQuiet(uint64_t now) const {
  // a gesture, a leak or a voice of its own take the face: silent from half of their rise on
  float other = speechWeight(now);
  for(auto& r:runs)
    other = std::max(other,cueStrength(r,now));
  if(leakRun.on)
    other = std::max(other,leakEnvelope(now));
  return 1.f - std::min(1.f,2.f*other);
  }

float FaceRig::listenWeight(uint64_t now) const {
  return std::max({listenWeight(ListenKind::Flash,now),listenWeight(ListenKind::Smile,now),listenWeight(ListenKind::Frown,now)});
  }

float FaceRig::listenWeight(ListenKind kind, uint64_t now) const {
  if(!isActive() || !profile->listen.has_value() || listenRuns.empty())
    return 0;
  float ret = 0;
  for(auto& r:listenRuns) {
    if(r.kind!=kind)
      continue;
    const size_t parts = kind==ListenKind::Smile ? 1 : profile->cues[(kind==ListenKind::Flash ? profile->listen->flash : profile->listen->frown).cue].parts.size();
    for(size_t i=0; i<parts; ++i)
      ret = std::max(ret,listenEnvelope(r,i,now));
    }
  return ret*listenQuiet(now);
  }

bool FaceRig::listenBusy(uint64_t now) const {
  if(!isActive())
    return true;
  if(!runs.empty() || leakRun.on || speechWeight(now)>0)
    return true;
  // an expression of a mood (the bridge's say.mood faces) stronger than the smile of the one listening
  const float smile = profile->listen.has_value() ? profile->listen->smileWeight : 0.f;
  for(auto name:{"S_FRIENDLY","S_ANGRY","S_HOSTILE","S_FRIGHTENED","T_HURT"})
    if(weight(name,now)>smile)
      return true;
  return false;
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

void FaceRig::setEyes(const EyeTurn& left, const EyeTurn& right) {
  if(!hasEyes())
    return;
  eyeWant[0] = left;
  eyeWant[1] = right;
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
  // the brows of the voice, the same way, unless a brow gesture or a leak of the upper face has them
  if(profile->brows && !browRuns.empty()) {
    const float quiet = browQuiet(now);
    for(auto& r:browRuns) {
      const float e = browEnvelope(r,now)*r.strength*quiet;
      if(e<=0)
        continue;
      for(auto& [t,w]:(r.down ? profile->brows->down : profile->brows->up))
        out[t] += w*e;
      }
    }
  // the face of the one listening, the same way, unless a gesture, a leak or its own voice has the face
  if(profile->listen && !listenRuns.empty()) {
    const auto& l     = *profile->listen;
    const float quiet = listenQuiet(now);
    for(auto& r:listenRuns) {
      if(r.kind==ListenKind::Smile) {
        const float e = listenEnvelope(r,0,now)*quiet;
        if(e>0)
          for(auto& [t,w]:l.smile)
            out[t] += w*e;
        continue;
        }
      const auto& g = r.kind==ListenKind::Flash ? l.flash : l.frown;
      const auto& c = profile->cues[g.cue];
      for(size_t i=0; i<c.parts.size(); ++i) {
        const float e = listenEnvelope(r,i,now)*quiet*g.weight;
        if(e<=0)
          continue;
        for(auto& [t,w]:c.parts[i].weights)
          out[t] += w*e;
        }
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
  runs.erase(std::remove_if(runs.begin(),runs.end(),[&](const CueRun& r) {
    return now>r.start && float(now-r.start)>profile->cues[r.cue].span(r.hold);
    }),runs.end());
  if(leakRun.on && now>leakRun.start && float(now-leakRun.start)>profile->leakTime.span())
    leakRun.on = false;
  if(profile->brows)
    browRuns.erase(std::remove_if(browRuns.begin(),browRuns.end(),[&](const BrowRun& r) {
      return now>r.start && float(now-r.start)>profile->brows->span();
      }),browRuns.end());
  if(profile->listen)
    listenRuns.erase(std::remove_if(listenRuns.begin(),listenRuns.end(),[&](const ListenRun& r) {
      return now>r.start && float(now-r.start)>listenSpan(r);
      }),listenRuns.end());
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
  // with the lids' shade the neutral face keeps its deformation: the shade is in it
  if(zero && !hasLidShade()) {
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
  lidVertex.assign(count,0);
  if(hasLidShade())
    for(auto& l:profile->eyes->shade->lids)
      for(auto v:l.verts)
        if(v<count)
          lidVertex[v] = 1;
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
  Vec3  p = pos[v];
  Vec3  n = nrm[v];
  float w = 0;  // the free w of the normal: 1 + the lids' shade of a ball's vertex (vertex_process.glsl), 0 the rest
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
      if(hasLidShade())
        w = 1.f + lidEdges[i].light(p-e.center,e.radius,ey->shade->params);
      break;
      }
  for(uint32_t i=drawnFirst[v]; i<drawnFirst[v+1]; ++i) {
    const uint32_t r = drawn[i];
    changedFirst = std::min(changedFirst,r);
    changedEnd   = std::max(changedEnd,r+1);
    deform[size_t(r)*2+0] = Vec4(p.x,p.y,p.z,1.f);
    deform[size_t(r)*2+1] = Vec4(n.x,n.y,n.z,w);
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
  bool lidsMoved = false;  // the lids' edges are found again only when a shape moves a lid now or moved it before
  for(auto v:moved) {
    pos[v]  = basePos[v];
    nrm[v]  = baseNrm[v];
    mark[v] = 1;
    lidsMoved |= lidVertex[v]!=0;
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
        lidsMoved |= lidVertex[v]!=0;
        }
      pos[v] += d.position*w;
      nrm[v] += d.normal*w;
      }
    }
  // the lids are where the shapes put them now: their edges over the balls, before any vertex is written
  if(lidsMoved || !lidEdges[0].hasMargins())
    lidMargins();
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

void FaceRig::lidMargins() {
  if(!hasLidShade())
    return;
  const auto& ey = *profile->eyes;
  for(int i=0; i<2; ++i)
    lidEdges[i].margins(pos,ey.shade->lids[i],ey.eye[i].center);
  }

float FaceRig::lidLight(uint32_t drawnVertex) const {
  if(size_t(drawnVertex)*2+1>=deform.size() || deform[size_t(drawnVertex)*2+1].w<0.5f)
    return -1;
  return deform[size_t(drawnVertex)*2+1].w-1.f;
  }

void FaceRig::turnEyes() {
  // only the balls turned: their vertices, the rest of the head as it is
  if(basePos.size()!=head->mesh.positions.size() || deform.size()!=head->renderVertexIds.size()*2) {
    prepare();
    lidMargins();
    }
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
  p.brows = browWeight(now);
  if(p.brows!=0) {
    char buf[64];
    std::snprintf(buf,sizeof(buf),"%sbrows:%s %.2f",p.shown.empty() ? "" : " ",p.brows>0 ? "up" : "down",double(std::abs(p.brows)));
    p.shown += buf;
    }
  p.listen = listenWeight(now);
  for(auto [kind,name]:{std::pair{ListenKind::Flash,"flash"},std::pair{ListenKind::Smile,"smile"},std::pair{ListenKind::Frown,"frown"}})
    if(const float k = listenWeight(kind,now); k>0) {
      char buf[64];
      std::snprintf(buf,sizeof(buf),"%slisten:%s %.2f",p.shown.empty() ? "" : " ",name,double(k));
      p.shown += buf;
      }
  for(auto w:applied)
    if(w>0)
      ++p.targets;
  p.blendUs = blendUs;
  p.blends  = blends;
  }
