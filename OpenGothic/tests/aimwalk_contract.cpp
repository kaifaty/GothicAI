#include "graphics/mesh/aimwalk.h"
#include "graphics/mesh/animmath.h"
#include "graphics/mesh/footik.h"
#include "graphics/mesh/ikmath.h"
#include "graphics/mesh/skeleton.h"
#include "asset/fromzenkit.h"

#include <zenkit/ModelHierarchy.hh>
#include <zenkit/ModelAnimation.hh>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

using Tempest::Matrix4x4;
using Tempest::Vec3;
using IkMath::length;
using IkMath::position;

constexpr float Pi = 3.14159265358979323846f;
constexpr uint64_t FrameMs = 33;
constexpr size_t Root = 0;
constexpr size_t LeftThigh = 7, LeftCalf = 8, LeftFoot = 9, LeftToe = 10;
constexpr size_t RightThigh = 11, RightCalf = 12, RightFoot = 13, RightToe = 14;
constexpr std::array<size_t,2> Feet = {LeftFoot,RightFoot};
constexpr std::array<size_t,2> Toes = {LeftToe,RightToe};

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

void near(const Vec3& actual, const Vec3& expected, float tolerance, const char* message) {
  near(length(actual-expected),0.f,tolerance,message);
  }

Vec3 rotated(const Vec3& point, float yaw) {
  const float c = std::cos(yaw), s = std::sin(yaw);
  return {c*point.x+s*point.z,point.y,-s*point.x+c*point.z};
  }

Matrix4x4 objectAt(const Vec3& origin, float yaw) {
  auto result = Matrix4x4::mkIdentity();
  const float c = std::cos(yaw), s = std::sin(yaw);
  result.set(0,0,c); result.set(0,2,-s);
  result.set(2,0,s); result.set(2,2,c);
  result.set(3,0,origin.x); result.set(3,1,origin.y); result.set(3,2,origin.z);
  return result;
  }

zenkit::ModelHierarchy hierarchy() {
  zenkit::ModelHierarchy result{};
  struct Bone final { const char* name; int16_t parent; Vec3 offset; };
  // Slightly bent knees give the solver a stable bend plane and room to settle the pelvis.
  const Bone bones[] = {
    {"BIP01",-1,{0,0,0}}, {"BIP01 PELVIS",0,{0,86,0}},
    {"BIP01 SPINE",1,{0,8,0}}, {"BIP01 SPINE1",2,{0,16,0}},
    {"BIP01 SPINE2",3,{0,18,0}},
    {"BIP01 L UPPERARM",4,{20,0,0}}, {"BIP01 R UPPERARM",4,{-20,0,0}},
    {"BIP01 L THIGH",1,{10,0,0}}, {"BIP01 L CALF",7,{0,-37,10}},
    {"BIP01 L FOOT",8,{0,-37,-10}}, {"BIP01 L TOE0",9,{0,-12,15}},
    {"BIP01 R THIGH",1,{-10,0,0}}, {"BIP01 R CALF",11,{0,-37,10}},
    {"BIP01 R FOOT",12,{0,-37,-10}}, {"BIP01 R TOE0",13,{0,-12,15}}
    };
  for(const auto& bone:bones) {
    zenkit::ModelHierarchyNode node;
    node.name = bone.name;
    node.parent_index = bone.parent;
    auto local = Matrix4x4::mkIdentity();
    local.set(3,0,bone.offset.x); local.set(3,1,bone.offset.y); local.set(3,2,bone.offset.z);
    static_assert(sizeof(node.transform)==sizeof(local));
    std::memcpy(&node.transform,&local,sizeof(local));
    result.nodes.push_back(node);
    }
  result.bbox.min = {-40,0,-40}; result.bbox.max = {40,160,40};
  result.collision_bbox = result.bbox;
  return result;
  }

struct Fixture final {
  Skeleton skeleton{asset::fromZenKit(hierarchy()),nullptr,"HUMANS.MDS"};
  std::vector<asset::AnimationSample> base;
  Animation::Sequence sequence;

  Fixture() {
    for(const auto& node:skeleton.nodes) {
      asset::AnimationSample sample;
      sample.position = {node.tr.at(3,0),node.tr.at(3,1),node.tr.at(3,2)};
      sample.rotation = {1,0,0,0};
      base.push_back(sample);
      }
    sequence.data = std::make_shared<Animation::AnimData>();
    auto& data = *sequence.data;
    data.numFrames = 9;
    data.fpsRate = 8.f;
    data.moveTr = {0,0,100};
    data.hasMoveTr = true;
    for(size_t bone=0; bone<base.size(); ++bone)
      data.nodeIndex.push_back(uint32_t(bone));
    for(size_t frame=0; frame<data.numFrames; ++frame) {
      const float angle = 2.f*Pi*float(frame)/float(data.numFrames-1);
      for(size_t bone=0; bone<base.size(); ++bone) {
        auto sample = base[bone];
        float bend = 0.f;
        if(bone==LeftThigh || bone==RightThigh)
          bend = .24f*std::sin(angle+(bone==RightThigh ? Pi : 0.f));
        if(bone==LeftCalf || bone==RightCalf)
          bend = -.18f*std::max(0.f,std::sin(angle+(bone==RightCalf ? Pi : 0.f)));
        sample.rotation = {std::cos(bend*.5f),std::sin(bend*.5f),0,0};
        if(bone==Root)
          sample.position.y -= 2.f*(1.f-std::cos(2.f*angle));
        data.samples.push_back(sample);
        }
      if(frame+1<data.numFrames)
        data.tr.push_back({0,0,100.f*float(frame)/float(data.numFrames-1)});
      }
    }

  std::vector<Matrix4x4> pose(const Matrix4x4& object) const {
    std::vector<Matrix4x4> result(base.size());
    for(size_t i=0; i<base.size(); ++i) {
      const auto parent = skeleton.nodes[i].parent;
      result[i] = (parent<result.size() ? result[parent] : object)*mkMatrix(base[i]);
      }
    return result;
    }
  };

using Contacts = std::array<Vec3,2>;

void testImportedHierarchyAndSamples() {
  zenkit::ModelHierarchy source{};
  source.nodes.resize(3);
  const Matrix4x4 local[] = {objectAt({7,8,9},.31f),objectAt({-5,4,3},-.27f),objectAt({2,-6,1},.43f)};
  const int16_t parents[] = {2,-1,-1};
  for(size_t i=0;i<source.nodes.size();++i) {
    auto& node=source.nodes[i];
    node.name="IMPORT_NODE_"+std::to_string(i);
    node.parent_index=parents[i];
    std::memcpy(&node.transform,&local[i],sizeof(local[i]));
    }
  source.root_translation={11,-22,33};
  source.bbox={{-40,-2,-30},{40,160,35}};
  source.collision_bbox={{-20,0,-15},{20,150,18}};
  const auto own=asset::fromZenKit(source);
  require(own.nodes.size()==source.nodes.size(),"hierarchy import must retain every bone");
  for(size_t i=0;i<source.nodes.size();++i)
    require(own.nodes[i].name==source.nodes[i].name && own.nodes[i].parent_index==parents[i] &&
            std::memcmp(&own.nodes[i].transform,&source.nodes[i].transform,sizeof(source.nodes[i].transform))==0,
            "hierarchy import must preserve bone order, parents and matrix bytes");
  require(std::memcmp(&own.bbox,&source.bbox,sizeof(source.bbox))==0 &&
          std::memcmp(&own.collision_bbox,&source.collision_bbox,sizeof(source.collision_bbox))==0 &&
          std::memcmp(&own.root_translation,&source.root_translation,sizeof(source.root_translation))==0,
          "hierarchy import must preserve both bounds and root translation");
  const Skeleton skeleton(own,nullptr,"IMPORTED.MDS");
  require(!skeleton.ordered && skeleton.rootNodes==std::vector<size_t>({1,2}) &&
          skeleton.nodes[0].parent==2,"runtime skeleton must retain unordered bones and multiple roots");
  auto firstRoot=local[1],secondRoot=local[2];
  firstRoot.translate(11,-22,33);
  secondRoot.translate(11,-22,33);
  const Matrix4x4 expected[]={secondRoot*local[0],firstRoot,secondRoot};
  for(size_t i=0;i<3;++i)
    for(int column=0;column<4;++column)
      for(int row=0;row<4;++row)
        near(skeleton.tr[i].at(column,row),expected[i].at(column,row),.0001f,
             "imported skeleton must keep accumulated bind transforms and translate every root");

  zenkit::ModelAnimation animation{};
  animation.samples={{{.25f,-0.f,3.f},{1,0,0,0}},{{10,2,-4},{-1,-0.f,-0.f,-0.f}}};
  const auto imported=asset::fromZenKit(animation);
  require(imported.samples.size()==animation.samples.size(),"animation import must retain the sample count");
  for(size_t i=0;i<animation.samples.size();++i)
    require(std::memcmp(&imported.samples[i],&animation.samples[i],sizeof(animation.samples[i]))==0,
            "animation import must preserve sample bytes, signed zero and quaternion order");
  const auto sample=mix(imported.samples[0],imported.samples[1],.25f);
  near(Vec3(sample.position.x,sample.position.y,sample.position.z),{2.6875f,.5f,1.25f},.000001f,
       "imported samples must retain linear position interpolation");
  near(sample.rotation.w,1.f,.000001f,"opposite quaternion signs must retain the shortest interpolation");
  near(sample.rotation.x,0.f,.000001f,"imported quaternion X must retain its component");
  near(sample.rotation.y,0.f,.000001f,"imported quaternion Y must retain its component");
  near(sample.rotation.z,0.f,.000001f,"imported quaternion Z must retain its component");
  const auto transformed=mkMatrix(sample);
  auto expectedSample=Matrix4x4::mkIdentity();
  expectedSample.translate(2.6875f,.5f,1.25f);
  for(int column=0;column<4;++column)
    for(int row=0;row<4;++row)
      near(transformed.at(column,row),expectedSample.at(column,row),.000001f,
           "imported sample matrices must retain their translation and rotation");
  }

struct Actor final {
  const Fixture& fixture;
  AimWalk walk;
  FootIk ik;
  Matrix4x4 object;
  Vec3 bodyOffset;
  std::vector<Matrix4x4> palette;

  Actor(const Fixture& fixture, const Vec3& origin = {}, float yaw = 0.f, float counterweight = 0.f)
    :fixture(fixture),object(objectAt(origin,yaw)),bodyOffset(rotated({0,0,-counterweight},yaw)) {
    walk.setSkeleton(&fixture.skeleton);
    ik.setSkeleton(&fixture.skeleton);
    require(walk.isValid(),"the synthetic skeleton must support aim walking");
    require(ik.isValid() && !ik.isQuadruped(),"the synthetic skeleton must support human foot IK");
    rebuild();
    }

  void rebuild() {
    palette = fixture.pose(object);
    walk.apply(palette.data(),palette.size(),fixture.base.data(),object);
    ik.apply(palette.data(),palette.size(),object.at(3,1),bodyOffset,&walk);
    walk.captureFeet(palette.data(),palette.size());
    }

  bool tick(float direction, bool blocked = false, bool active = true, int rebuilds = 1) {
    AimWalk::Input input;
    input.active = active;
    input.legs = &fixture.sequence;
    input.walk = direction;
    input.blocked = blocked;
    input.dt = FrameMs;
    const bool changed = walk.update(input);
    FootIk::Input ground;
    ground.enable = true;
    ground.floorY = object.at(3,1);
    ground.crouch = 8.f;
    ground.dt = FrameMs;
    for(size_t leg=0; leg<2; ++leg) {
      ground.ground[leg].hasCol = true;
      ground.ground[leg].toeCol = true;
      ground.ground[leg].y = ground.floorY;
      ground.ground[leg].toeY = ground.floorY;
      }
    ik.update(ground);
    for(int i=0; i<rebuilds; ++i)
      rebuild();
    return changed;
    }

  Contacts contacts() const {
    return {position(palette[LeftFoot]),position(palette[RightFoot])};
    }

  AimWalk::Probe probe() const {
    AimWalk::Probe result;
    walk.probe(result);
    return result;
    }
  };

void warm(Actor& actor, float direction, int frames = 40, int rebuilds = 1) {
  for(int frame=0; frame<frames; ++frame)
    actor.tick(direction,false,true,rebuilds);
  near(actor.probe().weight,1.f,.0001f,"walking must be fully blended before the stop");
  }

void testWalkingRootMotion(const Fixture& fixture) {
  for(float direction:{-1.f,1.f}) {
    AimWalk walk;
    walk.setSkeleton(&fixture.skeleton);
    float phase = 0.f;
    for(int frame=0; frame<40; ++frame) {
      AimWalk::Input input;
      input.active = true;
      input.legs = &fixture.sequence;
      input.walk = direction;
      input.dt = FrameMs;
      walk.update(input);
      const auto probe = [&] { AimWalk::Probe p; walk.probe(p); return p; }();
      const float weight = std::min(1.f,float(frame+1)*float(FrameMs)/250.f);
      const float speed = direction>0.f ? AimWalk::Forward : AimWalk::Back;
      phase = std::fmod(phase+direction*speed*float(FrameMs)/100000.f+1.f,1.f);
      near(probe.weight,weight,.00001f,"the moving gait must retain its original 250 ms blend-in");
      near(probe.phase,phase,.00001f,"the moving gait must retain its original signed cycle rate");
      near(walk.move(FrameMs),Vec3(0,0,direction*speed*float(FrameMs)/1000.f*weight),.0001f,
           "walking root motion must keep its original speed and blend weight");
      }
    }
  }

void testStop(const Fixture& fixture, float direction, bool blocked, int warmFrames) {
  Actor actor(fixture,{},0.f,blocked ? 6.f : 0.f);
  warm(actor,direction,warmFrames);
  const float phase = actor.probe().phase;
  std::vector<Contacts> trace{actor.contacts()};
  std::vector<Contacts> toeTrace{{position(actor.palette[LeftToe]),position(actor.palette[RightToe])}};
  std::vector<bool> stopping{false};
  const bool blockedForward = blocked && direction>0.f;
  float maxTravel = 0.f;
  uint64_t settledAt = 0;
  size_t lastStopping = 0;
  for(int frame=0; frame<24; ++frame) {
    actor.tick(blocked ? direction : 0.f,blocked);
    near(actor.walk.move(FrameMs),{},.0001f,"stopping legs must never move the body");
    near(actor.probe().phase,phase,.000001f,"stopping must freeze the moving gait phase");
    const auto contacts = actor.contacts();
    // Include the completed step's handoff in every case, including the blocked forward step.
    for(size_t foot=0; foot<Feet.size(); ++foot)
      maxTravel = std::max(maxTravel,length(contacts[foot]-trace.back()[foot]));
    trace.push_back(contacts);
    toeTrace.push_back({position(actor.palette[LeftToe]),position(actor.palette[RightToe])});
    stopping.push_back(actor.walk.isStopping());
    if(actor.walk.isStopping())
      lastStopping = trace.size()-1;
    else if(settledAt==0 && lastStopping>0)
      settledAt=uint64_t(frame+1)*FrameMs;
    }
  // Discover the support from the first three final poses, independently of the implementation's side.
  float earlyTravel[2] = {};
  for(size_t frame=1; frame<=3; ++frame)
    for(size_t foot=0; foot<Feet.size(); ++foot)
      earlyTravel[foot] += length(trace[frame][foot]-trace[frame-1][foot]);
  const size_t support = earlyTravel[0]<=earlyTravel[1] ? 0 : 1;
  const size_t swing = support^1;
  float supportTravel = 0.f, supportDrift = 0.f;
  for(size_t frame=1; frame<trace.size(); ++frame) {
    if(!stopping[frame])
      continue;
    supportTravel = std::max(supportTravel,length(trace[frame][support]-trace[frame-1][support]));
    supportDrift = std::max(supportDrift,length(trace[frame][support]-trace.front()[support]));
    }
  const float supportHandoff = lastStopping+1<trace.size() ?
                                 length(trace[lastStopping+1][support]-trace[lastStopping][support]) : 0.f;

  const auto start = trace.front()[swing];
  const size_t finishFrame = trace.size()-1;
  const auto finish = trace[finishFrame][swing];
  float lift = 0.f;
  if(!blockedForward) {
    const Vec3 horizontal(finish.x-start.x,0,finish.z-start.z);
    const float horizontalSquared = IkMath::dot(horizontal,horizontal);
    for(size_t frame=0; frame<=finishFrame; ++frame) {
      const auto& contact = trace[frame][swing];
      const Vec3 offset(contact.x-start.x,0,contact.z-start.z);
      // A tiny horizontal return still has a meaningful lift above its start-to-finish chord.
      const float timeProgress = float(frame>0 ? frame-1 : 0)/std::max(1.f,float(settledAt)/float(FrameMs)-1.f);
      const float progress = std::clamp(horizontalSquared>1.e-4f ?
                                        IkMath::dot(offset,horizontal)/horizontalSquared : timeProgress,0.f,1.f);
      const float chordY = start.y+(finish.y-start.y)*progress;
      lift = std::max(lift,contact.y-chordY);
      }
    }
  std::printf("aimwalk stop %s %s: settle=%llu ms ankle=%.3f cm/frame support=%.3f cm/frame handoff=%.3f cm drift=%.3f cm lift=%.3f cm\n",
              direction>0.f ? "forward" : "back",blocked ? "blocked" : "released",
              static_cast<unsigned long long>(settledAt),double(maxTravel),double(supportTravel),double(supportHandoff),double(supportDrift),double(lift));
  std::fflush(stdout);
  require(settledAt>=200 && settledAt<=400,"the legs must settle in 0.2 to 0.4 seconds");
  require(maxTravel<=6.f+.001f,"a final ankle must travel at most 6 cm per 30 fps frame, including the completed handoff");
  require(supportTravel<=1.f+.001f,"the support foot must slide at most 1 cm per frame");
  require(supportDrift<=.1f,"the support contact must remain planted throughout the stop");
  require(supportHandoff<=1.f+.001f,"the support foot must slide at most 1 cm in the first completed handoff frame");
  if(!blockedForward)
    require(lift>=3.f,"the free foot must lift at least 3 cm above a sliding return to the stance");
  if(blocked) {
    // The synthetic edge is the stopped body's plane, perpendicular to the requested movement.
    const Vec3 towardEdge(0,0,direction);
    const Vec3 stop = position(actor.object);
    for(size_t frame=lastStopping+1; frame<trace.size(); ++frame) {
      require(IkMath::dot(trace[frame][swing]-stop,towardEdge)<=.1f,
              "the free ankle must remain on the safe side through the completed handoff");
      require(IkMath::dot(toeTrace[frame][swing]-stop,towardEdge)<=.1f,
              "the free toes must remain on the safe side through the completed handoff");
      }
    }
  require(!actor.walk.isStopping() && !actor.walk.isActive(),"a completed step must release the legs to the ordinary stance");
  require(!actor.tick(0.f),"idle manual aim must not request another walking pose rebuild");
  }

void testTurnAfterStop(const Fixture& fixture) {
  for(bool blocked:{false,true}) {
    const float counterweight = blocked ? 6.f : 0.f;
    Actor stopped(fixture,{},0.f,counterweight);
    warm(stopped,1.f);
    for(int frame=0; frame<24; ++frame)
      stopped.tick(blocked ? 1.f : 0.f,blocked);
    require(!stopped.walk.isStopping() && !stopped.walk.isActive(),"turning begins after the stop contacts have been released");
    const auto before = stopped.contacts();
    const Contacts toesBefore{position(stopped.palette[LeftToe]),position(stopped.palette[RightToe])};
    constexpr float yaw = Pi*.5f;
    stopped.object = objectAt({},yaw);
    stopped.bodyOffset = rotated({0,0,-counterweight},yaw);
    for(int frame=0; frame<30; ++frame)
      require(!stopped.tick(0.f),"turning in settled manual aim must not reactivate the stop");
    const auto actual = stopped.contacts();
    for(size_t foot=0; foot<Feet.size(); ++foot) {
      const auto expected = rotated(before[foot],yaw);
      const auto toe = position(stopped.palette[Toes[foot]]);
      const auto expectedToe = rotated(toesBefore[foot],yaw);
      near(Vec3(actual[foot].x,0,actual[foot].z),Vec3(expected.x,0,expected.z),.1f,
           "after a 90 degree turn settled feet must turn with the model and ordinary FootIk");
      near(Vec3(toe.x,0,toe.z),Vec3(expectedToe.x,0,expectedToe.z),.1f,
           "after a 90 degree turn settled toes must turn with the model");
      require(length(Vec3(actual[foot].x-before[foot].x,0,actual[foot].z-before[foot].z))>5.f,
              "a completed stop must not pin either foot to its old world-space contact");
      }
    }
  }

void testRebuildAndWorldSpace(const Fixture& fixture) {
  const Vec3 origin(250,35,-180);
  constexpr float yaw = 1.3f;
  Actor once(fixture), repeated(fixture), transformed(fixture,origin,yaw);
  for(int frame=0; frame<65; ++frame) {
    const float direction = frame<40 ? 1.f : 0.f;
    once.tick(direction);
    repeated.tick(direction,false,true,3);
    transformed.tick(direction,false,true,3);
    const auto a = once.contacts(), b = repeated.contacts(), c = transformed.contacts();
    for(size_t foot=0; foot<Feet.size(); ++foot) {
      near(b[foot],a[foot],.002f,"rebuilding the pose three times must not advance the step or recapture contacts");
      near(c[foot],origin+rotated(a[foot],yaw),.01f,"stop contacts must remain in world space under model yaw and translation");
      }
    near(repeated.probe().phase,once.probe().phase,.000001f,"rebuild count must not advance phase");
    near(repeated.probe().weight,once.probe().weight,.000001f,"rebuild count must not advance the settling weight");
    }
  }

void testResumeAndLeaveManualAim(const Fixture& fixture) {
  Actor resumed(fixture), fresh(fixture);
  warm(resumed,1.f);
  for(int frame=0; frame<24; ++frame)
    resumed.tick(0.f);
  for(int frame=0; frame<12; ++frame) {
    resumed.tick(-1.f);
    fresh.tick(-1.f);
    near(resumed.probe().weight,fresh.probe().weight,.00001f,"resuming must use the ordinary walking blend-in");
    near(resumed.probe().phase,fresh.probe().phase,.00001f,"resuming must use the ordinary new walking phase");
    near(resumed.walk.move(FrameMs),fresh.walk.move(FrameMs),.0001f,"resuming must restore ordinary root motion");
    }
  for(int frame=0; frame<24; ++frame)
    resumed.tick(0.f,false,false);
  require(!resumed.walk.isActive(),"leaving manual aim must release the stopped contacts after its ordinary fade");

  Actor held(fixture);
  warm(held,1.f);
  for(int frame=0; frame<24; ++frame)
    held.tick(0.f);
  held.tick(0.f,false,false);
  require(!held.walk.isActive(),"leaving settled manual aim must release the retained stance");
  }

void testIdleWithoutSequence(const Fixture& fixture) {
  for(bool blocked:{false,true}) {
    Actor actor(fixture,{},0.f,blocked ? 6.f : 0.f);
    warm(actor,1.f);
    for(int frame=0; frame<24; ++frame)
      actor.tick(blocked ? 1.f : 0.f,blocked);
    require(!actor.walk.isActive() && !actor.walk.isStopping(),"the idle sequence is released after the stop finishes");
    const auto settled = actor.palette;
    AimWalk::Input idle;
    idle.active = true;
    idle.dt = FrameMs;
    // The game drops the gait sequence once neither movement nor a stop owns the legs.
    idle.legs = nullptr;
    for(int frame=0; frame<3; ++frame) {
      require(!actor.walk.update(idle),"idle with no gait sequence must not request a pose rebuild");
      require(!actor.walk.isActive() && !actor.walk.isStopping(),"idle with no gait sequence must remain inactive");
      near(actor.walk.move(FrameMs),{},.0001f,"idle with no gait sequence must not request root motion");
      actor.rebuild();
      for(size_t bone=0; bone<settled.size(); ++bone)
        for(int column=0; column<4; ++column)
          for(int row=0; row<4; ++row)
            near(actor.palette[bone].at(column,row),settled[bone].at(column,row),.001f,
                 "dropping the idle gait sequence must preserve the settled local pose");
      }
    }
  }

void testInterruptedStopClock(const Fixture& fixture) {
  for(float initialDirection:{-1.f,1.f}) {
    Actor actor(fixture);
    warm(actor,initialDirection,initialDirection>0.f ? 40 : 48);
    float expectedWeight = actor.probe().weight;
    float expectedPhase = actor.probe().phase;
    const float initialSpeed = initialDirection>0.f ? AimWalk::Forward : AimWalk::Back;
    for(int frame=0; frame<6; ++frame) {
      actor.tick(0.f);
      // The original BlendOut advances the gait's clock on every frame with nonzero weight.
      expectedWeight = std::max(0.f,expectedWeight-float(FrameMs)/300.f);
      if(expectedWeight>0.f)
        expectedPhase = std::fmod(expectedPhase+initialDirection*initialSpeed*float(FrameMs)/100000.f+1.f,1.f);
      }
    require(actor.walk.isStopping(),"a six-frame release must still be in the unfinished stop");
    const float resumedDirection = -initialDirection;
    const float resumedSpeed = resumedDirection>0.f ? AimWalk::Forward : AimWalk::Back;
    for(int frame=0; frame<6; ++frame) {
      actor.tick(resumedDirection);
      expectedWeight = std::min(1.f,expectedWeight+float(FrameMs)/250.f);
      expectedPhase = std::fmod(expectedPhase+resumedDirection*resumedSpeed*float(FrameMs)/100000.f+1.f,1.f);
      const auto probe = actor.probe();
      near(probe.weight,expectedWeight,.00001f,"an interrupted stop must resume the original linear-fade blend weight");
      near(probe.phase,expectedPhase,.00001f,"an interrupted stop must resume the original continuously fading gait clock");
      near(actor.walk.move(FrameMs),Vec3(0,0,resumedDirection*resumedSpeed*float(FrameMs)/1000.f*expectedWeight),.0001f,
           "an interrupted stop must resume the original root motion when the direction reverses");
      }
    }
  }

void testSettledStanceFloor(const Fixture& fixture) {
  const bool wasProbing = FootIk::isProbing();
  FootIk::setProbing(true);
  FootIk ik;
  ik.setSkeleton(&fixture.skeleton);
  auto palette = fixture.pose(Matrix4x4::mkIdentity());
  ik.apply(palette.data(),palette.size(),0.f);
  FootIk::Input input;
  input.enable = true;
  input.dt = FrameMs;
  for(size_t leg=0; leg<2; ++leg) {
    input.ground[leg].hasCol = true;
    input.ground[leg].toeCol = true;
    input.ground[leg].y = input.ground[leg].toeY = leg==0 ? -10.f : 5.f;
    }
  auto target = [&]() {
    ik.update(input);
    ik.update(input);
    FootIk::Probe probe;
    ik.probe(probe,input);
    require(probe.valid,"the explicit ground fixture must expose a valid FootIk probe");
    return probe.target;
    };
  near(target(),-10.f,.001f,"ordinary FootIk must lower the body toward the lower actual foot ground");
  input.hasStanceFloor = true;
  input.stanceFloor = -4.f;
  near(target(),-4.f,.001f,"settled aim must use its ordinary stance floor for the body target");
  input.ground[0].y = input.ground[0].toeY = -18.f;
  input.ground[1].y = input.ground[1].toeY = 12.f;
  near(target(),-4.f,.001f,"changing actual foot grounds must not change a retained stance body target");
  input.hasStanceFloor = false;
  near(target(),-18.f,.001f,"releasing the stance floor must restore the ordinary actual-ground body target");
  FootIk::setProbing(wasProbing);
  }

}

int main() {
  AimWalk::setEnabled(true);
  FootIk::setEnabled(true);
  testImportedHierarchyAndSamples();
  const Fixture fixture;
  testWalkingRootMotion(fixture);
  for(float direction:{-1.f,1.f})
    for(bool blocked:{false,true})
      testStop(fixture,direction,blocked,direction>0.f ? 40 : 48);
  testRebuildAndWorldSpace(fixture);
  testResumeAndLeaveManualAim(fixture);
  testTurnAfterStop(fixture);
  testIdleWithoutSequence(fixture);
  testInterruptedStopClock(fixture);
  testSettledStanceFloor(fixture);
  std::puts("aimwalk contracts passed");
  }
