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

// the axis (the rest frame, the bottom's way) for a turn by its angle across: (x, z) radians toward that way
Vec3 axisOfAngle(const Vec3& a) {
  const float l = std::hypot(a.x,a.z);
  if(l<1e-6f)
    return {0,-1,0};
  return {std::sin(l)*a.x/l,-std::cos(l),std::sin(l)*a.z/l};
  }

// cm between segments a0-a1 and b0-b1 (sampled: the lift's lines are long, the capsules short)
float segSeg(const Vec3& a0, const Vec3& a1, const Vec3& b0, const Vec3& b1) {
  auto ptSeg = [](const Vec3& p, const Vec3& a, const Vec3& b) {
    const Vec3  ab = b-a;
    const float l  = Vec3::dotProduct(ab,ab);
    const float t  = l>1e-6f ? std::clamp(Vec3::dotProduct(p-a,ab)/l,0.f,1.f) : 0.f;
    return (a+ab*t-p).length();
    };
  float best = 1e9f;
  const float la = (a1-a0).length();
  const int   n  = std::clamp(int(la/2.f),1,64);
  for(int i=0; i<=n; ++i)
    best = std::min(best,ptSeg(a0+(a1-a0)*(float(i)/float(n)),b0,b1));
  return best;
  }

// cm the point q is out of the trunk (< 0: in it) by its three nearest points and the normals there: the most out of
// them (one point's plane, far and grazing, would put q deep in where it is not); TrunkNear: far
float trunkGap(const std::vector<Quiver::Obstacle>& pts, const Vec3& q) {
  float d2[3] = {Quiver::TrunkNear*Quiver::TrunkNear,Quiver::TrunkNear*Quiver::TrunkNear,Quiver::TrunkNear*Quiver::TrunkNear};
  const Quiver::Obstacle* at[3] = {};
  for(auto& o:pts) {
    const Vec3 d = q-o.a;
    if(std::abs(d.y)>Quiver::TrunkNear)
      continue;
    const float l = Vec3::dotProduct(d,d);
    if(l>=d2[2])
      continue;
    int k = 2;
    while(k>0 && l<d2[k-1]) {
      d2[k] = d2[k-1];
      at[k] = at[k-1];
      --k;
      }
    d2[k] = l;
    at[k] = &o;
    }
  if(at[0]==nullptr)
    return Quiver::TrunkNear;
  // out of a point's plane, no nearer than that point less the points' spacing (its plane, far aside and grazing,
  // would bring the surface nearer than it is)
  constexpr float Spacing = 2.5f;
  float gap = -1e9f;
  for(int i=0; i<3; ++i)
    if(at[i]!=nullptr) {
      const float g = Vec3::dotProduct(q-at[i]->a,at[i]->n);
      gap = std::max(gap,g<0.f ? g : std::max(g,std::sqrt(d2[i])-Spacing));
      }
  return std::min(gap,Quiver::TrunkNear);
  }

// the lift's search (Quiver::step): the fletching no deeper than FletchOk cm in the trunk, the drawn arrow's line
// clear of it by LineRadius - LineOk cm; by halves of the turn, LiftWays ways round the first time, the last one and
// LiftNear (rad) each side after; again when the target is LiftSlack cm too deep or LiftLess deg lower clears; the aim
// held LiftHold s; no faster than
// LiftStep deg in 1/60 s
constexpr float FletchOk   = 0.75f;
constexpr float LineOk     = 1.f;
constexpr int   LiftHalves = 6;
constexpr int   LiftWays   = 8;
constexpr float LiftNear   = 0.2f;
constexpr float LiftHold   = 0.15f;
constexpr float LiftStep   = 6.f;
constexpr float LiftBracket = 4.f;  // deg
constexpr float LiftSlack   = 0.5f; // cm
constexpr float LiftLess    = 3.f;  // deg
constexpr float LiftLower   = 0.4f; // s
constexpr float LiftWayGain = 2.f;  // deg
constexpr float LiftBackNear = 1.f;  // deg
constexpr float LiftBackMost = 0.5f; // s
constexpr float LiftFollow  = 0.1f; // s: 90% of the way to the aim's target

Matrix4x4 swingOf(const Vec3& axis) {
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

Vec3 angleOf(const Vec3& u) {
  const float s = std::hypot(u.x,u.z);
  if(s<1e-6f)
    return {};
  const float l = std::atan2(s,-u.y);
  return {u.x/s*l,0,u.z/s*l};
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
bool          Quiver::measureGround = false;
bool          Quiver::liftOn = true;

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
  {
    Vec3 o = right;
    const auto& b = inv;
    f.outward = Vec3(b.at(0,0)*o.x+b.at(1,0)*o.y+b.at(2,0)*o.z,
                     b.at(0,1)*o.x+b.at(1,1)*o.y+b.at(2,1)*o.z,
                     b.at(0,2)*o.x+b.at(1,2)*o.y+b.at(2,2)*o.z);
  }
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

Matrix4x4 Quiver::frameNow(const Matrix4x4& pelvis, const Matrix4x4& spine) const {
  return restFrame(pelvis,spine)*swingMatrix();
  }

Matrix4x4 Quiver::reloadFrame() const {
  Matrix4x4 m = fitted.local;
  const Vec3 back = fitted.outward*(ReloadOut-fitted.out);
  m.set(3,0,m.at(3,0)+back.x);
  m.set(3,1,m.at(3,1)+back.y);
  m.set(3,2,m.at(3,2)+back.z);
  // turned onto the line the clips draw the arrow out by
  return m*swingOf(axisOfAngle(Vec3(ReloadTurnX,0,ReloadTurnZ)));
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

std::vector<Vec3> Quiver::surface(const Matrix4x4& pelvis, const Matrix4x4& spine, int rings, int around) const {
  std::vector<Vec3> ret;
  if(!st.on || !fitted.valid || rings<2 || around<1)
    return ret;
  const auto at = restFrame(pelvis,spine)*swingMatrix();
  ret.reserve(size_t(rings*around));
  for(int i=0; i<rings; ++i) {
    const float t = Length*float(i)/float(rings-1);
    const float r = radiusAt(t);
    for(int j=0; j<around; ++j) {
      const float a = 2.f*float(M_PI)*float(j)/float(around);
      Vec3 v = {r*std::cos(a),-t,r*std::sin(a)};
      at.project(v);
      ret.push_back(v);
      }
    }
  return ret;
  }

float Quiver::setTextureOwner(const Vec3& position, bool spatial) {
  float reach=body.setTextureOwner(position,spatial);
  for(auto& a:arrows)
    reach=std::min(reach,a.setTextureOwner(position,spatial));
  return reach;
  }

Quiver::Probe Quiver::probe(const Matrix4x4& pelvis, const Matrix4x4& spine, const std::vector<SkinPoint>& posed,
                            const std::vector<bool>& legBone, const std::vector<bool>& armBone,
                            const std::vector<Vec3>& bowPts, const std::vector<Vec3>& longPts,
                            const std::vector<bool>& leftBone, const std::vector<bool>& trunkBone) const {
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
    p.axis    = a;
    p.swing   = std::acos(std::clamp(-a.y,-1.f,1.f))*180.f/float(M_PI);
    p.back    = std::atan2(-a.z,-a.y)*180.f/float(M_PI);
    p.outward = std::atan2( a.x,-a.y)*180.f/float(M_PI);
    p.yield   = groundMode==0 ? std::atan2(devTarget.length(),MassAt)*180.f/float(M_PI)
                                  : std::hypot(groundTarget.x,groundTarget.z)*180.f/float(M_PI);
    // the rest's turn against SPINE1 about the vertical: the pelvis's less the half of the gait's
    p.twist = (twistOf(pelvis,spine)-yawOffset)*180.f/float(M_PI);
    p.ground   = groundMode;
    p.groundOn = groundSeen;
    if(measureGround && measured.on) {
      // the least turn (2 deg steps, 24 ways) that leaves the tube from 14 cm down no deeper than 2 cm under the
      // plane of the last step
      const Vec3 gn = measured.n;
      const float gd = measured.d;
      auto deepest = [&](const Vec3& u) {
        const float cu = Vec3::dotProduct(gn,u), side = std::sqrt(std::max(0.f,1.f-cu*cu));
        float most = 0;
        for(int k=0; k<8; ++k) {
          const float t = 2.f*RingContact + (Length-2.f*RingContact)*float(k)/7.f;
          most = std::max(most,gd - cu*t + radiusAt(t)*side);
          }
        return most;
        };
      p.needAngle = -1;
      for(int a=0; a<=60 && p.needAngle<0; ++a) {
        const float th = float(a)*2.f*float(M_PI)/180.f;
        float bestDepth = 1e9f;
        Vec3  bestU;
        for(int k=0; k<(a==0 ? 1 : 24); ++k) {
          const float ph = float(k)*2.f*float(M_PI)/24.f;
          const Vec3  u  = {std::sin(th)*std::cos(ph),-std::cos(th),std::sin(th)*std::sin(ph)};
          const float d  = deepest(u);
          if(d<bestDepth) {
            bestDepth = d;
            bestU     = u;
            }
          }
        if(bestDepth>2.f)
          continue;
        p.needAngle = float(a)*2.f;
        p.needBack  = std::atan2(-bestU.z,-bestU.y)*180.f/float(M_PI);
        p.needOut   = std::atan2( bestU.x,-bestU.y)*180.f/float(M_PI);
        for(auto& q:measured.body) {
          const Vec3  pt = {q.x,q.y,q.z};
          const float t  = Vec3::dotProduct(pt,bestU);
          if(t<0.f)
            continue;
          const float tc = std::min(t,Length);
          p.needBody = std::max(p.needBody,radiusAt(tc)+q.w-(pt-bestU*tc).length());
          }
        }
      }
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
    if(pt.bone<trunkBone.size() && trunkBone[pt.bone])
      p.trunk = std::max(p.trunk,depth);
    if(pt.bone<armBone.size() && armBone[pt.bone])
      p.arm = std::max(p.arm,depth); else
    if(pt.bone<legBone.size() && legBone[pt.bone] && pt.bone<leftBone.size() && leftBone[pt.bone])
      p.legLeft = std::max(p.legLeft,depth);
    if(pt.bone<legBone.size() && legBone[pt.bone]) {
      if(depth>p.leg) {
        Vec3 q = pt.pos;
        inv.project(q);
        p.legAt = -q.y;
        }
      p.leg = std::max(p.leg,depth);
      } else
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
  p.fletch = fletchNow;
  p.line   = lineNow;
  p.drawOut = outNow;
  p.lift   = groundMode==3 ? std::hypot(liftTarget.x,liftTarget.z)*180.f/float(M_PI) : 0.f;
  // the drawing hand, its forearm and the arrow against the trunk's points of the last step (the bench's)
  if(liftSeen.hand && !trunkSeen.empty()) {
    auto gapOf = [&](Vec3 w) { seenInv.project(w); return trunkGap(trunkSeen,w); };
    auto along = [&](const Vec3& w0, const Vec3& w1, float step) {
      const int n = std::clamp(int((w1-w0).length()/step),1,40);
      float g = 99.f;
      for(int i=0; i<=n; ++i)
        g = std::min(g,gapOf(w0+(w1-w0)*(float(i)/float(n))));
      return g;
      };
    p.trunkHand = gapOf(liftSeen.handAt);
    p.trunkFore = along(liftSeen.foreAt,liftSeen.handAt,2.f);
    if(liftSeen.arrow)
      p.trunkArrow = along(liftSeen.nock,liftSeen.tip,3.f);
    }
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
  constexpr size_t MaxLeg    = 80, MaxOther = 80;
  auto inv = sk.tr[f.pelvis]*f.local;
  inv.inverse();
  const auto arm  = armBones(sk);
  const auto legs = legBones(sk);
  std::vector<std::pair<float,size_t>> near, nearLeg;
  // the right leg whole (the foot kicks up behind to the bottom in a run): its side of the body
  for(size_t i=0; i<bind.size(); ++i) {
    if(bind[i].src==size_t(-1) || (bind[i].bone<arm.size() && arm[bind[i].bone]))
      continue;
    Vec3 q = bind[i].pos;
    inv.project(q);
    const float gap = std::hypot(q.x,q.z)-radiusAt(std::clamp(-q.y,0.f,Length));
    const bool  isLeg = bind[i].bone<legs.size() && legs[bind[i].bone] &&
                        sk.nodes[bind[i].bone].name.find(" R ")!=std::string::npos;
    if(isLeg) {
      nearLeg.emplace_back(gap,i);
      continue;
      }
    if(q.y>5.f || q.y<-Length-Reach || gap>Reach)
      continue;
    near.emplace_back(gap,i);
    }
  // the nearest of each, so many: the leg's own share (the flaps must not crowd it out)
  for(auto* list:{&nearLeg,&near}) {
    std::sort(list->begin(),list->end());
    const size_t most = list==&nearLeg ? MaxLeg : MaxOther;
    if(list->size()>most) {
      // every n-th: of the whole right leg, of the nearest 3x as many of the rest
      std::vector<std::pair<float,size_t>> thin;
      const size_t pool = list==&nearLeg ? list->size() : std::min(list->size(),most*3);
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

void Quiver::widePoints(const Fit& f, const Skeleton& sk, const std::vector<SkinPoint>& bind,
                        const std::vector<size_t>& near, std::vector<size_t>& ids) {
  ids.clear();
  if(!f.valid || f.pelvis>=sk.tr.size())
    return;
  // the tube turned up to GroundAngle round the ring sweeps a cone; a point of the armour within its reach (the
  // back, the buttock, the thigh, the other leg) may stop it: not the arms (their capsules), not the near ones
  constexpr size_t MaxWide = 120;
  const float reach = Length*std::sin(GroundAngle*float(M_PI)/180.f) + MouthRadius + 4.f;
  auto inv = sk.tr[f.pelvis]*f.local;
  inv.inverse();
  const auto arm = armBones(sk);
  std::vector<size_t> skip = near;
  std::sort(skip.begin(),skip.end());
  std::vector<std::pair<float,size_t>> list;
  for(size_t i=0; i<bind.size(); ++i) {
    if(bind[i].src==size_t(-1) || (bind[i].bone<arm.size() && arm[bind[i].bone]))
      continue;
    if(std::binary_search(skip.begin(),skip.end(),bind[i].src))
      continue;
    Vec3 q = bind[i].pos;
    inv.project(q);
    if(q.y>MouthRadius || q.y<-Length-MouthRadius || q.length()>Length+MouthRadius+4.f)
      continue;
    const float across = std::hypot(q.x,q.z);
    if(across>reach)
      continue;
    list.emplace_back(across,i);
    }
  std::sort(list.begin(),list.end());
  if(list.size()>MaxWide) {
    std::vector<std::pair<float,size_t>> thin;
    for(size_t k=0; k<MaxWide; ++k)
      thin.push_back(list[k*list.size()/MaxWide]);
    list = std::move(thin);
    }
  for(auto& n:list)
    ids.push_back(bind[n.second].src);
  }

void Quiver::abovePoints(const Fit& f, const Skeleton& sk, const std::vector<SkinPoint>& bind,
                         std::vector<size_t>& ids) {
  ids.clear();
  if(!f.valid || f.pelvis>=sk.tr.size())
    return;
  // over the mouth, within the lift's reach (the drawn arrow's line turned to LiftAngle and more: the aim turns the
  // chest against the pelvis); the arms and the legs are not the trunk (the clip and the give keep to them)
  constexpr size_t MaxAbove = 360;
  auto inv = sk.tr[f.pelvis]*f.local;
  inv.inverse();
  const auto limbs = bonesNamed(sk,{"UPPERARM","FOREARM","HAND","FINGER","THIGH","CALF","FOOT","TOE","ZS_"});
  std::vector<std::pair<float,size_t>> list;
  for(size_t i=0; i<bind.size(); ++i) {
    if(bind[i].src==size_t(-1) || (bind[i].bone<limbs.size() && limbs[bind[i].bone]))
      continue;
    Vec3 q = bind[i].pos;
    inv.project(q);
    if(q.y<-5.f || q.length()>ReloadLine+TrunkNear || std::hypot(q.x,q.z)>60.f)
      continue;
    list.emplace_back(q.y,i);
    }
  std::sort(list.begin(),list.end());
  if(list.size()>MaxAbove) {
    std::vector<std::pair<float,size_t>> thin;
    for(size_t k=0; k<MaxAbove; ++k)
      thin.push_back(list[k*list.size()/MaxAbove]);
    list = std::move(thin);
    }
  for(auto& n:list)
    ids.push_back(bind[n.second].src);
  }

void Quiver::rest() {
  axis      = {0,-1,0};
  moving    = false;
  liftBack  = false;
  drawing   = false;
  devV      = Vec3();
  devTarget = Vec3();
  yawOffset = 0;
  groundMode = 0;
  lastInvOn  = false;
  gw = gwV = groundTarget = Vec3();
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

void Quiver::step(const Matrix4x4& pelvis, const Matrix4x4& spine, const std::vector<Obstacle>& obstacles, float dt,
                  const Ground& ground, const Lift& lf) {
  if(!st.on || !fitted.valid)
    return;
  if(still) {
    rest();
    return;
    }
  // no time: as it was (a forced pose update)
  if(dt<=0.f)
    return;
  groundSeen = ground.on;
  lastPelvis = pelvis;
  // the gait's turn of the pelvis against SPINE1: what the slow follower does not catch (a stance's twist,
  // the bow drawn, is caught: the quiver stays on the pelvis then)
  twistNow = twistOf(pelvis,spine);
  if(!moving)
    twistSlow = twistNow;
  twistSlow += (twistNow-twistSlow)*(1.f-std::exp(-dt/TwistSlow));
  yawOffset  = 0.5f*(twistNow-twistSlow);
  const auto frame = restFrame(pelvis,spine);  // the quiver at rest, in the world
  const Vec3 pivot = translation(frame);

  // the give: one turn round the ring (two axes, up to GiveAngle) that leaves the least sum of the squares
  // of how deep goes what is in the tube - the leg, the forearm and hand, the flaps, the bow and a two-hander on the back.
  // The ground is one more of them; while it is still in the tube at GiveAngle the turn may go to GroundAngle, and
  // then the armour's points the wide turn reaches (Body) count too
  auto inv = frame;
  inv.inverse();
  const float reachOut  = Length*std::tan(GiveAngle*float(M_PI)/180.f) + 2.f;
  const float reachWide = Length*std::sin(GroundAngle*float(M_PI)/180.f) + 2.f;
  static thread_local std::vector<Tempest::Vec4> near, nearWide; // in the rest frame: the centre, the radius
  near.clear();
  nearWide.clear();
  auto take = [&](Vec3 p, float r, bool body) {
    inv.project(p);
    if(p.y>r || p.y<-Length-reachOut-r)
      ;
    else if(!body && std::hypot(p.x,p.z)-r<=MouthRadius+reachOut)
      near.emplace_back(p.x,p.y,p.z,r);
    if(ground.on && p.y<=r && p.y>=-Length-r && std::hypot(p.x,p.z)-r<=MouthRadius+reachWide)
      nearWide.emplace_back(p.x,p.y,p.z,r);
    };
  // the lift's: the trunk's points with their normals and the head's capsule over the mouth (the rest frame)
  static thread_local std::vector<Obstacle> above, trunkPts;
  static thread_local std::vector<Vec3>     trunkRaw; // all of them, in the armour's order: the lift's reference
  above.clear();
  trunkPts.clear();
  trunkRaw.clear();
  trunkSeen.clear();
  liftSeen = lf;
  seenInv  = inv;
  const float coneOut = std::tan((LiftAngle+15.f)*float(M_PI)/180.f);
  for(auto& o:obstacles) {
    if(o.kind==Trunk) {
      Obstacle q = o;
      inv.project(q.a);
      q.n = Vec3::normalize(Vec3(Vec3::dotProduct(o.n,Vec3(inv.at(0,0),inv.at(1,0),inv.at(2,0))),
                                 Vec3::dotProduct(o.n,Vec3(inv.at(0,1),inv.at(1,1),inv.at(2,1))),
                                 Vec3::dotProduct(o.n,Vec3(inv.at(0,2),inv.at(1,2),inv.at(2,2)))));
      trunkRaw.push_back(q.a);
      // within the reach of the lines the lift may take (and the bench's hand far from them: not needed)
      if(q.a.y>-TrunkNear && q.a.length()<ReloadLine+TrunkNear &&
         std::hypot(q.a.x,q.a.z)<std::max(q.a.y,0.f)*coneOut+TrunkNear+MouthRadius)
        trunkPts.push_back(q);
      if(measureGround && lf.hand)
        trunkSeen.push_back(q);
      continue;
      }
    if(o.kind==Head) {
      Obstacle q = o;
      inv.project(q.a);
      inv.project(q.b);
      above.push_back(q);
      continue;
      }
    if(o.kind==Body && !ground.on)
      continue;
    const float len = (o.b-o.a).length();
    const int   n   = len<1e-3f ? 1 : std::clamp(int(std::ceil(len/std::max(o.r,3.f))),2,12);
    for(int i=0; i<n; ++i)
      take(n==1 ? o.a : o.a+(o.b-o.a)*(float(i)/float(n-1)),o.r,o.kind==Body);
    }
  // how much of them is in the tube for the axis u (the rest frame, the bottom's direction): the sum of the
  // squares of how deep each goes, so each contact down the tube goes out as far as the turn allows
  auto insideOf = [&](const std::vector<Tempest::Vec4>& list, const Vec3& u) {
    float sum = 0;
    for(auto& q:list) {
      const Vec3  p = {q.x,q.y,q.z};
      const float t = Vec3::dotProduct(p,u);
      if(t<0.f)
        continue; // over the mouth
      const float tc   = std::min(t,Length);
      const float d    = radiusAt(tc)+q.w-(p-u*tc).length();
      // a contact at the ring cannot be turned away (research.md: 17-25 deg for 2-3 cm): it does not count, or
      // it would hold the quiver turned to the limit and slow it to the contacts down the tube
      const float w    = std::clamp((tc-RingContact)/RingContact,0.f,1.f);
      if(d>0.f)
        sum += w*d*d;
      }
    return sum;
    };
  // the ground's plane in the rest frame; how deep the tube goes under it along its sections (the ring's own
  // part does not count, as above)
  // GroundAhead: the same plane as the last rest frame saw it, carried on as far: the tube starts out of the
  // ground's way before it is there (a roll brings it 50 deg in 3 frames, GroundStep allows 30)
  Vec3  gn, gnAhead;
  float gd = 0, gdAhead = 0;
  if(ground.on) {
    auto planeIn = [&](const Matrix4x4& m, Vec3& n, float& d) {
      Vec3 a = ground.at, b = ground.at+ground.n;
      m.project(a);
      m.project(b);
      n = Vec3::normalize(b-a);
      d = Vec3::dotProduct(n,a);
      };
    planeIn(inv,gn,gd);
    gnAhead = gn;
    gdAhead = gd;
    if(lastInvOn && moving && dt<0.1f) {
      Vec3  gnLast;
      float gdLast = 0;
      planeIn(lastInv,gnLast,gdLast);
      const float k = GroundAhead/dt;
      gnAhead = Vec3::normalize(gn + (gn-gnLast)*k);
      gdAhead = gd + (gd-gdLast)*k;
      }
    }
  lastInv   = inv;
  lastInvOn = ground.on;
  // lift: the plane raised so much (the hysteresis of the push)
  auto underground = [&](const Vec3& u, float lift) {
    if(!ground.on)
      return 0.f;
    const float cu   = Vec3::dotProduct(gn,u), ca = Vec3::dotProduct(gnAhead,u);
    const float side = std::sqrt(std::max(0.f,1.f-cu*cu)), sa = std::sqrt(std::max(0.f,1.f-ca*ca));
    float sum = 0;
    for(int k=0; k<8; ++k) {
      const float t = RingContact + (Length-RingContact)*float(k)/7.f;
      const float d = std::max(gd - cu*t + radiusAt(t)*side, gdAhead - ca*t + radiusAt(t)*sa) + lift;
      const float w = std::clamp((t-RingContact)/RingContact,0.f,1.f);
      if(d>0.f)
        sum += w*d*d;
      }
    return sum;
    };
  auto inside = [&](const Vec3& u) { return insideOf(near,u) + GroundWeight*underground(u,0.f); };
  // the give's axis by its tangents across (x, z); no farther than GiveAngle
  const float maxTan = std::tan(GiveAngle*float(M_PI)/180.f);
  auto clampTan = [&](float& x, float& z) {
    const float l = std::hypot(x,z);
    if(l>maxTan) {
      x *= maxTan/l;
      z *= maxTan/l;
      }
    };
  float lastX = devTarget.x/MassAt, lastZ = devTarget.z/MassAt;
  clampTan(lastX,lastZ);
  // a small price on the turn and on the change keeps it from wandering between equal answers
  auto score = [&](float x, float z) {
    const Vec3 u = Vec3::normalize(Vec3(x,-1.f,z));
    return inside(u) + 2.f*std::hypot(x,z) + 2.f*std::hypot(x-lastX,z-lastZ);
    };
  // from the last answer or the rest, whichever is better, down the gradient with a step that halves when it
  // does not help
  float bx = 0, bz = 0;
  // nothing in the tube at rest (most frames): the rest, no search
  if(inside(Vec3(0,-1,0))>0.f) {
    float s0 = score(0,0), sl = score(lastX,lastZ);
    float x = 0, z = 0, sc = s0;
    if(sl<s0) {
      x  = lastX;
      z  = lastZ;
      sc = sl;
      }
    float step = maxTan*0.5f;
    constexpr float h = 0.004f;
    for(int it=0; it<6 && step>maxTan*0.02f; ++it) {
      const float gx = (score(x+h,z)-sc)/h, gz = (score(x,z+h)-sc)/h;
      const float gl = std::hypot(gx,gz);
      if(gl<1e-6f)
        break;
      float nx = x - gx/gl*step, nz = z - gz/gl*step;
      clampTan(nx,nz);
      const float sn = score(nx,nz);
      if(sn<sc) {
        x  = nx;
        z  = nz;
        sc = sn;
        step *= 1.2f;
        } else {
        step *= 0.5f;
        }
      }
    bx = x;
    bz = z;
    }
  const Vec3 best = Vec3::normalize(Vec3(bx,-1.f,bz));
  Vec3 dTarget = {best.x/-best.y*MassAt,0,best.z/-best.y*MassAt};
  devTarget = dTarget;

  // the lift: how deep the trunk and the head go into the line over the mouth for the axis u (the bottom's way): the
  // fletching (len cm, the nocks' spread) or the drawn arrow's line; the deepest, cm. The bench's qv_fletch, qv_line
  auto deepestIn = [&](const std::vector<Obstacle>& pts, const Vec3& u, float len, float r) {
    // from RingContact up: what touches at the ring the turn cannot take away (as the give's)
    const Vec3 a = u*-RingContact, b = u*-len;
    float most = 0;
    for(auto& o:above)
      most = std::max(most,o.r + r - segSeg(a,b,o.a,o.b));
    if(!pts.empty()) {
      const int n = std::clamp(int((len-RingContact)/(len>Fletching+1.f ? 5.f : 3.f)),2,32);
      for(int i=0; i<=n; ++i)
        most = std::max(most,r - trunkGap(pts,a+(b-a)*(float(i)/float(n))));
      }
    return most;
    };
  auto deepest = [&](const Vec3& u, float len, float r) { return deepestIn(trunkPts,u,len,r); };
  // the trunk's points that can matter for the turns toward phi: the axes lie in the plane of the rest axis and that
  // way, and a point farther than TrunkNear from them is never the nearest (trunkGap)
  static thread_local std::vector<Obstacle> slab;
  auto slabOf = [&](float phi) -> const std::vector<Obstacle>& {
    const Vec3 nrm = {-std::sin(phi),0,std::cos(phi)};
    slab.clear();
    for(auto& o:trunkPts)
      if(std::abs(Vec3::dotProduct(o.a,nrm))<=TrunkNear)
        slab.push_back(o);
    return slab;
    };
  // the least turn toward phi (rad round the axis, the rest frame) that leaves the line no deeper than ok: by halves
  // of [0, LiftAngle]; the turn (rad) and how deep it is there
  const float liftMost = LiftAngle*float(M_PI)/180.f;
  auto leastTurn = [&](float phi, float len, float r, float ok, float* deep, float near) {
    const auto& pts = slabOf(phi);
    auto at = [&](float m) { return deepestIn(pts,axisOfAngle(Vec3(std::cos(phi)*m,0,std::sin(phi)*m)),len,r); };
    if(near>=0.f) {
      // from the last answer: a bracket of LiftBracket each side of it, by halves (the pose moved a little)
      const float br = LiftBracket*float(M_PI)/180.f;
      float lo = std::max(0.f,near-br), hi = std::min(liftMost,near+br);
      float dLo = lo>0.f ? at(lo) : at(0), dHi = at(hi);
      if(dLo<=ok) {
        if(lo<=0.f) {
          *deep = dLo;
          return 0.f;
          }
        near = -1; // went down farther than the bracket: the whole way
        } else if(dHi>ok && hi<liftMost) {
        near = -1; // up farther
        } else {
        for(int i=0; i<LiftHalves-1; ++i) {
          const float mid = 0.5f*(lo+hi), d = at(mid);
          if(d>ok)
            lo = mid; else {
            hi  = mid;
            dHi = d;
            }
          }
        *deep = dHi;
        return hi;
        }
      }
    float d0 = at(0);
    if(d0<=ok) {
      *deep = d0;
      return 0.f;
      }
    float dHi = at(liftMost);
    if(dHi>ok) {
      *deep = dHi;
      return liftMost;
      }
    float lo = 0, hi = liftMost;
    for(int i=0; i<LiftHalves; ++i) {
      const float mid = 0.5f*(lo+hi), d = at(mid);
      if(d>ok)
        lo = mid; else {
        hi  = mid;
        dHi = d;
        }
      }
    *deep = dHi;
    return hi;
    };
  // the way round the axis that needs the least turn: the last one and its neighbours (a ring of them the first time)
  // (near: the last turn this way, < 0: none; side: which neighbour to try besides it: 1, -1, 0 both, 2 none)
  auto searchWay = [&](float& phi, bool first, float len, float r, float ok, float near, int side) {
    float bestM = 1e9f, bestD = 1e9f, bestPhi = phi;
    // a neighbour way takes over only LiftWayGain lower: on the way between two ways the tube may go through the trunk
    const float gain = first ? 1e-4f : LiftWayGain*float(M_PI)/180.f;
    auto tryWay = [&](float f) {
      float d = 0;
      const float m = leastTurn(f,len,r,ok,&d,f==phi ? near : -1.f);
      if(m<bestM-(bestM<1e8f ? gain : 0.f) || (m>=liftMost-1e-4f && bestM>=liftMost-1e-4f && d<bestD)) {
        bestM   = m;
        bestD   = d;
        bestPhi = f;
        }
      };
    if(first) {
      // coarse: how deep at half and the whole of LiftAngle each way
      float least = 1e9f;
      const float f0 = phi;
      for(int k=0; k<LiftWays; ++k) {
        const float f = f0+float(k)*2.f*float(M_PI)/float(LiftWays);
        const auto& pts = slabOf(f);
        auto at = [&](float m) { return deepestIn(pts,axisOfAngle(Vec3(std::cos(f)*m,0,std::sin(f)*m)),len,r); };
        const float d = at(liftMost*0.5f)+at(liftMost);
        if(d<least) {
          least = d;
          phi   = f;
          }
        }
      }
    if(first)
      near = -1;
    tryWay(phi);
    if(side==0 || side==1)
      tryWay(phi+LiftNear);
    if(side==0 || side==-1)
      tryWay(phi-LiftNear);
    phi = bestPhi;
    return Vec3(std::cos(phi)*bestM,0,std::sin(phi)*bestM);
    };
  // the aim: the bow up (S_BOWAIM, S_BOWSHOOT, the reload), held LiftHold s over a gap between them
  liftHold = lf.aim ? LiftHold : std::max(0.f,liftHold-dt);
  const bool trunkOn = !above.empty() || !trunkPts.empty();
  // (over the gap the trunk is not passed: the last search holds)
  const bool aimNow  = liftOn && ((lf.aim && trunkOn) || (liftHold>0.f && liftValid));
  const bool drawn   = aimNow && lf.out>=0.f;
  outNow = drawn ? lf.out : -1.f;
  fletchNow = lineNow = 0;
  bool lifting = false;
  if(aimNow) {
    lifting = true;
    // the last target against the trunk as posed now: deeper than FletchOk + LiftSlack, or clear LiftLess deg lower:
    // search again
    bool moved = !liftValid && !trunkPts.empty();
    if(liftValid && !trunkPts.empty()) {
      const float m   = std::hypot(liftTarget.x,liftTarget.z);
      const float phi = liftPhi;
      auto at = [&](float k) { return deepest(axisOfAngle(Vec3(std::cos(phi)*k,0,std::sin(phi)*k)),Fletching,FletchRadius); };
      // (lower: only when it has stayed clear LiftLower s; liftRef[0].x keeps how long, s)
      float& clearFor = liftRef[0].x;
      if(at(m)>FletchOk+LiftSlack) {
        moved    = true;
        clearFor = 0;
        } else if(m>0.f && at(std::max(0.f,m-LiftLess*float(M_PI)/180.f))<=FletchOk-LiftSlack) {
        clearFor += dt;
        moved = clearFor>=LiftLower;
        if(moved)
          clearFor = 0;
        } else {
        clearFor = 0;
        }
      }
    if(moved) {
      // the fletching: the least turn that leaves it no deeper than FletchOk; the draw's line next frame
      const bool first = !liftValid;
      if(first)
        liftPhi = std::atan2(ReloadTurnZ,ReloadTurnX);
      // after the first, one neighbour way a time, by turns
      static thread_local int side = 1;
      side = -side;
      const float last = first ? -1.f : std::hypot(liftTarget.x,liftTarget.z);
      liftTarget = searchWay(liftPhi,first,Fletching,FletchRadius,FletchOk,last,first ? 0 : side);
      liftValid = true;
      }
    // the drawn arrow's line: clear of the trunk and the head by LineRadius - LineOk; once a draw, from the aim's way
    if(drawn && !lineValid && !trunkPts.empty()) {
      linePhi    = liftPhi;
      lineTarget = searchWay(linePhi,false,ReloadLine,LineRadius,LineOk,std::hypot(liftTarget.x,liftTarget.z),0);
      lineValid  = true;
      } else if(drawn && !trunkPts.empty() && deepest(axisOfAngle(lineTarget),ReloadLine,LineRadius)>LineOk+LiftSlack) {
      // the clip moves the head and the shoulder meanwhile: from the last line, one neighbour way a time
      static thread_local int lineSide = 1;
      lineSide   = -lineSide;
      lineTarget = searchWay(linePhi,false,ReloadLine,LineRadius,LineOk,std::hypot(lineTarget.x,lineTarget.z),lineSide);
      }
    if(!drawn)
      lineValid = false;
    if(measureGround) {
      fletchNow = deepest(Vec3::normalize(axis),Fletching,FletchRadius);
      if(drawn)
        lineNow = deepest(Vec3::normalize(axis),ReloadLine,LineRadius);
      }
    } else {
    liftValid = lineValid = false;
    liftRef[0].x = 0;
    if(measureGround && trunkOn)
      fletchNow = deepest(Vec3::normalize(axis),Fletching,FletchRadius);
    }
  if(!lifting || !drawn)
    drawing = false;

  // the ground still in the tube at GiveAngle: the wide turn, to GroundAngle, by the turn's own angle across
  // (x, z: radians toward that way). The ground outweighs the body far (GroundWeight against GroundBody): the quiver
  // goes rather where the body lies on the ground, out of sight, than stands in the open ground
  const float lift  = groundMode==1 ? GroundClear : 0.f; // pushed: stays so till the tube is that clear
  const float wideA = GroundAngle*float(M_PI)/180.f;
  Vec3 gTarget = angleOf(best);
  bool wide    = false;
  if(ground.on && underground(best,lift)>0.04f) {
    wide = true;
    const Vec3 last = groundMode!=0 ? groundTarget : gTarget;
    // its weight: the height of the tube's middle over the ground (its potential energy), so the quiver the ground
    // pushes falls and lies on it rather than standing up; the body's points keep it from lying across the body
    auto score2 = [&](float x, float z) {
      const Vec3  u      = axisOfAngle(Vec3(x,0,z));
      const float height = Vec3::dotProduct(gn,u)*(Length*0.5f) - gd;
      return GroundBody*insideOf(nearWide,u) + GroundWeight*underground(u,0.f) + GroundGravity*height +
             0.2f*std::hypot(x,z) + 2.f*std::hypot(x-last.x,z-last.z);
      };
    auto clampA = [&](float& x, float& z) {
      const float l = std::hypot(x,z);
      if(l>wideA) {
        x *= wideA/l;
        z *= wideA/l;
        }
      };
    float x = gTarget.x, z = gTarget.z, sc = score2(x,z);
    auto seed = [&](float sx, float sz) {
      clampA(sx,sz);
      if(const float sn = score2(sx,sz); sn<sc) {
        x  = sx;
        z  = sz;
        sc = sn;
        }
      };
    seed(last.x,last.z);
    // the way out may be on the other side of the body from where it is (a roll turns the ground round it):
    // a coarse ring of starts, 8 ways at a third, two thirds and the whole of GroundAngle
    for(int k=0; k<8; ++k) {
      const float ph = float(k)*float(M_PI)/4.f;
      for(int m=1; m<=3; ++m)
        seed(std::cos(ph)*wideA*float(m)/3.f,std::sin(ph)*wideA*float(m)/3.f);
      }
    float step = wideA*0.25f;
    constexpr float h = 0.004f;
    for(int it=0; it<16 && step>wideA*0.005f; ++it) {
      const float gx = (score2(x+h,z)-sc)/h, gz = (score2(x,z+h)-sc)/h;
      const float gl = std::hypot(gx,gz);
      if(gl<1e-6f)
        break;
      float nx = x - gx/gl*step, nz = z - gz/gl*step;
      clampA(nx,nz);
      const float sn = score2(nx,nz);
      if(sn<sc) {
        x  = nx;
        z  = nz;
        sc = sn;
        step *= 1.2f;
        } else {
        step *= 0.5f;
        }
      }
    gTarget = Vec3(x,0,z);
    }
  groundTarget = gTarget;

  // the ground pushes it (1) or it comes back from the ground (2), faster and farther than the give (0)
  const Vec3  now      = Vec3::normalize(axis);
  const float nowAngle = std::acos(std::clamp(-now.y,-1.f,1.f))*180.f/float(M_PI);
  const bool  pushed   = ground.on && (wide || underground(Vec3(0,-1,0),lift)>0.f || underground(now,lift)>0.f);
  if(groundMode==3 && !lifting) {
    liftBack = true;
    liftRef[1].x = 0; // how long it has been coming back, s
    }
  if(liftBack)
    liftRef[1].x += dt;
  // back within LiftBackNear deg of the give's target, or after LiftBackMost s (walking, the target moves on)
  if(liftBack && !lifting && !pushed &&
     ((gw-gTarget).length()<LiftBackNear*float(M_PI)/180.f || liftRef[1].x>=LiftBackMost)) {
    // back: where the give has it, no speed left (nothing of the aim carried on)
    gw  = gTarget;
    gwV = Vec3();
    liftBack = false;
    } else if(liftBack && (lifting || pushed)) {
    liftBack = false;
    }
  const int   mode     = pushed ? 1 : (lifting ? 3 : (liftBack || nowAngle>GiveAngle+0.05f ? 2 : 0));

  // a jump (a teleport, a load) or the first step: at the target
  if(!moving || (pivot-lastPivot).length()>100.f || dt>0.25f) {
    dev       = dTarget;
    devV      = Vec3();
    gw        = mode==3 ? liftTarget : gTarget;
    gwV       = Vec3();
    lastPivot = pivot;
    moving    = true;
    axis      = mode==0 ? Vec3::normalize(Vec3(dev.x,-MassAt,dev.z)) : axisOfAngle(gw);
    } else if(mode==0) {
    lastPivot = pivot;
    if(groundMode!=0) {
      // back within the give: its follower takes over from where the ground left it
      const Vec3 u = Vec3::normalize(axis);
      dev  = Vec3(u.x/-u.y*MassAt,0,u.z/-u.y*MassAt);
      devV = gwV*MassAt;
      }
    // critically damped to the target, no overshoot (90% in GiveTime), no faster than GiveStep in 1/60 s
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
    axis = Vec3::normalize(Vec3(dev.x,-MassAt,dev.z));
    } else {
    lastPivot = pivot;
    if(groundMode==0) {
      // from the give: the same axis, its speed
      gw  = angleOf(Vec3::normalize(axis));
      gwV = devV/MassAt;
      }
    const Vec3 from = gw;
    if(mode==1) {
      // the ground pushes it: straight to the target
      gw  = gTarget;
      gwV = Vec3();
      } else if(mode==3 && drawn) {
      // the arrow drawn out takes the tube with it: onto the line in the first LiftRamp cm of the draw
      if(!drawing) {
        drawing  = true;
        drawFrom = gw;
        }
      const float k = std::clamp(lf.out/LiftRamp,0.f,1.f);
      gw  = drawFrom + (lineTarget-drawFrom)*(k*k*(3.f-2.f*k));
      gwV = (gw-from)/dt;
      } else {
      const Vec3 target = mode==3 ? liftTarget : gTarget;
      // back: critically damped, no overshoot, 90% in GroundReturn; after the chest in the aim, in LiftFollow
      const float w = 3.89f/(mode==3 ? LiftFollow : GroundReturn);
      const int   n = std::max(1,int(std::ceil(dt*240.f)));
      const float h = dt/float(n);
      for(int i=0; i<n; ++i) {
        Vec3 acc = (target-gw)*(w*w) - gwV*(2.f*w);
        acc.y = 0;
        gwV += acc*h;
        gw  += gwV*h;
        gw.y = 0;
        }
      }
    // no faster than GroundStep in 1/60 s, as the angle the axis turns
    const Vec3  moved = gw-from;
    const float most  = (mode==1 ? GroundStep : std::min(GroundStep,LiftStep))*dt*60.f*float(M_PI)/180.f;
    const Vec3  a0    = axisOfAngle(from);
    auto turn = [&](float f) {
      return std::acos(std::clamp(Vec3::dotProduct(a0,axisOfAngle(from+moved*f)),-1.f,1.f));
      };
    if(turn(1.f)>most) {
      float lo = 0, hi = 1;
      for(int i=0; i<12; ++i) {
        const float mid = 0.5f*(lo+hi);
        if(turn(mid)>most)
          hi = mid; else
          lo = mid;
        }
      gw  = from + moved*lo;
      gwV = mode==1 ? Vec3() : (gw-from)/dt;
      }
    axis = axisOfAngle(gw);
    }
  groundMode = mode;

  if(measureGround) {
    measured.on    = ground.on;
    measured.n     = gn;
    measured.d     = gd;
    measured.body.assign(nearWide.begin(),nearWide.end());
    }
  }

float Quiver::lowest(const Matrix4x4& pelvis, const Matrix4x4& spine, Vec3* midOut, Vec3* bottomOut) const {
  const auto frame = restFrame(pelvis,spine);
  const auto at    = frame*swingMatrix();
  Vec3 ring = {0,0,0}, bottom = {0,-Length,0}, restBottom = {0,-Length,0}, mid = {0,-Length*0.5f,0};
  frame.project(ring);
  at.project(bottom);
  at.project(mid);
  frame.project(restBottom);
  if(midOut!=nullptr)
    *midOut = mid;
  if(bottomOut!=nullptr)
    *bottomOut = bottom;
  return std::min({ring.y,bottom.y,restBottom.y}) - MouthRadius;
  }

bool Quiver::restsOnGround(const Matrix4x4& pelvis) const {
  if(groundMode!=1 || !moving || (gw-groundTarget).length()>1e-4f)
    return false;
  for(int c=0; c<4; ++c)
    for(int r=0; r<4; ++r)
      if(std::abs(pelvis.at(c,r)-lastPelvis.at(c,r))>(c==3 ? 0.05f : 1e-4f))
        return false;
  return true;
  }
