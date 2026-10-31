#include "aimwalk.h"

#include <Tempest/Log>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string_view>

#include "animmath.h"
#include "ikmath.h"
#include "skeleton.h"

using namespace Tempest;
using namespace IkMath;

namespace {

constexpr float BlendIn  = 0.25f; // seconds: the legs take the steps...
constexpr float BlendOut = 0.3f;  // ...and stand again
constexpr float StopDuration = 0.36f;
constexpr float StopLift = 6.f;
constexpr float StopStride = 32.f;

float smooth(float t) {
  t = std::clamp(t,0.f,1.f);
  return t*t*(3.f-2.f*t);
  }

bool enabled = []() {
  const char* v = std::getenv("OPENGOTHIC_AIMWALK");
  return v==nullptr || v[0]!='0';
  }();

float wrapAngle(float a) {
  return std::remainder(a,2.f*float(M_PI));
  }

// about up: the yaw (atan2 of z, x) of what it turns grows by a
Mat3 turnY(float a) {
  return axisAngle(Up,-a);
  }

Vec3 vector(const Matrix4x4& m, const Vec3& v) {
  return {m.at(0,0)*v.x+m.at(1,0)*v.y+m.at(2,0)*v.z,
          m.at(0,1)*v.x+m.at(1,1)*v.y+m.at(2,1)*v.z,
          m.at(0,2)*v.x+m.at(1,2)*v.y+m.at(2,2)*v.z};
  }

}

AimWalk::Probe AimWalk::playerProbe;

bool AimWalk::isEnabled() {
  return enabled;
  }

void AimWalk::setEnabled(bool e) {
  enabled = e;
  Log::i("aimwalk: ", e ? "on" : "off");
  }

void AimWalk::setSkeleton(const Skeleton* sk) {
  *this = AimWalk();
  skeleton = sk;
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
  auto  depth = [&](size_t id) {
    size_t d = 0;
    for(size_t i=nodes[id].parent; i<nodes.size(); i=nodes[i].parent)
      ++d;
    return d;
    };
  auto  tree = [&](size_t root, std::vector<size_t>& out) {
    for(size_t i=0; i<nodes.size(); ++i)
      if(isUnder(i,root))
        out.push_back(i);
    std::stable_sort(out.begin(),out.end(),[&](size_t a, size_t b){ return depth(a)<depth(b); });
    };

  root   = find("BIP01");
  pelvis = find("BIP01 PELVIS");
  spine  = find("BIP01 SPINE");
  spine1 = find("BIP01 SPINE1");
  spine2 = find("BIP01 SPINE2");
  lThigh = find("BIP01 L THIGH");
  rThigh = find("BIP01 R THIGH");
  lArm   = find("BIP01 L UPPERARM");
  rArm   = find("BIP01 R UPPERARM");
  lFoot  = find("BIP01 L FOOT");
  rFoot  = find("BIP01 R FOOT");
  for(auto i:{root,pelvis,spine,spine1,spine2,lThigh,rThigh,lArm,rArm,lFoot,rFoot})
    if(i>=nodes.size())
      return;
  if(nodes[pelvis].parent!=root || nodes[spine].parent!=pelvis || nodes[spine1].parent!=spine ||
     nodes[spine2].parent!=spine1 || nodes[lThigh].parent!=pelvis || nodes[rThigh].parent!=pelvis ||
     !isUnder(lFoot,lThigh) || !isUnder(rFoot,rThigh))
    return;
  const char* calfNames[] = {"BIP01 L CALF", "BIP01 R CALF"};
  const char* toeNames [] = {"BIP01 L TOE0", "BIP01 R TOE0"};
  for(size_t i=0; i<feet.size(); ++i) {
    auto& f = feet[i];
    f.thigh = i==0 ? lThigh : rThigh;
    f.foot  = i==0 ? lFoot  : rFoot;
    f.calf  = find(calfNames[i]);
    f.toe   = find(toeNames[i]);
    if(f.calf>=nodes.size() || !isUnder(f.calf,f.thigh) || !isUnder(f.foot,f.calf))
      return;
    tree(f.thigh,f.thighTree);
    tree(f.calf, f.calfTree);
    tree(f.foot, f.footTree);
    }
  tree(lThigh,legsTree);
  tree(rThigh,legsTree);
  std::stable_sort(legsTree.begin(),legsTree.end(),[&](size_t a, size_t b){ return depth(a)<depth(b); });
  tree(spine, spineTree);
  tree(spine1,spine1Tree);
  tree(spine2,spine2Tree);
  valid = true;
  }

float AimWalk::cycle() const {
  // the loop as its root goes: the last frame is the first one again
  if(seq==nullptr || seq->data==nullptr || seq->data->numFrames<2 || seq->data->fpsRate<=0.f)
    return 0;
  return float(seq->data->numFrames-1)*1000.f/seq->data->fpsRate;
  }

float AimWalk::rate() const {
  const float c = cycle();
  if(c<=0.f)
    return 0;
  const float v = std::sqrt(seq->data->moveTr.x*seq->data->moveTr.x + seq->data->moveTr.z*seq->data->moveTr.z)/c; // cm/ms
  if(v<=0.f)
    return 0;
  return (dir>0.f ? Forward : Back)/1000.f/v;
  }

bool AimWalk::sample(float at, size_t node, asset::AnimationSample& out) const {
  if(node>=seqNode.size() || seqNode[node]<0)
    return false;
  const auto&  d  = *seq->data;
  const size_t n  = d.nodeIndex.size();
  const float  fr = at*d.fpsRate/1000.f;
  uint32_t     a  = uint32_t(fr);
  const float  k  = fr-float(a);
  a = std::min(a,d.numFrames-2);
  const uint32_t b = a+1;
  out = mix(d.samples[size_t(a)*n+size_t(seqNode[node])],d.samples[size_t(b)*n+size_t(seqNode[node])],k);
  return true;
  }

float AimWalk::hipsYaw(const Matrix4x4* tr) const {
  const Vec3 d = position(tr[lThigh]) - position(tr[rThigh]);
  return std::atan2(d.z,d.x);
  }

float AimWalk::chestYaw(const Matrix4x4* tr) const {
  const Vec3 d = position(tr[lArm]) - position(tr[rArm]);
  return std::atan2(d.z,d.x);
  }

bool AimWalk::update(const Input& in) {
  if(!valid)
    return false;
  if(in.legs!=seq) {
    seq = in.legs;
    seqNode.assign(skeleton->nodes.size(),-1);
    if(seq!=nullptr && seq->data!=nullptr) {
      auto& d = *seq->data;
      for(size_t i=0; i<d.nodeIndex.size(); ++i)
        if(d.nodeIndex[i]<seqNode.size())
          seqNode[d.nodeIndex[i]] = int(i);
      if(d.numFrames<2 || d.samples.size()<size_t(d.numFrames)*d.nodeIndex.size() || seqNode[root]<0 || seqNode[pelvis]<0)
        seq = nullptr;
      }
    }

  const float dt   = float(in.dt)/1000.f;
  const float prev = weight;
  const bool wasStopping = stopping;
  const bool wasStanding = !standing.empty();
  finishPose = false;
  // Freeze the last completed pose, not an intermediate rebuild from this update.
  for(auto& f:feet) {
    f.speed = length(position(f.final)-position(f.previousFinal));
    f.previousRaw   = f.raw;
    f.previousFinal = f.final;
    }
  walk = std::clamp(in.walk,-1.f,1.f);
  blocked = in.blocked;
  const bool on = enabled && in.active && seq!=nullptr && cycle()>0.f;
  if(on && walk!=0.f && !blocked) {
    // A short release can be interrupted by another step. Resume the same gait clock/weight
    // that the original fade would have reached; settling only changes the stopped pose.
    if(resumeFade) {
      weight = resumeWeight;
      phase  = resumePhase;
      resumeFade = false;
      }
    stopping = contacts = false;
    if(weight<=0.f)
      phase = 0;
    dir    = walk>0.f ? 1.f : -1.f;
    weight = std::min(1.f, weight + dt/BlendIn);
    } else if(on && (weight>0.f || stopping) && haveFeet) {
    if(!stopping) {
      stopping = true;
      contacts = false;
      stopTime = 0;
      stopWeight = weight;
      resumeWeight = weight;
      resumePhase = phase;
      resumeFade = true;
      } else {
      stopTime = std::min(StopDuration,stopTime+dt);
      }
    weight = stopWeight*(1.f-smooth(stopTime/StopDuration));
    resumeWeight = std::max(0.f,resumeWeight-dt/BlendOut);
    if(resumeWeight>0.f && !blocked) {
      resumePhase = std::fmod(resumePhase+dir*rate()*float(in.dt),cycle());
      if(resumePhase<0.f)
        resumePhase += cycle();
      }
    if(stopTime>=StopDuration) {
      stopping = false;
      finishPose = true;
      }
    } else {
    stopping = contacts = false;
    weight = std::max(0.f, weight - dt/BlendOut);
    }
  if(!in.active || !enabled) {
    standing.clear();
    resumeFade = false;
    }
  if(weight>=1.f && !stopping)
    standing.clear();
  if(seq==nullptr)
    weight = 0;
  if(weight>0.f && !stopping && !finishPose) {
    const float c = cycle();
    phase = std::fmod(phase + dir*rate()*float(in.dt), c);
    if(phase<0.f)
      phase += c;
    }
  return isActive() || prev>0.f || wasStopping || (wasStanding && standing.empty());
  }

Vec3 AimWalk::move(uint64_t dt) const {
  if(!isActive() || stopping || blocked)
    return Vec3();
  const auto& d = *seq->data;
  const float c = cycle();
  if(c<=0.f || d.tr.empty())
    return Vec3();
  // the root's way: as Animation::Sequence::translateXZ, in fractions of a ms
  auto at = [&](float ms) {
    const float  fr = ms*d.fpsRate/1000.f;
    const size_t n  = d.tr.size();
    const size_t fa = size_t(fr);
    const float  k  = fr-float(fa);
    Vec3 a = d.tr[fa%n]     + d.moveTr*float(fa/n);
    Vec3 b = d.tr[(fa+1)%n] + d.moveTr*float((fa+1)/n);
    return a + (b-a)*k;
    };
  const float span = rate()*float(dt);
  Vec3 r;
  if(dir>0.f) {
    r = at(phase+span) - at(phase);
    } else {
    float from = phase-span;
    while(from<0.f)
      from += c;
    r = at(from) - at(from+span);
    }
  return Vec3(r.x*weight,0,r.z*weight);
  }

void AimWalk::apply(Matrix4x4* tr, size_t numBones, const asset::AnimationSample* base, const Matrix4x4& m) {
  twist = chest = drop = 0;
  if(!valid || base==nullptr || numBones!=skeleton->nodes.size())
    return;
  model = m;
  for(size_t i=0; i<feet.size(); ++i)
    stancePoints[i] = position(tr[feet[i].foot]);
  if(!isActive() && !finishPose) {
    if(standing.size()==numBones) {
      // The landed stance is ordinary local skeletal data: a turn moves the feet with the
      // hero, and FootIk is free to adapt it to the ground. No contact constraint survives.
      for(auto i:legsTree) {
        const size_t p = skeleton->nodes[i].parent;
        tr[i] = (p<numBones ? tr[p] : m)*standing[i];
        }
      }
    for(auto& f:feet)
      f.raw = tr[f.foot];
    return;
    }
  auto& nodes = skeleton->nodes;

  // the stance as the aim holds it
  stance.assign(tr,tr+numBones);
  const float hipsA  = hipsYaw(tr);
  const float chestA = chestYaw(tr);

  // the legs step: their animation over the stance's, as far as they are in; the root where the stance has
  // it on the ground (the model goes where the steps take it, Pose::animMoveSpeed), its height and turn
  // the steps'
  auto local = [&](size_t i) {
    asset::AnimationSample s;
    if(!sample(phase,i,s))
      return Matrix4x4();
    if(standing.size()==numBones && !stopping && !finishPose && i!=root && i!=pelvis)
      return blend(standing[i],mkMatrix(s),weight);
    s = mix(base[i],s,weight);
    if(i==root) {
      s.position.x = base[i].position.x;
      s.position.z = base[i].position.z;
      }
    return mkMatrix(s);
    };
  auto set = [&](size_t i) {
    const size_t p = nodes[i].parent;
    if(i<seqNode.size() && seqNode[i]>=0) {
      tr[i] = (p<numBones ? tr[p] : m)*local(i);
      } else {
      // not stepped: as it hangs in the stance
      Matrix4x4 inv = p<numBones ? stance[p] : m;
      inv.inverse();
      Matrix4x4 local = inv*stance[i];
      if(standing.size()==numBones && !stopping && !finishPose)
        local = blend(standing[i],local,weight);
      tr[i] = (p<numBones ? tr[p] : m)*local;
      }
    };
  auto build = [&]() {
    set(root);
    set(pelvis);
    for(auto i:legsTree)
      set(i);
    };
  build();
  if(stopping || finishPose) {
    if(!contacts) {
      // The lower foot bears the weight. At double support prefer the foot already slowing down.
      const float dy = position(feet[0].previousRaw).y-position(feet[1].previousRaw).y;
      support = std::abs(dy)>1.f ? (dy<0.f ? 0u : 1u) : (feet[0].speed<feet[1].speed ? 0u : 1u);
      const Vec3 forward = normalize(Vec3(m.at(2,0),0,m.at(2,2)))*dir;
      for(size_t i=0; i<feet.size(); ++i) {
        auto& f = feet[i];
        f.from = f.previousRaw;
        f.anchor = f.previousFinal;
        f.goal = position(stance[f.foot]);
        if(i==support) {
          f.goal = position(f.from);
          continue;
          }
        Vec3 delta = f.goal-position(f.from);
        const float distance = std::sqrt(delta.x*delta.x+delta.z*delta.z);
        if(distance>StopStride) {
          f.goal.x = f.from.at(3,0)+delta.x*StopStride/distance;
          f.goal.z = f.from.at(3,2)+delta.z*StopStride/distance;
          }
        // Land behind the stopped body's plane, including the toes: no new placement over the edge.
        if(blocked) {
          const Vec3 toe = f.toe<numBones ? position(stance[f.toe])-position(stance[f.foot]) : Vec3();
          const float ahead = dot(f.goal-position(m),forward)+std::max(0.f,dot(toe,forward));
          if(ahead>0.f)
            f.goal -= forward*ahead;
          }
        }
      contacts = true;
      }
    const float t = std::clamp(stopTime/StopDuration,0.f,1.f);
    const float k = smooth(t);
    for(size_t i=0; i<feet.size(); ++i) {
      auto& f = feet[i];
      Vec3 target = position(f.from)+(f.goal-position(f.from))*k;
      if(i!=support)
        target.y += StopLift*std::pow(std::sin(float(M_PI)*t),2.f);
      const Mat3 orientation = i==support ? rotationOf(f.from) : rotationOf(blend(f.from,stance[f.foot],k));
      Vec3 bend = position(tr[f.calf])-position(tr[f.thigh]);
      const Vec3 toe = f.toe<numBones ? position(tr[f.toe])-position(tr[f.foot]) : Vec3();
      bend += normalize(Vec3(toe.x,0,toe.z))*2.f;
      TwoBone solved;
      if(twoBone(tr,f.thigh,f.calf,f.foot,f.thighTree,f.calfTree,target,bend,0.999f,0.f,solved))
        rotateAbout(tr,f.footTree,mul(orientation,transpose(rotationOf(tr[f.foot]))),position(tr[f.foot]));
      }
    }
  for(auto& f:feet)
    f.raw = tr[f.foot];
  if(finishPose) {
    standing.resize(numBones);
    for(auto i:legsTree) {
      const size_t p = nodes[i].parent;
      Matrix4x4 inv = p<numBones ? tr[p] : m;
      inv.inverse();
      standing[i] = inv*tr[i];
      }
    }

  // the upper body goes with the hips, its turn kept: the hips' turn is spread over the spine (the lower
  // spine takes most of it, the chest none)
  const Vec3 shift = position(tr[pelvis]) - position(stance[pelvis]);
  for(auto i:spineTree) {
    tr[i].set(3,0,tr[i].at(3,0)+shift.x);
    tr[i].set(3,1,tr[i].at(3,1)+shift.y);
    tr[i].set(3,2,tr[i].at(3,2)+shift.z);
    }
  const float d = wrapAngle(hipsYaw(tr)-hipsA);
  rotateAbout(tr,spineTree, turnY( d*2.f/3.f),position(tr[spine]));
  rotateAbout(tr,spine1Tree,turnY(-d/3.f),    position(tr[spine1]));
  rotateAbout(tr,spine2Tree,turnY(-d/3.f),    position(tr[spine2]));

  twist = d*180.f/float(M_PI);
  chest = wrapAngle(chestYaw(tr)-chestA)*180.f/float(M_PI);
  drop  = -shift.y;
  }

void AimWalk::captureFeet(const Matrix4x4* tr, size_t numBones) {
  if(!valid || numBones!=skeleton->nodes.size())
    return;
  for(auto& f:feet)
    f.final = tr[f.foot];
  haveFeet = true;
  }

void AimWalk::constrainFoot(size_t leg, Vec3& target) const {
  if(leg>=feet.size())
    return;
  const auto& f = feet[leg];
  if(!stopping && !finishPose) {
    if(!standing.empty())
      target += vector(model,f.correction)*(1.f-weight);
    return;
    }
  if(!contacts)
    return;
  if(leg==support) {
    target = position(f.anchor);
    } else {
    const Vec3 from = position(f.previousFinal);
    const Vec3 delta = target-from;
    const float distance = length(delta);
    if(distance>5.5f && distance<100.f)
      target = from+delta*(5.5f/distance);
    }
  }

void AimWalk::captureFootTarget(size_t leg, const Vec3& normal, const Vec3& shown) {
  if(!finishPose || leg>=feet.size())
    return;
  // The raw leg may be short of the contact until FootIk moves the hips for the lean/ground.
  // Hand that final correction over in model coordinates, so it follows subsequent turns.
  Matrix4x4 inv = model;
  inv.inverse();
  feet[leg].correction = vector(inv,shown-normal);
  }

bool AimWalk::stanceFoot(size_t leg, Vec3& point) const {
  if(leg>=feet.size() || (!stopping && !finishPose && (standing.empty() || isActive())))
    return false;
  point = stancePoints[leg];
  return true;
  }

void AimWalk::probe(Probe& p) const {
  p.valid  = valid;
  p.weight = weight;
  p.walk   = walk;
  const float c = cycle();
  p.phase  = c>0.f ? phase/c : 0.f;
  p.speed  = 0;
  if(isActive()) {
    const Vec3 v = move(100);
    p.speed = std::sqrt(v.x*v.x+v.z*v.z)*10.f*dir;
    }
  p.twist  = twist;
  p.chest  = chest;
  p.drop   = drop;
  }
