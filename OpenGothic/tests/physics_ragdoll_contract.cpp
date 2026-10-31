#include "asset/fromzenkit.h"
#include "baseimport.h"
#include "commandline.h"
#include "physics/collisionworld.h"
#include "physics/dynamicworld.h"
#include "physics/ragdoll.h"
#include "physics/physicmeshshape.h"
#include "graphics/bounds.h"
#include "graphics/mesh/hitzones.h"
#include "graphics/mesh/protomesh.h"
#include "graphics/mesh/skeleton.h"
#include "graphics/mesh/pose.h"
#include "graphics/mesh/animmath.h"
#include "graphics/mesh/ikmath.h"
#include "graphics/mesh/submesh/packedmesh.h"
#include "game/serialize.h"

#include <Tempest/MemReader>
#include <Tempest/MemWriter>

#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>

#include <zenkit/ModelHierarchy.hh>
#include <zenkit/ModelScript.hh>
#include <zenkit/Stream.hh>
#include <zenkit/Vfs.hh>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unistd.h>
#include <vector>

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
#include <rapidjson/document.h>
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

static_assert(std::is_copy_constructible_v<Pose> && std::is_copy_assignable_v<Pose>,
              "origin replay tag must preserve the complete public Pose value copy");

// The contract keeps the legacy full-copy origin replay as an independent
// baseline. It never reimplements the new constructor's member selection.
class PoseOriginReplayContract final {
  public:
    static std::vector<Tempest::Matrix4x4> legacy(const Pose& source) {
      Pose scratch=source;
      scratch.clearRagdollPose();
      return replay(scratch,source);
      }
    static std::vector<Tempest::Matrix4x4> prepared(const Pose& source) {
      Pose scratch(source,Pose::OriginReplayTag{});
      return replay(scratch,source);
      }
    static void seed(Pose& pose, size_t bone, const Tempest::Matrix4x4& value) {
      pose.tr[bone]=value;
      }
  private:
    static std::vector<Tempest::Matrix4x4> replay(Pose& scratch, const Pose& source) {
      auto object=source.pos;
      object.set(3,0,0.f); object.set(3,1,0.f); object.set(3,2,0.f);
      scratch.mkSkeleton(object,source.pos.at(3,1));
      return std::vector<Tempest::Matrix4x4>(scratch.tr,scratch.tr+source.numBones);
      }
  };

namespace {

void require(bool condition, const char* message) {
  if(condition)
    return;
  std::fprintf(stderr,"FAIL: %s\n",message);
  std::exit(EXIT_FAILURE);
  }

void near(float actual, float expected, float tolerance, const char* message) {
  if(std::isfinite(actual) && std::abs(actual-expected)<=tolerance)
    return;
  std::fprintf(stderr,"FAIL: %s: %.9g versus %.9g (+/- %.9g)\n",
               message,double(actual),double(expected),double(tolerance));
  std::exit(EXIT_FAILURE);
  }

void exact(float actual, float expected, const char* message) {
  require(std::memcmp(&actual,&expected,sizeof(float))==0,message);
  }

Tempest::Matrix4x4 at(float x, float y, float z) {
  auto matrix = Tempest::Matrix4x4::mkIdentity();
  matrix.translate(x,y,z);
  return matrix;
  }

Tempest::Vec3 position(const Tempest::Matrix4x4& matrix) {
  Tempest::Vec3 result;
  matrix.project(result);
  return result;
  }

float scaleVolume(const Tempest::Matrix4x4& matrix) {
  // Accepted object frames have orthogonal axes; their lengths give an
  // independent volume oracle without using the production determinant helper.
  const auto* m=matrix.data();
  return Tempest::Vec3(m[0],m[1],m[2]).length()*Tempest::Vec3(m[4],m[5],m[6]).length()*
         Tempest::Vec3(m[8],m[9],m[10]).length();
  }

zenkit::ModelHierarchy hierarchy() {
  zenkit::ModelHierarchy source{};
  struct Bone { const char* name; int16_t parent; Tempest::Vec3 offset; };
  // A small human hierarchy with actual limb chains and a nonphysical root.
  const Bone bones[] = {
    {"BIP01",-1,{0,0,0}}, {"BIP01 PELVIS",0,{0,95,0}},
    {"BIP01 SPINE",1,{0,22,0}}, {"BIP01 SPINE1",2,{0,22,0}},
    {"BIP01 NECK",3,{0,15,0}}, {"BIP01 HEAD",4,{0,15,0}},
    {"BIP01 L UPPERARM",3,{25,0,0}}, {"BIP01 L FOREARM",6,{25,0,0}},
    {"BIP01 L HAND",7,{23,0,0}},
    {"BIP01 R UPPERARM",3,{-25,0,0}}, {"BIP01 R FOREARM",9,{-25,0,0}},
    {"BIP01 R HAND",10,{-23,0,0}},
    {"BIP01 L THIGH",1,{12,-8,0}}, {"BIP01 L CALF",12,{0,-42,0}},
    {"BIP01 L FOOT",13,{0,-40,0}}, {"BIP01 L TOE",14,{0,0,15}},
    {"BIP01 R THIGH",1,{-12,-8,0}}, {"BIP01 R CALF",16,{0,-42,0}},
    {"BIP01 R FOOT",17,{0,-40,0}}, {"BIP01 R TOE",18,{0,0,15}},
    {"ZS_HELMET",5,{0,12,0}}
    };
  for(const auto& bone:bones) {
    zenkit::ModelHierarchyNode node;
    node.name = bone.name;
    node.parent_index = bone.parent;
    const auto transform = at(bone.offset.x,bone.offset.y,bone.offset.z);
    static_assert(sizeof(node.transform)==sizeof(transform));
    std::memcpy(&node.transform,&transform,sizeof(transform));
    source.nodes.push_back(node);
    }
  source.bbox.min = {-80,0,-20}; source.bbox.max = {80,190,25};
  source.collision_bbox = source.bbox;
  return source;
  }

struct Fixture final {
  Skeleton skeleton{asset::fromZenKit(hierarchy()),nullptr,"HUMANS.MDS"};
  HitZones zones;

  Fixture() { zones.setSkeleton(&skeleton); require(zones.isValid(),"synthetic hierarchy must have hit capsules"); }

  std::unique_ptr<Ragdoll> create(CollisionWorld& world, const Tempest::Matrix4x4& object,
                                const Tempest::Vec3& velocity = {}) const {
    std::vector<Tempest::Matrix4x4> pose;
    for(const auto& matrix:skeleton.tr)
      pose.push_back(object*matrix);
    auto body = world.createRagdoll(skeleton,zones,pose.data(),pose.size(),object,velocity);
    require(body!=nullptr && body->valid(),"valid capsule hierarchy must create a ragdoll");
    require(body->partCount()>10 && body->partCount()<skeleton.nodes.size(),"physical parts must cover limbs and exclude slots/root");
    require(body->boneCount()==skeleton.nodes.size(),"pose output must retain skeleton indexing");
    near(body->mass(),75*scaleVolume(object),.001f,"human native mass must include the object's volume scale");
    return body;
    }
  };

void finite(const Ragdoll::State& state) {
  require(!state.parts.empty(),"ragdoll snapshot must have parts");
  for(const auto& part:state.parts) {
    for(size_t i=0;i<16;++i)
      require(std::isfinite(part.transform.data()[i]),"physical transform must remain finite");
    if(state.version>=2)
      for(size_t i=0;i<16;++i)
        require(std::isfinite(part.referenceTransform.data()[i]),"initial affine reference must remain finite");
    require(std::isfinite(part.linearVelocity.x) && std::isfinite(part.linearVelocity.y) &&
            std::isfinite(part.linearVelocity.z),"linear velocity must remain finite");
    require(std::isfinite(part.angularVelocity.x) && std::isfinite(part.angularVelocity.y) &&
            std::isfinite(part.angularVelocity.z),"angular velocity must remain finite");
    if(state.version>=3)
      require(std::isfinite(part.centerOfMassMeters.x) && std::isfinite(part.centerOfMassMeters.y) &&
              std::isfinite(part.centerOfMassMeters.z) && std::isfinite(part.rotation.x) && std::isfinite(part.rotation.y) &&
              std::isfinite(part.rotation.z) && std::isfinite(part.rotation.w) &&
              std::isfinite(part.linearVelocityMeters.x) && std::isfinite(part.linearVelocityMeters.y) &&
              std::isfinite(part.linearVelocityMeters.z),"precise native COM/quaternion/velocity must remain finite");
    }
  if(state.version>=3)
    for(const auto& ancestor:state.ancestors)
      for(size_t i=0;i<16;++i) require(std::isfinite(ancestor.referenceTransform.data()[i]),"historic ancestor reference must remain finite");
  }

void same(const Ragdoll::State& actual, const Ragdoll::State& expected, float positionTolerance) {
  require(actual.version==expected.version && actual.skeleton==expected.skeleton &&
          actual.parts.size()==expected.parts.size(),"restored snapshot identity must match");
  for(size_t i=0;i<16;++i)
    near(actual.rootTransform.data()[i],expected.rootTransform.data()[i],
         i>=12 && i<=14 ? positionTolerance : .00001f,"restored root transform must match");
  for(size_t p=0;p<actual.parts.size();++p) {
    const auto& a=actual.parts[p]; const auto& e=expected.parts[p];
    require(a.bone==e.bone && a.sleeping==e.sleeping,"restored bone identity and sleep flag must match");
    for(size_t i=0;i<16;++i)
      near(a.transform.data()[i],e.transform.data()[i],
           i>=12 && i<=14 ? positionTolerance : .00001f,"restored part transform must match");
    if(actual.version>=2)
      for(size_t i=0;i<16;++i)
        near(a.referenceTransform.data()[i],e.referenceTransform.data()[i],
             i>=12 && i<=14 ? .001f : .00001f,"restored immutable affine reference must match");
    near((a.linearVelocity-e.linearVelocity).length(),0,.001f,"restored linear velocity must match in cm/s");
    near((a.angularVelocity-e.angularVelocity).length(),0,.00001f,"restored angular velocity must match in radians/s");
    if(actual.version>=3 && positionTolerance<=.001f) {
      exact(a.centerOfMassMeters.x,e.centerOfMassMeters.x,"instant restore must preserve native COM x bits");
      exact(a.centerOfMassMeters.y,e.centerOfMassMeters.y,"instant restore must preserve native COM y bits");
      exact(a.centerOfMassMeters.z,e.centerOfMassMeters.z,"instant restore must preserve native COM z bits");
      exact(a.linearVelocityMeters.x,e.linearVelocityMeters.x,"instant restore must preserve native velocity x bits");
      exact(a.linearVelocityMeters.y,e.linearVelocityMeters.y,"instant restore must preserve native velocity y bits");
      exact(a.linearVelocityMeters.z,e.linearVelocityMeters.z,"instant restore must preserve native velocity z bits");
      exact(a.rotation.x,e.rotation.x,"instant restore must preserve native quaternion x bits");
      exact(a.rotation.y,e.rotation.y,"instant restore must preserve native quaternion y bits");
      exact(a.rotation.z,e.rotation.z,"instant restore must preserve native quaternion z bits");
      exact(a.rotation.w,e.rotation.w,"instant restore must preserve native quaternion w bits");
      }
    }
  if(actual.version>=3) {
    require(actual.legacyAncestors==expected.legacyAncestors && actual.ancestors.size()==expected.ancestors.size(),"restored ancestor topology/legacy mode must match");
    for(size_t a=0;a<actual.ancestors.size();++a) {
      const auto& got=actual.ancestors[a]; const auto& want=expected.ancestors[a];
      require(got.bone==want.bone && got.anchorBone==want.anchorBone,"restored ancestor mapping must match");
      for(size_t i=0;i<16;++i) near(got.referenceTransform.data()[i],want.referenceTransform.data()[i],0,"ancestor birth references must remain exactly immutable");
      }
    }
  }

void joints(const Fixture& fixture, const Ragdoll& body, int frame=-1) {
  std::vector<Tempest::Matrix4x4> pose;
  require(body.getPose(pose) && pose.size()==fixture.skeleton.nodes.size(),"physical pose must be available by bone");
  const auto& indices=body.boneIndices();
  for(auto bone:indices) {
    const auto parent=fixture.skeleton.nodes[bone].parent;
    if(std::find(indices.begin(),indices.end(),parent)==indices.end())
      continue;
    const auto bindLength=(position(fixture.skeleton.tr[bone])-position(fixture.skeleton.tr[parent])).length();
    const auto actualLength=(position(pose[bone])-position(pose[parent])).length();
    if(!std::isfinite(actualLength) || std::abs(actualLength-bindLength)>5.f) {
      const auto childAt=position(pose[bone]), parentAt=position(pose[parent]);
      std::fprintf(stderr,"joint diagnostic frame=%d child=%zu %s parent=%zu %s child_cm=[%.9g,%.9g,%.9g] parent_cm=[%.9g,%.9g,%.9g]\n",
                   frame,bone,fixture.skeleton.nodes[bone].name.c_str(),parent,fixture.skeleton.nodes[parent].name.c_str(),
                   double(childAt.x),double(childAt.y),double(childAt.z),double(parentAt.x),double(parentAt.y),double(parentAt.z));
      for(const auto& part:body.state().parts)
        if(part.bone==bone || part.bone==parent)
          std::fprintf(stderr,"joint velocity bone=%u linear_cm_s=[%.9g,%.9g,%.9g] angular_rad_s=[%.9g,%.9g,%.9g]\n",part.bone,
                       double(part.linearVelocity.x),double(part.linearVelocity.y),double(part.linearVelocity.z),
                       double(part.angularVelocity.x),double(part.angularVelocity.y),double(part.angularVelocity.z));
      }
    near(actualLength,bindLength,5.f,
         "joint anchors must not separate adjacent bones by more than five centimeters");
    }
  }

auto floor(CollisionWorld& world, float friction=.75f) {
  return world.addCollisionBody(new JPH::BoxShape(JPH::Vec3(20,.5f,20)),at(0,-50,0),
                                friction,DynamicWorld::C_Landscape);
  }

void testKinematicPoseAndInteraction() {
  Fixture fixture;
  {
    CollisionWorld world;
    const auto emptyCount=world.bodyCount();
    auto thinSource=hierarchy(); thinSource.nodes.resize(1);
    Skeleton thin(asset::fromZenKit(thinSource),nullptr,"NO_PHYSICAL_BONES.MDS");
    HitZones thinZones; thinZones.setSkeleton(&thin);
    require(!world.createKinematicBody(thin,thinZones,thin.tr.data(),thin.tr.size(),at(0,0,0)),
            "a skeleton without compatible physical owners must safely decline a live rig");
    require(world.bodyCount()==emptyCount,"an incompatible living skeleton must leave no native parts");
    auto object=at(23000,500,-18000); object.rotateOY(31); object.scale(.9f,1.f,1.f);
    auto pose=fixture.skeleton.tr;
    for(auto& bone:pose) bone=object*bone;
    auto body=world.createKinematicBody(fixture.skeleton,fixture.zones,pose.data(),pose.size(),object);
    require(body && body->isKinematic(),"an animation-driven rig must create independent kinematic parts");
    require(world.bodyCount()==emptyCount+body->partCount(),"every live capsule owner must belong to the shared physics world");
    const auto compare=[&] {
      std::vector<Tempest::Matrix4x4> actual;
      require(body->getPose(actual),"kinematic native pose must be readable");
      for(auto bone:body->poseIndices())
        for(size_t component=0;component<16;++component)
          near(actual[bone].data()[component],pose[bone].data()[component],
               component>=12 && component<=14 ? .02f : .00002f,
               "kinematic parts and ancestor frames must reproduce the requested animation pose");
      };
    compare();
    for(int frame=0;frame<6;++frame) {
      // Independent changes to head, forearm and calf detect rigs that update
      // only their root or accidentally retain the initial capsule orientation.
      for(size_t bone:{size_t(5),size_t(7),size_t(13)}) {
        pose[bone].rotateOZ(7.f);
        pose[bone].set(3,0,pose[bone].at(3,0)+float(bone));
        }
      require(body->moveKinematic(pose.data(),pose.size(),object,.007f),"a ready animation pose must queue without another solve");
      world.tick(17);
      compare();
      }
    for(int frame=0;frame<120;++frame) world.tick(17);
    compare();
    auto invalid=pose;
    invalid[7].set(3,0,std::numeric_limits<float>::quiet_NaN());
    require(!body->moveKinematic(invalid.data(),invalid.size(),object,.017f),"invalid live poses must be rejected atomically");
    world.tick(17); compare();
    const auto pelvis=position(body->pelvisTransform());
    require(world.rayCast(pelvis+Tempest::Vec3(0,0,300),pelvis-Tempest::Vec3(0,0,300),1u<<DynamicWorld::C_LivingBody).hasHit,
            "live geometry must be queryable only when its layer is explicitly requested");
    require(!world.rayCast(pelvis+Tempest::Vec3(0,0,300),pelvis-Tempest::Vec3(0,0,300),
                          (1u<<DynamicWorld::C_Landscape)|(1u<<DynamicWorld::C_Object)|(1u<<DynamicWorld::C_Ragdoll)).hasHit,
            "the established world and corpse hit layers must exclude living rigs");
    require(body->state().parts.empty(),"transient animation rigs must not export corpse save data");
    body.reset();
    require(world.bodyCount()==emptyCount,"destroying a live rig must remove every kinematic part");
    }
  {
    CollisionWorld world;
    auto ground=floor(world,.1f);
    auto item=world.addDynamicBody(new JPH::BoxShape(JPH::Vec3::sReplicate(.15f)),at(0,15,0),.1f,1.f);
    auto object=at(-100,-75,0);
    auto pose=fixture.skeleton.tr;
    const auto update=[&] { for(size_t i=0;i<pose.size();++i) pose[i]=object*fixture.skeleton.tr[i]; };
    update();
    auto body=world.createKinematicBody(fixture.skeleton,fixture.zones,pose.data(),pose.size(),object);
    require(bool(body),"a live rig must be created for native contact checks");
    for(int frame=0;frame<50;++frame) {
      object.set(3,0,-100.f+2.f*float(frame+1)); update();
      require(body->moveKinematic(pose.data(),pose.size(),object,.017f),"a translated live rig must accept the next animation pose");
      world.tick(17);
      }
    require(item->position().x>15.f,"moving live capsules must transfer motion to a dynamic item through native contacts");
    }
  {
    CollisionWorld world,control;
    auto ground=floor(world,.1f),controlGround=floor(control,.1f);
    auto item=world.addDynamicBody(new JPH::BoxShape(JPH::Vec3::sReplicate(.15f)),at(0,15,0),.1f,1.f);
    auto reference=control.addDynamicBody(new JPH::BoxShape(JPH::Vec3::sReplicate(.15f)),at(0,15,0),.1f,1.f);
    auto object=at(-25,-75,0);
    auto pose=fixture.skeleton.tr;
    for(size_t i=0;i<pose.size();++i) pose[i]=object*fixture.skeleton.tr[i];
    auto body=world.createKinematicBody(fixture.skeleton,fixture.zones,pose.data(),pose.size(),object);
    require(bool(body),"teleport contact check needs a live rig touching a dynamic item");
    const auto parts=world.bodyCount();
    // Each queued change is below the guard. Only the accumulated distance
    // from the real native frame reveals the 80 cm script/interaction jump.
    for(int step=0;step<2;++step) {
      object.set(3,0,object.at(3,0)+40.f);
      for(size_t i=0;i<pose.size();++i) pose[i]=object*fixture.skeleton.tr[i];
      require(body->moveKinematic(pose.data(),pose.size(),object,.017f),"script jumps must queue ordinary solved poses");
      }
    float maxItemError=0;
    for(int frame=0;frame<12;++frame) {
      world.tick(17);control.tick(17);
      maxItemError=std::max(maxItemError,(item->position()-reference->position()).length());
      require(world.bodyCount()==parts,"teleporting must preserve native part count");
      }
    std::vector<Tempest::Matrix4x4> actual;
    require(body->getPose(actual),"teleported native pose must remain available");
    for(auto bone:body->poseIndices())
      for(size_t component=0;component<16;++component)
        near(actual[bone].data()[component],pose[bone].data()[component],component>=12 && component<=14 ? .02f : .00002f,
             "a teleport must reach the full requested pose and retain zero residual motion");
    near(maxItemError,0,.01f,"an 80 cm kinematic jump must not launch a touching item along its old swept path");
    std::printf("kinematic teleport distance_cm=80 queued_steps=2 item_delta_cm=%.9g parts=%zu\n",double(maxItemError),body->partCount());
    }
  }

void testLocalMotorReactions() {
  Fixture fixture;
  for(const size_t hitBone:{size_t(6),size_t(13),size_t(3)}) {
    const bool chest=hitBone==3;
    CollisionWorld world;
    auto object=at(0,300,0);
    auto target=fixture.skeleton.tr;
    for(auto& frame:target) frame=object*frame;
    auto body=world.createKinematicBody(fixture.skeleton,fixture.zones,target.data(),target.size(),object);
    require(bool(body),"local motor checks need a valid living rig");
    const auto count=world.bodyCount(),parts=body->partCount();
    const auto hitPoint=position(target[hitBone])+Tempest::Vec3(12,0,0);
    require(!body->queueHit(fixture.skeleton.nodes.size(),hitPoint,{0,0,3},1),"unmapped hit bones must be rejected");
    require(body->queueHit(hitBone,hitPoint,{0,0,3},1),"a mapped bone must accept one point impulse in newton seconds");
    require(body->isKinematic(),"queued reactions must not mutate the native world before its fixed step");
    world.tick(17);
    require(!body->isKinematic() && !body->reactionBones().empty(),"a hit must activate a real dynamic limb chain");
    near(body->reactionWeight(),1,.0001f,"hit strength must control the initial physical pose weight");
    require(body->partCount()==parts && world.bodyCount()==count,"motor promotion must replace native parts without changing their count");
    const auto contains=[&](size_t bone) {
      const auto& bones=body->reactionBones(); return std::find(bones.begin(),bones.end(),bone)!=bones.end();
      };
    require(contains(hitBone) && !contains(1) && !contains(16) && (chest || !contains(9)),"physical reaction must exclude pelvis and unaffected limbs");
    require(chest ? !contains(2) && !contains(12) : hitBone==6 ? contains(7) && contains(8) && !contains(13) : contains(12) && contains(14) && !contains(6),
            "limb hits must select their anatomical chain while a chest hit leaves the lower spine and legs fixed");
    if(chest) require(body->reactionBones()==std::vector<size_t>{3},"a torso hit must activate only its spine branch, leaving head and arms kinematic");
    const auto probeBone=hitBone==6 ? size_t(8) : size_t(14);
    float peak=0,recovered=0;
    float chestAnglePeak=0,chestAngleRecovered=0;
    std::vector<Tempest::Matrix4x4> physical;
    for(int frame=0;frame<45;++frame) {
      // Keep the gameplay root moving during the physical limb response.
      object.set(3,0,float(frame)*.5f);
      for(size_t i=0;i<target.size();++i) target[i]=object*fixture.skeleton.tr[i];
      require(body->moveKinematic(target.data(),target.size(),object,.017f),"active rigs must continue consuming the ordinary ready animation target");
      world.tick(17);
      require(body->getPose(physical),"active native transforms must remain readable for the render overlay");
      for(const size_t fixed:{size_t(1),chest ? size_t(12) : size_t(9),size_t(16)})
        for(size_t component=0;component<16;++component)
          near(physical[fixed].data()[component],target[fixed].data()[component],component>=12 && component<=14 ? .02f : .00002f,
               "root and unaffected limbs must keep their animation transforms during a local reaction");
      const float displacement=std::abs(position(physical[probeBone]).z-position(target[probeBone]).z);
      peak=std::max(peak,displacement);
      if(frame==40) recovered=displacement;
      if(chest) {
        float trace=0;
        for(size_t axis=0;axis<3;++axis) {
          const auto* a=physical[hitBone].data()+axis*4;
          const auto* b=target[hitBone].data()+axis*4;
          trace+=Tempest::Vec3::dotProduct(Tempest::Vec3::normalize({a[0],a[1],a[2]}),Tempest::Vec3::normalize({b[0],b[1],b[2]}));
          }
        const float angle=std::acos(std::clamp((trace-1.f)*.5f,-1.f,1.f))*180.f/3.14159265358979323846f;
        chestAnglePeak=std::max(chestAnglePeak,angle);
        if(frame==40) chestAngleRecovered=angle;
        for(const size_t bone:body->boneIndices())
          if(bone!=hitBone)
            for(size_t component=0;component<16;++component)
              near(physical[bone].data()[component],target[bone].data()[component],component>=12 && component<=14 ? .02f : .00002f,
                   "head, both arms, pelvis and legs must retain their exact world animation pose during a chest reaction");
        }
      require(world.bodyCount()==count,"motor simulation must not append replacement bodies every frame");
      }
    // A spine's bone origin is its constrained pivot: rotation is the physical
    // response, while head/arms remain at their independent animation targets.
    const float responsePeak=chest ? chestAnglePeak : peak;
    const float responseRecovered=chest ? chestAngleRecovered : recovered;
    std::printf("local motor measured bone=%zu peak=%.9g recovered=%.9g unit=%s\n",hitBone,double(responsePeak),double(responseRecovered),chest?"degrees":"cm");
    require(responsePeak>(chest ? .1f : .3f),"the addressed limb/chest must show a measurable local native response after its point impulse");
    require(body->reactionWeight()>0.f,"motor recovery must be observed before the final kinematic deadline");
    require(responseRecovered<responsePeak*.65f+.05f,"real motors must return the limb/chest toward animation before the final kinematic reset");
    const auto point=position(physical[hitBone]);
    require(world.rayCast(point+Tempest::Vec3(0,0,300),point-Tempest::Vec3(0,0,300),1u<<DynamicWorld::C_ActiveBody).hasHit,
            "the activated physical chain must be present in its explicit contact layer");
    require(!world.rayCast(point+Tempest::Vec3(0,0,300),point-Tempest::Vec3(0,0,300),
                          (1u<<DynamicWorld::C_Landscape)|(1u<<DynamicWorld::C_Object)|(1u<<DynamicWorld::C_Ragdoll)).hasHit,
            "active living parts must remain excluded from the established primary hit layers");
    require(body->queueHit(hitBone,point,{0,0,-2},.35f) && body->queueHit(hitBone,point,{0,0,1},.35f),
            "an existing motor rig must accept repeated hits without allocating another rig");
    for(int frame=0;frame<100;++frame) {
      world.tick(17);
      require(world.bodyCount()==count && body->partCount()==parts,"repeated hits and recovery must preserve the native part count");
      }
    require(body->isKinematic() && body->reactionWeight()==0.f && body->reactionBones().empty(),"every finite reaction must return to pure kinematic mode within its deadline");
    require(body->getPose(physical),"the recovered kinematic pose must remain available");
    for(auto bone:body->poseIndices())
      for(size_t component=0;component<16;++component)
        near(physical[bone].data()[component],target[bone].data()[component],component>=12 && component<=14 ? .02f : .00002f,
             "the recovered rig must exactly follow its latest ordinary animation target");
    require(body->state().parts.empty(),"a partial active rig must never become corpse save data");
    std::printf("local motor bone=%zu peak=%.9g recovered_before_reset=%.9g unit=%s parts=%zu\n",hitBone,double(responsePeak),double(responseRecovered),chest?"degrees":"cm",parts);
    body.reset(); require(world.bodyCount()==0,"destroying a motor-capable rig must also remove its constraints and every native part");
    }
  }

void testLimpBoneImpulseOnce() {
  Fixture fixture;
  CollisionWorld hitWorld,controlWorld;
  auto hit=fixture.create(hitWorld,at(0,700,0));
  auto control=fixture.create(controlWorld,at(0,700,0));
  const auto initial=hit->state();
  const auto found=std::find_if(initial.parts.begin(),initial.parts.end(),[](const auto& part) { return part.bone==7; });
  require(found!=initial.parts.end(),"limp impulse fixture needs a forearm part");
  const auto point=position(found->transform)+Tempest::Vec3(12,0,0);
  require(hit->queueHit(7,point,{0,0,3},1),"a limp body must accept the same deferred point impulse API");
  same(hit->state(),initial,.001f);
  hitWorld.tick(17); controlWorld.tick(17);
  const auto hitState=hit->state(),controlState=control->state();
  const auto part=size_t(found-initial.parts.begin());
  require((hitState.parts[part].angularVelocity-controlState.parts[part].angularVelocity).length()>.05f,
          "a limp bone impulse away from its COM must generate local angular motion instead of a global velocity kick");
  require(hit->reactionWeight()==0.f && hit->reactionBones().empty() && !hit->isKinematic(),"limp hits must never enable active animation motors");
  require(hit->restore(initial) && control->restore(initial),"the one-shot impulse check must reset only observable physical state");
  hitWorld.tick(17); controlWorld.tick(17);
  same(hit->state(),control->state(),.01f);
  require(hitWorld.bodyCount()==hit->partCount(),"a limp point impulse must not allocate living or replacement parts");
  }

void testScaledMassAndInertia() {
  Fixture fixture;
  struct Response { Tempest::Vec3 linear,angular; };
  const auto response=[&](float scale,bool restore) {
    CollisionWorld hitWorld,controlWorld;
    auto object=at(0,1000,0); object.scale(scale);
    auto hit=fixture.create(hitWorld,object),control=fixture.create(controlWorld,object);
    const auto initial=hit->state();
    if(restore) {
      // Recreate with fresh BodyIDs, then restore the saved scaled body before
      // applying the same external impulse. Metadata alone cannot satisfy this.
      hit.reset(); control.reset();
      hit=fixture.create(hitWorld,object); control=fixture.create(controlWorld,object);
      require(hit->restore(initial) && control->restore(initial),"scaled corpse state must restore into a fresh native rig");
    }
    else {
      // A fresh corpse starts with fading muscles that resist the impulse;
      // restoring its own state leaves the bare body the restored case has.
      require(hit->restore(initial) && control->restore(initial),"a fresh corpse must accept its own state");
    }
    const auto part=std::find_if(initial.parts.begin(),initial.parts.end(),[](const auto& p) { return p.bone==7; });
    require(part!=initial.parts.end(),"mass/inertia oracle needs a physical forearm");
    const auto point=part->centerOfMassMeters*100.f+Tempest::Vec3(12*scale,0,0);
    require(hit->queueHit(7,point,{0,0,.15f},1),"scaled native forearm must accept a point impulse in Ns");
    hitWorld.tick(17); controlWorld.tick(17);
    const auto got=hit->state(),baseline=control->state();
    const size_t index=size_t(part-initial.parts.begin());
    return Response{got.parts[index].linearVelocity-baseline.parts[index].linearVelocity,
                    got.parts[index].angularVelocity-baseline.parts[index].angularVelocity};
    };
  const auto unit=response(1,false),large=response(2,false),restored=response(2,true);
  require(unit.linear.length()>.01f && unit.angular.length()>.01f,"native mass test must observe real linear and angular response");
  near(unit.linear.length()/large.linear.length(),8,1.6f,"double-size corpse must have eight times the native mass for the same Ns impulse");
  near(unit.angular.length()/large.angular.length(),16,3.2f,"mass and scaled inertia must reduce angular response by sixteen at a doubled lever arm");
  near((large.linear-restored.linear).length(),0,.001f,"restoring a scaled body must preserve native inverse mass");
  near((large.angular-restored.angular).length(),0,.0001f,"restoring a scaled body must preserve native inertia");
  CollisionWorld world;
  auto anisotropic=at(0,700,0); anisotropic.rotateOY(37); anisotropic.scale(.5f,2.f,3.f);
  auto body=fixture.create(world,anisotropic);
  near(body->mass(),225,.001f,"nonuniform object scale must use the volume determinant rather than a single axis");
  const auto saved=body->state();
  require(body->restore(saved),"nonuniform mass must survive native restore");
  near(body->mass(),225,.001f,"restored nonuniform mass must remain the actual native total");
  }

void testItemMassImpulseAndWake() {
  Resources resources;
  const auto tetrahedron=[](float scale) {
    asset::StaticMesh mesh;
    mesh.positions={{-10*scale,-20*scale,-30*scale},{10*scale,-20*scale,-30*scale},
                    {-10*scale,20*scale,-30*scale},{-10*scale,-20*scale,30*scale}};
    mesh.bbox={{-10*scale,-20*scale,-30*scale},{10*scale,20*scale,30*scale}};
    mesh.sub_meshes.resize(1);
    auto& part=mesh.sub_meshes[0];
    part.mat.group=asset::MaterialGroup::WOOD;
    for(size_t i=0;i<mesh.positions.size();++i)
      part.wedges.push_back({{0,1,0},{0,0},uint16_t(i)});
    part.triangles={{{0,2,1}},{{0,1,3}},{{0,3,2}},{{1,2,3}}};
    return mesh;
    };
  ProtoMesh proto(PackedMesh(tetrahedron(1),PackedMesh::PK_Visual),"CONTRACT_ITEM.MRM");
  DynamicWorld::Item empty;
  require(empty.mass()==0 && empty.isSleeping() && empty.velocity().quadLength()==0 && !empty.addImpulse({1,0,0}),
          "an empty item handle must safely reject impulse and report no dynamic state");
  asset::WorldMesh ground;
  ground.vertices={{-1000,0,-1000},{1000,0,-1000},{1000,0,1000},{-1000,0,1000}};
  ground.materials.resize(1); ground.materials[0].group=asset::MaterialGroup::STONE;
  ground.polygons.material_indices={0,0}; ground.polygons.vertex_indices={0,1,2,0,2,3};
  DynamicWorld world(nullptr,ground);
  std::unique_ptr<PhysicMeshShape> shape(PhysicMeshShape::load(PackedMesh(ground,PackedMesh::PK_Physic)));
  require(shape!=nullptr,"static item safety oracle needs a native terrain shape");
  auto fixed=world.addStaticObj(shape.get(),at(0,-100,0));
  auto moving=world.addMovableObj(shape.get(),at(0,-200,0));
  for(auto* body:{&fixed,&moving})
    require(body->mass()==0 && body->isSleeping() && body->velocity().quadLength()==0 && !body->addImpulse({1,0,0}),
            "static and animation-driven handles must never accept dynamic impulses");
  Bounds bounds; bounds.bbox[0]={-10,-20,-30}; bounds.bbox[1]={10,20,30};
  auto matrix=at(0,300,0); matrix.rotateOY(31); matrix.scale(.5f,2.f,3.f);
  auto body=world.addDynamicObj(matrix,bounds,asset::MaterialGroup::WOOD,&proto,"contract_item.mrm");
  // This tetrahedron occupies exactly one sixth of its bounds. Keep the old
  // tolerance while replacing the bounds-volume oracle with the actual hull.
  const float expected=.2f*.4f*.6f/6.f*700.f*3.f;
  near(body.mass(),expected,.001f,"native item mass must use hull volume, material density and the nonuniform scale determinant");
  const auto& hull=world.itemHullShape(proto,"CONTRACT_ITEM.MRM");
  require(hull.shape!=nullptr && hull.fallbackReason.empty(),"four noncoplanar mesh points must build an item hull");
  near(hull.volume,.2f*.4f*.6f/6.f,.000001f,"item hull volume must be the tetrahedron volume rather than its bounds");
  near(hull.originalShape->GetVolume(),hull.volume,.000001f,"item mass and water must retain the uninflated hull volume");
  const auto originalBounds=hull.originalShape->GetLocalBounds();
  const auto collisionBounds=hull.shape->GetLocalBounds();
  const auto originalCenter=hull.originalShape->GetCenterOfMass(),collisionCenter=hull.shape->GetCenterOfMass();
  for(int axis=0;axis<3;++axis) {
    near(collisionBounds.mMin[axis]+collisionCenter[axis],originalBounds.mMin[axis]+originalCenter[axis]-.01f,.0001f,
         "the Item collision shell must extend one centimetre below each source axis bound");
    near(collisionBounds.mMax[axis]+collisionCenter[axis],originalBounds.mMax[axis]+originalCenter[axis]+.01f,.0001f,
         "the Item collision shell must extend one centimetre above each source axis bound");
    }
  require(&hull==&world.itemHullShape(proto,"CoNtRaCt_ItEm.MrM"),"item hull cache keys must be case insensitive");
  auto legacy=world.addDynamicObj(at(700,300,0),bounds,asset::MaterialGroup::WOOD,&proto,"CONTRACT_ITEM.MRM",false);
  near(legacy.mass(),.2f*.4f*.6f*700.f,.001f,"item_hulls=0 must preserve the previous bounds-volume material mass");
  require(!body.addImpulse({std::numeric_limits<float>::quiet_NaN(),0,0}) &&
          !body.addImpulse({0,std::numeric_limits<float>::infinity(),0}) && !body.addImpulse({0,0,0}),
          "nonfinite or zero item impulses must be harmless no-ops");
  require(body.addImpulse({expected,0,0}),"a dynamic item must accept world-space Ns");
  near(body.velocity().x,100,.001f,"native item velocity must follow J/m in metres/second at the API boundary");
  require(body.addImpulse({-expected,0,0}),"opposite impulse must cancel horizontal item momentum");
  for(int frame=0;frame<600 && !body.isSleeping();++frame) world.tick(17);
  require(body.isSleeping(),"a dropped item must settle and retain a queryable sleeping native body");
  require(body.addImpulse({expected,0,0}) && !body.isSleeping(),"a spell-sized impulse must awaken the sleeping native item immediately");
  near(body.velocity().x,100,.001f,"the woken item's mass must still govern its actual native velocity");
  Bounds tiny; tiny.bbox[0]={-1,-2,-3}; tiny.bbox[1]={1,2,3};
  ProtoMesh tinyProto(PackedMesh(tetrahedron(.1f),PackedMesh::PK_Visual),"CONTRACT_TINY_ITEM.MRM");
  auto small=world.addDynamicObj(at(500,102,0),tiny,asset::MaterialGroup::WOOD,&tinyProto,"CONTRACT_TINY_ITEM.MRM");
  auto oldSmall=world.addDynamicObj(at(-500,102,0),tiny,asset::MaterialGroup::WOOD,&tinyProto,"CONTRACT_TINY_ITEM.MRM",false);
  near(small.mass(),.05f,.000001f,"small hull items must keep the fifty gram minimum mass");
  int sleepFrames=-1,oldSleepFrames=-1;
  for(int frame=0;frame<600 && (sleepFrames<0 || oldSleepFrames<0);++frame) {
    world.tick(frame%3==2 ? 16 : 17);
    if(sleepFrames<0 && small.isSleeping())sleepFrames=frame+1;
    if(oldSleepFrames<0 && oldSmall.isSleeping())oldSleepFrames=frame+1;
    }
  std::printf("Small item hull: mass %.9g kg, sleep ON %.3f s, OFF %.3f s\n",
              double(small.mass()),double(sleepFrames)/60.,double(oldSleepFrames)/60.);
  require(sleepFrames>0 && oldSleepFrames>0 && sleepFrames<=oldSleepFrames,
          "a small mesh hull must sleep no later than the legacy body in the same drop");
  auto lowest=std::numeric_limits<float>::max();
  const auto settled=small.matrix();
  const auto& smallHull=world.itemHullShape(tinyProto,"CONTRACT_TINY_ITEM.MRM");
  for(auto point:smallHull.vertices) {
    settled.project(point);
    lowest=std::min(lowest,point.y);
    }
  std::printf("Small item hull: support gap %.9g cm, extra padding %.9g cm, inner radius %.9g cm\n",
              double(lowest),double(smallHull.extraPaddingCm),double(smallHull.shape->GetInnerRadius()*100.f));
  near(smallHull.shape->GetInnerRadius(),.03f,.0001f,
       "a thin Item collision shell must have three centimetres from COM to its nearest support plane");
  require(lowest>=-1.05f && lowest<=1.05f+smallHull.extraPaddingCm,
          "a sleeping item's visible hull must stay within the authorised one centimetre range plus its thin-shell padding");
  for(int frame=0;frame<60;++frame) world.tick(frame%3==2 ? 16 : 17);
  require(small.isSleeping(),"a small sleeping hull must not wake or jitter on level ground");
  for(int c=0;c<4;++c)
    for(int r=0;r<4;++r)
      exact(small.matrix().at(c,r),settled.at(c,r),"a sleeping item hull must retain its exact transform");
  near(oldSmall.mass(),.02f*.04f*.06f*700.f,.000001f,"item_hulls=0 must retain even a sub-fifty-gram legacy mass");
  auto triangle=tetrahedron(1);
  triangle.positions.resize(3);
  triangle.sub_meshes[0].wedges.resize(3);
  triangle.sub_meshes[0].triangles={{{0,2,1}}};
  ProtoMesh incomplete(PackedMesh(triangle,PackedMesh::PK_Visual),"CONTRACT_TRIANGLE.MRM");
  const auto& fallback=world.itemHullShape(incomplete,"CONTRACT_TRIANGLE.MRM");
  require(fallback.shape==nullptr && !fallback.fallbackReason.empty(),"fewer than four referenced mesh vertices must report a box fallback");
  auto boxed=world.addDynamicObj(at(-700,300,0),bounds,asset::MaterialGroup::WOOD,&incomplete,"CONTRACT_TRIANGLE.MRM");
  near(boxed.mass(),.2f*.4f*.6f*700.f,.001f,"a failed hull must preserve the legacy box and its material mass");

  auto cube=tetrahedron(1);
  cube.positions={{-20,-20,-20},{20,-20,-20},{20,20,-20},{-20,20,-20},
                  {-20,-20,20},{20,-20,20},{20,20,20},{-20,20,20}};
  cube.bbox={{-20,-20,-20},{20,20,20}};
  auto& part=cube.sub_meshes[0];part.wedges.clear();
  for(size_t i=0;i<cube.positions.size();++i)
    part.wedges.push_back({{0,1,0},{0,0},uint16_t(i)});
  part.triangles={{{0,2,1}},{{0,3,2}},{{4,5,6}},{{4,6,7}},{{0,1,5}},{{0,5,4}},
                  {{3,7,6}},{{3,6,2}},{{0,4,7}},{{0,7,3}},{{1,2,6}},{{1,6,5}}};
  ProtoMesh cubeProto(PackedMesh(cube,PackedMesh::PK_Visual),"CONTRACT_SLOP_ITEM.MRM");
  Bounds cubeBounds;cubeBounds.assign(cubeProto.bboxMesh);
  DynamicWorld contactWorld(nullptr,ground);
  auto precise=contactWorld.addDynamicObj(at(-100,14,0),cubeBounds,asset::MaterialGroup::WOOD,&cubeProto,"CONTRACT_SLOP_ITEM.MRM");
  auto ordinary=contactWorld.addDynamicObj(at(100,14,0),cubeBounds,asset::MaterialGroup::WOOD);
  for(int frame=0;frame<360;++frame)contactWorld.tick(frame%3==2 ? 16 : 17);
  const float preciseGap=precise.position().y-20.f,ordinaryGap=ordinary.position().y-20.f;
  std::printf("Item contact slop: hull gap %.9g cm, ordinary gap %.9g cm\n",double(preciseGap),double(ordinaryGap));
  require(precise.isSleeping() && ordinary.isSleeping(),"penetration slop fixtures must reach genuine native sleep");
  // One centimetre of padding centres the unchanged two centimetre slop range.
  near(preciseGap,-1.f,.05f,"a deeply penetrating Item shell must retain one centimetre of visible penetration");
  near(ordinaryGap,-2.f,.05f,"an ordinary body must retain main's two centimetre penetration allowance");
  }

void testCorpseRayMetadata() {
  Fixture fixture;
  CollisionWorld world,reference;
  auto nearBody=fixture.create(world,at(0,300,0));
  auto farBody=fixture.create(world,at(0,300,-100));
  const auto a=position(fixture.skeleton.tr[7]),b=position(fixture.skeleton.tr[8]);
  std::vector<HitZones::Posed> capsules;
  fixture.zones.posed(fixture.skeleton.tr.data(),fixture.skeleton.tr.size(),1.f,capsules);
  const auto cap=std::find_if(capsules.begin(),capsules.end(),[&](const auto& c) { return (c.a-a).quadLength()<.0001f && (c.b-b).quadLength()<.0001f; });
  require(cap!=capsules.end(),"corpse query oracle needs the public posed forearm capsule");
  const auto center=(a+b)*.5f+Tempest::Vec3(0,300,0);
  auto frame=at(center.x,center.y,center.z); frame.rotateOZ(-90);
  auto plain=reference.addCollisionBody(new JPH::CapsuleShape((b-a).length()*.005f,cap->r*.01f),frame,.5f,DynamicWorld::C_Ragdoll);
  const Tempest::Vec3 from{center.x,center.y,300},to{center.x,center.y,-300};
  const uint32_t mask=1u<<DynamicWorld::C_Ragdoll;
  const auto expected=reference.rayCast(from,to,mask);
  require(expected.hasHit && expected.ragdoll==nullptr,"plain native primitive must give an independent geometry oracle without owner metadata");
  const auto compare=[&] {
    const auto hit=world.rayCast(from,to,mask);
    require(hit.hasHit && hit.ragdoll==nearBody.get() && hit.bone==7,"the selected corpse hit must identify its current owner and exact forearm bone");
    near(hit.fraction,expected.fraction,.000001f,"adding corpse ownership must preserve the selected native fraction");
    near((hit.point-expected.point).length(),0,.001f,"corpse metadata must preserve the selected world point");
    near((hit.normal-expected.normal).length(),0,.000001f,"corpse metadata must preserve the selected native normal");
    require(hit.material==expected.material,"corpse ownership must preserve the selected material");
    };
  compare();
  const auto saved=nearBody->state();
  require(nearBody->restore(saved),"corpse metadata oracle must survive native BodyID replacement on restore");
  compare();
  std::vector<CollisionWorld::QueryResult> all;
  world.rayCast(from,to,mask,false,&all);
  require(std::any_of(all.begin(),all.end(),[&](const auto& hit) { return hit.ragdoll==farBody.get() && hit.bone==7; }),
          "all-hit metadata must identify the far corpse independently of the closest one");
  nearBody.reset();
  require(world.rayCast(from,to,mask).ragdoll==farBody.get(),"destroyed corpse parts must never leave stale owner metadata");
  require(!world.rayCast(from,to,(1u<<DynamicWorld::C_Landscape)|(1u<<DynamicWorld::C_Object)).hasHit,
          "ordinary ray masks must remain unchanged by corpse metadata");
  }

void testProjectileWorldStopOptIn() {
  struct Callback final : DynamicWorld::BulletCallback {
    bool enabled=false,precise=false;
    DynamicWorld::BulletBody* body=nullptr;
    DynamicWorld::BulletWorldHit hit;
    std::string order;
    int worlds=0,materials=0,stops=0,moves=0;
    bool stopsAtWorldHit() const override { return enabled; }
    bool isPrecise() const override { return precise; }
    void onMove() override { ++moves; order+='M'; }
    void onCollide(asset::MaterialGroup) override { ++materials; order+='C'; }
    void onWorldHit(const DynamicWorld::BulletWorldHit& value) override {
      ++worlds; order+='W'; hit=value;
      near((body->position()-value.point).length(),0,0,"attachment must observe the unchanged stopped projectile position");
      require(body->pathLength()>0,"attachment must observe updated travelled distance");
      DynamicWorld::BulletCallback::onWorldHit(value);
      }
    void onStop() override { ++stops; order+='S'; }
    };
  const auto wallAt=[](float x, asset::MaterialGroup material, bool fromRight=false) {
    asset::WorldMesh wall;
    wall.vertices={{x,-2000,-2000},{x,2000,-2000},{x,0,2000}};
    wall.materials.resize(1); wall.materials[0].group=material;
    wall.polygons.material_indices={0};
    wall.polygons.vertex_indices=fromRight ? std::vector<uint32_t>{0,2,1} : std::vector<uint32_t>{0,1,2};
    return wall;
    };
  const Tempest::Vec3 from{0,500,0},direction{3,0,0};
  const auto to=from+direction*100.f-Tempest::Vec3(0,.5f*DynamicWorld::gravity*100.f*100.f,0);
  for(const auto material:{asset::MaterialGroup::STONE,asset::MaterialGroup::METAL})
    for(const bool enabled:{false,true})
      for(const int previousHits:{0,3}) {
        DynamicWorld world(nullptr,wallAt(100,material));
        Callback callback; callback.enabled=enabled;
        auto* arrow=world.bulletObj(&callback); callback.body=arrow;
        const auto expected=world.ray(from,to);
        require(expected.hasCol && expected.mat==material,"reflection contract must hit the selected hard wall");
        arrow->setPosition(from); arrow->setDirection(direction);
        for(int i=0;i<previousHits;++i) arrow->addHit();
        world.tick(100);
        near((arrow->position()-(from+(to-from)*expected.hitFraction)).length(),0,0,"ON/OFF must preserve the original hit interpolation");
        near((arrow->direction()-Tempest::Vec3(-1.5f,0,0)).length(),0,.000001f,"hard material must reflect at half speed even when attachment is ON");
        near(arrow->speed(),1.5f,.000001f,"reflection must preserve the original speed loss");
        near(arrow->pathLength(),(to-from).length()*expected.hitFraction,.0001f,"reflection must preserve travelled distance");
        require(arrow->hitCount()==previousHits+1 && callback.materials==1 && callback.moves==1,"hard impact must keep one hit/material/move callback");
        if(previousHits==0) {
          require(callback.worlds==0 && callback.stops==0 && callback.order=="CM","the first reflection must not stop or pin an arrow");
          }
        else {
          require(callback.stops==1 && callback.worlds==int(enabled) && callback.order==(enabled ? "CWSM" : "CSM"),
                  "fourth impact must preserve reflection before the original terminal decision and callbacks");
          if(enabled) {
            near((callback.hit.normal-expected.n).length(),0,0,"terminal attachment must preserve the selected normal");
            require(callback.hit.ragdoll==nullptr,"a hard world stop must not become a corpse hit");
            for(int frame=0;frame<4;++frame) world.tick(17);
            require(callback.worlds==1 && callback.stops==1 && callback.materials==1 && callback.moves==1,
                    "a terminal pin must not repeat callbacks while deletion is deferred");
            }
          }
        world.deleteObj(arrow);
        }

  for(const bool enabled:{false,true}) {
    DynamicWorld world(nullptr,wallAt(100,asset::MaterialGroup::WOOD));
    Callback callback; callback.enabled=enabled;
    auto* arrow=world.bulletObj(&callback); callback.body=arrow;
    const auto expected=world.ray(from,to);
    require(expected.hasCol && expected.mat==asset::MaterialGroup::WOOD,"terminal contract must hit a soft wall");
    arrow->setPosition(from); arrow->setDirection(direction);
    world.tick(100);
    require(callback.worlds==int(enabled) && callback.materials==1 && callback.stops==1 && callback.moves==1 &&
            callback.order==(enabled ? "CWSM" : "CSM"),"soft contact must preserve gameplay callbacks and add only an attachment notification");
    near((arrow->position()-(from+(to-from)*expected.hitFraction)).length(),0,0,"soft terminal position must retain original interpolation");
    near((arrow->direction()-direction).length(),0,0,"soft stop must retain original direction");
    near(arrow->pathLength(),(to-from).length()*expected.hitFraction,.0001f,"soft stop must retain travelled distance");
    require(arrow->hitCount()==1,"soft stop must record exactly one world hit");
    if(enabled) {
      near((callback.hit.normal-expected.n).length(),0,0,"soft attachment must retain the world normal");
      for(int frame=0;frame<4;++frame) world.tick(17);
      require(callback.worlds==1 && callback.stops==1 && callback.moves==1,"a soft pin must not repeat terminal callbacks");
      }
    world.deleteObj(arrow);
    }

  // The old speed*fraction gate moves to the wall but does not count, reflect,
  // collide or stop for this near-contact sample. Attachment must not widen it.
  for(const bool enabled:{false,true}) {
    DynamicWorld world(nullptr,wallAt(100,asset::MaterialGroup::STONE));
    Callback callback; callback.enabled=enabled;
    auto* arrow=world.bulletObj(&callback); callback.body=arrow;
    arrow->setPosition({95,500,0}); arrow->setDirection(direction);
    world.tick(100);
    require(callback.worlds==0 && callback.materials==0 && callback.stops==0 && callback.moves==1 && callback.order=="M",
            "near hard contact must retain the original no-callback speed/fraction gate");
    require(arrow->hitCount()==0 && arrow->pathLength()==0,"near-contact gate must preserve hit and distance counters");
    near((arrow->direction()-direction).length(),0,0,"near-contact gate must preserve incoming direction");
    near(arrow->position().x,100,.0001f,"near-contact gate must still reach the original wall point");
    world.deleteObj(arrow);
    }

  Fixture fixture;
  for(const bool enabled:{false,true})
    for(const bool precise:{false,true})
      for(const bool wallBehind:{false,true}) {
        DynamicWorld world(nullptr,wallBehind ? wallAt(-160,asset::MaterialGroup::WOOD,true) : asset::WorldMesh{});
        auto pose=fixture.skeleton.tr;
        const auto object=at(0,300,0);
        for(auto& frame:pose) frame=object*frame;
        auto corpse=world.createRagdoll(fixture.skeleton,fixture.zones,pose.data(),pose.size(),object);
        require(corpse && corpse->valid() && corpse->partCount()>0,"transparency contract needs an actual native corpse");
        const Tempest::Vec3 start{200,439,0},velocity{-4,0,0};
        const auto end=start+velocity*100.f-Tempest::Vec3(0,.5f*DynamicWorld::gravity*100.f*100.f,0);
        float crossing=0;
        require(fixture.zones.rayTest(pose.data(),pose.size(),1.f,start,end,crossing)!=HitZones::None,
                "the arrow segment must cross the corpse's source capsule geometry");
        Callback callback; callback.enabled=enabled; callback.precise=precise;
        auto* arrow=world.bulletObj(&callback); callback.body=arrow;
        arrow->setPosition(start); arrow->setDirection(velocity);
        const auto expected=world.ray(start,end);
        require(expected.hasCol==wallBehind,"only the unchanged wall mask may obstruct this flight");
        world.tick(100);
        if(wallBehind) {
          require(callback.materials==1 && callback.stops==1 && callback.worlds==int(enabled) && callback.order==(enabled ? "CWSM" : "CSM"),
                  "an arrow crossing a corpse must still reach the same terminal wall");
          near((arrow->position()-(start+(end-start)*expected.hitFraction)).length(),0,0,"a corpse must not shorten the wall-bound segment");
          require(arrow->hitCount()==1 && callback.hit.ragdoll==nullptr,"the accepted hit must belong to the wall, never the corpse");
          }
        else {
          require(callback.worlds==0 && callback.materials==0 && callback.stops==0 && callback.moves==1 && callback.order=="M",
                  "ON/OFF and precise/legacy arrows must pass through corpses without impact callbacks");
          near((arrow->position()-end).length(),0,0,"corpse transparency must preserve the complete flight segment");
          near(arrow->pathLength(),400,.0001f,"corpse transparency must preserve travelled distance");
          require(arrow->hitCount()==0,"passing a corpse must not count as an impact");
          }
        world.deleteObj(arrow);
        }

  for(const bool enabled:{false,true}) {
    DynamicWorld world(nullptr,wallAt(100,asset::MaterialGroup::STONE));
    Callback callback; callback.enabled=enabled;
    auto* spell=world.bulletObj(&callback); callback.body=spell;
    spell->setSpellId(1); spell->setPosition(from); spell->setDirection(direction);
    world.tick(100);
    require(callback.worlds==0 && callback.materials==1 && callback.stops==1 && callback.moves==1 && callback.order=="CSM",
            "arrow attachment must not change spell impact callbacks");
    near((spell->position()-from).length(),0,0,"spell stop must preserve the original projectile position");
    require(spell->hitCount()==0 && spell->pathLength()==0,"spell stop must preserve the original counters");
    world.deleteObj(spell);
    }

  for(const bool enabled:{false,true}) {
    DynamicWorld world(nullptr,asset::WorldMesh{});
    Callback callback; callback.enabled=enabled;
    auto* arrow=world.bulletObj(&callback); callback.body=arrow;
    arrow->setPosition(from); arrow->setDirection(direction); arrow->addPathLen(10001);
    world.tick(100);
    require(callback.worlds==0 && callback.materials==0 && callback.stops==1 && callback.moves==1 && callback.order=="SM",
            "the original distance limit must stop without inventing a world attachment");
    near((arrow->position()-to).length(),0,0,"distance limit must preserve the original final movement");
    require(arrow->hitCount()==0,"distance limit must not invent a world hit");
    world.deleteObj(arrow);
    }
  }

void testSpellWorldImpactMetadata() {
  struct Callback final : DynamicWorld::BulletCallback {
    DynamicWorld::BulletWorldHit hit;
    int worlds=0,materials=0,stops=0,moves=0;
    std::string order;
    bool stopsAtWorldHit() const override { return true; }
    void onSpellWorldHit(const DynamicWorld::BulletWorldHit& value) override {
      ++worlds; order+='W'; hit=value;
      DynamicWorld::BulletCallback::onSpellWorldHit(value);
      }
    void onCollide(asset::MaterialGroup material) override {
      require(material==asset::MaterialGroup::STONE,"default spell world-hit callback must preserve the old selected material callback");
      ++materials; order+='C';
      }
    void onStop() override { ++stops; order+='S'; }
    void onMove() override { ++moves; order+='M'; }
    };
  asset::WorldMesh wall;
  wall.vertices={{100,-2000,-2000},{100,2000,-2000},{100,0,2000}};
  wall.materials.resize(1); wall.materials[0].group=asset::MaterialGroup::STONE;
  wall.polygons.material_indices={0}; wall.polygons.vertex_indices={0,1,2};
  DynamicWorld world(nullptr,wall);
  Callback callback;
  auto* spell=world.bulletObj(&callback);
  const Tempest::Vec3 from{0,500,0},direction{3,0,0};
  const auto expected=world.ray(from,from+direction*100.f);
  require(expected.hasCol,"spell impact oracle needs the selected wall hit");
  spell->setSpellId(1); spell->setPosition(from); spell->setDirection(direction);
  world.tick(100);
  require(callback.worlds==1 && callback.materials==1 && callback.stops==1 && callback.moves==1 && callback.order=="WCSM",
          "spell world-hit metadata must preserve one default material, stop and move callback in legacy order");
  near((callback.hit.point-expected.v).length(),0,0,"spell explosion metadata must use the exact selected impact point");
  near((callback.hit.normal-expected.n).length(),0,0,"spell metadata must retain the selected normal");
  require(callback.hit.material==expected.mat && callback.hit.ragdoll==nullptr,"spell metadata must preserve its old world query selection");
  near((spell->position()-from).length(),0,0,"metadata must not change legacy spell position at world impact");
  near(spell->pathLength(),0,0,"metadata must not change legacy spell travelled distance");
  require(spell->hitCount()==0 && spell->direction().x==3,"spell metadata must not enter arrow stop/pin/bounce accounting");
  world.deleteObj(spell);
  }

void testDropSleepWake() {
  Fixture fixture;
  CollisionWorld world;
  auto ground=floor(world);
  const auto emptyCount=world.bodyCount();
  auto body=fixture.create(world,at(0,400,0));
  require(world.bodyCount()==emptyCount+body->partCount(),"each physical part must belong to the shared world");
  const auto start=position(body->pelvisTransform());
  for(int i=0;i<60;++i) { world.tick(17); finite(body->state()); joints(fixture,*body,i); }
  require(position(body->pelvisTransform()).y<start.y-200.f,"a body without dropped items must fall under gravity");
  for(int i=0;i<1800 && !body->isSleeping();++i) { world.tick(17); finite(body->state()); }
  require(body->isSleeping() && body->settled(),"a corpse must settle and sleep on the floor within thirty seconds");
  require(world.bodyCount()==emptyCount+body->partCount(),"sleep must retain every corpse part in the world");
  const auto resting=body->state();
  const auto pelvis=position(body->pelvisTransform());
  require(pelvis.y>-5.f && pelvis.y<150.f,"the pelvis must rest above the floor instead of passing through it");
  const auto hit=world.rayCast(pelvis+Tempest::Vec3(0,300,0),pelvis-Tempest::Vec3(0,300,0),
                              1u<<DynamicWorld::C_Ragdoll);
  require(hit.hasHit,"a sleeping corpse must remain queryable in its explicit layer");
  for(int i=0;i<60;++i) world.tick(17);
  same(body->state(),resting,.001f);
  body->addImpulse({150,100,0});
  require(!body->isSleeping(),"a new impulse must wake a sleeping corpse");
  for(int i=0;i<60;++i) world.tick(17);
  require((position(body->pelvisTransform())-pelvis).length()>20.f,"woken corpse must move after a mass-distributed impulse");
  joints(fixture,*body);
  body.reset();
  require(world.bodyCount()==emptyCount,"destroying the owner must remove all ragdoll parts and joints");
  }

void testAnatomicalHipAndAnkle() {
  // The fixture's bind faces +Z with its left at +X. Hard kicks drive a hip
  // backwards and a foot sideways, beyond what any human joint allows.
  Fixture fixture;
  CollisionWorld world;
  auto body=fixture.create(world,at(0,4000,0));
  // Let the muscles of the moment of death fade first.
  for(int i=0;i<70;++i) world.tick(17);
  const auto frame=[&](size_t bone) {
    for(const auto& part:body->state().parts) if(part.bone==bone) return part.transform;
    require(false,"anatomy oracle needs the part");
    return Tempest::Matrix4x4();
    };
  const auto local=[](const Tempest::Matrix4x4& m, const Tempest::Vec3& v) {
    const auto* d=m.data();
    return Tempest::Vec3(d[0]*v.x+d[1]*v.y+d[2]*v.z,d[4]*v.x+d[5]*v.y+d[6]*v.z,d[8]*v.x+d[9]*v.y+d[10]*v.z);
    };
  const float deg=180.f/float(3.14159265358979323846);
  require(body->queueHit(13,position(frame(13)),{0,0,-30},1),"a corpse calf must accept a kick");
  require(body->queueHit(19,position(frame(19)),{40,0,0},1),"a corpse toe must accept a kick");
  float hipBack=0,footOut=0;
  for(int i=0;i<40;++i) {
    world.tick(17);
    const auto thigh=local(frame(1),position(frame(13))-position(frame(12)));
    hipBack=std::max(hipBack,std::atan2(-thigh.z,-thigh.y)*deg);
    const auto foot=local(frame(17),position(frame(19))-position(frame(18)));
    footOut=std::max(footOut,std::asin(std::clamp(-foot.x/foot.length(),-1.f,1.f))*deg);
    }
  std::printf("anatomical limits hip_extension_deg=%.3f foot_out_deg=%.3f\n",double(hipBack),double(footOut));
  require(hipBack<22.f,"a hip must not extend backwards beyond its anatomical range");
  require(footOut<22.f,"a foot must not turn out beyond its anatomical range");
  }

zenkit::ModelHierarchy insectHierarchy() {
  // A Bloodfly-like insect: its root is the thorax itself and carries six
  // legs, two wing pairs, pincers and a tail directly, with no pelvis bone.
  zenkit::ModelHierarchy source{};
  auto add=[&](const char* name, int16_t parent, Tempest::Vec3 offset) {
    zenkit::ModelHierarchyNode node;
    node.name=name;
    node.parent_index=parent;
    const auto transform=at(offset.x,offset.y,offset.z);
    std::memcpy(&node.transform,&transform,sizeof(transform));
    source.nodes.push_back(node);
    return int16_t(source.nodes.size()-1);
    };
  add("BIP01 CENTER",-1,{0,0,0});
  for(const char* side:{"L","R"}) {
    const float x=side[0]=='L' ? 1.f : -1.f;
    const std::pair<const char*,float> legs[]={{"FRONT",8.f},{"MIDDLE",0.f},{"BACK",-8.f}};
    for(auto [leg,z]:legs) {
      const std::string base=std::string("BIP01 ")+side+"-LEG "+leg;
      auto parent=add((base+"01").c_str(),0,{5*x,-3,z});
      parent=add((base+"02").c_str(),parent,{10*x,-2,0});
      parent=add((base+"03").c_str(),parent,{12*x,-8,0});
      parent=add((base+"04").c_str(),parent,{4*x,-24,0});
      add((base+"05").c_str(),parent,{0,0,0});
      }
    add((std::string("BIP01 ")+side+"-WING01").c_str(),0,{6*x,6,2});
    add((std::string("BIP01 ")+side+"-WING02").c_str(),0,{6*x,6,-4});
    add((std::string("BIP01 ")+side+"-PINCER").c_str(),0,{3*x,-2,12});
    }
  int16_t tail=0;
  for(const char* name:{"BIP01 TAIL01","BIP01 TAIL02","BIP01 TAIL03","BIP01 TAIL04"})
    tail=add(name,tail,{0,-4,tail==0 ? -15.f : -20.f});
  add("BIP01 HOLD STING",tail,{0,0,-10});
  source.bbox.min={-40,-40,-90}; source.bbox.max={40,10,20};
  source.collision_bbox=source.bbox;
  return source;
  }

void testInsectRootBody() {
  // Without a thorax part, every limb hung off the first leg segment and the
  // corpse exploded into screen-sized triangles once it landed.
  Skeleton skeleton{asset::fromZenKit(insectHierarchy()),nullptr,"Bloodfly.mds"};
  HitZones zones;
  zones.setSkeleton(&skeleton);
  require(zones.isValid(),"insect fixture must have hit capsules");
  CollisionWorld world;
  auto ground=floor(world);
  std::vector<Tempest::Matrix4x4> pose;
  const auto object=at(0,150,0);
  for(const auto& matrix:skeleton.tr) pose.push_back(object*matrix);
  auto body=world.createRagdoll(skeleton,zones,pose.data(),pose.size(),object,{});
  require(body!=nullptr && body->valid(),"insect skeleton must create a ragdoll");
  require(!body->boneIndices().empty() && body->boneIndices().front()==0,
          "an insect's root thorax must be the ragdoll's root part");
  std::vector<Tempest::Matrix4x4> bones;
  float worst=0;
  for(int i=0;i<1800 && !(i>120 && body->isSleeping());++i) {
    world.tick(17);
    finite(body->state());
    require(body->getPose(bones),"insect pose must stay exportable");
    const auto center=position(bones[0]);
    for(size_t bone:body->boneIndices()) worst=std::max(worst,(position(bones[bone])-center).length());
    }
  require(worst<110.f,"insect limbs must stay attached to the thorax after landing");
  require(body->isSleeping(),"an insect corpse must settle and sleep within thirty seconds");
  }

zenkit::ModelHierarchy wolfHierarchy() {
  auto source=hierarchy();
  // A quadruped's bind pose is not a human silhouette with a different mass.
  // Keep the anatomical bone names but put the spine/snout forward, with four
  // real descending leg chains and a long tail behind its pelvis.
  const Tempest::Vec3 offsets[]={
    {0,0,0},{0,58,-30},{0,0,28},{0,0,30},{0,4,22},{0,2,22},
    {15,-5,0},{0,-24,0},{0,-25,4},
    {-15,-5,0},{0,-24,0},{0,-25,4},
    {13,-5,0},{0,-23,-8},{0,-25,7},{0,0,12},
    {-13,-5,0},{0,-23,-8},{0,-25,7},{0,0,12},{0,0,14}
    };
  static_assert(sizeof(offsets)/sizeof(offsets[0])==21);
  for(size_t i=0;i<source.nodes.size();++i) {
    const auto frame=at(offsets[i].x,offsets[i].y,offsets[i].z);
    std::memcpy(&source.nodes[i].transform,&frame,sizeof(frame));
    }
  // Gothic wolves have a digitigrade hock between calf and foot. Preserve
  // those real leg chains so an unrecognized HORSELINK cannot pass through
  // the generic torso buoyancy fallback in an otherwise floating wolf.
  source.nodes[14].name="BIP01 L HORSELINK";
  source.nodes[15].name="BIP01 L FOOT";
  source.nodes[18].name="BIP01 R HORSELINK";
  source.nodes[19].name="BIP01 R FOOT";
  for(const auto parent:{int16_t(1),int16_t(21)}) {
    zenkit::ModelHierarchyNode tail;
    tail.name=parent==1 ? "BIP01 TAIL" : "BIP01 TAIL1";
    tail.parent_index=parent;
    const auto frame=at(0,0,-25);
    std::memcpy(&tail.transform,&frame,sizeof(frame));
    source.nodes.push_back(tail);
    }
  source.bbox.min={-30,0,-50}; source.bbox.max={30,90,100};
  source.collision_bbox=source.bbox;
  return source;
  }

struct WaterRagdollFixture final {
  Skeleton skeleton;
  HitZones zones;

  explicit WaterRagdollFixture(const char* name,bool quadruped=false)
    :skeleton(asset::fromZenKit(quadruped ? wolfHierarchy() : hierarchy()),nullptr,name) {
    zones.setSkeleton(&skeleton);
    require(zones.isValid(),"water ragdoll fixture must have valid anatomical capsules");
    }

  std::unique_ptr<Ragdoll> create(CollisionWorld& world,const Tempest::Matrix4x4& object,
                                const Tempest::Vec3& velocity={}) const {
    auto pose=skeleton.tr;
    for(auto& bone:pose) bone=object*bone;
    auto result=world.createRagdoll(skeleton,zones,pose.data(),pose.size(),object,velocity);
    require(result && result->valid(),"water fixture must create a native limp ragdoll");
    near(result->mass(),Ragdoll::speciesMass(skeleton.name())*scaleVolume(object),.001f,
         "water setup must retain the actual species and scaled native mass");
    return result;
    }
  };

void waterSurface(CollisionWorld& world) {
  world.setWaterSurfaceCallback([](const Tempest::Vec3&,float,float& surface) {
    surface=0;
    return true;
    });
  }

auto waterFloor(CollisionWorld& world,float surface=-500.f) {
  return world.addCollisionBody(new JPH::BoxShape(JPH::Vec3(20,.5f,20)),at(0,surface-50.f,0),
                                .75f,DynamicWorld::C_Landscape);
  }

void tickWater(CollisionWorld& world,int frames) {
  for(int i=0;i<frames;++i) world.tick(i%3==2 ? 16 : 17);
  }

float maxWaterDrift(const Ragdoll::State& current,const Ragdoll::State& initial) {
  require(current.parts.size()==initial.parts.size(),"water stability must retain all native parts");
  float result=0;
  for(size_t i=0;i<current.parts.size();++i) {
    require(current.parts[i].bone==initial.parts[i].bone,"water stability must retain bone identity");
    result=std::max(result,(position(current.parts[i].transform)-position(initial.parts[i].transform)).length());
    }
  return result;
  }

float waterTorsoImmersion(const CollisionWorld& world,const Ragdoll& body,const Skeleton& skeleton) {
  float weighted=0,mass=0;
  bool visible=false;
  size_t count=0;
  const auto snapshots=world.waterBodySnapshot();
  for(const auto& part:snapshots) {
    if(part.ragdoll!=&body) continue;
    ++count;
    require(part.body==nullptr && part.bone<skeleton.nodes.size(),
            "water parts must expose their ragdoll and physical bone instead of an item payload");
    const auto& name=skeleton.nodes[part.bone].name;
    const bool torso=name.find("PELVIS")!=std::string::npos || name.find("SPINE")!=std::string::npos;
    const bool head=name.find("HEAD")!=std::string::npos || name.find("NECK")!=std::string::npos;
    near(part.targetFraction,torso ? .9f : head ? .95f : 1.f,.00001f,
         "water displacement must come from anatomy independently of each part's native mass");
    require(part.mass>0.f && std::isfinite(part.submergedFraction),"native wet part measurements must remain finite");
    if(torso) { weighted+=part.submergedFraction*part.mass; mass+=part.mass; }
    visible=visible || part.worldBounds[1].y>.1f;
    }
  require(count==body.partCount(),"every limp dynamic part must appear once in the native water diagnostic");
  require(mass>0.f,"water fixture must measure a real pelvis/spine");
  require(visible,"a floating corpse must leave some of its body visible above the surface");
  const float immersion=weighted/mass;
  near(immersion,.9f,.12f,"constrained pelvis/spine must float mostly submerged near ninety percent");
  require(position(body.pelvisTransform()).y>-200.f,"a floating corpse must stay by the surface instead of reaching the deep bottom");
  return immersion;
  }

void testRagdollWaterWolfHockTargetIndependentOfMass() {
  WaterRagdollFixture wolf("WOLF.MDS",true),warg("WARG.MDS",true);
  CollisionWorld lightWorld,heavyWorld;
  waterSurface(lightWorld);waterSurface(heavyWorld);
  const auto object=at(0,-30,0);
  auto light=wolf.create(lightWorld,object),heavy=warg.create(heavyWorld,object);
  const auto lightParts=lightWorld.waterBodySnapshot(),heavyParts=heavyWorld.waterBodySnapshot();
  for(const char* side:{"L","R"}) {
    const auto prefix=std::string("BIP01 ")+side;
    const auto hock=wolf.skeleton.findNode(prefix+" HORSELINK");
    const auto calf=wolf.skeleton.findNode(prefix+" CALF"),foot=wolf.skeleton.findNode(prefix+" FOOT");
    require(hock<wolf.skeleton.nodes.size() && calf<wolf.skeleton.nodes.size() && foot<wolf.skeleton.nodes.size() &&
            wolf.skeleton.nodes[hock].parent==calf && wolf.skeleton.nodes[foot].parent==hock,
            "the wolf hock regression must use a real calf-hock-foot chain");
    require(std::find(light->boneIndices().begin(),light->boneIndices().end(),hock)!=light->boneIndices().end() &&
            std::find(heavy->boneIndices().begin(),heavy->boneIndices().end(),hock)!=heavy->boneIndices().end(),
            "both differently weighted wolf hocks must be genuine dynamic native parts");
    const auto locate=[&](const auto& parts,const Ragdoll* ragdoll) -> const CollisionWorld::WaterBodySnapshot& {
      const auto found=std::find_if(parts.begin(),parts.end(),[&](const auto& part) {return part.ragdoll==ragdoll && part.bone==hock;});
      require(found!=parts.end(),"every physical wolf hock must participate in buoyancy diagnostics");
      return *found;
      };
    const auto& lightHock=locate(lightParts,light.get());
    const auto& heavyHock=locate(heavyParts,heavy.get());
    require(lightHock.mass>0.f && heavyHock.mass>lightHock.mass,"the hock regression must compare distinct actual native masses");
    near(heavyHock.mass/lightHock.mass,1.5f,.00001f,"identical wolf/warg hocks must carry their independent species mass");
    near(lightHock.targetFraction,1.f,.00001f,"a wolf HORSELINK must hang as a fully immersed limb");
    near(heavyHock.targetFraction,1.f,.00001f,"a heavier HORSELINK must retain the same limb displacement target");
    std::printf("Water wolf hock side=%s bone=%zu masses_kg=[%.9g,%.9g] target_fractions=[%.9g,%.9g]\n",side,hock,
                double(lightHock.mass),double(heavyHock.mass),double(lightHock.targetFraction),double(heavyHock.targetFraction));
    }
  }

void testRagdollWaterFloatSleepRestoreAndToggle() {
  for(const bool wolf:{false,true}) {
    WaterRagdollFixture fixture(wolf ? "WOLF.MDS" : "HUMANS.MDS",wolf);
    CollisionWorld world;
    waterSurface(world);
    auto ground=waterFloor(world);
    auto object=at(0,180,0);
    object.rotateOX(90);
    auto body=fixture.create(world,object,{0,-100,0});
    const float nativeMass=body->mass();
    int firstSleep=-1;
    for(int frame=0;frame<1800;++frame) {
      world.tick(frame%3==2 ? 16 : 17);
      if(firstSleep<0 && body->isSleeping()) firstSleep=frame+1;
      }
    finite(body->state());
    if(firstSleep<=0 || !body->isSleeping()) {
      std::fprintf(stderr,"water sleep diagnostic skeleton=%.*s asleep=%d first_sleep_frame=%d pelvis_y_cm=%.9g\n",
                   int(fixture.skeleton.name().size()),fixture.skeleton.name().data(),int(body->isSleeping()),firstSleep,
                   double(position(body->pelvisTransform()).y));
      for(const auto& part:world.waterBodySnapshot())
        if(part.ragdoll==body.get())
          std::fprintf(stderr,"water sleep part bone=%u fraction=%.9g target=%.9g velocity_cm_s=[%.9g,%.9g,%.9g] active=%d\n",
                       part.bone,double(part.submergedFraction),double(part.targetFraction),double(part.velocity.x),double(part.velocity.y),
                       double(part.velocity.z),int(!part.sleeping));
      }
    require(firstSleep>0 && body->isSleeping(),"a human or wolf corpse must float and sleep within thirty seconds");
    near(body->mass(),nativeMass,0.f,"water forces must leave the corpse's native mass unchanged");
    const float immersion=waterTorsoImmersion(world,*body,fixture.skeleton);
    const auto saved=body->state();

    // Fresh native bodies/world exercise the cold restore path: no old water
    // flags, BodyIDs or solver island are available to keep the corpse afloat.
    CollisionWorld loadedWorld;
    waterSurface(loadedWorld);
    auto loadedGround=waterFloor(loadedWorld);
    auto loaded=fixture.create(loadedWorld,at(300,400,100));
    require(loaded->restore(saved) && loaded->isSleeping(),"a fresh world must restore a sleeping floating corpse");
    same(loaded->state(),saved,.001f);
    float maxDrift=0;
    for(int frame=0;frame<1800;++frame) {
      loadedWorld.tick(frame%3==2 ? 16 : 17);
      maxDrift=std::max(maxDrift,maxWaterDrift(loaded->state(),saved));
      require(loaded->isSleeping(),"a restored floating corpse must remain asleep over thirty seconds");
      }
    near(maxDrift,0,1.f,"cold restored floating parts must stay within one centimetre for thirty seconds");
    waterTorsoImmersion(loadedWorld,*loaded,fixture.skeleton);

    CollisionWorld disabledWorld;
    waterSurface(disabledWorld);
    disabledWorld.setBuoyancyEnabled(false);
    auto disabledGround=waterFloor(disabledWorld);
    auto disabled=fixture.create(disabledWorld,at(0,400,0));
    require(disabled->restore(saved),"buoyancy=0 must still accept a floating sleeping corpse save");
    near(maxWaterDrift(disabled->state(),saved),0,.001f,"loading buoyancy=0 must retain every saved part position before it starts sinking");
    require(!disabled->isSleeping(),"loading buoyancy=0 must wake a saved floating sleeper immediately");
    tickWater(disabledWorld,1800);
    require(disabled->isSleeping() && position(disabled->pelvisTransform()).y<-350.f,
            "a floating sleeper loaded with buoyancy=0 must wake, sink and settle at the bottom");

    loadedWorld.setBuoyancyEnabled(false);
    tickWater(loadedWorld,1800);
    require(loaded->isSleeping() && position(loaded->pelvisTransform()).y<-350.f,
            "switching buoyancy off must wake a floating corpse and restore its former sinking behavior");
    loadedWorld.setBuoyancyEnabled(true);
    tickWater(loadedWorld,1800);
    require(loaded->isSleeping(),"reenabling buoyancy must lift and settle a sunken human or wolf corpse");
    waterTorsoImmersion(loadedWorld,*loaded,fixture.skeleton);
    std::printf("Water ragdoll %.*s mass_kg=%.9g torso_fraction=%.9g sleep_s=%.9g cold_30s_drift_cm=%.9g\n",
                int(fixture.skeleton.name().size()),fixture.skeleton.name().data(),double(nativeMass),double(immersion),double(firstSleep)/60.,double(maxDrift));
    }
  }

void testRagdollWaterDenseAndShallow() {
  for(const char* name:{"SKELETON.MDS","STONEGOLEM.MDS"}) {
    WaterRagdollFixture fixture(name);
    CollisionWorld world;
    waterSurface(world);
    auto ground=waterFloor(world);
    auto object=at(0,180,0); object.rotateOX(90);
    auto body=fixture.create(world,object);
    tickWater(world,1800);
    require(body->isSleeping() && position(body->pelvisTransform()).y<-350.f,
            "a skeleton or stone golem must sink and sleep at the deep bottom");
    size_t count=0;
    for(const auto& part:world.waterBodySnapshot()) {
      if(part.ragdoll!=body.get()) continue;
      ++count;
      require(part.targetFraction>1.f && part.submergedFraction>.99f,
              "all skeleton/golem parts must remain denser than water and fully submerged");
      }
    require(count==body->partCount(),"dense corpse water diagnostics must retain all parts");
    std::printf("Water ragdoll %s sunk_pelvis_cm=%.9g sleeping=1\n",name,double(position(body->pelvisTransform()).y));
    }

  WaterRagdollFixture fixture("HUMANS.MDS");
  CollisionWorld world;
  waterSurface(world);
  auto ground=waterFloor(world,-10.f);
  auto object=at(0,100,0); object.rotateOX(90);
  auto body=fixture.create(world,object);
  tickWater(world,1800);
  require(body->isSleeping(),"a corpse touching a ten-centimetre shallow bottom must settle without jitter");
  bool partlyWet=false;
  for(const auto& part:world.waterBodySnapshot())
    if(part.ragdoll==body.get() && part.submergedFraction>0.f && part.submergedFraction<.99f) partlyWet=true;
  require(partlyWet,"shallow stability must exercise a genuinely partly immersed corpse");
  const auto settled=body->state();
  float maxDrift=0;
  for(int frame=0;frame<1800;++frame) {
    world.tick(frame%3==2 ? 16 : 17);
    maxDrift=std::max(maxDrift,maxWaterDrift(body->state(),settled));
    require(body->isSleeping(),"a shallow corpse must stay asleep while water and ground support it together");
    }
  near(maxDrift,0,.1f,"a partly submerged corpse must remain motionless for thirty more seconds");
  std::printf("Water ragdoll shallow_10cm thirty_second_drift_cm=%.9g\n",double(maxDrift));
  }

void testRagdollWaterExcludesLivingReactions() {
  Fixture fixture;
  CollisionWorld wet,dry;
  waterSurface(wet);
  const auto object=at(0,-180,0);
  auto pose=fixture.skeleton.tr;
  for(auto& bone:pose) bone=object*bone;
  auto living=wet.createKinematicBody(fixture.skeleton,fixture.zones,pose.data(),pose.size(),object);
  auto control=dry.createKinematicBody(fixture.skeleton,fixture.zones,pose.data(),pose.size(),object);
  require(living && control,"living water exclusion needs native kinematic controls");
  std::vector<PhysicsNoise> noises;
  wet.setPhysicsNoiseEnabled(true);
  wet.setPhysicsNoiseCallback([&](const PhysicsNoise& noise) { if(noise.surface==asset::MaterialGroup::WATER) noises.push_back(noise); });
  tickWater(wet,10); tickWater(dry,10);
  require(living->isKinematic() && wet.waterBodySnapshot().empty(),"animation driven living parts must stay outside buoyancy");
  const auto point=position(pose[6])+Tempest::Vec3(10,0,0);
  require(living->queueHit(6,point,{0,0,3},1.f) && control->queueHit(6,point,{0,0,3},1.f),
          "living exclusion must also exercise a real dynamic hit reaction");
  float maxDifference=0;
  for(int frame=0;frame<120;++frame) {
    wet.tick(frame%3==2 ? 16 : 17); dry.tick(frame%3==2 ? 16 : 17);
    if(frame==0) require(!living->isKinematic() && !living->reactionBones().empty(),"the exclusion control must promote a living limb to native dynamics");
    require(wet.waterBodySnapshot().empty(),"an active living reaction must never be enrolled as a floating corpse");
    std::vector<Tempest::Matrix4x4> actual,expected;
    require(living->getPose(actual) && control->getPose(expected),"living wet/dry native poses must remain available");
    for(const auto bone:living->poseIndices())
      for(size_t component=0;component<16;++component) {
        const float error=std::abs(actual[bone].data()[component]-expected[bone].data()[component]);
        if(component>=12 && component<=14) maxDifference=std::max(maxDifference,error);
        near(actual[bone].data()[component],expected[bone].data()[component],
             component>=12 && component<=14 ? .02f : .00002f,
             "water must not change the pose, velocity response or recovery of a living native rig");
        }
    }
  require(noises.empty(),"kinematic and active living parts must not emit corpse splash noise");
  std::printf("Water living exclusion active_frames=120 maximum_pose_delta_cm=%.9g splashes=0\n",double(maxDifference));
  }

void testRagdollWaterSplashOnceAndMomentum() {
  struct Splash { float impulse=0,volume=0; };
  const auto drop=[](float scale,float speed) {
    WaterRagdollFixture fixture("HUMANS.MDS");
    CollisionWorld world;
    waterSurface(world);
    world.setPhysicsNoiseEnabled(true);
    std::vector<PhysicsNoise> noises;
    world.setPhysicsNoiseCallback([&](const PhysicsNoise& noise) { if(noise.surface==asset::MaterialGroup::WATER) noises.push_back(noise); });
    auto object=at(0,35.f,0); object.rotateOX(90); object.scale(scale);
    auto body=fixture.create(world,object,{0,-speed,0});
    // All horizontal parts enter in the same short impact. A single corpse
    // must not make one NPC noise for every limb in that impact window.
    tickWater(world,40);
    require(noises.size()==1,"one corpse entering water must dispatch one aggregated splash through the physics noise callback");
    const auto& splash=noises.front();
    require(splash.body!=0 && splash.impulse>0.f && std::isfinite(splash.impulse) &&
            splash.volume>0.f && splash.volume<=1.f && splash.radius>0.f &&
            splash.what.find("splash")!=std::string::npos,"corpse splash must carry finite audible momentum and hearing metadata");
    near(splash.position.y,0.f,.001f,"corpse splash noise must originate at the water surface");
    return Splash{splash.impulse,splash.volume};
    };
  const auto light=drop(.5f,40.f),heavy=drop(1.f,40.f),fast=drop(.5f,500.f);
  require(heavy.impulse>light.impulse && heavy.volume>=light.volume,
          "a heavier corpse must give greater entry momentum and at least as loud a splash");
  require(fast.impulse>light.impulse && fast.volume>=light.volume,
          "a faster corpse must give greater entry momentum and at least as loud a splash");
  std::printf("Water ragdoll splash impulses_Ns=[%.9g,%.9g,%.9g] volumes=[%.9g,%.9g,%.9g] noises_per_entry=1\n",
              double(light.impulse),double(heavy.impulse),double(fast.impulse),double(light.volume),double(heavy.volume),double(fast.volume));
  }

void testSlopeAndCliff() {
  Fixture fixture;
  {
    CollisionWorld world;
    auto tilted=at(0,-50,0); tilted.rotateOZ(-35);
    auto slope=world.addCollisionBody(new JPH::BoxShape(JPH::Vec3(20,.5f,20)),tilted,
                                     .01f,DynamicWorld::C_Landscape);
    auto body=fixture.create(world,at(0,100,0));
    const auto start=position(body->pelvisTransform());
    for(int i=0;i<300;++i) { world.tick(17); finite(body->state()); }
    const auto end=position(body->pelvisTransform());
    require(end.x>start.x+75.f && end.y<start.y-100.f,"a limp body must move down a low-friction slope");
    joints(fixture,*body);
    }
  {
    CollisionWorld world;
    auto platform=world.addCollisionBody(new JPH::BoxShape(JPH::Vec3(1,.5f,2)),at(0,-50,0),
                                        .75f,DynamicWorld::C_Landscape);
    auto body=fixture.create(world,at(250,300,0),{150,0,0});
    const auto start=position(body->pelvisTransform());
    for(int i=0;i<90;++i) { world.tick(17); finite(body->state()); }
    const auto end=position(body->pelvisTransform());
    require(end.y<start.y-600.f,"a body beyond a cliff must continue falling instead of standing on its NPC capsule");
    require(end.x>start.x+100.f,"initial NPC velocity must transfer to the falling body in cm/s");
    joints(fixture,*body);
    }
  }

void testStateRoundTripAndAtomicRejection() {
  Fixture fixture;
  CollisionWorld originalWorld, restoredWorld;
  auto originalGround=floor(originalWorld), restoredGround=floor(restoredWorld);
  auto original=fixture.create(originalWorld,at(0,400,0),{120,25,-40});
  auto restored=fixture.create(restoredWorld,at(0,400,0));
  for(int i=0;i<10;++i) { originalWorld.tick(17); restoredWorld.tick(17); }
  const auto moving=original->state();
  require(!original->isSleeping(),"roundtrip fixture must be moving");
  require(restored->restore(moving),"a compatible moving snapshot must restore");
  same(restored->state(),moving,.001f);
  // Both worlds have the same accumulated fixed-step remainder. Compare continuation,
  // not just serialization of fields that never reach the solver.
  for(int i=0;i<30;++i) { originalWorld.tick(17); restoredWorld.tick(17); }
  same(restored->state(),original->state(),.1f);

  const auto before=restored->state();
  std::vector<Ragdoll::State> invalid;
  auto state=before; ++state.version; invalid.push_back(state);
  state=before; state.skeleton="OTHER.MDS"; invalid.push_back(state);
  state=before; state.parts.pop_back(); invalid.push_back(state);
  state=before; state.parts[1].bone=state.parts[0].bone; invalid.push_back(state);
  state=before; state.parts[0].bone=uint32_t(fixture.skeleton.nodes.size()+10); invalid.push_back(state);
  state=before; state.parts[0].transform.set(3,1,std::numeric_limits<float>::quiet_NaN()); invalid.push_back(state);
  state=before; state.parts[0].linearVelocity.y=std::numeric_limits<float>::infinity(); invalid.push_back(state);
  state=before; state.parts[0].angularVelocity.x=std::numeric_limits<float>::quiet_NaN(); invalid.push_back(state);
  state=before; state.parts[0].referenceTransform.set(3,0,std::numeric_limits<float>::quiet_NaN()); invalid.push_back(state);
  state=before; state.parts[0].referenceTransform.scale(-1.f,1.f,1.f); invalid.push_back(state);
  state=before; state.parts[0].referenceTransform.scale(.8f,1.f,1.f); invalid.push_back(state);
  state=before; state.parts[0].referenceTransform.set(0,1,.1f); invalid.push_back(state);
  state=before; state.parts[0].centerOfMassMeters.x=std::numeric_limits<float>::quiet_NaN(); invalid.push_back(state);
  state=before; state.parts[0].centerOfMassMeters.y+=1.f; invalid.push_back(state);
  state=before; state.parts[0].rotation={0,0,0,0}; invalid.push_back(state);
  state=before; state.parts[0].linearVelocityMeters.x+=1.f; invalid.push_back(state);
  require(!before.ancestors.empty(),"fresh synthetic state must include its nonphysical BIP ancestor");
  state=before; state.ancestors.pop_back(); invalid.push_back(state);
  state=before; state.ancestors.push_back(state.ancestors.front()); invalid.push_back(state);
  state=before; state.ancestors[0].anchorBone=uint32_t(fixture.skeleton.nodes.size()+1); invalid.push_back(state);
  state=before; state.ancestors[0].bone=state.parts.front().bone; invalid.push_back(state);
  state=before; state.ancestors[0].referenceTransform.set(3,0,std::numeric_limits<float>::quiet_NaN()); invalid.push_back(state);
  state=before; state.ancestors[0].referenceTransform.scale(.8f,1.f,1.f); invalid.push_back(state);
  state=before; state.legacyAncestors=true; invalid.push_back(state);
  state=before; state.rootTransform.scale(2); invalid.push_back(state);
  for(const auto& bad:invalid) {
    require(!restored->restore(bad),"incompatible or nonfinite state must be rejected");
    same(restored->state(),before,.001f);
    }

  for(int i=0;i<1800 && !original->isSleeping();++i) originalWorld.tick(17);
  require(original->isSleeping(),"save roundtrip must include a real sleeping body");
  const auto sleeping=original->state();
  require(restored->restore(sleeping) && restored->isSleeping(),"restoring a sleeping snapshot must preserve sleep");
  same(restored->state(),sleeping,.001f);
  for(int i=0;i<30;++i) restoredWorld.tick(17);
  same(restored->state(),sleeping,.001f);
  }

void testMovedStateRebaseAndReference() {
  Fixture fixture;
  const Tempest::Vec3 displacement{1250,200,-800};
  const auto translated=[&](Tempest::Matrix4x4 matrix) {
    matrix.set(3,0,matrix.at(3,0)+displacement.x);
    matrix.set(3,1,matrix.at(3,1)+displacement.y);
    matrix.set(3,2,matrix.at(3,2)+displacement.z);
    return matrix;
    };
  const auto translatedState=[&](Ragdoll::State state) {
    require(Ragdoll::translateState(fixture.skeleton,state,displacement),"explicit state translation must retain consistent native precision");
    // Birth references describe the original geometry, not the current location.
    return state;
    };
  for(const bool anisotropic:{false,true}) {
    CollisionWorld originalWorld, restoredWorld;
    auto ground=floor(originalWorld);
    auto movedGround=restoredWorld.addCollisionBody(new JPH::BoxShape(JPH::Vec3(20,.5f,20)),
      translated(at(0,-50,0)),.75f,DynamicWorld::C_Landscape);
    auto object=at(0,400,0); object.rotateOY(31);
    if(anisotropic) object.scale(.9f,1.f,1.f);
    auto original=fixture.create(originalWorld,object,{120,25,-40});
    auto restored=fixture.create(restoredWorld,translated(object));
    const auto references=original->state();
    for(int i=0;i<10;++i) { originalWorld.tick(17); restoredWorld.tick(17); }
    const auto moved=translatedState(original->state());
    require(moved.version==3,"current bodies must save precise native poses plus affine creation references");
    require(restored->restore(moved),"moved compatible State3 snapshot must restore");
    same(restored->state(),moved,.001f);
    for(size_t part=0;part<moved.parts.size();++part)
      for(size_t i=0;i<16;++i)
        near(moved.parts[part].referenceTransform.data()[i],references.parts[part].referenceTransform.data()[i],0,
             "moving a body must leave every creation reference component unchanged");
    for(int i=0;i<30;++i) { originalWorld.tick(17); restoredWorld.tick(17); }
    same(restored->state(),translatedState(original->state()),.1f);
    }
  }

void testInvalidCreationAndScale() {
  Fixture fixture;
  CollisionWorld world;
  auto ground=floor(world);
  const auto count=world.bodyCount();
  auto pose=fixture.skeleton.tr;
  const auto object=at(0,300,0);
  for(auto& matrix:pose) matrix=object*matrix;
  require(!world.createRagdoll(fixture.skeleton,fixture.zones,nullptr,pose.size(),object),"null pose must not create parts");
  require(!world.createRagdoll(fixture.skeleton,fixture.zones,pose.data(),pose.size()-1,object),"incomplete pose must not create parts");
  auto invalidPose=pose;
  invalidPose[1].set(3,1,std::numeric_limits<float>::quiet_NaN());
  require(!world.createRagdoll(fixture.skeleton,fixture.zones,invalidPose.data(),invalidPose.size(),object),"nonfinite pose must not create parts");
  require(world.bodyCount()==count,"rejected creations must leave no partial bodies");
  auto scaled=at(0,300,0); scaled.scale(1.5f);
  auto body=fixture.create(world,scaled);
  std::vector<Tempest::Matrix4x4> output;
  require(body->getPose(output),"scaled ragdoll pose must be available");
  for(auto bone:body->boneIndices()) {
    const auto& matrix=output[bone];
    const auto axis=Tempest::Vec3(matrix.at(0,0),matrix.at(0,1),matrix.at(0,2));
    near(axis.length(),1.5f,.0001f,"physical bones must retain visual uniform scale");
    }
  finite(body->state());
  }

void replayHumanAnatomy(const Skeleton& skeleton, const HitZones& zones) {
  const auto rotateVector=[](const Tempest::Matrix4x4& matrix, const Tempest::Vec3& value) {
    const auto* m=matrix.data();
    return Tempest::Vec3(m[0]*value.x+m[4]*value.y+m[8]*value.z,
                        m[1]*value.x+m[5]*value.y+m[9]*value.z,
                        m[2]*value.x+m[6]*value.y+m[10]*value.z);
    };
  const auto inverseRotate=[](const Tempest::Matrix4x4& matrix, const Tempest::Vec3& value) {
    const auto* m=matrix.data();
    return Tempest::Vec3(m[0]*value.x+m[1]*value.y+m[2]*value.z,
                        m[4]*value.x+m[5]*value.y+m[6]*value.z,
                        m[8]*value.x+m[9]*value.y+m[10]*value.z);
    };
  size_t controls=0;
  for(size_t bone=0;bone<skeleton.nodes.size();++bone) {
    const auto& name=skeleton.nodes[bone].name;
    if(name.find("CALF")==std::string::npos && name.find("FOREARM")==std::string::npos)
      continue;
    const size_t parent=skeleton.nodes[bone].parent;
    size_t endpoint=skeleton.nodes.size();
    for(size_t child=0;child<skeleton.nodes.size();++child)
      if(skeleton.nodes[child].parent==bone && (skeleton.nodes[child].name.find("FOOT")!=std::string::npos ||
                                               skeleton.nodes[child].name.find("HAND")!=std::string::npos)) endpoint=child;
    require(parent<skeleton.nodes.size() && endpoint<skeleton.nodes.size(),"actual Human hinge needs its anatomical parent and endpoint");
    const auto pivot=position(skeleton.tr[bone]);
    const auto incoming=Tempest::Vec3::normalize(pivot-position(skeleton.tr[parent]));
    const auto outgoing=Tempest::Vec3::normalize(position(skeleton.tr[endpoint])-pivot);
    const Tempest::Vec3 cross {incoming.y*outgoing.z-incoming.z*outgoing.y,
                              incoming.z*outgoing.x-incoming.x*outgoing.z,
                              incoming.x*outgoing.y-incoming.y*outgoing.x};
    const auto axis=Tempest::Vec3::normalize(cross);
    const float bindBend=std::atan2(cross.length(),Tempest::Vec3::dotProduct(incoming,outgoing))*180.f/float(3.14159265358979323846);
    require(bindBend>5.f && bindBend<45.f,"actual Human bind must exercise a nonzero anatomical hinge offset");
    for(const float yaw:{0.f,37.f})
      for(const float wanted:{-30.f,30.f}) {
        auto object=at(0,700,0); object.rotateOY(yaw); object.scale(.9f,1.f,1.f);
        auto rotation=Tempest::Matrix4x4::mkIdentity(); rotation.rotate(wanted-bindBend,axis.x,axis.y,axis.z);
        const auto around=at(pivot.x,pivot.y,pivot.z)*rotation*at(-pivot.x,-pivot.y,-pivot.z);
        std::vector<Tempest::Matrix4x4> worldPose;
        for(size_t node=0;node<skeleton.nodes.size();++node) {
          bool child=false;
          for(size_t ancestor=node;ancestor<skeleton.nodes.size();ancestor=skeleton.nodes[ancestor].parent)
            if(ancestor==bone) { child=true; break; }
          worldPose.push_back(object*(child ? around*skeleton.tr[node] : skeleton.tr[node]));
          }
        CollisionWorld world;
        auto body=world.createRagdoll(skeleton,zones,worldPose.data(),worldPose.size(),object);
        require(body!=nullptr && body->valid(),"actual bent/yawed Human reference must create");
        const auto angle=[&] {
          const auto state=body->state();
          const Ragdoll::PartState* p=nullptr; const Ragdoll::PartState* c=nullptr;
          for(const auto& part:state.parts) { if(part.bone==parent) p=&part; if(part.bone==bone) c=&part; }
          require(p!=nullptr && c!=nullptr,"actual hinge native parent/child frames must be present");
          // Bring the same MODEL reference into each current native frame using
          // its canonical bind-local coordinates. Global yaw and parent tumbling
          // cancel; visual affine stretch never enters the native angle oracle.
          const auto axisNow=rotateVector(p->transform,inverseRotate(skeleton.tr[parent],axis));
          const auto planeParent=rotateVector(p->transform,inverseRotate(skeleton.tr[parent],outgoing));
          const auto planeChild=rotateVector(c->transform,inverseRotate(skeleton.tr[bone],outgoing));
          const Tempest::Vec3 crossed {planeParent.y*planeChild.z-planeParent.z*planeChild.y,
                                       planeParent.z*planeChild.x-planeParent.x*planeChild.z,
                                       planeParent.x*planeChild.y-planeParent.y*planeChild.x};
          return bindBend+std::atan2(Tempest::Vec3::dotProduct(axisNow,crossed),Tempest::Vec3::dotProduct(planeParent,planeChild))*
            180.f/float(3.14159265358979323846);
          };
        const float initial=angle();
        near(initial,wanted,.02f,"actual Human initial anatomical angle must survive native polar conversion and global yaw");
        for(int tick=0;tick<60;++tick) { world.tick(17); finite(body->state()); }
        const float final=angle();
        std::printf("actual Human hinge=%s yaw_deg=%.9g bind_deg=%.9g wanted_deg=%.9g initial_deg=%.9g after60_deg=%.9g\n",
                    name.c_str(),double(yaw),double(bindBend),double(wanted),double(initial),double(final));
        if(wanted<0) near(final,0,5.f,"actual Human hyperextension must settle at its anatomical straight limit");
        else near(final,30,5.f,"actual mirrored Human elbow/knee must retain allowed positive flexion");
        ++controls;
        }
    }
  require(controls==16,"actual Human must cover both knees/elbows, hyperextension/flexion and global yaw");
  }

void replayCreation(const char* input) {
  std::ifstream file(input);
  require(bool(file),"creation replay input must be readable");
  size_t cases=0;
  std::string line;
  while(std::getline(file,line)) {
    rapidjson::Document capture;
    capture.Parse(line.c_str());
    require(!capture.HasParseError() && capture.IsObject(),"creation replay must contain JSON objects");
    require(capture.HasMember("nodes") && capture["nodes"].IsArray() && capture.HasMember("skeleton") && capture["skeleton"].IsString(),
            "creation replay needs an actual skeleton");
    const auto matrix=[](const rapidjson::Value& array) {
      require(array.IsArray() && array.Size()==16,"creation replay matrix must have sixteen components");
      float values[16];
      for(size_t i=0;i<16;++i) {
        require(array[rapidjson::SizeType(i)].IsNumber(),"creation replay matrix component must be numeric");
        values[i]=float(array[rapidjson::SizeType(i)].GetDouble());
        require(std::isfinite(values[i]),"creation replay matrix must be finite");
        }
      Tempest::Matrix4x4 result; result.setData(values); return result;
      };
    const auto vector=[](const rapidjson::Value& array) {
      require(array.IsArray() && array.Size()==3,"creation replay vector must have three components");
      for(const auto& value:array.GetArray()) require(value.IsNumber() && std::isfinite(value.GetDouble()),"creation replay vector must be finite");
      return Tempest::Vec3(float(array[0].GetDouble()),float(array[1].GetDouble()),float(array[2].GetDouble()));
      };
    zenkit::ModelHierarchy source{};
    // Captured Skeleton::nodes already include root_translation on the root.
    // Adding it again would change the fixture instead of replaying the input.
    source.root_translation={0,0,0};
    source.bbox.min={float(capture["bbox"][0][0].GetDouble()),float(capture["bbox"][0][1].GetDouble()),float(capture["bbox"][0][2].GetDouble())};
    source.bbox.max={float(capture["bbox"][1][0].GetDouble()),float(capture["bbox"][1][1].GetDouble()),float(capture["bbox"][1][2].GetDouble())};
    source.collision_bbox.min={float(capture["collision_bbox"][0][0].GetDouble()),float(capture["collision_bbox"][0][1].GetDouble()),float(capture["collision_bbox"][0][2].GetDouble())};
    source.collision_bbox.max={float(capture["collision_bbox"][1][0].GetDouble()),float(capture["collision_bbox"][1][1].GetDouble()),float(capture["collision_bbox"][1][2].GetDouble())};
    std::vector<Tempest::Matrix4x4> worldPose;
    for(const auto& node:capture["nodes"].GetArray()) {
      zenkit::ModelHierarchyNode output;
      output.name=node["name"].GetString(); output.parent_index=int16_t(node["parent"].GetInt());
      const auto local=matrix(node["local"]);
      std::memcpy(&output.transform,&local,sizeof(local));
      source.nodes.push_back(std::move(output)); worldPose.push_back(matrix(node["world"]));
      }
    Skeleton skeleton(asset::fromZenKit(source),nullptr,capture["skeleton"].GetString());
    for(size_t i=0;i<skeleton.tr.size();++i) {
      const auto expected=matrix(capture["nodes"][rapidjson::SizeType(i)]["bind"]);
      for(size_t c=0;c<16;++c)
        near(skeleton.tr[i].data()[c],expected.data()[c],0,"reconstructed capture bind must match every original float");
      }
    HitZones zones; zones.setSkeleton(&skeleton);
    require(zones.isValid(),"captured actual skeleton must reproduce valid canonical HitZones");
    const auto object=matrix(capture["object"]);
    const auto actorOrigin=position(object);
    float lowest=std::numeric_limits<float>::max();
    for(const auto& frame:worldPose) lowest=std::min(lowest,position(frame).y);
    const auto groundFrame=at(actorOrigin.x,lowest-250,actorOrigin.z);
    CollisionWorld world;
    auto ground=world.addCollisionBody(new JPH::BoxShape(JPH::Vec3(20,.5f,20)),groundFrame,.75f,DynamicWorld::C_Landscape);
    auto body=world.createRagdoll(skeleton,zones,worldPose.data(),worldPose.size(),object,vector(capture["velocity"]));
    require(body!=nullptr && body->valid(),"exact captured real-model input must create a native body");
    require(body->partCount()==capture["physical_owners"].Size(),"actual captured physical owner coverage must match");
    near(body->mass(),Ragdoll::speciesMass(skeleton.name())*scaleVolume(object),.01f,"actual native body must use its species mass and captured object volume scale");
    std::vector<Tempest::Matrix4x4> initial;
    require(body->getPose(initial),"actual native initial visual frames must be available");
    float positionError=0,basisError=0;
    for(auto bone:body->poseIndices()) {
      positionError=std::max(positionError,(position(initial[bone])-position(worldPose[bone])).length());
      for(size_t c=0;c<16;++c)
        if(c<12 || c==15) basisError=std::max(basisError,std::abs(initial[bone].data()[c]-worldPose[bone].data()[c]));
      }
    // 23,000 cm captured coordinates have binary32 ULP about .002 cm. This
    // creation check covers that conversion; the save/load bound stays .001 cm.
    std::printf("creation replay case=%zu model=%.*s parts=%zu mass_kg=%.9g initial_position_error_cm=%.9g initial_basis_error=%.9g\n",
                cases,int(skeleton.name().size()),skeleton.name().data(),body->partCount(),double(body->mass()),double(positionError),double(basisError));
    near(positionError,0,.005f,"native creation must preserve actual world bone positions within conversion precision");
    near(basisError,0,.00001f,"native creation must preserve actual affine bone orientation and scale");
    const auto state=body->state();
    require(state.version==3,"actual nonuniform bodies need precise native State3 with affine/ancestor references");
    require(body->restore(state),"actual captured moving state must restore on the native body");
    same(body->state(),state,.001f);
    for(int frame=0;frame<30;++frame) { world.tick(17); finite(body->state()); }
    const auto coldRoundtrip=[&](const char* phase) {
      const auto saved=body->state();
      std::vector<Tempest::Matrix4x4> before;
      require(body->getPose(before),"captured actor must expose all native and ancestor frames before a cold restore");
      CollisionWorld loadedWorld;
      auto loadedGround=loadedWorld.addCollisionBody(new JPH::BoxShape(JPH::Vec3(20,.5f,20)),groundFrame,.75f,DynamicWorld::C_Landscape);
      auto loaded=loadedWorld.createRagdoll(skeleton,zones,worldPose.data(),worldPose.size(),object);
      require(loaded && loaded->restore(saved),"actual large-world actor must restore into a fresh native world");
      same(loaded->state(),saved,.001f);
      require(loaded->poseIndices()==body->poseIndices(),"cold restore must retain every physical and anchored ancestor frame");
      const auto compareFrames=[&] {
        std::vector<Tempest::Matrix4x4> after;
        require(loaded->getPose(after),"cold restored actor must expose final native and ancestor frames");
        float maxPosition=0,maxBasis=0;
        for(auto bone:loaded->poseIndices()) {
          maxPosition=std::max(maxPosition,(position(after[bone])-position(before[bone])).length());
          for(size_t c=0;c<16;++c) if(c<12 || c==15)
            maxBasis=std::max(maxBasis,std::abs(after[bone].data()[c]-before[bone].data()[c]));
          }
        near(maxPosition,0,.001f,"cold restore must preserve all native/ancestor world frames within the unchanged save bound");
        near(maxBasis,0,.00001f,"cold restore must preserve all native/ancestor frame bases");
        return maxPosition;
        };
      const float error=compareFrames();
      if(body->isSleeping()) {
        for(int tick=0;tick<60;++tick) loadedWorld.tick(17);
        same(loaded->state(),saved,.001f);
        compareFrames();
        }
      std::printf("State3 cold case=%zu phase=%s mapped_frames=%zu exact_native_fields=PASS max_world_position_error_cm=%.9g\n",
                  cases,phase,loaded->poseIndices().size(),double(error));
      // Explicit teleport keeps immutable birth references while rebuilding COM
      // from the requested public origin. Large -> near-zero cancellation is
      // deliberately exercised before a new native candidate is constructed.
      for(const auto target:{Tempest::Vec3(10,200,-12),Tempest::Vec3(-23000,900,-12000),Tempest::Vec3(30000,1100,25000)}) {
        auto moved=saved;
        const auto displacement=target-position(saved.rootTransform);
        const auto requestedOrigin=position(saved.rootTransform)+displacement;
        if(target.x==10) {
          auto naive=saved;
          for(size_t c=0;c<3;++c) {
            const float delta=c==0 ? displacement.x : c==1 ? displacement.y : displacement.z;
            naive.rootTransform.set(3,int(c),naive.rootTransform.at(3,int(c))+delta);
            for(auto& part:naive.parts) part.transform.set(3,int(c),part.transform.at(3,int(c))+delta);
            }
          for(auto& part:naive.parts) part.centerOfMassMeters+=displacement*.01f;
          require(!loaded->restore(naive),"independent large-world cm/COM translation must be rejected rather than weakening the precision guard");
          }
        require(Ragdoll::relocateState(skeleton,moved,target),"large-world active/dormant absolute teleport must rebuild consistent COM fields");
        // Keep the displacement-input rounding visible. The absolute API must
        // retain the original destination, without inheriting that error.
        std::printf("State3 teleport case=%zu phase=%s target_cm=[%.9g,%.9g,%.9g] displacement_rounding_cm=%.9g export_error_cm=%.9g\n",
                    cases,phase,double(target.x),double(target.y),double(target.z),double((requestedOrigin-target).length()),
                    double((position(moved.rootTransform)-target).length()));
        near((position(moved.rootTransform)-target).length(),0,.001f,"absolute state teleport must retain its requested destination");
        for(size_t p=0;p<moved.parts.size();++p)
          for(size_t c=0;c<16;++c)
            near(moved.parts[p].referenceTransform.data()[c],saved.parts[p].referenceTransform.data()[c],0,"teleport must not change native geometry birth references");
        for(size_t a=0;a<moved.ancestors.size();++a)
          for(size_t c=0;c<16;++c)
            near(moved.ancestors[a].referenceTransform.data()[c],saved.ancestors[a].referenceTransform.data()[c],0,"teleport must not change ancestor birth references");
        require(loaded->restore(moved),"teleported large-world DTO must restore atomically into native bodies");
        same(loaded->state(),moved,.001f);
        }
      };
    require(!body->isSleeping(),"actual corpus awake roundtrip must include moving native parts");
    coldRoundtrip("awake");
    for(int tick=0;tick<2000 && !body->isSleeping();++tick) { world.tick(17); finite(body->state()); }
    require(body->isSleeping(),"actual corpus sleeping roundtrip must use a settled native actor");
    coldRoundtrip("sleeping");
    if(cases==0 && skeleton.name().find("HUMAN")!=std::string_view::npos)
      replayHumanAnatomy(skeleton,zones);
    ++cases;
    }
  require(cases>0,"creation replay must not silently accept an empty corpus");
  std::printf("Actual creation corpus passed: %zu cases\n",cases);
  }

void testAnatomicalKneeWithLongitudinalBoneAxis() {
  auto source=hierarchy();
  // Keep the thigh/calf/foot origins anatomically straight, but align the calf's
  // LOCAL X with the limb, as Gothic's actual MAN skeleton does. A local-axis
  // projection must not replace the stable model sagittal flexion direction.
  auto calf=at(0,-42,0); calf.rotateOZ(90);
  auto foot=at(-40,0,0); foot.rotateOZ(-90);
  std::memcpy(&source.nodes[13].transform,&calf,sizeof(calf));
  std::memcpy(&source.nodes[14].transform,&foot,sizeof(foot));
  Skeleton skeleton(asset::fromZenKit(source),nullptr,"HUMANS_LONGITUDINAL_BIND.MDS");
  HitZones zones; zones.setSkeleton(&skeleton);
  auto object=at(0,700,0); object.scale(.9f,1.f,1.f);
  for(const float degrees:{-30.f,30.f}) {
    std::vector<Tempest::Matrix4x4> worldPose;
    for(const auto& frame:skeleton.tr) worldPose.push_back(object*frame);
    const auto pivot=position(worldPose[13]);
    auto turn=Tempest::Matrix4x4::mkIdentity(); turn.rotateOX(degrees);
    const auto around=at(pivot.x,pivot.y,pivot.z)*turn*at(-pivot.x,-pivot.y,-pivot.z);
    for(const auto bone:{13,14,15}) worldPose[size_t(bone)]=around*worldPose[size_t(bone)];
    CollisionWorld world;
    auto body=world.createRagdoll(skeleton,zones,worldPose.data(),worldPose.size(),object);
    require(body!=nullptr && body->valid(),"longitudinal-axis anatomical fixture must create");
    const auto kneeAngle=[&] {
      std::vector<Tempest::Matrix4x4> pose; require(body->getPose(pose),"knee pose must be available");
      const auto incoming=Tempest::Vec3::normalize(position(pose[13])-position(pose[12]));
      const auto outgoing=Tempest::Vec3::normalize(position(pose[14])-position(pose[13]));
      const float x=incoming.y*outgoing.z-incoming.z*outgoing.y;
      return std::atan2(x,Tempest::Vec3::dotProduct(incoming,outgoing))*180.f/float(3.14159265358979323846);
      };
    const float initial=kneeAngle();
    near(initial,degrees,.01f,"initial geometric knee bend must be independent of the bone's longitudinal local axis");
    for(int tick=0;tick<60;++tick) { world.tick(17); finite(body->state()); }
    const float final=kneeAngle();
    std::printf("anatomical calf bindX=longitudinal input_deg=%.9g initial_deg=%.9g after60_deg=%.9g\n",
                double(degrees),double(initial),double(final));
    // The limit snaps a hyperextended knee straight; a corpse's light knee
    // friction lets that swing carry on into some flexion, never backwards.
    if(degrees<0)
      require(final>-5.f && final<45.f,"a knee must correct thirty-degree anatomical hyperextension toward its straight limit");
    else
      require(final>5.f && final<60.f,"a knee must allow positive anatomical flexion rather than locking the projected local-axis plane");
    }
  }

void testPoseOverlayAndRecovery() {
  Fixture fixture;
  Pose pose;
  pose.setSkeleton(&fixture.skeleton);
  auto object=at(1100,120,-800); object.rotateOY(37);
  pose.setObjectMatrix(object,true);
  pose.update(1000,true);
  std::vector<Tempest::Matrix4x4> animated(pose.transform(),pose.transform()+pose.boneCount());
  auto physical=animated;
  physical[1].set(3,0,physical[1].at(3,0)+75.f);
  physical[1].set(3,1,physical[1].at(3,1)-30.f);
  physical[1].rotateOZ(42);
  require(pose.setRagdollPose(&fixture.skeleton,physical,{1}),"world-space physical pelvis must overlay the expected skeleton");
  pose.update(1016,true);
  for(size_t i=0;i<16;++i)
    near(pose.bone(1).data()[i],physical[1].data()[i],.0001f,"mapped bone must take the physical world frame exactly");
  auto inverse=animated[1]; inverse.inverse();
  const auto expectedSpine=physical[1]*(inverse*animated[2]);
  for(size_t i=0;i<16;++i)
    near(pose.bone(2).data()[i],expectedSpine.data()[i],.0002f,"unmapped child must retain its animated relation to the physical parent");
  std::vector<Tempest::Matrix4x4> shown(pose.transform(),pose.transform()+pose.boneCount());
  pose.clearRagdollPose(true,1016);
  require(!pose.hasRagdollPose() && pose.isRagdollBlending(),"physical overlay must transfer to an animation transition");
  pose.update(1016,true);
  for(size_t b=0;b<shown.size();++b)
    near((position(pose.bone(b))-position(shown[b])).length(),0,.001f,"first recovery frame must preserve every displayed world-bone position");
  for(uint64_t tick=1032;tick<=2000;tick+=16) pose.update(tick,true);
  require(!pose.isRagdollBlending(),"the physical-to-animation transition must finish");
  for(size_t b=0;b<animated.size();++b)
    near((position(pose.bone(b))-position(animated[b])).length(),0,.001f,"finished recovery must return to the animated pose");
  auto malformed=physical; malformed[1].set(3,0,std::numeric_limits<float>::quiet_NaN());
  require(!pose.setRagdollPose(&fixture.skeleton,malformed,{1}) && !pose.hasRagdollPose(),"nonfinite physical overlay must not reach rendering");
  require(!pose.setRagdollPose(&fixture.skeleton,physical,{physical.size()+1}),"out-of-range physical bone must be rejected");
  }

void testRecoveryMatchesAnimBlendAndSaves() {
  constexpr size_t count=3;
  asset::AnimationSample from[count],to[count],physical[count],ordinary[count];
  for(size_t i=0;i<count;++i) {
    from[i].rotation={1,0,0,0}; from[i].position={float(i*20),float(i*13),float(i*8)};
    to[i].rotation={std::cos(.3f*float(i+1)),std::sin(.3f*float(i+1)),0,0};
    to[i].position={float(i*4),float(i*18),float(i*6)};
    physical[i]=ordinary[i]=from[i];
    }
  Animation::Sequence oldAnimation,newAnimation;
  oldAnimation.name="old"; newAnimation.name="new";
  AnimBlend baseline,recovery;
  baseline.reset(count); recovery.reset(count);
  for(size_t i=0;i<count;++i) baseline.drive(i,oldAnimation,1000,from[i],from[i],60);
  baseline.apply(from,ordinary,count,1000);
  baseline.apply(from,ordinary,count,1016);
  for(size_t i=0;i<count;++i) baseline.drive(i,newAnimation,1016,to[i],to[i],60);
  recovery.transitionFrom(from,count,1016);
  for(uint64_t tick=1016;tick<=1600;tick+=16) {
    recovery.apply(to,physical,count,tick); baseline.apply(to,ordinary,count,tick);
    for(size_t b=0;b<count;++b) {
      const auto actual=mkMatrix(physical[b]), expected=mkMatrix(ordinary[b]);
      for(size_t i=0;i<16;++i)
        near(actual.data()[i],expected.data()[i],.0001f,"physical recovery must follow the ordinary AnimBlend transition");
      }
    }
  require(!recovery.apply(to,physical,count,1800),"physical recovery must settle after the usual blend interval");
  for(const uint64_t saveTick:{uint64_t(2000),uint64_t(2048)}) {
    AnimBlend before,after;
    before.reset(count); before.transitionFrom(from,count,2000);
    asset::AnimationSample first[count],second[count];
    std::copy_n(from,count,first);
    if(saveTick>2000) before.apply(to,first,count,saveTick);
    std::vector<uint8_t> bytes;
    { Tempest::MemWriter writer(bytes); Serialize out(writer); out.setEntry("return"); before.saveExternal(out); }
    { Tempest::MemReader reader(bytes); Serialize in(reader); require(in.setEntry("return"),"return archive entry must exist"); after.loadExternal(in,count); }
    std::copy_n(first,count,second);
    for(uint64_t tick=saveTick;tick<2400;tick+=16) {
      before.apply(to,first,count,tick); after.apply(to,second,count,tick);
      for(size_t b=0;b<count;++b)
        require(std::memcmp(&first[b],&second[b],sizeof(first[b]))==0,"pending and mid-curve saved transitions must continue exactly");
      }
    }
  }

void testMalformedRecoveryArchive() {
  // Construct the actual optional-entry wire format independently of AnimBlend's private Curve.
  // The valid control is 10*(1-10*u^3+15*u^4-6*u^5), u=t/.5, with no rotation or side offset.
  struct Curve {
    float t1=0, x0=0, v0=0, a0=0, A=0, B=0, C=0;
    };
  const Curve zero;
  const AnimBlend::Sample identity{{0,0,0},{1,0,0,0}};
  struct Archive {
    uint64_t outTime=1000, lastTime=0, externalStart=1000, start=1000;
    bool clockPending=false, active=true, fade=false;
    float end=.5f, fadeTime=0;
    Curve externalFade, rot, rotSide, pos{.5f,10.f,0,0,-1920.f,2400.f,-800.f}, posSide;
    Tempest::Vec3 axis{0,0,0}, axisSide{0,0,0}, dir{1,0,0}, dirSide{0,0,0};
    AnimBlend::Sample from{{0,0,0},{1,0,0,0}}, last{{0,0,0},{1,0,0,0}};
    std::vector<AnimBlend::Sample> external, previous;
    uint64_t previousTime=0;
    };
  const auto write=[&](const Archive& state) {
    std::vector<uint8_t> bytes;
    {
      Tempest::MemWriter writer(bytes); Serialize out(writer); out.setEntry("return");
      out.write(uint32_t(1),state.outTime,state.lastTime,state.externalStart,state.clockPending);
      const auto curve=[&](const Curve& c) { out.write(c.t1,c.x0,c.v0,c.a0,c.A,c.B,c.C); };
      curve(state.externalFade);
      out.write(state.active,state.start,state.end,state.fade,state.from,state.fadeTime);
      curve(state.rot); curve(state.rotSide); curve(state.pos); curve(state.posSide);
      out.write(state.axis,state.axisSide,state.dir,state.dirSide,state.last);
      out.write(uint32_t(state.external.size()));
      for(const auto& sample:state.external) out.write(sample);
      out.write(state.previousTime,uint32_t(state.previous.size()));
      for(const auto& sample:state.previous) out.write(sample);
    }
    return bytes;
    };
  const auto read=[&](const Archive& state, AnimBlend& blend) {
    const auto bytes=write(state);
    Tempest::MemReader reader(bytes); Serialize in(reader);
    require(in.setEntry("return"),"handcrafted recovery entry must exist");
    blend.loadExternal(in,1);
    };
  const auto reject=[&](const Archive& state, const char* message) {
    bool rejected=false;
    try { AnimBlend blend; read(state,blend); }
    catch(const std::runtime_error&) { rejected=true; }
    require(rejected,message);
    };

  const Archive control;
  AnimBlend valid; read(control,valid);
  AnimBlend::Sample out=identity;
  valid.apply(&identity,&out,1,1000); near(out.position.x,10.f,.00001f,"canonical recovery must retain its initial offset");
  valid.apply(&identity,&out,1,1250); near(out.position.x,5.f,.00001f,"canonical recovery must keep the quintic midpoint");
  valid.apply(&identity,&out,1,1500); near(out.position.x,0.f,.00001f,"canonical recovery must end at the raw pose");

  Archive state=control;
  state.pos={.3f,1.e38f,0,0,0,0,0}; state.end=.3f; state.dir={100,0,0};
  volatile float hugeOffset=state.pos.x0, hugeDirection=state.dir.x;
  require(!std::isfinite(hugeOffset*hugeDirection),"finite huge offset and direction must reproduce the first-frame overflow control");
  reject(state,"finite huge recovery offset and nonunit direction must be rejected before rendering");
  state.dir={1,0,0};
  reject(state,"a unit direction must not make an out-of-domain recovery offset admissible");

  state=control;
  state.pos={256.f,0,0,0,std::ldexp(1.f,92),-std::ldexp(1.f,100),0}; state.end=256.f;
  volatile float interiorTime=192.f;
  const float interior=((((state.pos.A*interiorTime+state.pos.B)*interiorTime+state.pos.C)*interiorTime+
                         .5f*state.pos.a0)*interiorTime+state.pos.v0)*interiorTime+state.pos.x0;
  require(std::isfinite(state.pos.A) && std::isfinite(state.pos.B) && state.pos.x0==0 &&
          !std::isfinite(interior),"unit-direction polynomial must overflow inside its interval despite finite coefficients and zero endpoint values");
  reject(state,"recovery must reject an interior-overflow polynomial with unit direction and harmless endpoint values");

  state=control; state.pos.A+=1.f;
  reject(state,"finite noncanonical recovery coefficients must be rejected");
  state=control; state.pos.x0=-10.f;
  reject(state,"negative recovery offset length must be rejected");
  state=control; state.pos.v0=7000.f;
  reject(state,"recovery velocity beyond the producer's clipped relative velocity must be rejected");
  state=control; state.pos.t1=.00001f; state.end=state.pos.t1;
  reject(state,"a positive subdomain recovery duration must be rejected");
  state=control; state.pos.t1=601.f; state.end=state.pos.t1;
  reject(state,"recovery duration beyond the bounded save domain must be rejected");
  state=control; state.pos.t1=-.5f;
  reject(state,"negative recovery duration must be rejected");
  state=control; state.pos.C=std::numeric_limits<float>::quiet_NaN();
  reject(state,"nonfinite recovery coefficients must be rejected");
  state=control; state.end=.4f;
  reject(state,"recovery end must agree with all serialized curve durations");
  state=control; state.active=true; state.pos=zero; state.end=0; state.dir={0,0,0};
  reject(state,"an active recovery offset must have a positive interval");
  state=control; state.lastTime=1001;
  reject(state,"recovery history must not lie after the saved output clock");
  state=control; state.externalStart=1001;
  reject(state,"a started recovery must not begin after its saved output clock");
  state=control; state.start=1001;
  reject(state,"a bone recovery start must not underflow the saved output clock");
  state=control; state.clockPending=true;
  reject(state,"pending recovery clock must require its pending external pose");
  state=control; state.previousTime=900;
  reject(state,"an empty previous recovery pose must not retain a velocity clock");
  state=control; state.previousTime=900; state.previous.push_back(identity);
  reject(state,"recovery velocity history must require its matching pending pose");
  state=control; state.dir={2,0,0};
  reject(state,"a used recovery displacement direction must be unit length");
  state=control; state.dir={0,0,0};
  reject(state,"a used recovery displacement direction must not be zero");
  state=control; state.last.rotation={2,0,0,0};
  reject(state,"used recovery history must contain unit quaternions");
  state=control; state.last.rotation={0,0,0,0};
  reject(state,"used recovery history must not contain an unused zero quaternion");
  state=control; state.last.position.x=1.e38f;
  reject(state,"finite recovery samples outside the saved coordinate domain must be rejected");
  state=control; state.fade=true;
  reject(state,"external recovery must not accept an ordinary cross-fade record");
  state=control; state.fadeTime=.5f;
  reject(state,"external recovery must not accept an unused cross-fade duration");

  // A pending seed has not initialized its offsets/history yet. Their default zero quaternions
  // are legitimate only in that unused state; the physical external pose itself is unit-valued.
  Archive pending;
  pending.clockPending=true; pending.active=false; pending.end=0; pending.pos=zero;
  pending.dir={0,0,0}; pending.from.rotation={0,0,0,0}; pending.last.rotation={0,0,0,0};
  pending.external.push_back(identity);
  AnimBlend pendingValid; read(pending,pendingValid);
  require(pendingValid.hasExternalSeed(),"valid pending recovery must retain its uninitialized history and external seed");
  state=pending; state.external[0].rotation={0,0,0,0};
  reject(state,"a pending physical pose must contain a unit quaternion");
  state=pending; state.externalFade.x0=1.f;
  reject(state,"pending recovery must not already contain an initialized fade curve");
  state=pending; state.previousTime=1000; state.previous.push_back(identity);
  reject(state,"pending velocity history must strictly precede the saved output clock");
  state=pending; state.previousTime=749; state.previous.push_back(identity);
  reject(state,"pending velocity history must remain within the measured producer gap");
  state=pending; state.previousTime=984; state.previous.push_back(identity);
  AnimBlend previousValid; read(state,previousValid);
  require(previousValid.hasExternalSeed(),"valid pending physical velocity history must remain loadable");
  state.previous[0].rotation={0,0,0,0};
  reject(state,"used pending physical velocity history must contain a unit quaternion");
  }

void testPoseActualDriverSave() {
  // Imported MAN data, AnimationSolver name resolution, Pose layers and its legacy ZIP entry:
  // an ordinary offset must already be active when the physical handoff takes ownership.
  auto pattern=(std::filesystem::temp_directory_path()/"gothic-pose-driver-XXXXXX").string();
  const auto* directory=mkdtemp(pattern.data());
  require(directory!=nullptr,"pose driver fixture directory must be created");
  const std::filesystem::path root=directory;
  const auto base=root/"base";
  std::filesystem::create_directories(root/"Data");
  std::filesystem::create_directories(root/"_work/Data/Scripts/_compiled");
  {
    const std::byte marker {1};
    zenkit::Vfs vfs;
    vfs.mount(zenkit::VfsNode::file("FIXTURE.BIN",{&marker,1,false}),"/");
    auto archive=zenkit::Write::to(root/"Data/FIXTURE.VDF");
    vfs.save(archive.get(),zenkit::GameVersion::GOTHIC_2);
  }
  const auto source=hierarchy();
  const size_t count=source.nodes.size();
  zenkit::ModelScript script;
  const char* names[]={"S_BEFORE_A","S_BEFORE_B","S_WOUNDED"};
  auto append=[](std::vector<std::byte>& bytes, const auto& value) {
    const size_t at=bytes.size(); bytes.resize(at+sizeof(value));
    std::memcpy(bytes.data()+at,&value,sizeof(value));
    };
  auto line=[](std::vector<std::byte>& bytes, const char* text) {
    const size_t size=std::strlen(text);
    const size_t at=bytes.size(); bytes.resize(at+size+1);
    std::memcpy(bytes.data()+at,text,size); bytes.back()=std::byte('\n');
    };
  for(size_t kind=0;kind<3;++kind) {
    std::vector<std::byte> bytes,header,samples;
    append(header,uint16_t(0xc)); line(header,names[kind]);
    append(header,uint32_t(0)); append(header,uint32_t(32)); append(header,uint32_t(count));
    append(header,60.f); append(header,60.f); append(header,-200.f); append(header,.01f);
    for(int i=0;i<6;++i) append(header,0.f);
    line(header,names[kind]);
    append(samples,uint32_t(0));
    for(size_t b=0;b<count;++b) append(samples,uint32_t(b));
    for(size_t frame=0;frame<32;++frame)
      for(size_t b=0;b<count;++b) {
        for(int axis=0;axis<3;++axis) append(samples,uint16_t(32767));
        Tempest::Matrix4x4 local;
        std::memcpy(&local,&source.nodes[b].transform,sizeof(local));
        for(int axis=0;axis<3;++axis) {
          const float offset=kind==1 ? (axis==0 ? 20.f : -12.f) :
                             kind==2 ? (axis==1 ? float(frame)*.2f : -10.f) : 0.f;
          const float value=local.at(3,axis)+offset;
          append(samples,uint16_t(std::lround((value+200.f)*100.f)));
          }
        }
    append(bytes,uint16_t(0xa020)); append(bytes,uint32_t(header.size()));
    bytes.insert(bytes.end(),header.begin(),header.end());
    append(bytes,uint16_t(0xa090)); append(bytes,uint32_t(samples.size()));
    bytes.insert(bytes.end(),samples.begin(),samples.end());
    const std::string file=std::string("POSE_RETURN-")+names[kind]+".MAN";
    std::ofstream output(root/"_work"/file,std::ios::binary);
    output.write(reinterpret_cast<const char*>(bytes.data()),std::streamsize(bytes.size()));
    require(bool(output),"synthetic MAN import input must be written");
    zenkit::MdsAnimation ani{};
    ani.name=names[kind]; ani.next=names[kind]; ani.layer=0;
    ani.direction=zenkit::AnimationDirection::FORWARD;
    ani.first_frame=0; ani.last_frame=31; ani.fps=60;
    script.animations.push_back(std::move(ani));
    }
  require(BaseImport::convertModels(root,base/"models",false)==0,"pose driver models must import into the base");
  require(BaseImport::convertTextures(root,base/"textures",false)==0,"pose driver texture index must import");
  require(BaseImport::convertMeshes(root,base/"meshes",false)==0,"pose driver mesh index must import");
  require(BaseImport::convertZen(root,base/"zen",false)==0,"pose driver ZEN index must import");
  require(BaseImport::convertFonts(root,base/"fonts",false)==0,"pose driver font index must import");
  require(BaseImport::convertPrograms(root,base/"programs",false)==0,"pose driver program index must import");
  require(BaseImport::convertOutputUnits(root,base/"outputunits",false)==0,"pose driver output unit index must import");
  require(BaseImport::convertAudio(root,base/"audio",false)==0,"pose driver audio index must import");
  require(BaseImport::convertMusic(root,base/"music",false)==0,"pose driver music index must import");
  {
    // Synthetic installation without remaining-family inputs: a labelled empty manifest.
    std::filesystem::create_directories(base/"sources");
    std::ofstream output(base/"sources/sources.json",std::ios::binary);
    output<<"{\"format\":\"gothic-sources\",\"version\":1,\"synthetic\":\"pose driver fixture without remaining-family inputs\"}\n";
    require(bool(output),"pose driver source manifest must be written");
  }
  BaseImport::writeBaseMetadata(base,BaseImport::identify(root));
  const auto* oldBase=std::getenv("OPENGOTHIC_BASE");
  const bool hadBase=oldBase!=nullptr;
  const std::string previousBase=hadBase ? oldBase : "";
  setenv("OPENGOTHIC_BASE",base.c_str(),1);
  const auto game=root.string();
  const char* args[]={"gothic-ragdoll-contract","-g",game.c_str()};
  CommandLine commandLine(3,args);
  Resources resources;
  Resources::loadBase();
  auto ownScript=asset::fromZenKit(script);
  Animation animation(ownScript,"POSE_RETURN",false);
  Skeleton skeleton(asset::fromZenKit(source),&animation,"POSE_RETURN.MDS");
  AnimationSolver solver;
  solver.setSkeleton(&skeleton);
  const auto old=solver.solveFrm(names[0]);
  const auto moving=solver.solveFrm(names[1]);
  const auto wounded=solver.solveFrm(names[2]);
  require(old && moving && wounded,"synthetic MAN fixture must resolve real saved animation names");
  for(const auto scale:{Tempest::Vec3(1.125f,1.125f,1.125f),Tempest::Vec3(.9f,1.f,1.f)}) {
    auto object=at(100,-20,70); object.rotateOY(29); object.scale(scale);
    for(const uint64_t saveTick:{uint64_t(1064),uint64_t(1112)}) {
    Pose before;
    before.setSkeleton(&skeleton); before.setObjectMatrix(object,true);
    require(before.startAnim(solver,old,0,BS_STAND,Pose::Force,1000),"initial fixture driver must start");
    before.update(1000,true); before.update(1016,true);
    require(before.startAnim(solver,moving,0,BS_STAND,Pose::Force,1032),"second fixture driver must start");
    before.update(1032,true); before.update(1048,true);
    Pose::Probe probe;
    before.probe(probe);
    require(probe.blend.active>0,"full-pose save test must start with a real ordinary inertial offset");
    std::vector<Tempest::Matrix4x4> physical(before.transform(),before.transform()+before.boneCount());
    physical[1].translate(0,-35,12); physical[1].rotateOX(65);
    require(before.setRagdollPose(&skeleton,physical,{1}),"physical handoff fixture must overlay the pelvis");
    before.update(1064,true);
    const auto snapshot=std::vector<Tempest::Matrix4x4>(before.transform(),before.transform()+before.boneCount());
    before.stopAllAnim();
    require(before.startAnim(solver,wounded,0,BS_UNCONSCIOUS,Pose::Force,1064),"wounded pose must take the real layer");
    before.clearRagdollPose(true,1064);
    if(saveTick>1064) before.update(saveTick,true);
    std::vector<uint8_t> bytes;
    {
      Tempest::MemWriter writer(bytes); Serialize out(writer);
      out.setEntry("visual"); before.save(out);
      out.setEntry("ragdoll-recovery"); before.saveRagdollRecovery(out);
    }
    Pose after;
    after.setSkeleton(&skeleton); after.setObjectMatrix(object,true);
    {
      Tempest::MemReader reader(bytes); Serialize in(reader);
      require(in.setEntry("visual"),"real legacy pose entry must load"); after.load(in,solver);
      // Npc::load updates its object/animated transform before reading the optional entry.
      after.update(saveTick,true);
      require(in.setEntry("ragdoll-recovery"),"optional physical return must load separately");
      after.loadRagdollRecovery(in);
    }
    require(after.isInAnim(wounded),"full Pose load must resolve and retain the saved wounded driver");
    for(uint64_t tick=saveTick;tick<=1608;tick+=16) {
      before.update(tick,true); after.update(tick,true);
      for(size_t b=0;b<count;++b) {
        const auto actual=after.bone(b),expected=before.bone(b);
        for(size_t i=0;i<16;++i)
          near(actual.data()[i],expected.data()[i],.0001f,
               "loaded full-pose driver must continue the physical return without changing its target");
        if(tick==1064)
          near((position(expected)-position(snapshot[b])).length(),0,.001f,
               "full driver handoff must preserve every physical bone on its first frame");
        }
      }
    require(!before.isRagdollBlending() && !after.isRagdollBlending(),"saved full-pose returns must both finish");
    require(before.startAnim(solver,moving,0,BS_STAND,Pose::Force,1610) &&
            after.startAnim(solver,moving,0,BS_STAND,Pose::Force,1610),"post-return animation must start normally");
    before.update(1610,true); after.update(1610,true);
    before.probe(probe);
    require(probe.blend.active>0,"ordinary inertialization must resume for a later animation switch");
    for(size_t b=0;b<count;++b)
      for(size_t i=0;i<16;++i)
        near(after.bone(b).data()[i],before.bone(b).data()[i],.0001f,
             "loaded and uninterrupted poses must agree on the next ordinary transition");
      }
    }
  if(hadBase) setenv("OPENGOTHIC_BASE",previousBase.c_str(),1);
  else unsetenv("OPENGOTHIC_BASE");
  std::filesystem::remove_all(root);
  }

void testPoseAffineRecoveryAndSaves() {
  {
    // Stretch can be the only offset: its clock must survive even when all rigid curves are idle.
    asset::AnimationSample still;
    still.rotation={1,0,0,0}; still.position={0,0,0};
    auto shown=still;
    AnimBlend clock;
    clock.reset(1); clock.transitionFrom(&still,1,1000);
    require(clock.apply(&still,&shown,1,1000) && clock.externalWeight(1000)==1.f,
            "an affine-only handoff must keep the shared initial blend clock");
    require(clock.apply(&still,&shown,1,1016) && clock.externalWeight(1016)>0.f && clock.externalWeight(1016)<1.f,
            "affine stretch must use the standard continuous quintic blend curve");
    require(!clock.apply(&still,&shown,1,1400) && clock.externalWeight(1400)==0.f,
            "an affine-only blend must settle without a rigid offset");
  }
  auto source=hierarchy();
  for(const size_t b:{size_t(1),size_t(5)}) {
    Tempest::Matrix4x4 local;
    std::memcpy(&local,&source.nodes[b].transform,sizeof(local));
    local.rotateOY(b==1 ? 35.f : -27.f);
    std::memcpy(&source.nodes[b].transform,&local,sizeof(local));
    }
  Skeleton skeleton(asset::fromZenKit(source),nullptr,"AFFINE_HUMAN.MDS");
  AnimationSolver solver;
  solver.setSkeleton(&skeleton);
  auto object=at(120,-30,90); object.rotateOY(29); object.scale(.9f,1.f,1.f);
  for(const uint64_t saveTick:{uint64_t(1016),uint64_t(1064)}) {
    Pose before;
    before.setSkeleton(&skeleton); before.setObjectMatrix(object,true); before.update(1000,true);
    const std::vector<Tempest::Matrix4x4> animated(before.transform(),before.transform()+before.boneCount());
    auto physical=animated;
    const std::vector<size_t> mapped={1,5};
    for(const size_t b:mapped) {
      // R(native) * C(initial visual affine), independent of the production backend. This keeps
      // nonuniform object scale and the shear produced when the authored bone rotates under it.
      const auto rotation=IkMath::mul(IkMath::rotationOf(object),IkMath::rotationOf(skeleton.tr[b]));
      const auto nativeInitial=IkMath::compose(rotation,position(animated[b]));
      auto inverse=nativeInitial; inverse.inverse();
      const auto correction=inverse*animated[b];
      const auto initial=nativeInitial*correction;
      for(size_t i=0;i<16;++i)
        near(initial.data()[i],animated[b].data()[i],.0001f,"native frame times affine correction must preserve the initial world bone");
      auto nativeMoved=nativeInitial;
      nativeMoved.translate(0,-35,12);
      nativeMoved.rotateOZ(b==1 ? 65.f : -25.f);
      physical[b]=nativeMoved*correction;
      }
    require(before.setRagdollPose(&skeleton,physical,mapped),"positive affine physical frames must be accepted");
    before.update(1016,true);
    for(const size_t b:mapped)
      for(size_t i=0;i<16;++i)
        near(before.bone(b).data()[i],physical[b].data()[i],.0001f,"affine physical overlay must preserve every matrix component");
    auto inverseAnimatedHead=animated[5]; inverseAnimatedHead.inverse();
    const auto expectedSlot=physical[5]*(inverseAnimatedHead*animated[20]);
    for(size_t i=0;i<16;++i)
      near(before.bone(20).data()[i],expectedSlot.data()[i],.0002f,"unmapped attachment must retain its affine relation to the physical head");
    const std::vector<Tempest::Matrix4x4> shown(before.transform(),before.transform()+before.boneCount());
    before.clearRagdollPose(true,1016);
    if(saveTick>1016) before.update(saveTick,true);
    std::vector<uint8_t> bytes;
    {
      Tempest::MemWriter writer(bytes); Serialize out(writer);
      out.setEntry("visual"); before.save(out);
      out.setEntry("ragdoll-recovery"); before.saveRagdollRecovery(out);
    }
    Pose after;
    after.setSkeleton(&skeleton); after.setObjectMatrix(object,true);
    {
      Tempest::MemReader reader(bytes); Serialize in(reader);
      require(in.setEntry("visual"),"affine legacy pose entry must load"); after.load(in,solver);
      after.update(saveTick,true);
      require(in.setEntry("ragdoll-recovery"),"affine optional return entry must load"); after.loadRagdollRecovery(in);
    }
    for(uint64_t tick=saveTick;tick<=1800;tick+=16) {
      before.update(tick,true); after.update(tick,true);
      for(size_t b=0;b<shown.size();++b) {
        for(size_t i=0;i<16;++i) {
          near(after.bone(b).data()[i],before.bone(b).data()[i],.0001f,
               "pending and mid-return affine archives must preserve the complete pose continuation");
          if(tick==1016)
            near(before.bone(b).data()[i],shown[b].data()[i],.0001f,
                 "first affine return frame must preserve every shown world-matrix component");
          }
        const auto& m=before.bone(b);
        const Tempest::Vec3 x(m.at(0,0),m.at(0,1),m.at(0,2)),y(m.at(1,0),m.at(1,1),m.at(1,2)),z(m.at(2,0),m.at(2,1),m.at(2,2));
        require(IkMath::dot(x,IkMath::cross(y,z))>0.f,"affine recovery must remain finite and properly oriented");
        }
      }
    require(!before.isRagdollBlending() && !after.isRagdollBlending(),"affine recovery must finish within the normal blend duration");
    for(size_t b=0;b<animated.size();++b)
      for(size_t i=0;i<16;++i)
        near(before.bone(b).data()[i],animated[b].data()[i],.0001f,"completed affine recovery must preserve the full ordinary scaled animation");
    auto reflected=physical; reflected[1].scale(-1.f,1.f,1.f);
    require(!before.setRagdollPose(&skeleton,reflected,mapped),"reflected physical frames must be rejected");
    auto projective=physical; projective[1].set(0,3,.1f);
    require(!before.setRagdollPose(&skeleton,projective,mapped),"projective physical frames must be rejected");
    }
  }

void testPhysicalVelocityHandoff() {
  {
    // Compare a moving physical source and moving target against the existing ordinary switch.
    // Neither side's velocity may silently become zero at the handoff.
    asset::AnimationSample previous,current,target,next,shown,ordinary;
    previous.position={19.97f,0,0}; previous.rotation={1,0,0,0};
    current=previous; current.position.x=20.f;
    target=previous; target.position.x=0.f;
    next=target; next.position.x=.006f;
    Animation::Sequence first,second;
    first.name="physical-source-control"; second.name="moving-target-control";
    AnimBlend baseline,physical;
    baseline.reset(1); physical.reset(1);
    ordinary=previous; shown=current;
    baseline.drive(0,first,0,previous,current,1000.f);
    baseline.apply(&previous,&ordinary,1,1000);
    baseline.apply(&current,&ordinary,1,1001);
    baseline.drive(0,second,1001,target,next,1000.f);
    physical.transitionFrom(&current,1,1001,&previous,1000);
    physical.setExternalTarget(&target,&next,1,1000.f);
    for(uint64_t time=1001;time<1500;time+=1) {
      auto moving=target; moving.position.x=float(time-1001)*.006f;
      baseline.apply(&moving,&ordinary,1,time);
      physical.apply(&moving,&shown,1,time);
      near(shown.position.x,ordinary.position.x,.0001f,"physical source and target velocities must match ordinary inertialization");
      }
    for(const bool pending:{true,false}) {
      AnimBlend before,after;
      before.reset(1); before.transitionFrom(&current,1,2001,&previous,2000);
      before.setExternalTarget(&target,&next,1,1000.f);
      auto a=current,b=current;
      if(!pending) before.apply(&target,&a,1,2001);
      std::vector<uint8_t> bytes;
      { Tempest::MemWriter writer(bytes); Serialize out(writer); out.setEntry("velocity"); before.saveExternal(out); }
      { Tempest::MemReader reader(bytes); Serialize in(reader); require(in.setEntry("velocity"),"velocity archive must exist"); after.loadExternal(in,1); }
      after.setExternalTarget(&target,&next,1,1000.f); b=a;
      for(uint64_t time=2001;time<2400;time+=16) {
        auto moving=target; moving.position.x=float(time-2001)*.006f;
        before.apply(&moving,&a,1,time); after.apply(&moving,&b,1,time);
        require(std::memcmp(&a,&b,sizeof(a))==0,"pending and initialized velocity seeds must retain exact saved continuation");
        }
      }
    }
  zenkit::ModelHierarchy source{};
  zenkit::ModelHierarchyNode node;
  node.name="BIP01"; node.parent_index=-1;
  const auto identity=Tempest::Matrix4x4::mkIdentity();
  std::memcpy(&node.transform,&identity,sizeof(identity)); source.nodes.push_back(node);
  source.bbox.min={-1,-1,-1}; source.bbox.max={1,1,1}; source.collision_bbox=source.bbox;
  Skeleton skeleton(asset::fromZenKit(source),nullptr,"VELOCITY.MDS");
  AnimationSolver solver; solver.setSkeleton(&skeleton);
  Animation::Sequence target;
  target.name="moving-root-target"; target.animCls=Animation::Loop;
  target.data=std::make_shared<Animation::AnimData>();
  target.data->numFrames=32; target.data->fpsRate=10.f; target.data->nodeIndex={0};
  for(int i=0;i<32;++i) {
    asset::AnimationSample sample;
    sample.rotation={1,0,0,0}; sample.position={float(i)*10.f,float(i)*.6f,0};
    target.data->samples.push_back(sample);
    }
  Pose pose;
  auto object=at(100,0,50); object.rotateOY(29); object.scale(.9f,1.f,1.f);
  pose.setSkeleton(&skeleton); pose.setObjectMatrix(object,true);
  require(pose.startAnim(solver,&target,0,BS_UNCONSCIOUS,Pose::Force,1000),"world-velocity fixture must have a moving animation target");
  auto world=object; world.translate(0,20,0);
  require(pose.setRagdollPose(&skeleton,{world},{0}),"world-velocity first physical sample must set"); pose.update(1000,true);
  world.set(3,1,20.03f);
  require(pose.setRagdollPose(&skeleton,{world},{0}),"world-velocity second physical sample must set"); pose.update(1001,true);
  const auto first=position(pose.bone(0));
  object.set(3,1,30.f); // Rebase from the body's world frame to the get-up frame.
  pose.setObjectMatrix(object,true);
  pose.clearRagdollPose(true,1010);
  pose.update(1010,true);
  near((position(pose.bone(0))-first).length(),0,.0001f,"velocity seeding must preserve the first physical world pose after rebasing");
  const auto atStart=position(pose.bone(0));
  pose.update(1011,true);
  const auto velocity=(position(pose.bone(0))-atStart)*1000.f;
  near(velocity.y,30.f,.15f,"recovery must preserve the measured previous world velocity");
  near(velocity.x,0.f,.15f,"cancelled MAN root motion must not inject horizontal world velocity");
  near(velocity.z,0.f,.15f,"rebasing and scaled yaw must not inject horizontal world velocity");
  }

std::vector<float> replayProducerState(Pose& pose) {
  std::vector<float> result;
  FootIk::Probe foot; pose.footIk().probe(foot,{});
  result.insert(result.end(),{foot.valid ? 1.f : 0.f,foot.weight,foot.pelvis,foot.chest,foot.target,foot.crouch,foot.floorY});
  for(const auto& leg:foot.leg) {
    result.insert(result.end(),{leg.smoothY,leg.plant,leg.slack,leg.kneeDev,leg.tilt,leg.roll,leg.yaw});
    for(size_t i=0;i<4;++i) {
      result.insert(result.end(),{leg.anim[i].x,leg.anim[i].y,leg.anim[i].z,leg.final[i].x,leg.final[i].y,leg.final[i].z});
      }
    }
  Gaze::Probe gaze; pose.gazeIk().probe(gaze);
  result.insert(result.end(),{gaze.valid ? 1.f : 0.f,gaze.weight,gaze.yaw,gaze.pitch,gaze.animYaw,gaze.animPitch,
                             gaze.headYaw,gaze.chestYaw,gaze.waistYaw,gaze.error,gaze.head.x,gaze.head.y,gaze.head.z,
                             gaze.facing.x,gaze.facing.y,gaze.facing.z,float(pose.gazeIk().shiftCount())});
  Lean::Probe lean; pose.leanIk().probe(lean);
  result.insert(result.end(),{lean.valid ? 1.f : 0.f,lean.roll,lean.pitch,lean.speed,lean.turn,lean.slope});
  Aim::Probe aim; pose.aimIk().probe(aim);
  result.insert(result.end(),{aim.valid ? 1.f : 0.f,aim.weight,aim.yaw,aim.pitch,aim.animYaw,aim.animPitch,
                             aim.turn,aim.bend,aim.lean,aim.side,aim.error});
  AimWalk::Probe walk; pose.aimWalk().probe(walk);
  result.insert(result.end(),{walk.valid ? 1.f : 0.f,walk.weight,walk.speed,walk.phase,walk.twist,walk.chest,walk.drop});
  BowDraw::Probe bow; pose.bowDrawIk().probe(bow);
  result.insert(result.end(),{bow.valid ? 1.f : 0.f,bow.weight,bow.draw,bow.span,bow.travel,bow.miss,bow.follow,
                             bow.fit,bow.offset,bow.angle,bow.aligned,bow.holding ? 1.f : 0.f,bow.nock,
                             bow.handAt.x,bow.handAt.y,bow.handAt.z});
  Flinch::Probe flinch; pose.flinchIk().probe(flinch);
  result.insert(result.end(),{flinch.valid ? 1.f : 0.f,flinch.bend,flinch.bendYaw,flinch.twist,flinch.head,
                             float(flinch.hits),flinch.strength,flinch.height});
  return result;
  }

void testOriginReplayCopy(Pose& pose) {
  const bool probing=FootIk::isProbing(); FootIk::setProbing(true);
  const auto before=std::vector<Tempest::Matrix4x4>(pose.transform(),pose.transform()+pose.boneCount());
  const auto producers=replayProducerState(pose);
  const auto expected=PoseOriginReplayContract::legacy(pose);
  const auto actual=PoseOriginReplayContract::prepared(pose);
  require(actual.size()==expected.size() && actual.size()==before.size(),"origin replay must preserve full skeleton indexing");
  for(size_t b=0;b<actual.size();++b)
    for(size_t i=0;i<16;++i) {
      require(std::isfinite(actual[b].data()[i]) && std::isfinite(expected[b].data()[i]),"origin replay must retain finite complete frames");
      exact(actual[b].data()[i],expected[b].data()[i],"replay-only scratch must match the preserved full-copy origin replay bit for bit");
      exact(pose.bone(b).data()[i],before[b].data()[i],"origin scratch must not mutate the parent's shown world pose");
      }
  const auto after=replayProducerState(pose);
  require(after.size()==producers.size(),"origin scratch must retain all producer probes");
  for(size_t i=0;i<after.size();++i)
    exact(after[i],producers[i],"origin scratch must not mutate the parent's retained procedural state");
  FootIk::setProbing(probing);
  }

void testOriginReplayHierarchySeed() {
  for(const bool unordered:{false,true}) {
    zenkit::ModelHierarchy source{};
    const int16_t parentsOrdered[]={-1,0,1,2,1,4};
    const int16_t parentsUnordered[]={1,-1,0,2,1,4};
    for(int16_t i=0;i<6;++i) {
      zenkit::ModelHierarchyNode node; node.name="REPLAY_NODE_"+std::to_string(i);
      node.parent_index=unordered ? parentsUnordered[i] : parentsOrdered[i];
      auto local=at(float(i)*3.f,float(i)*5.f,float(i)*2.f); local.rotateOY(float(i)*7.f);
      std::memcpy(&node.transform,&local,sizeof(local)); source.nodes.push_back(node);
      }
    source.bbox.min={-30,0,-30}; source.bbox.max={30,80,30}; source.collision_bbox=source.bbox;
    Skeleton skeleton(asset::fromZenKit(source),nullptr,unordered ? "REPLAY_UNORDERED.MDS" : "REPLAY_FULL_SEED.MDS");
    require(skeleton.ordered!=unordered,"origin fixture must exercise the selected hierarchy branch");
    Pose pose; pose.setSkeleton(&skeleton);
    auto object=at(23446.3984375f,1122.7470703125f,-10364.30859375f); object.rotateOY(74); object.scale(.9f,1.f,1.f);
    pose.setFlags(Pose::NoTranslation); pose.setObjectMatrix(object,true);
    if(!unordered) {
      // The generic ordered pass intentionally permits a parent in the fixed
      // bank beyond numBones. Preserve its previously retained affine seed.
      skeleton.nodes[2].parent=Resources::MAX_NUM_SKELETAL_NODES-1;
      auto seed=at(17,-8,11); seed.rotateOY(23); seed.scale(1.05f,1.f,.95f);
      PoseOriginReplayContract::seed(pose,Resources::MAX_NUM_SKELETAL_NODES-1,seed);
      }
    std::vector<Tempest::Matrix4x4> physical;
    for(const auto& bone:skeleton.tr) physical.push_back(object*bone);
    require(pose.setRagdollPose(&skeleton,physical,{1,2,4}),"origin hierarchy fixture must accept a mixed physical mask");
    pose.update(1000,true); testOriginReplayCopy(pose);
    pose.update(1000,false); testOriginReplayCopy(pose);
    pose.update(1017,true); testOriginReplayCopy(pose);
    }
  }

void testPoseFarWorldAttachments() {
  // Independent column-wise Gaussian solve of P * local = child; no homogeneous inverse.
  auto relative=[](const Tempest::Matrix4x4& parent, const Tempest::Matrix4x4& child) {
    auto result=Tempest::Matrix4x4::mkIdentity();
    for(int column=0;column<4;++column) {
      double rows[3][4] = {};
      for(int r=0;r<3;++r) {
        for(int c=0;c<3;++c) rows[r][c]=parent.at(c,r);
        rows[r][3]=column<3 ? double(child.at(column,r)) : double(child.at(3,r))-parent.at(3,r);
        }
      for(int pivot=0;pivot<3;++pivot) {
        int best=pivot;
        for(int r=pivot+1;r<3;++r) if(std::abs(rows[r][pivot])>std::abs(rows[best][pivot])) best=r;
        if(best!=pivot) for(int c=0;c<4;++c) std::swap(rows[pivot][c],rows[best][c]);
        const double scale=rows[pivot][pivot];
        for(int c=pivot;c<4;++c) rows[pivot][c]/=scale;
        for(int r=0;r<3;++r) if(r!=pivot) {
          const double coefficient=rows[r][pivot];
          for(int c=pivot;c<4;++c) rows[r][c]-=coefficient*rows[pivot][c];
          }
        }
      for(int r=0;r<3;++r) result.set(column,r,float(rows[r][3]));
      }
    return result;
    };
  auto source=hierarchy();
  auto add=[&](const char* name, int16_t parent, Tempest::Matrix4x4 local) {
    zenkit::ModelHierarchyNode node; node.name=name; node.parent_index=parent;
    std::memcpy(&node.transform,&local,sizeof(local)); source.nodes.push_back(node);
    return source.nodes.size()-1;
    };
  const asset::AnimationSample actualRoot{{67.9932175f,11.873786f,90.8665161f},
                                         {.694189906f,-.213284492f,.381034523f,.572208703f}};
  const auto rootFrame=mkMatrix(actualRoot);
  std::memcpy(&source.nodes[0].transform,&rootFrame,sizeof(rootFrame));
  const auto spine2=add("BIP01 SPINE2",3,at(0,8,0));
  for(const size_t bone:{size_t(4),size_t(6),size_t(9)}) source.nodes[bone].parent_index=int16_t(spine2);
  auto arrow=at(0,2,0); arrow.rotateOY(180);
  const auto rightSlot=add("ZS_RIGHTHAND",11,arrow);
  const auto leftSlot=add("ZS_LEFTHAND",8,at(0,1,0));
  const auto unkeyed=add("UNKEYED_FOOT_ATTACHMENT",14,at(3,7,5));
  Skeleton skeleton(asset::fromZenKit(source),nullptr,"FAR_WORLD.MDS");
  AnimationSolver solver; solver.setSkeleton(&skeleton);
  Animation::Sequence stance,steps;
  for(auto* sequence:{&stance,&steps}) {
    sequence->name=sequence==&stance ? "keyed-stance" : "moving-steps";
    sequence->animCls=Animation::Loop; sequence->data=std::make_shared<Animation::AnimData>();
    auto& data=*sequence->data; data.numFrames=32; data.fpsRate=10;
    for(size_t b=0;b<skeleton.nodes.size();++b) if(b!=unkeyed) data.nodeIndex.push_back(uint32_t(b));
    for(size_t frame=0;frame<data.numFrames;++frame)
      for(auto b:data.nodeIndex) {
        const auto local=skeleton.nodes[b].tr;
        asset::AnimationSample sample;
        const auto q=IkMath::toQuat(IkMath::transpose(IkMath::rotationOf(local)));
        sample.rotation={q.w,q.x,q.y,q.z};
        sample.position={local.at(3,0),local.at(3,1),local.at(3,2)};
        if(sequence==&stance && b==20) { sample.position.x=5.7f; sample.position.y=13.4f; sample.position.z=-2.8f; }
        if(sequence==&steps) {
          if(b==0) sample.position.z+=float(frame)*3.f;
          if(b==1) sample.position.y+=float(frame%8)*.8f;
          }
        data.samples.push_back(sample);
        }
    data.setupMoveTr();
    }
  auto basis=Tempest::Matrix4x4::mkIdentity(); basis.rotateOY(74); basis.scale(.9f,1,1);
  auto object=basis; object.set(3,0,24023.61328125f); object.set(3,1,1122.7470703125f); object.set(3,2,-10364.30859375f);
  {
    // The actual v5 saved attachment's homogeneous W is one float above one. This seemingly tiny
    // local error scales the whole parent translation, rather than only the attachment offset.
    auto affine=at(8.1712675f,4.0605712f,2.5401063f);
    auto inverseResidue=affine;
    inverseResidue.set(3,3,std::nextafter(1.f,2.f));
    const auto exactWorld=object*affine, damagedWorld=object*inverseResidue;
    const auto delta=Tempest::Vec3(damagedWorld.at(3,0)-exactWorld.at(3,0),
                                 damagedWorld.at(3,1)-exactWorld.at(3,1),
                                 damagedWorld.at(3,2)-exactWorld.at(3,2));
    require(delta.length()>.001f,
            "the actual nonunit inverse W must reproduce the far-world attachment error");
  }
  std::vector<size_t> mapped;
  for(size_t b=0;b<skeleton.nodes.size();++b)
    if(b!=20 && b!=rightSlot && b!=leftSlot && b!=unkeyed) mapped.push_back(b);
  std::vector<Tempest::Matrix4x4> physical;
  auto native=at(24020,1200,-10360); native.rotateOX(49); native.rotateOY(-33); native.scale(.9f,1,1);
  for(const auto& bone:skeleton.tr) physical.push_back(native*bone);
  const size_t slots[]={20,rightSlot,leftSlot,unkeyed};
  for(const int producer:{0,1,2,3,4,5,6,7,8,9}) {
    Pose far,reference;
    far.setSkeleton(&skeleton); reference.setSkeleton(&skeleton);
    far.setObjectMatrix(object,true); reference.setObjectMatrix(basis,true);
    if(producer==9) {
      far.setFlags(Pose::NoTranslation); reference.setFlags(Pose::NoTranslation);
      far.setHeadRotation(11,-17); reference.setHeadRotation(11,-17);
      }
    require(far.startAnim(solver,&stance,0,BS_UNCONSCIOUS,Pose::Force,1000) &&
            reference.startAnim(solver,&stance,0,BS_UNCONSCIOUS,Pose::Force,1000),"far-world keyed stance must start");
    far.update(1000,true); reference.update(1000,true);
    auto configure=[&](Pose& pose, float floorY) {
      if(producer==1 || producer==9) {
        FootIk::Input input; input.enable=true; input.dt=200; input.floorY=floorY;
        for(size_t i=0;i<2;++i) { input.ground[i].hasCol=true; input.ground[i].y=floorY+(i==0 ? 10.f : -6.f); input.ground[i].n={.1f,.99f,.02f}; }
        pose.updateFootIk(input);
        }
      if(producer==2) {
        require(pose.aimWalk().isValid(),"far-world fixture must have an actual AimWalk hierarchy");
        AimWalk::Input input; input.legs=&steps; input.walk=1; input.dt=200; input.active=true; pose.updateAimWalk(input);
        require(pose.aimWalk().isActive(),"far-world fixture must exercise active AimWalk");
        }
      if(producer==3 || producer==4) {
        require(pose.bowDrawIk().isValid(),"far-world fixture must have an actual bow draw chain");
        BowDraw::Input input; input.active=true; input.ready=true; input.draw=.55f; input.span=40; input.arrow=true;
        input.hasString=true; input.stringRest={0,0,0}; input.stringPull={-40,0,0}; input.stringHigh={0,2,0}; input.bowAxis={0,1,0}; input.dt=200;
        pose.updateBowDraw(input);
        }
      if(producer==5) {
        require(pose.gazeIk().isValid(),"origin replay fixture must have a gaze hierarchy");
        Gaze::Input input; input.active=true; input.source=Gaze::Source::Attention; input.forward={0,0,1}; input.dt=200;
        input.target=position(pose.bone(5))+Tempest::Vec3(80,35,120); input.talk={.08f,.1f,-.04f}; pose.updateGaze(input);
        require(pose.gazeIk().isActive(),"origin replay fixture must retain active gaze");
        }
      if(producer==6) {
        require(pose.leanIk().isValid(),"origin replay fixture must have a lean hierarchy");
        Lean::Input input; input.active=true; input.position={0,floorY,0}; input.forward={0,0,1}; input.dt=200; pose.updateLean(input);
        input.position={40,floorY+15,80}; input.forward={.2f,0,.98f}; pose.updateLean(input);
        Lean::Probe probe; pose.leanIk().probe(probe);
        require(probe.active && std::abs(probe.pitch)+std::abs(probe.roll)>0,"origin replay fixture must retain a measured lean");
        }
      if(producer==7) {
        require(pose.flinchIk().isValid(),"origin replay fixture must have a flinch hierarchy");
        Flinch::Hit hit; hit.dir={.4f,0,-1}; hit.height=.9f; hit.twist=.4f; hit.strength=.8f; pose.flinchIk().hit(hit,{0,0,1});
        Flinch::Input input; input.forward={0,0,1}; input.dt=32; pose.updateFlinch(input);
        Flinch::Probe probe; pose.flinchIk().probe(probe);
        require(probe.active && probe.hits==1,"origin replay fixture must retain a real flinch kick");
        }
      if(producer==8) {
        require(pose.aimIk().isValid(),"origin replay fixture must have an aim hierarchy");
        Aim::Input input; input.active=true; input.arrow=true; input.manual=true; input.slot=rightSlot;
        input.dir={.3f,.15f,1}; input.forward={0,0,1}; input.lean=.2f; input.side=.1f;
        input.sway={.02f,-.01f}; input.tremor={-.01f,.015f}; input.dt=200;
        // Choosing the arrow slot resets its measurement. updateAim performs the
        // real mkSkeleton/apply pass; the next four steady measured frames make
        // the bow ready. A single large dt cannot replace that frame counter.
        for(int frame=0;frame<5;++frame)
          require(pose.updateAim(input),"manual aim initialization must rebuild each measured world pose");
        Aim::Probe probe; pose.aimIk().probe(probe);
        require(probe.active && probe.weight>0,"origin replay fixture must retain active manual aim");
        require(probe.steady && probe.error>=0,"origin replay fixture must retain a measured and applied arrow aim");
        }
      };
    configure(far,object.at(3,1)); configure(reference,0);
    far.update(1016,true); reference.update(1016,true);
    if(producer==4) {
      BowDraw::Input input; input.active=true; input.ready=false; input.reload=.2f; input.reloadTime=1.f; input.dt=16;
      far.updateBowDraw(input); reference.updateBowDraw(input);
      far.update(1032,true); reference.update(1032,true);
      BowDraw::Probe probe; reference.bowDrawIk().probe(probe);
      require(probe.holding,"far-world fixture must exercise a genuinely held reload pose");
      // heldRel was captured by the real far-world producer. Preserve that exact state in the
      // independent ordinary reference, rather than replacing its earlier capture with another one.
      reference=far;
      reference.setObjectMatrix(basis,true);
      for(size_t f=0;f<stance.data->numFrames;++f)
        stance.data->samples[f*stance.data->nodeIndex.size()+20].position.z+=30.f;
      far.update(1048,true); reference.update(1048,true);
      auto held=reference.bone(skeleton.nodes[20].parent); held.inverse();
      require(std::abs((held*reference.bone(20)).at(3,2)-stance.data->samples[20].position.z)>20.f,
              "held body must retain its captured local frame despite a different current keyed target");
      }
    if(producer==3) {
      BowDraw::Probe probe; reference.bowDrawIk().probe(probe);
      require(probe.aligned>0 && probe.bowMove>0,"far-world fixture must exercise an independently moved bow hand");
      }
    if(producer==0) {
      std::vector<Tempest::Matrix4x4> half;
      for(size_t b=0;b<skeleton.nodes.size();++b) half.push_back(IkMath::blend(far.bone(b),physical[b],.5f));
      require(far.setRagdollPose(&skeleton,physical,mapped,.5f),"far-world partial physical overlay must set"); far.update(1040,true);
      for(auto b:mapped)
        for(size_t i=0;i<16;++i)
          near(far.bone(b).data()[i],half[b].data()[i],.0001f,"partial physical weight must blend two world frames without moving the NPC toward the origin");
      }
    require(far.setRagdollPose(&skeleton,physical,mapped),"far-world physical overlay must set"); far.update(1048,true);
    testOriginReplayCopy(far);
    Pose repeated=far; repeated.update(1048,false); testOriginReplayCopy(repeated);
    if(producer>=5) continue; // New retained-state variants use the exact legacy replay oracle above.
    for(auto b:slots) {
      const auto expected=physical[skeleton.nodes[b].parent]*relative(reference.bone(skeleton.nodes[b].parent),reference.bone(b));
      require(far.bone(b).at(0,3)==0 && far.bone(b).at(1,3)==0 && far.bone(b).at(2,3)==0 && far.bone(b).at(3,3)==1,
              "a recomposed physical attachment must retain its exact affine homogeneous row");
      for(size_t i=0;i<16;++i) {
        if(std::abs(far.bone(b).data()[i]-expected.data()[i])>(i>=12 && i<=14 ? .001f : .00001f))
          std::fprintf(stderr,"far attachment diagnostic producer=%d bone=%zu component=%zu\n",producer,b,i);
        near(far.bone(b).data()[i],expected.data()[i],i>=12 && i<=14 ? .001f : .00001f,
             "far-world attachment must preserve the actual finished keyed/unkeyed procedural local frame");
        }
      }
    if(producer!=0) continue;
    const auto before=std::vector<Tempest::Matrix4x4>(far.transform(),far.transform()+far.boneCount());
    // A disabled corpse uses another object origin/alignment before the exact native state returns.
    auto fallback=object; fallback.set(3,1,999.f); fallback.rotateOZ(5.4f);
    far.setObjectMatrix(fallback,true); far.update(1064,true);
    far.setObjectMatrix(object,true); far.update(1080,true);
    std::vector<uint8_t> bytes;
    { Tempest::MemWriter writer(bytes); Serialize out(writer); out.setEntry("visual"); far.save(out); }
    Pose cold; cold.setSkeleton(&skeleton); cold.setObjectMatrix(fallback,true);
    { Tempest::MemReader reader(bytes); Serialize in(reader); require(in.setEntry("visual"),"far-world visual samples must archive"); cold.load(in,solver); }
    // No resolved layer is required for this static payload: Pose still owns its keyed base/raw samples.
    cold.update(1080,true); cold.setObjectMatrix(object,true);
    require(cold.setRagdollPose(&skeleton,physical,mapped),"cold far-world native overlay must restore"); cold.update(1080,true);
    for(size_t b=0;b<before.size();++b)
      for(size_t i=0;i<16;++i) {
        near(far.bone(b).data()[i],before[b].data()[i],i>=12 && i<=14 ? .001f : .00001f,"far-world object rebasing must retain every physical and attachment frame");
        near(cold.bone(b).data()[i],before[b].data()[i],i>=12 && i<=14 ? .001f : .00001f,"cold archived samples must reconstruct every far-world attachment frame");
        }
    }
  }

void testFiniteRecoveryHierarchy() {
  zenkit::ModelHierarchy source{};
  const auto identity=Tempest::Matrix4x4::mkIdentity();
  constexpr uint32_t count=8;
  for(uint32_t b=0;b<count;++b) {
    zenkit::ModelHierarchyNode node;
    node.name="finite-local-"+std::to_string(b); node.parent_index=int16_t(b)-1;
    std::memcpy(&node.transform,&identity,sizeof(identity)); source.nodes.push_back(node);
    }
  source.bbox.min={-1,-1,-1}; source.bbox.max={1,1,1}; source.collision_bbox=source.bbox;
  Skeleton skeleton(asset::fromZenKit(source),nullptr,"FINITE_LOCALS.MDS");
  Pose pose; pose.setSkeleton(&skeleton); pose.setObjectMatrix(identity,true); pose.update(1000,true);
  const auto ordinary=std::vector<Tempest::Matrix4x4>(pose.transform(),pose.transform()+pose.boneCount());
  const AnimBlend::Sample unit{{0,0,0},{1,0,0,0}}, unused{{0,0,0},{0,0,0,0}};
  auto stretch=identity; stretch.scale(1.e6f,1.e6f,1.e6f);
  std::vector<uint8_t> bytes;
  {
    Tempest::MemWriter writer(bytes); Serialize out(writer); out.setEntry("recovery");
    out.write(true,uint16_t(3),skeleton.name(),count);
    for(uint32_t b=0;b<count;++b) out.write(unit);
    for(uint32_t b=0;b<count;++b) out.write(stretch);
    out.write(count,uint64_t(1000),uint64_t(1000),uint64_t(1000),true);
    auto zero=[&] { for(int i=0;i<7;++i) out.write(0.f); };
    zero();
    for(uint32_t b=0;b<count;++b) {
      out.write(false,uint64_t(0),0.f,false,unused,0.f);
      for(int i=0;i<4;++i) zero();
      out.write(Tempest::Vec3(),Tempest::Vec3(),Tempest::Vec3(),Tempest::Vec3(),unused);
      }
    out.write(count); for(uint32_t b=0;b<count;++b) out.write(unit);
    out.write(uint64_t(0),uint32_t(0));
  }
  {
    Tempest::MemReader reader(bytes); Serialize in(reader);
    require(in.setEntry("recovery"),"finite affine local hierarchy archive must exist"); pose.loadRagdollRecovery(in);
  }
  require(pose.isRagdollBlending(),"finite affine locals are valid individually before hierarchy composition");
  pose.update(1000,true);
  require(!pose.isRagdollBlending(),"an overflowing affine hierarchy must end recovery before rendering");
  for(size_t b=0;b<ordinary.size();++b)
    for(size_t i=0;i<16;++i)
      near(pose.bone(b).data()[i],ordinary[b].data()[i],0,
           "an overflowing recovery must restore the complete ordinary world pose transactionally");
  }

void testBenchmarkSleepPolicy() {
  Fixture fixture;
  CollisionWorld world;
  auto ground=floor(world);
  auto body=fixture.create(world,at(0,30,0));
  const auto count=world.bodyCount();
  for(int frame=0;frame<1800 && !body->isSleeping();++frame) world.tick(17);
  require(body->isSleeping(),"ordinary benchmark fixture must naturally sleep with default policy");
  body->setAllowSleeping(false);
  for(int frame=0;frame<600;++frame) {
    world.tick(17);
    require(world.bodyCount()==count,"disabling benchmark sleep must retain all native parts");
    const auto state=body->state();
    require(!body->isSleeping() && std::none_of(state.parts.begin(),state.parts.end(),[](const auto& p) { return p.sleeping; }),
            "benchmark allow=false must keep every native part actually active beyond the ordinary sleep window");
    for(const auto& part:state.parts)
      require(position(part.transform).length()<1000,"sleep policy must not launch or explode an otherwise settled body");
    }
  body->setAllowSleeping(true);
  for(int frame=0;frame<1800 && !body->isSleeping();++frame) world.tick(17);
  const auto sleeping=body->state();
  require(body->isSleeping() && std::all_of(sleeping.parts.begin(),sleeping.parts.end(),[](const auto& p) { return p.sleeping; }),
          "reenabling sleep must restore natural sleeping of all native parts");
  require(world.bodyCount()==count,"reenabling benchmark sleep must retain body lifetime and collision coverage");
  std::unique_ptr<Ragdoll> detached;
  { CollisionWorld temporary; detached=fixture.create(temporary,at(0,30,0)); }
  detached->setAllowSleeping(false); detached->setAllowSleeping(true);
  detached.reset();
  }

bool benchmarkTwentyBodies(const char* output, bool preflight) {
  Fixture fixture;
  struct Counts {
    size_t ragdolls=0, awake=0, sleeping=0, invalid=0;
    size_t rigParts=0, nativeParts=0, worldBodies=0;
    };
  struct Frame { Counts before, afterImpulse, afterTick; unsigned failureMask=0; };
  struct Result {
    const char* mode;
    size_t parts=0, staticBodies=0;
    int preparationFrames=0;
    Counts initial, prepared;
    std::vector<Frame> frames;
    std::vector<float> times;
    bool preparationValid=true, coverageValid=true, timingValid=true;
    };
  constexpr int frames=1020, warmup=120, signBlockFrames=5;
  std::vector<Result> results;
  for(const auto mode:{"disabled","active","sleeping"}) {
    CollisionWorld world;
    auto ground=floor(world);
    Result result; result.mode=mode; result.staticBodies=world.bodyCount();
    std::vector<std::unique_ptr<Ragdoll>> bodies;
    if(std::string_view(mode)!="disabled")
      for(int i=0;i<20;++i) {
        const auto object=at(float(i%5)*250.f-500.f,30,float(i/5)*250.f-375.f);
        std::vector<Tempest::Matrix4x4> pose;
        for(const auto& matrix:fixture.skeleton.tr) pose.push_back(object*matrix);
        // Retain failed creations in the counts instead of exiting before the raw output exists.
        bodies.push_back(world.createRagdoll(fixture.skeleton,fixture.zones,pose.data(),pose.size(),object,{}));
        }
    const size_t expectedRagdolls=std::string_view(mode)=="disabled" ? 0 : 20;
    const auto counts=[&] {
      Counts value; value.worldBodies=world.bodyCount();
      value.nativeParts=value.worldBodies>=result.staticBodies ? value.worldBodies-result.staticBodies : 0;
      for(const auto& body:bodies) {
        if(body==nullptr || !body->valid()) { ++value.invalid; continue; }
        ++value.ragdolls; value.rigParts+=body->partCount();
        if(body->isSleeping()) ++value.sleeping; else ++value.awake;
        }
      return value;
      };
    result.initial=counts(); result.parts=result.initial.rigParts;
    if(std::string_view(mode)=="active")
      for(const auto& body:bodies)
        if(body!=nullptr && body->valid()) body->setAllowSleeping(false);
    if(std::string_view(mode)=="sleeping") {
      for(;result.preparationFrames<1800 && counts().sleeping!=expectedRagdolls;++result.preparationFrames)
        world.tick(17);
      result.preparationValid=counts().sleeping==expectedRagdolls;
      }
    result.prepared=counts();
    const auto partsValid=[&](const Counts& value) {
      return result.staticBodies==1 && value.invalid==0 && value.ragdolls==expectedRagdolls &&
             value.awake+value.sleeping==expectedRagdolls && value.rigParts==result.parts &&
             value.nativeParts==result.parts && value.worldBodies==result.staticBodies+result.parts;
      };
    const auto activityValid=[&](const Counts& value) {
      if(std::string_view(mode)=="active") return value.awake==20 && value.sleeping==0;
      if(std::string_view(mode)=="sleeping") return value.sleeping==20 && value.awake==0;
      return value.awake==0 && value.sleeping==0;
      };
    result.coverageValid=result.preparationValid && partsValid(result.initial) && partsValid(result.prepared);
    result.frames.reserve(frames);
    if(!preflight) result.times.reserve(frames-warmup);
    for(int frame=0;frame<frames;++frame) {
      Frame value; value.before=counts();
      if(std::string_view(mode)=="active")
        for(size_t i=0;i<bodies.size();++i) {
          if(bodies[i]==nullptr || !bodies[i]->valid()) continue;
          // Fixed, preregistered input with conservative per-rig sleep disabled.
          // Fast bounded motion can still satisfy Jolt's spatial sleep test;
          // these samples measure twenty continuously active rigs, independently
          // of the normal game's unchanged sleeping policy.
          const float sign=((frame+2)/signBlockFrames+int(i))%2==0 ? 1.f : -1.f;
          bodies[i]->addImpulse({50.f*sign,5.f,25.f*sign});
          }
      value.afterImpulse=counts();
      world.tick(17);
      value.afterTick=counts();
      if(!partsValid(value.before)) value.failureMask|=1;
      if(!partsValid(value.afterImpulse)) value.failureMask|=2;
      if(!partsValid(value.afterTick)) value.failureMask|=4;
      if(!activityValid(value.before)) value.failureMask|=8;
      if(!activityValid(value.afterImpulse)) value.failureMask|=16;
      if(!activityValid(value.afterTick)) value.failureMask|=32;
      result.coverageValid=result.coverageValid && value.failureMask==0;
      result.frames.push_back(value);
      // The counts-only preflight neither reads nor emits any timing samples.
      if(!preflight && frame>=warmup) {
        const float time=world.stepMilliseconds(); result.times.push_back(time);
        result.timingValid=result.timingValid && std::isfinite(time) && time>=0;
        }
      }
    if(!preflight) result.timingValid=result.timingValid && result.times.size()==size_t(frames-warmup);
    results.push_back(std::move(result));
    }
  const bool valid=std::all_of(results.begin(),results.end(),[](const Result& result) {
    return result.coverageValid && result.timingValid;
    });
  std::FILE* file=std::fopen(output,"w");
  if(file==nullptr) { std::fprintf(stderr,"FAIL: twenty-body output is not writable: %s\n",output); return false; }
  std::fprintf(file,"{\"schema_version\":2,\"kind\":\"%s\",\"status\":\"%s\",\"coverage_valid\":%s,"
                    "\"scope\":\"%s\",\"dt_ms\":17,\"warmup_frames\":120,\"coverage_frames\":1020,\"body_count\":20,"
                    "\"active_policy\":\"conservative per-ragdoll allowSleeping=false; normal game policy unchanged\","
                    "\"active_input\":{\"impulse_ns\":[50,5,25],\"sign_block_frames\":%d,"
                    "\"schedule\":\"each frame; sign=(((frame+2)/5+actor_index)%%2==0?+1:-1); X and Z share sign; Y always positive\"},"
                    "\"coverage_failure_mask\":{\"1\":\"before input parts\",\"2\":\"after input parts\",\"4\":\"after tick parts\","
                    "\"8\":\"before input activity\",\"16\":\"after input activity\",\"32\":\"after tick activity\"}",
                 preflight ? "counts_only_preflight" : "native_cpu_benchmark",valid ? "PASS" : "FAIL",
                 std::all_of(results.begin(),results.end(),[](const Result& r) { return r.coverageValid; }) ? "true" : "false",
                 preflight ? "actual ragdoll activity and native part counts before input, after input and after each tick" :
                             "CollisionWorld::tick wall time; no game, rendering or query work",signBlockFrames);
  if(!preflight) std::fprintf(file,",\"measurement_frames\":900");
  std::fprintf(file,",\"cases\":[");
  const auto writeCounts=[&](const Counts& value) {
    std::fprintf(file,"{\"ragdolls\":%zu,\"awake\":%zu,\"sleeping\":%zu,\"invalid\":%zu,"
                      "\"rig_parts\":%zu,\"native_parts\":%zu,\"world_bodies\":%zu}",
                 value.ragdolls,value.awake,value.sleeping,value.invalid,value.rigParts,value.nativeParts,value.worldBodies);
    };
  for(size_t i=0;i<results.size();++i) {
    const auto& result=results[i];
    if(i>0) std::fprintf(file,",");
    std::fprintf(file,"{\"mode\":\"%s\",\"allow_sleeping\":%s,\"physical_parts\":%zu,\"static_bodies\":%zu,\"coverage_valid\":%s,"
                      "\"failed_frames\":%zu,\"preparation_frames\":%d,\"preparation_valid\":%s,\"initial\":",
                 result.mode,std::string_view(result.mode)=="active" ? "false" : "true",
                 result.parts,result.staticBodies,result.coverageValid ? "true" : "false",
                 size_t(std::count_if(result.frames.begin(),result.frames.end(),[](const Frame& f) { return f.failureMask!=0; })),
                 result.preparationFrames,result.preparationValid ? "true" : "false");
    writeCounts(result.initial); std::fprintf(file,",\"prepared\":"); writeCounts(result.prepared);
    std::fprintf(file,",\"counts\":[");
    for(size_t f=0;f<result.frames.size();++f) {
      const auto& value=result.frames[f];
      std::fprintf(file,"%s{\"frame\":%zu,\"failure_mask\":%u,\"before_input\":",f==0 ? "" : ",",f,value.failureMask);
      writeCounts(value.before); std::fprintf(file,",\"after_input\":"); writeCounts(value.afterImpulse);
      std::fprintf(file,",\"after_tick\":"); writeCounts(value.afterTick); std::fprintf(file,"}");
      }
    std::fprintf(file,"]");
    if(!preflight) {
      std::fprintf(file,",\"timing_valid\":%s",result.timingValid ? "true" : "false");
      if(result.timingValid) {
        auto sorted=result.times; std::sort(sorted.begin(),sorted.end());
        std::fprintf(file,",\"p50_ms\":%.9g,\"p95_ms\":%.9g",double(sorted[449]),double(sorted[854]));
        } else {
        std::fprintf(file,",\"p50_ms\":null,\"p95_ms\":null");
        }
      std::fprintf(file,",\"samples_ms\":[");
      for(size_t f=0;f<result.times.size();++f) {
        std::fprintf(file,"%s",f==0 ? "" : ",");
        if(std::isfinite(result.times[f]) && result.times[f]>=0) std::fprintf(file,"%.9g",double(result.times[f]));
        else std::fprintf(file,"null");
        }
      std::fprintf(file,"]");
      }
    std::fprintf(file,"}");
    }
  std::fprintf(file,"]}\n");
  const bool outputValid=std::ferror(file)==0;
  const bool closeValid=std::fclose(file)==0;
  // Even a disqualified sample window keeps all 1020 activity rows and all 900
  // native timing samples. Return failure only after the raw file is complete.
  if(!outputValid || !closeValid) std::fprintf(stderr,"FAIL: twenty-body raw output could not be completed\n");
  if(!valid) std::fprintf(stderr,"FAIL: twenty-body coverage or samples; full raw result written to %s\n",output);
  return valid && outputValid && closeValid;
  }

}

int main(int argc, const char** argv) {
  try {
    if(argc==3 && (std::string_view(argv[1])=="--benchmark20" || std::string_view(argv[1])=="--preflight20")) {
      return benchmarkTwentyBodies(argv[2],std::string_view(argv[1])=="--preflight20") ? EXIT_SUCCESS : EXIT_FAILURE;
      }
    if(argc==3 && std::string_view(argv[1])=="--replay-creation") {
      replayCreation(argv[2]);
      return EXIT_SUCCESS;
      }
    if(argc==2 && std::string_view(argv[1])=="--water") {
      testRagdollWaterWolfHockTargetIndependentOfMass();
      testRagdollWaterFloatSleepRestoreAndToggle();
      testRagdollWaterDenseAndShallow();
      testRagdollWaterExcludesLivingReactions();
      testRagdollWaterSplashOnceAndMomentum();
      std::puts("Ragdoll water contracts passed");
      return EXIT_SUCCESS;
      }
    require(argc==1,"usage: gothic-ragdoll-contract [--water | --benchmark20 output.json | --preflight20 output.json | --replay-creation captured.jsonl]");
    testInvalidCreationAndScale();
    testKinematicPoseAndInteraction();
    testLocalMotorReactions();
    testLimpBoneImpulseOnce();
    testScaledMassAndInertia();
    testItemMassImpulseAndWake();
    testCorpseRayMetadata();
    testProjectileWorldStopOptIn();
    testSpellWorldImpactMetadata();
    testPoseOverlayAndRecovery();
    testRecoveryMatchesAnimBlendAndSaves();
    testMalformedRecoveryArchive();
    testPoseActualDriverSave();
    testPoseAffineRecoveryAndSaves();
    testPhysicalVelocityHandoff();
    testPoseFarWorldAttachments();
    testOriginReplayHierarchySeed();
    testFiniteRecoveryHierarchy();
    testBenchmarkSleepPolicy();
    testDropSleepWake();
    testAnatomicalHipAndAnkle();
    testInsectRootBody();
    testRagdollWaterWolfHockTargetIndependentOfMass();
    testRagdollWaterFloatSleepRestoreAndToggle();
    testRagdollWaterDenseAndShallow();
    testRagdollWaterExcludesLivingReactions();
    testRagdollWaterSplashOnceAndMomentum();
    testSlopeAndCliff();
    testStateRoundTripAndAtomicRejection();
    testMovedStateRebaseAndReference();
    testAnatomicalKneeWithLongitudinalBoneAxis();
    std::puts("Ragdoll contracts passed");
    return EXIT_SUCCESS;
    } catch(const std::exception& error) {
      std::fprintf(stderr,"FAIL: ragdoll contract exception: %s\n",error.what());
      return EXIT_FAILURE;
      }
  }
