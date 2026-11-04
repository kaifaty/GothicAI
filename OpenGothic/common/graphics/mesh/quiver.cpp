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
  const Vec3 pelvis = translation(sk.tr[f.pelvis]);
  const Vec3 mouth0 = Vec3(pelvis.x,floor+MouthHeight,pelvis.z) - fwd*MouthBack;

  const float tb = std::tan(TiltBack*float(M_PI)/180.f), to = std::tan(TiltOut*float(M_PI)/180.f);
  const Vec3  down = Vec3::normalize(up*-1.f - fwd*tb + right*to);
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

  s = std::max(s,MinOut);
  const Vec3 mouth = mouth0 + right*s;
  const Vec3 y     = down*-1.f;
  const Vec3 z     = Vec3::normalize(fwd - y*Vec3::dotProduct(fwd,y));
  const Vec3 x     = Vec3::crossProduct(y,z);
  auto inv = sk.tr[f.pelvis];
  inv.inverse();
  f.local = inv*basis(x,y,z,mouth);
  f.out   = s;
  f.valid = true;
  return f;
  }

std::vector<Matrix4x4> Quiver::arrowLayout(const Vec3* box, int count) {
  std::vector<Matrix4x4> ret;
  if(box==nullptr || count<=0)
    return ret;
  const float len  = box[1].x-box[0].x;
  const float cy   = (box[0].y+box[1].y)*0.5f, cz = (box[0].z+box[1].z)*0.5f;
  // the arrows stand on the bottom; a long one shows no more than FletchingMax
  const float base = std::clamp(len-(Length-1.f),0.f,FletchingMax);
  const float rin  = MouthRadius-Wall-Shaft;
  for(int i=0; i<count; ++i) {
    // a sunflower: spread evenly over the mouth, a lone one leans aside
    const float r     = count==1 ? rin*0.6f : rin*std::sqrt((float(i)+0.5f)/float(count));
    const float an    = float(i)*2.39996f;
    const float lift  = std::fmod(float(i)*0.37f,1.f)*2.f;   // 0..2 cm: not all fletchings level
    const float above = std::min(base+lift,FletchingMax);
    Matrix4x4 m;
    m.identity();
    m.translate(r*std::cos(an),above,r*std::sin(an));
    m.rotateOY(float(i)*67.f);                                // each turned its own way round its shaft
    m.mul(basis({0,1,0},{-1,0,0},{0,0,1},{0,0,0}));          // the arrow's +X (the nock) up
    m.translate(-box[1].x,-cy,-cz);
    ret.push_back(m);
    }
  return ret;
  }

void Quiver::set(const State& s, const Fit& f, MeshObjects::Mesh&& b, std::vector<MeshObjects::Mesh>&& a,
                 std::vector<Matrix4x4>&& al) {
  st         = s;
  fitted     = f;
  body       = std::move(b);
  arrows     = std::move(a);
  arrowLocal = std::move(al);
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

void Quiver::sync(const Matrix4x4& pelvis) {
  if(!st.on || !fitted.valid)
    return;
  const auto at = pelvis*fitted.local*swingMatrix();
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

Quiver::Probe Quiver::probe(const Matrix4x4& pelvis, const std::vector<SkinPoint>& posed,
                            const std::vector<bool>& legBone, const std::vector<bool>& armBone) const {
  Probe p;
  p.on = st.on && fitted.valid;
  if(!p.on)
    return p;
  p.arrows = int(arrows.size());
  p.mouth  = translation(fitted.local);
  p.out    = fitted.out;
  auto inv = pelvis*fitted.local*swingMatrix();
  inv.inverse();
  {
    const Vec3 a = Vec3::normalize(axis);
    p.swing   = std::acos(std::clamp(-a.y,-1.f,1.f))*180.f/float(M_PI);
    p.back    = std::atan2(-a.z,-a.y)*180.f/float(M_PI);
    p.outward = std::atan2( a.x,-a.y)*180.f/float(M_PI);
  }
  for(auto& pt:posed) {
    Vec3 q = pt.pos;
    inv.project(q);
    if(q.y>0 || q.y<-Length)
      continue;
    const float depth = radiusAt(-q.y) - std::hypot(q.x,q.z);
    if(depth<=0)
      continue;
    if(pt.bone<armBone.size() && armBone[pt.bone])
      p.arm = std::max(p.arm,depth); else
    if(pt.bone<legBone.size() && legBone[pt.bone])
      p.leg = std::max(p.leg,depth); else
      p.armor = std::max(p.armor,depth);
    }
  // the arrows' span along the quiver: the nock's end and the tip of each
  bool first = true;
  for(size_t i=0; i<arrows.size() && i<arrowLocal.size(); ++i) {
    const auto* pm = arrows[i].protoMesh();
    if(pm==nullptr)
      continue;
    const Vec3* box = pm->bbox();
    for(float x:{box[0].x,box[1].x}) {
      Vec3 v = {x,(box[0].y+box[1].y)*0.5f,(box[0].z+box[1].z)*0.5f};
      arrowLocal[i].project(v);
      if(first) {
        p.top    = v.y;
        p.bottom = v.y;
        first    = false;
        }
      p.top    = std::max(p.top,v.y);
      p.bottom = std::min(p.bottom,v.y);
      }
    }
  p.bottom += Length;
  return p;
  }

std::vector<size_t> Quiver::nearPoints(const Fit& f, const Skeleton& sk, const std::vector<SkinPoint>& bind) {
  std::vector<size_t> ret;
  if(!f.valid || f.pelvis>=sk.tr.size())
    return ret;
  // the quiver at rest in the bind pose; a point that far from its axis may come to it as the legs and the
  // flaps move and the quiver swings (SwingMax at the bottom ~ 38 cm aside)
  constexpr float Reach = 30.f;
  auto inv = sk.tr[f.pelvis]*f.local;
  inv.inverse();
  const auto arm = armBones(sk);
  for(size_t i=0; i<bind.size(); ++i) {
    if(bind[i].src==size_t(-1) || (bind[i].bone<arm.size() && arm[bind[i].bone]))
      continue; // the arms are capsules
    Vec3 q = bind[i].pos;
    inv.project(q);
    if(q.y>5.f || q.y<-Length-Reach)
      continue;
    if(std::hypot(q.x,q.z)>radiusAt(-q.y)+Reach)
      continue;
    ret.push_back(bind[i].src);
    }
  // no more than that many: every n-th (the mesh is dense on the flaps)
  constexpr size_t MaxPoints = 600;
  if(ret.size()>MaxPoints) {
    std::vector<size_t> thin;
    const double stride = double(ret.size())/double(MaxPoints);
    for(size_t k=0; k<MaxPoints; ++k)
      thin.push_back(ret[size_t(double(k)*stride)]);
    ret = std::move(thin);
    }
  return ret;
  }

void Quiver::rest() {
  axis   = {0,-1,0};
  moving = false;
  massV  = Vec3();
  }

Matrix4x4 Quiver::swingMatrix() const {
  // the rotation that takes the rest's bottom (0,-1,0) to the axis: about their common normal
  Matrix4x4 m;
  m.identity();
  const Vec3  from = {0,-1,0};
  const Vec3  to   = Vec3::normalize(axis);
  const Vec3  k    = Vec3::crossProduct(from,to);
  const float s    = k.length(), c = Vec3::dotProduct(from,to);
  if(s<1e-6f)
    return m;
  const Vec3  u = k/s;
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

void Quiver::step(const Matrix4x4& pelvis, const std::vector<Obstacle>& obstacles, float dt) {
  if(!st.on || !fitted.valid)
    return;
  const auto frame = pelvis*fitted.local;          // the quiver at rest, in the world
  const Vec3 pivot = translation(frame);
  Vec3 restDir = {0,-1,0};
  {
    Vec3 o = {0,0,0}, b = {0,-1,0};
    frame.project(o);
    frame.project(b);
    restDir = Vec3::normalize(b-o);
  }
  const Vec3 restAt = pivot + restDir*MassAt;
  // a jump (a teleport, a load) or the first step: from the rest
  if(!moving || (pivot-lastPivot).length()>100.f || dt<=0.f || dt>0.25f) {
    mass      = restAt;
    massV     = Vec3();
    lastPivot = pivot;
    lastRest  = restAt;
    axis      = {0,-1,0};
    moving    = dt>0.f && dt<=0.25f;
    return;
    }

  const float w     = 2.f*float(M_PI)*SwingFreq;
  const float cosMax= std::cos(SwingMax*float(M_PI)/180.f);
  const Vec3  restV = (restAt-lastRest)/dt;
  const Vec3  pivV  = (pivot-lastPivot)/dt;
  const int   n     = std::max(1,int(std::ceil(dt*120.f)));
  const float h     = dt/float(n);
  for(int i=1; i<=n; ++i) {
    const float k   = float(i)/float(n);
    const Vec3  pv  = lastPivot + (pivot-lastPivot)*k;
    const Vec3  ra  = lastRest  + (restAt-lastRest)*k;
    // the strap's spring to the rest and the leather's damping; the pelvis's speeding up is felt as the
    // rest point runs away from the mass
    const Vec3  acc = (ra-mass)*(w*w) - (massV-restV)*(2.f*SwingDamp*w);
    massV += acc*h;
    mass  += massV*h;
    // on the sphere round the ring
    Vec3 d = mass-pv;
    float len = d.length();
    if(len<1e-3f) {
      d   = restDir;
      len = 1;
      }
    d = d/len;
    const Vec3 rd = Vec3::normalize(ra-pv);
    const float c = Vec3::dotProduct(d,rd);
    if(c<cosMax) {
      const Vec3 side = Vec3::normalize(d - rd*c);
      d = rd*cosMax + side*std::sqrt(1.f-cosMax*cosMax);
      }
    mass = pv + d*MassAt;
    const Vec3 rel = massV-pivV;
    massV -= d*Vec3::dotProduct(rel,d);
    }

  // the legs, the arm and the flaps push it round the ring: each obstacle within the tube moves the axis
  // away at its depth along it
  Vec3 a = Vec3::normalize(mass-pivot);
  const Vec3 before = a;
  for(int pass=0; pass<3; ++pass) {
    bool hit = false;
    for(auto& o:obstacles) {
      // the closest points of the axis (pivot + a*t, t 0..Length) and the obstacle's segment
      const Vec3  e  = o.b-o.a;
      const Vec3  r0 = pivot-o.a;
      const float ee = Vec3::dotProduct(e,e);
      const float ae = Vec3::dotProduct(a,e);
      const float ar = Vec3::dotProduct(a,r0);
      const float er = Vec3::dotProduct(e,r0);
      float t = 0, u = 0;
      if(ee<1e-6f) {
        t = std::clamp(-ar,0.f,Length);
        } else {
        const float den = ee - ae*ae;
        t = den>1e-6f ? std::clamp((ae*er - ee*ar)/den,0.f,Length) : 0.f;
        u = std::clamp((t*ae + er)/ee,0.f,1.f);
        t = std::clamp(Vec3::dotProduct(o.a+e*u-pivot,a),0.f,Length);
        }
      if(t<2.f)
        continue; // at the ring: it cannot turn away there
      const Vec3  onAxis = pivot + a*t;
      const Vec3  onObs  = o.a + e*u;
      Vec3        away   = onAxis-onObs;
      away -= a*Vec3::dotProduct(away,a);
      float dist = away.length();
      const float need = radiusAt(t) + o.r + PushMargin;
      if(dist>=need)
        continue;
      if(dist<1e-3f) {
        // on the axis: away from the body, outward
        Vec3 out = {frame.at(0,0),frame.at(0,1),frame.at(0,2)};
        away = Vec3::normalize(out - a*Vec3::dotProduct(out,a));
        dist = 0;
        } else {
        away = away/dist;
        }
      a   = Vec3::normalize(a + away*((need-dist)/t));
      hit = true;
      }
    if(!hit)
      break;
    }
  if((a-before).length()>1e-5f) {
    const Vec3 moved = pivot + a*MassAt;
    const Vec3 n     = Vec3::normalize(moved-mass);
    const float vn   = Vec3::dotProduct(massV-pivV,n);
    if(vn<0.f)
      massV -= n*vn; // it does not keep running into the leg
    mass = moved;
    }

  // the axis in the quiver's rest frame
  auto inv = frame;
  inv.inverse();
  Vec3 o = {0,0,0}, b = a;
  b = pivot + a;
  inv.project(o);
  inv.project(b);
  axis      = Vec3::normalize(b-o);
  lastPivot = pivot;
  lastRest  = restAt;
  }
