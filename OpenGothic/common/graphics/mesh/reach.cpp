#include "reach.h"

#include <algorithm>
#include <cmath>
#include <string_view>

#include "skeleton.h"

using namespace Tempest;
using namespace IkMath;

Reach::Probe Reach::playerProbe;
const void*  Reach::probeNpc = nullptr;
Reach::Probe Reach::subjectProbe;

namespace {

constexpr float Deg = float(M_PI)/180.f;

float smooth(float k) {
  k = std::clamp(k,0.f,1.f);
  return k*k*(3.f-2.f*k);
  }

Vec3 flat(const Vec3& v) {
  return normalize(Vec3(v.x,0,v.z));
  }

float angleDeg(const Vec3& a, const Vec3& b) {
  return std::acos(std::clamp(dot(normalize(a),normalize(b)),-1.f,1.f))/Deg;
  }

}

Reach::Form& Reach::form() {
  static Form f;
  return f;
  }

void Reach::setSkeleton(const Skeleton* sk) {
  *this = Reach();
  if(sk==nullptr || sk->tr.size()!=sk->nodes.size())
    return;
  auto& nodes = sk->nodes;
  auto  find  = [&](std::string_view name) {
    for(size_t i=0; i<nodes.size(); ++i)
      if(nodes[i].name==name)
        return i;
    return size_t(-1);
    };
  auto  isUnder = [&](size_t id, size_t root) {
    for(size_t i=id; i<nodes.size(); i=nodes[i].parent)
      if(i==root)
        return true;
    return false;
    };
  auto  tree = [&](size_t root, std::vector<size_t>& out) {
    for(size_t i=0; i<nodes.size(); ++i)
      if(isUnder(i,root))
        out.push_back(i);
    };
  static const char* names[2][4] = {
    {"BIP01 L UPPERARM","BIP01 L FOREARM","BIP01 L HAND","BIP01 L FINGER0"},
    {"BIP01 R UPPERARM","BIP01 R FOREARM","BIP01 R HAND","BIP01 R FINGER0"},
    };
  for(size_t i=0; i<2; ++i) {
    auto& a = arms[i];
    a.upper  = find(names[i][0]);
    a.fore   = find(names[i][1]);
    a.hand   = find(names[i][2]);
    a.finger = find(names[i][3]);
    if(a.upper==size_t(-1) || a.fore==size_t(-1) || a.hand==size_t(-1))
      return;
    if(!isUnder(a.fore,a.upper) || !isUnder(a.hand,a.fore))
      return;
    tree(a.upper,a.upperTree);
    tree(a.fore,a.foreTree);
    tree(a.hand,a.handTree);
    if(a.finger!=size_t(-1))
      tree(a.finger,a.fingerTree);
    a.length = length(position(sk->tr[a.fore])-position(sk->tr[a.upper])) +
               length(position(sk->tr[a.hand])-position(sk->tr[a.fore]));
    if(a.length<1.f)
      return;
    }
  for(size_t i=0; i<2; ++i) {
    // where the fingers and the palm are in the hand bone's frame: the hand has one finger bone, the palm is what
    // the bind pose says - arms spread have it down, arms that hang have it at the thigh
    auto&      a     = arms[i];
    const Mat3 bind  = rotationOf(sk->tr[a.hand]);
    const Vec3 wrist = position(sk->tr[a.hand]);
    Vec3 fingers = a.finger!=size_t(-1) ? position(sk->tr[a.finger])-wrist : wrist-position(sk->tr[a.fore]);
    if(length(fingers)<Eps)
      fingers = wrist-position(sk->tr[a.fore]);
    fingers = normalize(fingers);
    Vec3 palm = Vec3(0,-1,0);
    if(std::abs(fingers.y)>0.7f)
      palm = flat(position(sk->tr[arms[1-i].hand])-wrist);
    palm = normalize(palm - fingers*dot(palm,fingers));
    if(length(palm)<Eps)
      return;
    a.fingersLocal = mul(transpose(bind),fingers);
    a.palmLocal    = mul(transpose(bind),palm);
    if(a.finger!=size_t(-1))
      a.fingerBind = mul(transpose(bind),rotationOf(sk->tr[a.finger]));
    }
  valid = true;
  }

float Reach::armLength() const {
  return valid ? std::min(arms[0].length,arms[1].length)+Palm : 0.f;
  }

bool Reach::update(const Input& in) {
  if(!valid)
    return false;
  const Form& f = form();
  bool any = false;
  toward = in.toward;
  for(size_t i=0; i<2; ++i) {
    auto&       a    = arms[i];
    const auto& t    = in.target[i];
    const bool  want = in.active && in.free[i] && t.found;
    const bool  was  = a.w>0.f;
    if(want) {
      if(!a.has) {
        a.point   = t.point;
        a.normal  = t.normal;
        a.planted = false;
        }
      else if(!a.planted || !t.fixed) {
        // not on it yet, or it moves: follow, softly
        const float k = 1.f-std::exp(-float(in.dt)/std::max(f.follow,1.f));
        a.point  += (t.point-a.point)*k;
        a.normal  = normalize(a.normal+(t.normal-a.normal)*k);
        if(!t.fixed)
          a.planted = false;
        }
      a.fixed = t.fixed;
      a.has   = true;
      }
    const float step = float(in.dt)/std::max(want ? f.in : f.out,1.f);
    a.w = std::clamp(a.w + std::clamp((want ? 1.f : 0.f)-a.w,-step,step),0.f,1.f);
    if(!(a.w>0.f)) {
      a.has     = false;
      a.planted = false;
      a.gap     = 0;
      a.slip    = 0;
      }
    any |= was || a.w>0.f;
    }
  return any;
  }

void Reach::apply(Matrix4x4* tr, size_t numBones) {
  if(!valid)
    return;
  for(size_t i=0; i<2; ++i)
    if(arms[i].w>0.f && arms[i].has)
      applyArm(arms[i],arms[1-i],tr,numBones);
  }

void Reach::applyArm(Arm& a, const Arm& other, Matrix4x4* tr, size_t numBones) {
  if(a.upper>=numBones || a.fore>=numBones || a.hand>=numBones || other.upper>=numBones)
    return;
  const Form& f  = form();
  const float k  = smooth(a.w);
  const Vec3  s  = position(tr[a.upper]);
  const Vec3  h0 = position(tr[a.hand]);
  const Vec3  n  = a.normal;
  Vec3 outward = flat(s-position(tr[other.upper]));
  if(length(outward)<Eps)
    outward = Vec3(1,0,0);

  // the fingers on the surface: up a wall, leaning inwards; along the move on a top
  const float top    = std::clamp(n.y,0.f,1.f);
  const Vec3  prefer = Up*(1.f-top) + toward*top - outward*std::tan(f.lean*Deg)*(1.f-top);
  Vec3        fingers = normalize(prefer - n*dot(prefer,n));
  if(length(fingers)<Eps)
    fingers = normalize(Up - n*dot(Up,n));

  // the wrist so that the palm's middle is on the point; solved twice: a hand that cannot bend back as far as the
  // surface asks leans its fingers off it, and the wrist moves so that the palm still is where the point is
  Vec3 fw = fingers, pw = n*-1.f, at, forearm;
  const Vec3 pole = normalize(Vec3(0,-1,0) + outward*f.elbow);
  const std::vector<Matrix4x4> before(tr,tr+numBones);
  for(int pass=0; pass<2; ++pass) {
    if(pass>0)
      for(auto i:a.upperTree)
        tr[i] = before[i];
    Vec3        wrist = a.point + n*PalmThick - fw*Palm;
    const Vec3  d     = wrist-s;
    const float dist  = length(d);
    if(pass==0) {
      if(!a.planted && a.fixed && dist<=f.guard*a.length) {
        a.planted   = true;
        a.plantedAt = a.point;
        }
      if(a.planted && dist>MaxReach*a.length*1.05f)
        a.planted = false; // the body went away from it: the point follows the rays again
      }
    if(!a.planted && dist>f.guard*a.length && dist>Eps)
      wrist = s + d*(f.guard*a.length/dist); // on its way: out, the elbow kept bent

    const Vec3 to = h0 + (wrist-h0)*k;
    // the arm laid along the way to the wrist first: the solver bends it in the plane of its own line and the
    // elbow's way, and an arm that hangs has its line where the elbow should go
    const Vec3 line = normalize(to-s);
    const Vec3 own0 = normalize(position(tr[a.fore]) - (s+h0)*0.5f);
    const Mat3 lay  = rotationBetween(normalize(h0-s),line);
    rotateAbout(tr,a.upperTree,lay,s);
    // the elbow: as the animation has it, then down and out
    const Vec3 bend = normalize(mul(lay,own0)*(1.f-k) + pole*k);
    TwoBone tb;
    if(!twoBone(tr,a.upper,a.fore,a.hand,a.upperTree,a.foreTree,to,bend,MaxReach,0.f,tb)) {
      for(auto i:a.upperTree)
        tr[i] = before[i];
      return;
      }
    at      = position(tr[a.hand]);
    forearm = normalize(at-position(tr[a.fore]));
    fw      = fingers;
    pw      = n*-1.f;
    const float off = angleDeg(forearm,fw);
    if(off>f.wrist) {
      const Mat3 back = axisAngle(normalize(cross(fw,forearm)),(off-f.wrist)*Deg);
      fw = mul(back,fw);
      pw = mul(back,pw);
      }
    else
      break;
    }
  const Vec3 pl   = f.palmFlip>0.5f ? a.palmLocal*-1.f : a.palmLocal;
  const Mat3 want = mul(fromColumns(fw,pw,cross(fw,pw)),transpose(fromColumns(a.fingersLocal,pl,cross(a.fingersLocal,pl))));
  const Mat3 cur  = rotationOf(tr[a.hand]);
  const Mat3 now  = toMat3(slerp(toQuat(cur),toQuat(want),k));
  rotateAbout(tr,a.handTree,mul(now,transpose(cur)),at);

  // the fingers open
  if(a.finger<numBones) {
    auto curl = [&]() {
      const Mat3  bend = mul(mul(transpose(rotationOf(tr[a.hand])),rotationOf(tr[a.finger])),transpose(a.fingerBind));
      return std::acos(std::clamp((bend.m[0][0]+bend.m[1][1]+bend.m[2][2]-1.f)*0.5f,-1.f,1.f))/Deg;
      };
    a.curlWas = curl();
    const Mat3 fc = rotationOf(tr[a.finger]);
    const Mat3 ft = mul(rotationOf(tr[a.hand]),a.fingerBind);
    const Mat3 fn = toMat3(slerp(toQuat(fc),toQuat(ft),k));
    rotateAbout(tr,a.fingerTree,mul(fn,transpose(fc)),position(tr[a.finger]));
    a.curlNow = curl();
    }

  // the bench
  const Vec3 fingersNow = mul(rotationOf(tr[a.hand]),a.fingersLocal);
  const Vec3 palm       = at + fingersNow*Palm;
  a.gap   = dot(palm-a.point,n) - PalmThick;
  a.slip  = a.planted ? length(palm - n*dot(palm-a.plantedAt,n) - a.plantedAt) : 0.f;
  a.elbow = angleDeg(position(tr[a.upper])-position(tr[a.fore]),at-position(tr[a.fore]));
  a.wrist = angleDeg(forearm,fingersNow);
  }

float Reach::weight() const {
  return valid ? smooth(std::max(arms[0].w,arms[1].w)) : 0.f;
  }

float Reach::weight(size_t arm) const {
  return valid && arms[arm].has ? smooth(arms[arm].w) : 0.f;
  }

void Reach::probe(Probe& p) const {
  p = Probe();
  p.valid     = valid;
  p.armLength = armLength();
  p.curl      = {arms[0].curlWas,arms[0].curlNow};
  for(size_t i=0; i<2; ++i) {
    const auto& a = arms[i];
    p.weight[i]  = a.w;
    p.planted[i] = a.planted;
    p.gap[i]     = a.w>0.f ? a.gap : 0.f;
    p.slip[i]    = a.slip;
    p.elbow[i]   = a.w>0.f ? a.elbow : 0.f;
    p.wrist[i]   = a.w>0.f ? a.wrist : 0.f;
    }
  }
