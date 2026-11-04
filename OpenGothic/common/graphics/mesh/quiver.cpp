#include "quiver.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string_view>

#include "protomesh.h"
#include "skeleton.h"

using namespace Tempest;

namespace {

constexpr float Wall  = 0.4f;  // cm: the tube's wall
constexpr float Shaft = 1.f;   // cm an arrow's centre keeps from the wall (the shaft, the fletching's root)

Vec3 translation(const Matrix4x4& m) {
  return {m.at(3,0),m.at(3,1),m.at(3,2)};
  }

Matrix4x4 basis(const Vec3& x, const Vec3& y, const Vec3& z, const Vec3& at) {
  Matrix4x4 m;
  m.identity();
  const Vec3 c[] = {x,y,z,at};
  for(int i=0; i<4; ++i) {
    m.set(i,0,c[i].x);
    m.set(i,1,c[i].y);
    m.set(i,2,c[i].z);
    }
  return m;
  }

// a rotation by the angle (its sine, cosine) about the unit axis u
Matrix4x4 rotation(const Vec3& u, float s, float c) {
  Matrix4x4 m;
  m.identity();
  const float t = 1.f-c;
  const float r[3][3] = {
    {c+u.x*u.x*t,     u.x*u.y*t-u.z*s, u.x*u.z*t+u.y*s},
    {u.y*u.x*t+u.z*s, c+u.y*u.y*t,     u.y*u.z*t-u.x*s},
    {u.z*u.x*t-u.y*s, u.z*u.y*t+u.x*s, c+u.z*u.z*t    },
    };
  for(int row=0; row<3; ++row)
    for(int col=0; col<3; ++col)
      m.set(col,row,r[row][col]);
  return m;
  }

Vec3 turned(const Matrix4x4& m, const Vec3& v) {
  return {m.at(0,0)*v.x+m.at(1,0)*v.y+m.at(2,0)*v.z, m.at(0,1)*v.x+m.at(1,1)*v.y+m.at(2,1)*v.z,
          m.at(0,2)*v.x+m.at(1,2)*v.y+m.at(2,2)*v.z};
  }

float radiusAt(float depth) {
  const float k = std::clamp(depth/Quiver::Length,0.f,1.f);
  return Quiver::MouthRadius + (Quiver::BottomRadius-Quiver::MouthRadius)*k;
  }

bool nameHas(std::string_view name, std::initializer_list<std::string_view> parts) {
  for(auto p:parts)
    if(name.find(p)!=std::string_view::npos)
      return true;
  return false;
  }

std::vector<bool> bonesNamed(const Skeleton& sk, std::initializer_list<std::string_view> parts) {
  std::vector<bool> ret(sk.nodes.size(),false);
  for(size_t i=0; i<sk.nodes.size(); ++i)
    ret[i] = nameHas(sk.nodes[i].name,parts);
  return ret;
  }

}

Quiver::Place Quiver::place;
bool          Quiver::still = false;

bool Quiver::enabled() {
  static const bool on = [](){
    const char* v = std::getenv("OPENGOTHIC_QUIVER");
    return v==nullptr || std::string_view(v)!="0";
    }();
  return on;
  }

int Quiver::visibleArrows(size_t count) {
  if(count==0)
    return 0;
  if(count<2)
    return 1;
  if(count<5)
    return 3;
  if(count<10)
    return 6;
  if(count<20)
    return 9;
  return 12;
  }

std::vector<bool> Quiver::legBones(const Skeleton& sk) {
  return bonesNamed(sk,{"THIGH","CALF","FOOT","TOE"});
  }

std::vector<bool> Quiver::armBones(const Skeleton& sk) {
  return bonesNamed(sk,{"CLAVICLE","UPPERARM","FOREARM","HAND","FINGER","HEAD","NECK","ZS_"});
  }

Quiver::Fit Quiver::fit(const Skeleton& sk, const std::vector<SkinPoint>& bind) {
  Fit f;
  f.pelvis = sk.findNode("BIP01 PELVIS");
  if(f.pelvis==size_t(-1) || bind.empty())
    return f;

  // the body's own axes in the bind pose: the right thigh to the right, the toes forward
  const Vec3 up = {0,1,0};
  Vec3 right = {1,0,0}, fwd = {0,0,1};
  const size_t rt = sk.findNode("BIP01 R THIGH"), lt = sk.findNode("BIP01 L THIGH");
  if(rt!=size_t(-1) && lt!=size_t(-1) && translation(sk.tr[rt]).x<translation(sk.tr[lt]).x)
    right = {-1,0,0};
  const size_t foot = sk.findNode("BIP01 R FOOT"), toe = sk.findNode("BIP01 R TOE0");
  if(foot!=size_t(-1) && toe!=size_t(-1) && translation(sk.tr[toe]).z<translation(sk.tr[foot]).z)
    fwd = {0,0,-1};

  const auto arm = armBones(sk);
  float floor = bind[0].pos.y;
  for(auto& p:bind)
    floor = std::min(floor,p.pos.y);
  const Place pl     = place.on ? place : Place();
  const Vec3  side   = right;
  const float ar     = pl.around*float(M_PI)/180.f;
  right = right*std::cos(ar) - fwd*std::sin(ar); // the push: to the right side, or behind it (the probe)
  const Vec3 pelvis = translation(sk.tr[f.pelvis]);
  const Vec3 mouth0 = Vec3(pelvis.x,floor+pl.height,pelvis.z) - fwd*pl.back + side*pl.side;

  const float tb = std::tan(pl.tiltBack*float(M_PI)/180.f), to = std::tan(pl.tiltOut*float(M_PI)/180.f);
  const float tr = std::tan(pl.tiltRight*float(M_PI)/180.f);
  const Vec3  down = Vec3::normalize(up*-1.f - fwd*tb + right*to + side*tr);
  const Vec3  rp   = right - down*Vec3::dotProduct(right,down);
  const float rl   = rp.length();
  const Vec3  rh   = rp/rl;
  const float rd   = Vec3::dotProduct(right,down);

  // push out along the right till no point of the armour is within the tube and its clearance
  float s = 0;
  for(int iter=0; iter<4; ++iter) {
    float need = 0;
    for(const auto& p:bind) {
      if(p.bone<arm.size() && arm[p.bone])
        continue;
      const Vec3  w0 = p.pos-mouth0;
      const float t0 = Vec3::dotProduct(w0,down);
      const float t  = t0 - s*rd;
      if(t<-Clearance || t>Length+Clearance)
        continue;
      const Vec3  wp = w0 - down*t0;
      const float a  = Vec3::dotProduct(wp,rh);
      const float b  = (wp - rh*a).length();
      const float r  = radiusAt(t) + Clearance;
      if(b>=r)
        continue;
      need = std::max(need,(a + std::sqrt(r*r - b*b))/rl);
      }
    if(std::abs(need-s)<0.01f) {
      s = need;
      break;
      }
    s = need;
    }

  s = std::max(s,pl.minPush);
  const Vec3 mouth = mouth0 + right*s;
  const Vec3 y     = down*-1.f;
  const Vec3 z     = Vec3::normalize(fwd - y*Vec3::dotProduct(fwd,y));
  const Vec3 x     = Vec3::crossProduct(y,z);
  auto inv = sk.tr[f.pelvis];
  inv.inverse();
  f.local = inv*basis(x,y,z,mouth);
  f.out   = s;
  f.fwd   = fwd;
  f.spine = sk.findNode("BIP01 SPINE1");
  f.pelvisBind = inv;
  if(f.spine!=size_t(-1)) {
    f.spineBind = sk.tr[f.spine];
    f.spineBind.inverse();
    }
  f.valid = true;
  return f;
  }

std::vector<Matrix4x4> Quiver::arrowLayout(const ProtoMesh* arrow, int count) {
  std::vector<Matrix4x4> ret;
  if(arrow==nullptr || count<=0)
    return ret;
  const Vec3* box = arrow->bbox();
  const float len = box[1].x-box[0].x;
  // the shaft runs through the middle of the head (the fifth of the arrow at the tip): the bounds are off it
  // by the fletching
  float cy = (box[0].y+box[1].y)*0.5f, cz = (box[0].z+box[1].z)*0.5f;
  {
    Vec3 lo = {0,1e9f,1e9f}, hi = {0,-1e9f,-1e9f};
    bool any = false;
    for(auto& p:arrow->points)
      if(p.x<box[0].x+len*0.2f) {
        lo.y = std::min(lo.y,p.y); lo.z = std::min(lo.z,p.z);
        hi.y = std::max(hi.y,p.y); hi.z = std::max(hi.z,p.z);
        any  = true;
        }
    if(any) {
      cy = (lo.y+hi.y)*0.5f;
      cz = (lo.z+hi.z)*0.5f;
      }
  }
  // the arrows stand on the bottom; a long one shows no more than FletchingMax
  const float base = std::clamp(len-(Length-1.f),0.f,FletchingMax);
  const float rin  = MouthRadius-Wall-Shaft;
  for(int i=0; i<count; ++i) {
    // the nocks: a sunflower over the mouth, a lone one aside; the tips: on the axis at the bottom, so the
    // heads (wider than the bottom leaves to an arrow off the axis) stay in the tube
    const float r     = count==1 ? rin*0.6f : rin*std::sqrt((float(i)+0.5f)/float(count));
    const float an    = float(i)*2.39996f;
    const float lift  = std::fmod(float(i)*0.37f,1.f)*2.f;   // 0..2 cm: not all fletchings level
    const float above = std::min(base+lift,FletchingMax);
    const Vec3  nock  = {r*std::cos(an),above,r*std::sin(an)};
    const Vec3  tip   = {0,above-len,0};
    const Vec3  ax    = Vec3::normalize(nock-tip);            // the arrow's +X: from the tip to the nock
    // the head's flat across the radius (along the circle), so it reaches out the least
    Vec3 flat = {-std::sin(an),0,std::cos(an)};
    flat = Vec3::normalize(flat - ax*Vec3::dotProduct(flat,ax));
    Matrix4x4 m = basis(ax,Vec3::crossProduct(flat,ax),flat,nock);
    m.translate(-box[1].x,-cy,-cz);
    ret.push_back(m);
    }
  return ret;
  }

void Quiver::set(const State& s, const Fit& f, MeshObjects::Mesh&& b, std::vector<MeshObjects::Mesh>&& a,
                 std::vector<Matrix4x4>&& al) {
  // a new count of arrows keeps the give: no jump; a new fit (the armour, the skeleton) starts at rest
  const bool same = fitted.valid && f.valid && st.body==s.body && st.skeleton==s.skeleton;
  st         = s;
  fitted     = f;
  body       = std::move(b);
  arrows     = std::move(a);
  arrowLocal = std::move(al);
  if(!same)
    rest();
  }

void Quiver::clear() {
  st         = State();
  fitted     = Fit();
  body       = MeshObjects::Mesh();
  arrows.clear();
  arrowLocal.clear();
  nearIds.clear();
  rest();
  }

void Quiver::rebind(const Skeleton& sk) {
  if(fitted.valid)
    fitted.pelvis = sk.findNode("BIP01 PELVIS");
  }

float Quiver::twistOf(const Matrix4x4& pelvis, const Matrix4x4& spine) const {
  if(fitted.spine==size_t(-1))
    return 0;
  // the pelvis's and the spine's forward in the world, about the vertical
  Vec3 fp = turned(pelvis*fitted.pelvisBind,fitted.fwd), fs = turned(spine*fitted.spineBind,fitted.fwd);
  fp.y = 0;
  fs.y = 0;
  if(fp.length()<1e-3f || fs.length()<1e-3f)
    return 0;
  fp = Vec3::normalize(fp);
  fs = Vec3::normalize(fs);
  return std::atan2(Vec3::dotProduct(Vec3::crossProduct(fp,fs),Vec3(0,1,0)),Vec3::dotProduct(fp,fs));
  }

Matrix4x4 Quiver::restFrame(const Matrix4x4& pelvis, const Matrix4x4& spine) const {
  (void)spine;
  Matrix4x4 frame = pelvis*fitted.local;
  if(yawOffset==0.f)
    return frame;
  // turned about the vertical through the ring by half the gait's turn of the pelvis against SPINE1 (step)
  const Vec3 at = translation(frame);
  Matrix4x4 m;
  m.identity();
  m.translate(at);
  m.mul(rotation(Vec3(0,1,0),std::sin(yawOffset),std::cos(yawOffset)));
  m.translate(at*-1.f);
  m.mul(frame);
  return m;
  }

void Quiver::sync(const Matrix4x4& pelvis, const Matrix4x4& spine) {
  if(!st.on || !fitted.valid)
    return;
  const auto at = restFrame(pelvis,spine)*swingMatrix();
  if(!body.isEmpty())
    body.setObjMatrix(at);
  for(size_t i=0; i<arrows.size() && i<arrowLocal.size(); ++i)
    arrows[i].setObjMatrix(at*arrowLocal[i]);
  }

void Quiver::setTextureOwner(const Vec3& position, bool spatial) {
  body.setTextureOwner(position,spatial);
  for(auto& a:arrows)
    a.setTextureOwner(position,spatial);
  }

Quiver::Probe Quiver::probe(const Matrix4x4& pelvis, const Matrix4x4& spine, const std::vector<SkinPoint>& posed,
                            const std::vector<bool>& legBone, const std::vector<bool>& armBone,
                            const std::vector<Vec3>& bowPts, const std::vector<Vec3>& longPts) const {
  Probe p;
  p.on = st.on && fitted.valid;
  if(!p.on)
    return p;
  p.arrows = int(arrows.size());
  p.mouth  = translation(fitted.local);
  p.out    = fitted.out;
  const auto rest = restFrame(pelvis,spine);
  auto inv = rest*swingMatrix();
  inv.inverse();
  {
    const Vec3 a = Vec3::normalize(axis);
    p.swing   = std::acos(std::clamp(-a.y,-1.f,1.f))*180.f/float(M_PI);
    p.back    = std::atan2(-a.z,-a.y)*180.f/float(M_PI);
    p.outward = std::atan2( a.x,-a.y)*180.f/float(M_PI);
    p.yield   = std::atan2(devTarget.length(),MassAt)*180.f/float(M_PI);
    // the rest's turn against SPINE1 about the vertical: the pelvis's less the half of the gait's
    p.twist = (twistOf(pelvis,spine)-yawOffset)*180.f/float(M_PI);
  }
  auto depthOf = [&](Vec3 q) {
    inv.project(q);
    if(q.y>0 || q.y<-Length)
      return 0.f;
    return std::max(0.f,radiusAt(-q.y) - std::hypot(q.x,q.z));
    };
  for(auto& pt:posed) {
    const float depth = depthOf(pt.pos);
    if(depth<=0)
      continue;
    if(pt.bone<armBone.size() && armBone[pt.bone])
      p.arm = std::max(p.arm,depth); else
    if(pt.bone<legBone.size() && legBone[pt.bone])
      p.leg = std::max(p.leg,depth); else
      p.armor = std::max(p.armor,depth);
    }
  for(auto& v:bowPts)
    p.bow = std::max(p.bow,depthOf(v));
  for(auto& v:longPts)
    p.longsword = std::max(p.longsword,depthOf(v));
  // the arrows: the span along the quiver (the nock's end and the tip of each), how far out of the wall
  bool first = true;
  for(size_t i=0; i<arrows.size() && i<arrowLocal.size(); ++i) {
    const auto* pm = arrows[i].protoMesh();
    if(pm==nullptr)
      continue;
    for(auto v:pm->points) {
      arrowLocal[i].project(v);
      if(first) {
        p.top    = v.y;
        p.bottom = v.y;
        first    = false;
        }
      p.top    = std::max(p.top,v.y);
      p.bottom = std::min(p.bottom,v.y);
      if(v.y<0 && v.y>-Length)
        p.blade = std::max(p.blade,std::hypot(v.x,v.z) - radiusAt(-v.y));
      }
    }
  p.bottom += Length;
  return p;
  }

void Quiver::nearPoints(const Fit& f, const Skeleton& sk, const std::vector<SkinPoint>& bind,
                        std::vector<size_t>& ids, std::vector<uint8_t>& leg) {
  ids.clear();
  leg.clear();
  if(!f.valid || f.pelvis>=sk.tr.size())
    return;
  // the quiver at rest in the bind pose; a point that far from it may come to it as the legs and the flaps
  // move: the nearest first (the thigh, the boot, the flaps), the far ones dropped past MaxPoints. Not the
  // arms: their capsules push it; a robe's wide sleeves (cloth) do not
  constexpr float  Reach     = 45.f;
  constexpr size_t MaxLeg    = 160, MaxOther = 160;
  auto inv = sk.tr[f.pelvis]*f.local;
  inv.inverse();
  const auto arm  = armBones(sk);
  const auto legs = legBones(sk);
  std::vector<std::pair<float,size_t>> near, nearLeg;
  for(size_t i=0; i<bind.size(); ++i) {
    if(bind[i].src==size_t(-1) || (bind[i].bone<arm.size() && arm[bind[i].bone]))
      continue;
    Vec3 q = bind[i].pos;
    inv.project(q);
    if(q.y>5.f || q.y<-Length-Reach)
      continue;
    const float gap = std::hypot(q.x,q.z)-radiusAt(-q.y);
    if(gap>Reach)
      continue;
    if(bind[i].bone<legs.size() && legs[bind[i].bone])
      nearLeg.emplace_back(gap,i); else
      near.emplace_back(gap,i);
    }
  // the nearest of each, so many: the leg's own share (the flaps must not crowd it out)
  for(auto* list:{&nearLeg,&near}) {
    std::sort(list->begin(),list->end());
    const size_t most = list==&nearLeg ? MaxLeg : MaxOther;
    if(list->size()>most) {
      // every n-th of the nearest 3x as many: the whole near part, thinned
      std::vector<std::pair<float,size_t>> thin;
      const size_t pool = std::min(list->size(),most*3);
      for(size_t k=0; k<most; ++k)
        thin.push_back((*list)[k*pool/most]);
      *list = std::move(thin);
      }
    for(auto& n:*list) {
      ids.push_back(bind[n.second].src);
      leg.push_back(list==&nearLeg ? 1 : 0);
      }
    }
  }

void Quiver::rest() {
  axis      = {0,-1,0};
  moving    = false;
  devV      = Vec3();
  devHeld   = Vec3();
  yawOffset = 0;
  }

Matrix4x4 Quiver::swingMatrix() const {
  // the rotation that takes the rest's bottom (0,-1,0) to the axis: about their common normal
  const Vec3  from = {0,-1,0};
  const Vec3  to   = Vec3::normalize(axis);
  const Vec3  k    = Vec3::crossProduct(from,to);
  const float s    = k.length();
  if(s<1e-6f) {
    Matrix4x4 m;
    m.identity();
    return m;
    }
  return rotation(k/s,s,Vec3::dotProduct(from,to));
  }

void Quiver::step(const Matrix4x4& pelvis, const Matrix4x4& spine, const std::vector<Obstacle>& obstacles, float dt) {
  if(!st.on || !fitted.valid)
    return;
  if(still) {
    rest();
    return;
    }
  // no time: as it was (a forced pose update)
  if(dt<=0.f)
    return;
  // the gait's turn of the pelvis against SPINE1: what the slow follower does not catch (a stance's twist,
  // the bow drawn, is caught: the quiver stays on the pelvis then)
  twistNow = twistOf(pelvis,spine);
  if(!moving)
    twistSlow = twistNow;
  twistSlow += (twistNow-twistSlow)*(1.f-std::exp(-dt/TwistSlow));
  yawOffset  = 0.5f*(twistNow-twistSlow);
  const auto frame = restFrame(pelvis,spine);  // the quiver at rest, in the world
  const Vec3 pivot = translation(frame);
  const Vec3 restDir = Vec3::normalize(turned(frame,{0,-1,0}));

  // the target: the rest, turned away from what is in the tube; each contact allows so much (the leg more)
  Vec3  target = restDir;
  float allow  = 0; // rad it may turn: the most any contact allows
  for(int pass=0; pass<2; ++pass) {
    bool hit = false;
    for(auto& o:obstacles) {
      // the closest points of the axis (pivot + target*t, t 0..Length) and the obstacle's segment
      const Vec3  e  = o.b-o.a;
      const float ee = Vec3::dotProduct(e,e);
      float u = 0;
      if(ee>1e-6f) {
        const Vec3  r0  = pivot-o.a;
        const float ae  = Vec3::dotProduct(target,e);
        const float er  = Vec3::dotProduct(e,r0);
        const float den = ee - ae*ae;
        const float t0  = den>1e-6f ? std::clamp((ae*er - ee*Vec3::dotProduct(target,r0))/den,0.f,Length) : 0.f;
        u = std::clamp((t0*ae + er)/ee,0.f,1.f);
        }
      const Vec3  onObs = o.a + e*u;
      const float t     = std::clamp(Vec3::dotProduct(onObs-pivot,target),0.f,Length);
      if(t<4.f)
        continue; // at the ring: it cannot turn away there
      Vec3 away = (pivot + target*t) - onObs;
      away -= target*Vec3::dotProduct(away,target);
      const float dist = away.length();
      const float need = radiusAt(t) + o.r + PushMargin;
      if(dist>=need || dist<1e-3f)
        continue;
      target = Vec3::normalize(target + away*((need-dist)/(dist*t)));
      const float cm  = o.leg ? LegYieldMax   : YieldMax;
      const float deg = o.leg ? LegYieldAngle : YieldAngle;
      allow  = std::max(allow,std::min(std::atan2(cm,t),deg*float(M_PI)/180.f));
      hit    = true;
      }
    if(!hit)
      break;
    }
  // the target across the axis in the rest frame, at MassAt down it, no farther than allowed
  auto inv = frame;
  inv.inverse();
  Vec3 tl = turned(inv,target);
  if(tl.y>-0.1f)
    tl = {0,-1,0};
  Vec3 dTarget = {tl.x/-tl.y*MassAt,0,tl.z/-tl.y*MassAt};
  const float maxDev = MassAt*std::tan(allow);
  if(dTarget.length()>maxDev)
    dTarget = dTarget*(maxDev/std::max(dTarget.length(),1e-6f));
  // held: a new or a greater give at once, a smaller one lets go slowly (the leg comes back each step)
  if(!moving || dTarget.length()>=devHeld.length())
    devHeld = dTarget; else
    devHeld = dTarget + (devHeld-dTarget)*std::exp(-dt/GiveRelease);
  dTarget   = devHeld;
  devTarget = dTarget;

  // a jump (a teleport, a load) or the first step: at the target
  if(!moving || (pivot-lastPivot).length()>100.f || dt>0.25f) {
    dev       = dTarget;
    devV      = Vec3();
    lastPivot = pivot;
    moving    = true;
    } else {
    // critically damped to the target, no overshoot (90% in GiveTime), no faster than GiveStep in 1/60 s
    lastPivot = pivot;
    const Vec3  from = dev;
    const float w    = 3.89f/GiveTime;
    const int   n    = std::max(1,int(std::ceil(dt*240.f)));
    const float h    = dt/float(n);
    for(int i=0; i<n; ++i) {
      Vec3 acc = (dTarget-dev)*(w*w) - devV*(2.f*w);
      acc.y = 0;
      devV += acc*h;
      dev  += devV*h;
      dev.y = 0;
      }
    const Vec3  moved = dev-from;
    const float most  = MassAt*std::tan(GiveStep*float(M_PI)/180.f)*dt*60.f;
    if(moved.length()>most) {
      dev  = from + moved*(most/moved.length());
      devV = (dev-from)/dt;
      }
    }
  axis = Vec3::normalize(Vec3(dev.x,-MassAt,dev.z));
  }
