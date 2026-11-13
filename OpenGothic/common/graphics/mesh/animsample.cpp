#include "animsample.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace {

bool on = []() {
  const char* v = std::getenv("OPENGOTHIC_ANIM_SMOOTH");
  return v==nullptr || v[0]!='0';
  }();

// the tangent at a key may be this many times as long as the shorter chord at it (Fritsch, Carlson)
constexpr float TangentLimit = 3.f;

// a vector of N numbers: a position (3) or a rotation as its four numbers
template<size_t N>
struct Vec final {
  float v[N] = {};

  Vec operator+(const Vec& o) const { Vec r; for(size_t i=0; i<N; ++i) r.v[i] = v[i]+o.v[i]; return r; }
  Vec operator-(const Vec& o) const { Vec r; for(size_t i=0; i<N; ++i) r.v[i] = v[i]-o.v[i]; return r; }
  Vec operator*(float k)      const { Vec r; for(size_t i=0; i<N; ++i) r.v[i] = v[i]*k;      return r; }
  float dot(const Vec& o)     const { float r=0; for(size_t i=0; i<N; ++i) r += v[i]*o.v[i]; return r; }
  };

Vec<3> vec(const asset::Vec3& p) {
  return {{p.x,p.y,p.z}};
  }

Vec<4> vec(const asset::Quat& q) {
  return {{q.w,q.x,q.y,q.z}};
  }

// half the sum of the chords at a key, not longer than TangentLimit of the shorter one
template<size_t N>
Vec<N> tangent(const Vec<N>& in, const Vec<N>& out) {
  Vec<N>      m   = (in+out)*0.5f;
  const float len = m.dot(m);
  const float lim = TangentLimit*TangentLimit*std::min(in.dot(in),out.dot(out));
  if(len>lim)
    m = m*std::sqrt(lim/len);
  return m;
  }

// the offset from a at t on the curve; in, out - the chords before a and after b (null: the clip has no key
// there, it starts from rest or comes to rest)
template<size_t N>
Vec<N> curve(const Vec<N>* in, const Vec<N>& chord, const Vec<N>* out, float t) {
  const Vec<N> m1 = in !=nullptr ? tangent(*in,chord)  : Vec<N>();
  const Vec<N> m2 = out!=nullptr ? tangent(chord,*out) : Vec<N>();
  const float  t2 = t*t, t3 = t2*t;
  // Hermite, as an offset from a: a hold (no chord, no tangents) gives exactly a
  return chord*(3.f*t2-2.f*t3) + m1*(t3-2.f*t2+t) + m2*(t3-t2);
  }

// q or -q, whichever is the same rotation nearer to ref
Vec<4> near(const Vec<4>& ref, const Vec<4>& q) {
  return ref.dot(q)<0.f ? q*-1.f : q;
  }

}

bool AnimSample::enabled() {
  return on;
  }

void AnimSample::setEnabled(bool e) {
  on = e;
  }

bool AnimSample::sparse(float fpsRate, uint32_t numFrames) {
  return fpsRate>0.f && fpsRate<SparseRate && numFrames>=3;
  }

AnimSample::Keys AnimSample::keys(uint64_t ms, float fpsRate, size_t count, bool loop, bool reverse) {
  Keys k;
  if(count==0)
    return k;
  const uint64_t frame = uint64_t(float(ms)*fpsRate);
  const uint64_t at    = frame/1000;
  k.t = float(frame%1000)/1000.f;

  // in the order of play: at-1, at, at+1, at+2
  uint64_t before=0, a=0, b=0, after=0;
  if(loop) {
    const uint64_t cycle = at/count;
    a      = at%count;
    b      = (at+1)%count;
    after  = (at+2)%count;
    before = (at+count-1)%count;
    k.hasBefore = k.hasAfter = true;
    k.cycB      = int((at+1)/count - cycle);
    k.cycAfter  = int((at+2)/count - cycle);
    k.cycBefore = a==0 ? -1 : 0;
    } else {
    const uint64_t last = count-1;
    a = std::min<uint64_t>(at,  last);
    b = std::min<uint64_t>(at+1,last);
    k.hasBefore = a>0;
    k.hasAfter  = b<last;
    before = k.hasBefore ? a-1 : a;
    after  = k.hasAfter  ? b+1 : b;
    }

  if(reverse) {
    // played from its last frame back: a cycle on in play is a cycle back in the clip
    const uint64_t last = count-1;
    a = last-a; b = last-b; before = last-before; after = last-after;
    k.cycBefore = -k.cycBefore; k.cycB = -k.cycB; k.cycAfter = -k.cycAfter;
    }
  k.a = size_t(a); k.b = size_t(b); k.before = size_t(before); k.after = size_t(after);
  return k;
  }

AnimSample::Sample AnimSample::mix(const Sample* before, const Sample& a, const Sample& b, const Sample* after, float t) {
  Sample r;

  const Vec<3> pa = vec(a.position), pb = vec(b.position);
  Vec<3> in, out;
  if(before!=nullptr)
    in = pa-vec(before->position);
  if(after!=nullptr)
    out = vec(after->position)-pb;
  const Vec<3> p = pa + curve(before!=nullptr ? &in : nullptr, pb-pa, after!=nullptr ? &out : nullptr, t);
  r.position = asset::Vec3(p.v[0],p.v[1],p.v[2]);

  // a rotation as four numbers on the same curve, then back to unit length (as glTF's CUBICSPLINE and Unity's
  // curves do); every key the nearer of its two signs to the one before it
  const Vec<4> qa = vec(a.rotation), qb = near(qa,vec(b.rotation));
  Vec<4> qin, qout;
  if(before!=nullptr)
    qin = qa-near(qa,vec(before->rotation));
  if(after!=nullptr)
    qout = near(qb,vec(after->rotation))-qb;
  const Vec<4> q   = qa + curve(before!=nullptr ? &qin : nullptr, qb-qa, after!=nullptr ? &qout : nullptr, t);
  const float  len = std::sqrt(q.dot(q));
  if(len>1e-6f)
    r.rotation = asset::Quat(q.v[0]/len,q.v[1]/len,q.v[2]/len,q.v[3]/len); else
    r.rotation = a.rotation;
  return r;
  }

asset::Vec3 AnimSample::offLine(const asset::Vec3* before, const asset::Vec3& a, const asset::Vec3& b,
                                const asset::Vec3* after, float t) {
  const Vec<3> pa = vec(a), pb = vec(b);
  Vec<3> in, out;
  if(before!=nullptr)
    in = pa-vec(*before);
  if(after!=nullptr)
    out = vec(*after)-pb;
  const Vec<3> chord = pb-pa;
  const Vec<3> d     = curve(before!=nullptr ? &in : nullptr, chord, after!=nullptr ? &out : nullptr, t) - chord*t;
  return asset::Vec3(d.v[0],d.v[1],d.v[2]);
  }
