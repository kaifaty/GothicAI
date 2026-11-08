#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#include "physics_contract_common.h"
#include "asset/fromzenkit.h"
#include "physics/collisionworld.h"
#include "physics/ragdoll.h"
#include "graphics/mesh/skeleton.h"
#include "graphics/mesh/hitzones.h"
#include "game/damagecalculator.h"
#include "world/objects/npc.h"
#include "world/bodytraces.h"
#include "gothic.h"

#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <zenkit/ModelHierarchy.hh>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <optional>
#include <string_view>
#include <vector>

namespace PhysicsContract {
  void testPhysicsNoise() {
    CollisionWorld world;
    std::vector<PhysicsNoise> noises;
    std::vector<uint64_t> steps;
    world.setPhysicsNoiseCallback([&](const PhysicsNoise& noise) {
      noises.push_back(noise);
      steps.push_back(world.simulationStep());
      });
    require(world.physicsNoiseEnabled(),"physics noise must be enabled by default");
    auto floor=world.addCollisionBody(new JPH::BoxShape(JPH::Vec3(20,.5f,20)),at(0,-50,0),.7f,
                                      DynamicWorld::C_Landscape);
    auto cup=world.addDynamicBody(new JPH::BoxShape(JPH::Vec3::sReplicate(.05f)),at(0,100,0),.7f,.15f);
    cup->setNoiseProfile(MAT_CLAY,asset::MaterialGroup::STONE,"cup");
    auto table=world.addDynamicBody(new JPH::BoxShape(JPH::Vec3::sReplicate(.35f)),at(300,100,0),.7f,50.f,
                                    DynamicWorld::C_MovableVob);
    table->setNoiseProfile(MAT_WOOD,asset::MaterialGroup::WOOD,"table");
    auto soft=world.addDynamicBody(new JPH::BoxShape(JPH::Vec3::sReplicate(.05f)),at(-300,5.1f,0),.7f,.15f);
    soft->setNoiseProfile(MAT_CLAY,asset::MaterialGroup::STONE,"soft");
    auto wood=world.addCollisionBody(new JPH::BoxShape(JPH::Vec3(.3f,.1f,.3f)),at(600,40,0),.7f,DynamicWorld::C_Object);
    wood->setNoiseProfile(MAT_WOOD,asset::MaterialGroup::WOOD,"wood");
    auto metal=world.addDynamicBody(new JPH::BoxShape(JPH::Vec3::sReplicate(.05f)),at(600,100,0),.7f,1.f);
    metal->setNoiseProfile(MAT_METAL,asset::MaterialGroup::METAL,"metal");
    for(int i=0;i<120;++i) world.tick(17);
    const auto find=[&](std::string_view what) -> const PhysicsNoise* {
      for(const auto& noise:noises) if(noise.what==what) return &noise;
      return nullptr;
      };
    const auto* cupNoise=find("cup");
    const auto* tableNoise=find("table");
    const auto* metalNoise=find("metal");
    require(cupNoise!=nullptr && tableNoise!=nullptr && metalNoise!=nullptr,
            "items, movable furniture and contact with another object must emit noise");
    require(find("soft")==nullptr,"resting/subthreshold contact must remain silent");
    require(cupNoise->material==MAT_CLAY && cupNoise->radius<300.f && cupNoise->culprit==nullptr,
            "a small fallen cup must retain clay material, local radius and unknown culprit");
    require(tableNoise->material==MAT_WOOD && tableNoise->radius>=600.f && tableNoise->volume>cupNoise->volume,
            "a heavy table must be louder and audible at six metres");
    require(metalNoise->surface==asset::MaterialGroup::WOOD && metalNoise->material==MAT_METAL,
            "noise must retain both source and struck surface materials");
    requireNear(cupNoise->position.y,0.f,2.f,"noise position must be the contact in world centimetres");
    for(size_t i=0;i<noises.size();++i)
      for(size_t j=i+1;j<noises.size();++j)
        if(noises[i].body==noises[j].body)
          require(steps[j]>=steps[i]+29,"a logical body must not emit more often than every half second");
    const auto before=noises.size();
    world.setPhysicsNoiseEnabled(false);
    cup->restoreTransform(at(0,100,0),false);
    for(int i=0;i<60;++i) world.tick(17);
    require(noises.size()==before && !world.physicsNoiseEnabled(),"disabled physics noise must not emit events");
    world.setPhysicsNoiseEnabled(true);
    cup->restoreTransform(at(0,100,0),false);
    for(int i=0;i<60 && noises.size()==before;++i) world.tick(17);
    require(noises.size()>before,"runtime re-enable must restore impact noise");
    // Repeated hard contacts before the same half-second expires must share
    // the body's cooldown, including multiple collision manifolds/substeps.
    const auto firstCount=noises.size();
    cup->restoreTransform(at(0,5,0),false);
    require(cup->addImpulse({0,-2.f,0}),"cooldown fixture must accept a second hard impact");
    world.tick(17);
    require(noises.size()==firstCount,"a second immediate impact must be throttled per body");
    }

  void testRagdollNoise() {
    zenkit::ModelHierarchy hierarchy{};
    const char* names[]={"BIP01","BIP01 PELVIS","BIP01 SPINE","BIP01 HEAD"};
    const float offsets[]={0,95,40,40};
    for(int i=0;i<4;++i) {
      zenkit::ModelHierarchyNode node;
      node.name=names[i];
      node.parent_index=int16_t(i-1);
      const auto matrix=at(0,offsets[i],0);
      std::memcpy(&node.transform,&matrix,sizeof(matrix));
      hierarchy.nodes.push_back(node);
      }
    Skeleton skeleton(asset::fromZenKit(hierarchy),nullptr,"NOISE_HUMAN.MDS");
    HitZones zones;
    zones.setSkeleton(&skeleton);
    require(zones.isValid(),"noise ragdoll fixture must contain canonical capsules");
    CollisionWorld world;
    auto floor=world.addCollisionBody(new JPH::BoxShape(JPH::Vec3(20,.5f,20)),at(0,-50,0),.7f,
                                      DynamicWorld::C_Landscape);
    auto object=at(0,100,0);
    object.rotateOZ(90.f);
    std::vector<Tempest::Matrix4x4> pose;
    for(const auto& bone:skeleton.tr) pose.push_back(object*bone);
    auto body=world.createRagdoll(skeleton,zones,pose.data(),pose.size(),object,{0,-600,0});
    require(body!=nullptr && body->partCount()>1,"noise fixture must create several physical limbs");
    std::vector<uint64_t> steps;
    uint64_t identity=0;
    world.setPhysicsNoiseCallback([&](const PhysicsNoise& noise) {
      require(noise.material==MAT_LEATHER && noise.what=="ragdoll","body noise must retain its material and source");
      if(identity!=0) require(noise.body==identity,"limbs must share one logical noise identity");
      identity=noise.body;
      if(!steps.empty()) require(world.simulationStep()>=steps.back()+29,"ragdoll limbs must share a half-second cooldown");
      steps.push_back(world.simulationStep());
      });
    for(int i=0;i<120;++i) world.tick(17);
    require(!steps.empty(),"a fallen ragdoll must emit impact noise");
    }

  void testBodyTraces() {
    require(Gothic::PhysicsFlags{}.bodyTraces,"body traces must default to ON");
    require(BodyTraces::LifetimeMinutes==4320,"a body lives three game days");
    BodyTraces::Body body;
    body.clockFrom    = 1000;
    body.now          = 1000+BodyTraces::LifetimeMinutes;
    body.heroDistance = 3001;
    require(BodyTraces::removable(body),"an unseen three-day-old body with the hero beyond 30 m must leave");
    const auto with=[&](auto change) { auto b=body; change(b); return BodyTraces::removable(b); };
    require(!with([](auto& b){ b.now-=1; }),"a body younger than three game days must stay");
    require(!with([](auto& b){ b.heroDistance=3000; }),"a body exactly 30 m from the hero must stay");
    require(!with([](auto& b){ b.heroDistance=1200; }),"a body near the hero must stay");
    require(!with([](auto& b){ b.heroSees=true; }),"a body the hero sees must stay");
    {
    auto seen=body; seen.heroSees=true;
    require(BodyTraces::removableUnseen(seen),"sight is the last test: rays only for bodies that pass the rest");
    }
    require(!with([](auto& b){ b.missionItems=true; }),"a body with a mission item must stay");
    require(!with([](auto& b){ b.dragon=true; }),"a dragon must stay");
    require(!with([](auto& b){ b.immortal=true; }),"an IMMORTAL body must stay");
    require(!with([](auto& b){ b.storyType=true; }),"a body of a main script type must stay");
    require(!with([](auto& b){ b.enabled=false; }),"body traces OFF must never remove a body");
    require(!with([](auto& b){ b.dead=false; }),"the living never leave by the body rule");
    require(!with([](auto& b){ b.player=true; }),"the hero never leaves by the body rule");
    require(!with([](auto& b){ b.clockFrom=-1; }),"a body without a lifetime start must stay");
    require(with([](auto& b){ b.now+=100000; b.heroDistance=1e6f; }),"an old far body must leave");

    require(BodyTraces::isTrace(true,true,false,GIL_NONE,GIL_SEPERATOR_HUM),"a dead person is a trace");
    require(BodyTraces::isTrace(true,true,false,GIL_SEPERATOR_HUM-1,GIL_SEPERATOR_HUM),"every human guild is a trace");
    require(!BodyTraces::isTrace(true,true,false,GIL_SEPERATOR_HUM,GIL_SEPERATOR_HUM),"a monster's body is not a trace");
    require(!BodyTraces::isTrace(true,true,false,GIL_DRAGON,GIL_SEPERATOR_HUM),"a dragon's body is not a trace");
    require(!BodyTraces::isTrace(false,true,false,GIL_NONE,GIL_SEPERATOR_HUM),"body traces OFF: no body trace");
    require(!BodyTraces::isTrace(true,false,false,GIL_NONE,GIL_SEPERATOR_HUM),"a living person is not a body trace");
    require(!BodyTraces::isTrace(true,true,true,GIL_NONE,GIL_SEPERATOR_HUM),"the dead hero is not a body trace");

    for(int32_t type:{1,4,7})
      require(BodyTraces::isStoryType(type,BodyTraces::StoryTypes),"NPCTYPE_MAIN, NPCTYPE_OCMAIN and NPCTYPE_BL_MAIN are story types");
    for(int32_t type:{0,2,3,5,6})
      require(!BodyTraces::isStoryType(type,BodyTraces::StoryTypes),"ambient and friend types are not story types");

    // Story types from the scripts: the Gothic II fallback only when no symbol exists at all.
    {
    const auto none = BodyTraces::storyTypes({std::nullopt,std::nullopt,std::nullopt});
    require(none==BodyTraces::StoryTypes,"no NPCTYPE symbol at all: the Gothic II values 1, 4, 7");
    const auto g1 = BodyTraces::storyTypes({1,std::nullopt,std::nullopt});
    require(g1==std::array<int32_t,3>{1,-1,-1},"a missing symbol beside a present one must match nothing (-1)");
    require(BodyTraces::isStoryType(1,g1) && !BodyTraces::isStoryType(4,g1) && !BodyTraces::isStoryType(7,g1),
            "with only NPCTYPE_MAIN defined, types 4 and 7 are not exceptions");
    const auto all = BodyTraces::storyTypes({11,14,17});
    require(all==std::array<int32_t,3>{11,14,17},"the scripts' own values win");
    }

    // One death stamps the record once: a BodyController defers ZS_Dead, later blows keep it.
    require(BodyTraces::stampsDeath(true,false,0,false),"the first fatal blow stamps the body record");
    require(!BodyTraces::stampsDeath(true,false,4,false),"a blow before the deferred ZS_Dead keeps the first record");
    require(!BodyTraces::stampsDeath(true,true,4,true),"a blow on a corpse keeps its record");
    require(!BodyTraces::stampsDeath(false,false,0,false),"unconsciousness stamps nothing");
    require(BodyTraces::stampsDeath(true,false,4,true),"a body that rose again (it reached ZS_Dead) dies anew");
    }

  void testBodyImpactDamage() {
    require(Gothic::PhysicsFlags{}.bodyImpactDamage,"body impact damage must default to ON");
    require(Npc::canTakeBodyImpact(true,false,false,false),"a living physical body must receive impact damage");
    require(!Npc::canTakeBodyImpact(false,false,false,false),"body impact OFF must preserve the old damage path");
    require(!Npc::canTakeBodyImpact(true,true,false,false),"an unconscious body must not receive impact damage");
    require(!Npc::canTakeBodyImpact(true,false,true,false),"a dead body must not receive impact damage");
    require(!Npc::canTakeBodyImpact(true,false,false,true),"an IMMORTAL body must not receive impact damage");

    Npc::BodyImpactWindow window;
    // Native torso contacts and spell capsule wall/landing contacts feed this
    // same per-NPC window, so later sources must replace rather than add damage.
    window.add(1000,.6f);
    window.add(1100,1.f);
    window.add(1299,.8f);
    requireNear(window.take(1299),0.f,.000001f,"a body impact window must wait for all three hundred milliseconds");
    requireNear(window.take(1300),1.f,.000001f,"one body impact window must emit only its maximum normal speed");
    requireNear(window.take(1300),0.f,.000001f,"a consumed body impact window must not emit twice");
    window.add(1300,.6f);
    requireNear(window.take(1600),.6f,.000001f,"a later impact must open an independent body window");
    window.add(1700,1.4f);
    window.clear();
    requireNear(window.take(2000),0.f,.000001f,"disabling body impact damage must discard pending contacts");

    const auto impact=[&](bool limbs,int category,bool enabled=true) {
      zenkit::ModelHierarchy hierarchy{};
      const char* torsoNames[]={"BIP01","BIP01 PELVIS","BIP01 SPINE","BIP01 HEAD"};
      const char* limbNames[]={"BIP01","BIP01 L UPPERARM","BIP01 L FOREARM","BIP01 L HAND"};
      const float offsets[]={0,95,40,40};
      for(int i=0;i<4;++i) {
        zenkit::ModelHierarchyNode node;
        node.name=limbs ? limbNames[i] : torsoNames[i];
        node.parent_index=int16_t(i-1);
        const auto matrix=at(0,offsets[i],0);
        std::memcpy(&node.transform,&matrix,sizeof(matrix));
        hierarchy.nodes.push_back(node);
        }
      Skeleton skeleton(asset::fromZenKit(hierarchy),nullptr,limbs ? "IMPACT_ARM.MDS" : "IMPACT_TORSO.MDS");
      HitZones zones;
      zones.setSkeleton(&skeleton);
      require(zones.isValid(),"body impact fixture must contain native capsules");
      CollisionWorld world;
      world.setBodyImpactEnabled(enabled);
      auto floor=world.addCollisionBody(new JPH::BoxShape(JPH::Vec3(20,.5f,20)),at(0,-50,0),.7f,category);
      auto object=at(0,100,0);
      object.rotateOZ(90.f);
      std::vector<Tempest::Matrix4x4> pose;
      for(const auto& bone:skeleton.tr) pose.push_back(object*bone);
      auto body=world.createRagdoll(skeleton,zones,pose.data(),pose.size(),object,{0,-600,0});
      require(body!=nullptr && body->partCount()>1,"body impact fixture must create multiple physical parts");
      require(!body->impactBone(0),"an unphysical root must not count as a torso contact");
      for(size_t bone=1;bone<4;++bone)
        require(body->impactBone(bone)==!limbs,"only pelvis, spine and head may supply impact speed");
      float maximum=0;
      for(int frame=0;frame<90;++frame) {
        world.tick(17);
        maximum=std::max(maximum,body->takeImpactSpeed());
        requireNear(body->takeImpactSpeed(),0.f,.000001f,"reading a native body's impact speed must consume the contact sample");
        }
      return maximum;
      };
    const float torso=impact(false,DynamicWorld::C_Landscape);
    require(torso>.6f && torso<1.5f,"a native torso landing must report incoming normal speed in centimetres per millisecond");
    requireNear(impact(true,DynamicWorld::C_Landscape),0.f,.000001f,"native arm contacts must never supply body impact damage");
    require(impact(false,DynamicWorld::C_Object)>.6f,"a native torso must report impacts against static objects");
    require(impact(false,DynamicWorld::C_MovableVob)>.6f,"a native torso must report impacts against furniture");
    requireNear(impact(false,DynamicWorld::C_Item),0.f,.000001f,"item contacts must not supply body impact damage");
    requireNear(impact(false,DynamicWorld::C_Landscape,false),0.f,.000001f,
                "body impact OFF must suppress real native torso contact samples");
    std::printf("Body impact damage: 300 ms maximum window; unconscious/dead/IMMORTAL/OFF exclusions; "
                "native torso %.6f cm/ms, limbs ignored, landscape/object/furniture categories passed\n",double(torso));
    }

  void testOffCenterImpulse() {
    CollisionWorld world;
    auto centered=world.addDynamicBody(new JPH::BoxShape(JPH::Vec3::sReplicate(.5f)),at(0,1000,0),.7f,2.f);
    auto tipped=world.addDynamicBody(new JPH::BoxShape(JPH::Vec3::sReplicate(.5f)),at(300,1000,0),.7f,2.f);
    require(centered->addImpulse({2,0,0}),"centre-of-mass fixture must accept an impulse");
    require(tipped->addImpulseAtPoint({2,0,0},{300,1050,0}),"off-centre fixture must accept an impulse at a world point");
    require(!tipped->addImpulseAtPoint({2,0,0},{0,std::numeric_limits<float>::infinity(),0}),
            "a nonfinite application point must be rejected");
    world.tick(17);
    const auto straight=centered->matrix(),rotated=tipped->matrix();
    requireNear(straight.at(0,1),0.f,.00001f,"centre impulse must preserve the cube's orientation");
    require(std::abs(rotated.at(0,1))>.01f,"an impulse above the centre of mass must rotate the body");
    requireNear(rotated.at(3,0)-straight.at(3,0),300.f,.001f,
                "the application point changes angular response while preserving linear impulse");
    }
  }
