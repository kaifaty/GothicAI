#include "quiver.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string_view>

#include "protomesh.h"
#include "skeleton.h"
#include "animmath.h"

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

std::vector<std::vector<Matrix4x4>> Quiver::motionFrames(const Skeleton& sk) {
  // the moves that swing the legs and the flaps the most, with the bow and without; the aim's crouch is made
  // by the engine (aim), not a clip: the clearance covers it
  static const char* moves[] = {"S_WALKL","S_RUNL","S_SNEAKL","S_WALKWL","S_BOWWALKL","S_BOWRUNL","S_BOWSNEAKL",
                                "S_BOWRUN","S_BOWAIM","S_BOWWALK"};
  constexpr uint32_t PerMove = 12;
  std::vector<std::vector<Matrix4x4>> ret;
  const size_t n = sk.nodes.size();
  for(auto name:moves) {
    const auto* seq = sk.sequence(name);
    if(seq==nullptr || seq->data==nullptr)
      continue;
    const auto&  d      = *seq->data;
    const size_t idSize = d.nodeIndex.size();
    if(d.numFrames==0 || idSize==0 || d.samples.size()<size_t(d.numFrames)*idSize)
      continue;
    for(uint32_t k=0; k<PerMove; ++k) {
      const size_t frame = size_t(k)*d.numFrames/PerMove;
      std::vector<Matrix4x4> local(n), world(n);
      for(size_t i=0; i<n; ++i)
        local[i] = sk.nodes[i].tr;
      for(size_t i=0; i<idSize; ++i)
        if(d.nodeIndex[i]<n)
          local[d.nodeIndex[i]] = mkMatrix(d.samples[frame*idSize+i]);
      std::vector<bool> done(n,false);
      for(size_t pass=0; pass<n; ++pass) {
        bool all = true;
        for(size_t i=0; i<n; ++i) {
          if(done[i])
            continue;
          const size_t parent = sk.nodes[i].parent;
          if(parent<n && !done[parent]) {
            all = false;
            continue;
            }
          world[i] = parent<n ? world[parent]*local[i] : local[i];
          done[i]  = true;
          }
        if(all)
          break;
        }
      ret.push_back(std::move(world));
      }
    }
  return ret;
  }

void Quiver::toBind(const Skeleton& sk, const std::vector<Matrix4x4>& frame, std::vector<SkinPoint>& pts) {
  const size_t pelvis = sk.findNode("BIP01 PELVIS");
  if(pelvis>=frame.size() || pelvis>=sk.tr.size())
    return;
  auto inv = frame[pelvis];
  inv.inverse();
  const auto m = sk.tr[pelvis]*inv;
  for(auto& p:pts)
    m.project(p.pos);
  }

Quiver::Fit Quiver::fit(const Skeleton& sk, const std::vector<SkinPoint>& bind, const std::vector<SkinPoint>& moving) {
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
    for(size_t pi=0; pi<bind.size()+moving.size(); ++pi) {
      const auto& p = pi<bind.size() ? bind[pi] : moving[pi-bind.size()];
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
  arrowSpan[0] = arrowSpan[1] = Vec3();
  }

void Quiver::clear() {
  st         = State();
  fitted     = Fit();
  body       = MeshObjects::Mesh();
  arrows.clear();
  arrowLocal.clear();
  }

void Quiver::rebind(const Skeleton& sk) {
  if(fitted.valid)
    fitted.pelvis = sk.findNode("BIP01 PELVIS");
  }

void Quiver::sync(const Matrix4x4& pelvis) {
  if(!st.on || !fitted.valid)
    return;
  const auto at = pelvis*fitted.local;
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
  auto inv = pelvis*fitted.local;
  inv.inverse();
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
