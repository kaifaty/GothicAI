#include "footik.h"
#include "aimwalk.h"

#include <Tempest/Log>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "game/constants.h"
#include "physics/dynamicworld.h"
#include "world/objects/npc.h"
#include "world/world.h"
#include "skeleton.h"
#include "ikmath.h"

using namespace Tempest;
using namespace IkMath;

namespace {

// Gothic units are centimeters; the parameters can be changed at runtime (FootIk::setParam)
float           MaxRaise       = 55.f;   // how far a foot may be lifted above the animation floor (a high step)
float           MaxDrop        = 45.f;   // how far the body may be lowered
float           MaxStep        = 50.f;   // larger height difference between the feet is a ledge, not a slope
float           MaxTilt        = 0.61f;  // radians (~35 degrees), foot tilt limit
float           MaxPitch       = 0.52f;  // radians (~30 degrees), toes up/down
float           MaxRoll        = 0.17f;  // radians (~10 degrees), sideways: an ankle hardly rolls more
float           OutwardFactor  = 0.35f;  // on a slope the legs turn outward by this part of its steepness
float           MaxOutward     = 0.21f;  // radians (~12 degrees)
float           MaxSpread      = 100.f;  // animated ankle further than this from the origin: pose is not valid yet
float           PlantLift      = 12.f;   // an ankle this much above the other one swings
float           SwingAllowance = 30.f;   // a swinging foot does not pull the body down
float           SwingHeight    = 30.f;   // a foot swung this high above the standing one goes with the body, not the ground
float           ToeOff         = 10.f;   // a planted foot behind the hip may stay this much higher, standing on the toes
float           MaxBodyRoll    = 0.35f;  // radians (~20 degrees): four legs lean the body sideways across a slope
float           MinPawFold     = 0.35f;  // four legs: a paw comes no closer to its hip than this part of the leg length
float           ToeOffDist     = 30.f;   // ... fully when it is this far behind the hip
float           PlantRate      = 200.f;  // 1/s, the ground under a planted foot hardly needs smoothing
float           SwingRate      = 25.f;   // 1/s, smoothing of the ground under a swinging foot
float           NormalRate     = 15.f;   // 1/s, smoothing of the foot tilt
float           PelvisRate     = 8.f;    // 1/s, smoothing of the body height
float           ReachRate      = 30.f;   // 1/s, how fast the body goes down when a planted foot can not reach
float           FloorJump      = 8.f;    // the floor moving this much faster in one frame is a step: the body stays put
float           LegReach       = 0.98f;  // fraction of the full leg length a leg may straighten to
float           BlendTime      = 0.25f;  // seconds, fade in/out of the whole effect
float           AimCrouch      = 28.f;   // cm the body goes down crouched to aim a bow (knees bent, feet planted)
constexpr float MinGroundNy    = 0.5f;   // steeper than 60 degrees is a wall, not ground
constexpr float RayMargin      = 10.f;   // ground rays start this much above MaxRaise
constexpr float QuadHandHeight = 0.35f;  // front paws lower than this part of the hip height: four legs
constexpr float QuadSlope      = 0.84f;  // tan(40 degrees): paws far from the middle of the body stand that much higher or lower
constexpr float HumanLeg       = 87.f;   // hip height of a human in the bind pose: the size the parameters are for
constexpr float Snap           = 0.01f;  // smoothing stops this close to the target
constexpr float SnapCos        = 1.f - 1e-6f;


struct Param final {
  std::string_view name;
  float*           value;
  };

const Param params[] = {
  {"MaxRaise",       &MaxRaise},       {"MaxDrop",        &MaxDrop},       {"AimCrouch",      &AimCrouch},
  {"MaxStep",        &MaxStep},        {"MaxTilt",        &MaxTilt},
  {"MaxPitch",       &MaxPitch},       {"MaxRoll",        &MaxRoll},
  {"OutwardFactor",  &OutwardFactor},  {"MaxOutward",     &MaxOutward},
  {"MaxSpread",      &MaxSpread},      {"PlantLift",      &PlantLift},
  {"SwingAllowance", &SwingAllowance}, {"SwingHeight",    &SwingHeight},
  {"ToeOff",         &ToeOff},         {"ToeOffDist",     &ToeOffDist},
  {"MaxBodyRoll",    &MaxBodyRoll},    {"MinPawFold",     &MinPawFold},
  {"PlantRate",      &PlantRate},      {"SwingRate",      &SwingRate},
  {"NormalRate",     &NormalRate},     {"PelvisRate",     &PelvisRate},
  {"ReachRate",      &ReachRate},      {"FloorJump",      &FloorJump},
  {"LegReach",       &LegReach},       {"BlendTime",      &BlendTime},
  };

// Hits on objects (not the landscape) have no normal: unknown is not a wall
bool isWall(const Vec3& n) {
  return n.y<MinGroundNy && (n.x*n.x + n.y*n.y + n.z*n.z)>0.25f;
  }

int envInt(const char* name, int def) {
  const char* v = std::getenv(name);
  if(v==nullptr || v[0]=='\0')
    return def;
  return std::atoi(v);
  }

bool enabled  = envInt("OPENGOTHIC_FOOTIK",     1)!=0;
int  logLevel = envInt("OPENGOTHIC_FOOTIK_LOG", 0); // 1: npc's on uneven ground once a second, 2: also the player every frame
bool probing  = false;

// Ground normal turned up and limited to MaxTilt
Vec3 footNormal(Vec3 n) {
  n = normalize(n);
  if(n.y<0)
    n = n*-1.f;
  if(length(n)<Eps)
    return Up;
  const float maxCos = std::cos(MaxTilt);
  if(n.y>=maxCos)
    return n;
  const Vec3  side   = normalize(Vec3(n.x,0,n.z));
  const float maxSin = std::sin(MaxTilt);
  return {side.x*maxSin, maxCos, side.z*maxSin};
  }

// debug: knee offset from the hip-ankle line
Vec3 kneeBend(const Matrix4x4* tr, size_t thigh, size_t calf, size_t foot) {
  const Vec3 hip  = position(tr[thigh]);
  const Vec3 leg  = normalize(position(tr[foot])-hip);
  const Vec3 knee = position(tr[calf]) - hip;
  return knee - leg*dot(knee,leg);
  }

// Ground normal limited in the frame of the foot: toes may go up and down more than the foot may roll
Vec3 footTilt(const Vec3& n, const Vec3& heading) {
  const Vec3  side  = cross(Up,heading);
  const float pitch = std::clamp(std::atan2(dot(n,heading),n.y), -MaxPitch, MaxPitch);
  const float roll  = std::clamp(std::atan2(dot(n,side),   n.y), -MaxRoll,  MaxRoll);
  return normalize(heading*std::tan(pitch) + side*std::tan(roll) + Up);
  }

// How much a leg turns outward on ground with normal n, walking along heading
float outwardTarget(const Vec3& n, const Vec3& heading) {
  const float pitch = std::abs(std::atan2(dot(n,heading),n.y));
  return std::min(pitch*OutwardFactor, MaxOutward);
  }

void approach(float& v, float target, float k, bool& changed) {
  float nv = v + (target-v)*k;
  if(std::abs(target-nv)<Snap)
    nv = target;
  if(nv!=v)
    changed = true;
  v = nv;
  }

}

bool FootIk::isEnabled() {
  return enabled;
  }

void FootIk::setEnabled(bool e) {
  enabled = e;
  Log::i("foot ik: ", e ? "on" : "off");
  }

float FootIk::aimCrouchDepth() {
  return AimCrouch;
  }

bool FootIk::setParam(std::string_view name, float value) {
  for(auto& p:params)
    if(p.name==name) {
      *p.value = value;
      return true;
      }
  return false;
  }

std::string FootIk::paramList() {
  std::string ret;
  for(auto& p:params) {
    char buf[64];
    std::snprintf(buf,sizeof(buf),"%s%.*s=%g",ret.empty() ? "" : " ",int(p.name.size()),p.name.data(),double(*p.value));
    ret += buf;
    }
  return ret;
  }

FootIk::Probe FootIk::playerProbe;

void FootIk::setProbing(bool p) {
  probing = p;
  if(!p)
    playerProbe = Probe();
  }

bool FootIk::isProbing() {
  return probing;
  }

void FootIk::setSkeleton(const Skeleton* sk) {
  skeleton  = sk;
  valid     = false;
  quadruped = false;
  numLegs   = 0;
  scale     = 1;
  spread    = MaxSpread;
  pelvis    = 0;
  chest     = 0;
  weight    = 0;
  for(auto& c:legs)
    c = Chain();
  if(sk==nullptr)
    return;

  static const std::string_view names[MaxLegs][4] = {
    {"BIP01 L THIGH",    "BIP01 L CALF",    "BIP01 L FOOT", "BIP01 L TOE0"   },
    {"BIP01 R THIGH",    "BIP01 R CALF",    "BIP01 R FOOT", "BIP01 R TOE0"   },
    {"BIP01 L UPPERARM", "BIP01 L FOREARM", "BIP01 L HAND", "BIP01 L FINGER0"},
    {"BIP01 R UPPERARM", "BIP01 R FOREARM", "BIP01 R HAND", "BIP01 R FINGER0"},
    };

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
  // a leg: the joint between the two bones can have more bones under it (horselink of a hind paw)
  auto  mkChain = [&](size_t l) {
    Chain& c = legs[l];
    c.front    = l>=FrontLeft;
    c.bendSign = c.front ? -1.f : 1.f;
    c.thigh    = find(names[l][0]);
    c.calf     = find(names[l][1]);
    c.foot     = find(names[l][2]);
    c.toe      = find(names[l][3]);
    if(c.thigh==size_t(-1) || c.calf==size_t(-1) || c.foot==size_t(-1))
      return false;
    if(!isUnder(c.calf,c.thigh) || !isUnder(c.foot,c.calf))
      return false;
    if(c.toe!=size_t(-1) && !isUnder(c.toe,c.foot))
      c.toe = size_t(-1);
    for(size_t i=0; i<nodes.size(); ++i) {
      if(isUnder(i,c.thigh))
        c.thighTree.push_back(i);
      if(isUnder(i,c.calf))
        c.calfTree.push_back(i);
      if(isUnder(i,c.foot))
        c.footTree.push_back(i);
      }
    return true;
    };

  if(!mkChain(Left) || !mkChain(Right))
    return;
  numLegs = 2;
  valid   = true;

  // Four legs: the front paws are as low as the hind feet in the bind pose, humanoids hold the hands high
  if(mkChain(FrontLeft) && mkChain(FrontRight) && sk->tr.size()==nodes.size()) {
    const float foot = sk->tr[legs[Left].foot].at(3,1);
    const float hip  = sk->tr[legs[Left].thigh].at(3,1) - foot;
    const float hand = sk->tr[legs[FrontLeft].foot].at(3,1) - foot;
    if(hip>Eps && hand<QuadHandHeight*hip) {
      quadruped = true;
      numLegs   = 4;
      }
    }
  if(!quadruped) {
    legs[FrontLeft]  = Chain();
    legs[FrontRight] = Chain();
    }

  // creatures of other sizes: a molerat steps lower than a human, a shadowbeast's paws are far apart
  if(sk->tr.size()==nodes.size()) {
    const float leg = sk->tr[legs[Left].thigh].at(3,1) - sk->tr[legs[Left].foot].at(3,1);
    if(leg>Eps)
      scale = std::clamp(leg/HumanLeg, 0.3f, 3.f);
    for(size_t i=0; i<numLegs; ++i) {
      const auto& t = sk->tr[legs[i].foot];
      spread = std::max(spread, 2.f*std::sqrt(t.at(3,0)*t.at(3,0) + t.at(3,2)*t.at(3,2)));
      }
    }
  }

FootIk::Input FootIk::mkInput(const Npc& npc, World& world, const Matrix4x4& obj,
                              bool alignedToGround, uint64_t dt, const AimWalk* walk) const {
  Input in;
  in.dt     = dt;
  in.floorY = obj.at(3,1);
  in.crouch = npc.aimCrouch()*AimCrouch*scale;
  // the test bench measures the ground with IK off too, and under a body tilted to the ground as
  // a whole (surface_align): that is the baseline
  if(!valid || (!enabled && !probing) || (alignedToGround && !probing))
    return in;
  if(npc.isDead() || npc.isUnconscious() || npc.interactive()!=nullptr)
    return in;
  if(npc.isInAir() || npc.isFalling() || npc.isSlide() || npc.isSwim()) {
    in.hardOff = true;
    return in;
    }

  const uint32_t bs = npc.bodyStateMasked() & BS_MAX;
  if(bs==(BS_JUMP & BS_MAX) || bs==(BS_FALL & BS_MAX) || bs==(BS_SWIM & BS_MAX) ||
     bs==(BS_DIVE & BS_MAX) || bs==(BS_CLIMB & BS_MAX)) {
    in.hardOff = true;
    return in;
    }
  // A melee roll goes over the ground on the body, off as in a jump; a dash fades out as the jump back does
  if(bs==(BS_PARADE & BS_MAX) && npc.isMeleeDodgeRoll()) {
    in.hardOff = true;
    return in;
    }
  // Two legs: standing, locomotion, aiming a bow (a shot is a frame of BS_HIT) and casting a spell, the
  // feet stand still there; other animations (attacks, items, ...) fade out.
  // Four legs keep it: a beast attacks standing on the ground, and is not tilted to it otherwise
  const auto ws    = npc.weaponState();
  const bool bow   = ws==WeaponState::Bow || ws==WeaponState::CBow;
  const bool stand = bs==BS_NONE || bs==BS_AIMNEAR || bs==BS_AIMFAR || bs==BS_CASTING || (bow && bs==(BS_HIT & BS_MAX));
  if(!quadruped && !stand && bs!=(BS_WALK & BS_MAX) && bs!=(BS_SNEAK & BS_MAX) && bs!=BS_RUN && bs!=BS_SPRINT)
    return in;

  const Vec3 pos = position(obj);
  if(!world.isInPfxRange(pos))
    return in;
  auto physic = world.physic();
  if(physic==nullptr)
    return in;

  for(size_t i=0; i<numLegs; ++i) {
    const Vec3 ankle = legs[i].animAnkle;
    if(std::abs(ankle.x-pos.x)>spread || std::abs(ankle.z-pos.z)>spread)
      return in;
    }

  for(size_t i=0; i<numLegs; ++i) {
    const Vec3 ankle = legs[i].animAnkle;
    // a bit above MaxRaise: running up a steep slope, the ground under the swinging foot is that
    // high and a ray starting right at MaxRaise misses it (higher ground is not used anyway).
    // Walls are not ground: running against a rock, the rays hit its side
    const float top    = in.floorY + MaxRaise + RayMargin + legs[i].reach;
    const float bottom = in.floorY - MaxDrop - legs[i].reach;
    auto r = physic->ray({ankle.x, top, ankle.z}, {ankle.x, bottom, ankle.z});
    in.ground[i].hasCol = r.hasCol && !isWall(r.n);
    in.ground[i].y      = r.v.y;
    in.ground[i].n      = r.n;
    if(legs[i].toe!=size_t(-1)) {
      const Vec3 toe = legs[i].animToe;
      auto t = physic->ray({toe.x, top, toe.z}, {toe.x, bottom, toe.z});
      in.ground[i].toeCol = t.hasCol && !isWall(t.n);
      in.ground[i].toeY   = t.v.y;
      }
    }
  if(walk!=nullptr && !quadruped) {
    float ground[2] = {};
    bool supported = true;
    for(size_t i=0; i<2; ++i) {
      Vec3 foot;
      if(!walk->stanceFoot(i,foot)) {
        supported = false;
        break;
        }
      auto ray = physic->ray({foot.x,in.floorY+MaxRaise+RayMargin,foot.z},
                             {foot.x,in.floorY-MaxDrop,foot.z});
      supported &= ray.hasCol && !isWall(ray.n);
      ground[i] = ray.v.y;
      }
    if(supported && std::abs(ground[0]-ground[1])<MaxStep) {
      in.hasStanceFloor = true;
      in.stanceFloor = std::min(ground[0],ground[1]);
      }
    }
  in.enable = enabled && !alignedToGround;
  return in;
  }

float FootIk::offset(const Chain& c, float floor) const {
  return std::clamp(c.groundY - floor, -MaxDrop-c.reach, MaxRaise+c.reach);
  }

// A foot swung high goes where the body carries it and is raised only as far as it has to be to stay
// above the ground (by the height of the standing ankle): following the whole ground under it lifts
// a foot kicked back over a downhill slope far too high. A foot near the ground follows the ground:
// short steps (walking backwards) would shuffle along the ground instead.
float FootIk::swingOffset(const Chain& c, float floor, float lowAnkle, float body) const {
  const float o      = offset(c,floor);
  const float h      = c.animAnkle.y-lowAnkle;
  const float lift   = std::max(body, o - h);
  const float follow = std::clamp(1.f - h/(SwingHeight*scale), 0.f, 1.f);
  return lift + (o-lift)*follow;
  }

bool FootIk::update(const Input& in) {
  if(!valid)
    return false;

  const float  dt    = float(in.dt)/1000.f;
  const size_t pairs = numLegs/2;
  bool         on    = in.enable;

  // of each pair, the lower animated ankle stands on the ground, a raised one swings
  float plant[MaxLegs] = {};
  for(size_t i=0; i<numLegs; ++i) {
    const float low = std::min(legs[i].animAnkle.y, legs[i^1].animAnkle.y);
    plant[i] = std::clamp(1.f - (legs[i].animAnkle.y-low)/(PlantLift*scale), 0.f, 1.f);
    }

  // ground under each foot; a foot without ground keeps its animated height
  bool  has[MaxLegs] = {};
  float gy [MaxLegs] = {};
  Vec3  n  [MaxLegs] = {};
  for(size_t i=0; i<numLegs; ++i) {
    gy[i] = in.floorY;
    n [i] = Up;
    if(!on || !in.ground[i].hasCol)
      continue;
    has[i] = true;
    gy [i] = in.ground[i].y;
    n  [i] = footNormal(in.ground[i].n);
    }
  for(size_t p=0; p<pairs; ++p) {
    const size_t l = 2*p, r = l+1;
    // the paws of a creature walking across a slope are that much higher on one side: a waran's
    // paws are 80 cm apart and a meter higher on the uphill side of a 40 degree slope
    float step = MaxStep;
    if(quadruped) {
      const Vec3 d = legs[l].animAnkle - legs[r].animAnkle;
      step += std::sqrt(d.x*d.x + d.z*d.z)*QuadSlope;
      }
    if(has[l] && has[r] && std::abs(gy[l]-gy[r])>step) {
      // the lower foot is over a ledge
      const size_t low = gy[l]<gy[r] ? l : r;
      has[low] = false;
      gy [low] = in.floorY;
      n  [low] = Up;
      }
    }
  bool any = false;
  for(size_t i=0; i<numLegs; ++i)
    any |= has[i];
  if(!any)
    on = false;
  // a swinging foot goes over the higher of the ground under the ankle and under the toes
  for(size_t i=0; i<numLegs; ++i) {
    if(has[i] && in.ground[i].toeCol && in.ground[i].toeY>gy[i] && in.ground[i].toeY-gy[i]<MaxStep)
      gy[i] += (in.ground[i].toeY-gy[i])*(1.f-plant[i]);
    }

  // The body follows the lower leg whichever foot stands on it: following only the planted
  // foot makes the body dip on every step of the lower leg (looks like limping).
  // For a swinging leg that is the ground under its hip: running up a slope, the ground under the
  // foot kicked back is far lower than where the foot lands, following it sinks the body by 30 cm
  // and more. Only along the way it goes, with the slope under the planted foot: across a slope
  // the ground under the swinging foot is right (its side stays lower)
  auto legHeight = [&](size_t i) {
    const size_t o = i^1;
    float        h = offset(legs[i],in.floorY);
    const Vec3   g = in.ground[o].n;
    if(plant[o]>plant[i] && has[o] && g.y>0.5f) {
      // the toes of a foot kicked back point anywhere, a lizard's toes point outward
      const Vec3  f     = quadruped ? forward : legs[o].heading;
      const Vec3  a     = legs[i].animAnkle;
      const Vec3  hip   = legs[i].animHip;
      const float along = (hip.x-a.x)*f.x + (hip.z-a.z)*f.z;
      // a big creature's paw is far from its hip: noise in the slope grows with it
      const float lift  = std::clamp(-(g.x*f.x + g.z*f.z)/g.y*along, -SwingAllowance*scale, SwingAllowance*scale);
      h += (std::clamp(h+lift, -MaxDrop-legs[i].reach, MaxRaise+legs[i].reach) - h)*(1.f-plant[i]);
      }
    return h;
    };
  // Each pair of legs lowers its end of the body: hips, and shoulders of a four-legged creature.
  // A creature on four legs is not tilted as a whole any more: its ends also go up, on a slope
  auto pairReach = [&](size_t p) { return std::max(legs[2*p].reach, legs[2*p+1].reach); };
  auto bodyTop    = [&](size_t p) { return quadruped ? MaxRaise+pairReach(p) : 0.f; };
  // crouched to aim: the hips go down on purpose (the legs bend to the planted feet)
  auto crouchOf   = [&](size_t p) { return p==0 && !quadruped ? in.crouch : 0.f; };
  auto bodyLow    = [&](size_t p) { return -MaxDrop-pairReach(p)-crouchOf(p); };
  // Four legs across a slope: the body leans toward the lower side (the uphill legs would fold up),
  // what is left over the legs take. Left legs are even: they go up by side(p), right ones down
  float rollTarget = 0;
  if(quadruped) {
    float sum = 0;
    int   cnt = 0;
    for(size_t p=0; p<pairs; ++p)
      if(pairWidth[p]>1.f) {
        sum += std::atan2(legHeight(2*p)-legHeight(2*p+1), pairWidth[p]);
        ++cnt;
        }
    if(cnt>0)
      rollTarget = std::clamp(sum/float(cnt), -MaxBodyRoll, MaxBodyRoll);
    }
  auto side = [&](size_t i) {
    const float s = std::tan(roll)*pairWidth[i/2]*0.5f;
    return (i%2==0) ? s : -s;
    };
  auto bodyTarget = [&](size_t p) {
    float h = std::min(legHeight(2*p)-side(2*p), legHeight(2*p+1)-side(2*p+1)) - crouchOf(p);
    if(p==0 && in.hasStanceFloor && !quadruped)
      h = in.stanceFloor-in.floorY-crouchOf(p);
    return std::clamp(h, bodyLow(p), bodyTop(p));
    };
  crouch = in.crouch;
  // A planted foot must reach the ground: the only reason to lower the body fast
  // (not at once: a sudden dip at every step of the lower leg looks like limping).
  // At the end of a step, behind the body, it may reach it with the toes only: running up a slope
  // the body would go down by 15 cm at every step for the heel to touch
  auto bodyLimit = [&](size_t p) {
    float h = bodyTop(p);
    for(size_t i=2*p; i<2*p+2; ++i)
      h = std::min(h, offset(legs[i],in.floorY) - side(i) + legs[i].slack + (1.f-legs[i].plant)*SwingAllowance*scale +
                      legs[i].plant*legs[i].behind*ToeOff*scale);
    return std::max(h, bodyLow(p));
    };
  // A step is a sudden change of how fast the floor moves, not a big move: running up a steep
  // slope at 30 fps the floor goes up by 8 cm and more every frame
  const float dFloor = in.floorY - lastFloor;
  const float jump   = dFloor - lastDFloor;
  lastFloor  = in.floorY;
  lastDFloor = std::abs(dFloor)<MaxStep ? dFloor : 0.f;

  bool        changed = false;
  const float prevW   = weight;
  if(in.hardOff)
    weight = 0;
  else if(on)
    weight = std::min(1.f, weight + dt/BlendTime);
  else
    weight = std::max(0.f, weight - dt/BlendTime);
  if(weight!=prevW)
    changed = true;

  float* body[2] = {&pelvis, &chest};
  if(prevW<=0.f) {
    if(weight<=0.f)
      return changed;
    // fading in: start from the current ground without smoothing lag
    for(size_t i=0; i<numLegs; ++i) {
      legs[i].groundY = gy[i];
      legs[i].plant   = plant[i];
      legs[i].normal  = normalize(Up*(1.f-plant[i]) + footTilt(n[i],legs[i].heading)*plant[i]);
      legs[i].outward = has[i] && !quadruped ? outwardTarget(n[i],legs[i].heading) : 0.f;
      }
    roll = rollTarget;
    for(size_t p=0; p<pairs; ++p)
      *body[p] = bodyTarget(p);
    return true;
    }

  {
    const float prevRoll = roll;
    approach(roll,rollTarget,1.f - std::exp(-PelvisRate*dt),changed);
    if(roll!=prevRoll)
      changed = true;
  }

  const float kN = 1.f - std::exp(-NormalRate*dt);
  for(size_t i=0; i<numLegs; ++i) {
    auto&       c    = legs[i];
    const float rate = SwingRate + (PlantRate-SwingRate)*plant[i];
    if(std::abs(gy[i]-c.groundY)>MaxStep) {
      // teleport or a ledge: no smoothing across it
      c.groundY = gy[i];
      changed   = true;
      }
    approach(c.groundY,gy[i],1.f - std::exp(-rate*dt),changed);
    if(c.plant!=plant[i])
      changed = true;
    c.plant = plant[i];

    // only a planted foot lies on the ground
    const Vec3 nt = normalize(Up*(1.f-plant[i]) + footTilt(n[i],c.heading)*plant[i]);
    Vec3 nn = normalize(c.normal + (nt-c.normal)*kN);
    if(dot(nn,nt)>SnapCos)
      nn = nt;
    if(nn!=c.normal)
      changed = true;
    c.normal = nn;

    // people turn the toes out on a slope; paws do not
    approach(c.outward, has[i] && !quadruped ? outwardTarget(n[i],c.heading) : 0.f, kN, changed);
    }

  for(size_t p=0; p<pairs; ++p) {
    float&      b     = *body[p];
    const float bt    = bodyTarget(p);
    const float limit = bodyLimit(p);
    const float prev  = b;
    if(p==0) {
      dbgTarget = bt;
      dbgLimit  = limit;
      }
    if(std::abs(dFloor)>=MaxStep) {
      b = bt; // teleport
      }
    else {
      if(std::abs(jump)>FloorJump)
        b = std::clamp(b-jump, bodyLow(p), bodyTop(p)); // a step: the body stays where it was
      approach(b,bt,1.f - std::exp(-PelvisRate*dt),changed);
      if(b>limit)
        b += (limit-b)*(1.f - std::exp(-ReachRate*dt));
      }
    if(b!=prev)
      changed = true;
    }
  return changed;
  }

void FootIk::apply(Matrix4x4* tr, size_t numBones, float floor, const Vec3& bodyOffset, AimWalk* walk) {
  if(!valid)
    return;
  for(size_t i=0; i<numLegs; ++i) {
    auto& c = legs[i];
    if(c.thighTree.empty() || c.thighTree.back()>=numBones)
      return;
    }
  // without toes: the paw points where the body goes
  Vec3 middle = {};
  if(quadruped) {
    const Vec3 v = (position(tr[legs[FrontLeft].thigh]) + position(tr[legs[FrontRight].thigh]) -
                    position(tr[legs[Left].thigh]) - position(tr[legs[Right].thigh]));
    forward = normalize(Vec3(v.x,0,v.z));
    for(size_t i=0; i<numLegs; ++i)
      middle += position(tr[legs[i].thigh])*(1.f/float(numLegs));
    }
  for(size_t i=0; i<numLegs; ++i) {
    auto& c = legs[i];
    c.animAnkle  = position(tr[c.foot]);
    c.animHip    = position(tr[c.thigh]);
    c.finalAnkle = c.animAnkle;

    // how much lower the ankle could go if the leg straightened (to LegReach)
    const Vec3  hip  = position(tr[c.thigh]);
    const float lmax = (length(position(tr[c.calf])-hip) + length(c.animAnkle-position(tr[c.calf])))*LegReach;
    const float h    = hip.y - c.animAnkle.y;
    const float x2   = (hip.x-c.animAnkle.x)*(hip.x-c.animAnkle.x) + (hip.z-c.animAnkle.z)*(hip.z-c.animAnkle.z);
    c.slack = std::max(0.f, std::sqrt(std::max(0.f, lmax*lmax - x2)) - h);

    if(c.toe!=size_t(-1)) {
      c.animToe = position(tr[c.toe]);
      const Vec3 toe = c.animToe - c.animAnkle;
      const Vec3 hd  = Vec3(toe.x,0,toe.z);
      if(length(hd)>1.f)
        c.heading = normalize(hd);
      }
    else if(length(forward)>0.5f) {
      c.heading = forward;
      }
    c.behind = std::clamp(-dot(Vec3(c.animAnkle.x-hip.x, 0, c.animAnkle.z-hip.z), c.heading)/(ToeOffDist*scale), 0.f, 1.f);
    // the body pitches: a paw far ahead of or behind the middle stands that much higher or lower
    // (not sideways: the body does not roll, and higher the rays would find the tops of rocks)
    if(quadruped)
      c.reach = std::min(std::abs(dot(Vec3(c.animAnkle.x-middle.x, 0, c.animAnkle.z-middle.z), forward))*QuadSlope,
                         hip.y-c.animAnkle.y + std::max(0.f, c.slack));
    }
  for(size_t l=0; l<numLegs; l+=2) {
    const Vec3 hips = position(tr[legs[l].thigh]) - position(tr[legs[l+1].thigh]);
    const Vec3 side = normalize(Vec3(hips.x,0,hips.z));
    legs[l  ].outDir = side;
    legs[l+1].outDir = side*-1.f;
    pairWidth[l/2]   = length(Vec3(hips.x,0,hips.z));
    }
  const bool dbg = logLevel>=2 || probing;
  if(dbg) {
    for(size_t i=0; i<numLegs; ++i) {
      auto&        c     = legs[i];
      const size_t id[4] = {c.thigh, c.calf, c.foot, c.toe!=size_t(-1) ? c.toe : c.foot};
      for(size_t k=0; k<4; ++k)
        c.dbgAnim[k] = c.dbgFinal[k] = position(tr[id[k]]);
      }
    }
  if(weight<=0.f && length(bodyOffset)<=0.f) {
    for(auto& c:legs)
      c.kneeDev = c.tilt = c.roll = c.yaw = 0;
    return;
    }

  const float dy = pelvis*weight;
  if(dy!=0.f || length(bodyOffset)>0.f) {
    // The hips counterbalance an edge lean; retain the animated ankle targets recorded above.
    for(size_t i=0; i<numBones; ++i) {
      tr[i].set(3,0,tr[i].at(3,0)+bodyOffset.x);
      tr[i].set(3,1,tr[i].at(3,1)+dy+bodyOffset.y);
      tr[i].set(3,2,tr[i].at(3,2)+bodyOffset.z);
      }
    }
  if(quadruped) {
    // the body pitches around the hips: the shoulders go down to the front paws' ground
    const Vec3  hips      = (position(tr[legs[Left].thigh]) + position(tr[legs[Right].thigh]))*0.5f;
    const Vec3  shoulders = (position(tr[legs[FrontLeft].thigh]) + position(tr[legs[FrontRight].thigh]))*0.5f;
    const Vec3  v         = shoulders - hips;
    const float d         = (chest-pelvis)*weight;
    if(std::abs(d)>Snap && length(v)>1.f)
      rotateAll(tr,numBones,rotationBetween(normalize(v),normalize(v+Up*d)),hips);

    // and leans sideways around its middle, across a slope
    const float r = roll*weight;
    if(std::abs(r)>1e-3f) {
      const Vec3 left  = (position(tr[legs[Left ].thigh]) + position(tr[legs[FrontLeft ].thigh]))*0.5f;
      const Vec3 right = (position(tr[legs[Right].thigh]) + position(tr[legs[FrontRight].thigh]))*0.5f;
      const Vec3 sd    = Vec3(left.x-right.x, 0, left.z-right.z);
      if(length(sd)>1.f) {
        const Vec3 sn = normalize(sd);
        rotateAll(tr,numBones,rotationBetween(sn,normalize(sn+Up*std::tan(r))),(left+right)*0.5f);
        }
      }
    }

  for(size_t i=0; i<numLegs; ++i) {
    auto&       c    = legs[i];
    const float low  = std::min(c.animAnkle.y, legs[i^1].animAnkle.y);
    Vec3 target = c.animAnkle;
    // the body end this leg hangs from; a body leaning sideways is higher on one side
    float body = c.front ? chest : pelvis;
    if(quadruped)
      body += std::tan(roll)*pairWidth[i/2]*0.5f*(i%2==0 ? 1.f : -1.f);
    target.y += swingOffset(c,floor,low,body)*weight;
    const Vec3 normalTarget = target;
    if(walk!=nullptr && !quadruped)
      walk->constrainFoot(i,target);
    // the smoothed normal lags behind a turning foot: limit it to the current heading once more
    const Vec3 n = normalize(Up*(1.f-weight) + footTilt(c.normal,c.heading)*weight);
    Vec3      animBend;
    if(dbg)
      animBend = kneeBend(tr,c.thigh,c.calf,c.foot);
    solve(tr,c,target,n,c.outward*weight,c.plant*c.behind*weight);
    c.finalAnkle = position(tr[c.foot]);
    if(walk!=nullptr && !quadruped)
      walk->captureFootTarget(i,normalTarget,c.finalAnkle);
    if(dbg) {
      const size_t id[4] = {c.thigh, c.calf, c.foot, c.toe!=size_t(-1) ? c.toe : c.foot};
      for(size_t k=0; k<4; ++k)
        c.dbgFinal[k] = position(tr[id[k]]);
      const Vec3 bend = kneeBend(tr,c.thigh,c.calf,c.foot);
      c.kneeDev = length(animBend)>2.f && length(bend)>2.f ?
                    std::acos(std::clamp(dot(normalize(animBend),normalize(bend)),-1.f,1.f))*180.f/float(M_PI) : 0.f;
      c.tilt    = std::acos(std::clamp(n.y,-1.f,1.f))*180.f/float(M_PI);
      c.roll    = std::atan2(dot(n,cross(Up,c.heading)),n.y)*180.f/float(M_PI);
      if(c.toe!=size_t(-1)) {
        // horizontal turn of the toes from the animated direction, + is outward
        const Vec3  ha = normalize(Vec3(c.animToe.x-c.animAnkle.x, 0, c.animToe.z-c.animAnkle.z));
        const Vec3  tf = position(tr[c.toe]) - c.finalAnkle;
        const Vec3  hf = normalize(Vec3(tf.x,0,tf.z));
        const float a  = std::acos(std::clamp(dot(ha,hf),-1.f,1.f))*180.f/float(M_PI);
        c.yaw = dot(hf-ha,c.outDir)>=0.f ? a : -a;
        }
      }
    }
  if(dbg) {
    dbgBad = 0;
    for(size_t i=0; i<numBones; ++i)
      for(int k=0; k<16; ++k)
        if(!std::isfinite(tr[i].at(k/4,k%4))) {
          ++dbgBad;
          break;
          }
    }
  }

void FootIk::solve(Matrix4x4* tr, const Chain& c, const Vec3& target, const Vec3& normal, float outward, float toeOff) const {
  const Vec3  a   = position(tr[c.thigh]);
  const Vec3  b   = position(tr[c.calf]);
  const Vec3  f   = position(tr[c.foot]);
  const float lab = length(b-a);

  // The knee bends in the plane of the animated leg, turned toward the target.
  // A nearly straight animated leg bends toward the toes (an elbow away from them).
  // Note: the bend must not be taken relative to the target line: at a heel strike uphill
  // the animated knee is behind that line and the knee would flip backwards.
  // A straight knee has no direction and wobbles: the leg does not straighten beyond LegReach,
  // or beyond the animation if it is straighter already (standing). A paw folded up to the body
  // looks broken (a zigzag): it had better go into the ground
  Vec3 bend = b-a;
  if(c.toe!=size_t(-1)) {
    const Vec3  toe     = position(tr[c.toe]) - f;
    const float footLen = length(toe);
    if(footLen>Eps)
      bend += Vec3(toe.x,0,toe.z)*(0.05f*lab/footLen*c.bendSign);
    }
  TwoBone tb;
  if(!twoBone(tr,c.thigh,c.calf,c.foot,c.thighTree,c.calfTree,target,bend,LegReach,quadruped ? MinPawFold : 0.f,tb))
    return;
  const Vec3 ankle = tb.end;

  // On a slope the whole leg turns outward around the hip-ankle line, like people do:
  // the ankle stays, the knee and the toes go away from the other leg
  Mat3 legRot = mul(tb.r1,tb.r0);
  Vec3 out    = {0,0,0};
  if(outward>0.f && length(c.outDir)>0.5f) {
    const Vec3 axis = normalize(ankle-a);
    const Vec3 move = cross(axis,c.heading);
    if(length(axis)>Eps && std::abs(dot(move,c.outDir))>Eps) {
      const Mat3 twist = axisAngle(axis, dot(move,c.outDir)>0 ? outward : -outward);
      rotateAbout(tr,c.thighTree,twist,a);
      legRot = mul(twist,legRot);
      }
    out = c.outDir;
    }

  // The foot keeps its animated orientation relative to a frame that is turned outward and laid
  // on the ground: toes keep their heading (a plain tilt would swing them sideways on a side slope)
  const Vec3 h0  = c.heading;
  const Vec3 hd  = normalize(h0*std::cos(outward) + out*std::sin(outward));
  const Vec3 fd  = normalize(hd - normal*dot(hd,normal));
  const Mat3 to  = fromColumns(fd, normal, cross(fd,normal));
  const Mat3 frm = fromColumns(h0, Up,     cross(h0,Up));
  const Mat3 fix = mul(mul(to,transpose(frm)), transpose(legRot));
  rotateAbout(tr,c.footTree,fix,position(tr[c.foot]));

  // A leg too short to reach: the foot stands on its toes, they go down by what the ankle misses
  const float gap = (ankle.y - target.y)*toeOff;
  if(gap>0.5f && c.toe!=size_t(-1)) {
    const Vec3  t  = position(tr[c.toe]) - ankle;
    const float l  = length(t);
    const float ty = std::max(t.y - gap, -0.9f*l);
    const float hl = std::sqrt(std::max(0.f, l*l - ty*ty));
    const Vec3  th = normalize(Vec3(t.x,0,t.z));
    if(l>Eps && length(th)>Eps)
      rotateAbout(tr,c.footTree,rotationBetween(t*(1.f/l), normalize(th*hl + Up*ty)),ankle);
    }
  }

void FootIk::probe(Probe& p, const Input& in) const {
  if(!probing)
    return;
  p.skeleton = skeleton;
  p.valid   = valid;
  p.numLegs = uint8_t(numLegs);
  p.badBones = dbgBad;
  if(!valid)
    return;
  p.chest   = chest;
  p.enable  = in.enable;
  p.hardOff = in.hardOff;
  p.weight  = weight;
  p.pelvis  = pelvis;
  p.target  = dbgTarget;
  p.crouch  = crouch;
  p.limit   = dbgLimit;
  p.floorY  = in.floorY;
  for(size_t i=0; i<numLegs; ++i) {
    auto&       l   = p.leg[i];
    auto&       c   = legs[i];
    const auto& g   = in.ground[i];
    const float off = g.hasCol ? g.y - in.floorY : 0.f;
    for(size_t k=0; k<4; ++k) {
      l.anim [k] = c.dbgAnim [k];
      l.final[k] = c.dbgFinal[k];
      }
    l.hasCol  = g.hasCol;
    l.groundY = g.y;
    l.n       = g.n;
    l.toeCol  = g.toeCol;
    l.toeY    = g.toeY;
    l.smoothY = c.groundY;
    l.plant   = std::clamp(1.f - (c.animAnkle.y-std::min(c.animAnkle.y,legs[i^1].animAnkle.y))/(PlantLift*scale), 0.f, 1.f);
    l.slack   = c.slack;
    l.err     = (c.finalAnkle.y - (in.floorY + off)) - (c.animAnkle.y - in.floorY);
    l.kneeDev = c.kneeDev;
    l.tilt    = c.tilt;
    l.roll    = c.roll;
    l.yaw     = c.yaw;
    l.outward = c.outward*180.f/float(M_PI);
    }
  }

void FootIk::debugLog(std::string_view name, const Input& in, bool verbose) {
  if(logLevel<=0)
    return;
  logTime += in.dt;

  // contact error: ankle height over the real ground minus its animated height over the floor;
  // for a planted foot it should be ~0, without IK it is -off
  float off[2] = {};
  float err[2] = {};
  bool  hit    = true;
  for(size_t i=0; i<2; ++i) {
    hit = hit && in.ground[i].hasCol;
    off[i] = in.ground[i].hasCol ? in.ground[i].y - in.floorY : 0.f;
    err[i] = (legs[i].finalAnkle.y - (in.floorY + off[i])) - (legs[i].animAnkle.y - in.floorY);
    }

  char buf[320] = {};
  if(verbose && logLevel>=2) {
    std::snprintf(buf, sizeof(buf),
                  "t=%llu on=%d w=%.2f floor=%.1f pelvis=%.1f | L lift=%.1f plant=%.2f off=%.1f err=%.1f knee=%.1f tilt=%.1f roll=%.1f out=%.1f"
                  " | R lift=%.1f plant=%.2f off=%.1f err=%.1f knee=%.1f tilt=%.1f roll=%.1f out=%.1f",
                  (unsigned long long)logTime, int(in.enable), weight, in.floorY, pelvis,
                  legs[0].animAnkle.y-in.floorY, legs[0].plant, off[0], err[0], legs[0].kneeDev, legs[0].tilt, legs[0].roll, legs[0].yaw,
                  legs[1].animAnkle.y-in.floorY, legs[1].plant, off[1], err[1], legs[1].kneeDev, legs[1].tilt, legs[1].roll, legs[1].yaw);
    Log::i("[footik] ", buf);
    return;
    }

  if(!in.enable || !hit)
    return;
  logTimer += in.dt;
  if(logTimer<1000)
    return;
  logTimer = 0;
  if(std::abs(off[0])<1.f && std::abs(off[1])<1.f)
    return;
  std::snprintf(buf, sizeof(buf), "offL=%.1f offR=%.1f pelvis=%.1f w=%.2f errL=%.1f errR=%.1f",
                off[0], off[1], pelvis, weight, err[0], err[1]);
  Log::i("[footik] ", name, " ", buf);
  }
