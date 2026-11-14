#include "ragdoll_brace_rig.h"

#include <Jolt/Physics/Constraints/SwingTwistConstraint.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

namespace RagdollBrace {

namespace {
  constexpr float degrees = 180.f/3.14159265358979323846f;

  bool has(std::string_view name, std::string_view part) {
    return name.find(part)!=std::string_view::npos;
    }

  // Angle between two orientations, either sign of the quaternion. atan2 of the relative
  // rotation, not acos of a dot product: that one cannot tell 0.04 degrees from 0 in float.
  float angleDeg(JPH::QuatArg a, JPH::QuatArg b) {
    const auto r=a.Normalized().Conjugated()*b.Normalized();
    return 2.f*std::atan2(r.GetXYZ().Length(),std::abs(r.GetW()))*degrees;
    }

  void raise(std::atomic<uint32_t>& value, uint32_t candidate) {
    auto current=value.load(std::memory_order_relaxed);
    while(candidate>current && !value.compare_exchange_weak(current,candidate,std::memory_order_relaxed)) {}
    }

  uint32_t scaled(float value, float scale) {
    return uint32_t(std::clamp(value*scale,0.f,4.e9f));
    }

  struct Number final { std::string_view group, key; float Profile::*field; };
  constexpr Number numbers[] = {
    {"arms","enter",&Profile::armsEnter}, {"arms","hold",&Profile::armsHold}, {"arms","drop",&Profile::armsDrop},
    {"arms","level",&Profile::armsLevel}, {"arms","rest",&Profile::armsRest},
    {"arms","yield",&Profile::armsYield}, {"arms","torque",&Profile::armsTorque},
    {"neck","enter",&Profile::neckEnter}, {"neck","hold",&Profile::neckHold}, {"neck","yield",&Profile::neckYield},
    {"neck","torque",&Profile::neckTorque}, {"spine","enter",&Profile::spineEnter}, {"legs","enter",&Profile::legsEnter},
    {"legs","front",&Profile::legsFront}, {"legs","back",&Profile::legsBack}, {"legs","side",&Profile::legsSide},
    {"land","enter",&Profile::landEnter}, {"land","hold",&Profile::landHold},
    {"land","yield",&Profile::landYield}, {"land","torque",&Profile::landTorque},
    };
  }

std::string_view clipName(Kind kind) {
  switch(kind) {
    case Kind::Front: return "S_BRACE_F";
    case Kind::Back:  return "S_BRACE_B";
    case Kind::Left:  return "S_BRACE_L";
    case Kind::Right: return "S_BRACE_R";
    case Kind::None:  break;
    }
  return "none";
  }

std::string_view landClipName(Kind kind) {
  switch(kind) {
    case Kind::Front: return "S_BRACELAND_F";
    case Kind::Back:  return "S_BRACELAND_B";
    case Kind::Left:  return "S_BRACELAND_L";
    case Kind::Right: return "S_BRACELAND_R";
    case Kind::None:  break;
    }
  return "none";
  }

Kind select(float forward, float left) {
  if(!std::isfinite(forward) || !std::isfinite(left) || forward*forward+left*left<1.e-12f)
    return Kind::None;
  // A thousandth of a degree keeps the inclusive sector borders exact for a rounded direction.
  const float angle=std::atan2(left,forward)*degrees;
  if(std::abs(angle)<=60.001f)
    return Kind::Front;
  if(std::abs(angle)>=119.999f)
    return Kind::Back;
  return angle>0.f ? Kind::Left : Kind::Right;
  }

Role role(std::string_view n) {
  // The same order as the hit zones that give the parts their capsules.
  if(has(n,"HEAD"))     return Role::Head;
  if(has(n,"KIEFER") || has(n,"JAW")) return Role::Other;
  if(has(n,"NECK"))     return Role::Neck;
  if(has(n,"CLAVICLE")) return Role::Clavicle;
  if(has(n,"UPPERARM")) return Role::UpperArm;
  if(has(n,"FOREARM"))  return Role::Forearm;
  if(has(n,"FINGER"))   return Role::Finger;
  if(has(n,"HAND"))     return Role::Hand;
  if(has(n,"THIGH"))    return Role::Thigh;
  if(has(n,"CALF"))     return Role::Calf;
  if(has(n,"TOE"))      return Role::Toe;
  if(has(n,"FOOT"))     return Role::Foot;
  if(has(n,"PELVIS"))   return Role::Pelvis;
  if(has(n,"SPINE"))    return Role::Spine;
  return Role::Other;
  }

Side side(std::string_view n) {
  if(has(n," L ")) return Side::Left;
  if(has(n," R ")) return Side::Right;
  return Side::None;
  }

Profile& profile() {
  static Profile value;
  return value;
  }

bool setNumber(Profile& profile, std::string_view group, std::string_view key, float value) {
  if(!std::isfinite(value) || value<0.f || (key=="level" && value>1.f))
    return false;
  if(group=="legs" && (key=="front" || key=="back" || key=="side") && value!=0.f && value!=1.f)
    return false;
  for(const auto& n:numbers)
    if(n.group==group && n.key==key) {
      profile.*n.field=value;
      return true;
      }
  return false;
  }

std::string describe(const Profile& profile) {
  std::string result;
  std::string_view group;
  for(const auto& n:numbers) {
    if(n.group!=group) {
      if(!result.empty()) result+="; ";
      result+=n.group;
      group=n.group;
      }
    char value[32];
    std::snprintf(value,sizeof(value)," %.*s %g",int(n.key.size()),n.key.data(),double(profile.*n.field));
    result+=value;
    }
  return result;
  }

void Contacts::mark(uint32_t part, float penetrationM, float approachMs) {
  if(part>=64) return;
  const uint64_t bit=uint64_t(1)<<part;
  if((watched&bit)==0) return;
  touched.fetch_or(bit,std::memory_order_relaxed);
  if((deep&bit)!=0 && penetrationM>0.f) raise(depthUm,scaled(penetrationM,1.e6f));
  if((fast&bit)!=0 && approachMs>0.f) raise(speedMmS,scaled(approachMs,1.e3f));
  }

std::unique_ptr<Rig> Rig::create(Kind kind, const Profile& profile, JPH::Ragdoll& native,
                                 const std::vector<Joint>& joints, const std::vector<JPH::Quat>& pose, float age,
                                 const std::vector<JPH::Quat>& landPose) {
  const size_t count=native.GetBodyCount();
  if(count==0 || count>64 || joints.size()!=count || (kind!=Kind::None && pose.size()!=count) ||
     (!landPose.empty() && landPose.size()!=count))
    return nullptr;
  std::unique_ptr<Rig> rig(new Rig());
  rig->braceKind=kind;
  rig->numbers=profile;
  rig->age=age;
  rig->hasLandPose=kind!=Kind::None && !landPose.empty();
  if(rig->hasLandPose && age>=profile.armsHold) rig->landAt=profile.armsHold;
  rig->parts.resize(count);
  rig->measures.kind=kind;
  rig->measures.landKind=rig->hasLandPose ? kind : Kind::None;
  for(size_t i=0;i<count;++i) {
    auto& p=rig->parts[i];
    p.role=role(joints[i].name);
    p.side=side(joints[i].name);
    const size_t s=p.side==Side::Right ? 1 : 0;
    switch(p.role) {
      case Role::Clavicle: case Role::Forearm: case Role::Hand: case Role::Finger: case Role::UpperArm:
        p.group=Group::Arms; break;
      case Role::Neck: case Role::Head: p.group=Group::Neck; break;
      case Role::Spine: p.group=Group::Spine; break;
      case Role::Thigh: case Role::Calf: case Role::Foot: case Role::Toe: p.group=Group::Legs; break;
      default: break;
      }
    if(p.side==Side::None) {
      if(p.group==Group::Arms || p.group==Group::Legs) p.group=Group::None;
      }
    else if(p.role==Role::UpperArm && rig->upperArm[s]<0) rig->upperArm[s]=int(i);
    else if(p.role==Role::Forearm && rig->forearm[s]<0) rig->forearm[s]=int(i);
    else if(p.role==Role::Hand && rig->hand[s]<0) rig->hand[s]=int(i);
    }
  for(size_t s=0;s<2;++s)
    if(rig->upperArm[s]<0 || rig->forearm[s]<0 || rig->hand[s]<0)
      return nullptr;
  for(size_t s=0;s<2;++s) {
    const auto sideOf=s==0 ? Side::Left : Side::Right;
    rig->armMask[s]=rig->mask(Role::Forearm,Role::Hand,Role::Finger,sideOf);
    rig->handMask[s]=rig->mask(Role::Hand,Role::Finger,Role::Finger,sideOf);
    }
  rig->torsoMask=rig->mask(Role::Pelvis,Role::Spine,Role::Spine,Side::None);
  rig->headMask=rig->mask(Role::Head,Role::Head,Role::Head,Side::None);
  rig->touch.watched=rig->armMask[0]|rig->armMask[1]|rig->torsoMask|rig->headMask;
  rig->touch.deep=rig->handMask[0]|rig->handMask[1];
  rig->touch.fast=rig->headMask;
  const auto* settings=native.GetRagdollSettings();
  rig->measures.upperArmKg=settings->mParts[size_t(rig->upperArm[0])].mMassPropertiesOverride.mMass;
  rig->measures.forearmKg=settings->mParts[size_t(rig->forearm[0])].mMassPropertiesOverride.mMass;
  rig->measures.handKg=settings->mParts[size_t(rig->hand[0])].mMassPropertiesOverride.mMass;
  if(kind==Kind::None)
    return rig;
  for(size_t i=0;i<count;++i) {
    auto& p=rig->parts[i];
    const int parent=joints[i].parent;
    const int index=settings->GetConstraintIndexForBodyIndex(int(i));
    if(p.group==Group::None || parent<0 || index<0) continue;
    // The constraint clamps whatever target it gets. Let it clamp the pose once, here,
    // and measure by how much: the motors then never receive a target off their limits.
    auto* joint=static_cast<JPH::SwingTwistConstraint*>(native.GetConstraint(index));
    const auto previous=joint->GetTargetOrientationCS();
    const auto toBody1=joint->GetConstraintToBody1(), toBody2=joint->GetConstraintToBody2();
    const auto clamp=[&](const std::vector<JPH::Quat>& frame) {
      const auto request=(frame[size_t(parent)].Conjugated()*frame[i]).Normalized();
      joint->SetTargetOrientationBS(request);
      const auto clamped=joint->GetTargetOrientationCS();
      p.clampDeg=std::max(p.clampDeg,angleDeg(toBody1.Conjugated()*request*toBody2,clamped));
      return (toBody1*clamped*toBody2.Conjugated()).Normalized();
      };
    p.pose=clamp(pose);
    if(rig->hasLandPose) p.landPose=clamp(landPose);
    joint->SetTargetOrientationCS(previous);
    const float legs=kind==Kind::Front ? profile.legsFront : kind==Kind::Back ? profile.legsBack : profile.legsSide;
    p.posed=p.group!=Group::Legs || (rig->hasLandPose ? legs!=0.f : kind==Kind::Back);
    if((p.posed || rig->hasLandPose) && p.clampDeg>rig->measures.clampMaxDeg) {
      rig->measures.clampMaxDeg=p.clampDeg;
      rig->measures.clampBone=std::string(joints[i].name);
      }
    }
  return rig;
  }

uint64_t Rig::mask(Role a, Role b, Role c, Side s) const {
  uint64_t result=0;
  for(size_t i=0;i<parts.size();++i)
    if((parts[i].role==a || parts[i].role==b || parts[i].role==c) && (s==Side::None || parts[i].side==s))
      result|=uint64_t(1)<<i;
  return result;
  }

float Rig::armWeight(size_t s) const {
  const float touched=armAt[s]>=0.f ? armAt[s] : std::numeric_limits<float>::infinity();
  const float d=age-std::min(touched,numbers.armsHold);
  const float rest=hasLandPose ? numbers.armsRest : 0.f;
  if(d<=0.f) return 1.f;
  if(d>=numbers.armsDrop+rest+numbers.armsYield) return 0.f;
  if(d<numbers.armsDrop) return 1.f-(1.f-numbers.armsLevel)*d/numbers.armsDrop;
  if(d<numbers.armsDrop+rest) return numbers.armsLevel;
  return numbers.armsLevel*(1.f-(d-numbers.armsDrop-rest)/numbers.armsYield);
  }

float Rig::neckWeight() const {
  const float touched=torsoAt>=0.f ? torsoAt : std::numeric_limits<float>::infinity();
  const float d=age-std::min(touched,numbers.neckHold);
  if(d<=0.f) return 1.f;
  if(d>=numbers.neckYield) return 0.f;
  return 1.f-d/numbers.neckYield;
  }

void Rig::advance(float deltaSeconds) {
  age+=deltaSeconds;
  if(hasLandPose && landAt<0.f && age>=numbers.armsHold) landAt=numbers.armsHold;
  }

void Rig::drive(size_t part, float mass, JPH::Quat& target, float& torque) {
  if(braceKind==Kind::None || part>=parts.size()) return;
  auto& p=parts[part];
  if(p.posed) {
    const float enter=p.group==Group::Arms ? numbers.armsEnter : p.group==Group::Neck ? numbers.neckEnter :
                      p.group==Group::Spine ? numbers.spineEnter : numbers.legsEnter;
    // The ordinary fade supplies the birth target on every step. At landing, freeze
    // its first-phase interpolation at that instant, including a still-entering pose.
    const float firstAge=landAt>=0.f ? landAt : age;
    float s=enter>0.f ? std::clamp(firstAge/enter,0.f,1.f) : 1.f;
    s=s*s*(3.f-2.f*s);
    target=s>=1.f ? p.pose : target.SLERP(p.pose,s).Normalized();
    }
  if(landAt>=0.f && p.group!=Group::None) {
    float s=numbers.landEnter>0.f ? std::clamp((age-landAt)/numbers.landEnter,0.f,1.f) : 1.f;
    s=s*s*(3.f-2.f*s);
    target=s>=1.f ? p.landPose : target.SLERP(p.landPose,s).Normalized();
    }
  if(p.posed || (landAt>=0.f && p.group!=Group::None)) {
    p.request=target;
    p.requested=true;
    }
  const size_t s=p.side==Side::Right ? 1 : 0;
  if(p.group==Group::Arms)
    torque=mass*numbers.armsTorque*armWeight(s);
  else if(p.group==Group::Neck)
    torque=mass*numbers.neckTorque*neckWeight();
  else if(p.group==Group::Legs && landAt>=0.f) {
    const float elapsed=age-landAt-numbers.landHold;
    const float weight=elapsed<=0.f ? 1.f : numbers.landYield>0.f ? std::max(0.f,1.f-elapsed/numbers.landYield) : 0.f;
    torque=std::max(torque,mass*numbers.landTorque*weight);
    }
  }

float Rig::elbow(const JPH::Ragdoll& native, const JPH::BodyInterface& bodies, size_t s) const {
  const auto at=[&](int part) { return JPH::Vec3(bodies.GetPosition(native.GetBodyID(part))); };
  const auto upper=at(forearm[s])-at(upperArm[s]), lower=at(hand[s])-at(forearm[s]);
  if(upper.LengthSq()<1.e-8f || lower.LengthSq()<1.e-8f) return -1.f;
  return std::acos(std::clamp(upper.Normalized().Dot(lower.Normalized()),-1.f,1.f))*degrees;
  }

void Rig::afterStep(const JPH::Ragdoll& native, const JPH::BodyInterface& bodies) {
  const uint64_t touched=touch.touched.exchange(0,std::memory_order_relaxed);
  const uint32_t speed=touch.speedMmS.exchange(0,std::memory_order_relaxed);
  if(touched==0) return;
  if(hasLandPose && landAt<0.f && (touched&(armMask[0]|armMask[1]|torsoMask))!=0) landAt=age;
  for(size_t s=0;s<2;++s) {
    if(armAt[s]<0.f && (touched&armMask[s])!=0) armAt[s]=age;
    if(handAt[s]<0.f && (touched&handMask[s])!=0) {
      handAt[s]=age;
      measures.elbowDeg[s]=elbow(native,bodies,s);
      }
    }
  if(torsoAt<0.f && (touched&torsoMask)!=0) torsoAt=age;
  if(headAt<0.f && (touched&headMask)!=0) {
    headAt=age;
    measures.headImpact=float(speed)*.001f;
    }
  }

Report Rig::report() const {
  Report result=measures;
  const auto ms=[](float seconds) { return seconds>=0.f ? seconds*1000.f : -1.f; };
  for(size_t s=0;s<2;++s) {
    result.handMs[s]=ms(handAt[s]);
    result.armMs[s]=ms(armAt[s]);
    }
  result.torsoMs=ms(torsoAt);
  result.landMs=ms(landAt);
  result.handDepthCm=float(touch.depthUm.load(std::memory_order_relaxed))*1.e-4f;
  return result;
  }

float Rig::requestDeg(size_t part, const JPH::SwingTwistConstraint& joint) const {
  if(part>=parts.size() || !parts[part].requested) return 0.f;
  const auto wanted=joint.GetConstraintToBody1().Conjugated()*parts[part].request*joint.GetConstraintToBody2();
  return angleDeg(wanted,joint.GetTargetOrientationCS());
  }

std::vector<MotorProbe> probes(const JPH::Ragdoll& native, const std::vector<size_t>& bones, const Rig* rig) {
  std::vector<MotorProbe> result;
  const auto* settings=native.GetRagdollSettings();
  for(size_t i=0;i<native.GetBodyCount() && i<bones.size();++i) {
    const int index=settings->GetConstraintIndexForBodyIndex(int(i));
    if(index<0) continue;
    const auto& joint=*static_cast<const JPH::SwingTwistConstraint*>(native.GetConstraint(index));
    MotorProbe probe;
    probe.bone=uint32_t(bones[i]);
    probe.massKg=settings->mParts[i].mMassPropertiesOverride.mMass;
    const bool swing=joint.GetSwingMotorState()==JPH::EMotorState::Position;
    const bool twist=joint.GetTwistMotorState()==JPH::EMotorState::Position;
    probe.on=swing || twist;
    probe.torqueLimit=std::max(swing ? joint.GetSwingMotorSettings().mMaxTorqueLimit : 0.f,
                               twist ? joint.GetTwistMotorSettings().mMaxTorqueLimit : 0.f);
    const auto target=joint.GetTargetOrientationCS();
    probe.target={target.GetX(),target.GetY(),target.GetZ(),target.GetW()};
    probe.errorDeg=angleDeg(joint.GetRotationInConstraintSpace(),target);
    if(rig!=nullptr) {
      probe.requestDeg=rig->requestDeg(i,joint);
      probe.clampDeg=rig->clampDeg(i);
      }
    result.push_back(probe);
    }
  return result;
  }

}
