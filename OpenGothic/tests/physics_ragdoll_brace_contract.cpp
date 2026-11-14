// The protective fall (physics/ragdoll_brace.*): cases of gothic-ragdoll-contract, run from
// physics_ragdoll_contract.cpp's main. The pose is built here on the synthetic human; the
// eight authored records are read only when OPENGOTHIC_BRACE_POSES names their content folder.
#include "physics/collisionworld.h"
#include "physics/dynamicworld.h"
#include "physics/ragdoll.h"
#include "graphics/mesh/animmath.h"
#include "graphics/mesh/hitzones.h"
#include "graphics/mesh/skeleton.h"
#include "asset/fromzenkit.h"
#include "modeldata.h"

#include <Jolt/Jolt.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>

#include <zenkit/ModelHierarchy.hh>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

// The synthetic human of physics_ragdoll_contract.cpp.
zenkit::ModelHierarchy ragdollContractHierarchy();

// CollisionWorld's friend: the arms' reach is measured without gravity.
class RagdollBraceContract final {
  public:
    static void zeroGravity(CollisionWorld& world) { world.physicsSystem().SetGravity(JPH::Vec3::sZero()); }
    // World bounds of all ragdoll parts together, metres.
    static JPH::AABox ragdollBounds(CollisionWorld& world) {
      JPH::BodyIDVector ids;
      world.physicsSystem().GetBodies(ids);
      JPH::AABox result;
      for(const auto id:ids) {
        JPH::BodyLockRead lock(world.physicsSystem().GetBodyLockInterface(),id);
        if(lock.Succeeded() && lock.GetBody().GetObjectLayer()==DynamicWorld::C_Ragdoll)
          result.Encapsulate(lock.GetBody().GetWorldSpaceBounds());
        }
      return result;
      }
  };

namespace {

using RagdollBrace::Kind;
constexpr float pi = 3.14159265358979323846f;
// Bones of the synthetic human.
enum : size_t { Spine1=3, LUpper=6, LFore=7, LHand=8, RUpper=9, RFore=10, RHand=11,
                LThigh=12, LCalf=13, LFoot=14, LToe=15, RThigh=16, RCalf=17, RFoot=18, RToe=19 };

void require(bool condition, const char* message) {
  if(condition)
    return;
  std::fprintf(stderr,"FAIL: %s\n",message);
  std::exit(EXIT_FAILURE);
  }

Tempest::Matrix4x4 at(float x, float y, float z) {
  auto matrix=Tempest::Matrix4x4::mkIdentity();
  matrix.translate(x,y,z);
  return matrix;
  }

Tempest::Vec3 origin(const Tempest::Matrix4x4& m) { return {m.data()[12],m.data()[13],m.data()[14]}; }

// Rotation about the vertical axis: +X turns towards +Z for a negative angle.
Tempest::Matrix4x4 turnY(float degrees) {
  const float a=degrees*pi/180.f, c=std::cos(a), s=std::sin(a);
  float m[16]={c,0,-s,0, 0,1,0,0, s,0,c,0, 0,0,0,1};
  Tempest::Matrix4x4 result;
  result.setData(m);
  return result;
  }

JPH::Quat rotation(const Tempest::Matrix4x4& matrix) {
  const auto* m=matrix.data();
  return JPH::Mat44(JPH::Vec4(m[0],m[1],m[2],0),JPH::Vec4(m[4],m[5],m[6],0),JPH::Vec4(m[8],m[9],m[10],0),
                    JPH::Vec4(0,0,0,1)).GetQuaternion().Normalized();
  }

float angleDeg(JPH::QuatArg a, JPH::QuatArg b) {
  return 2.f*std::acos(std::min(1.f,std::abs(a.Normalized().Dot(b.Normalized()))))*180.f/pi;
  }

struct Human final {
  Skeleton skeleton{asset::fromZenKit(ragdollContractHierarchy()),nullptr,"HUMANS.MDS"};
  HitZones zones;
  Human() { zones.setSkeleton(&skeleton); }

  std::unique_ptr<Ragdoll> create(CollisionWorld& world, const Tempest::Matrix4x4& object) const {
    std::vector<Tempest::Matrix4x4> pose;
    for(const auto& matrix:skeleton.tr)
      pose.push_back(object*matrix);
    auto body=world.createRagdoll(skeleton,zones,pose.data(),pose.size(),object);
    require(body!=nullptr && body->valid(),"the brace cases need a valid human ragdoll");
    return body;
    }

  bool descends(size_t bone, size_t from) const {
    for(size_t i=bone,n=0;i<skeleton.nodes.size() && n<=skeleton.nodes.size();i=skeleton.nodes[i].parent,++n)
      if(i==from) return true;
    return false;
    }

  // Turns a bone and everything it carries about its own origin, in model space.
  void turn(std::vector<Tempest::Matrix4x4>& pose, size_t bone, float degrees) const {
    const auto o=origin(pose[bone]);
    const auto r=at(o.x,o.y,o.z)*turnY(degrees)*at(-o.x,-o.y,-o.z);
    for(size_t i=0;i<pose.size();++i)
      if(descends(i,bone)) pose[i]=r*pose[i];
    }

  // Arms raised forward from the bind T: shoulders 80 degrees, elbows bent inwards.
  std::vector<Tempest::Matrix4x4> armsForward(float leftElbow=40.f) const {
    auto pose=skeleton.tr;
    turn(pose,LUpper,-80.f);
    turn(pose,LFore,-leftElbow);
    turn(pose,RUpper,80.f);
    turn(pose,RFore,40.f);
    return pose;
    }

  std::vector<Tempest::Matrix4x4> landed() const {
    auto pose=armsForward(90.f);
    turn(pose,RFore,50.f); // both elbows at 90 degrees
    auto bend=Tempest::Matrix4x4::mkIdentity();
    bend.rotateOX(60.f);
    for(size_t calf:{size_t(LCalf),size_t(RCalf)}) {
      const auto o=origin(pose[calf]);
      const auto r=at(o.x,o.y,o.z)*bend*at(-o.x,-o.y,-o.z);
      for(size_t i=0;i<pose.size();++i)
        if(descends(i,calf)) pose[i]=r*pose[i];
      }
    return pose;
    }
  };

auto ground(CollisionWorld& world) {
  return world.addCollisionBody(new JPH::BoxShape(JPH::Vec3(20,.5f,20)),at(0,-50,0),.75f,DynamicWorld::C_Landscape);
  }

const RagdollBrace::MotorProbe& probe(const std::vector<RagdollBrace::MotorProbe>& probes, size_t bone) {
  for(const auto& p:probes)
    if(p.bone==bone) return p;
  require(false,"every joint of the synthetic human must have a motor probe");
  return probes.front();
  }

JPH::Quat target(const RagdollBrace::MotorProbe& p) { return {p.target.x,p.target.y,p.target.z,p.target.w}; }

std::vector<RagdollBrace::MotorProbe> poseTargets(const Human& human, const std::vector<Tempest::Matrix4x4>& pose) {
  CollisionWorld world;
  RagdollBraceContract::zeroGravity(world);
  auto body=human.create(world,at(0,300,0));
  RagdollBrace::Profile profile;
  profile.armsEnter=profile.neckEnter=profile.spineEnter=profile.legsEnter=0.f;
  require(body->brace(Kind::Back,pose.data(),pose.size(),profile),"the target reference needs its pose");
  return body->motorProbes();
  }

// Tick in the birth guard's 17/17/16 rhythm; the age counts native steps since birth.
float tick(CollisionWorld& world, int frame, uint64_t birthStep) {
  world.tick(frame%3==2 ? 16 : 17);
  return float(world.simulationStep()-birthStep)/60.f;
  }

void testSelection() {
  // Eight throws 45 degrees apart, clockwise from forward: F, F|R, R, R|B, B, B|L, L, L|F.
  const Kind expected[]={Kind::Front,Kind::Front,Kind::Right,Kind::Back,Kind::Back,Kind::Back,Kind::Left,Kind::Front};
  for(int i=0;i<8;++i) {
    const float a=float(i)*45.f*pi/180.f;
    require(RagdollBrace::select(std::cos(a),-std::sin(a))==expected[i],"eight throw directions must follow the 60-degree sectors");
    }
  const auto at=[](float degrees) { const float a=degrees*pi/180.f; return RagdollBrace::select(std::cos(a),std::sin(a)); };
  require(at(60.f)==Kind::Front && at(-60.f)==Kind::Front && at(60.5f)==Kind::Left && at(-60.5f)==Kind::Right,
          "60 degrees from forward is still forward");
  require(at(120.f)==Kind::Back && at(-120.f)==Kind::Back && at(119.5f)==Kind::Left && at(-119.5f)==Kind::Right,
          "60 degrees from back is still back");
  require(RagdollBrace::select(0,0)==Kind::None,"no horizontal throw selects no pose");
  // The body's frame, not the world's: an actor turned about the vertical axis.
  Human human;
  CollisionWorld world;
  auto object=Tempest::Matrix4x4::mkIdentity();
  object.translate(0,300,0);
  object.rotateOY(90);
  auto body=human.create(world,object);
  const auto* m=object.data();
  const Tempest::Vec3 forward(m[8],0,m[10]), left(m[0],0,m[2]);
  const Tempest::Vec3 back(-forward.x,0,-forward.z), right(-left.x,0,-left.z);
  require(body->braceKind(forward)==Kind::Front && body->braceKind(back)==Kind::Back &&
          body->braceKind(left)==Kind::Left && body->braceKind(right)==Kind::Right,
          "the pose follows the throw in the turned body's own frame");
  std::puts("ragdoll brace selection: 8 directions and 4 borders by the sector rule, turned body ok");
  }

void testReachWithoutGravity() {
  Human human;
  CollisionWorld world;
  RagdollBraceContract::zeroGravity(world);
  auto body=human.create(world,at(0,300,0));
  const auto birth=world.simulationStep();
  const auto pose=human.armsForward();
  require(body->brace(Kind::Front,pose.data(),pose.size(),RagdollBrace::Profile{}),"a fresh human ragdoll must accept a brace pose");
  RagdollBrace::Report report;
  require(body->braceReport(report) && report.kind==Kind::Front,"an accepted brace must report its pose");
  std::printf("ragdoll brace synthetic pose clamp_max_deg=%.3f bone=%s masses_kg upperarm=%.3f forearm=%.3f hand=%.3f\n",
              double(report.clampMaxDeg),report.clampBone.c_str(),double(report.upperArmKg),double(report.forearmKg),double(report.handKg));
  require(report.clampMaxDeg<1.f,"the synthetic arms-forward pose lies inside the joint limits");
  const std::pair<size_t,size_t> joints[]={{LUpper,Spine1},{LFore,LUpper},{RUpper,Spine1},{RFore,RUpper}};
  float error[4]={},reached[4]={-1,-1,-1,-1};
  float age=0;
  for(int frame=0;age<.3f-1.e-4f;++frame) {
    age=tick(world,frame,birth);
    const auto state=body->state();
    const auto part=[&](size_t bone) {
      for(const auto& p:state.parts) if(p.bone==bone) return JPH::Quat(p.rotation.x,p.rotation.y,p.rotation.z,p.rotation.w);
      require(false,"the arm parts must be physical");
      return JPH::Quat::sIdentity();
      };
    for(size_t j=0;j<4;++j) {
      const auto [bone,parent]=joints[j];
      const auto wanted=rotation(pose[parent]).Conjugated()*rotation(pose[bone]);
      error[j]=angleDeg(part(parent).Conjugated()*part(bone),wanted);
      if(reached[j]<0.f && error[j]<15.f) reached[j]=age;
      }
    }
  std::printf("ragdoll brace reach at %.3f s error_deg shoulder_l=%.2f elbow_l=%.2f shoulder_r=%.2f elbow_r=%.2f; "
              "under 15 deg at s: %.3f %.3f %.3f %.3f\n",double(age),double(error[0]),double(error[1]),double(error[2]),
              double(error[3]),double(reached[0]),double(reached[1]),double(reached[2]),double(reached[3]));
  for(float e:error)
    require(e<15.f,"without gravity the shoulders and elbows reach the pose within 15 degrees by 0.3 s");
  }

RagdollBrace::Report forwardFall(const Human& human, bool braced) {
  CollisionWorld world;
  auto floor=ground(world);
  auto body=human.create(world,at(0,1,0));
  const auto birth=world.simulationStep();
  body->knockOff({0,0,1},30.f);
  const auto pose=human.armsForward();
  require(body->brace(braced ? Kind::Front : Kind::None,braced ? pose.data() : nullptr,braced ? pose.size() : 0,RagdollBrace::Profile{}),
          "a fresh ragdoll must accept the pose, or measuring alone");
  for(int frame=0;tick(world,frame,birth)<2.f;++frame) {}
  RagdollBrace::Report report;
  require(body->braceReport(report),"the brace measures must last the whole fall");
  return report;
  }

void testForwardFall() {
  Human human;
  const auto with=forwardFall(human,true), without=forwardFall(human,false);
  const auto first=[](const RagdollBrace::Report& r) {
    const float l=r.handMs[0],h=r.handMs[1];
    return l<0.f ? h : h<0.f ? l : std::min(l,h);
    };
  std::printf("ragdoll brace forward fall: pose hand_ms=%.0f/%.0f torso_ms=%.0f elbow_deg=%.1f/%.1f head_m_s=%.2f depth_cm=%.2f; "
              "no pose hand_ms=%.0f/%.0f torso_ms=%.0f head_m_s=%.2f\n",
              double(with.handMs[0]),double(with.handMs[1]),double(with.torsoMs),double(with.elbowDeg[0]),double(with.elbowDeg[1]),
              double(with.headImpact),double(with.handDepthCm),double(without.handMs[0]),double(without.handMs[1]),
              double(without.torsoMs),double(without.headImpact));
  require(with.torsoMs>=0.f,"with the pose the trunk still lands: its touch must be seen");
  require(first(with)>=0.f && first(with)<with.torsoMs,"with the pose a hand touches the ground before the trunk");
  require(without.torsoMs>=0.f && (first(without)<0.f || without.torsoMs<=first(without)),
          "the same throw without a pose lands the trunk no later than a hand");
  }

void testYieldAfterTouch() {
  // Without gravity a post stops the left forearm on its way forward; the right arm meets nothing.
  Human human;
  CollisionWorld world;
  RagdollBraceContract::zeroGravity(world);
  auto post=world.addCollisionBody(new JPH::BoxShape(JPH::Vec3(.04f,.2f,.04f)),at(60,139,45),.75f,DynamicWorld::C_Landscape);
  auto body=human.create(world,at(0,0,0));
  const auto birth=world.simulationStep();
  const auto pose=human.armsForward();
  require(body->brace(Kind::Front,pose.data(),pose.size(),RagdollBrace::Profile{}),"the yield case needs an accepted pose");
  const RagdollBrace::Profile profile;
  float touchedAt=-1, worstLeft01=0, worstLeft05=0, rightHeld=1, rightAfter=1;
  float age=0;
  for(int frame=0;age<1.3f;++frame) {
    age=tick(world,frame,birth);
    RagdollBrace::Report report;
    require(body->braceReport(report),"the brace must last");
    if(touchedAt<0.f && report.armMs[0]>=0.f) touchedAt=report.armMs[0]*.001f;
    require(report.armMs[1]<0.f,"the right arm meets nothing");
    const auto probes=body->motorProbes();
    for(size_t bone:{LUpper,LFore,LHand}) {
      const auto& p=probe(probes,bone);
      const float share=p.torqueLimit/(p.massKg*profile.armsTorque);
      if(touchedAt>=0.f && age>=touchedAt+.1f-1.e-4f) worstLeft01=std::max(worstLeft01,share);
      if(touchedAt>=0.f && age>=touchedAt+.5f+1.e-3f) worstLeft05=std::max(worstLeft05,p.on ? 1.f : share);
      }
    for(size_t bone:{RUpper,RFore,RHand}) {
      const auto& p=probe(probes,bone);
      const float share=p.on ? p.torqueLimit/(p.massKg*profile.armsTorque) : 0.f;
      if(age<=profile.armsHold+1.e-4f) rightHeld=std::min(rightHeld,share);
      if(age>=profile.armsHold+.05f) rightAfter=std::min(rightAfter,share);
      }
    }
  std::printf("ragdoll brace yield: left touch %.3f s, left limit share after 0.1 s <= %.3f, after 0.5 s %.3f; "
              "right held share >= %.4f until 0.7 s, then down to %.3f\n",
              double(touchedAt),double(worstLeft01),double(worstLeft05),double(rightHeld),double(rightAfter));
  require(touchedAt>0.f && touchedAt<.7f,"the post must stop the left arm before its hold ends");
  require(worstLeft01<=profile.armsLevel+1.e-4f,"0.1 s after its touch an arm holds at most 35 per cent of its limit");
  require(worstLeft05==0.f,"0.5 s after its touch an arm has no motor torque");
  require(rightHeld>=1.f-1.e-5f,"an arm without a touch holds its full limit until 0.7 s");
  require(rightAfter<1.f,"after 0.7 s an untouched arm gives way too");
  }

void testGapIsNoTouch() {
  // A still body 1 cm short of a wall at each fingertip: Jolt reports speculative contacts
  // there, but surfaces that stay apart are no touch.
  Human human;
  CollisionWorld world;
  RagdollBraceContract::zeroGravity(world);
  auto body=human.create(world,at(0,300,0));
  require(body->brace(Kind::None,nullptr,0,RagdollBrace::Profile{}),"the gap case measures only");
  const auto bounds=RagdollBraceContract::ragdollBounds(world);
  const auto c=bounds.GetCenter();
  auto left =world.addCollisionBody(new JPH::BoxShape(JPH::Vec3(.05f,2,2)),at((bounds.mMin.GetX()-.06f)*100.f,c.GetY()*100.f,c.GetZ()*100.f),.75f,DynamicWorld::C_Landscape);
  auto right=world.addCollisionBody(new JPH::BoxShape(JPH::Vec3(.05f,2,2)),at((bounds.mMax.GetX()+.06f)*100.f,c.GetY()*100.f,c.GetZ()*100.f),.75f,DynamicWorld::C_Landscape);
  for(int frame=0;frame<6;++frame)
    world.tick(17);
  RagdollBrace::Report report;
  require(body->braceReport(report),"the measures must last");
  const auto after=RagdollBraceContract::ragdollBounds(world);
  std::printf("ragdoll brace 1 cm gap: span %.4f -> %.4f m, arm_ms=%.0f/%.0f hand_ms=%.0f/%.0f\n",
              double(bounds.GetSize().GetX()),double(after.GetSize().GetX()),double(report.armMs[0]),double(report.armMs[1]),
              double(report.handMs[0]),double(report.handMs[1]));
  require(std::abs(after.GetSize().GetX()-bounds.GetSize().GetX())<.002f,"the still body keeps its gaps");
  require(report.armMs[0]<0.f && report.armMs[1]<0.f && report.handMs[0]<0.f && report.handMs[1]<0.f,
          "a speculative contact of surfaces 1 cm apart is no touch");
  }

void testLandingPhase() {
  Human human;
  const auto first=human.armsForward(),land=human.landed();
  const auto firstTargets=poseTargets(human,first),landTargets=poseTargets(human,land);
  const RagdollBrace::Profile profile;
  for(bool touchHand:{true,false}) {
    CollisionWorld world,control;
    RagdollBraceContract::zeroGravity(world);
    RagdollBraceContract::zeroGravity(control);
    auto body=human.create(world,at(0,300,0)),reference=human.create(control,at(0,300,0));
    const auto birth=world.simulationStep();
    body->knockOff({0,0,1},30.f); reference->knockOff({0,0,1},30.f);
    require(body->brace(Kind::Front,first.data(),first.size(),profile,land.data()),"the landing case needs both poses");
    float age=0.f;
    int frame=0;
    for(;age<.35f-1.e-4f;++frame) { age=tick(world,frame,birth); tick(control,frame,birth); }
    RagdollBrace::Report report;
    require(body->braceReport(report) && report.landMs<0.f && report.landKind==Kind::Front,"before touch the second pose waits");
    float beforeError=0.f;
    for(size_t bone:{LUpper,LFore,LHand,RUpper,RFore,RHand})
      beforeError=std::max(beforeError,angleDeg(target(probe(body->motorProbes(),bone)),target(probe(firstTargets,bone))));
    require(beforeError<1.f,"before contact the arm targets equal the first pose");
    // Put a small support inside the left hand's current collision capsule. There is
    // no other scenery or gravity; this is a real hand contact, after the first pose.
    Tempest::Vec3 handPosition;
    for(const auto& part:body->state().parts)
      if(part.bone==LHand) handPosition=part.centerOfMassMeters*100.f;
    auto support=touchHand ? world.addCollisionBody(new JPH::BoxShape(JPH::Vec3(.015f,.015f,.015f)),
                        at(handPosition.x,handPosition.y,handPosition.z),.75f,DynamicWorld::C_Landscape) : nullptr;
    float landedAt=-1.f,afterError=0.f,restMin=1.f,restMax=0.f,armAfter=0.f,legFloor=1.e9f,legAfter=0.f;
    float midFromStart=180.f,midToLand=180.f,earlyLegRatio=1.e9f,ordinaryLegMax=0.f;
    size_t restSamples=0,legSamples=0,targetSamples=0,midSamples=0,earlyLegSamples=0,strongerFadeSamples=0;
    std::vector<RagdollBrace::MotorProbe> landingTargets;
    for(;age<1.5f;++frame) {
      age=tick(world,frame,birth);
      tick(control,frame,birth);
      require(world.simulationStep()==control.simulationStep(),"the landing and ordinary fade must have the same age");
      require(body->braceReport(report),"the landing measures last");
      if(report.landMs>=0.f) landedAt=report.landMs*.001f;
      if(landedAt<0.f) continue;
      const float since=age-landedAt;
      const auto motors=body->motorProbes();
      if(landingTargets.empty()) landingTargets=motors;
      if(since>=profile.landEnter*.4f && since<=profile.landEnter*.6f && midSamples==0) {
        for(size_t bone:{LFore,RFore,LCalf,RCalf}) {
          const auto start=target(probe(landingTargets,bone)),end=target(probe(landTargets,bone)),now=target(probe(motors,bone));
          const float fromStart=angleDeg(start,now),toLand=angleDeg(now,end),span=angleDeg(start,end);
          midFromStart=std::min(midFromStart,fromStart); midToLand=std::min(midToLand,toLand);
          require(fromStart>1.f && toLand>1.f && fromStart<span && toLand<span && std::abs(fromStart+toLand-span)<.1f,
                  "midway through landEnter each elbow and knee target lies strictly between its landing-onset and second-pose targets");
          }
        ++midSamples;
        }
      if(since>=profile.landEnter+1.e-3f && targetSamples==0) {
        for(size_t bone:{LUpper,LFore,LHand,RUpper,RFore,RHand,LThigh,LCalf,LFoot,LToe,RThigh,RCalf,RFoot,RToe})
          afterError=std::max(afterError,angleDeg(target(probe(motors,bone)),target(probe(landTargets,bone))));
        ++targetSamples;
        }
      for(size_t bone:{LUpper,LFore,LHand}) {
        const auto& p=probe(motors,bone);
        const float share=p.torqueLimit/(p.massKg*profile.armsTorque);
        if(since>=profile.armsDrop+1.e-3f && since<=profile.armsDrop+profile.armsRest-1.e-3f) {
          restMin=std::min(restMin,share); restMax=std::max(restMax,share); ++restSamples;
          }
        if(since>=profile.armsDrop+profile.armsRest+profile.armsYield+1.e-3f)
          armAfter=std::max(armAfter,share);
        }
      for(size_t bone:{LThigh,LCalf,LFoot,LToe,RThigh,RCalf,RFoot,RToe}) {
        const auto& p=probe(motors,bone);
        if(since>1.e-3f && since<=.1f+1.e-4f) {
          const float ordinary=probe(reference->motorProbes(),bone).torqueLimit;
          const float floor=p.massKg*profile.landTorque,expected=std::max(ordinary,floor);
          earlyLegRatio=std::min(earlyLegRatio,p.torqueLimit/expected);
          ordinaryLegMax=std::max(ordinaryLegMax,ordinary/p.massKg);
          ++earlyLegSamples;
          if(ordinary>floor+1.e-4f) ++strongerFadeSamples;
          require(p.torqueLimit>=expected-1.e-4f,"in the first 0.1 s landed legs keep the greater of ordinary fade and mass times landTorque");
          }
        if(since>=.1f && since<=profile.landHold-1.e-3f) {
          legFloor=std::min(legFloor,p.torqueLimit/p.massKg); ++legSamples;
          }
        if(since>=profile.landHold+profile.landYield+1.e-3f)
          legAfter=std::max(legAfter,p.torqueLimit);
        }
      }
    std::printf("ragdoll brace landing %s: land_ms=%.3f hand_ms=%.3f/%.3f target_error_deg before=%.5f after=%.5f; "
                "mid_target_deg from_start=%.5f to_land=%.5f; arm_rest_share=%.5f..%.5f after_yield=%.5f; "
                "leg_floor_Nm_kg=%.5f after_yield_Nm=%.5f early_min_ratio=%.5f ordinary_max_Nm_kg=%.5f stronger_fade_samples=%zu\n",
                touchHand ? "hand" : "timeout",double(report.landMs),double(report.handMs[0]),double(report.handMs[1]),
                double(beforeError),double(afterError),double(midFromStart),double(midToLand),double(restMin),double(restMax),
                double(armAfter),double(legFloor),double(legAfter),double(earlyLegRatio),double(ordinaryLegMax),strongerFadeSamples);
    require(midSamples>0,"the landing transition must be sampled near landEnter/2");
    require(earlyLegSamples>0 && (!touchHand || strongerFadeSamples>0),"the early landing samples must include a stronger ordinary leg fade");
    require(targetSamples>0 && afterError<1.f,"after landEnter the targets equal the second pose within one degree");
    if(touchHand)
      require(report.handMs[0]>=0.f && std::abs(report.landMs-report.handMs[0])<.01f && landedAt<profile.armsHold,
              "the first hand contact starts landing before the timeout");
    else
      require(report.handMs[0]<0.f && report.handMs[1]<0.f && report.torsoMs<0.f && std::abs(landedAt-profile.armsHold)<.001f,
              "without contact landing starts at armsHold from birth");
    require(restSamples>0 && std::abs(restMin-profile.armsLevel)<1.e-4f && std::abs(restMax-profile.armsLevel)<1.e-4f,
            "the arm keeps armsLevel for armsRest after its drop");
    require(armAfter==0.f,"after armsRest and armsYield the arm torque is zero");
    require(legSamples>0 && legFloor>=profile.landTorque-1.e-4f,"landed legs keep at least mass times landTorque");
    require(legAfter==0.f,"after landHold and landYield the leg torque is zero");
    }
  }

void testSideLegSwitch() {
  Human human;
  const auto pose=human.landed(),land=human.armsForward();
  const auto posed=poseTargets(human,pose);
  RagdollBrace::Profile profile;
  require(RagdollBrace::setNumber(profile,"land","enter",profile.landEnter) &&
          RagdollBrace::setNumber(profile,"land","hold",profile.landHold) &&
          RagdollBrace::setNumber(profile,"land","yield",profile.landYield) &&
          RagdollBrace::setNumber(profile,"land","torque",profile.landTorque) &&
          RagdollBrace::setNumber(profile,"arms","rest",profile.armsRest) &&
          RagdollBrace::setNumber(profile,"legs","front",profile.legsFront) &&
          RagdollBrace::setNumber(profile,"legs","back",profile.legsBack) &&
          !RagdollBrace::setNumber(profile,"legs","side",.5f),"brace-profile accepts the new numbers and rejects a fractional leg switch");
  for(const auto kind:{Kind::Left,Kind::Right}) {
    for(float enabled:{0.f,1.f}) {
      CollisionWorld world;
      RagdollBraceContract::zeroGravity(world);
      auto body=human.create(world,at(0,300,0));
      const auto birth=world.simulationStep();
      const auto initial=body->motorProbes();
      require(RagdollBrace::setNumber(profile,"legs","side",enabled) &&
              RagdollBrace::setNumber(profile,"legs","front",1.f-enabled) &&
              RagdollBrace::setNumber(profile,"legs","back",1.f-enabled),"brace-profile accepts opposite side and front/back leg switches");
      require(body->brace(kind,pose.data(),pose.size(),profile,land.data()),"the side switch needs both poses");
      float birthError=0.f,poseError=0.f;
      for(int frame=0;tick(world,frame,birth)<.4f;++frame) {
        const auto motors=body->motorProbes();
        for(size_t bone:{LThigh,LCalf,LFoot,LToe,RThigh,RCalf,RFoot,RToe}) {
          birthError=std::max(birthError,angleDeg(target(probe(motors,bone)),target(probe(initial,bone))));
          if(float(world.simulationStep()-birth)/60.f>=profile.legsEnter)
            poseError=std::max(poseError,angleDeg(target(probe(motors,bone)),target(probe(posed,bone))));
          }
        }
      RagdollBrace::Report report;
      require(body->braceReport(report) && report.landMs<0.f,"the side switch is measured before landing");
      std::printf("ragdoll brace %.*s legs side %.0f front/back %.0f: birth_target_error_deg=%.5f first_pose_error_deg=%.5f\n",
                  int(RagdollBrace::clipName(kind).size()),RagdollBrace::clipName(kind).data(),double(enabled),double(1.f-enabled),
                  double(birthError),double(poseError));
      require(enabled==0.f ? birthError<.05f : birthError>30.f && poseError<.05f,
              "legsSide alone chooses whether side-fall leg targets stay at birth or enter the first pose");
      }
    }
  }

// The physics half of "death ends the protection": Npc::onNoHealth calls endBrace through
// Npc::finishRagdollBrace; this contract has no Npc, so it ends the brace itself.
void testEndBraceRestoresFade() {
  Human human;
  for(float deathAge:{.2f,.9f,1.5f}) {
    const bool secondPose=deathAge>.2f;
    CollisionWorld world,control;
    auto floor=ground(world),controlFloor=ground(control);
    auto body=human.create(world,at(0,1,0)),reference=human.create(control,at(0,1,0));
    const auto birth=world.simulationStep();
    body->knockOff({0,0,1},30.f); reference->knockOff({0,0,1},30.f);
    const auto pose=human.armsForward();
    const auto land=human.landed();
    require(body->brace(Kind::Front,pose.data(),pose.size(),RagdollBrace::Profile{},secondPose ? land.data() : nullptr),"the end case needs an accepted pose");
    for(int frame=0;float(world.simulationStep()-birth)/60.f<deathAge;++frame) {
      tick(world,frame,birth);
      tick(control,frame,birth);
      }
    require(world.simulationStep()==control.simulationStep(),"both bodies must be at the same age");
    RagdollBrace::Report report;
    if(secondPose)
      require(body->braceReport(report) && report.landMs>=0.f,"death in phase two needs an already landed body");
    const auto braced=body->motorProbes();
    if(deathAge<1.4f)
      require(probe(braced,secondPose ? LCalf : LUpper).torqueLimit!=probe(reference->motorProbes(),secondPose ? LCalf : LUpper).torqueLimit,
              "the braced motor differs from the fade");
    else
      require(std::memcmp(&probe(braced,LFore).target,&probe(reference->motorProbes(),LFore).target,sizeof(Tempest::Vec4))!=0,
              "after torque expiry the landed target still differs from the fade");
    body->endBrace();
    require(!body->braceReport(report),"endBrace ends the protection");
    world.tick(17); control.tick(17);
    const auto a=body->motorProbes(), b=reference->motorProbes();
    require(a.size()==b.size() && !a.empty(),"both bodies have the same motors");
    for(size_t i=0;i<a.size();++i)
      require(a[i].bone==b[i].bone && a[i].on==b[i].on && std::memcmp(&a[i].torqueLimit,&b[i].torqueLimit,sizeof(float))==0 &&
              std::memcmp(&a[i].target,&b[i].target,sizeof(a[i].target))==0,
              "the step after endBrace drives the ordinary fade: the same targets and limits as a body never braced");
    std::printf("ragdoll brace end phase %d age=%.3f s: %zu motors equal the ordinary fade in the next step\n",secondPose ? 2 : 1,double(deathAge),a.size());
    }
  }

void testClampedTarget() {
  // A left elbow bent 170 degrees: 25 past its 145-degree hinge.
  Human human;
  CollisionWorld world;
  RagdollBraceContract::zeroGravity(world);
  auto body=human.create(world,at(0,300,0));
  const auto birth=world.simulationStep();
  const auto pose=human.armsForward(170.f);
  require(body->brace(Kind::Front,pose.data(),pose.size(),RagdollBrace::Profile{}),"an over-bent pose is still accepted");
  RagdollBrace::Report report;
  require(body->braceReport(report),"the clamp must be reported");
  const float clamp=probe(body->motorProbes(),LFore).clampDeg;
  // Every step of the way into the pose, not only its end: the motors get only targets inside the limits.
  float request=0;
  const auto worst=[&] { for(const auto& p:body->motorProbes()) request=std::max(request,p.requestDeg); };
  worst();
  for(int frame=0;tick(world,frame,birth)<.35f;++frame)
    worst();
  std::printf("ragdoll brace clamp: max %.3f deg at %s (forearm %.3f), largest target the limits moved on any step %.5f deg\n",
              double(report.clampMaxDeg),report.clampBone.c_str(),double(clamp),double(request));
  require(report.clampBone=="BIP01 L FOREARM" && std::abs(clamp-25.f)<1.f && std::abs(report.clampMaxDeg-clamp)<1.e-4f,
          "a target past the elbow's range is clamped to it once, by the excess");
  require(request<.05f,"the motors never receive a target outside their joint limits");
  }

// The unbraced fall below on main before the brace code (merge base ce07b507b, Mac and Linux):
// an independent reference. A deliberate change of the knock-off fall updates it.
constexpr uint64_t mainFallDigest=0xae473cf18cd65887ull;

uint64_t digest(const Ragdoll::State& state) {
  uint64_t hash=1469598103934665603ull;
  const auto add=[&](const void* data, size_t size) {
    for(size_t i=0;i<size;++i) { hash^=static_cast<const unsigned char*>(data)[i]; hash*=1099511628211ull; }
    };
  for(const auto& p:state.parts) {
    add(p.transform.data(),16*sizeof(float));
    add(&p.centerOfMassMeters,sizeof(p.centerOfMassMeters)); add(&p.rotation,sizeof(p.rotation));
    add(&p.linearVelocityMeters,sizeof(p.linearVelocityMeters)); add(&p.angularVelocity,sizeof(p.angularVelocity));
    }
  return hash;
  }

void testUnbracedUnchanged() {
  // A fall without a pose (a model without the clips) or with the flag off never calls brace:
  // its parts after 2 s are bit for bit those of a rig that only measured, or refused a pose.
  Human human;
  uint64_t hashes[3]={};
  for(int variant=0;variant<3;++variant) {
    CollisionWorld world;
    auto floor=ground(world);
    auto body=human.create(world,at(0,1,0));
    const auto birth=world.simulationStep();
    body->knockOff({0,0,1},30.f);
    if(variant==1) require(body->brace(Kind::None,nullptr,0,RagdollBrace::Profile{}),"a rig may only measure its fall");
    if(variant==2) require(!body->brace(Kind::Front,nullptr,0,RagdollBrace::Profile{}),"a brace without a pose is refused");
    for(int frame=0;tick(world,frame,birth)<2.f;++frame) {}
    hashes[variant]=digest(body->state());
    }
  std::printf("ragdoll brace unbraced fall digest=%016llx measured=%016llx refused=%016llx\n",
              (unsigned long long)hashes[0],(unsigned long long)hashes[1],(unsigned long long)hashes[2]);
  require(hashes[0]==mainFallDigest,"a fall without a pose is bit for bit the fall of the code before the brace");
  require(hashes[0]==hashes[1] && hashes[0]==hashes[2],"measuring or refusing a pose must not change the fall by one bit");
  }

void testSinglePoseUnchanged() {
  // Captured before adding the landing phase: all native part positions, orientations
  // and velocities after two seconds, with the original single-pose profile.
  Human human;
  const Kind kinds[]={Kind::Front,Kind::Back,Kind::Left,Kind::Right};
  const Tempest::Vec3 throws[]={{0,0,1},{0,0,-1},{1,0,0},{-1,0,0}};
  constexpr uint64_t expected[]={0xe4d49eeb6c33f417ull,0xbf297ce1c5e0d71aull,0x808500ad0404c278ull,0xaee2c61b88c0d2c0ull};
  for(size_t i=0;i<4;++i) {
    for(bool changedNumbers:{false,true}) {
      CollisionWorld world;
      auto floor=ground(world);
      auto body=human.create(world,at(0,1,0));
      const auto birth=world.simulationStep();
      body->knockOff(throws[i],30.f);
      const auto pose=human.armsForward();
      RagdollBrace::Profile numbers;
      if(changedNumbers) {
        numbers.landEnter=5.f; numbers.landHold=5.f; numbers.landYield=5.f; numbers.landTorque=500.f;
        numbers.armsRest=5.f; numbers.legsFront=1.f; numbers.legsBack=0.f; numbers.legsSide=0.f;
        }
      require(body->brace(kinds[i],pose.data(),pose.size(),numbers),"the legacy case needs its first pose");
      for(int frame=0;tick(world,frame,birth)<2.f;++frame) {}
      const auto actual=digest(body->state());
      RagdollBrace::Report report;
      require(body->braceReport(report) && report.landKind==Kind::None && report.landMs<0.f,"without a second pose there is no landing phase");
      std::printf("ragdoll brace single pose %.*s changed_new_numbers=%d digest=%016llx\n",int(RagdollBrace::clipName(kinds[i]).size()),
                  RagdollBrace::clipName(kinds[i]).data(),changedNumbers,(unsigned long long)actual);
      require(actual==expected[i],"without the second pose all parts remain bit for bit the pre-change single-pose fall");
      }
    }
  }

void testAuthoredPoses() {
  const char* dir=std::getenv("OPENGOTHIC_BRACE_POSES");
  if(dir==nullptr || dir[0]==0) {
    std::puts("ragdoll brace authored poses: skipped, set OPENGOTHIC_BRACE_POSES=<content folder with S_BRACE and S_BRACELAND records>");
    return;
    }
  const char* base=std::getenv("OPENGOTHIC_BASE");
  require(base!=nullptr && base[0]!=0,"OPENGOTHIC_BRACE_POSES reads the human hierarchy from OPENGOTHIC_BASE/models/HUMANS.MDH.json");
  Skeleton skeleton{ModelData::readHierarchy(std::filesystem::path(base)/"models"/"HUMANS.MDH.json"),nullptr,"HUMANS.MDS"};
  HitZones zones;
  zones.setSkeleton(&skeleton);
  for(const auto kind:{Kind::Front,Kind::Back,Kind::Left,Kind::Right}) {
    std::vector<Tempest::Matrix4x4> first;
    for(bool secondPose:{false,true}) {
      const auto clip=std::string(secondPose ? RagdollBrace::landClipName(kind) : RagdollBrace::clipName(kind));
      const auto anim=ModelData::readAnimation(std::filesystem::path(dir)/"models"/("HUMANS-"+clip+".MAN.json"));
      require(anim.frame_count>=1 && anim.node_indices.size()==anim.node_count && anim.samples.size()>=anim.node_count,
              "an authored brace record must hold one full frame");
      // Frame 0 in model space: the same chain as Pose::animRelative from the model frame.
      std::vector<Tempest::Matrix4x4> model(skeleton.nodes.size());
      for(size_t i=0;i<skeleton.nodes.size();++i) {
        auto local=skeleton.nodes[i].tr;
        for(size_t k=0;k<anim.node_indices.size();++k)
          if(anim.node_indices[k]==i) local=mkMatrix(anim.samples[k]);
        const size_t parent=skeleton.nodes[i].parent;
        require(parent==size_t(-1) || parent<i,"the hierarchy lists parents first");
        model[i]=parent==size_t(-1) ? local : model[parent]*local;
        }
      CollisionWorld world;
      auto body=world.createRagdoll(skeleton,zones,skeleton.tr.data(),skeleton.tr.size(),Tempest::Matrix4x4::mkIdentity());
      require(body && body->valid(),"the human hierarchy makes a ragdoll");
      require(body->brace(kind,model.data(),model.size(),RagdollBrace::Profile{}),"an authored pose is accepted");
      RagdollBrace::Report report;
      require(body->braceReport(report),"an authored pose reports its clamp");
      // Read actual frame samples, including elbow flexion, rather than the bind pose.
      const auto elbow=[&](const char* side) {
        const auto bone=[&](const char* part) { return skeleton.findNode(std::string("BIP01 ")+side+" "+part); };
        const auto upper=origin(model[bone("FOREARM")])-origin(model[bone("UPPERARM")]);
        const auto lower=origin(model[bone("HAND")])-origin(model[bone("FOREARM")]);
        return std::acos(std::clamp(Tempest::Vec3::dotProduct(Tempest::Vec3::normalize(upper),Tempest::Vec3::normalize(lower)),-1.f,1.f))*180.f/pi;
        };
      float allClamp=0.f;
      const char* clampBone="-";
      const auto motors=body->motorProbes();
      for(const auto& p:motors)
        if(p.clampDeg>allClamp) {
          allClamp=p.clampDeg;
          clampBone=skeleton.nodes[p.bone].name.c_str();
          }
      std::printf("ragdoll brace authored %s: elbows %.1f/%.1f deg, clamp_max_deg=%.3f at %s;",clip.c_str(),double(elbow("L")),
                  double(elbow("R")),double(allClamp),clampBone);
      for(const auto& p:motors) {
        if(p.clampDeg>=.05f)
          std::printf(" %s=%.2f",skeleton.nodes[p.bone].name.c_str(),double(p.clampDeg));
        }
      std::printf("\n");
      require(allClamp<=2.f,"every joint of each authored pose lies within 2 degrees of its limits");
      if(!secondPose) first=model;
      else {
        auto paired=world.createRagdoll(skeleton,zones,skeleton.tr.data(),skeleton.tr.size(),Tempest::Matrix4x4::mkIdentity());
        require(paired && paired->brace(kind,first.data(),first.size(),RagdollBrace::Profile{},model.data()),"the two authored poses are accepted together");
        for(const auto& p:paired->motorProbes())
          require(p.clampDeg<=2.f,"both accepted poses are clamped within two degrees in each paired joint");
        }
      }
    }
  }

}

void testFallBrace() {
  testSelection();
  testReachWithoutGravity();
  testForwardFall();
  testYieldAfterTouch();
  testGapIsNoTouch();
  testLandingPhase();
  testSideLegSwitch();
  testEndBraceRestoresFade();
  testClampedTarget();
  testUnbracedUnchanged();
  testSinglePoseUnchanged();
  testAuthoredPoses();
  std::puts("Ragdoll brace contracts passed");
  }
