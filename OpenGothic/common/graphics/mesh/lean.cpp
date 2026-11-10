#include "lean.h"

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
constexpr float Gravity    = 981.f;     // cm/s^2
constexpr float RollShare  = 0.6f;      // of the lean the physics asks for (v*w/g): a game runner leans less
constexpr float MaxRoll    = 14.f*Deg;
constexpr float RollFrom   = 150.f;     // cm/s: walking does not lean into a turn...
constexpr float RollFull   = 260.f;     // ...running does
constexpr float PitchUp    = 25.f*Deg;  // trunk forward per unit of slope (rise over run) uphill...
constexpr float PitchDown  = 12.f*Deg;  // ...and back downhill
constexpr float MaxPitchUp = 14.f*Deg;
constexpr float MaxPitchDown = 6.f*Deg;
constexpr float PitchFull  = 300.f;     // cm/s: slower leans less on a slope
constexpr float SpeedLag   = 0.15f;     // seconds
constexpr float TurnLag    = 0.05f;     // the turn is taken nearly as it is: the lean smooths itself
constexpr float SlopeLag   = 0.35f;
constexpr float LeanLag    = 0.15f;
constexpr float MaxLeanSpeed = 60.f*Deg; // per second
constexpr float MaxSpeed   = 1500.f;    // cm/s: moved faster - a teleport
constexpr float MinMove    = 20.f;      // cm/s: slower the slope is not measured
constexpr float Snap       = 0.0005f;

bool enabled = []() {
  const char* v = std::getenv("OPENGOTHIC_LEAN");
  return v==nullptr || v[0]!='0';
  }();

float lag(float dt, float tau) {
  return 1.f - std::exp(-dt/tau);
  }

// v toward target: smoothed, not faster than maxRate per second
void approach(float& v, float target, float dt, bool& changed) {
  float nv = v + std::clamp((target-v)*lag(dt,LeanLag), -MaxLeanSpeed*dt, MaxLeanSpeed*dt);
  if(std::abs(target-nv)<Snap)
    nv = target;
  if(nv!=v)
    changed = true;
  v = nv;
  }

}

Lean::Probe Lean::playerProbe;
Lean::Probe Lean::subjectProbe;
const void* Lean::probeNpc = nullptr;

bool Lean::isEnabled() {
  return enabled;
  }

void Lean::setEnabled(bool e) {
  enabled = e;
  Log::i("lean: ", e ? "on" : "off");
  }

void Lean::setSkeleton(const Skeleton* sk) {
  *this = Lean();
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
  if(spine==size_t(-1))
    spine = find("BIP01 SPINE1");
  lfoot = find("BIP01 L FOOT");
  rfoot = find("BIP01 R FOOT");
  if(spine==size_t(-1) || lfoot==size_t(-1) || rfoot==size_t(-1))
    return;
  tree(spine,spineTree);
  tree(lfoot,lfootTree);
  tree(rfoot,rfootTree);
  valid = true;
  }

bool Lean::update(const Input& in) {
  if(!valid)
    return false;

  const float dt      = float(in.dt)/1000.f;
  const Vec3  f       = normalize(Vec3(in.forward.x,0,in.forward.z));
  // the body's way is followed while it stands too: a stop brakes after the run's state has ended
  const bool  track   = (in.active || in.grounded) && enabled && dt>0.f && length(f)>0.5f;
  const bool  on      = in.active && track;
  bool        changed = false;

  if(track) {
    const Vec3  d     = in.position - prevPos;
    const float dh    = std::sqrt(d.x*d.x + d.z*d.z);
    const bool  jump  = !moving || dh>MaxSpeed*dt;
    if(jump) {
      // the first step, or a teleport: nothing to measure yet
      speed = 0;
      turn  = 0;
      LeanAccel::reset(accel);
      } else {
      speed += (dh/dt - speed)*lag(dt,SpeedLag);
      LeanAccel::step(accel,dot(Vec3(d.x,0,d.z),f)/dt,dt);
      if(on) {
        const Vec3  left = normalize(cross(forward,Up));
        const float w    = dot(f-forward,left)/dt;
        turn  += (w - turn)*lag(dt,TurnLag);
        if(dh>MinMove*dt) {
          // along the facing: walking backwards up a slope leans back
          const float s = d.y/dh * (dot(Vec3(d.x,0,d.z),f)<0 ? -1.f : 1.f);
          slope += (std::clamp(s,-1.5f,1.5f) - slope)*lag(dt,SlopeLag);
          }
        } else {
        turn  = 0;
        slope = 0;
        }
      }
    prevPos = in.position;
    forward = f;
    moving  = true;
    } else {
    moving = false;
    speed  = 0;
    turn   = 0;
    slope  = 0;
    LeanAccel::reset(accel);
    }

  float rollGoal = 0, pitchGoal = 0;
  if(on) {
    const float v     = speed;
    const float share = std::clamp((v-RollFrom)/(RollFull-RollFrom), 0.f, 1.f)*RollShare;
    rollGoal  = std::clamp(std::atan(v*std::abs(turn)/Gravity)*share, 0.f, MaxRoll)*(turn<0 ? -1.f : 1.f);
    const float k = std::clamp(v/PitchFull, 0.f, 1.f);
    pitchGoal = slope>0 ? std::min(slope*PitchUp, MaxPitchUp)*k : std::max(slope*PitchDown, -MaxPitchDown)*k;
    }
  if(track && LeanAccel::enabled())
    pitchGoal += LeanAccel::pitch(accel);
  approach(roll, rollGoal, dt,changed);
  approach(pitch,pitchGoal,dt,changed);
  return changed;
  }

void Lean::apply(Matrix4x4* tr, size_t numBones, const Vec3& ground) {
  if(!valid || (roll==0.f && pitch==0.f) || spineTree.back()>=numBones ||
     lfootTree.back()>=numBones || rfootTree.back()>=numBones)
    return;
  // the trunk leans forward or back around its base
  if(pitch!=0.f)
    rotateAbout(tr,spineTree,axisAngle(normalize(cross(Up,forward)),pitch),position(tr[spine]));
  if(roll!=0.f) {
    // the whole body leans into the turn around the ground under it; the feet stay flat
    const Vec3 axis = normalize(cross(Up,cross(forward,Up)));
    rotateAll(tr,numBones,axisAngle(axis,roll),ground);
    rotateAbout(tr,lfootTree,axisAngle(axis,-roll),position(tr[lfoot]));
    rotateAbout(tr,rfootTree,axisAngle(axis,-roll),position(tr[rfoot]));
    }
  }

void Lean::probe(Probe& p) const {
  p.valid  = valid;
  p.active = roll!=0.f || pitch!=0.f;
  p.roll   = roll/Deg;
  p.pitch  = pitch/Deg;
  p.speed  = speed;
  p.turn   = turn/Deg;
  p.slope  = std::atan(slope)/Deg;
  p.accel  = accel.accel;
  }
