#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#include "physics_contract_common.h"
#include "physics/collisionworld.h"
#include "physics/physicvbo.h"
#include "graphics/mesh/protomesh.h"
#include "graphics/mesh/hitzones.h"
#include "graphics/mesh/submesh/packedmesh.h"
#include "game/serialize.h"
#include "world/objects/npc.h"
#include "world/bloodstains.h"
#include "gothic.h"

#include <Tempest/MemReader>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace PhysicsContract;

namespace {
  void testBloodStains() {
    using namespace BloodStains;
    using Tempest::Vec3;
    require(Gothic::PhysicsFlags{}.blood,"blood must default to ON");
    require(Limit==256,"a world keeps 256 stains");

    // Lifetime: outdoors one game day, indoors three.
    Stain out;
    out.bornAt = 5*MinuteMs;
    require(!expired(out,out.bornAt+LifetimeOutMs-1),"an outdoor stain lives just under a game day");
    require(expired(out,out.bornAt+24*60*MinuteMs),"an outdoor stain is gone after 24 game hours");
    Stain in = out;
    in.indoor = true;
    require(!expired(in,in.bornAt+24*60*MinuteMs),"an indoor stain outlives one game day");
    require(!expired(in,in.bornAt+LifetimeInMs-1),"an indoor stain lives just under three game days");
    require(expired(in,in.bornAt+3*24*60*MinuteMs),"an indoor stain is gone after three game days");

    // Evidence: people's and beasts' blood, a meatbug's, a zombie's; never a golem's stone chips.
    for(auto b:{Blood::Red,Blood::Yellow,Blood::Gore}) {
      Stain s; s.blood = b;
      require(isEvidence(s),"red, yellow and gore blood are evidence");
      }
    {
    Stain s; s.blood = Blood::Stone;
    require(!isEvidence(s),"stone chips are not evidence");
    }

    // Adding: ids never reused or zero, the birth stamped from the clock, nothing with blood off.
    const auto noBody   = [](uint32_t) { return false; };
    const auto body7    = [](uint32_t id) { return id==7; };
    {
    std::vector<Stain> stains;
    uint32_t next = 1;
    require(add(stains,next,Stain{},1000,false,noBody)==0 && stains.empty() && next==1,
            "blood OFF: addBloodStain adds nothing and returns 0");
    const auto first = add(stains,next,Stain{},1000,true,noBody);
    require(first==1 && stains.back().bornAt==1000,"a new stain takes id 1 and the current game time");
    Stain born; born.bornAt = 400;
    require(add(stains,next,born,1000,true,noBody)==2 && stains.back().bornAt==400,"a stain born earlier keeps its time");
    stains.clear();
    require(add(stains,next,Stain{},2000,true,noBody)==3,"ids are never reused, even after the stains are gone");
    }

    // Eviction at the limit: the oldest goes; a pool whose body is still in the world goes last.
    {
    std::vector<Stain> stains;
    uint32_t next = 1;
    Stain pool; pool.kind = Kind::Pool; pool.bodyId = 7; pool.bornAt = 1;
    const auto poolId = add(stains,next,pool,1,true,body7);
    for(int64_t i=0;stains.size()<Limit;++i) {
      Stain s; s.bornAt = 10+i;
      add(stains,next,s,10+i,true,body7);
      }
    require(evictIndex(stains,body7)==1,"the oldest stain but a living body's pool leaves first");
    Stain extra; extra.bornAt = 100000;
    add(stains,next,extra,100000,true,body7);
    require(stains.size()==Limit && stains.front().id==poolId && stains[1].bornAt==11,
            "a full world drops its oldest splash, not the pool of a body still there");
    require(evictIndex(stains,noBody)==0,"once the body has left, its pool is the oldest stain and leaves first");
    std::vector<Stain> pools(3,pool);
    pools[0].bornAt = 30; pools[1].bornAt = 20; pools[2].bornAt = 25;
    for(uint32_t i=0;i<3;++i) pools[i].id = i+1;
    require(evictIndex(pools,body7)==1,"among pools of bodies still there, the oldest leaves");
    require(evictIndex(std::vector<Stain>{},noBody)==0,"nothing to evict in an empty world");
    }

    // The minute sweep: past lifetime, or a pool whose body left; blood off clears all.
    {
    std::vector<Stain> stains(4);
    stains[0].id = 1; stains[0].bornAt = 0;                         // outdoor, a day old
    stains[1].id = 2; stains[1].bornAt = 0; stains[1].indoor = true; // indoor, a day old
    stains[2].id = 3; stains[2].bornAt = 60*MinuteMs; stains[2].kind = Kind::Pool; stains[2].bodyId = 7;
    stains[3].id = 4; stains[3].bornAt = 60*MinuteMs; stains[3].kind = Kind::Pool; stains[3].bodyId = 9;
    auto kept = stains;
    sweep(kept,LifetimeOutMs,true,body7);
    require(kept.size()==2 && kept[0].id==2 && kept[1].id==3,
            "the sweep takes the day-old outdoor stain and the pool of a body that left, keeps the rest");
    kept = stains;
    sweep(kept,0,false,body7);
    require(kept.empty(),"blood OFF leaves no stain in the world");
    }

    // Grouping into `blood` traces: single linkage within 2 m, evidence only.
    {
    std::vector<Stain> stains(7);
    const auto at = [&](size_t i, Vec3 p, float r, int64_t born) {
      stains[i].id = uint32_t(i+1); stains[i].pos = p; stains[i].radiusCm = r; stains[i].bornAt = born;
      };
    at(0,{0,0,0},10,500);    // chain: 0 - 1 - 2, each link under 2 m, the ends 3.8 m apart
    at(1,{190,0,0},30,300);
    at(2,{380,0,0},15,900);
    at(3,{1000,0,0},20,100); // alone
    at(4,{1150,0,0},40,50);  // stone chips 1.5 m from it: no evidence, no link
    stains[4].blood = Blood::Stone;
    at(5,{3000,0,0},50,700); // a body's pool and a splash beside it
    stains[5].kind = Kind::Pool; stains[5].bodyId = 7;
    at(6,{3100,0,0},10,800);
    const auto groups = evidenceGroups(stains,body7);
    require(groups.size()==3,"three evidence groups: the chain, the lone splash, the body's blood");
    require(groups[0].members==std::vector<size_t>{0,1,2},"single linkage joins a chain of stains under 2 m apart");
    require(groups[0].oldest==1 && groups[0].largest==1 && groups[0].freshest==2,
            "a group is named by its oldest stain, placed at its largest, aged by its freshest");
    require(groups[1].members==std::vector<size_t>{3},"stone chips never join or form a trace");
    require(groups[2].heldByBody,"a group with the pool of a body still there is that body's");
    const auto traces = traceGroups(stains,true,body7);
    require(traces.size()==2,"the body's blood gives no second find");
    require(traceGroups(stains,true,noBody).size()==3,"once the body is gone, its blood is a trace of its own");
    require(traceGroups(stains,false,noBody).empty(),"blood OFF: no blood traces");
    std::vector<Stain> far(2);
    far[0].pos = {0,0,0}; far[1].pos = {200.5f,0,0};
    require(evidenceGroups(far,noBody).size()==2,"stains farther than 2 m are separate traces");
    far[1].pos = {200,0,0};
    require(evidenceGroups(far,noBody).size()==1,"stains exactly 2 m apart are one trace");
    }

    // Sight points: the centre and four rim points, 5 cm off the surface along its normal.
    {
    Stain s;
    s.pos = {100,50,-20}; s.radiusCm = 30; s.normal = {0,1,0}; s.dir = {1,0,0};
    const auto p = sightPoints(s);
    requireNear(p[0].y,55.f,1e-3f,"the centre sight point is 5 cm above the stain");
    for(size_t i=1;i<p.size();++i) {
      requireNear((p[i]-p[0]).length(),30.f,1e-3f,"rim sight points lie on the radius");
      requireNear(p[i].y,55.f,1e-3f,"rim sight points are lifted with the centre");
      }
    Stain wall = s;
    wall.normal = {0,0,-2}; wall.dir = {0,0,1}; // a wall splash whose direction runs into the wall
    const auto w = sightPoints(wall);
    requireNear(w[0].z,-25.f,1e-3f,"a wall stain's sight points stand off the wall along its normal");
    for(size_t i=1;i<w.size();++i)
      requireNear(w[i].z,-25.f,1e-3f,"a wall stain's rim lies in the wall's plane");
    }
    }

  void testSkinnedMeshMinimum() {
    Resources resources;
    require(!Resources::hasGraphicsDevice(),"skinned minimum geometry contracts must run without a GPU device");
    asset::ModelMesh model;
    model.meshes.resize(1);
    auto& skin=model.meshes[0];
    skin.mesh.positions={{0,10,0},{10,-10,0},{0,20,10},{0,-10000,0}};
    skin.mesh.bbox={{0,-10000,0},{10,20,10}};
    skin.mesh.sub_meshes.resize(1);
    auto& surface=skin.mesh.sub_meshes[0];
    surface.mat.group=asset::MaterialGroup::WOOD;
    for(uint16_t i=0;i<skin.mesh.positions.size();++i)
      surface.wedges.push_back({{.6f,-.48f,.64f},{0,0},i});
    surface.triangles={{{0,1,2}}};
    skin.weights={{{.4f,{0,10,0},0},{.3f,{0,10,0},1},{.2f,{0,10,0},2},{.1f,{0,10,0},3}},
                  {{1.f,{10,-10,0},0}},{{1.f,{0,20,10},0}},{{1.f,{0,-10000,0},0}}};
    const PackedMesh packed(skin);
    require(packed.verticesA.size()>3,"the skin fixture must contain actual meshlet vertex padding");
    ProtoMesh mesh(model,nullptr,"CONTRACT_SKIN.MDM");
    const std::array<Tempest::Matrix4x4,4> localBones={at(0,20,0),at(0,-40,0),at(0,-20,0),at(0,-30,0)};
    Tempest::Vec3 minimum;
    require(mesh.minSkinnedVertex(localBones.data(),localBones.size(),at(0,0,0),0,minimum),
            "an indexed headless soft-skin mesh must expose its posed minimum");
    // The four heights of the first vertex are 30, -30, -10 and -20 cm.
    // Their weighted height is -1; dominant-bone sampling instead selects the
    // second vertex at 10 cm and misses the actual lowest rendered vertex.
    requireNear(minimum.x,0,.0001f,"four-bone minimum must retain the selected vertex's X");
    requireNear(minimum.y,-1,.0001f,"all four weighted bones must determine the visible minimum");
    requireNear(minimum.z,0,.0001f,"four-bone minimum must retain the selected vertex's Z");

    auto object=at(7,100,-11);
    object.rotateOY(90);
    object.scale(2,3,4);
    std::array<Tempest::Matrix4x4,4> worldBones;
    for(size_t i=0;i<worldBones.size();++i) worldBones[i]=object*localBones[i];
    require(mesh.minSkinnedVertex(worldBones.data(),worldBones.size(),object,4,minimum),
            "the minimum must accept an already world-transformed palette and nonuniform object scale");
    // Rotation and scale put the weighted point at (7,97,-11). The unit
    // source normal becomes (2.56,-1.44,-1.2); shader fatness 4 adds twice it.
    requireNear(minimum.x,12.12f,.0001f,"fatness must use the full rotated and scaled source normal");
    requireNear(minimum.y,94.12f,.0001f,
                "unindexed vertices and meshlet padding must not lower the rendered skin minimum");
    requireNear(minimum.z,-13.4f,.0001f,"object translation must not be applied twice to skinned positions");
    Tempest::Vec3 repeated;
    require(mesh.minSkinnedVertex(worldBones.data(),worldBones.size(),object,4,repeated),
            "the same posed geometry must remain queryable without consuming or changing the cache");
    requireNear((repeated-minimum).length(),0,.0001f,"repeated minimum queries must not accumulate an offset");

    const Tempest::Vec3 sentinel(123,456,789);
    minimum=sentinel;
    require(!mesh.minSkinnedVertex(worldBones.data(),3,object,4,minimum),
            "a missing weighted bone must reject the palette instead of approximating the mesh");
    requireNear((minimum-sentinel).length(),0,0,"a rejected minimum query must leave its output unchanged");
    ProtoMesh empty(asset::ModelMesh{},nullptr,"CONTRACT_EMPTY_SKIN.MDM");
    require(!empty.minSkinnedVertex(worldBones.data(),worldBones.size(),object,4,minimum),
            "a model without rendered skin geometry must have no skinned minimum");
    requireNear((minimum-sentinel).length(),0,0,"an absent skin must leave the minimum output unchanged");
    }

  void testContracts() {
    {
      using MeleeImpact::BackstabKind;
      requireNear(MeleeImpact::backstabAngle(0.f,119.9f),119.9f,.0001f,
                  "backstab angles must retain the horizontal sector boundary");
      requireNear(MeleeImpact::backstabAngle(350.f,110.f),120.f,.0001f,
                  "backstab angles must wrap across zero degrees");
      require(MeleeImpact::backstabKind(119.9f,1,false,false,false)==BackstabKind::None,
              "an unaware target at 119.9 degrees must not receive a backstab");
      require(MeleeImpact::backstabKind(120.f,1,false,false,false)==BackstabKind::Unaware,
              "an unaware target at 120 degrees must receive a backstab");
      require(MeleeImpact::backstabKind(180.f,0,false,false,false)==BackstabKind::None,
              "an unaware backstab must require the sneak talent");
      require(MeleeImpact::backstabKind(180.f,0,false,true,true)==BackstabKind::Stunned,
              "a rear hit on a stunned target must qualify without sneak or surprise");
      require(MeleeImpact::backstabKind(0.f,1,true,false,false)==BackstabKind::Sleeping,
              "a sleeping target must allow a frontal backstab with the sneak talent");
      require(MeleeImpact::backstabKind(0.f,0,true,false,false)==BackstabKind::None,
              "a sleeping backstab must still require the sneak talent");
      require(MeleeImpact::backstabKind(0.f,0,false,true,true)==BackstabKind::None,
              "a frontal hit on a stunned target must not qualify as a backstab");
      require(MeleeImpact::backstabKind(180.f,1,false,false,true)==BackstabKind::None,
              "combat or a recent hit must prevent an unaware backstab");
      require(MeleeImpact::backstabRecentHit(10900,1000),
              "a hit 9.9 seconds ago must keep the target aware of its attacker");
      require(!MeleeImpact::backstabRecentHit(11000,1000),
              "a hit exactly 10 seconds ago must no longer keep the target aware");
      requireNear(MeleeImpact::backstabMultiplier(BackstabKind::Unaware,false,1.f),3.f,.0001f,
                  "an unaware backstab must triple final melee damage");
      requireNear(MeleeImpact::backstabMultiplier(BackstabKind::Unaware,true,1.f),1.5f,.0001f,
                  "a huge beast must receive only a 1.5 backstab multiplier");
      requireNear(MeleeImpact::backstabMultiplier(BackstabKind::Stunned,false,1.5f),3.f,.0001f,
                  "a stunned backstab must replace rather than multiply the stun bonus");
      requireNear(MeleeImpact::backstabMultiplier(BackstabKind::None,false,1.5f),1.5f,.0001f,
                  "an ordinary hit must retain the existing stun bonus");
      }

    {
      using MeleeImpact::Weapon;
      const auto fist=MeleeImpact::impulse(Weapon::Fist,{600,800,0},90);
      requireNear(fist.x,9.f,.0001f,"fist impulse must convert world cm/s into N s");
      requireNear(fist.y,12.f,.0001f,"impulse must follow the contact velocity direction");
      requireNear(MeleeImpact::impulse(Weapon::OneHand,{1000,0,0},0).length(),12.5f,.0001f,
                  "one-hand impulse must use 2.5 kg and the glancing edge factor");
      requireNear(MeleeImpact::impulse(Weapon::TwoHand,{1000,0,0},30).length(),30.f,.0001f,
                  "two-hand impulse must use 4 kg and the sine of the blade angle");
      const auto capped=MeleeImpact::impulse(Weapon::TwoHand,{-6000,0,8000},90);
      requireNear(capped.length(),60.f,.0001f,"melee impulse magnitude must be capped at 60 N s");
      requireNear(capped.x,-36.f,.0001f,"the impulse cap must retain contact direction");
      requireNear(MeleeImpact::impulse(Weapon::Fist,{},90).length(),0.f,.0001f,
                  "a motionless blade must not add an impulse");

      MeleeImpact::Posture posture;
      require(!posture.hit(30,75,1000),"a quarter-threshold impulse must not knock down");
      requireNear(posture.value,25.f,.0001f,"posture must accumulate in units of body-mass-scaled impulse");
      posture.tick(2500,1500);
      requireNear(posture.value,25.f,.0001f,"posture must retain the full 1500 ms recovery delay");
      posture.tick(2750,500);
      requireNear(posture.value,17.5f,.0001f,"recovery must count only time after the hit grace period");
      posture.tick(3750,1000);
      requireNear(posture.value,0.f,.0001f,"posture recovery must stop at zero");
      require(!posture.hit(60,75,4000),"a full capped impulse must add only half the 75 kg knockdown threshold");
      requireNear(posture.value,50.f,.0001f,"a 75 kg body must require 120 N s for knockdown");
      require(!posture.hit(59,75,4000),"posture just below 100 must not knock down");
      require(posture.hit(1,75,4000),"posture must knock down exactly at 100");
      requireNear(posture.value,100.f,.0001f,"posture must stop at 100");
      posture.recover(5000);
      requireNear(posture.value,0.f,.0001f,"getting up must reset posture");
      require(!posture.hit(60,75,5000),"getting up must protect against another knockdown");
      require(!posture.hit(60,75,5000),"successive hits during immunity must not knock down");
      requireNear(posture.value,60.f,.0001f,"post-rise immunity must cap accumulating posture at 60");
      require(!posture.hit(60,75,8999),"post-rise immunity must cover the first 3999 ms");
      require(posture.hit(48,75,9000),"post-rise immunity must expire at 4000 ms");
      posture.reset();
      require(!posture.hit(60,150,10000),"a doubled body mass must double the knockdown impulse");
      requireNear(posture.value,25.f,.0001f,"a 150 kg body must require 240 N s for knockdown");
      require(!posture.hit(60,150,10000),"two full capped impulses must not knock down a 150 kg body");
      require(!posture.hit(60,150,10000),"three full capped impulses must not knock down a 150 kg body");
      require(posture.hit(60,150,10000),"four full capped impulses must knock down a 150 kg body");
      requireNear(MeleeImpact::skillThresholdScale(60,60,60),1.248f,.0001f,
                  "two 60% masters of strength 60 must need 1.248 times the base knockdown impulse");
      require(MeleeImpact::skillThresholdScale(100,100,30)<MeleeImpact::skillThresholdScale(60,60,30),
              "a trained strong attacker must knock down sooner");
      require(MeleeImpact::skillThresholdScale(30,40,100)>MeleeImpact::skillThresholdScale(30,40,10),
              "a trained defender must stand longer");
      requireNear(MeleeImpact::skillThresholdScale(0,0,0),1.2f*.8f/.58f,.0001f,"an untrained weak attacker keeps a floor of 0.58");
      requireNear(MeleeImpact::skillThresholdScale(200,500,200),1.2f*1.2f/1.4f,.0001f,"skill inputs must be clamped");
      MeleeImpact::Posture skilled;
      require(!skilled.hit(60,75,20000,1.248f),"a full capped impulse must fill 40% of a 60% masters' threshold");
      requireNear(skilled.value,100.f*60.f/(120.f*1.248f),.0001f,"the skill scale must multiply the body-mass threshold");
      }
    CollisionWorld world;
    auto ground = world.addCollisionBody(new JPH::BoxShape(JPH::Vec3(20,.5f,20)),at(0,-50,0),
                                         .75f,DynamicWorld::C_Landscape);

    // The game supplies centimeters; shapes are expressed in meters.
    const auto hit = world.rayCast({0,100,0},{0,-100,0},landscapeMask);
    require(hit.hasHit,"ground ray must hit");
    requireNear(hit.point.y,0,.001f,"ground ray must report centimeters");
    requireNear(hit.normal.y,1,.001f,"ground ray normal must face upward");

    // Overlap alone must not block a sphere moving away from the surface.
    const auto departing = world.raySweep({0,10,0},{0,1000,0},50,landscapeMask);
    require(!departing.hasHit,"separating sphere overlap must not report a hit");
    const auto approaching = world.raySweep({0,10,0},{0,-1000,0},50,landscapeMask);
    require(approaching.hasHit,"approaching sphere overlap must report a hit");
    requireNear(approaching.point.y,-40,.01f,"overlap sweep must project the sphere bottom onto the game ray");

    // NPC movement consumes the overlap normal, so its orientation is gameplay behavior.
    Tempest::Vec3 normal;
    auto* staleVob = reinterpret_cast<Interactive*>(ground.get());
    Interactive* vob = staleVob;
    JPH::CapsuleShape capsule(.1f,.1f);
    require(world.hasCollision(&capsule,{0,5,0},normal,vob),"NPC capsule must overlap the ground");
    require(normal.y>.9f,"NPC overlap normal must face away from the ground");
    require(vob==nullptr,"terrain contact must clear a previous interactive pointer");
    vob = staleVob;
    require(!world.hasCollision(&capsule,{0,10000,0},normal,vob),"distant NPC query must miss the terrain");
    require(vob==staleVob,"missed NPC query must preserve the caller's interactive pointer");

    auto sensor = world.addCollisionBody(new JPH::BoxShape(JPH::Vec3(1,.1f,1)),at(0,500,0),0,
                                         DynamicWorld::C_Ghost,false,true);
    require(!world.rayCast({0,700,0},{0,300,0},landscapeMask).hasHit,
            "trigger sensor must be excluded from landscape rays");
    const auto sensorHit = world.rayCast({0,700,0},{0,300,0},sensorMask);
    require(sensorHit.hasHit,"trigger sensor must remain explicitly queryable");
    requireNear(sensorHit.point.y,510,.01f,"sensor query must report its surface in centimeters");

    auto matrix = at(0,700,0);
    matrix.scale(2,3,4);
    auto item = world.addDynamicBody(new JPH::BoxShape(JPH::Vec3(.15f,.15f,.15f)),matrix,.4f,1);
    world.tick(1000);
    // At most two 1/60 s updates may run after a long frame. Semi-implicit integration
    // moves the box by g * step^2 + 2 * g * step^2 on those two updates.
    const float step = 1.f/60.f;
    const float expectedY = 700.f-3.f*DynamicWorld::gravityMS*100.f*step*step;
    requireNear(item->position().y,expectedY,.01f,"long frame must simulate two fixed steps");
    const auto itemMatrix = item->matrix();
    requireNear(itemMatrix.at(0,0),2,.001f,"dynamic body must retain its x scale");
    requireNear(itemMatrix.at(1,1),3,.001f,"dynamic body must retain its y scale");
    requireNear(itemMatrix.at(2,2),4,.001f,"dynamic body must retain its z scale");

    for(int i=0;i<300;++i)
      world.tick(17);
    require(item->position().y>30 && item->position().y<50,
            "dropped box must fall through the sensor and settle on the ground");

    // Animated models use kinematic triangle meshes, whose mass cannot be inferred.
    JPH::TriangleList triangles;
    triangles.emplace_back(JPH::Float3(-1,0,-1),JPH::Float3(0,0,1),JPH::Float3(1,0,-1));
    const auto mesh = JPH::MeshShapeSettings(triangles).Create();
    require(!mesh.HasError(),"kinematic test mesh must be constructible");
    auto moving = world.addCollisionBody(mesh.Get(),at(300,100,0),.4f,DynamicWorld::C_Object,true);
    require(world.rayCast({300,200,0},{300,0,0},objectMask).hasHit,
            "kinematic mesh must participate in object queries");
    moving->setObjMatrix(at(300,200,0));
    const auto moved = world.rayCast({300,300,0},{300,0,0},objectMask);
    require(moved.hasHit,"moved kinematic mesh must remain queryable");
    requireNear(moved.point.y,200,.01f,"kinematic move must update the broad phase immediately");


    // Sector counts exceed Jolt's native 32 mesh materials. Triangle user data must
    // survive BVH reordering and the compound children used for large landscapes.
    std::vector<JPH::Vec3> vertices;
    std::vector<uint32_t> indices;
    std::vector<std::string> sectors(40);
    for(size_t i=0;i<sectors.size();++i) {
      const float x = float(i)*10.f;
      vertices.emplace_back(x-1,0,-1);
      vertices.emplace_back(x+1,0,-1);
      vertices.emplace_back(x,0,1);
      indices.push_back(uint32_t(i*3));
      indices.push_back(uint32_t(i*3+1));
      indices.push_back(uint32_t(i*3+2));
      sectors[i] = "sector-"+std::to_string(i);
      }
    PhysicVbo metadataMesh(&vertices);
    for(size_t i=0;i<sectors.size();++i) {
      const auto material = i%2==0 ? asset::MaterialGroup::STONE : asset::MaterialGroup::WOOD;
      metadataMesh.addIndex(indices,i*3,3,material,sectors[i].c_str());
      }
    auto metadataBody = world.addCollisionBody(metadataMesh.shape(),at(500,-1000,0),.75f,
                                               DynamicWorld::C_Landscape,false,false,&metadataMesh);
    for(size_t i=0;i<sectors.size();++i) {
      const float x = 500.f+float(i)*1000.f;
      const auto triangleHit = world.rayCast({x,-900,0},{x,-1100,0},landscapeMask);
      require(triangleHit.hasHit,"translated landscape triangle must be queryable");
      requireNear(triangleHit.point.y,-1000,.01f,"translated compound mesh must preserve its shape origin");
      const auto material = i%2==0 ? asset::MaterialGroup::STONE : asset::MaterialGroup::WOOD;
      require(triangleHit.material==material,"mesh hit must retain its Gothic material");
      require(triangleHit.sector!=nullptr && sectors[i]==triangleHit.sector,
              "mesh hit must retain its sector beyond 32 segments");
      }

    {
      std::vector<HitZones::Posed> first{{{0,80,0},{0,140,0},10.f,HitZones::Body,0}};
      std::vector<HitZones::Posed> second{{{60,90,0},{60,150,0},12.f,HitZones::Body,0}};
      requireNear(HitZones::horizontalBodyGap(first,second),38.f,.0001f,
                  "torso gap must subtract both posed capsule radii");
      requireNear(HitZones::horizontalBodyGap(first,second,{20,0,0}),18.f,.0001f,
                  "a bounded forward feed must retain the eighteen-centimetre torso clearance");
      requireNear(HitZones::horizontalBodyGap(first,second,{45,0,0}),-7.f,.0001f,
                  "overlapping torso capsules must report negative clearance");
      first.push_back({{60,80,0},{60,140,0},100.f,HitZones::Head,1});
      second.push_back({{0,90,0},{0,150,0},100.f,HitZones::Limb,1});
      requireNear(HitZones::horizontalBodyGap(first,second),38.f,.0001f,
                  "head and limb contacts must not replace the torso safety gap");
      first={{{-10,80,-10},{10,140,10},2.f,HitZones::Body,0}};
      second={{{-10,400,10},{10,460,-10},3.f,HitZones::Body,0}};
      requireNear(HitZones::horizontalBodyGap(first,second),-5.f,.0001f,
                  "crossed torso axes must overlap in the horizontal projection despite different heights");
      requireNear(HitZones::horizontalBodyGap(first,second,{-1,0,0}),-5.f,.0001f,
                  "a retreat can begin on a flat projected-axis overlap without worsening the clearance");
      require(HitZones::horizontalBodyGap(first,second,{-25,0,0})>-5.f,
              "continued retreat must be able to leave the projected-axis overlap plateau");
      }

    require(Npc::skipMeleeAlly(false,false,true,false,true),
            "an incidental same-guild contact must skip NPC damage even with hostile attitude");
    require(Npc::skipMeleeAlly(false,false,false,true,true),
            "an incidental party contact must skip NPC damage even with hostile attitude");
    require(Npc::skipMeleeAlly(false,false,false,false,false),
            "an incidental nonhostile contact must skip NPC damage");
    require(!Npc::skipMeleeAlly(false,false,false,false,true),
            "an incidental enemy contact must receive NPC damage");
    require(!Npc::skipMeleeAlly(true,false,true,true,false),
            "the hero must damage incidental allies");
    require(!Npc::skipMeleeAlly(false,true,false,false,false),
            "the selected neutral or angry target must receive NPC damage");
    require(!Npc::skipMeleeAlly(false,true,true,false,false),
            "the selected same-guild target must receive NPC damage");
    require(!Npc::skipMeleeAlly(false,true,false,true,false),
            "the selected party target must receive NPC damage");

    require(Npc::canAccumulateMeleePosture(true,false,false,false),"an enabled live human hit must accumulate posture");
    require(!Npc::canAccumulateMeleePosture(false,false,false,false),"posture OFF must retain the old hit path");
    require(!Npc::canAccumulateMeleePosture(true,true,false,false),"a blocked hit must leave posture unchanged");
    require(!Npc::canAccumulateMeleePosture(true,false,true,false),"a knockdown must not accumulate another knockdown");
    require(!Npc::canAccumulateMeleePosture(true,false,false,true),"death or unconsciousness must retain the old path");

    require(MeleeImpact::beastSize(54.9f)==MeleeImpact::BeastSize::Small,
            "beasts below 55 kg must be small");
    require(MeleeImpact::beastSize(55.f)==MeleeImpact::BeastSize::Medium &&
            MeleeImpact::beastSize(299.9f)==MeleeImpact::BeastSize::Medium,
            "beasts from 55 kg to below 300 kg must be medium");
    require(MeleeImpact::beastSize(300.f)==MeleeImpact::BeastSize::Huge,
            "beasts from 300 kg must be huge");
    require(MeleeImpact::beastCanAccumulatePosture(54.9f) && MeleeImpact::beastCanAccumulatePosture(299.9f) &&
            !MeleeImpact::beastCanAccumulatePosture(300.f) && !MeleeImpact::beastCanAccumulatePosture(600.f),
            "small and medium beasts must accumulate posture while huge beasts never do");
    requireNear(Ragdoll::speciesMass("Shadow"),250.f,.0001f,"the Shadow model must weigh 250 kg");
    requireNear(Ragdoll::speciesMass("sHaDoW"),250.f,.0001f,"Shadow mass must be case insensitive");
    requireNear(Ragdoll::speciesMass("Golem"),500.f,.0001f,"the Golem model must weigh 500 kg");
    requireNear(Ragdoll::speciesMass("gOlEm"),500.f,.0001f,"Golem mass must be case insensitive");

    requireNear(MeleeImpact::beastImpulse(40.f),20.f,.0001f,"a 40 kg beast must strike with 20 N s");
    requireNear(MeleeImpact::beastImpulse(90.f),45.f,.0001f,"a 90 kg beast must strike with 45 N s");
    requireNear(MeleeImpact::beastImpulse(250.f),60.f,.0001f,"a 250 kg beast impulse must cap at 60 N s");
    requireNear(MeleeImpact::beastSkillThresholdScale(0),.96f,.0001f,
                "beast attacks must use a neutral attacker scale against an unskilled defender");
    requireNear(MeleeImpact::beastSkillThresholdScale(100),1.44f,.0001f,
                "beast attacks must retain the skilled defender threshold multiplier");
    {
      const auto threshold = MeleeImpact::beastSkillThresholdScale(0);
      const auto hugeHit = MeleeImpact::postureIncrease(60.f,75.f,threshold);
      const auto mediumHit = MeleeImpact::postureIncrease(45.f,75.f,threshold);
      require(hugeHit>=40.f && mediumHit<40.f,
              "60 N s must cross the heavy posture threshold against 75 kg while 45 N s must not");
      require(MeleeImpact::heavyHit(MeleeImpact::Weapon::Fist,{0,0,0},hugeHit)==MeleeImpact::HeavyHit::Posture &&
              MeleeImpact::heavyHit(MeleeImpact::Weapon::Fist,{0,0,0},mediumHit)==MeleeImpact::HeavyHit::None,
              "unarmed beast hits must qualify as heavy only through their posture increase");
      }

    require(MeleeImpact::heavyHit(MeleeImpact::Weapon::TwoHand,{0,0,0},0)==MeleeImpact::HeavyHit::TwoHanded,
            "a two-handed weapon must qualify as a heavy hit");
    require(MeleeImpact::heavyHit(MeleeImpact::Weapon::OneHand,{80,-60,0},0)==MeleeImpact::HeavyHit::Downward,
            "a downward velocity fraction of 0.6 must qualify as a heavy hit");
    require(MeleeImpact::heavyHit(MeleeImpact::Weapon::OneHand,{std::sqrt(10000.f-59.f*59.f),-59,0},0)==MeleeImpact::HeavyHit::None,
            "a downward velocity fraction of 0.59 must not qualify as a heavy hit");
    require(MeleeImpact::heavyHit(MeleeImpact::Weapon::OneHand,{0,0,0},40.f)==MeleeImpact::HeavyHit::Posture,
            "a posture increase of 40 must qualify as a heavy hit");
    require(MeleeImpact::heavyHit(MeleeImpact::Weapon::OneHand,{0,0,0},39.9f)==MeleeImpact::HeavyHit::None,
            "a posture increase of 39.9 must not qualify as a heavy hit");
    require(MeleeImpact::armorClass(39)==MeleeImpact::ArmorClass::Light,
            "edge protection below 40 must be light armor");
    require(MeleeImpact::armorClass(40)==MeleeImpact::ArmorClass::Medium &&
            MeleeImpact::armorClass(79)==MeleeImpact::ArmorClass::Medium,
            "edge protection from 40 through 79 must be medium armor");
    require(MeleeImpact::armorClass(80)==MeleeImpact::ArmorClass::Heavy,
            "edge protection from 80 must be heavy armor");
    requireNear(MeleeImpact::armorThresholdScale(MeleeImpact::armorClass(0)),1.f,.0001f,
                "no armor must retain the posture threshold");
    requireNear(MeleeImpact::armorThresholdScale(MeleeImpact::ArmorClass::Light),1.f,.0001f,
                "light armor must retain the posture threshold");
    requireNear(MeleeImpact::armorThresholdScale(MeleeImpact::ArmorClass::Medium),1.15f,.0001f,
                "medium armor must multiply the posture threshold by 1.15");
    requireNear(MeleeImpact::armorThresholdScale(MeleeImpact::ArmorClass::Heavy),1.3f,.0001f,
                "heavy armor must multiply the posture threshold by 1.3");
    require(MeleeImpact::stunDuration("parried")==1000 && MeleeImpact::stunDuration("guard_break")==1000 &&
            MeleeImpact::stunDuration("posture")==1500,
            "parry and guard-break stuns must last 1000 ms and posture stuns 1500 ms");

    {
      // Tip heights of T_2HSFINISH and T_1HSFINISH at 30 fps, sampled in the game: the stroke down
      // starts at the top it falls from (not the higher wind-up before the swing round the side)
      // and ends where the tip rests at its lowest; the axe's old window closed 56 cm above the ground.
      const std::vector<float> axe = {164,177,184,188,188,185,182,184,185,177,163,160,175,156,118,109,131,158,
                                      183,193,193,184,168,146,122,99,78,56,38,25,20,19,19,18,19,19};
      const std::vector<float> sword = {28,38,56,91,140,182,200,205,207,198,181,178,197,169,115,103,130,162,
                                        189,198,196,185,164,136,109,86,62,39,21,8,3,3,2,3,3,4};
      const auto axeStroke = Npc::meleeFinishStroke(axe), swordStroke = Npc::meleeFinishStroke(sword);
      require(axeStroke && axeStroke->first==19 && axeStroke->second==30,"the axe's chop must be windowed on its stroke down");
      require(swordStroke && swordStroke->first==19 && swordStroke->second==30,"a higher wind-up must not start the chop's stroke");
      require(!Npc::meleeFinishStroke({50,50,50,50}),"a blade that never drops has no stroke");
      }

    {
      // t_1hAttackR of the master is frames 21..40 of the ASC whose 1..19 are t_1hAttackL, with L's tags:
      // its DEF_OPT_FRAME counts from its own start (read as a frame of the file, it clamped to 0)
      Animation::AnimData left,right;
      left.firstFrame = 1;  left.lastFrame = 19;  left.numFrames = 19;
      right.firstFrame = 21; right.lastFrame = 40; right.numFrames = 20;
      require(left.optimalFrame(3)==2 && right.optimalFrame(3)==2,"a side swing's optimal frame must count from its own start");
      require(right.optimalFrame(25)==4,"a frame of the file must stay that frame");
      require(right.optimalFrame(60)==19,"a frame past the end must clamp to the last");
      }

    {
      // A weapon body's switch into a blow settles within blowTime, however far the arm turns; the usual
      // 0.3 s for so large a turn pulled t_1hAttackR's arc to the stance and it crossed short of the body
      constexpr size_t count = 2;
      asset::AnimationSample stance[count],blow[count],shown[count];
      for(size_t i=0;i<count;++i) {
        stance[i].rotation = {1,0,0,0};
        stance[i].position = {0,float(i*20),0};
        blow[i].rotation   = {std::cos(1.f),std::sin(1.f),0,0};
        blow[i].position   = stance[i].position;
        }
      Animation::Sequence idle,attack;
      attack.data = std::make_shared<Animation::AnimData>();
      asset::MdsEventTag optimal{};
      optimal.type   = asset::MdsEventType::OPTIMAL_FRAME;
      optimal.frames = {3};
      attack.data->events.push_back(optimal);
      for(const bool weapon:{true,false}) {
        AnimBlend blend;
        blend.reset(count);
        blend.setWeaponBody(weapon);
        for(size_t i=0;i<count;++i) blend.drive(i,idle,1000,stance[i],stance[i],25);
        blend.apply(stance,shown,count,1000);
        blend.apply(stance,shown,count,1033);
        for(size_t i=0;i<count;++i) blend.drive(i,attack,1033,blow[i],blow[i],25);
        blend.apply(blow,shown,count,1033);
        const bool blending = blend.apply(blow,shown,count,1033+uint64_t(AnimBlend::params().blowTime*1000.f)+17);
        require(blending!=weapon,weapon ? "a weapon body's blow must settle within its blow time" :
                                          "a body without a weapon must keep the usual blend into a blow");
        }
      }

    require(FightAlgo::resolveDownTactics(true,true,false,false,true)==FightAlgo::MV_MOVE,
            "a distant lying target must be approached");
    require(FightAlgo::resolveDownTactics(true,true,false,true,true)==FightAlgo::MV_ATTACK,
            "a lying target in finish range and focus must receive a down strike");
    require(FightAlgo::resolveDownTactics(true,true,false,true,false)==FightAlgo::MV_TURN2HIT,
            "a down strike must wait until the attacker faces its target");
    require(FightAlgo::resolveDownTactics(true,true,true,true,true)==FightAlgo::MV_WAIT,
            "a rising target in weapon range must be awaited without attacking");
    require(FightAlgo::resolveDownTactics(true,true,true,false,true)==FightAlgo::MV_MOVE,
            "a distant rising target must be approached without attacking");
    require(FightAlgo::resolveDownTactics(true,false,false,true,true)==FightAlgo::MV_NULL,
            "a standing target must retain native fight instructions");
    require(FightAlgo::resolveDownTactics(false,true,false,true,true)==FightAlgo::MV_NULL,
            "down tactics OFF must retain native fight instructions");
    {
      const auto bytes = saveArchive([](Serialize& output) {
        output.setEntry("fight");
        output.write(int32_t(Def::FightMove::WAIT));
        for(int i=0;i<FightAlgo::MV_MAX;++i)
          output.write(uint8_t(FightAlgo::MV_ATTACK));
        output.write(false);
        });
      Tempest::MemReader reader(bytes);
      Serialize input(reader);
      input.setEntry("fight");
      FightAlgo fight;
      fight.load(input);
      fight.onTargetKnockdown();
      for(auto action:fight.queueSnapshot())
        require(action==FightAlgo::MV_NULL,"target knockdown must discard the entire remaining combo");
      }

    // The actual melee damage multiplier is bounded before cleave attenuation.
    requireNear(DamageCalculator::meleeMultiplier(0,1000,0,0,0),.5f,.0001f,"slow grip contacts must respect the lower damage bound");
    requireNear(DamageCalculator::meleeMultiplier(10000,1000,1,90,0),1.5f,.0001f,"fast tip contacts must respect the upper damage bound");
    const auto full=DamageCalculator::meleeMultiplier(1000,1000,1,90,0);
    requireNear(DamageCalculator::meleeMultiplier(1000,1000,1,90,1),full*.6f,.0001f,"second target must receive 0.6 of the physical multiplier");
    requireNear(DamageCalculator::meleeMultiplier(1000,1000,1,90,2),full*.36f,.0001f,"third target must receive 0.36 of the physical multiplier");

    {
      CollisionWorld reach;
      const std::vector<MeleeBladeSegment> blade{{{0,100,0},{100,100,0},{0,0,1},2.f}};
      const std::vector<MeleeBodyCapsule> target{{{130,90,0},{130,110,0},6.f,0,1}};
      requireNear(reach.meleeBladeAdvance(blade,target,{1,0,0},40),22.f,.02f,
                  "blade reach must retain the capsule contact distance");
      require(!std::isfinite(reach.meleeBladeAdvance(blade,target,{1,0,0},20)),
              "blade reach must reject a target beyond the allowed advance");
      require(!std::isfinite(reach.meleeBladeAdvance({{{0,100,30},{100,100,30},{0,0,1},2.f}},target,{1,0,0},60)),
              "blade reach must reject a lateral miss despite sufficient forward projection");
      require(!std::isfinite(reach.meleeBladeAdvance({{{0,150,0},{100,150,0},{0,0,1},2.f}},target,{1,0,0},60)),
              "blade reach must reject a height miss despite sufficient forward projection");
      const std::vector<MeleeBodyCapsule> behind{{{-30,90,0},{-30,110,0},6.f,0,1}};
      require(!std::isfinite(reach.meleeBladeAdvance(blade,behind,{1,0,0},40)),
              "a blade beyond a close target cannot correct by moving forward");
      requireNear(reach.meleeBladeAdvance(blade,behind,{-1,0,0},40),22.f,.02f,
                  "backward magnet must retain the signed capsule contact distance");
      auto targets=target;
      targets.insert(targets.begin(),{{160,90,0},{160,110,0},6.f,1,1});
      requireNear(reach.meleeBladeAdvance(blade,targets,{1,0,0},80),22.f,.02f,
                  "blade reach must choose the nearest actual capsule contact");
      requireNear(reach.meleeBladeAdvance(blade,{{{100,90,0},{100,110,0},6.f,0,1}},{1,0,0},60),0.f,.0001f,
                  "a blade already touching its target needs no advance");
      }

    {
      CollisionWorld shadow;
      int attackerId=0,firstId=0,secondId=0,missId=0;
      auto* attacker=reinterpret_cast<Npc*>(&attackerId);
      auto* first=reinterpret_cast<Npc*>(&firstId);
      auto* second=reinterpret_cast<Npc*>(&secondId);
      auto* miss=reinterpret_cast<Npc*>(&missId);
      auto ground=shadow.addCollisionBody(new JPH::BoxShape({2,.01f,2}),at(0,-1,0),.7f,DynamicWorld::C_Landscape);
      shadow.updateShadowBody(first,{{{-30,0,0},{-30,20,0},1.f,0,1}});
      shadow.updateShadowBody(second,{{{30,0,0},{30,20,0},1.f,0,1}});
      shadow.updateShadowBody(miss,{{{0,0,50},{0,20,50},1.f,0,1}});
      const MeleeBladeSegment before{{-100,1,0},{-100,20,0},{1,0,0},2.f};
      const MeleeBladeSegment after{{100,1,0},{100,20,0},{1,0,0},2.f};
      shadow.updateShadowBlade(attacker,1,{before},0,0,100);
      shadow.updateShadowBlade(attacker,1,{after},100,0,100);
      shadow.flushShadowBlades();
      const auto hits=shadow.takeShadowBladeContacts(attacker);
      std::vector<Npc*> bodies;
      size_t floors=0;
      for(const auto& hit:hits) {
        if(hit.world) {
          require(!hit.stopsSwing && hit.normal.y>.99f,"walkable ground must not arrest the swing");
          ++floors;
          }
        else bodies.push_back(hit.npc);
        }
      require(floors==1 && bodies==std::vector<Npc*>{first,second},"blade must hit both crossed bodies in order, miss the offset body and continue through ground");
      shadow.updateShadowBlade(attacker,1,{before},150,0,200);
      shadow.flushShadowBlades();
      require(shadow.takeShadowBladeContacts(attacker).empty(),"floor and targets must not repeat during a swing");
      }

    // Clip both ends of a published pose interval, including a whole window
    // between two inactive poses. A target outside that overlap stays untouched.
    for(const auto window: {std::pair(0.,240.),std::pair(240.,260.),std::pair(235.,245.)}) {
      CollisionWorld shadow;
      int attackerIdentity=0,insideIdentity=0,outsideIdentity=0;
      auto* attacker=reinterpret_cast<Npc*>(&attackerIdentity);
      auto* inside=reinterpret_cast<Npc*>(&insideIdentity);
      auto* outside=reinterpret_cast<Npc*>(&outsideIdentity);
      const float x=window.first==0 ? 5.f : window.first==240 ? 15.f : 10.f;
      const float excluded=window.first==0 ? 15.f : 2.f;
      shadow.updateShadowBody(inside,{{{x,95,0},{x,105,0},.01f,1,1}});
      shadow.updateShadowBody(outside,{{{excluded,95,0},{excluded,105,0},.01f,1,1}});
      shadow.updateShadowBlade(attacker,1,{{{0,90,0},{0,110,0},{0,0,1},.01f}},230,window.first,window.second);
      shadow.tick(1);
      shadow.updateShadowBlade(attacker,1,{{{20,90,0},{20,110,0},{0,0,1},.01f}},250,window.first,window.second);
      shadow.tick(20);
      const auto contacts=shadow.takeShadowBladeContacts(attacker);
      require(contacts.size()==1 && contacts[0].npc==inside,
              "shadow sweep must cover exactly the overlap with its active window");
      requireNear(contacts[0].speedCmS,1000.f,.1f,"clipping must retain the published pose velocity");
      requireNear(contacts[0].velocityCmS.x,1000.f,.1f,"clipping must retain signed world contact velocity");
      requireNear(contacts[0].velocityCmS.y,0.f,.001f,"contact velocity must retain the world vertical component");
      }

    // The 8 ms no-step frame already consumes its pose. The following 9 ms
    // publication has the same velocity with or without an unrelated Jolt body.
    for(bool native:{false,true}) {
      CollisionWorld shadow;
      auto falling=native ? shadow.addDynamicBody(new JPH::BoxShape({.1f,.1f,.1f}),at(10000,10000,0),.7f,1.f) : nullptr;
      int attackerIdentity=0,targetIdentity=0;
      auto* attacker=reinterpret_cast<Npc*>(&attackerIdentity);
      auto* target=reinterpret_cast<Npc*>(&targetIdentity);
      shadow.updateShadowBody(target,{{{1.25f,95,0},{1.25f,105,0},.01f,1,1}});
      shadow.updateShadowBlade(attacker,1,{{{0,90,0},{0,110,0},{0,0,1},.01f}},0,0,100);
      shadow.updateShadowBlade(attacker,1,{{{.8f,90,0},{.8f,110,0},{0,0,1},.01f}},8,0,100);
      shadow.tick(8);
      shadow.updateShadowBlade(attacker,1,{{{1.7f,90,0},{1.7f,110,0},{0,0,1},.01f}},17,0,100);
      shadow.tick(9);
      const auto contacts=shadow.takeShadowBladeContacts(attacker);
      require(contacts.size()==1,"constant-speed shadow probe must cross its thin target");
      requireNear(contacts[0].speedCmS,100.f,.01f,"contact speed must use published pose time, not the Jolt step");
      requireNear(contacts[0].velocityCmS.x,100.f,.01f,"world contact velocity must use published pose time");
      // The frame budget caps a 50 ms interval at two native steps. Each query
      // still advances its proportional share of animation time.
      shadow.updateShadowBody(target,{{{5,95,0},{5,105,0},.01f,1,1}});
      shadow.updateShadowBlade(attacker,2,{{{6.7f,90,0},{6.7f,110,0},{0,0,1},.01f}},67,0,100);
      shadow.tick(50);
      const auto capped=shadow.takeShadowBladeContacts(attacker);
      require(capped.size()==1,"capped physics steps must cover the complete pose interval");
      requireNear(capped[0].speedCmS,100.f,.01f,"capped steps must preserve animation velocity");
      }

    // The runner publishes its last animation pose after the physics tick,
    // then exits without another tick. Flushing queries must consume that
    // interval without simulating bodies or producing the contact twice.
    for(bool native:{false,true}) {
      CollisionWorld shadow;
      auto falling=native ? shadow.addDynamicBody(new JPH::BoxShape({.1f,.1f,.1f}),at(10000,10000,0),.7f,1.f) : nullptr;
      int attackerIdentity=0,targetIdentity=0;
      auto* attacker=reinterpret_cast<Npc*>(&attackerIdentity);
      auto* target=reinterpret_cast<Npc*>(&targetIdentity);
      shadow.updateShadowBody(target,{{{10,95,0},{10,105,0},.01f,1,1}});
      shadow.updateShadowBlade(attacker,1,{{{0,90,0},{0,110,0},{0,0,1},.01f}},0,0,100);
      shadow.tick(33);
      const auto steps=shadow.simulationStep();
      const auto bodyPosition=falling ? falling->position() : Tempest::Vec3();
      shadow.updateShadowBlade(attacker,1,{{{20,90,0},{20,110,0},{0,0,1},.01f}},33,0,100);
      require(shadow.takeShadowBladeContacts(attacker).empty(),"last published pose must still await its query");
      shadow.flushShadowBlades();
      const auto contacts=shadow.takeShadowBladeContacts(attacker);
      require(contacts.size()==1 && contacts[0].npc==target,"episode flush must retain the last physical blade contact");
      requireNear(contacts[0].speedCmS,20.f/.033f,.01f,"episode flush must retain the pose interval velocity");
      shadow.flushShadowBlades();
      require(shadow.takeShadowBladeContacts(attacker).empty(),"flushing the same final pose must not repeat contacts");
      require(shadow.simulationStep()==steps,"episode flush must not advance the native simulation clock");
      if(falling)
        require((falling->position()-bodyPosition).quadLength()==0.f,"episode flush must not move native bodies");
      }

    // Shadow queries use the supplied HitZones even in a headless world with
    // no native living rig. A single animation step rotates through this thin
    // target; neither endpoint pose overlaps it.
    {
      CollisionWorld shadow;
      int attackerIdentity=0,targetIdentity=0;
      auto* attacker=reinterpret_cast<Npc*>(&attackerIdentity);
      auto* target=reinterpret_cast<Npc*>(&targetIdentity);
      const float diagonal=100.f*std::sqrt(.5f);
      const MeleeBodyCapsule body{{diagonal,95,diagonal},{diagonal,105,diagonal},.05f,7,2};
      shadow.updateShadowBody(target,{body,body});
      // An identical capsule owned by the attacker must never be a target.
      shadow.updateShadowBody(attacker,{body});
      const MeleeBladeSegment before{{0,100,0},{100,100,0},{0,1,0},.1f};
      const MeleeBladeSegment after{{0,100,0},{0,100,100},{0,1,0},.1f};
      shadow.updateShadowBlade(attacker,1,{before},0,0,100);
      shadow.tick(17);
      shadow.updateShadowBlade(attacker,1,{after},17,0,100);
      shadow.tick(17);
      const auto contacts=shadow.takeShadowBladeContacts(attacker);
      require(contacts.size()==1 && contacts[0].npc==target && contacts[0].bone==7 && contacts[0].zone==2,
              "fast rotating blade must cross thin HitZones exactly once per target");
      require((contacts[0].point-Tempest::Vec3(diagonal,100,diagonal)).length()<6.f,
              "shadow contact must report the target surface in world centimetres");
      require(contacts[0].bladeFraction>.8f && contacts[0].bladeFraction<=1.f && contacts[0].speedCmS>100.f &&
              std::isfinite(contacts[0].surfaceAngleDeg) && contacts[0].surfaceAngleDeg>=0.f && contacts[0].surfaceAngleDeg<=90.f,
              "shadow contact must retain blade location, point speed and surface angle");
      shadow.updateShadowBlade(attacker,1,{before},34,0,100);
      shadow.tick(17);
      require(shadow.takeShadowBladeContacts(attacker).empty(),
              "draining shadow contacts must preserve one-hit-per-target swing state");
      shadow.updateShadowBlade(attacker,2,{after},51,0,100);
      shadow.tick(17);
      require(shadow.takeShadowBladeContacts(attacker).size()==1,
              "a new shadow swing must allow the same target again");
      require(shadow.bodyCount()==0 && shadow.simulationStep()==0,
              "shadow queries must not create bodies or add a native simulation step");
      shadow.removeShadowBlade(attacker);
      shadow.removeShadowBody(attacker);
      shadow.removeShadowBody(target);
      }

    {
      CollisionWorld shadow;
      int attackerIdentity=0,nearIdentity=0,farIdentity=0;
      auto* attacker=reinterpret_cast<Npc*>(&attackerIdentity);
      auto* nearTarget=reinterpret_cast<Npc*>(&nearIdentity);
      auto* farTarget=reinterpret_cast<Npc*>(&farIdentity);
      auto wall=shadow.addCollisionBody(new JPH::BoxShape({.001f,1,1},.0001f),at(0,100,0),.7f,DynamicWorld::C_Object);
      // Keep an unrelated native body active so this exercises the existing
      // physics step listener as well as the headless path above.
      auto falling=shadow.addDynamicBody(new JPH::BoxShape({.1f,.1f,.1f}),at(10000,10000,0),.7f,1.f);
      shadow.updateShadowBody(nearTarget,{{{-50,95,0},{-50,105,0},.05f,3,1}});
      shadow.updateShadowBody(farTarget,{{{50,95,0},{50,105,0},.05f,4,1}});
      const MeleeBladeSegment before{{-100,80,0},{-100,120,0},{0,0,1},.1f};
      const MeleeBladeSegment after{{100,80,0},{100,120,0},{0,0,1},.1f};
      shadow.updateShadowBlade(attacker,1,{before},0,0,100);
      shadow.tick(17);
      shadow.updateShadowBlade(attacker,1,{after},17,0,100);
      shadow.tick(17);
      const auto contacts=shadow.takeShadowBladeContacts(attacker);
      require(contacts.size()==2 && contacts[0].npc==nearTarget && contacts[1].world && contacts[1].stopsSwing,
              "shadow contacts must be ordered and the first wall must hide the later target");
      requireNear(contacts[1].point.x,-.1f,.02f,"shadow world contact must retain the wall surface point");
      shadow.updateShadowBlade(attacker,1,{before},34,0,100);
      shadow.tick(17);
      require(shadow.takeShadowBladeContacts(attacker).empty(),
              "a world contact must stop the entire shadow swing across later physics steps");
      require(shadow.bodyCount()==2,"shadow stepping must not add a sensor body");

      // The same sweep going back from the attacker's facing (a wind-up): the wall is
      // passed, the target behind it is reached.
      shadow.updateShadowBlade(attacker,2,{before},51,0,200,.7f,{-1,0,0});
      shadow.tick(17);
      shadow.updateShadowBlade(attacker,2,{after},68,0,200,.7f,{-1,0,0});
      shadow.tick(17);
      const auto windup=shadow.takeShadowBladeContacts(attacker);
      require(windup.size()==3 && windup[0].npc==nearTarget && windup[1].world && !windup[1].stopsSwing &&
              windup[1].windup && windup[2].npc==farTarget,
              "a wall met by a blade going back must be passed and not hide the later target");
      }
    }
  }

void testFractureBonds(); // physics_bonds_contract.cpp

int main() {
  try {
    testShadowBodySlots();
    testCompoundChunkSeams();
    if(const auto* only=std::getenv("OPENGOTHIC_COMPOUND_SEAMS_ONLY"); only && std::string_view(only)=="1")
      return EXIT_SUCCESS;
    testMovableVobImpulseInertia();
    testMovableTableImpulseCouple();
    testMeshRayEdges();
    testCoincidentMaterials();
    testSharedRayEdge();
    testOriginalFloorProjection();
    testSourceTerrainNormals();
    testTerrainRayNormalScope();
    testSphereQueryPrecision();
    testShallowOverlapProjection();
    testNpcContactDistance();
    testNpcFrozenBodyReplacement();
    testNpcScalarWitness();
    testNpcSeparatedContactResponse();
    testFixedStepCarry();
    testFixedStepCadence();
    testItemDeepestContact();
    testItemWake();
    testItemWeaponWake();
    testContracts();
    testCharacterVirtual();
    testVirtualFloorGuard();
    testMovableVobBodies();
    testMovableVobLegacyCollision();
    testMovableVobGeometrySupport();
    testMovableVobGeometryPenetration();
    testMovableVobGeometryTopObjects();
    testMovableVobGeometryAuthoredToggle();
    testMovableVobClampedSavePose();
    testMovableVobSave();
    testWaterBuoyancy();
    testWaterDenseVobs();
    testWaterShallowAndCave();
    testMovableTraceThresholds();
    testMovableVobArrowIdentity();
    testSkinnedMeshMinimum();
    testDestruction();
    testOwnFractureMotion();
    testFractureBonds();
    testRotatingFragments();
    testPhysicsNoise();
    testRagdollNoise();
    testBodyImpactDamage();
    testBodyTraces();
    testBloodStains();
    testOffCenterImpulse();
    std::puts("Physics contracts passed");
    return EXIT_SUCCESS;
    }
  catch(const std::exception& error) {
    std::fprintf(stderr,"FAIL: physics contract exception: %s\n",error.what());
    return EXIT_FAILURE;
    }
  }
