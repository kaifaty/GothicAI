#include "eyes.h"

#include <algorithm>
#include <cmath>

using namespace Tempest;

Eyes::Probe Eyes::subjectProbe;
const void* Eyes::probeNpc = nullptr;

namespace {

constexpr float Deg = float(M_PI)/180.f;

// Where the eyes go (eyes.md: Argyle and Cook 1976; Lee, Badler and Badler, "Eyes Alive", 2002)
constexpr float EyeGap     = 3.f;    // cm: the other one's eyes, to each side of the point Gaze looks at
constexpr float MouthDrop  = 7.f;    // cm: its mouth, below it
constexpr float AwayMin    = 4.f;    // degrees of a look away from the face...
constexpr float AwayMax    = 12.f;
constexpr float AwayDown   = -50.f;  // ...to a side, from this far down...
constexpr float AwayUp     = 25.f;   // ...to this far up (degrees of its direction)
constexpr float OtherEye   = 0.75f;  // a jump from one eye goes to the other one with this chance
constexpr float WanderMin  = 2.f;    // degrees of a small jump with no face to go over...
constexpr float WanderMax  = 8.f;
constexpr float WanderMs[2]  = {500,2000};  // ...this often
constexpr float LineAwayMs[2] = {600,1500}; // its line starts: it looks away so long
constexpr float EndOfLine  = 800.f;  // ms: the last of its line it looks at the other one's eyes
constexpr float ForwardCm  = 500.f;  // with nothing to look at the point is this far...
constexpr float ForwardOff = 15.f;   // ...and when the head turns it this far in the socket, it is forward again
constexpr float Behind     = 160.f;  // degrees from the face's forward: a target further back is not looked at (short
                                     // of it the eyes wait at their limit toward it while the head turns)

struct Fixations final {
  float eye, mouth;  // the chance of a look at its eyes, at its mouth; the rest: away
  float eyeMs[2], mouthMs[2], awayMs[2];
  };
// listening (and talking while not speaking): at the other one about 70% of the time, ~1.7 jumps a second
constexpr Fixations Listening = {0.62f, 0.18f, {300,800}, {250,600}, {500,1200}};
// speaking: at the other one about 40% of the time
constexpr Fixations Speaking  = {0.45f, 0.05f, {300,700}, {250,500}, {400,1200}};

Vec3 direction(float yaw, float pitch) {
  // in the frame of the head bone: x up, y across, z forward
  return Vec3(std::sin(pitch), std::cos(pitch)*std::sin(yaw), std::cos(pitch)*std::cos(yaw));
  }

Vec3 center(const FaceProfile::Eyes& e) {
  return (e.eye[0].center + e.eye[1].center)*0.5f;
  }

Vec3 project(const Matrix4x4& m, Vec3 v) {
  m.project(v);
  return v;
  }

}

float Eyes::bell(float t) {
  t = std::clamp(t,0.f,1.f);
  return t*t*t*(10.f - 15.f*t + 6.f*t*t);
  }

void Eyes::setup(const FaceProfile::Eyes* e, uint32_t seed) {
  if(e==eyes && e!=nullptr)
    return;  // the same head in a new view: the eyes go on
  *this = Eyes();
  eyes  = e;
  rnd   = (seed+1u)*2654435761u | 1u;
  }

float Eyes::rand01() {
  rnd ^= rnd<<13;
  rnd ^= rnd>>17;
  rnd ^= rnd<<5;
  return float(rnd%10000u)/10000.f;
  }

Eyes::Angles Eyes::limited(int side, Angles a) const {
  using E = FaceProfile::Eyes;
  const auto& e   = eyes->eye[side];
  const float out = std::clamp(a.yaw*e.outSign, -eyes->limit[E::In]/Deg, eyes->limit[E::Out]/Deg);
  a.yaw   = out*e.outSign;
  a.pitch = std::clamp(a.pitch, -eyes->limit[E::Down]/Deg, eyes->limit[E::Up]/Deg);
  return a;
  }

float Eyes::over(int side, const Angles& a) const {
  using E = FaceProfile::Eyes;
  const float out = a.yaw*eyes->eye[side].outSign;
  return std::max({0.f, out-eyes->limit[E::Out]/Deg, -out-eyes->limit[E::In]/Deg,
                   a.pitch-eyes->limit[E::Up]/Deg, -a.pitch-eyes->limit[E::Down]/Deg});
  }

Eyes::Angles Eyes::want(int side, const Vec3& world, bool limit) const {
  const Vec3 d = project(toHead,world) - eyes->eye[side].center;
  Angles     a;
  a.yaw   = std::atan2(d.y,d.z)/Deg;
  a.pitch = std::atan2(d.x,std::sqrt(d.y*d.y + d.z*d.z))/Deg;
  return limit ? limited(side,a) : a;
  }

void Eyes::pick(Mode m, bool fresh, bool lineStart, const Input& in) {
  const Kind before = kind;
  picked = true;
  offset = Angles();
  const auto wander = [&]() {
    const float a = range(WanderMin,WanderMax), phi = range(0.f,360.f)*Deg;
    offset = {a*std::cos(phi), a*std::sin(phi)};
    nextPick = in.now + uint64_t(range(WanderMs[0],WanderMs[1]));
    };
  switch(m) {
    case Mode::Free: {
      kind = Kind::Forward;
      wander();
      if(fresh)
        offset = Angles();
      // a point in the world: the eyes keep to it as the head moves, till the next jump
      const Vec3 c = center(*eyes);
      fixed = project(in.head, c + direction(offset.yaw*Deg,offset.pitch*Deg)*ForwardCm);
      return;
      }
    case Mode::Look:
    case Mode::Think:
      kind = m==Mode::Look ? Kind::Target : Kind::Aside;
      wander();
      if(fresh)
        offset = Angles();
      return;
    case Mode::Listen:
    case Mode::Speak:
      break;
    }

  const auto& f = m==Mode::Speak ? Speaking : Listening;
  const auto  r = rand01();
  float       ms[2] = {};
  if(lineStart) {
    kind = Kind::Away;
    std::copy(LineAwayMs,LineAwayMs+2,ms);
    }
  else if((m==Mode::Speak && in.left>0 && float(in.left)<EndOfLine) || r<f.eye) {
    kind = Kind::Eye;
    std::copy(f.eyeMs,f.eyeMs+2,ms);
    }
  else if(r<f.eye+f.mouth) {
    kind = Kind::Mouth;
    std::copy(f.mouthMs,f.mouthMs+2,ms);
    }
  else {
    kind = Kind::Away;
    std::copy(f.awayMs,f.awayMs+2,ms);
    }
  if(kind==Kind::Eye)
    side = before==Kind::Eye ? (rand01()<OtherEye ? -side : side) : (rand01()<0.5f ? 1.f : -1.f);
  if(kind==Kind::Away) {
    const float a = range(AwayMin,AwayMax), phi = range(AwayDown,AwayUp)*Deg;
    offset = {(rand01()<0.5f ? a : -a)*std::cos(phi), a*std::sin(phi)};
    }
  nextPick = in.now + uint64_t(range(ms[0],ms[1]));
  }

Vec3 Eyes::point(const Input& in) const {
  const Vec3 c = project(in.head,center(*eyes));
  Vec3       base;
  switch(kind) {
    case Kind::None:
    case Kind::Forward:
      return fixed;
    case Kind::Eye: {
      // one of its eyes: across the line of sight, level
      const Vec3  d = in.at - c;
      const Vec3  l = Vec3(-d.z,0,d.x);
      const float n = l.length();
      return n>1e-3f ? in.at + l*(EyeGap*side/n) : in.at;
      }
    case Kind::Mouth:
      return in.at - Vec3(0,MouthDrop,0);
    case Kind::Away:
    case Kind::Target:
      base = in.at;
      break;
    case Kind::Aside:
      base = in.asideAt;
      break;
    }
  if(offset.yaw==0.f && offset.pitch==0.f)
    return base;
  // the base point turned by the offset around the middle of the eyes, in the frame of the head
  const Vec3  cl = center(*eyes);
  const Vec3  b  = project(toHead,base) - cl;
  const float r  = b.length();
  const float y  = std::atan2(b.y,b.z) + offset.yaw*Deg;
  const float p  = std::atan2(b.x,std::sqrt(b.y*b.y + b.z*b.z)) + offset.pitch*Deg;
  return project(in.head, cl + direction(y,p)*r);
  }

void Eyes::update(const Input& in) {
  if(eyes==nullptr)
    return;
  toHead = in.head;
  toHead.inverse();
  const uint64_t dt = hasLast && in.now>lastNow ? in.now-lastNow : 0;
  lastNow = in.now;

  // what to look at: a target far behind the face is not looked at
  bool target = in.target;
  if(target) {
    const Vec3 d = project(toHead,in.at) - center(*eyes);
    target = d.length()>1.f && std::acos(std::clamp(d.z/d.length(),-1.f,1.f))/Deg<=Behind;
    }
  const Mode m = !target ? Mode::Free : !in.talk ? Mode::Look :
                 in.aside ? Mode::Think : in.speaking ? Mode::Speak : Mode::Listen;
  const bool lineStart = m==Mode::Speak && in.speaking && !speaking;
  const bool fresh     = !started || m!=mode;
  speaking = in.speaking;
  if(!in.roam) {
    if(fresh) {
      kind   = target ? Kind::Target : Kind::Forward;
      offset = Angles();
      picked = true;
      fixed  = project(in.head, center(*eyes) + Vec3(0,0,ForwardCm));
      }
    }
  else {
    bool again = fresh || lineStart || in.now>=nextPick;
    if(!again && kind==Kind::Forward)
      for(int s=0; s<2 && !again; ++s) {
        const auto a = want(s,fixed,false);
        again = std::sqrt(a.yaw*a.yaw + a.pitch*a.pitch)>ForwardOff;
        }
    if(again)
      pick(m,fresh,lineStart,in);
    }
  mode    = m;
  started = true;

  // the eyes: each from its centre to the point, within the limits
  const Vec3 at = point(in);
  if(!hasLast) {
    last    = at;
    hasLast = true;
    }
  Angles w[2], was[2];
  for(int s=0; s<2; ++s) {
    w[s]   = want(s,at,true);
    was[s] = want(s,last,true);
    // the point jumped (another target, or it was moved there): no waiting out the refractory time
    if(std::max(std::abs(w[s].yaw-was[s].yaw),std::abs(w[s].pitch-was[s].pitch))>Saccade)
      picked = true;
    }
  bool justLanded = false;  // the error is to the point as it is now already
  if(moving) {
    // a jump goes where the point was as it started (the head may turn meanwhile: it is the frame of the head now)
    const float t = float(in.now-jumpFrom)/std::max(jumpMs,1.f);
    const float k = t>=1.f ? 0.f : 1.f-bell(t);
    for(int s=0; s<2; ++s) {
      const auto to = want(s,jumpTo,true);
      err[s] = {to.yaw + jumpErr[s].yaw*k - w[s].yaw, to.pitch + jumpErr[s].pitch*k - w[s].pitch};
      }
    if(t>=1.f) {
      moving     = false;
      landed     = in.now;
      justLanded = true;
      }
    }
  if(!moving) {
    // the point moved, or another one: the eyes follow up to Pursuit, the rest stays as an error to jump over
    const float cap = Pursuit*float(dt)/1000.f;
    float       a   = 0;
    for(int s=0; s<2; ++s) {
      const Angles d = justLanded ? Angles() : Angles{w[s].yaw-was[s].yaw, w[s].pitch-was[s].pitch};
      Angles       v = {d.yaw-err[s].yaw, d.pitch-err[s].pitch};
      const float  n = std::sqrt(v.yaw*v.yaw + v.pitch*v.pitch);
      if(n>cap) {
        v.yaw   *= cap/n;
        v.pitch *= cap/n;
        }
      err[s] = {err[s].yaw + v.yaw - d.yaw, err[s].pitch + v.pitch - d.pitch};
      a = std::max(a, std::sqrt(err[s].yaw*err[s].yaw + err[s].pitch*err[s].pitch));
      }
    if(a>Saccade && (picked || float(in.now-landed)>=Refractory)) {
      moving   = true;
      jumpTo   = at;
      jumpFrom = in.now;
      jumpMs   = duration(a);
      for(int s=0; s<2; ++s)
        jumpErr[s] = err[s];
      ++saccades;
      if(a>=BigSaccade)
        ++big;
      }
    picked = false;
    }
  last = at;

  // the point from the middle of the eyes and the head's forward, in the world (the probe)
  const Vec3  d  = at - project(in.head,center(*eyes));
  const float dh = std::sqrt(d.x*d.x + d.z*d.z);
  const Vec3  fw = Vec3(in.head.at(2,0),in.head.at(2,1),in.head.at(2,2));
  rise   = d.y;
  down   = -std::atan2(d.y,dh)/Deg;
  headUp = fw.length()>1e-6f ? std::asin(std::clamp(fw.y/fw.length(),-1.f,1.f))/Deg : 0.f;

  // as shown, and how far from the point
  error  = 0;
  beyond = 0;
  for(int s=0; s<2; ++s) {
    const Angles e = limited(s,{w[s].yaw+err[s].yaw, w[s].pitch+err[s].pitch});
    shown[s] = {e.yaw*Deg, e.pitch*Deg};
    beyond   = std::max(beyond,over(s,e));
    const Vec3  look = direction(shown[s].yaw,shown[s].pitch);
    Vec3        to   = project(toHead,at) - eyes->eye[s].center;
    const float n    = to.length();
    if(n>1e-3f)
      error = std::max(error, std::acos(std::clamp(Vec3::dotProduct(look,to/n),-1.f,1.f))/Deg);
    }
  }

void Eyes::probe(Probe& p) const {
  p = Probe();
  p.valid = eyes!=nullptr && started;
  if(!p.valid)
    return;
  p.kind = kind;
  for(int s=0; s<2; ++s) {
    p.yaw[s]   = shown[s].yaw/Deg;
    p.pitch[s] = shown[s].pitch/Deg;
    }
  p.error    = error;
  p.over     = beyond;
  p.moving   = moving;
  p.saccades = saccades;
  p.big      = big;
  p.rise     = rise;
  p.down     = down;
  p.head     = headUp;
  }
