#include "aim.h"

#include <Tempest/Log>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string_view>

#include "ikmath.h"
#include "aimframe.h"
#include "skeleton.h"
#include "physics/dynamicworld.h"

using namespace Tempest;
using namespace IkMath;

namespace {

constexpr float Deg         = float(M_PI)/180.f;
constexpr float MaxTurn     = 60.f*Deg;  // the upper body turns at most that far to the target
constexpr float WaistShare  = 0.45f;     // part of the turn the waist does, the chest the rest
constexpr float TrackRate   = 16.f;      // 1/s, how fast the aim follows the target...
constexpr float MaxTrack    = 240.f*Deg; // ...but not faster than this, per second
constexpr float BodyRate    = 16.f;      // 1/s, how fast the upper body takes a new turn
constexpr float MaxBody     = 180.f*Deg; // per second
constexpr float HandRate    = 3.f;       // aimed by hand: all of it that many times faster (the mouse is
                                         // smooth already, the crosshair has to keep up)
constexpr float SteadyAngle = 1.5f*Deg;  // the arrow moved less than this in a frame...
constexpr int   SteadyFrames= 4;         // ...that many frames: the bow holds still
constexpr float LeanRate    = 6.f;       // 1/s, leaning over an edge...
constexpr float MaxLeanRate = 90.f*Deg;  // ...not faster than this, per second
constexpr float BlendIn     = 0.2f;      // seconds: the upper body turns to the target...
constexpr float BlendOut    = 0.25f;     // ...and back when the bow goes down
constexpr float Snap        = 0.0005f;

bool enabled = []() {
  const char* v = std::getenv("OPENGOTHIC_AIM");
  return v==nullptr || v[0]!='0';
  }();

// angle around Up that turns horizontal unit vector a toward b
float yawBetween(const Vec3& a, const Vec3& b) {
  return std::atan2(dot(cross(Up,a),b), dot(a,b));
  }

float wrap(float a) {
  return std::remainder(a, 2.f*float(M_PI));
  }

// the body's frame: forward, up, side
Vec3 toBody(const Vec3& v, const Vec3& f) {
  return Vec3(dot(v,f), v.y, dot(v,cross(Up,f)));
  }

Vec3 fromBody(const Vec3& v, const Vec3& f) {
  return f*v.x + Up*v.y + cross(Up,f)*v.z;
  }

// bending forward toward where the shot goes (horizontally; straight down: where the body faces)
Vec3 leanDir(const Vec3& want, const Vec3& forward, bool manual) {
  const Vec3 horizontal(want.x,0,want.z);
  return normalize((manual ? normalize(horizontal) : horizontal) + forward*0.25f);
  }

// leaning out sideways (+ right): the trunk tilts about where the body faces
Mat3 sideRotation(const Vec3& forward, float angle) {
  return axisAngle(forward,-angle);
  }

Mat3 leanRotation(const Vec3& want, const Vec3& forward, float angle, bool manual) {
  if(angle==0.f)
    return axisAngle(Up,0.f);
  return axisAngle(normalize(cross(Up,leanDir(want,forward,manual))),angle);
  }

Mat3 turnRotation(const Vec3& turn, float share = 1.f) {
  const float angle = length(turn);
  return angle>1e-6f ? axisAngle(turn*(1.f/angle),angle*share) : axisAngle(Up,0.f);
  }

Vec3 torsoTurn(Vec3 turn, float lean, bool arms) {
  // A deep hip bend must carry the head forward. The arms take the upward correction
  // that would otherwise straighten the trunk again while still pointing the arrow down.
  if(arms && turn.z<0.f)
    turn.z *= 1.f-std::clamp(lean/(20.f*Deg),0.f,1.f);
  return turn;
  }

Vec3 counterweight(const Vec3& want, const Vec3& forward, float angle) {
  return leanDir(want,forward,true)*(-Aim::PelvisBack*std::clamp(angle/(Aim::LeanMax*Deg),0.f,1.f));
  }

// Axis times angle, in world space. Shared by the pose and its prospective head positions.
Vec3 bodyTurn(const Vec3& rest, const Vec3& want, const Vec3& hipDirection,
              const Vec3& forward, float lean, float side, bool manual) {
  const Vec3 ar   = mul(sideRotation(forward,side),mul(leanRotation(hipDirection,forward,lean,manual),rest));
  const Vec3 axis = cross(ar,want);
  const float s   = length(axis);
  const float an  = std::min(std::atan2(s,dot(ar,want)),MaxTurn);
  return s>1e-4f ? axis*(an/s) : Vec3();
  }

Vec3 rotatedPoint(const Vec3& p, const Mat3& r, const Vec3& pivot) {
  return pivot + mul(r,p-pivot);
  }

void approach(Vec3& v, const Vec3& target, float k, float maxStep, bool& changed) {
  Vec3 d = (target-v)*k;
  if(length(d)>maxStep)
    d = d*(maxStep/length(d));
  if(length(target-v-d)<Snap)
    d = target-v;
  if(length(d)>0.f)
    changed = true;
  v += d;
  }

void approach(float& v, float target, float k, float maxStep, bool& changed) {
  float nv = v + std::clamp(wrap(target-v)*k, -maxStep, maxStep);
  if(std::abs(wrap(target-nv))<Snap)
    nv = target;
  if(nv!=v)
    changed = true;
  v = nv;
  }

}

Aim::Probe Aim::playerProbe;
Aim::Probe Aim::subjectProbe;
const void* Aim::probeNpc = nullptr;
float Aim::playerLeanOverride = -1.f;

float Aim::leanFor(float down) {
  const float k = std::clamp((down/Deg - LeanFrom)/(LeanFull - LeanFrom), 0.f, 1.f);
  return k*LeanMax*Deg;
  }

float Aim::leanForNpc(float down) {
  return leanFor(down)*(NpcLeanMax/LeanMax);
  }

Vec3 Aim::pelvisOffset() const {
  if(!manualMode)
    return {};
  return counterweight(leanForward,forward,leanAmt*weight);
  }

Vec3 Aim::pointAt(const Vec3& point, const Vec3& origin, const Vec3& facing, const Vec3& dir,
                  float angle, bool shot, bool inverse) const {
  Vec3 f = normalize(Vec3(facing.x,0,facing.z));
  if(length(f)<0.5f)
    f = forward;
  Vec3 want = normalize(dir);
  if(length(want)<0.5f)
    want = f;
  angle = std::clamp(angle,0.f,LeanMax*Deg);
  const Vec3 offset = origin + (poseManual ? counterweight(want,f,angle) : Vec3());
  const Vec3 sp = offset + fromBody(baseSpine,f);
  Vec3 wa = offset + fromBody(baseWaist,f);
  Vec3 ch = offset + fromBody(baseChest,f);
  const Mat3 hip = mul(sideRotation(f,poseSide),leanRotation(want,f,angle,poseManual));
  wa = rotatedPoint(wa,hip,sp);
  ch = rotatedPoint(ch,hip,sp);
  const Vec3 turn = fromBody(torsoTurn(poseRotation,angle,poseManual && !armsTree.empty()),f);
  const Mat3 wr = turnRotation(turn,WaistShare);
  const Mat3 cr = turnRotation(turn,1.f-WaistShare);
  ch = rotatedPoint(ch,wr,wa);
  const Mat3 arms = mul(turnRotation(fromBody(poseRotation,f)),transpose(turnRotation(turn)));
  Vec3 right = cross(fromBody(poseShotDirectionB,f),Up);
  right = length(right)>1e-3f ? normalize(right) : normalize(cross(f,Up));
  const Mat3 sw = mul(axisAngle(Up,poseSway.x),axisAngle(right,poseSway.y));
  const Mat3 tm = mul(axisAngle(Up,poseTremor.x),axisAngle(right,poseTremor.y));
  const bool armPoint = shot && slotInArms;
  if(inverse) {
    Vec3 p = point;
    if(armPoint)
      p = rotatedPoint(p,transpose(tm),ch);
    p = rotatedPoint(p,transpose(sw),ch);
    if(armPoint)
      p = rotatedPoint(p,transpose(arms),ch);
    p = rotatedPoint(p,transpose(cr),ch);
    p = rotatedPoint(p,transpose(wr),wa);
    p = rotatedPoint(p,transpose(hip),sp);
    return toBody(p-offset,f);
    }
  Vec3 p = offset + fromBody(point,f);
  p = rotatedPoint(p,hip,sp);
  p = rotatedPoint(p,wr,wa);
  p = rotatedPoint(p,cr,ch);
  if(armPoint)
    p = rotatedPoint(p,arms,ch);
  p = rotatedPoint(p,sw,ch);
  if(armPoint)
    p = rotatedPoint(p,tm,ch);
  return p;
  }

Vec3 Aim::headAt(const Vec3& origin, const Vec3& facing, const Vec3& dir, float angle) const {
  return pointAt(baseHead,origin,facing,dir,angle,false);
  }

Vec3 Aim::arrowAt(const Vec3& origin, const Vec3& facing, const Vec3& dir, float angle) const {
  return pointAt(baseSlot,origin,facing,dir,angle,true);
  }

Vec3 Aim::appliedDirection() const {
  return poseLeanDirection;
  }

Aim::LeanResult Aim::leanNeeded(const DynamicWorld* physics, const Tempest::Vec3& origin, float rotation, const Vec3& lookDirection,
                                const Vec3& shotDirection, float down, const LeanEdge& edge) const {
  LeanResult out;
  out.edgeDistance = edge.distance;
  out.edgeDrop = edge.drop;
  out.edgeHeight = edge.supportY;
  if(!valid || !enabled)
    return out;
  const float a = rotation;
  const Vec3 facing(std::cos(a),0,std::sin(a));
  const Vec3 look = length(lookDirection)>0.5f ? normalize(lookDirection) : facing;
  const Vec3 shot = length(shotDirection)>0.5f ? normalize(shotDirection) : look;
  const auto* world = physics;
  out.edge = edge.found && edge.distance<=100.f && edge.drop>100.f && down>LeanStart*Deg;

  struct Clearance final {
    bool head = false, shot = false;
    float headHit = -1.f, shotHit = -1.f;
    };
  auto rayClear = [&](const Vec3& from, const Vec3& direction, float& hitDistance) {
    const auto r = world->ray(from,from+direction*200.f);
    hitDistance = r.hasCol ? length(r.v-from) : -1.f;
    // A roof slope just below the feet is still the overhang. Only a hit at least the
    // dangerous-drop threshold below its supporting surface counts as ground below.
    return !r.hasCol || hitDistance>=200.f || (out.edge && r.v.y<edge.supportY-100.f);
    };
  auto clearance = [&](float angle) {
    Clearance c;
    c.head = rayClear(headAt(origin,facing,look,angle),look,c.headHit);
    c.shot = rayClear(arrowAt(origin,facing,look,angle),shot,c.shotHit);
    return c;
    };
  auto record = [&](const Clearance& c) {
    out.headClear = c.head;
    out.shotClear = c.shot;
    out.visible = c.head && c.shot;
    out.headHitDistance = c.headHit;
    out.shotHitDistance = c.shotHit;
    };
  auto blockingLine = [](const Clearance& c) {
    return !c.shot ? LeanLine::Shot : (!c.head ? LeanLine::Head : LeanLine::None);
    };

  Clearance current = clearance(0.f);
  out.hitDistance = current.headHit;
  record(current);
  if(out.edge && out.visible)
    return out;

  // Away from a dangerous edge retain the old 55-degree angle rule. An edge has a
  // deterministic maximum fallback even while the camera or animated hands are settling.
  const float limit = out.edge ? LeanMax*Deg : leanForNpc(down);
  if(limit<=0.f)
    return out;
  Vec3 previous = headAt(origin,facing,look,0.f);
  float safe = 0.f;
  LeanLine limiter = blockingLine(current);
  for(float angle=std::min(Deg,limit);; angle=std::min(angle+Deg,limit)) {
    const Vec3 next = headAt(origin,facing,look,angle);
    if(world->ray(previous,next).hasCol) {
      out.room = false;
      out.roomLimited = true;
      break;
      }
    current = clearance(angle);
    if(out.edge && current.head && current.shot) {
      float lo = safe, hi = angle;
      for(int i=0; i<5; ++i) {
        const float mid = (lo+hi)*0.5f;
        const Clearance c = clearance(mid);
        if(c.head && c.shot && !world->ray(previous,headAt(origin,facing,look,mid)).hasCol)
          hi = mid;
        else {
          lo = mid;
          limiter = blockingLine(c);
          }
        }
      // A checked margin keeps animation sampling from closing the two directional rays
      // again at the exact silhouette of the overhang.
      const float margin = std::min(hi+0.5f*Deg,limit);
      const Clearance c = clearance(margin);
      if(c.head && c.shot &&
         !world->ray(headAt(origin,facing,look,hi),headAt(origin,facing,look,margin)).hasCol)
        hi = margin;
      out.angle = hi;
      out.line = limiter;
      record(clearance(hi));
      return out;
      }
    safe = angle;
    previous = next;
    if(out.edge)
      limiter = blockingLine(current);
    if(angle>=limit)
      break;
    }
  out.angle = safe;
  record(clearance(safe));
  out.limited = out.roomLimited || (out.edge && !out.visible);
  out.line = out.edge && !out.roomLimited ? limiter : LeanLine::None;
  return out;
  }

bool Aim::isEnabled() {
  return enabled;
  }

void Aim::setEnabled(bool e) {
  enabled = e;
  Log::i("aim: ", e ? "on" : "off");
  }

void Aim::setSkeleton(const Skeleton* sk) {
  *this = Aim();
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

  spine = find("BIP01 SPINE");
  waist = find("BIP01 SPINE1");
  chest = find("BIP01 SPINE2");
  slot  = find("ZS_RIGHTHAND"); // the arrow, the bolt, the spell sit there
  head  = find("BIP01 HEAD");
  if(waist==size_t(-1) || chest==size_t(-1) || slot==size_t(-1) || !isUnder(chest,waist) || !isUnder(slot,chest))
    return;
  if(spine==size_t(-1) || !isUnder(waist,spine))
    spine = waist;
  if(head==size_t(-1) || !isUnder(head,chest))
    return;
  // A real skeleton supplies a usable first query even before the first animated pose.
  const Vec3 modelForward(0,0,1);
  baseSpine = toBody(position(sk->tr[spine]),modelForward);
  baseWaist = toBody(position(sk->tr[waist]),modelForward);
  baseChest = toBody(position(sk->tr[chest]),modelForward);
  baseHead  = toBody(position(sk->tr[head]),modelForward);
  baseSlot  = toBody(position(sk->tr[slot]),modelForward);
  tree(spine,spineTree);
  tree(waist,waistTree);
  tree(chest,chestTree);
  for(auto side:{"BIP01 L","BIP01 R"}) {
    size_t arm = find(std::string(side)+" CLAVICLE");
    if(arm==size_t(-1))
      arm = find(std::string(side)+" UPPERARM");
    if(arm!=size_t(-1) && isUnder(arm,chest))
      tree(arm,armsTree);
    }
  slotInArms = std::find(armsTree.begin(),armsTree.end(),slot)!=armsTree.end();
  valid = true;
  }

bool Aim::update(const Input& in) {
  if(!valid)
    return false;

  const float dt      = float(in.dt)/1000.f;
  bool        changed = false;
  const Vec3  f       = normalize(Vec3(in.forward.x,0,in.forward.z));
  Vec3        direction = in.dir;
  if(in.manual && in.active && length(in.referenceDir)>0.5f) {
    if(!in.reload && !in.holdOrigin) {
      nockedOffsetB = toBody(in.referenceDir-in.dir,f);
      nockedOrigin = true;
      } else if(nockedOrigin) {
      // The arrow travels round the head to the quiver during reload. Its moving origin
      // must not steer the torso at a pole; retain the last nocked origin in the body frame.
      // Both vectors still follow the target and the hero's translation/turn.
      const Vec3 held = in.referenceDir-fromBody(nockedOffsetB,f);
      const float k = AimFrame::poleWeight(normalize(held));
      direction += (held-direction)*k;
      }
    } else {
    nockedOrigin = false;
    }
  const Vec3  d       = normalize(direction);
  const bool  on      = in.active && enabled && length(f)>0.5f && length(d)>0.5f;
  const float fast    = (in.manual ? HandRate : 1.f)*in.follow;
  if(manualMode!=in.manual)
    changed = true;
  manualMode = in.manual;
  // The pose is captured even with the bow lowered, so its frame must follow body turns then too.
  if(length(f)>0.5f)
    forward = f;
  const Vec3 hipDirection = manualMode ?
      (on ? (length(in.leanDirection)>0.5f ? normalize(in.leanDirection) : d) : forward) :
      fromBody(wantB,forward);
  if(hipDirection!=leanForward)
    changed = true;
  leanForward = hipDirection;

  if(on) {
    forward = f;
    arrow   = in.arrow;
    if(arrowSlot!=(in.slot!=size_t(-1) ? in.slot : slot)) {
      // the arrow lies elsewhere now: measured anew
      arrowSlot = in.slot!=size_t(-1) ? in.slot : slot;
      measured  = false;
      }
    // a direction, not yaw and pitch: straight down the yaw means nothing
    const Vec3 db = toBody(d,forward);
    if(!ready) {
      wantB = db;
      } else {
      approach(wantB,db,1.f - std::exp(-TrackRate*fast*dt),MaxTrack*fast*dt,changed);
      wantB = normalize(wantB);
      }
    if(!manualMode)
      leanForward = fromBody(wantB,forward);

    // where the animation aims, measured while the bow holds still (raising it, reloading swing the
    // arrow around), kept in the body's frame; then the turn the upper body needs: the shortest one from
    // there to where the shot has to go, every frame (the target may change while reloading)
    const Vec3 a = arrow ? animArrow : forward;
    const bool still = !arrow || (!in.reload && measured && std::acos(std::clamp(dot(a,prevArrow),-1.f,1.f))<SteadyAngle);
    steadyFrames = still ? uint8_t(std::min(steadyFrames+1,255)) : 0;
    steady       = steadyFrames>=SteadyFrames || !arrow;
    if(steady)
      rest = toBody(a,forward);
    approach(leanAmt,in.lean,1.f - std::exp(-LeanRate*dt),MaxLeanRate*dt,changed);
    approach(sideAmt,in.side,1.f - std::exp(-LeanRate*dt),MaxLeanRate*dt,changed);
    if(steady || ready) {
      // where it aims once leaned over the edge, out sideways
      const Vec3  want = fromBody(wantB,forward);
      goal = toBody(bodyTurn(fromBody(rest,forward),want,leanForward,forward,leanAmt,sideAmt,manualMode),forward);
      if(!ready)
        rot = goal;
      ready = true;
      }
    } else {
    steady       = false;
    steadyFrames = 0;
    }
  prevArrow = animArrow;

  if(!on) {
    approach(leanAmt,0.f,1.f - std::exp(-LeanRate*dt),MaxLeanRate*dt,changed);
    approach(sideAmt,0.f,1.f - std::exp(-LeanRate*dt),MaxLeanRate*dt,changed);
    }
  // the sway as it is: the arms tremble faster than any smoothing would let through
  const Tempest::Vec2 sw = on ? in.sway : Tempest::Vec2();
  if(sw.x!=sway.x || sw.y!=sway.y)
    changed = true;
  sway = sw;
  const Tempest::Vec2 tm = on ? in.tremor : Tempest::Vec2();
  if(tm.x!=tremor.x || tm.y!=tremor.y)
    changed = true;
  tremor = tm;
  const Tempest::Vec2 sk = on ? in.shake : Tempest::Vec2();
  if(sk.x!=shake.x || sk.y!=shake.y)
    changed = true;
  shake = sk;
  if(ready)
    approach(rot,goal,1.f - std::exp(-BodyRate*fast*dt),MaxBody*fast*dt,changed);

  // the weight fades in once the turn is known, out when the bow goes down
  const float prevW = weight;
  if(on && ready)
    weight = std::min(1.f, weight + dt/BlendIn);
  else if(!on)
    weight = std::max(0.f, weight - dt/BlendOut);
  if(weight!=prevW)
    changed = true;
  if(weight<=0.f)
    ready = false; // the bow is down: measure anew next time
  return changed;
  }

Vec3 Aim::arrowOf(const Matrix4x4* tr) const {
  // the arrow (the bolt) lies along -X of its slot, the nock at the string
  const size_t s = arrowSlot!=size_t(-1) ? arrowSlot : slot;
  return normalize(Vec3(-tr[s].at(0,0),-tr[s].at(0,1),-tr[s].at(0,2)));
  }

void Aim::apply(Matrix4x4* tr, size_t numBones, const Vec3& origin, const Vec3& facing) {
  error = -1;
  if(!valid || spineTree.back()>=numBones || waistTree.back()>=numBones || chestTree.back()>=numBones || slot>=numBones || head>=numBones)
    return;
  const Vec3 f = normalize(Vec3(facing.x,0,facing.z));
  const Vec3 captureForward = length(f)>0.5f ? f : forward;
  const Vec3 baseline = origin + pelvisOffset();
  baseSpine = toBody(position(tr[spine])-baseline,captureForward);
  baseWaist = toBody(position(tr[waist])-baseline,captureForward);
  baseChest = toBody(position(tr[chest])-baseline,captureForward);
  baseHead  = toBody(position(tr[head])-baseline,captureForward);
  if(arrowSlot!=size_t(-1) && arrowSlot>=numBones)
    arrowSlot = size_t(-1);
  const size_t s = arrowSlot!=size_t(-1) ? arrowSlot : slot;
  baseSlot = toBody(position(tr[s])-baseline,captureForward);
  slotInArms = std::find(armsTree.begin(),armsTree.end(),s)!=armsTree.end();
  poseRotation = toBody(fromBody(rot*weight,forward),captureForward);
  poseManual = manualMode;
  poseLeanDirection = normalize(leanForward);
  if(length(poseLeanDirection)<0.5f)
    poseLeanDirection = captureForward;
  poseShotDirectionB = toBody(normalize(fromBody(wantB,forward)),captureForward);
  if(length(poseShotDirectionB)<0.5f)
    poseShotDirectionB = {1,0,0};
  poseLean = leanAmt*weight;
  poseSide = sideAmt*weight;
  poseSway = sway*weight;
  poseTremor = tremor*weight;
  animArrow = arrowOf(tr);
  measured  = true;
  if(!ready)
    rest = toBody(animArrow,captureForward);
  if(weight<=0.f)
    return;

  // over an edge the trunk bends forward from the hips toward the target first
  const Vec3  want = fromBody(wantB,forward);
  const float l    = leanAmt*weight;
  if(l>0.f)
    rotateAbout(tr,spineTree,leanRotation(leanForward,forward,l,poseManual),position(tr[spine]));
  // round a corner the trunk leans out sideways from the hips
  const float sd   = sideAmt*weight;
  if(sd!=0.f)
    rotateAbout(tr,spineTree,sideRotation(forward,sd),position(tr[spine]));

  // the upper body turns from where it aims to the target: the waist a part, the chest the rest
  const Vec3  r  = fromBody(torsoTurn(poseRotation,l,poseManual && !armsTree.empty()),captureForward);
  const float an = length(r);
  if(an>0.f) {
    const Vec3 axis = normalize(r);
    rotateAbout(tr,waistTree,axisAngle(axis,an*WaistShare),       position(tr[waist]));
    rotateAbout(tr,chestTree,axisAngle(axis,an*(1.f-WaistShare)), position(tr[chest]));
    }

  // Keep the complete shot rotation in the arms after removing its upward part from the trunk.
  // The product is the original full rotation; at zero lean the residual is the identity.
  const Mat3 fullTurn = turnRotation(fromBody(poseRotation,captureForward));
  const Mat3 armTurn = mul(fullTurn,transpose(turnRotation(r)));
  if(poseManual && !armsTree.empty())
    rotateAbout(tr,armsTree,armTurn,position(tr[chest]));

  const Vec3 now  = arrow ? arrowOf(tr) : mul(fullTurn,forward);
  error = std::acos(std::clamp(dot(now,want),-1.f,1.f))/Deg;

  // the manual aim sways off that: the chest turns about up and bends (+ up); the arms tremble more, the
  // head does not (both arms turn about the chest: the arrow and the bow turn together)
  Vec3 side = cross(want,Up);
  side = length(side)>1e-3f ? normalize(side) : normalize(cross(forward,Up));
  if(sway.x!=0.f || sway.y!=0.f) {
    const Mat3 rs = mul(axisAngle(Up,sway.x*weight),axisAngle(side,sway.y*weight));
    rotateAbout(tr,chestTree,rs,position(tr[chest]));
    }
  if(tremor.x!=0.f || tremor.y!=0.f) {
    const Mat3 rt = mul(axisAngle(Up,tremor.x*weight),axisAngle(side,tremor.y*weight));
    rotateAbout(tr,armsTree.empty() ? chestTree : armsTree,rt,position(tr[chest]));
    }
  // straining past their strength the arms shake where it is seen: both hands, the bow and the arrow move
  // together, across the arrow (the arrow keeps its line: the aim's error is the tremor)
  if((shake.x!=0.f || shake.y!=0.f) && !armsTree.empty()) {
    const Vec3 up = normalize(cross(side,want));
    const Vec3 d  = (side*shake.x + up*shake.y)*weight;
    for(auto i:armsTree) {
      tr[i].set(3,0,tr[i].at(3,0)+d.x);
      tr[i].set(3,1,tr[i].at(3,1)+d.y);
      tr[i].set(3,2,tr[i].at(3,2)+d.z);
      }
    }
  }

void Aim::finish(const Matrix4x4* tr, size_t numBones, const Vec3& origin, const Vec3& facing, const Vec3& seen) {
  const size_t s = arrowSlot!=size_t(-1) ? arrowSlot : slot;
  if(!valid || head>=numBones || s>=numBones)
    return;
  // BowDraw may slide the nock along the arrow. Bring the final points back through exactly
  // the shared transform, so every prospective angle starts from that same animated pose.
  baseHead = pointAt(position(tr[head]),origin,facing,poseLeanDirection,poseLean,false,true);
  baseSlot = pointAt(position(tr[s])-seen,origin,facing,poseLeanDirection,poseLean,true,true);
  }

void Aim::probe(Probe& p) const {
  const Vec3 a  = arrow ? animArrow : forward;
  const Vec3 ah = Vec3(a.x,0,a.z);
  p.valid     = valid;
  p.active    = weight>0.f;
  p.steady    = steady;
  p.weight    = weight;
  const Vec3 w  = fromBody(wantB,forward);
  const Vec3 wh = Vec3(w.x,0,w.z);
  p.yaw       = length(wh)>1e-3f ? yawBetween(forward,normalize(wh))/Deg : 0.f;
  p.pitch     = std::asin(std::clamp(w.y,-1.f,1.f))/Deg;
  p.animYaw   = length(ah)>0.05f ? yawBetween(forward,normalize(ah))/Deg : 0.f;
  p.animPitch = std::asin(std::clamp(a.y,-1.f,1.f))/Deg;
  p.turn      = rot.y*weight/Deg;   // about up, + to the right
  p.bend      = -rot.z*weight/Deg;  // about the side, + up
  p.lean      = leanAmt*weight/Deg;
  p.side      = sideAmt*weight/Deg;
  p.error     = error;
  }
