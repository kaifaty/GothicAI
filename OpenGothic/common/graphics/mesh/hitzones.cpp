#include "hitzones.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

#include "ikmath.h"
#include "skeleton.h"

using namespace Tempest;
using namespace IkMath;

namespace {

constexpr float RefSize   = 160.f; // cm: a human from the toes to the head bone; the radii below are for it
constexpr float FitPart   = 0.8f;  // the thickness takes in that much of the flesh around a bone (the
constexpr float FitHead   = 0.9f;  // head more: a snout, the back of the skull is the head too)...
constexpr size_t FitMin   = 3;     // ...if it has that many points
constexpr float FitMax    = 0.35f; // of the creature's size at most

bool fitEnabled = true;

struct Kind final {
  HitZones::Zone zone;
  float          r;     // cm, for RefSize
  };

bool has(std::string_view name, std::string_view part) {
  return name.find(part)!=std::string_view::npos;
  }

bool isSlot(std::string_view name) {
  return name.substr(0,3)=="ZS_";
  }

Kind kindOf(std::string_view n) {
  if(has(n,"HEAD"))
    return {HitZones::Head,11};
  if(has(n,"KIEFER") || has(n,"JAW"))
    return {HitZones::Head,8};
  if(has(n,"NECK"))
    return {HitZones::Body,7};
  if(has(n,"CLAVICLE"))
    return {HitZones::Body,8};
  if(has(n,"UPPERARM"))
    return {HitZones::Limb,6};
  if(has(n,"FOREARM"))
    return {HitZones::Limb,5};
  if(has(n,"FINGER"))
    return {HitZones::Limb,3};
  if(has(n,"HAND"))
    return {HitZones::Limb,4};
  if(has(n,"THIGH"))
    return {HitZones::Limb,8};
  if(has(n,"CALF"))
    return {HitZones::Limb,6};
  if(has(n,"TOE"))
    return {HitZones::Limb,4};
  if(has(n,"FOOT"))
    return {HitZones::Limb,5};
  if(has(n,"TAIL"))
    return {HitZones::Limb,5};
  if(has(n,"LEG") || has(n,"ARM") || has(n,"CLAW") || has(n,"ZANGE") || has(n,"PINCER") || has(n,"FUEHLER") || has(n,"WING"))
    return {HitZones::Limb,4};
  if(has(n,"PELVIS"))
    return {HitZones::Body,14};
  if(has(n,"SPINE"))
    return {HitZones::Body,15};
  return {HitZones::Body,12};
  }

float segmentDist(const Vec3& p, const Vec3& a, const Vec3& b) {
  const Vec3  ab = b-a;
  const float l  = dot(ab,ab);
  const float t  = l>1e-6f ? std::clamp(dot(p-a,ab)/l,0.f,1.f) : 0.f;
  return length(a+ab*t-p);
  }

// a ray (rd: unit) and a sphere: the distance it enters at, 0 from inside, -1: misses
float sphereHit(const Vec3& ro, const Vec3& rd, const Vec3& c, float r) {
  const Vec3  oc = ro - c;
  const float b  = dot(oc,rd);
  const float cc = dot(oc,oc) - r*r;
  if(cc<=0.f)
    return 0.f;
  const float h = b*b - cc;
  if(h<0.f)
    return -1.f;
  const float t = -b - std::sqrt(h);
  return t>=0.f ? t : -1.f;
  }

// a ray and a capsule from pa to pb
float capsuleHit(const Vec3& ro, const Vec3& rd, const Vec3& pa, const Vec3& pb, float r) {
  const Vec3  ba   = pb - pa;
  const float baba = dot(ba,ba);
  float       best = -1.f;
  auto take = [&](float t) {
    if(t>=0.f && (best<0.f || t<best))
      best = t;
    };
  if(baba>1e-4f) {
    const Vec3  oa   = ro - pa;
    const float k    = std::clamp(dot(oa,ba)/baba,0.f,1.f);
    if(length(oa - ba*k)<=r)
      return 0.f;
    const float bard = dot(ba,rd);
    const float baoa = dot(ba,oa);
    const float a    = baba - bard*bard;
    if(a>1e-6f) {
      const float b = baba*dot(rd,oa) - baoa*bard;
      const float c = baba*dot(oa,oa) - baoa*baoa - r*r*baba;
      const float h = b*b - a*c;
      if(h<0.f)
        return -1.f; // misses the endless cylinder: the caps lie inside it
      const float t = (-b - std::sqrt(h))/a;
      const float y = baoa + t*bard;
      if(y>0.f && y<baba)
        take(t);
      }
    take(sphereHit(ro,rd,pb,r));
    }
  take(sphereHit(ro,rd,pa,r));
  return best;
  }

}

bool HitZones::isFitEnabled() {
  return fitEnabled;
  }

void HitZones::setFitEnabled(bool e) {
  fitEnabled = e;
  }

void HitZones::setSkeleton(const Skeleton* sk) {
  caps.clear();
  shape.clear();
  skeleton = sk;
  if(sk==nullptr || sk->tr.size()!=sk->nodes.size())
    return;

  auto& nodes = sk->nodes;
  auto  skip  = [&](size_t i) {
    return nodes[i].parent==size_t(-1) || isSlot(nodes[i].name) || has(nodes[i].name,"FOOTSTEPS");
    };
  auto  pos   = [&](size_t i) {
    return position(sk->tr[i]);
    };
  // a point (in the model) in the frame of a bone
  auto  local = [&](size_t bone, const Vec3& p) {
    Matrix4x4 inv = sk->tr[bone];
    inv.inverse();
    Vec3 at = p;
    inv.project(at);
    return End{bone,at};
    };

  // how big the creature is: the radii are for a human
  Vec3 lo = {}, hi = {};
  bool any = false;
  for(size_t i=0; i<nodes.size(); ++i) {
    if(skip(i))
      continue;
    const Vec3 p = pos(i);
    lo  = any ? Vec3(std::min(lo.x,p.x),std::min(lo.y,p.y),std::min(lo.z,p.z)) : p;
    hi  = any ? Vec3(std::max(hi.x,p.x),std::max(hi.y,p.y),std::max(hi.z,p.z)) : p;
    any = true;
    }
  if(!any)
    return;
  const float k = std::max({hi.x-lo.x,hi.y-lo.y,hi.z-lo.z})/RefSize;
  if(k<=0.01f)
    return;

  for(size_t i=0; i<nodes.size(); ++i) {
    if(skip(i))
      continue;
    const Kind ki = kindOf(nodes[i].name);
    const Vec3 pi = pos(i);

    // from the parent to the bone: the parent's zone and thickness
    const size_t p = nodes[i].parent;
    if(p<nodes.size() && !skip(p)) {
      const Kind kp = kindOf(nodes[p].name);
      const Vec3 pp = pos(p);
      Capsule c;
      c.a    = End{p,Vec3()};
      c.b    = End{i,Vec3()};
      c.r     = kp.r*k;
      c.zone  = kp.zone;
      c.owner = p;
      // the trunk ends where the neck, the head begin: else its round end covers the chin
      const float len = length(pi-pp);
      if(kp.zone==Body && (ki.zone==Head || has(nodes[i].name,"NECK")) && len>0.f)
        c.b = local(i, pi - (pi-pp)*(std::min(c.r,len)/len));
      caps.push_back(c);
      }

    bool children = false;
    End  helmet   = {size_t(-1),Vec3()};
    for(size_t j=0; j<nodes.size(); ++j) {
      if(nodes[j].parent!=i)
        continue;
      if(!skip(j))
        children = true;
      else if(nodes[j].name=="ZS_HELMET")
        helmet = End{j,Vec3()};
      }

    if(ki.zone==Head && has(nodes[i].name,"HEAD")) {
      // the skull: up to the helmet slot (a human), else on past the bone, away from the neck
      Capsule c;
      c.a     = End{i,Vec3()};
      c.r     = ki.r*k;
      c.zone  = Head;
      c.owner = i;
      if(helmet.bone!=size_t(-1)) {
        // in the head's frame: the head's mesh goes with the head, the slot does not quite
        c.b = local(i, pos(helmet.bone));
        } else {
        Vec3 dir = p<nodes.size() ? normalize(pi-pos(p)) : Vec3(0,1,0);
        if(length(dir)<0.5f)
          dir = Vec3(0,1,0);
        c.b = local(i, pi + dir*(c.r*1.2f));
        }
      caps.push_back(c);
      }
    else if(!children) {
      // an end: a hand, a toe, a tail tip
      Capsule c;
      c.a     = End{i,Vec3()};
      c.b     = c.a;
      c.r     = ki.r*k;
      c.zone  = ki.zone;
      c.owner = i;
      caps.push_back(c);
      }
    }
  for(auto& c:caps)
    c.guess = c.r;
  size = k*RefSize;
  }

void HitZones::fit(const std::vector<Shape>& sh) {
  shape.clear();
  for(auto& c:caps) {
    c.r      = c.guess;
    c.fitted = false;
    }
  if(skeleton==nullptr || caps.empty())
    return;
  const size_t numNodes = skeleton->tr.size();
  for(auto& s:sh) {
    if(s.byNode!=nullptr)
      for(size_t n=0; n<s.byNode->size() && n<numNodes; ++n)
        for(auto& p:(*s.byNode)[n])
          shape.push_back({n,p});
    if(s.points!=nullptr && s.node<numNodes)
      for(auto& p:*s.points)
        shape.push_back({s.node,p});
    }

  // each point to the nearest capsule of its bone (else of any: the flesh of a bone without one), in the
  // bind pose; the thickness takes in most of each one's points
  std::vector<std::vector<float>> dist(caps.size());
  auto& bind = skeleton->tr;
  for(auto& pt:shape) {
    Vec3 q = pt.at;
    bind[pt.node].project(q);
    size_t best = size_t(-1);
    float  bd   = 0;
    for(int pass=0; pass<2 && best==size_t(-1); ++pass)
      for(size_t i=0; i<caps.size(); ++i) {
        auto& c = caps[i];
        if(pass==0 && c.owner!=pt.node)
          continue;
        const float d = segmentDist(q,point(bind.data(),c.a),point(bind.data(),c.b));
        if(best==size_t(-1) || d<bd) {
          best = i;
          bd   = d;
          }
        }
    if(best!=size_t(-1))
      dist[best].push_back(bd);
    }
  if(!fitEnabled)
    return;
  for(size_t i=0; i<caps.size(); ++i) {
    auto& d = dist[i];
    if(d.size()<FitMin)
      continue;
    std::sort(d.begin(),d.end());
    const float part = caps[i].zone==Head ? FitHead : FitPart;
    const float r    = d[std::min(d.size()-1,size_t(float(d.size())*part))];
    caps[i].r      = std::clamp(r,0.5f,size*FitMax);
    caps[i].fitted = true;
    }
  }

Vec3 HitZones::point(const Matrix4x4* tr, const End& e) {
  Vec3 p = e.at;
  tr[e.bone].project(p);
  return p;
  }

HitZones::Zone HitZones::rayTest(const Matrix4x4* tr, size_t numBones, float scale, const Vec3& from, const Vec3& to, float& t, size_t* bone) const {
  t = 0;
  if(bone!=nullptr)
    *bone = size_t(-1);
  const Vec3  d   = to - from;
  const float len = length(d);
  if(tr==nullptr || len<1e-3f)
    return None;
  const Vec3 rd   = d*(1.f/len);
  float      best = len;
  Zone       ret  = None;
  for(auto& c:caps) {
    if(c.a.bone>=numBones || c.b.bone>=numBones)
      continue;
    const float h = capsuleHit(from,rd,point(tr,c.a),point(tr,c.b),c.r*scale);
    if(h>=0.f && h<best) {
      best = h;
      ret  = c.zone;
      if(bone!=nullptr)
        *bone = c.owner;
      }
    }
  t = best;
  return ret;
  }

size_t HitZones::closestBone(const Matrix4x4* tr, size_t numBones, float scale, const Vec3& at) const {
  if(tr==nullptr)
    return size_t(-1);
  float best = std::numeric_limits<float>::infinity();
  size_t bone = size_t(-1);
  for(const auto& c:caps) {
    if(c.a.bone>=numBones || c.b.bone>=numBones || c.owner>=numBones)
      continue;
    const auto a = point(tr,c.a), b = point(tr,c.b), ab = b-a;
    const float length2 = ab.quadLength();
    const float t = length2>1.e-6f ? std::clamp(Vec3::dotProduct(at-a,ab)/length2,0.f,1.f) : 0.f;
    const float distance = std::abs((at-(a+ab*t)).length()-c.r*scale);
    if(distance<best) {
      best = distance;
      bone = c.owner;
      }
    }
  return bone;
  }

bool HitZones::center(const Matrix4x4* tr, size_t numBones, Zone z, Vec3& at) const {
  // the head, the trunk: the middle of their capsules; a limb: the biggest one (a thigh)
  Vec3  sum  = {};
  float wsum = 0, wmax = 0;
  for(auto& c:caps) {
    if(c.zone!=z || c.a.bone>=numBones || c.b.bone>=numBones)
      continue;
    const Vec3  pa = point(tr,c.a), pb = point(tr,c.b);
    const float w  = (length(pb-pa) + 2.f*c.r)*c.r*c.r;
    if(z==Limb) {
      if(w>wmax) {
        wmax = w;
        at   = (pa+pb)*0.5f;
        }
      continue;
      }
    sum  += (pa+pb)*(0.5f*w);
    wsum += w;
    }
  if(z==Limb)
    return wmax>0.f;
  if(wsum<=0.f)
    return false;
  at = sum*(1.f/wsum);
  return true;
  }

void HitZones::posed(const Matrix4x4* tr, size_t numBones, float scale, std::vector<Posed>& out) const {
  out.clear();
  for(auto& c:caps) {
    if(c.a.bone>=numBones || c.b.bone>=numBones)
      continue;
    out.push_back({point(tr,c.a),point(tr,c.b),c.r*scale,c.zone,c.owner});
    }
  }

float HitZones::horizontalBodyGap(const std::vector<Posed>& first, const std::vector<Posed>& second,
                                 const Vec3& offset) {
  float gap = std::numeric_limits<float>::infinity();
  for(const auto& firstCapsule:first) {
    if(firstCapsule.zone!=Body)
      continue;
    const Vec3 a(firstCapsule.a.x+offset.x,0,firstCapsule.a.z+offset.z);
    const Vec3 u(firstCapsule.b.x-firstCapsule.a.x,0,firstCapsule.b.z-firstCapsule.a.z);
    const float aa = dot(u,u);
    for(const auto& secondCapsule:second) {
      if(secondCapsule.zone!=Body)
        continue;
      const Vec3 b(secondCapsule.a.x,0,secondCapsule.a.z);
      const Vec3 v(secondCapsule.b.x-secondCapsule.a.x,0,secondCapsule.b.z-secondCapsule.a.z);
      const Vec3 r = a-b;
      const float ee = dot(v,v), f = dot(v,r);
      float s = 0, t = 0;
      if(aa<=1.e-6f)
        t = ee>1.e-6f ? std::clamp(f/ee,0.f,1.f) : 0.f;
      else {
        const float c = dot(u,r);
        if(ee<=1.e-6f)
          s = std::clamp(-c/aa,0.f,1.f);
        else {
          const float bb = dot(u,v), denominator = aa*ee-bb*bb;
          if(denominator>1.e-6f)
            s = std::clamp((bb*f-c*ee)/denominator,0.f,1.f);
          t = (bb*s+f)/ee;
          if(t<0) { t = 0; s = std::clamp(-c/aa,0.f,1.f); }
          else if(t>1) { t = 1; s = std::clamp((bb-c)/aa,0.f,1.f); }
          }
        }
      gap = std::min(gap,length(r+u*s-v*t)-firstCapsule.r-secondCapsule.r);
      }
    }
  return gap;
  }

std::string HitZones::dump(const Matrix4x4* tr, size_t numBones, float scale) const {
  std::string out;
  for(auto& c:caps) {
    if(c.a.bone>=numBones || c.b.bone>=numBones)
      continue;
    const Vec3 a = point(tr,c.a), b = point(tr,c.b);
    char buf[200];
    std::snprintf(buf,sizeof(buf),"  %s %zu-%zu: %.0f %.0f %.0f - %.0f %.0f %.0f r %.1f (guess %.1f)\n",std::string(name(c.zone)).c_str(),c.a.bone,c.b.bone,
                  a.x,a.y,a.z,b.x,b.y,b.z,double(c.r*scale),double(c.guess*scale));
    out += buf;
    }
  return out;
  }

std::string HitZones::coverage(const Matrix4x4* tr, size_t numBones, float scale) const {
  // each point of the body as posed: the zone of the capsule nearest to it among its bone's (the flesh of
  // that zone), whether it is in a capsule of that zone, in one of another zone only, in none
  struct Count { size_t n = 0, own = 0, wrong = 0; };
  Count cnt[4];
  for(auto& pt:shape) {
    if(pt.node>=numBones)
      continue;
    Vec3 q = pt.at;
    tr[pt.node].project(q);
    Zone  zone = None, any = None;
    float bd   = 0;
    bool  in   = false;
    for(auto& c:caps) {
      if(c.a.bone>=numBones || c.b.bone>=numBones)
        continue;
      const float d = segmentDist(q,point(tr,c.a),point(tr,c.b));
      if(c.owner==pt.node && (zone==None || d<bd)) {
        zone = c.zone;
        bd   = d;
        }
      if(d<=c.r*scale)
        any = any==None ? c.zone : any;
      }
    if(zone==None)
      continue;
    for(auto& c:caps)
      if(c.zone==zone && c.a.bone<numBones && c.b.bone<numBones &&
         segmentDist(q,point(tr,c.a),point(tr,c.b))<=c.r*scale)
        in = true;
    auto& k = cnt[zone];
    ++k.n;
    if(in)
      ++k.own;
    else if(any!=None)
      ++k.wrong;
    }
  std::string out = "  coverage (points in their zone / in another / in none):";
  for(Zone z:{Head,Body,Limb}) {
    auto& k = cnt[z];
    if(k.n==0)
      continue;
    char buf[96];
    std::snprintf(buf,sizeof(buf)," %s %zu: %.0f%% / %.0f%% / %.0f%%;",std::string(name(z)).c_str(),k.n,
                  100.0*double(k.own)/double(k.n),100.0*double(k.wrong)/double(k.n),100.0*double(k.n-k.own-k.wrong)/double(k.n));
    out += buf;
    }
  size_t fitted = 0;
  for(auto& c:caps)
    if(c.fitted)
      ++fitted;
  char buf[96];
  std::snprintf(buf,sizeof(buf)," %zu of %zu capsules fitted (%s)",fitted,caps.size(),fitEnabled ? "fit on" : "fit off");
  return out + buf;
  }

std::string_view HitZones::name(Zone z) {
  switch(z) {
    case None: return "none";
    case Body: return "body";
    case Head: return "head";
    case Limb: return "limb";
    }
  return "?";
  }
