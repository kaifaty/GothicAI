#include "flinch.h"

#include <Tempest/Log>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string_view>

#include "ikmath.h"
#include "skeleton.h"

using namespace Tempest;
using namespace IkMath;

namespace {

constexpr float Deg        = float(M_PI)/180.f;
constexpr float TrunkFreq  = 11.f;     // rad/s: the trunk springs back...
constexpr float TrunkDamp  = 0.85f;    // ...nearly without swinging over
constexpr float NeckFreq   = 15.f;     // the head against the trunk: quicker,
constexpr float NeckDamp   = 0.5f;     // it swings
constexpr float Inertia    = 0.9f;     // the head lags behind the trunk's jerk
constexpr float BendKick   = 8.f;      // rad/s at full strength: ~16 degrees away from the blow...
constexpr float TwistKick  = 6.f;      // ...~12 about up off the middle
constexpr float HeadKick   = 7.f;      // a blow to the head throws it on top: ~22 degrees more
constexpr float MaxTrunk   = 28.f*Deg;
constexpr float MaxNeck    = 35.f*Deg;
constexpr float WaistShare = 0.4f;     // of the trunk's turn the waist does, the chest the rest
constexpr float MaxStep    = 1.f/240.f;
constexpr float RestAngle  = 0.05f*Deg;
constexpr float RestSpeed  = 0.5f*Deg;
constexpr float MaxBumpTrunk = 25.f*Deg;
constexpr float MaxBumpNeck  = 30.f*Deg;

// trunk, twist, head, freq, damp, headFreq, headDamp, floor
Flinch::BumpForm bumpForms[Flinch::Bump::KindCount] = {
  {20.f, 8.f, 22.f, 14.f, 0.75f, 18.f, 0.55f, 0.35f}, // Wall: off it, the head stays behind
  {20.f, 0.f, -6.f, 14.f, 1.f,   16.f, 0.7f,  0.35f}, // Waist: folds over it and comes up, no swing back
  {16.f, 8.f, 18.f, 14.f, 0.75f, 18.f, 0.55f, 0.35f}, // Person: as off a wall; the other body gives
  {18.f, 0.f, 16.f, 11.f, 0.8f,  15.f, 0.5f,  0.35f}, // Pushed: the runner's way, a body at rest is softer
  };

// the speed that carries a damped spring to this peak
float kickFor(float peak, float freq, float damp) {
  if(damp>=1.f)
    return peak*freq*float(M_E);
  const float w = std::sqrt(1.f-damp*damp);
  return peak*freq*std::exp(damp*std::atan2(w,damp)/w);
  }

bool enabled = []() {
  const char* v = std::getenv("OPENGOTHIC_FLINCH");
  return v==nullptr || v[0]!='0';
  }();

float yawBetween(const Vec3& a, const Vec3& b) {
  return std::atan2(dot(cross(Up,a),b), dot(a,b));
  }

// the body's frame: forward, up, side
Vec3 toBody(const Vec3& v, const Vec3& f) {
  return Vec3(dot(v,f), v.y, dot(v,cross(Up,f)));
  }

Vec3 fromBody(const Vec3& v, const Vec3& f) {
  return f*v.x + Up*v.y + cross(Up,f)*v.z;
  }

void clampLength(Vec3& v, float max) {
  const float l = length(v);
  if(l>max)
    v = v*(max/l);
  }

}

Flinch::Probe Flinch::playerProbe;
Flinch::Probe Flinch::subjectProbe;
const void*   Flinch::probeNpc = nullptr;

bool Flinch::isEnabled() {
  return enabled;
  }

void Flinch::setEnabled(bool e) {
  enabled = e;
  Log::i("flinch: ", e ? "on" : "off");
  }

void Flinch::setSkeleton(const Skeleton* sk) {
  *this = Flinch();
  if(sk==nullptr)
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
    for(size_t i=0; root!=size_t(-1) && i<nodes.size(); ++i)
      if(isUnder(i,root))
        out.push_back(i);
    };

  waist = find("BIP01 SPINE1");
  chest = find("BIP01 SPINE2");
  head  = find("BIP01 HEAD");
  if(head==size_t(-1))
    return;
  if(waist==size_t(-1) || chest==size_t(-1) || !isUnder(chest,waist) || !isUnder(head,chest)) {
    // no trunk to bend: the head alone
    waist = size_t(-1);
    chest = size_t(-1);
    } else {
    tree(waist,waistTree);
    tree(chest,chestTree);
    }
  tree(head,headTree);
  valid = true;
  }

bool Flinch::active() const {
  return trunk!=Vec3() || trunkV!=Vec3() || neck!=Vec3() || neckV!=Vec3() || bumpActive();
  }

bool Flinch::bumpActive() const {
  return bTrunk!=Vec3() || bTrunkV!=Vec3() || bNeck!=Vec3() || bNeckV!=Vec3();
  }

Flinch::BumpForm& Flinch::bumpForm(Bump::Kind kind) {
  return bumpForms[kind<Bump::KindCount ? kind : Bump::Wall];
  }

void Flinch::bump(const Bump& b, const Vec3& fwd) {
  if(!valid || b.kind>=Bump::KindCount)
    return;
  const Vec3 f = normalize(Vec3(fwd.x,0,fwd.z));
  if(length(f)>0.5f)
    forward = f;
  const Vec3 n = normalize(Vec3(b.normal.x,0,b.normal.z));
  const float s = std::clamp(b.strength,0.f,1.f);
  if(length(n)<0.5f || s<=0.f)
    return;
  const auto& form = bumpForms[b.kind];
  const float a    = form.floor + (1.f-form.floor)*s;
  bFreq     = std::max(form.freq,1.f);
  bDamp     = std::clamp(form.damp,0.1f,1.f);
  bHeadFreq = std::max(form.headFreq,1.f);
  bHeadDamp = std::clamp(form.headDamp,0.1f,1.f);

  // where the top of the trunk is thrown, and where the obstacle is
  const Vec3 thrown   = b.kind==Bump::Waist ? n*-1.f : n;
  const Vec3 obstacle = b.kind==Bump::Pushed ? Vec3() : n*-1.f;
  const Vec3 axis     = toBody(cross(Up,thrown),forward);
  bTrunkV += axis*(kickFor(form.trunk*Deg,bFreq,bDamp)*a);
  // the shoulder that meets the obstacle stops, the other one goes on: the trunk turns to the obstacle
  const float side = toBody(obstacle,forward).z;
  bTrunkV += Vec3(0,1,0)*(-kickFor(form.twist*Deg,bFreq,bDamp)*a*std::clamp(side,-1.f,1.f));
  bNeckV  += axis*(-kickFor(form.head*Deg,bHeadFreq,bHeadDamp)*a);
  bumps++;
  }

void Flinch::hit(const Hit& h, const Vec3& fwd) {
  if(!valid || !enabled)
    return;
  const Vec3 f = normalize(Vec3(fwd.x,0,fwd.z));
  if(length(f)>0.5f)
    forward = f;

  // straight down or up: it pushes the body back
  Vec3 d = normalize(Vec3(h.dir.x,0,h.dir.z));
  if(length(d)<0.5f)
    d = forward*-1.f;
  const float s  = std::clamp(h.strength,0.f,1.f);
  const float ht = std::clamp(h.height,0.f,1.f);

  // the top of the trunk goes where the blow pushes: about cross(up, d); higher has more lever
  const Vec3 axis = toBody(cross(Up,d),forward);
  trunkV += axis*(BendKick*s*(0.6f + 0.4f*ht));
  trunkV += Vec3(0,1,0)*(TwistKick*s*std::clamp(h.twist,-1.f,1.f));
  neckV  += axis*(HeadKick*s*std::clamp((ht-0.75f)/0.25f,0.f,1.f));

  hits++;
  lastStrength = s;
  lastHeight   = ht;
  }

bool Flinch::update(const Input& in) {
  if(!valid)
    return false;
  const Vec3 f = normalize(Vec3(in.forward.x,0,in.forward.z));
  if(length(f)>0.5f)
    forward = f;
  headOnly = in.headOnly || waist==size_t(-1);
  if(!active())
    return false;
  if(!enabled) {
    trunk = trunkV = neck = neckV = Vec3();
    if(!bumpActive())
      return true;
    }

  const float dt = float(in.dt)/1000.f;
  if(dt<=0.f)
    return false;
  const int   n = std::max(1,int(std::ceil(dt/MaxStep)));
  const float h = dt/float(n);
  for(int i=0; i<n; ++i) {
    // a damped spring each; the head is shaken by how the trunk speeds up
    const Vec3 at = trunk*(-TrunkFreq*TrunkFreq) - trunkV*(2.f*TrunkDamp*TrunkFreq);
    trunkV += at*h;
    trunk  += trunkV*h;
    const Vec3 an = neck*(-NeckFreq*NeckFreq) - neckV*(2.f*NeckDamp*NeckFreq) - at*Inertia;
    neckV  += an*h;
    neck   += neckV*h;
    // the bump: the same springs with its own stiffness, the head is thrown by itself
    bTrunkV += (bTrunk*(-bFreq*bFreq) - bTrunkV*(2.f*bDamp*bFreq))*h;
    bTrunk  += bTrunkV*h;
    bNeckV  += (bNeck*(-bHeadFreq*bHeadFreq) - bNeckV*(2.f*bHeadDamp*bHeadFreq))*h;
    bNeck   += bNeckV*h;
    }
  clampLength(trunk,MaxTrunk);
  clampLength(neck, MaxNeck);
  clampLength(bTrunk,MaxBumpTrunk);
  clampLength(bNeck, MaxBumpNeck);
  if(length(bTrunk)<RestAngle && length(bTrunkV)<RestSpeed && length(bNeck)<RestAngle && length(bNeckV)<RestSpeed)
    bTrunk = bTrunkV = bNeck = bNeckV = Vec3();

  if(length(trunk)<RestAngle && length(trunkV)<RestSpeed && length(neck)<RestAngle && length(neckV)<RestSpeed)
    trunk = trunkV = neck = neckV = Vec3();
  return true;
  }

void Flinch::apply(Matrix4x4* tr, size_t numBones) {
  if(!valid || !active() || headTree.back()>=numBones)
    return;
  const Vec3  t  = fromBody(trunk+bTrunk,forward);
  const float at = length(t);
  Vec3 n = fromBody(neck+bNeck,forward);
  if(!headOnly && chestTree.back()<numBones && waistTree.back()<numBones) {
    if(at>0.f) {
      const Vec3 axis = t*(1.f/at);
      rotateAbout(tr,waistTree,axisAngle(axis,at*WaistShare),       position(tr[waist]));
      rotateAbout(tr,chestTree,axisAngle(axis,at*(1.f-WaistShare)), position(tr[chest]));
      }
    } else {
    // four legs: the trunk carries the front paws on the ground, the head takes all of it
    n += t;
    }
  const float an = length(n);
  if(an>0.f)
    rotateAbout(tr,headTree,axisAngle(n*(1.f/an),an),position(tr[head]));
  }

void Flinch::probe(Probe& p) const {
  p.valid    = valid;
  p.active   = active();
  // the bend: the trunk's turn without the part about up
  const Vec3 bendB = Vec3(trunk.x,0,trunk.z);
  p.bend     = length(bendB)/Deg;
  const Vec3 top = cross(fromBody(bendB,forward),Up); // where the top of the trunk goes
  p.bendYaw  = length(top)>1e-5f ? yawBetween(forward,normalize(top))/Deg : 0.f;
  p.twist    = trunk.y/Deg;
  p.head     = length(neck)/Deg;
  p.hits     = hits;
  p.strength = lastStrength;
  p.height   = lastHeight;
  // a turn about the side axis leans forward
  p.bumpPitch = bTrunk.z/Deg;
  p.bumpRoll  = bTrunk.x/Deg;
  p.bumpTwist = bTrunk.y/Deg;
  p.bumpHead  = bNeck.z/Deg;
  p.bumps     = bumps;
  }
