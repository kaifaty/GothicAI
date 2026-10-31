#include "animblend.h"

#include "animmath.h"
#include "game/serialize.h"

#include <Tempest/Log>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <stdexcept>

using namespace Tempest;

namespace {

using Quat = zenkit::Quat;

constexpr uint64_t MaxGap   = 250;     // ms between two frames: longer, and the velocity is not known
constexpr float    MaxSpeed = 3000.f;  // cm/s of a bone in its parent
constexpr float    MaxTurn  = 40.f;    // rad/s
constexpr float    MinAngle = 1e-4f;   // radians: less is no offset
constexpr float    MinMove  = 0.01f;   // cm
constexpr float    MaxTurnOffset = 170.f*float(M_PI)/180.f; // going further out, a rotation would flip the short way round
constexpr float    OutPerSpeed   = 0.135f; // how much further out (of the offset) the curve goes per unit of outSpeed
constexpr float    SidePeak      = 0.197f; // the curve from zero goes aside up to this * v0 * t1

bool enabled = []() {
  const char* v = std::getenv("OPENGOTHIC_ANIBLEND");
  return v==nullptr || v[0]!='0';
  }();

AnimBlend::Params prm;

Vec3 vec(const zenkit::Vec3& v) {
  return Vec3(v.x,v.y,v.z);
  }

zenkit::Vec3 vec(const Vec3& v) {
  return zenkit::Vec3(v.x,v.y,v.z);
  }

float dot(const Vec3& a, const Vec3& b) {
  return a.x*b.x + a.y*b.y + a.z*b.z;
  }

Vec3 clampLength(const Vec3& v, float max) {
  const float l = v.length();
  return l>max ? v*(max/l) : v;
  }

Quat mul(const Quat& a, const Quat& b) {
  return Quat(a.w*b.w - a.x*b.x - a.y*b.y - a.z*b.z,
              a.w*b.x + a.x*b.w + a.y*b.z - a.z*b.y,
              a.w*b.y - a.x*b.z + a.y*b.w + a.z*b.x,
              a.w*b.z + a.x*b.y - a.y*b.x + a.z*b.w);
  }

Quat conj(const Quat& q) {
  return Quat(q.w,-q.x,-q.y,-q.z);
  }

Quat normalize(const Quat& q) {
  const float l = std::sqrt(q.w*q.w + q.x*q.x + q.y*q.y + q.z*q.z);
  return l>0 ? Quat(q.w/l,q.x/l,q.y/l,q.z/l) : Quat(1,0,0,0);
  }

// rotation vector (axis * angle) of a unit quaternion, the short way round
Vec3 rotVec(Quat q) {
  if(q.w<0)
    q = Quat(-q.w,-q.x,-q.y,-q.z);
  const float s = std::sqrt(q.x*q.x + q.y*q.y + q.z*q.z);
  if(s<1e-6f)
    return Vec3(q.x,q.y,q.z)*2.f;
  const float angle = 2.f*std::atan2(s,q.w);
  return Vec3(q.x,q.y,q.z)*(angle/s);
  }

Quat fromRotVec(const Vec3& v) {
  const float angle = v.length();
  if(angle<1e-6f)
    return normalize(Quat(1.f,v.x*0.5f,v.y*0.5f,v.z*0.5f));
  const float s = std::sin(angle*0.5f)/angle;
  return Quat(std::cos(angle*0.5f),v.x*s,v.y*s,v.z*s);
  }

Vec3 rotate(const Quat& q, const Vec3& v) {
  const Quat r = mul(mul(q,Quat(0,v.x,v.y,v.z)),conj(q));
  return Vec3(r.x,r.y,r.z);
  }

}

bool AnimBlend::isEnabled() {
  return enabled;
  }

void AnimBlend::setEnabled(bool e) {
  enabled = e;
  Log::i("aniblend: ", e ? "on" : "off");
  }

bool AnimBlend::setParam(std::string_view name, float v) {
  if(name=="min")
    prm.minTime = v;
  else if(name=="scale")
    prm.scale = v;
  else if(name=="max")
    prm.maxTime = v;
  else if(name=="turn")
    prm.turnSpeed = v;
  else if(name=="move")
    prm.moveSpeed = v;
  else if(name=="out")
    prm.outSpeed = v;
  else if(name=="side")
    prm.sideSpeed = v;
  else if(name=="blow")
    prm.blowTime = v;
  else
    return false;
  return true;
  }

const AnimBlend::Params& AnimBlend::params() {
  return prm;
  }

void AnimBlend::Curve::init(float x, float v, float t, float vMax) {
  // x >= 0: a length or an angle. Velocity away from zero carries the bone further out first: kept
  // up to vMax. Fast toward zero ends sooner, so the curve does not overshoot zero
  x0 = x;
  t1 = t;
  v0 = std::min(v,vMax);
  if(v0<-1e-4f)
    t1 = std::min(t1,-5.f*x0/v0);
  if(t1<=1e-4f || (x0<=0 && v0<=0)) {
    t1 = 0;
    return;
    }
  const float t2 = t1*t1, t3 = t2*t1, t4 = t3*t1, t5 = t4*t1;
  a0 = std::max((-8.f*v0*t1 - 20.f*x0)/t2, 0.f);
  A  = -(a0*t2 + 6.f*v0*t1 + 12.f*x0)/(2.f*t5);
  B  =  (3.f*a0*t2 + 16.f*v0*t1 + 30.f*x0)/(2.f*t4);
  C  = -(3.f*a0*t2 + 12.f*v0*t1 + 20.f*x0)/(2.f*t3);
  }

float AnimBlend::Curve::at(float t) const {
  if(t>=t1)
    return 0;
  return ((((A*t + B)*t + C)*t + 0.5f*a0)*t + v0)*t + x0;
  }

void AnimBlend::reset(size_t numBones) {
  cur .assign(numBones,Driver());
  drv .assign(numBones,Driver());
  off .assign(numBones,Offset());
  last.assign(numBones,Sample());
  outTime  = 0;
  lastTime = 0;
  external.clear();
  externalPrevious.clear();
  externalPreviousTime = 0;
  externalDriven = false;
  externalFade = Curve();
  externalStart = 0;
  externalClockPending = false;
  }

void AnimBlend::transitionFrom(const Sample* shown, size_t numBones, uint64_t now,
                              const Sample* previous, uint64_t previousTime) {
  numBones = std::min(numBones,cur.size());
  external.assign(shown,shown+numBones);
  externalPrevious.clear();
  externalPreviousTime = 0;
  if(previous!=nullptr && previousTime<now && now-previousTime<=MaxGap) {
    externalPrevious.assign(previous,previous+numBones);
    externalPreviousTime = previousTime;
    }
  for(auto& o:off)
    o = Offset();
  pend.clear();
  outTime = now;
  lastTime = 0;
  externalDriven = true;
  externalFade = Curve();
  externalStart = now;
  externalClockPending = true;
  }

void AnimBlend::setExternalTarget(const Sample* current, const Sample* next, size_t numBones, float rate) {
  if(current==nullptr || next==nullptr || !std::isfinite(rate) || rate<=0)
    return;
  for(size_t i=0;i<std::min(numBones,cur.size());++i) {
    cur[i].a=current+i;
    cur[i].b=next+i;
    cur[i].rate=rate;
    }
  }

float AnimBlend::externalWeight(uint64_t now) const {
  if(!externalDriven)
    return 0.f;
  if(externalClockPending || now<=externalStart)
    return 1.f;
  return std::clamp(externalFade.at(float(now-externalStart)*.001f),0.f,1.f);
  }

void AnimBlend::saveExternal(Serialize& fout) const {
  fout.write(uint32_t(off.size()),outTime,lastTime,externalStart,externalClockPending);
  auto curve = [&](const Curve& c) {
    fout.write(c.t1,c.x0,c.v0,c.a0,c.A,c.B,c.C);
    };
  curve(externalFade);
  for(size_t i=0; i<off.size(); ++i) {
    const auto& o = off[i];
    fout.write(o.active,o.start,o.end,o.fade,o.from,o.fadeTime);
    curve(o.rot); curve(o.rotSide); curve(o.pos); curve(o.posSide);
    fout.write(o.axis,o.axisSide,o.dir,o.dirSide,last[i]);
    }
  fout.write(uint32_t(external.size()));
  for(const auto& i:external)
    fout.write(i);
  fout.write(externalPreviousTime,uint32_t(externalPrevious.size()));
  for(const auto& i:externalPrevious)
    fout.write(i);
  }

void AnimBlend::loadExternal(Serialize& fin, size_t expectedBones, bool hasVelocityState) {
  uint32_t count = 0;
  fin.read(count);
  if(count!=expectedBones || count==0)
    throw std::runtime_error("Ragdoll blend skeleton mismatch");
  reset(count);
  fin.read(outTime,lastTime,externalStart,externalClockPending);
  auto finite = [](const auto& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
    };
  auto sample = [&](const Sample& s, bool unused=false) {
    const double q=double(s.rotation.w)*s.rotation.w+double(s.rotation.x)*s.rotation.x+
                   double(s.rotation.y)*s.rotation.y+double(s.rotation.z)*s.rotation.z;
    return finite(s.position) && std::abs(s.position.x)<=1.e8f && std::abs(s.position.y)<=1.e8f &&
           std::abs(s.position.z)<=1.e8f && std::isfinite(s.rotation.w) && finite(s.rotation) &&
           (std::abs(q-1.)<=.001 || (unused && q==0.));
    };
  auto curve = [&](Curve& c, float maxOffset, float maxVelocity, double intervalBound) {
    fin.read(c.t1,c.x0,c.v0,c.a0,c.A,c.B,c.C);
    if(!std::isfinite(c.t1) || c.t1<0 || c.t1>600 || !std::isfinite(c.x0) ||
       !std::isfinite(c.v0) || !std::isfinite(c.a0) || !std::isfinite(c.A) ||
       !std::isfinite(c.B) || !std::isfinite(c.C) || c.x0<0 || c.x0>maxOffset ||
       std::abs(c.v0)>maxVelocity || (c.t1>0 && c.t1<=1.e-4f))
      throw std::runtime_error("Invalid ragdoll blend curve");
    // Serialized coefficients must describe a curve produced by the actual initializer. Checking
    // only its endpoints misses finite polynomials which overflow in the middle of the transition.
    Curve expected;
    expected.init(c.x0,c.v0,c.t1,std::max(c.v0,0.f));
    auto equal=[](float a, float b) {
      return std::abs(double(a)-b)<=32.*std::numeric_limits<float>::epsilon()*
             std::max({1.e-12,std::abs(double(a)),std::abs(double(b))});
      };
    if(!equal(c.t1,expected.t1) || !equal(c.a0,expected.a0) || !equal(c.A,expected.A) ||
       !equal(c.B,expected.B) || !equal(c.C,expected.C))
      throw std::runtime_error("Noncanonical ragdoll blend curve");
    const double t=c.t1, t2=t*t, t3=t2*t, t4=t3*t, t5=t4*t;
    const double bound=std::abs(double(c.x0))+std::abs(double(c.v0))*t+
                       .5*std::abs(double(c.a0))*t2+std::abs(double(c.C))*t3+
                       std::abs(double(c.B))*t4+std::abs(double(c.A))*t5;
    if(!std::isfinite(bound) || bound>intervalBound)
      throw std::runtime_error("Unbounded ragdoll blend curve");
    };
  curve(externalFade,1.f,0.f,100.);
  if(lastTime>outTime || (!externalClockPending && externalStart>outTime) ||
     (externalClockPending && (externalFade.t1!=0 || externalFade.x0!=0)) ||
     (!externalClockPending && externalFade.t1>0 && externalFade.x0!=1.f))
    throw std::runtime_error("Invalid ragdoll blend clock");
  auto direction=[&](const Vec3& v, const Curve& c) {
    const double n=double(v.x)*v.x+double(v.y)*v.y+double(v.z)*v.z;
    return finite(v) && (std::abs(n-1.)<=.001 || (c.t1==0 && n==0.));
    };
  for(size_t i=0; i<count; ++i) {
    auto& o = off[i];
    fin.read(o.active,o.start,o.end,o.fade,o.from,o.fadeTime);
    curve(o.rot,float(M_PI)+.001f,2.f*MaxTurn+.01f,1024.);
    curve(o.rotSide,0.f,2.f*MaxTurn+.01f,1024.);
    curve(o.pos,2.e8f,2.f*MaxSpeed+1.f,1.e12);
    curve(o.posSide,0.f,2.f*MaxSpeed+1.f,1.e12);
    fin.read(o.axis,o.axisSide,o.dir,o.dirSide,last[i]);
    if(!std::isfinite(o.end) || o.end<0 || o.end>600 || !std::isfinite(o.fadeTime) ||
       o.fadeTime!=0 || o.fade || !sample(o.from,true) || !sample(last[i],externalClockPending) ||
       !direction(o.axis,o.rot) || !direction(o.axisSide,o.rotSide) ||
       !direction(o.dir,o.pos) || !direction(o.dirSide,o.posSide) || o.start>outTime ||
       o.end!=std::max({o.rot.t1,o.rotSide.t1,o.pos.t1,o.posSide.t1}) || (o.active && o.end==0))
      throw std::runtime_error("Invalid ragdoll blend offset");
    }
  uint32_t pending = 0;
  fin.read(pending);
  if(pending!=0 && pending!=count)
    throw std::runtime_error("Invalid pending ragdoll blend pose");
  if(externalClockPending!=(pending!=0))
    throw std::runtime_error("Inconsistent pending ragdoll blend clock");
  external.resize(pending);
  for(auto& i:external) {
    fin.read(i);
    if(!sample(i))
      throw std::runtime_error("Invalid ragdoll blend sample");
    }
  if(hasVelocityState) {
    uint32_t previousCount=0;
    fin.read(externalPreviousTime,previousCount);
    if(previousCount!=0 && (previousCount!=count || pending!=count || externalPreviousTime>=outTime ||
                           outTime-externalPreviousTime>MaxGap))
      throw std::runtime_error("Invalid previous ragdoll blend pose");
    if(previousCount==0 && externalPreviousTime!=0)
      throw std::runtime_error("Invalid previous ragdoll blend time");
    externalPrevious.resize(previousCount);
    for(auto& i:externalPrevious) {
      fin.read(i);
      if(!sample(i))
        throw std::runtime_error("Invalid previous ragdoll blend sample");
      }
    }
  externalDriven = true;
  }

// What the bone switched from: the shown pose and how it moved, carried on to now, against the new
// animation and how that moves (from its key frames)
void AnimBlend::prepare(size_t i, const Sample& target, const Sample& shown, float dtShown, float dtNow) {
  const Driver& c = cur[i];
  const Driver& d = drv[i];

  Vec3 vel, turn;
  if(dtShown>0) {
    vel  = clampLength((vec(shown.position) - vec(last[i].position))*(1.f/dtShown), MaxSpeed);
    turn = clampLength(rotVec(mul(shown.rotation,conj(last[i].rotation)))*(1.f/dtShown), MaxTurn);
    }
  const Vec3 pos = vec(shown.position) + vel*dtNow;
  const Quat rot = normalize(mul(fromRotVec(turn*dtNow),shown.rotation));

  Vec3 tvel, tturn;
  if(c.a!=nullptr && c.b!=nullptr && c.rate>0) {
    tvel  = (vec(c.b->position) - vec(c.a->position))*c.rate;
    tturn = rotVec(mul(c.b->rotation,conj(c.a->rotation)))*c.rate;
    if(externalDriven) {
      tvel=clampLength(tvel,MaxSpeed);
      tturn=clampLength(tturn,MaxTurn);
      }
    }

  Quat q = normalize(mul(rot,conj(target.rotation)));
  if(q.w<0)
    q = Quat(-q.w,-q.x,-q.y,-q.z);

  Switch sw;
  sw.bone  = i;
  sw.turn  = rotVec(q);
  sw.angle = sw.turn.length();
  // the offset turns as the shown pose minus the new animation (seen through the offset)
  sw.dturn = turn - rotate(q,tturn);
  sw.move  = pos - vec(target.position);
  sw.len   = sw.move.length();
  sw.dmove = vel - tvel;
  // the MDS: the new animation's blend in, the old one's blend out
  sw.time  = float(std::max(c.in,d.out))*0.001f*prm.scale;
  pend.push_back(sw);
  }

// The switches of this frame: one blend time for all, so that a limb stays one limb on the way (a
// time per bone bent the legs of a stumbling waran apart). A large offset takes longer, not to whip
void AnimBlend::begin(uint64_t now) {
  float time = prm.minTime;
  bool  blow = false;
  for(auto& sw:pend) {
    time = std::max(time,sw.time);
    if(prm.turnSpeed>0)
      time = std::max(time,1.875f*sw.angle/prm.turnSpeed);
    if(prm.moveSpeed>0)
      time = std::max(time,1.875f*sw.len/prm.moveSpeed);
    const auto* seq = cur[sw.bone].seq;
    blow = blow || (weaponBody && seq!=nullptr && seq->data!=nullptr && seq->isAttackAnim());
    }
  time      = std::min(time,std::max(prm.maxTime,prm.minTime));
  if(weaponBody && blow)
    time = std::min(time,prm.blowTime);
  lastBlend = std::max(lastBlend,time);
  if(externalDriven && externalClockPending) {
    externalFade.init(1.f,0.f,time,0.f);
    externalStart = now;
    externalClockPending = false;
    }

  // an offset and its velocity: a curve along it from its size, one across it from zero; a rotation
  // does not go out past MaxTurnOffset (limit), aside or along
  auto init = [&](Curve& along, Curve& across, const Vec3& dir, Vec3& side, const Vec3& v, float x0, float limit) {
    const float va    = dot(dir,v);
    float       out   = prm.outSpeed*x0/time;
    float       aside = prm.sideSpeed*x0/time;
    if(limit>0) {
      out   = std::min(out,  std::max(limit-x0,0.f)/OutPerSpeed/time);
      aside = std::min(aside,std::sqrt(std::max(limit*limit - x0*x0,0.f))/(SidePeak*time));
      }
    along.init(x0,va,time,out);
    const Vec3  vs = v - dir*va;
    const float s  = vs.length();
    if(s>1e-3f*std::max(x0,1.f)) {
      side = vs*(1.f/s);
      across.init(0,s,time,aside);
      }
    };

  for(auto& sw:pend) {
    Offset& o = off[sw.bone];
    o = Offset();
    o.start = now;
    if(sw.angle>MinAngle) {
      o.axis = sw.turn*(1.f/sw.angle);
      init(o.rot,o.rotSide,o.axis,o.axisSide,sw.dturn,sw.angle,MaxTurnOffset);
      }
    if(sw.len>MinMove) {
      o.dir = sw.move*(1.f/sw.len);
      init(o.pos,o.posSide,o.dir,o.dirSide,sw.dmove,sw.len,0);
      }
    o.end    = std::max(std::max(o.rot.t1,o.rotSide.t1),std::max(o.pos.t1,o.posSide.t1));
    o.active = o.end>0;
    if(log && o.active) {
      char buf[256];
      std::snprintf(buf,sizeof(buf),"aniblend: %llu bone %zu %s -> %s: turn %.1f deg v %.1f/%.1f side %.1f, move %.1f cm v %.0f, t %.3f",
                    (unsigned long long)now,sw.bone,drv[sw.bone].seq ? drv[sw.bone].seq->name.c_str() : "physical",
                    cur[sw.bone].seq ? cur[sw.bone].seq->name.c_str() : "pose",
                    double(sw.angle*180.f/float(M_PI)),double(dot(o.axis,sw.dturn)),double(o.rot.v0),double(o.rotSide.v0),
                    double(sw.len),double(dot(o.dir,sw.dmove)),double(time));
      Log::d(buf);
      }
    }
  pend.clear();
  }

bool AnimBlend::apply(const Sample* raw, Sample* out, size_t numBones, uint64_t now) {
  numBones = std::min(numBones,cur.size());
  if(now<outTime) {
    // the time went back (a save loaded): nothing to blend from
    for(auto& o:off)
      o.active = false;
    outTime  = 0;
    lastTime = 0;
    }

  const bool  next    = (now!=outTime);
  const float dtShown = (lastTime>0 && outTime>lastTime && outTime-lastTime<=MaxGap) ? float(outTime-lastTime)*0.001f : 0.f;
  const float dtNow   = (outTime>0 && next) ? float(std::min(now-outTime,MaxGap))*0.001f : 0.f;

  // Start from the physical pose without advancing its position, preserving its measured velocity
  // and subtracting the finished target's velocity just like an ordinary animation switch.
  if(!external.empty()) {
    for(size_t i=0; i<std::min(numBones,external.size()); ++i) {
      out[i] = external[i];
      const bool known=externalPrevious.size()==external.size() && externalPreviousTime<outTime &&
                       outTime-externalPreviousTime<=MaxGap;
      last[i] = known ? externalPrevious[i] : external[i];
      const float dt=known ? float(outTime-externalPreviousTime)*.001f : 0.f;
      prepare(i,raw[i],external[i],dt,0.f);
      started++;
      }
    external.clear();
    externalPrevious.clear();
    externalPreviousTime=0;
    }

  // the switches first: they start from the pose shown, before it changes
  for(size_t i=0; i<numBones; ++i) {
    const Driver& c = cur[i];
    const Driver& d = drv[i];
    if(c.seq==nullptr || d.seq==nullptr || (c.seq==d.seq && c.sAnim==d.sAnim))
      continue;
    started++;
    if(enabled) {
      prepare(i,raw[i],out[i],dtShown,dtNow);
      continue;
      }
    Offset& o  = off[i];
    o          = Offset();
    o.fade     = true;
    o.from     = out[i];
    o.start    = now;
    o.fadeTime = float(std::max(c.in,c.out))*0.001f;
    o.active   = o.fadeTime>0;
    lastBlend  = std::max(lastBlend,o.fadeTime);
    }
  if(!pend.empty())
    begin(now);

  bool blending = externalDriven && externalWeight(now)>0.f;
  for(size_t i=0; i<numBones; ++i) {
    const Driver& c = cur[i];
    if(c.seq==nullptr && !externalDriven)
      continue;
    Driver&    d     = drv[i];
    const bool fresh = (d.seq==nullptr);
    d = c;

    Sample  res = raw[i];
    Offset& o   = off[i];
    if(o.active && o.fade) {
      const float t = float(now-o.start)*0.001f;
      if(t>=o.fadeTime) {
        o.active = false;
        } else {
        res      = mix(o.from,raw[i],t/o.fadeTime);
        blending = true;
        }
      }
    else if(o.active) {
      const float t = float(now-o.start)*0.001f;
      if(t>=o.end) {
        o.active = false;
        } else {
        if(t<o.rot.t1 || t<o.rotSide.t1)
          res.rotation = normalize(mul(fromRotVec(o.axis*o.rot.at(t) + o.axisSide*o.rotSide.at(t)),raw[i].rotation));
        if(t<o.pos.t1 || t<o.posSide.t1)
          res.position = vec(vec(raw[i].position) + o.dir*o.pos.at(t) + o.dirSide*o.posSide.at(t));
        blending = true;
        }
      }
    if(next)
      last[i] = fresh ? res : out[i];
    out[i] = res;
    }

  if(next) {
    lastTime = outTime;
    outTime  = now;
    }
  return blending;
  }

void AnimBlend::probe(Probe& p) {
  p.valid   = !cur.empty();
  p.active  = 0;
  p.angle   = 0;
  p.started = started;
  p.time    = lastBlend;
  started   = 0;
  lastBlend = 0;
  const float t = float(outTime)*0.001f;
  for(auto& o:off) {
    if(!o.active || o.fade)
      continue;
    p.active++;
    p.angle = std::max(p.angle,o.rot.at(t - float(o.start)*0.001f)*180.f/float(M_PI));
    }
  }
