#include "gaze.h"

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

constexpr float Deg          = float(M_PI)/180.f;
constexpr float MaxHeadYaw   = 65.f*Deg;  // the head alone
constexpr float MaxChestYaw  = 25.f*Deg;  // the chest joins for larger turns...
constexpr float MaxWaistYaw  = 20.f*Deg;  // ...and the waist
constexpr float HeadShare    = 0.7f;      // of a turn the head takes this part, the rest the body
constexpr float GiveUp       = 150.f*Deg; // the partner further behind the back: stop looking
constexpr float MaxPitchUp   = 30.f*Deg;
constexpr float MaxPitchDown = 35.f*Deg;
constexpr float ChestPitch   = 0.25f;     // part of looking up or down the chest does
constexpr float TurnRate     = 6.f;       // 1/s, how fast the turn follows the partner
constexpr float BlendTime    = 0.4f;      // seconds, turning to the partner and back...
constexpr float MaxTurnSpeed = 180.f*Deg; // ...but not faster than this: a large turn takes longer
constexpr float MaxTrackSpeed= 270.f*Deg; // following the partner, or turning to another one
constexpr float MaxAnimYaw   = 50.f*Deg;  // the turn of the head the animation does, the gaze takes out
constexpr float AnimLag      = 0.12f;     // seconds: of the head up and down only the slow part (a nod stays)
constexpr float MaxAnimPitch = 30.f*Deg;
constexpr float FaceAxisCos  = 0.85f;     // the axis of the face is picked when the head looks this close to forward
constexpr float MaxFaceDown  = 0.8f;      // the face looks further down or up (a bent head): its turn is not known
constexpr float Snap         = 0.002f;
constexpr float ShiftJump    = 15.f*Deg;  // the target moved this far in one update: the eyes jump

bool enabled = []() {
  const char* v = std::getenv("OPENGOTHIC_GAZE");
  return v==nullptr || v[0]!='0';
  }();

void approach(float& v, float target, float k, float maxStep, bool& changed) {
  float nv = v + std::clamp((target-v)*k, -maxStep, maxStep);
  if(std::abs(target-nv)<Snap)
    nv = target;
  if(nv!=v)
    changed = true;
  v = nv;
  }

Vec3 axis(const Matrix4x4& m, int k) {
  return normalize(Vec3(m.at(k,0), m.at(k,1), m.at(k,2)));
  }

// where the body faces, from the hip joints: y up, the left hip on the left
Vec3 hipsForward(const Vec3& left, const Vec3& right) {
  const Vec3 f = cross(Up,left-right);
  return Vec3(f.x,0,f.z);
  }

// angle around Up that turns horizontal unit vector a toward b
float yawBetween(const Vec3& a, const Vec3& b) {
  return std::atan2(dot(cross(Up,a),b), dot(a,b));
  }

}

Gaze::Probe Gaze::playerProbe;
Gaze::Probe Gaze::subjectProbe;
const void* Gaze::probeNpc = nullptr;

bool Gaze::isEnabled() {
  return enabled;
  }

void Gaze::setEnabled(bool e) {
  enabled = e;
  Log::i("gaze: ", e ? "on" : "off");
  }

void Gaze::setSkeleton(const Skeleton* sk) {
  *this = Gaze();
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

  lthigh = find("BIP01 L THIGH");
  rthigh = find("BIP01 R THIGH");
  head  = find("BIP01 HEAD");
  chest = find("BIP01 SPINE2");
  waist = find("BIP01 SPINE1");
  if(head==size_t(-1))
    return;
  if(chest!=size_t(-1) && !isUnder(head,chest))
    chest = size_t(-1);
  if(waist!=size_t(-1) && (chest==size_t(-1) || !isUnder(chest,waist)))
    waist = size_t(-1);
  tree(head, headTree);
  tree(chest,chestTree);
  tree(waist,waistTree);

  // the face looks along the axis of the head bone closest to the facing of the bind pose, where the
  // head is upright (an npc may be bent over a cauldron all the time)
  if(lthigh!=size_t(-1) && rthigh!=size_t(-1) && sk->tr.size()==nodes.size()) {
    const Vec3 f = hipsForward(position(sk->tr[lthigh]),position(sk->tr[rthigh]));
    if(length(f)>1.f) {
      const Vec3 fn   = normalize(Vec3(f.x,0,f.z));
      float      best = 0;
      for(int k=0; k<3; ++k) {
        const float c = dot(axis(sk->tr[head],k),fn);
        if(std::abs(c)>std::abs(best)) {
          best     = c;
          faceAxis = k;
          faceSign = c>0 ? 1.f : -1.f;
          }
        }
      faceFixed = std::abs(best)>FaceAxisCos;
      if(!faceFixed)
        faceAxis = -1;
      }
    }
  valid = true;
  }

bool Gaze::update(const Input& in) {
  if(!valid)
    return false;

  const float dt      = float(in.dt)/1000.f;
  bool        changed = false;
  bool        on      = in.active && enabled;
  headOnly = in.headOnly;
  if(in.talk.x!=talk.x || in.talk.y!=talk.y || in.talk.z!=talk.z) {
    talk    = in.talk;
    changed = true;
    }
  if(on)
    source = in.source;

  npcForward = normalize(Vec3(in.forward.x,0,in.forward.z));
  if(on) {
    // the body facing: the hips as animated (at a mob the animation may turn the whole body), or the npc's
    const Vec3 f = length(hips)>0.5f ? hips : npcForward;
    if(tracking && weight>0.f && length(f)>0.5f && length(forward)>0.5f) {
      // the body turns under the head (to the one it looks at, say): the head keeps looking there
      const float turn = yawBetween(forward,f);
      if(turn!=0.f) {
        yaw    -= turn;
        changed = true;
        }
      }
    forward = f;
    target  = in.target;
    eyes    = in.eyes;
    eyesAt  = in.eyesAt;
    // from the eyes as the last pose drew them: the head turned so moves them, the next update aims from there
    const Vec3 d  = target - (eyes && eyesKnown ? eyesPos : headPos);
    const Vec3 dh = Vec3(d.x,0,d.z);
    const float y = length(dh)>1.f ? yawBetween(forward,normalize(dh)) : 0.f;
    if(length(dh)<=1.f || length(forward)<0.5f || std::abs(y)>GiveUp) {
      on = false;
      } else {
      const float maxYaw = headOnly ? MaxHeadYaw : MaxHeadYaw+MaxChestYaw+MaxWaistYaw;
      yawGoal   = std::clamp(y, -maxYaw, maxYaw);
      pitchGoal = std::clamp(std::atan2(d.y,length(dh)), -MaxPitchDown, MaxPitchUp);
      yaw       = std::clamp(yaw, -maxYaw, maxYaw);
      }
    }
  // the eyes go to another place: to the partner, away from it, or the partner is elsewhere at once
  if(on!=tracking || (on && std::abs(yawGoal-lastGoal)>ShiftJump))
    ++shifts;
  lastGoal = yawGoal;
  tracking = on;

  // the animation bends the head up and down too (down to a book): follow its slow part
  const float prevAp = animPitchLp;
  animPitchLp += (animPitch-animPitchLp)*(1.f - std::exp(-dt/AnimLag));
  if(weight>0.f && std::abs(animPitchLp-prevAp)>Snap*0.1f)
    changed = true;

  const float prevW = weight;
  const float angle = std::max(std::abs(on && prevW<=0.f ? yawGoal : yaw), Snap);
  const float dw    = std::min(dt/BlendTime, MaxTurnSpeed*dt/angle);
  if(on)
    weight = std::min(1.f, weight + dw); else
    weight = std::max(0.f, weight - dw);
  if(weight!=prevW)
    changed = true;
  if(weight<=0.f) {
    yaw    = 0;
    pitch  = 0;
    source = Source::None;
    return changed;
    }
  if(prevW<=0.f) {
    // turning to the partner: the weight fades the turn in
    yaw   = yawGoal;
    pitch = pitchGoal;
    return true;
    }
  if(on) {
    const float k = 1.f - std::exp(-TurnRate*dt);
    approach(yaw,  yawGoal,  k,MaxTrackSpeed*dt,changed);
    approach(pitch,pitchGoal,k,MaxTrackSpeed*dt,changed);
    }
  return changed;
  }

void Gaze::apply(Matrix4x4* tr, size_t numBones) {
  if(!valid || head>=numBones || (!headTree.empty() && headTree.back()>=numBones) ||
     (!chestTree.empty() && chestTree.back()>=numBones) || (!waistTree.empty() && waistTree.back()>=numBones))
    return;
  headPos = position(tr[head]);
  hips    = {};
  if(lthigh<numBones && rthigh<numBones) {
    const Vec3 f = hipsForward(position(tr[lthigh]),position(tr[rthigh]));
    if(length(f)>1.f)
      hips = normalize(f);
    }

  // without a bind pose to tell: the axis of the head bone closest to the body facing, as animated;
  // kept while the animation turns the head far (into a turn), another axis would come closer then
  if(!faceFixed && length(forward)>0.5f) {
    float best = 0;
    int   ax   = -1;
    for(int k=0; k<3; ++k) {
      const float c = dot(axis(tr[head],k),forward);
      if(std::abs(c)>std::abs(best)) {
        best = c;
        ax   = k;
        }
      }
    if(faceAxis<0 || std::abs(best)>FaceAxisCos) {
      faceAxis = ax;
      faceSign = best>0 ? 1.f : -1.f;
      }
    }

  // the facing of the head as animated, from the body facing
  if(faceAxis>=0 && length(forward)>0.5f) {
    const Vec3 fa = axis(tr[head],faceAxis)*faceSign;
    const Vec3 fh = Vec3(fa.x,0,fa.z);
    animYaw   = std::abs(fa.y)<MaxFaceDown ? yawBetween(forward,normalize(fh)) : 0.f;
    animPitch = std::asin(std::clamp(fa.y,-1.f,1.f));
    }

  parts[0] = parts[1] = parts[2] = 0;
  error    = -1;
  if(weight>0.f) {
    const float y = yaw*weight;
    const float p = pitch*weight;
    // the head takes out what the animation turned it by, so it looks at the partner
    const float ay = std::clamp(animYaw,    -MaxAnimYaw,  MaxAnimYaw  )*weight;
    const float ap = std::clamp(animPitchLp,-MaxAnimPitch,MaxAnimPitch)*weight;
    // the head turns first, the chest and the waist take what is left (not with the head only)
    float hy = std::clamp(y*(headOnly || chest==size_t(-1) ? 1.f : HeadShare), -MaxHeadYaw, MaxHeadYaw);
    float cy = 0, wy = 0;
    if(!headOnly && chest!=size_t(-1)) {
      const float rest = y - hy;
      cy = std::clamp(rest*(waist==size_t(-1) ? 1.f : 0.55f), -MaxChestYaw, MaxChestYaw);
      wy = waist==size_t(-1) ? 0.f : std::clamp(rest-cy, -MaxWaistYaw, MaxWaistYaw);
      hy = std::clamp(y-cy-wy, -MaxHeadYaw, MaxHeadYaw);
      }
    if(wy!=0.f)
      rotateAbout(tr,waistTree,axisAngle(Up,wy),position(tr[waist]));
    if(cy!=0.f)
      rotateAbout(tr,chestTree,axisAngle(Up,cy),position(tr[chest]));
    hy -= ay;
    if(hy!=0.f)
      rotateAbout(tr,headTree, axisAngle(Up,hy),position(tr[head]));

    // up and down around the line through the shoulders, turned with the body
    const Vec3  f   = mul(axisAngle(Up,y),forward);
    const Vec3  lat = normalize(cross(f,Up));
    const float cp  = (headOnly || chest==size_t(-1)) ? 0.f : p*ChestPitch;
    const float hp  = p - cp - ap;
    if(cp!=0.f)
      rotateAbout(tr,chestTree,axisAngle(lat,cp),position(tr[chest]));
    if(hp!=0.f)
      rotateAbout(tr,headTree, axisAngle(lat,hp),position(tr[head]));

    parts[0] = hy;
    parts[1] = cy;
    parts[2] = wy;
    }

  // the head moving with the voice (TalkGestures): around the neck, after the turn to the partner
  if(talk.x!=0.f || talk.y!=0.f || talk.z!=0.f) {
    const Vec3 body = length(forward)>0.5f ? forward : npcForward;
    if(length(body)>0.5f) {
      const Vec3 f   = mul(axisAngle(Up,yaw*weight),body);
      const Vec3 lat = normalize(cross(f,Up));
      const Vec3 at  = position(tr[head]);
      if(talk.y!=0.f)
        rotateAbout(tr,headTree,axisAngle(Up,talk.y),at);
      if(talk.x!=0.f)
        rotateAbout(tr,headTree,axisAngle(lat,talk.x),at);
      if(talk.z!=0.f)
        rotateAbout(tr,headTree,axisAngle(f,talk.z),at);
      }
    }

  eyesKnown = eyes;
  if(eyes) {
    eyesPos = eyesAt;
    tr[head].project(eyesPos);
    }
  if(faceAxis>=0) {
    facing = axis(tr[head],faceAxis)*faceSign;
    const Vec3 d = normalize(target - (eyesKnown ? eyesPos : position(tr[head])));
    if(weight>0.f && length(d)>0.5f)
      error = std::acos(std::clamp(dot(facing,d),-1.f,1.f))/Deg;
    }
  }

void Gaze::probe(Probe& p) const {
  p.valid     = valid;
  p.active    = weight>0.f;
  p.source    = source;
  p.weight    = weight;
  p.yaw       = yaw/Deg;
  p.pitch     = pitch/Deg;
  p.yawGoal   = yawGoal/Deg;
  p.pitchGoal = pitchGoal/Deg;
  p.error     = error;
  p.headYaw   = parts[0]/Deg;
  p.chestYaw  = parts[1]/Deg;
  p.waistYaw  = parts[2]/Deg;
  p.animYaw   = animYaw/Deg;
  p.animPitch = animPitch/Deg;
  p.hipsYaw   = length(hips)>0.5f && length(npcForward)>0.5f ? yawBetween(npcForward,hips)/Deg : 0.f;
  p.head      = headPos;
  p.facing    = facing;
  p.target    = target;
  }
