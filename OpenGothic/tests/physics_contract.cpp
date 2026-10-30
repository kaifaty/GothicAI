#include "asset/fromzenkit.h"
#include "physics/collisionworld.h"
#include "physics/dynamicworld.h"
#include "physics/physicvbo.h"
#include "physics/ragdoll.h"
#include "graphics/mesh/protomesh.h"
#include "graphics/mesh/skeleton.h"
#include "graphics/mesh/hitzones.h"
#include "graphics/mesh/submesh/packedmesh.h"
#include "graphics/bounds.h"
#include "game/serialize.h"
#include "game/damagecalculator.h"
#include "game/meleeimpact.h"
#include "world/objects/npc.h"
#include "gothic.h"

#include <Tempest/MemReader>
#include <Tempest/MemWriter>

#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/PhysicsSettings.h>

#include <Jolt/Physics/Collision/CastSphereVsTriangles.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <zenkit/ModelHierarchy.hh>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <limits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <functional>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <string>
#include <utility>
#include <vector>

namespace {
  constexpr uint32_t landscapeMask = 1u << DynamicWorld::C_Landscape;
  constexpr uint32_t objectMask    = 1u << DynamicWorld::C_Object;
  constexpr uint32_t sensorMask    = 1u << DynamicWorld::C_Ghost;

  void require(bool condition, const char* message) {
    // These checks must also run in Release builds, where assert is disabled.
    if(condition)
      return;
    std::fprintf(stderr,"FAIL: %s\n",message);
    std::exit(EXIT_FAILURE);
    }

  void requireNear(float actual, float expected, float tolerance, const char* message) {
    if(std::isfinite(actual) && std::abs(actual-expected)<=tolerance)
      return;
    std::fprintf(stderr,"FAIL: %s: actual %.9g, expected %.9g +/- %.9g\n",
                 message,double(actual),double(expected),double(tolerance));
    std::exit(EXIT_FAILURE);
    }

  Tempest::Matrix4x4 at(float x, float y, float z) {
    auto matrix = Tempest::Matrix4x4::mkIdentity();
    matrix.translate(x,y,z);
    return matrix;
    }

  void requireMatrix(const Tempest::Matrix4x4& actual, const Tempest::Matrix4x4& expected,
                     const char* message) {
    for(int c=0;c<4;++c)
      for(int r=0;r<4;++r)
        requireNear(actual.at(c,r),expected.at(c,r),c==3 ? .001f : .00001f,message);
    }

  asset::StaticMesh movableCube() {
    asset::StaticMesh mesh;
    mesh.positions = {{-10,-10,-10},{10,-10,-10},{10,10,-10},{-10,10,-10},
                      {-10,-10,10},{10,-10,10},{10,10,10},{-10,10,10}};
    mesh.bbox = {{-10,-10,-10},{10,10,10}};
    mesh.sub_meshes.resize(1);
    auto& part = mesh.sub_meshes[0];
    part.mat.group = asset::MaterialGroup::WOOD;
    for(size_t i=0;i<mesh.positions.size();++i)
      part.wedges.push_back({{0,1,0},{0,0},uint16_t(i)});
    part.triangles = {{{0,2,1}},{{0,3,2}},{{4,5,6}},{{4,6,7}},
                      {{0,4,7}},{{0,7,3}},{{1,2,6}},{{1,6,5}},
                      {{0,1,5}},{{0,5,4}},{{3,7,6}},{{3,6,2}}};
    return mesh;
    }

  asset::StaticMesh movableBox(const Tempest::Vec3& dimensionsCm) {
    auto mesh=movableCube();
    for(auto& vertex:mesh.positions) {
      vertex.x*=dimensionsCm.x/20.f;
      vertex.y*=dimensionsCm.y/20.f;
      vertex.z*=dimensionsCm.z/20.f;
      }
    mesh.bbox={{-.5f*dimensionsCm.x,-.5f*dimensionsCm.y,-.5f*dimensionsCm.z},
               { .5f*dimensionsCm.x, .5f*dimensionsCm.y, .5f*dimensionsCm.z}};
    return mesh;
    }

  asset::StaticMesh movableCollisionBox(const Tempest::Vec3& dimensionsCm) {
    auto mesh=movableBox(dimensionsCm);
    // movableCube's winding is suitable for its hull-only fixtures. Static
    // Gothic source meshes need clockwise faces: PhysicVbo reverses them.
    for(auto& triangle:mesh.sub_meshes[0].triangles)
      std::swap(triangle.wedges[1],triangle.wedges[2]);
    return mesh;
    }

  asset::WorldMesh movableFloor() {
    asset::WorldMesh mesh;
    mesh.vertices={{-2000,0,-2000},{-2000,0,2000},{2000,0,2000},{2000,0,-2000}};
    mesh.materials.resize(1);
    mesh.materials[0].group=asset::MaterialGroup::STONE;
    // Gothic source faces are reversed by PhysicVbo::addIndex.
    mesh.polygons.vertex_indices={0,2,1,0,3,2};
    mesh.polygons.material_indices={0,0};
    return mesh;
    }

  void requireVectorRelative(const Tempest::Vec3& actual, const Tempest::Vec3& expected,
                             const char* message) {
    requireNear(actual.x,expected.x,std::max(std::abs(expected.x)*.02f,.0001f),message);
    requireNear(actual.y,expected.y,std::max(std::abs(expected.y)*.02f,.0001f),message);
    requireNear(actual.z,expected.z,std::max(std::abs(expected.z)*.02f,.0001f),message);
    }

  struct ImpulseResponse {
    float mass;
    Tempest::Vec3 impulse,linear,angular;
    };

  ImpulseResponse movableImpulseResponse(const ProtoMesh& proto, const Tempest::Vec3& dimensionsCm,
                                         float massMultiplier, const Tempest::Vec3* sameImpulse=nullptr) {
    DynamicWorld world(nullptr,asset::WorldMesh{});
    const Tempest::Vec3 center(123,10000,-257);
    const auto origin=at(center.x,center.y,center.z);
    auto object=world.addMovableVob(proto,"CONTRACT_IMPULSE.MRM",origin,massMultiplier,true,"table",true,44);
    require(!object.isEmpty() && object.mass()>0,"impulse fixture must use a native movable vob");
    const float mass=object.mass();
    const auto impulse=sameImpulse!=nullptr ? *sameImpulse : Tempest::Vec3(.2f,.4f,-.1f)*mass;
    const Tempest::Vec3 r(dimensionsCm.x*.0035f,dimensionsCm.y*.003f,-dimensionsCm.z*.002f);
    const auto point=center+r*100.f;
    require(world.applyMovableVobImpulse(center,1.f,[&](const Tempest::Vec3&,float) { return impulse; },
                                        nullptr,&point)==1,
            "airborne movable vob must receive exactly one impulse at a world point");
    world.tick(17);

    const auto size=dimensionsCm*.01f;
    const Tempest::Vec3 inertia(mass*(size.y*size.y+size.z*size.z)/12.f,
                                mass*(size.x*size.x+size.z*size.z)/12.f,
                                mass*(size.x*size.x+size.y*size.y)/12.f);
    const Tempest::Vec3 momentum(r.y*impulse.z-r.z*impulse.y,
                                 r.z*impulse.x-r.x*impulse.z,
                                 r.x*impulse.y-r.y*impulse.x);
    const Tempest::Vec3 expectedAngular(momentum.x/inertia.x,momentum.y/inertia.y,momentum.z/inertia.z);
    auto expectedLinear=impulse*(100.f/mass);
    expectedLinear.y-=DynamicWorld::gravityMS*100.f/60.f;
    const auto actualAngular=object.angularVelocity(),actualLinear=object.velocity();
    std::printf("Movable impulse %.0fx%.0fx%.0f cm, multiplier %.3f, mass %.6f kg: "
                "omega expected (%.6f, %.6f, %.6f), actual (%.6f, %.6f, %.6f) rad/s; "
                "v expected (%.6f, %.6f, %.6f), actual (%.6f, %.6f, %.6f) cm/s\n",
                double(dimensionsCm.x),double(dimensionsCm.y),double(dimensionsCm.z),
                double(massMultiplier),double(mass),
                double(expectedAngular.x),double(expectedAngular.y),double(expectedAngular.z),
                double(actualAngular.x),double(actualAngular.y),double(actualAngular.z),
                double(expectedLinear.x),double(expectedLinear.y),double(expectedLinear.z),
                double(actualLinear.x),double(actualLinear.y),double(actualLinear.z));
    requireVectorRelative(actualAngular,expectedAngular,
                          "movable angular velocity after one step must match I^-1(r cross J) within two percent");
    requireVectorRelative(actualLinear,expectedLinear,
                          "movable linear velocity after one step must match J/m plus gravity within two percent");
    return {mass,impulse,actualLinear,actualAngular};
    }

  void testMovableVobImpulseInertia() {
    Resources resources;
    for(const auto dimensions:{Tempest::Vec3(20,20,20),Tempest::Vec3(40,60,80)}) {
      ProtoMesh proto(PackedMesh(movableBox(dimensions),PackedMesh::PK_Visual),"CONTRACT_IMPULSE.MRM");
      const auto normal=movableImpulseResponse(proto,dimensions,.25f);
      const auto doubled=movableImpulseResponse(proto,dimensions,.5f,&normal.impulse);
      requireNear(doubled.mass,normal.mass*2.f,normal.mass*.0001f,
                  "doubling massMultiplier must double native movable mass");
      requireVectorRelative(doubled.angular,normal.angular*.5f,
                            "doubling massMultiplier must halve angular response to the same impulse");
      requireNear(doubled.linear.x,normal.linear.x*.5f,.0001f,
                  "doubling massMultiplier must halve X velocity from the same impulse");
      requireNear(doubled.linear.z,normal.linear.z*.5f,.0001f,
                  "doubling massMultiplier must halve Z velocity from the same impulse");
      }
    }

  void testMovableTableImpulseCouple() {
    Resources resources;
    // NW_CITY_TABLE_PEASANT_01's source bounds are 200 x 90 x 120 cm.
    const Tempest::Vec3 dimensions(200,90,120);
    constexpr float tableMass=51.81584f;
    const float multiplier=tableMass/(2.f*.9f*1.2f*DynamicWorld::materialDensity(asset::MaterialGroup::WOOD));
    ProtoMesh proto(PackedMesh(movableBox(dimensions),PackedMesh::PK_Visual),"CONTRACT_TABLE.MRM");
    const auto origin=at(0,45,0);
    DynamicWorld world(nullptr,movableFloor());
    const auto floor=world.landRay({0,100,0},200.f);
    require(floor.hasCol && floor.n.y>.99f,"table fixture must have an upward-facing static floor");
    requireNear(floor.v.y,0.f,.001f,"table fixture's bottom face must start on the floor");
    auto table=world.addMovableVob(proto,"CONTRACT_TABLE.MRM",origin,multiplier,true,"table",true,45);
    require(!table.isEmpty(),"table couple fixture must use a native movable vob on a static floor");
    requireNear(table.mass(),tableMass,.001f,"table fixture must retain the tavern table's mass");
    const Tempest::Vec3 center(0,45,0),point(0,90,60);
    const Tempest::Vec3 force(0,0,6.f*table.mass());
    for(const auto& application:{std::pair(force,point),std::pair(force*-1.f,center)})
      require(world.applyMovableVobImpulse(center,140.f,[&](const Tempest::Vec3&,float) { return application.first; },
                                          nullptr,&application.second)==1,
              "table fixture must receive both opposing bench impulses");
    const auto afterPair=table.angularVelocity();
    float maximumTilt=0;
    for(int i=0;i<120;++i) {
      world.tick(i%3==2 ? 16 : 17);
      const auto matrix=world.movableVobSnapshot()[0].transform;
      const Tempest::Vec3 up(matrix.at(1,0),matrix.at(1,1),matrix.at(1,2));
      const float cosine=std::clamp(up.y/up.length(),-1.f,1.f);
      maximumTilt=std::max(maximumTilt,std::acos(cosine)*(180.f/3.14159265358979323846f));
      }
    std::printf("Movable table couple %.6f Ns: omega after pair (%.6f, %.6f, %.6f) rad/s; maximum tilt %.6f deg\n",
                double(force.z),double(afterPair.x),double(afterPair.y),double(afterPair.z),double(maximumTilt));
    require(maximumTilt>45.f,"the bench impulse couple must tilt a free-standing table beyond forty-five degrees");

    DynamicWorld untouched(nullptr,movableFloor());
    auto resting=untouched.addMovableVob(proto,"CONTRACT_TABLE.MRM",origin,multiplier,true,"table",true,46);
    for(int i=0;i<3600;++i)
      untouched.tick(i%3==2 ? 16 : 17);
    const auto settled=untouched.movableVobSnapshot()[0].transform;
    const Tempest::Vec3 displacement(settled.at(3,0)-origin.at(3,0),settled.at(3,1)-origin.at(3,1),
                                      settled.at(3,2)-origin.at(3,2));
    std::printf("Movable table untouched for 60 s: drift %.9f cm\n",double(displacement.length()));
    require(resting.isSleeping(),"an untouched table must remain asleep for sixty seconds");
    requireNear(displacement.length(),0.f,.00001f,"an untouched table must have zero drift after sixty seconds");
    requireMatrix(settled,origin,"an untouched table must retain its complete pose for sixty seconds");
    }

  std::vector<uint8_t> saveArchive(const std::function<void(Serialize&)>& write) {
    std::vector<uint8_t> bytes;
    Tempest::MemWriter writer(bytes);
    {
      Serialize output(writer);
      write(output);
    }
    return bytes;
    }

  void loadMovables(DynamicWorld& world, const std::vector<uint8_t>& bytes) {
    Tempest::MemReader reader(bytes);
    Serialize input(reader);
    world.loadMovableVobs(input);
    }

  asset::WorldMesh waterBasin(float floorY=-400.f, bool cave=false) {
    asset::WorldMesh mesh;
    mesh.materials.resize(2);
    mesh.materials[0].group=asset::MaterialGroup::STONE;
    mesh.materials[1].group=asset::MaterialGroup::WATER;
    const auto plane=[&](float y,uint32_t material,bool twoSided) {
      const auto first=uint32_t(mesh.vertices.size());
      mesh.vertices.insert(mesh.vertices.end(),{{-2000,y,-2000},{-2000,y,2000},
                                                {2000,y,2000},{2000,y,-2000}});
      const auto triangle=[&](uint32_t a,uint32_t b,uint32_t c) {
        mesh.polygons.vertex_indices.insert(mesh.polygons.vertex_indices.end(),{first+a,first+b,first+c});
        mesh.polygons.material_indices.push_back(material);
        };
      triangle(0,2,1);
      triangle(0,3,2);
      if(twoSided) {
        triangle(0,1,2);
        triangle(0,2,3);
        }
      };
    plane(floorY,0,false);
    // Gothic's water queries look upwards from below the surface. Preserve
    // both faces so the fixture also exercises bodies starting above water.
    plane(0,1,true);
    if(cave) plane(-100,0,true);
    return mesh;
    }

  void tickWater(DynamicWorld& world,int frames) {
    for(int i=0;i<frames;++i)
      world.tick(i%3==2 ? 16 : 17);
    }

  const DynamicWorld::WaterBodySnapshot& waterBody(
      const std::vector<DynamicWorld::WaterBodySnapshot>& bodies,uint32_t zenId,size_t piece=size_t(-1)) {
    const auto found=std::find_if(bodies.begin(),bodies.end(),[&](const auto& body) {
      return body.zenId==zenId && body.piece==piece;
      });
    require(found!=bodies.end(),"water snapshots must retain each native body's parent and piece identity");
    return *found;
    }

  void testWaterBuoyancy() {
    Resources resources;
    ProtoMesh proto(PackedMesh(movableBox({80,80,80}),PackedMesh::PK_Visual),"CONTRACT_FLOAT.MRM");
    DynamicWorld world(nullptr,waterBasin());
    require(world.buoyancyEnabled(),"buoyancy must be enabled by default");
    require(world.waterRay({0,-50,0},0).hasCol,"water fixture must expose its world surface from below");
    require(world.waterRay({0,30,0},40).hasCol,"a body with its centre above water must still find a submerged surface");
    requireNear(world.waterRay({0,-50,0},0).wdepth,0,.001f,"the fixture water must be at zero centimetres");

    auto barrel=world.addMovableVob(proto,"CONTRACT_FLOAT.MRM",at(-350,110,0),.2f,true,"barrel",true,601);
    auto table=world.addMovableVob(proto,"CONTRACT_FLOAT.MRM",at(-150,110,0),.8f,true,"table",true,602);
    auto crate=world.addMovableVob(proto,"CONTRACT_FLOAT.MRM",at(-750,110,0),.4f,true,"crate",true,604);
    auto heavyBarrel=world.addMovableVob(proto,"CONTRACT_FLOAT.MRM",at(-550,110,0),.8f,true,"barrel",true,605);
    auto cupboard=world.addMovableVob(proto,"CONTRACT_FLOAT.MRM",at(-950,110,0),.8f,true,"cupboard",true,606);
    auto otherWood=world.addMovableVob(proto,"CONTRACT_FLOAT.MRM",at(-1150,110,0),.8f,true,"other",true,607);
    const float solidMass=.8f*.8f*.8f*DynamicWorld::materialDensity(asset::MaterialGroup::WOOD);
    requireNear(barrel.mass(),solidMass*.2f,.001f,"barrel buoyancy must preserve its hull density and authored mass multiplier");
    requireNear(heavyBarrel.mass(),solidMass*.8f,.001f,"a heavier barrel must preserve its distinct authored physical mass");
    requireNear(heavyBarrel.mass(),barrel.mass()*4.f,.001f,"barrel physical mass must still scale exactly with its mass multiplier");
    const float barrelMass=barrel.mass(),heavyBarrelMass=heavyBarrel.mass();
    Bounds cube;
    cube.assign(std::pair(Tempest::Vec3(-20,-20,-20),Tempest::Vec3(20,20,20)));
    Bounds arrow;
    arrow.assign(std::pair(Tempest::Vec3(-1,-1,-30),Tempest::Vec3(1,1,30)));
    ProtoMesh itemCube(PackedMesh(movableBox({40,40,40}),PackedMesh::PK_Visual),"CONTRACT_WATER_ITEM.MRM");
    ProtoMesh itemArrow(PackedMesh(movableBox({2,2,60}),PackedMesh::PK_Visual),"CONTRACT_WATER_ARROW.MRM");
    auto wood=world.addDynamicObj(at(100,110,0),cube,asset::MaterialGroup::WOOD,&itemCube,"CONTRACT_WATER_ITEM.MRM");
    auto metal=world.addDynamicObj(at(300,110,0),cube,asset::MaterialGroup::METAL,&itemCube,"CONTRACT_WATER_ITEM.MRM");
    auto stone=world.addDynamicObj(at(500,110,0),cube,asset::MaterialGroup::STONE,&itemCube,"CONTRACT_WATER_ITEM.MRM");
    auto woodenArrow=world.addDynamicObj(at(700,110,0),arrow,asset::MaterialGroup::WOOD);
    auto metalBlade=world.addDynamicObj(at(900,110,0),arrow,asset::MaterialGroup::METAL);
    auto woodenArrowHull=world.addDynamicObj(at(1100,110,0),arrow,asset::MaterialGroup::WOOD,&itemArrow,"CONTRACT_WATER_ARROW.MRM");
    auto metalBladeHull=world.addDynamicObj(at(1300,110,0),arrow,asset::MaterialGroup::METAL,&itemArrow,"CONTRACT_WATER_ARROW.MRM");
    requireNear(wood.mass(),.4f*.4f*.4f*700.f,.001f,"a wood hull must retain its density-derived physical mass in water");
    requireNear(woodenArrowHull.mass(),.02f*.02f*.6f*700.f,.000001f,
                "a thin wood hull must displace its real hull volume without the legacy thirty-centimetre box");
    const std::array<DynamicWorld::Item*,13> objects={&barrel,&heavyBarrel,&table,&crate,&cupboard,&otherWood,
                                                    &wood,&metal,&stone,&woodenArrow,&metalBlade,&woodenArrowHull,&metalBladeHull};
    const std::array<const char*,13> names={"barrel","heavier barrel","wooden table","wooden crate","wooden cupboard",
                                           "other wooden vob","wood item","metal item","stone item",
                                           "thin wooden arrow","thin metal blade","thin wooden arrow hull","thin metal blade hull"};
    std::array<int,13> firstSleep;
    firstSleep.fill(-1);
    for(auto* object:objects)
      require(object->mass()>0 && object->addImpulse({0,-object->mass()*.1f,0}),
              "water fixtures must wake genuine native dynamic bodies");
    std::vector<PhysicsNoise> splashes;
    world.setPhysicsNoiseCallback([&](const PhysicsNoise& noise) {
      if(noise.surface==asset::MaterialGroup::WATER) splashes.push_back(noise);
      });
    for(int frame=0;frame<1800;++frame) {
      world.tick(frame%3==2 ? 16 : 17);
      for(size_t i=0;i<objects.size();++i)
        if(firstSleep[i]<0 && objects[i]->isSleeping()) firstSleep[i]=frame+1;
      }
    for(size_t i=0;i<objects.size();++i) {
      std::printf("Water %s: mass %.6f kg, centre %.6f cm, first sleep %.3f s, speed %.6f cm/s\n",
                  names[i],double(objects[i]->mass()),double(objects[i]->position().y),
                  double(firstSleep[i])/60.,double(objects[i]->velocity().length()));
      require(firstSleep[i]>0 && objects[i]->isSleeping(),"still water must let every floating or grounded fixture sleep within thirty seconds");
      }
    const auto floating=world.waterBodySnapshot();
    for(const uint32_t id:{601u,605u})
      requireNear(waterBody(floating,id).submergedFraction,1.f/3.f,.04f,
                  "both barrel masses must settle with one third of their volume submerged");
    requireNear(waterBody(floating,604).submergedFraction,.5f,.04f,
                "a wooden crate must settle with half its volume submerged");
    for(const uint32_t id:{602u,606u})
      requireNear(waterBody(floating,id).submergedFraction,.9f,.04f,
                  "wooden tables and cupboards must settle with ninety percent of their volume submerged");
    requireNear(waterBody(floating,607).submergedFraction,.6f,.04f,
                "other wooden vobs must settle with sixty percent of their volume submerged");
    requireNear(barrel.mass(),barrelMass,0.f,"floating must leave the barrel's exact physical mass unchanged");
    requireNear(heavyBarrel.mass(),heavyBarrelMass,0.f,"floating must leave the heavier barrel's exact physical mass unchanged");
    requireNear(barrel.position().y,40.f-80.f/3.f,2.5f,"a barrel's class must set its draft to one third immersion");
    requireNear(heavyBarrel.position().y,barrel.position().y,2.5f,"different barrel mass multipliers must yield the same draft");
    requireNear(table.position().y,40.f-80.f*.9f,2.5f,"a wooden table's centre must settle below the surface at ninety percent immersion");
    require(table.position().y<0.f,"a mostly submerged wooden table must have its centre below water");
    requireNear(crate.position().y,0.f,2.5f,"a half-submerged wooden crate must have its centre at the water surface");
    require(barrel.position().y-table.position().y>25.f,"hollow barrel must sit visibly higher than a dense wooden table");
    requireNear(wood.position().y,20.f-40.f*.7f,2.5f,"solid wood items must float with seventy percent of their volume submerged");
    requireNear(woodenArrow.position().y,1.f-2.f*.7f,1.f,"a thin wooden item must use its actual shell for water draft");
    require(metal.position().y<-350.f && stone.position().y<-350.f && metalBlade.position().y<-350.f,
            "metal, stone and a padded thin metal item must sink to the bottom instead of floating");
    requireNear(woodenArrowHull.position().y,1.f-2.f*.7f,1.f,"a thin wooden item must use its actual shell for water draft");
    require(metalBladeHull.position().y<-350.f,"a thin metal hull must sink to the bottom instead of floating");
    require(!splashes.empty(),"entering water must emit a water noise through the existing NPC noise callback");
    require(std::all_of(splashes.begin(),splashes.end(),[](const auto& splash) {
      return splash.volume>0.f && splash.volume<=1.f && splash.radius>0.f;
      }),"splash noises must have audible finite volume and a positive NPC hearing radius");

    const auto settled=barrel.matrix();
    const std::array<Tempest::Matrix4x4,4> extraSettled={crate.matrix(),heavyBarrel.matrix(),cupboard.matrix(),otherWood.matrix()};
    const auto saved=saveArchive([&](Serialize& output) { world.saveMovableVobs(output); });
    DynamicWorld disabledRestore(nullptr,waterBasin());
    disabledRestore.setBuoyancyEnabled(false);
    auto disabledBarrel=disabledRestore.addMovableVob(proto,"CONTRACT_FLOAT.MRM",at(-350,110,0),.2f,true,"barrel",true,601);
    auto disabledTable=disabledRestore.addMovableVob(proto,"CONTRACT_FLOAT.MRM",at(-150,110,0),.8f,true,"table",true,602);
    auto disabledCrate=disabledRestore.addMovableVob(proto,"CONTRACT_FLOAT.MRM",at(-750,110,0),.4f,true,"crate",true,604);
    auto disabledHeavyBarrel=disabledRestore.addMovableVob(proto,"CONTRACT_FLOAT.MRM",at(-550,110,0),.8f,true,"barrel",true,605);
    auto disabledCupboard=disabledRestore.addMovableVob(proto,"CONTRACT_FLOAT.MRM",at(-950,110,0),.8f,true,"cupboard",true,606);
    auto disabledOtherWood=disabledRestore.addMovableVob(proto,"CONTRACT_FLOAT.MRM",at(-1150,110,0),.8f,true,"other",true,607);
    loadMovables(disabledRestore,saved);
    requireMatrix(disabledBarrel.matrix(),settled,"loading with buoyancy disabled must initially restore the saved floating pose");
    tickWater(disabledRestore,1800);
    require(disabledBarrel.position().y<-340.f && disabledTable.position().y<-340.f,
            "loading floating sleepers into a fresh buoyancy=0 world must let both props sink to the bottom");
    require(disabledCrate.position().y<-340.f && disabledHeavyBarrel.position().y<-340.f &&
            disabledCupboard.position().y<-340.f && disabledOtherWood.position().y<-340.f,
            "loading buoyancy=0 must let every wooden vob class and mass sink to the bottom");
    DynamicWorld restored(nullptr,waterBasin());
    auto loadedBarrel=restored.addMovableVob(proto,"CONTRACT_FLOAT.MRM",at(-350,110,0),.2f,true,"barrel",true,601);
    auto loadedTable=restored.addMovableVob(proto,"CONTRACT_FLOAT.MRM",at(-150,110,0),.8f,true,"table",true,602);
    auto loadedCrate=restored.addMovableVob(proto,"CONTRACT_FLOAT.MRM",at(-750,110,0),.4f,true,"crate",true,604);
    auto loadedHeavyBarrel=restored.addMovableVob(proto,"CONTRACT_FLOAT.MRM",at(-550,110,0),.8f,true,"barrel",true,605);
    auto loadedCupboard=restored.addMovableVob(proto,"CONTRACT_FLOAT.MRM",at(-950,110,0),.8f,true,"cupboard",true,606);
    auto loadedOtherWood=restored.addMovableVob(proto,"CONTRACT_FLOAT.MRM",at(-1150,110,0),.8f,true,"other",true,607);
    const std::array<DynamicWorld::Item*,4> loadedExtra={&loadedCrate,&loadedHeavyBarrel,&loadedCupboard,&loadedOtherWood};
    loadMovables(restored,saved);
    require(loadedBarrel.isSleeping() && loadedTable.isSleeping(),"a fresh world must restore floating props asleep");
    requireMatrix(loadedBarrel.matrix(),settled,"floating save restore must preserve the complete settled pose before any tick");
    for(size_t i=0;i<loadedExtra.size();++i) {
      require(loadedExtra[i]->isSleeping(),"every wooden vob class and mass must restore asleep");
      requireMatrix(loadedExtra[i]->matrix(),extraSettled[i],"every wooden vob class and mass must restore its complete floating pose");
      }
    tickWater(restored,1800);
    require(loadedBarrel.isSleeping() && loadedTable.isSleeping(),"restored floating props must remain asleep for thirty seconds");
    requireMatrix(loadedBarrel.matrix(),settled,"a restored floating prop must neither sink nor jump over thirty seconds");
    for(size_t i=0;i<loadedExtra.size();++i) {
      require(loadedExtra[i]->isSleeping(),"every restored wooden vob class and mass must remain asleep for thirty seconds");
      requireMatrix(loadedExtra[i]->matrix(),extraSettled[i],"every restored wooden vob class and mass must remain motionless for thirty seconds");
      }
    requireNear(loadedBarrel.mass(),barrelMass,0.f,"save restore must preserve the barrel's exact physical mass");
    requireNear(loadedHeavyBarrel.mass(),heavyBarrelMass,0.f,"save restore must preserve the heavier barrel's exact physical mass");
    require(loadedBarrel.addImpulse({loadedBarrel.mass()*.1f,0,0}),"a restored sleeping float must still accept native impulses");
    tickWater(restored,1800);
    // A real off-centre contact after waking may rotate the cube. Preserve the
    // former 2.5 cm / 80 cm volume tolerance without forcing its old orientation.
    requireNear(waterBody(restored.waterBodySnapshot(),601).submergedFraction,1.f/3.f,2.5f/80.f,
                "a restored float must recover its class displacement after waking in any orientation");

    world.setBuoyancyEnabled(false);
    require(!world.buoyancyEnabled(),"the runtime water toggle must disable buoyancy");
    tickWater(world,1800);
    require(barrel.position().y<-340.f && table.position().y<-340.f && wood.position().y<-350.f,
            "buoyancy=0 must wake floating sleepers and restore the old sink-to-bottom behaviour");
    require(crate.position().y<-340.f && heavyBarrel.position().y<-340.f &&
            cupboard.position().y<-340.f && otherWood.position().y<-340.f,
            "buoyancy=0 must wake every wooden vob class and mass and let it sink");
    world.setBuoyancyEnabled(true);
    require(world.buoyancyEnabled(),"the runtime water toggle must re-enable buoyancy");
    tickWater(world,1800);
    require(barrel.position().y>0.f && wood.position().y>-30.f,
            "re-enabling buoyancy must let submerged wooden sleepers rise again");
    const auto refloated=world.waterBodySnapshot();
    for(const auto expected:{std::pair(601u,1.f/3.f),std::pair(602u,.9f),std::pair(604u,.5f),
                             std::pair(605u,1.f/3.f),std::pair(606u,.9f),std::pair(607u,.6f)})
      requireNear(waterBody(refloated,expected.first).submergedFraction,expected.second,.04f,
                  "re-enabling buoyancy must restore each wooden vob's class immersion independently of mass");
    }

  void testWaterDenseVobs() {
    Resources resources;
    auto metalMesh=movableBox({80,80,80}),stoneMesh=movableBox({80,80,80});
    metalMesh.sub_meshes[0].mat.group=asset::MaterialGroup::METAL;
    stoneMesh.sub_meshes[0].mat.group=asset::MaterialGroup::STONE;
    ProtoMesh metalProto(PackedMesh(metalMesh,PackedMesh::PK_Visual),"CONTRACT_METAL_WATER.MRM");
    ProtoMesh stoneProto(PackedMesh(stoneMesh,PackedMesh::PK_Visual),"CONTRACT_STONE_WATER.MRM");
    DynamicWorld world(nullptr,waterBasin());
    auto metal=world.addMovableVob(metalProto,"CONTRACT_METAL_WATER.MRM",at(-200,110,0),.03f,true,"barrel",true,608);
    auto stone=world.addMovableVob(stoneProto,"CONTRACT_STONE_WATER.MRM",at(200,110,0),.03f,true,"barrel",true,609);
    const std::array<DynamicWorld::Item*,2> objects={&metal,&stone};
    const std::array<asset::MaterialGroup,2> materials={asset::MaterialGroup::METAL,asset::MaterialGroup::STONE};
    const std::array<float,2> masses={metal.mass(),stone.mass()};
    for(size_t i=0;i<objects.size();++i) {
      const float expected=.8f*.8f*.8f*DynamicWorld::materialDensity(materials[i])*.03f;
      requireNear(masses[i],expected,.001f,"dense vobs must retain their authored hollow mass multiplier");
      require(objects[i]->addImpulse({0,-masses[i]*.1f,0}),"dense vob water fixtures must wake native dynamic bodies");
      }
    tickWater(world,1800);
    const auto sunken=world.waterBodySnapshot();
    for(size_t i=0;i<objects.size();++i) {
      const auto& body=waterBody(sunken,608u+uint32_t(i));
      std::printf("Water dense barrel %zu: mass %.6f kg, centre %.6f cm, submerged %.6f, sleeping %d\n",
                  i,double(objects[i]->mass()),double(objects[i]->position().y),
                  double(body.submergedFraction),int(objects[i]->isSleeping()));
      requireNear(objects[i]->mass(),masses[i],0.f,"sinking must leave a dense vob's exact physical mass unchanged");
      require(objects[i]->isSleeping() && objects[i]->position().y<-300.f && body.worldBounds[1].y<0.f,
              "metal and stone barrel vobs must fully submerge and sleep at the bottom within thirty seconds despite hollow mass multipliers");
      requireNear(body.submergedFraction,1.f,.001f,"dense barrel vobs must sink with their entire native hull submerged");
      }
    }

  void testWaterShallowAndCave() {
    Resources resources;
    ProtoMesh proto(PackedMesh(movableBox({80,80,80}),PackedMesh::PK_Visual),"CONTRACT_WATER_FLOOR.MRM");
    for(const bool cave:{false,true}) {
      const float floorY=cave ? -400.f : -10.f;
      DynamicWorld world(nullptr,waterBasin(floorY,cave));
      const float birthY=cave ? -170.f : 110.f;
      if(cave)
        require(!world.waterRay({0,birthY,0},40).hasCol,
                "the cave fixture must reject world water separated from its body by a stone ceiling");
      auto object=world.addMovableVob(proto,"CONTRACT_WATER_FLOOR.MRM",at(0,birthY,0),.8f,true,"table",true,603);
      require(object.addImpulse({0,-object.mass()*.1f,0}),"shallow and cave fixtures must activate their native bodies");
      tickWater(world,1800);
      const auto settled=object.matrix();
      std::printf("Water %s: centre %.6f cm, sleeping %d, speed %.6f cm/s\n",
                  cave ? "cave below water" : "ten-centimetre shallows",double(object.position().y),
                  int(object.isSleeping()),double(object.velocity().length()));
      require(object.isSleeping(),"a float without sufficient water depth must settle on the bottom without jitter");
      // CollisionWorld keeps Jolt's default contact slop (2 cm). The movable
      // hull's 1 mm convex radius preserves its outer bounds, so its lowest
      // transformed corner may penetrate the floor by that slop, not more.
      float lowest=std::numeric_limits<float>::infinity();
      for(float x:{-40.f,40.f}) for(float y:{-40.f,40.f}) for(float z:{-40.f,40.f}) {
        Tempest::Vec3 corner(x,y,z);
        settled.project(corner);
        lowest=std::min(lowest,corner.y);
        }
      const JPH::PhysicsSettings contact;
      const float floorClearance=lowest-floorY;
      require(floorClearance>=-contact.mPenetrationSlop*100.f-.01f &&
              floorClearance<=contact.mSpeculativeContactDistance*100.f+.01f,
              "shallow and cave bodies must rest in the native floor contact band, without excess penetration or hovering");
      tickWater(world,1800);
      requireMatrix(object.matrix(),settled,"shallow or cave sleepers must remain completely motionless for thirty more seconds");
      }
    }

  const DynamicWorld::MovableVobContacts& movableContact(
      const std::vector<DynamicWorld::MovableVobContacts>& contacts, uint32_t zenId) {
    const auto found=std::find_if(contacts.begin(),contacts.end(),[&](const auto& entry) {
      return entry.zenId==zenId;
      });
    require(found!=contacts.end(),"geometry census must retain every candidate's ZEN identity");
    return *found;
    }

  void testMovableVobGeometrySupport() {
    Resources resources;
    ProtoMesh proto(PackedMesh(movableCube(),PackedMesh::PK_Visual),"CONTRACT_CLAMP.MRM");
    for(const bool initiallyEnabled:{false,true}) {
      DynamicWorld world(nullptr,movableFloor());
      world.setMovableVobsEnabled(initiallyEnabled);
      const auto supportedOrigin=at(0,10,0),hangingOrigin=at(100,100,0);
      auto supported=world.addMovableVob(proto,"CONTRACT_CLAMP.MRM",supportedOrigin,.25f,false,"utensil",true,101);
      auto hanging=world.addMovableVob(proto,"CONTRACT_CLAMP.MRM",hangingOrigin,.25f,false,"utensil",true,102);
      auto nonColliding=world.addMovableVob(proto,"CONTRACT_CLAMP.MRM",at(200,100,0),.25f,false,"utensil",false,103);
      require(!supported.isEmpty() && !hanging.isEmpty() && !nonColliding.isEmpty(),
              "geometry fixtures must retain handles with movable_vobs initially on or off");
      world.finalizeMovableVobs();
      world.setMovableVobsEnabled(true);
      const auto contacts=world.movableVobContacts(1.f);
      require(contacts.size()==3,"geometry validation must include colliding and non-colliding candidates");
      const auto& allowed=movableContact(contacts,101);
      require(allowed.enabled && allowed.disposition=="ok" && supported.mass()>0,
              "a hull resting on landscape must remain dynamic after load or runtime enable");
      for(const auto id:{102u,103u}) {
        const auto& rejected=movableContact(contacts,id);
        require(!rejected.enabled && rejected.disposition=="hangs",
                "a hull with no support within one centimetre must remain static");
        }
      require(hanging.mass()==0 && !hanging.addImpulse({1,0,0}),
              "a geometry-rejected vob must have no dynamic body or impulse response");
      require(!world.setMovableVobTransform(102,at(150,100,0)),
              "a geometry-rejected vob must not accept controlled dynamic movement");
      requireMatrix(world.movableVobPose(102)->transform,hangingOrigin,
                    "a rejected vob's attachment pose must remain at its original ZEN frame");
      require(world.landRay({100,130,0},60).hasCol && world.ray({70,100,0},{130,100,0}).hasCol,
              "a rejected small vob must restore its original C_Object source mesh queries");
      require(!world.landRay({200,130,0},60).hasCol && !world.ray({170,100,0},{230,100,0}).hasCol,
              "a rejected cd_dynamic=0 vob must preserve the original absence of C_Object collision");
      require(world.physicsStats().bodies==4,
              "rejected hulls must be removed, leaving landscape, one hull and two eligible source meshes");
      const auto state=world.movableVobSnapshot();
      const auto staticState=std::find_if(state.begin(),state.end(),[](const auto& entry) { return entry.zenId==102; });
      require(staticState!=state.end() && !staticState->trace && !staticState->tilted && staticState->traceTime==-1,
              "geometry rejection must not create a fallen-furniture trace");
      require(std::isfinite(world.movableVobValidationMilliseconds()) && world.movableVobValidationMilliseconds()>=0,
              "load-time geometry validation must expose a finite nonnegative duration");
      // A second toggle must preserve the authored decision and original pose.
      world.setMovableVobsEnabled(false);
      world.setMovableVobsEnabled(true);
      require(supported.mass()>0 && hanging.mass()==0 && nonColliding.mass()==0,
              "runtime re-enable must preserve geometry decisions for each individual vob");
      requireMatrix(world.movableVobPose(102)->transform,hangingOrigin,
                    "runtime toggles must not move a rejected static source mesh");
      }
    DynamicWorld gaps(nullptr,movableFloor());
    auto nearSupport=gaps.addMovableVob(proto,"CONTRACT_CLAMP.MRM",at(0,10.5f,0),.25f,true,"table",true,104);
    auto farSupport=gaps.addMovableVob(proto,"CONTRACT_CLAMP.MRM",at(100,11.5f,0),.25f,true,"table",true,105);
    gaps.finalizeMovableVobs();
    const auto gapContacts=gaps.movableVobContacts(1.f);
    require(movableContact(gapContacts,104).enabled && nearSupport.mass()>0,
            "a half-centimetre gap above landscape must count as support within the one-centimetre margin");
    require(movableContact(gapContacts,105).disposition=="hangs" && farSupport.mass()==0,
            "landscape one and a half centimetres below a hull must not count as support");
    }

  void testMovableVobGeometryPenetration() {
    Resources resources;
    ProtoMesh proto(PackedMesh(movableCube(),PackedMesh::PK_Visual),"CONTRACT_CLAMP.MRM");
    ProtoMesh wall(PackedMesh(movableCollisionBox({20,200,200}),PackedMesh::PK_Visual),"CONTRACT_CLAMP_WALL.MRM");
    DynamicWorld world(nullptr,movableFloor());
    auto shallow=world.addMovableVob(proto,"CONTRACT_CLAMP.MRM",at(-100,7,0),.25f,true,"table",true,111);
    auto deep=world.addMovableVob(proto,"CONTRACT_CLAMP.MRM",at(0,4,0),.25f,true,"table",true,112);
    // The hull crosses the middle of the wall's -X face by five centimetres,
    // far from its top/bottom faces and outer edges.
    auto structure=world.addStaticObj(wall.attach[0].shape.get(),at(115,10,0),"CONTRACT_CLAMP_WALL.MRM");
    auto embedded=world.addMovableVob(proto,"CONTRACT_CLAMP.MRM",at(100,10,0),.25f,true,"table",true,113);
    auto first=world.addMovableVob(proto,"CONTRACT_CLAMP.MRM",at(300,10,0),.25f,true,"table",true,114);
    auto second=world.addMovableVob(proto,"CONTRACT_CLAMP.MRM",at(315,10,0),.25f,true,"table",true,115);
    require(!structure.isEmpty(),"penetration fixture must install its authored static C_Object before validation");
    world.finalizeMovableVobs();
    const auto contacts=world.movableVobContacts(1.f);
    require(movableContact(contacts,111).enabled && shallow.mass()>0,
            "three centimetres of landscape penetration must remain below the four-centimetre clamp threshold");
    require(movableContact(contacts,112).disposition=="floor" && deep.mass()==0,
            "more than four centimetres of landscape penetration must restore the static source mesh");
    const auto& blocked=movableContact(contacts,113);
    std::printf("Movable C_Object fixture: %s, mass %.6f kg",blocked.disposition.c_str(),double(embedded.mass()));
    for(const auto& contact:blocked.contacts)
      std::printf(" [%s, side %d, depth %.6f cm]",contact.other.c_str(),contact.side,double(contact.depth));
    std::puts("");
    require(blocked.disposition=="structure" && embedded.mass()==0,
            "more than four centimetres of static C_Object penetration must restore the static source mesh");
    for(const auto id:{114u,115u}) {
      const auto& entry=movableContact(contacts,id);
      require(!entry.enabled && entry.disposition=="neighbour",
              "both members of a deeply intersecting movable pair must become static together");
      require(std::any_of(entry.contacts.begin(),entry.contacts.end(),[&](const auto& contact) {
        return contact.category==DynamicWorld::C_MovableVob && contact.otherZenId==(id==114 ? 115u : 114u) &&
               contact.depth>4.f;
        }),"the retained origin census must identify the deeply intersecting movable neighbour");
      }
    require(first.mass()==0 && second.mass()==0,
            "deep-pair decisions must be computed before replacing either member's hull");
    }

  void testMovableVobGeometryTopObjects() {
    Resources resources;
    ProtoMesh base(PackedMesh(movableBox({100,100,100}),PackedMesh::PK_Visual),"CONTRACT_CLAMP_BASE.MRM");
    ProtoMesh small(PackedMesh(movableCollisionBox({20,20,20}),PackedMesh::PK_Visual),"CONTRACT_CLAMP_SMALL.MRM");
    ProtoMesh large(PackedMesh(movableCollisionBox({100,20,20}),PackedMesh::PK_Visual),"CONTRACT_CLAMP_LARGE.MRM");
    DynamicWorld world(nullptr,movableFloor());
    auto exempt=world.addMovableVob(base,"CONTRACT_CLAMP_BASE.MRM",at(0,50,0),.25f,true,"table",true,121);
    auto held=world.addMovableVob(base,"CONTRACT_CLAMP_BASE.MRM",at(300,50,0),.25f,true,"table",true,122);
    auto trinket=world.addStaticObj(small.attach[0].shape.get(),at(0,105,0),"CONTRACT_CLAMP_SMALL.MRM");
    auto load=world.addStaticObj(large.attach[0].shape.get(),at(300,110,0),"CONTRACT_CLAMP_LARGE.MRM");
    require(!trinket.isEmpty() && !load.isEmpty(),"top-object fixture must install both static source meshes");
    world.finalizeMovableVobs();
    const auto contacts=world.movableVobContacts(1.f);
    const auto& item=movableContact(contacts,121);
    require(item.enabled && item.disposition=="item" && exempt.mass()>0,
            "a small C_Object beginning above ninety percent of hull height must not clamp its support");
    require(std::any_of(item.contacts.begin(),item.contacts.end(),[](const auto& contact) {
      return contact.category==DynamicWorld::C_Object && contact.depth>4.f;
      }),"small-top exemption fixture must exercise penetration deeper than four centimetres");
    require(!movableContact(contacts,122).enabled && movableContact(contacts,122).disposition=="carries" && held.mass()==0,
            "a C_Object larger than eighty centimetres resting on the top must clamp its support");
    }

  void testMovableVobGeometryAuthoredToggle() {
    Resources resources;
    ProtoMesh proto(PackedMesh(movableCube(),PackedMesh::PK_Visual),"CONTRACT_CLAMP.MRM");
    ProtoMesh wall(PackedMesh(movableCollisionBox({20,200,200}),PackedMesh::PK_Visual),"CONTRACT_CLAMP_WALL.MRM");
    DynamicWorld world(nullptr,movableFloor());
    world.setMovableVobsEnabled(false);
    auto blocked=world.addMovableVob(proto,"CONTRACT_CLAMP.MRM",at(0,10,0),.25f,true,"table",true,141);
    auto clear=world.addMovableVob(proto,"CONTRACT_CLAMP.MRM",at(300,10,0),.25f,true,"table",true,142);
    auto obstacle=world.addStaticObj(wall.attach[0].shape.get(),at(15,10,0),"CONTRACT_CLAMP_WALL.MRM");
    require(!obstacle.isEmpty(),"runtime-toggle fixture must install its authored C_Object collision");
    world.finalizeMovableVobs();
    const auto authored=world.movableVobContacts(1.f);
    require(!world.movableVobsEnabled() && blocked.mass()==0 && clear.mass()==0,
            "capturing geometry while disabled must restore the disabled physics setting");
    require(movableContact(authored,141).disposition=="structure" && movableContact(authored,142).disposition=="ok",
            "finalization while disabled must capture the original authored obstruction for both candidates");
    // A door or scripted object can move before physics is enabled. Its current
    // pose must neither release the original clamp nor create a new one.
    obstacle.setObjMatrix(at(315,10,0));
    world.setMovableVobsEnabled(true);
    const auto enabled=world.movableVobContacts(1.f);
    require(!movableContact(enabled,141).enabled && blocked.mass()==0,
            "runtime enable must retain an authored clamp after its static obstruction moves away");
    require(movableContact(enabled,142).enabled && clear.mass()>0,
            "runtime enable must not invent a clamp from a static obstruction's later scripted pose");
    }

  void testMovableVobClampedSavePose() {
    Resources resources;
    ProtoMesh proto(PackedMesh(movableCube(),PackedMesh::PK_Visual),"CONTRACT_CLAMP.MRM");
    const auto origin=at(0,100,0);
    auto moved=at(75,125,0);
    moved.rotateOZ(60);
    const auto saved=saveArchive([&](Serialize& output) {
      output.setEntry("worlds/_/movable-vobs");
      output.write(uint32_t(0x31564f4d),uint16_t(2),std::string_view("_"),uint32_t(1));
      output.write(uint32_t(131),std::string_view("CONTRACT_CLAMP.MRM"),origin,moved,true);
      output.write(true,uint64_t(1501),std::string_view("PLAYER"));
      });
    DynamicWorld world(nullptr,movableFloor());
    auto object=world.addMovableVob(proto,"CONTRACT_CLAMP.MRM",origin,.25f,true,"table",true,131);
    Tempest::Matrix4x4 rendered=origin;
    object.setTransformCallback([&](const auto& matrix) { rendered=matrix; });
    world.finalizeMovableVobs();
    loadMovables(world,saved);
    const auto state=world.movableVobSnapshot();
    require(state.size()==1 && !state[0].enabled && object.mass()==0,
            "a saved pose must not restore a dynamic body for an authored geometry clamp");
    requireMatrix(state[0].transform,origin,"a shifted save record for a clamped vob must retain the original ZEN pose");
    requireMatrix(rendered,origin,"ignoring a clamped saved pose must retain the original render transform");
    requireMatrix(world.movableVobPose(131)->transform,origin,
                  "ignoring a clamped saved pose must retain the original attachment transform");
    require(!state[0].trace && !state[0].tilted && state[0].traceTime==-1 && state[0].traceCulprit.empty(),
            "ignoring a clamped saved pose must also ignore its moved trace metadata");
    }

  void testMovableVobSave() {
    Resources resources;
    ProtoMesh proto(PackedMesh(movableCube(),PackedMesh::PK_Visual),"CONTRACT_CUBE.MRM");
    constexpr uint32_t zenId=42;
    auto origin=at(120,100,-200);
    origin.rotateOY(17.f);
    auto moved=at(170.125f,118.5f,-219.25f);
    moved.rotateOX(13.f);
    moved.rotateOY(39.f);
    // The independent optional entry can describe a settled pose without having to
    // wait for a game asset or a particular native solver trajectory in this fixture.
    auto savedRecord=[&](std::string_view worldName, uint32_t id, std::string_view visual,
                         const Tempest::Matrix4x4& birth) {
      return saveArchive([&](Serialize& output) {
        output.setEntry("worlds/_/movable-vobs");
        output.write(uint32_t(0x31564f4d),uint16_t(1),worldName,uint32_t(1));
        output.write(id,visual,birth,moved,true);
        });
      };
    const auto saved=savedRecord("_",zenId,"CONTRACT_CUBE.MRM",origin);
    DynamicWorld first(nullptr,asset::WorldMesh{});
    auto firstObject=first.addMovableVob(proto,"CONTRACT_CUBE.MRM",origin,.25f,true,"table",true,zenId);
    require(!firstObject.isEmpty(),"movable save fixture must have a physical body");
    loadMovables(first,saved);
    auto firstState=first.movableVobSnapshot();
    require(firstState.size()==1 && firstState[0].zenId==zenId && firstState[0].sleeping,
            "saved movable identity and sleep must restore in a fresh world");
    requireMatrix(firstState[0].origin,origin,"restore must retain the original ZEN frame");
    requireMatrix(firstState[0].transform,moved,"restore must retain moved translation and rotation");
    const auto roundtrip=saveArchive([&](Serialize& output) { first.saveMovableVobs(output); });

    DynamicWorld second(nullptr,asset::WorldMesh{});
    auto secondObject=second.addMovableVob(proto,"CONTRACT_CUBE.MRM",origin,.25f,true,"table",true,zenId);
    loadMovables(second,roundtrip);
    require(secondObject.isSleeping(),"a sleeping saved vob must remain asleep after a save roundtrip");
    auto pose=second.movableVobPose(zenId);
    require(pose!=nullptr && pose->zenId==zenId && pose->visual=="CONTRACT_CUBE.MRM",
            "attachments must find a loaded vob by its stable ZEN identity");
    requireMatrix(pose->transform,moved,"attachment pose must synchronize during restore before any tick");
    for(int i=0;i<1800;++i)
      second.tick(17);
    require(secondObject.isSleeping(),"restored furniture must remain asleep for thirty seconds");
    requireMatrix(second.movableVobSnapshot()[0].transform,moved,"sleeping restored furniture must not drift");
    const auto pinLocal=at(2,3,4);
    const auto pinBefore=pose->transform*pinLocal;
    require(secondObject.addImpulse({secondObject.mass(),0,0}),"restored furniture must still accept impulses");
    second.tick(17);
    const auto after=second.movableVobSnapshot()[0].transform;
    requireMatrix(pose->transform,after,"attachment pose must follow later furniture motion");
    require(std::abs((pose->transform*pinLocal).at(3,0)-pinBefore.at(3,0))>.01f,
            "a local arrow attachment must travel with a restored vob after another push");

    const std::vector<std::vector<uint8_t>> rejected = {
      savedRecord("OTHERWORLD.ZEN",zenId,"CONTRACT_CUBE.MRM",origin),
      savedRecord("_",zenId+1,"CONTRACT_CUBE.MRM",origin),
      savedRecord("_",zenId,"OTHER_CUBE.MRM",origin),
      savedRecord("_",zenId,"CONTRACT_CUBE.MRM",at(121,100,-200)),
      saveArchive([](Serialize& output) { output.setEntry("old-save"); output.write(uint8_t(1)); })
      };
    for(const auto& bytes:rejected) {
      DynamicWorld other(nullptr,asset::WorldMesh{});
      auto object=other.addMovableVob(proto,"CONTRACT_CUBE.MRM",origin,.25f,true,"table",true,zenId);
      loadMovables(other,bytes);
      requireMatrix(other.movableVobSnapshot()[0].transform,origin,
                    "old saves and mismatched world, ZEN id, visual or origin must retain the ZEN pose");
      require(object.isSleeping(),"ignoring an optional record must not wake furniture");
      }
    DynamicWorld disabled(nullptr,asset::WorldMesh{});
    disabled.setMovableVobsEnabled(false);
    auto disabledObject=disabled.addMovableVob(proto,"CONTRACT_CUBE.MRM",origin,.25f,true,"table",true,zenId);
    loadMovables(disabled,saved);
    requireMatrix(disabled.movableVobSnapshot()[0].transform,origin,
                  "movable_vobs=0 must ignore saved motion and keep the ZEN frame");
    }

  void testMovableTraceThresholds() {
    Resources resources;
    ProtoMesh proto(PackedMesh(movableCube(),PackedMesh::PK_Visual),"CONTRACT_CUBE.MRM");
    constexpr uint32_t zenId=43;
    const auto origin=at(0,100,0);
    DynamicWorld world(nullptr,asset::WorldMesh{});
    auto object=world.addMovableVob(proto,"CONTRACT_CUBE.MRM",origin,.25f,true,"table",true,zenId);
    require(!object.isEmpty(),"trace fixture must have a movable body");
    world.setTraceTime(1500);
    for(const float distance:{10.f,50.f}) {
      require(world.setMovableVobTransform(zenId,at(distance,100,0)),"trace fixture must accept controlled translation");
      const auto state=world.movableVobSnapshot()[0];
      require(!state.trace && !state.tilted && state.traceTime==-1,
              "ten or exactly fifty centimetres of displacement must not create a trace");
      }
    require(world.setMovableVobTransform(zenId,at(50.1f,100,0)),"trace fixture must accept above-threshold displacement");
    auto state=world.movableVobSnapshot()[0];
    require(state.trace && !state.tilted && state.traceTime==1500 && state.traceCulprit.empty(),
            "displacement beyond fifty centimetres must create an unknown-culprit trace at game time");
    world.setTraceTime(1530);
    require(world.setMovableVobTransform(zenId,at(75,100,0)),"an existing trace must accept further movement");
    require(world.movableVobSnapshot()[0].traceTime==1500,
            "later furniture movement must retain the first trace appearance time");

    const auto roundtrip=saveArchive([&](Serialize& output) { world.saveMovableVobs(output); });
    DynamicWorld restored(nullptr,asset::WorldMesh{});
    auto restoredObject=restored.addMovableVob(proto,"CONTRACT_CUBE.MRM",origin,.25f,true,"table",true,zenId);
    restored.setTraceTime(2000);
    loadMovables(restored,roundtrip);
    state=restored.movableVobSnapshot()[0];
    require(state.trace && state.traceTime==1500 && state.traceCulprit.empty(),
            "a v2 save roundtrip must retain trace appearance rather than load time");

    for(const float degrees:{30.f,31.f}) {
      auto rotated=origin;
      rotated.rotateOX(degrees);
      require(world.setMovableVobTransform(zenId,rotated),"trace fixture must accept a tilted pose");
      state=world.movableVobSnapshot()[0];
      require(state.trace==(degrees>30.f) && state.tilted==(degrees>30.f),
              "exactly thirty degrees must not be a trace, but thirty-one degrees must be tilted");
      }
    auto yaw=origin;
    yaw.rotateOY(140.f);
    require(world.setMovableVobTransform(zenId,yaw),"trace fixture must accept an upright yaw rotation");
    state=world.movableVobSnapshot()[0];
    require(!state.trace && !state.tilted && state.traceTime==-1,
            "yaw-only rotation must not create a fallen-furniture trace");

    const auto moved=at(75,100,0);
    for(const uint16_t schema:{uint16_t(1),uint16_t(2)}) {
      const auto saved=saveArchive([&](Serialize& output) {
        output.setEntry("worlds/_/movable-vobs");
        output.write(uint32_t(0x31564f4d),schema,std::string_view("_"),uint32_t(1));
        output.write(zenId,std::string_view("CONTRACT_CUBE.MRM"),origin,moved,true);
        if(schema>=2)
          output.write(true,uint64_t(1501),std::string_view("PLAYER"));
        });
      DynamicWorld loaded(nullptr,asset::WorldMesh{});
      auto loadedObject=loaded.addMovableVob(proto,"CONTRACT_CUBE.MRM",origin,.25f,true,"table",true,zenId);
      loaded.setTraceTime(2200);
      loadMovables(loaded,saved);
      state=loaded.movableVobSnapshot()[0];
      require(state.trace && !state.tilted && state.zenId==zenId,
              "both optional save schemas must recover the same moved trace identity");
      require(state.traceTime==(schema==1 ? -1 : 1500) && state.traceCulprit==(schema==1 ? "" : "PLAYER"),
              "legacy trace time and culprit must remain unknown; v2 must retain engine truth");
      require(loadedObject.addImpulse({loadedObject.mass(),0,0}),"a loaded trace must accept another impulse");
      loaded.tick(17);
      const auto afterImpulse=loaded.movableVobSnapshot()[0];
      require(afterImpulse.trace && afterImpulse.traceTime==state.traceTime && afterImpulse.traceCulprit==state.traceCulprit,
              "a later unattributed impulse must not rewrite an existing trace's first time or culprit");
      const auto savedAgain=saveArchive([&](Serialize& output) { loaded.saveMovableVobs(output); });
      DynamicWorld reloaded(nullptr,asset::WorldMesh{});
      auto reloadedObject=reloaded.addMovableVob(proto,"CONTRACT_CUBE.MRM",origin,.25f,true,"table",true,zenId);
      reloaded.setTraceTime(2300);
      loadMovables(reloaded,savedAgain);
      const auto reloadedState=reloaded.movableVobSnapshot()[0];
      require(reloadedState.trace && reloadedState.traceTime==state.traceTime && reloadedState.traceCulprit==state.traceCulprit,
              "saving either legacy unknown metadata or v2 known metadata must preserve it in the next process");
      }
    }

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

  void testMovableVobArrowIdentity() {
    Resources resources;
    ProtoMesh proto(PackedMesh(movableCube(),PackedMesh::PK_Visual),"CONTRACT_CUBE.MRM");
    DynamicWorld world(nullptr,asset::WorldMesh{});
    auto object=world.addMovableVob(proto,"CONTRACT_CUBE.MRM",at(0,100,0),.25f,true,"table",true,42);
    struct ArrowContact final : DynamicWorld::BulletCallback {
      bool stopsAtWorldHit() const override { return true; }
      void onWorldHit(const DynamicWorld::BulletWorldHit& hit) override {
        ++hits;
        movable=hit.movable;
        }
      int hits=0;
      std::weak_ptr<const DynamicWorld::MovableVobPose> movable;
      } contact;
    auto* arrow=world.bulletObj(&contact);
    arrow->setPosition({-40,100,0});
    arrow->setDirection({3,0,0});
    world.tick(17);
    require(contact.hits==1 && !contact.movable.expired(),
            "a world arrow contact must identify the movable body it hit");
    require(contact.movable.lock()==world.movableVobPose(42),
            "arrow contact identity must share the live ZEN vob attachment pose");
    world.deleteObj(arrow);
    object=DynamicWorld::Item();
    require(contact.movable.expired() && world.movableVobPose(42)==nullptr,
            "removing a vob must expire weak arrow attachments without leaving a dangling body");
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

  void testDestruction() {
    struct CacheFixture {
      std::filesystem::path path;
      std::string old;
      bool hadOld=false;
      CacheFixture() {
        if(const auto* value=std::getenv("OPENGOTHIC_FRACTURE")) { old=value; hadOld=true; }
        path=std::filesystem::temp_directory_path()/
             ("gothic-fracture-contract-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(path);
        std::ofstream file(path/"NW_HARBOUR_CRATE_01.fracture");
        file<<"GOTHIC_FRACTURE 1\npieces 8\n";
        for(int x:{-1,1}) for(int y:{-1,1}) for(int z:{-1,1}) {
          auto mesh=movableBox({50,50,50});
          for(auto& p:mesh.positions) { p.x+=float(x)*25.f; p.y+=float(y)*25.f; p.z+=float(z)*25.f; }
          file<<"piece 125000 "<<x*25<<' '<<y*25<<' '<<z*25<<" 12\n";
          for(const auto& triangle:mesh.sub_meshes[0].triangles) {
            file<<"tri WOOD.TGA\n";
            for(const auto index:triangle.wedges) {
              const auto& p=mesh.positions[index];
              file<<p.x<<' '<<p.y<<' '<<p.z<<" 0 1 0 0 0\n";
              }
            }
          }
        file.close();
        std::filesystem::copy_file(path/"NW_HARBOUR_CRATE_01.fracture",path/"NW_HARBOUR_BARREL_01.fracture");
        std::filesystem::copy_file(path/"NW_HARBOUR_CRATE_01.fracture",path/"NW_NATURE_FENCE_01.fracture");
        setenv("OPENGOTHIC_FRACTURE",path.string().c_str(),1);
        }
      ~CacheFixture() {
        if(hadOld) setenv("OPENGOTHIC_FRACTURE",old.c_str(),1);
        else unsetenv("OPENGOTHIC_FRACTURE");
        std::filesystem::remove_all(path);
        }
      } cache;
    Resources resources;
    ProtoMesh proto(PackedMesh(movableCollisionBox({100,100,100}),PackedMesh::PK_Visual),"NW_HARBOUR_CRATE_01.MRM");
    constexpr uint32_t zenId=4321;
    const auto make=[&](DynamicWorld& world,const Tempest::Matrix4x4& matrix) {
      return world.addMovableVob(proto,"NW_HARBOUR_CRATE_01.MRM",matrix,.04f,true,"crate",true,zenId);
      };
    require(FractureRule::classify("path/nw_harbour_crate_01.3ds")!=nullptr &&
            FractureRule::classify("NW_CITY_TABLE_01.MRM")==nullptr,
            "destruction must use the explicit visual data table");
    {
      const auto& rule=*FractureRule::classify("NW_HARBOUR_CRATE_01.MRM");
      requireNear(rule.meleeDamage(FractureRule::Melee::Fist,0,100),17.5f,.001f,
                  "fists must use strength and the CSV fist multiplier");
      requireNear(rule.meleeDamage(FractureRule::Melee::OneHand,40,100),70.f,.001f,
                  "one-handed damage must combine weapon damage and strength");
      requireNear(rule.meleeDamage(FractureRule::Melee::TwoHand,40,100),91.f,.001f,
                  "two-handed strikes must use their own CSV multiplier");
      DynamicWorld world(nullptr,asset::WorldMesh{});
      auto object=make(world,at(100,1000,0));
      const auto swing=[&](const Tempest::Vec3& from,const Tempest::Vec3& facing,float range) {
        return world.meleeMovableVobs(from,facing,range,40,100,FractureRule::Melee::OneHand);
        };
      require(swing({0,1000,0},{-1,0,0},150)==0 && swing({0,1000,0},{1,0,0},40)==0 &&
              swing({0,1300,0},{1,0,0},150)==0,
              "melee must reject a target behind, out of reach or outside the striking height");
      requireNear(world.movableVobSnapshot()[0].health,100,.001f,"misses must leave structural health untouched");
      ProtoMesh wallMesh(PackedMesh(movableCollisionBox({10,200,200}),PackedMesh::PK_Visual),"MELEE_WALL.MRM");
      auto wall=world.addStaticObj(wallMesh.attach[0].shape.get(),at(25,1000,0),"MELEE_WALL.MRM");
      require(swing({0,1000,0},{1,0,0},150)==0,
              "a wall before a crate must stop all melee contact rays");
      wall=DynamicWorld::Item{};
      require(swing({0,1000,0},{1,0,0},150)==1,"a real first contact must reach the crate");
      requireNear(world.movableVobSnapshot()[0].health,30,.001f,
                  "one phase must damage a contacted body only once even when many rays hit it");
      require(swing({0,1000,0},{1,0,0},150)==1,"the next phase must be a separate strike");
      world.tick(17);
      require(object.isBroken() && world.fragmentSnapshot().size()==8,
              "two one-handed strikes must break and replace the crate");
      world.setDestructionEnabled(false);
      require(swing({0,1000,0},{1,0,0},150)==0 && !object.isBroken(),
              "destruction=0 must ignore otherwise valid melee strikes");
      }
    {
      DynamicWorld world(nullptr,asset::WorldMesh{});
      world.setTraceTime(1500);
      auto object=make(world,at(0,1000,0));
      require(world.destructionEnabled() && object.fractureMesh()!=nullptr && object.fractureMesh()->pieces.size()==8,
              "destruction must preload all native piece hulls and be enabled by default");
      bool brokenCallback=false;
      object.setFractureCallback([&](bool broken,const std::vector<Tempest::Matrix4x4>& pieces) {
        brokenCallback=broken;
        require(pieces.size()==size_t(broken ? 8 : 0),"render callbacks must match the physical piece count");
        });
      std::vector<PhysicsNoise> noises;
      world.setPhysicsNoiseCallback([&](const PhysicsNoise& noise) { noises.push_back(noise); });
      const auto parentPose=world.movableVobPose(zenId);
      const float mass=object.mass();
      const Tempest::Vec3 point(0,1050,0);
      require(world.applyMovableVobImpulse({0,1000,0},1.f,[&](const Tempest::Vec3&,float) {
        return Tempest::Vec3(mass*10.f,0,0);
        },nullptr,&point)==1,"a strong blast must reach its target");
      world.tick(17);
      require(object.isBroken() && brokenCallback && parentPose->broken,
              "breaking must hide the intact body and invalidate pinned-arrow parents together");
      const auto state=world.movableVobSnapshot()[0];
      require(state.broken && state.destructible && state.health==0 && !state.enabled,
              "a broken original must retain diagnostic identity without an intact collision body");
      require(state.traceTime==1500 && state.traceCulprit.empty(),
              "breaking must record the game minute and leave an unknown actor unknown");
      const auto pieces=world.fragmentSnapshot();
      require(pieces.size()==8 && world.physicsStats().bodies==8,
              "replacement must remove both intact and query collisions and install only fragments");
      Tempest::Vec3 average;
      for(const auto& piece:pieces) {
        require(piece.parentId==state.id && piece.zenId==zenId && !piece.sleeping,
                "each dynamic fragment must retain its original parent identity");
        requireNear((piece.angularVelocity-pieces[0].angularVelocity).length(),0,.001f,
                    "all fragments must inherit the same parent angular velocity");
        average+=piece.velocity/8.f;
        }
      requireNear(average.x,1000.f,5.f,"fragments must inherit parent linear momentum");
      require(pieces[0].angularVelocity.length()>1.f,"fragments must inherit nonzero parent rotation");
      require(!noises.empty() && noises.back().volume==1.f && noises.back().radius==1600.f &&
              noises.back().material==MAT_WOOD && noises.back().what.find("break")!=std::string::npos,
              "breaking must emit one loud material noise through the existing physics noise callback");
      require(!world.landRay({0,1100,0},300).hasCol,
              "fragments must never become walking or floor query obstacles");
      const auto saved=saveArchive([&](Serialize& output) { world.saveMovableVobs(output); });
      DynamicWorld restored(nullptr,asset::WorldMesh{});
      restored.setTraceTime(9000);
      auto restoredObject=make(restored,at(0,1000,0));
      loadMovables(restored,saved);
      const auto restoredPieces=restored.fragmentSnapshot();
      require(restoredObject.isBroken() && restoredPieces.size()==pieces.size(),
              "a fresh world must restore the broken original and every surviving fragment");
      require(restored.movableVobSnapshot()[0].traceTime==1500 &&
              restored.movableVobSnapshot()[0].traceCulprit.empty(),
              "loading debris must preserve its destruction minute rather than use load time");
      for(size_t i=0;i<pieces.size();++i) {
        require(restoredPieces[i].piece==pieces[i].piece && restoredPieces[i].sleeping==pieces[i].sleeping,
                "fragment cache index and sleep state must survive save/load");
        requireMatrix(restoredPieces[i].transform,pieces[i].transform,"saved fragment pose must restore before any tick");
        requireNear((restoredPieces[i].velocity-pieces[i].velocity).length(),0,.001f,
                    "awake fragment linear velocity must survive save/load");
        requireNear((restoredPieces[i].angularVelocity-pieces[i].angularVelocity).length(),0,.001f,
                    "awake fragment angular velocity must survive save/load");
        }
      for(const uint16_t schema:{uint16_t(1),uint16_t(2)}) {
        const auto versioned=saveArchive([&](Serialize& output) {
          output.setEntry("worlds/",output.worldName(),"/destruction-vobs");
          output.write(uint32_t(0x31534544),schema,output.worldName(),uint32_t(1));
          output.write(zenId,state.visual,state.origin,state.transform,0.f,true,uint32_t(pieces.size()));
          if(schema>=2)
            output.write(uint64_t(1501),std::string("NPC_DESTRUCTION_TEST"));
          for(const auto& piece:pieces)
            output.write(true,piece.permanent,uint64_t(piece.asleepSince)+1,
                         piece.transform,piece.velocity,piece.angularVelocity,piece.sleeping);
          });
        DynamicWorld loaded(nullptr,asset::WorldMesh{});
        loaded.setTraceTime(9000);
        auto loadedObject=make(loaded,at(0,1000,0));
        loadMovables(loaded,versioned);
        const auto metadata=loaded.movableVobSnapshot()[0];
        require(loadedObject.isBroken() && metadata.traceTime==(schema==1 ? -1 : 1500) &&
                metadata.traceCulprit==(schema==1 ? "" : "NPC_DESTRUCTION_TEST"),
                "legacy debris must have unknown age and actor; v2 must retain engine truth");
        const auto savedAgain=saveArchive([&](Serialize& output) { loaded.saveMovableVobs(output); });
        DynamicWorld reloaded(nullptr,asset::WorldMesh{});
        auto reloadedObject=make(reloaded,at(0,1000,0));
        loadMovables(reloaded,savedAgain);
        const auto repeated=reloaded.movableVobSnapshot()[0];
        require(reloadedObject.isBroken() && repeated.traceTime==metadata.traceTime &&
                repeated.traceCulprit==metadata.traceCulprit,
                "saving loaded debris again must preserve both unknown and recorded break metadata");
        }
      DynamicWorld movementOff(nullptr,asset::WorldMesh{});
      movementOff.setMovableVobsEnabled(false);
      auto offObject=make(movementOff,at(0,1000,0));
      loadMovables(movementOff,saved);
      const auto offPieces=movementOff.fragmentSnapshot();
      require(offObject.isBroken() && offPieces.size()==pieces.size(),
              "movable-vobs=0 must restore destruction independently of intact prop simulation");
      for(size_t i=0;i<pieces.size();++i) {
        const auto& restored=offPieces[i];
        requireMatrix(restored.transform,pieces[i].transform,"movement OFF loads must preserve debris pose");
        requireNear((restored.velocity-pieces[i].velocity).length(),0,.001f,
                    "movement OFF loads must preserve awake debris velocity");
        }
      const auto offSave=saveArchive([&](Serialize& output) { movementOff.saveMovableVobs(output); });
      DynamicWorld offReloaded(nullptr,asset::WorldMesh{});
      auto offReloadedObject=make(offReloaded,at(0,1000,0));
      loadMovables(offReloaded,offSave);
      require(offReloadedObject.isBroken() && offReloaded.fragmentSnapshot().size()==pieces.size(),
              "saving with movable-vobs=0 must retain the destruction archive entry");
      restored.setMovableVobsEnabled(false);
      require(restoredObject.isBroken() && restored.fragmentSnapshot().size()==pieces.size(),
              "disabling movable-vobs must not resurrect an already broken prop");
      restored.tick(17);
      require(restored.fragmentSnapshot()[0].transform.at(3,0)!=restoredPieces[0].transform.at(3,0),
              "awake debris must continue native integration with intact prop movement disabled");
      restored.setMovableVobsEnabled(true);
      require(restoredObject.isBroken(),"enabling movable-vobs must not resurrect broken props either");
      DynamicWorld disabled(nullptr,asset::WorldMesh{});
      disabled.setDestructionEnabled(false);
      auto disabledObject=make(disabled,at(0,1000,0));
      loadMovables(disabled,saved);
      require(!disabledObject.isBroken() && disabled.fragmentSnapshot().empty(),
              "destruction=0 must ignore destruction archive records and keep intact objects");
      world.setDestructionEnabled(false);
      require(!world.destructionEnabled() && !object.isBroken() && !brokenCallback && world.fragmentSnapshot().empty(),
              "disabling destruction must restore intact visuals and remove transient fragments");
      require(object.addImpulse({mass*20.f,0,0}),"disabled furniture must retain ordinary impulse physics");
      world.tick(17);
      require(!object.isBroken(),"destruction=0 must ignore otherwise destructive impulses");
      }
    {
      auto source=movableCollisionBox({100,100,100});
      source.sub_meshes[0].mat.group=asset::MaterialGroup::STONE;
      ProtoMesh barrel(PackedMesh(source,PackedMesh::PK_Visual),"NW_HARBOUR_BARREL_01.MRM");
      DynamicWorld world(nullptr,asset::WorldMesh{});
      auto object=world.addMovableVob(barrel,"NW_HARBOUR_BARREL_01.MRM",at(0,1000,0),.03f,true,"barrel");
      std::vector<PhysicsNoise> noises;
      world.setPhysicsNoiseCallback([&](const PhysicsNoise& noise) { noises.push_back(noise); });
      requireNear(object.mass(),DynamicWorld::materialDensity(asset::MaterialGroup::STONE)*.03f,.01f,
                  "destructible class must not change the authored mesh density");
      object.addImpulse({object.mass()*10.f,0,0});
      world.tick(17);
      require(object.isBroken() && !noises.empty() && noises.back().material==MAT_WOOD &&
              noises.back().surface==asset::MaterialGroup::WOOD,
              "a wooden barrel mislabeled as stone must still emit a wooden break sound");
      }
    for(const bool tilted:{false,true}) for(const float height:{100.f,500.f}) {
      auto floor=movableFloor();
      if(tilted)
        for(auto& vertex:floor.vertices) vertex.y=vertex.x*std::tan(.5f*3.141592654f/180.f);
      DynamicWorld world(nullptr,floor);
      auto object=make(world,at(0,height+50.f,0));
      require(object.addImpulse({0,-.001f,0}),"drop fixture must wake without structural damage");
      for(int i=0;i<900;++i) world.tick(17);
      const auto state=world.movableVobSnapshot()[0];
      std::printf("Destruction drop %.0f cm, floor tilt %.1f deg: broken %d, health %.3f, fragments %zu\n",
                  double(height),tilted ? .5 : 0.,int(state.broken),double(state.health),world.fragmentSnapshot().size());
      require(state.broken==(height>100.f),"a one metre fall must survive and a five metre fall must break");
      for(const auto& piece:world.fragmentSnapshot())
        require(piece.sleeping,"fragments must settle and sleep after fifteen seconds");
      }
    {
      DynamicWorld world(nullptr,movableFloor());
      world.setTraceTime(1500);
      world.setFragmentObserver({0,0,0});
      auto object=make(world,at(0,550,0));
      object.addImpulse({0,-.001f,0});
      for(int i=0;i<900;++i) world.tick(17);
      const auto settled=world.fragmentSnapshot();
      require(object.isBroken() && settled.size()==8,"cleanup fixture must break and settle without retiring pieces early");
      size_t permanentCount=0;
      for(const auto& piece:settled) {
        require(piece.sleeping && piece.asleepSince==1500,"a sleeping fragment must record its absolute game minute");
        permanentCount+=piece.permanent;
        }
      require(permanentCount==3,"three largest cache pieces must be marked permanent");
      world.setTraceTime(1529);
      world.tick(17);
      require(world.fragmentSnapshot().size()==8,"sleeping debris must remain during the first twenty-nine game minutes");
      const auto saved=saveArchive([&](Serialize& output) { world.saveMovableVobs(output); });
      DynamicWorld restored(nullptr,movableFloor());
      restored.setTraceTime(1529);
      restored.setFragmentObserver({0,0,0});
      auto restoredObject=make(restored,at(0,550,0));
      loadMovables(restored,saved);
      const auto loaded=restored.fragmentSnapshot();
      require(restoredObject.isBroken() && loaded.size()==settled.size(),"sleeping fragments must restore in a fresh world");
      for(size_t i=0;i<loaded.size();++i) {
        requireMatrix(loaded[i].transform,settled[i].transform,"sleeping debris must retain its complete saved pose");
        require(loaded[i].asleepSince==1500 && loaded[i].permanent==settled[i].permanent,
                "loading must preserve cleanup age and permanent-piece selection");
        }
      restored.setTraceTime(1530);
      restored.tick(17);
      const auto retained=restored.fragmentSnapshot();
      require(retained.size()==3,"thirty game minutes must remove only nonpermanent sleeping debris");
      require(restored.movableVobSnapshot()[0].traceTime==1500,
              "debris cleanup must retain the original destruction minute");
      for(const auto& piece:retained) {
        require(piece.permanent,"cleanup must retain the three permanent piece indices");
        requireMatrix(piece.transform,settled[piece.piece].transform,"cleanup must not move the permanent pieces");
        }
      const auto cleanedSave=saveArchive([&](Serialize& output) { restored.saveMovableVobs(output); });
      DynamicWorld cleaned(nullptr,movableFloor());
      cleaned.setTraceTime(1530);
      auto cleanedObject=make(cleaned,at(0,550,0));
      std::vector<Tempest::Matrix4x4> rendered;
      cleanedObject.setFractureCallback([&](bool broken,const std::vector<Tempest::Matrix4x4>& pieces) {
        if(broken) rendered=pieces;
        });
      loadMovables(cleaned,cleanedSave);
      require(cleanedObject.isBroken() && cleaned.fragmentSnapshot().size()==3 && rendered.size()==8,
              "cleaned saves must not resurrect removed bodies or lose the render index mapping");
      require(cleaned.movableVobSnapshot()[0].traceTime==1500,
              "loading the permanent debris must preserve its original break time");
      size_t hidden=0;
      for(const auto& matrix:rendered) hidden+=matrix.at(3,1)<-1000000.f;
      require(hidden==5,"loading cleaned debris must hide removed render batches immediately");
      cleaned.setTraceTime(5000);
      cleaned.setFragmentObserver({100000,0,0});
      cleaned.tick(17);
      require(cleaned.fragmentSnapshot().size()==3,"permanent pieces must survive later age and distance cleanup");

      DynamicWorld distant(nullptr,movableFloor());
      distant.setTraceTime(1529);
      auto distantObject=make(distant,at(0,550,0));
      loadMovables(distant,saved);
      distant.setFragmentObserver({10000,0,0});
      distant.tick(17);
      require(distantObject.isBroken() && distant.fragmentSnapshot().size()==3,
              "leaving farther than thirty metres must retire sleeping pieces before their age expires");
      DynamicWorld offCleanup(nullptr,movableFloor());
      offCleanup.setMovableVobsEnabled(false);
      offCleanup.setTraceTime(1530);
      auto offCleanupObject=make(offCleanup,at(0,550,0));
      loadMovables(offCleanup,saved);
      offCleanup.tick(17);
      require(offCleanupObject.isBroken() && offCleanup.fragmentSnapshot().size()==3,
              "sleeping debris cleanup must continue with intact prop movement disabled");

      DynamicWorld backwards(nullptr,movableFloor());
      backwards.setTraceTime(1000);
      auto backwardsObject=make(backwards,at(0,550,0));
      loadMovables(backwards,saved);
      backwards.tick(17);
      require(backwardsObject.isBroken() && backwards.fragmentSnapshot().size()==8 &&
              backwards.fragmentSnapshot()[0].asleepSince==1000,
              "a backwards game clock must restart cleanup age without removing fragments");
      }
    for(const bool movable:{false,true}) {
      DynamicWorld world(nullptr,movableFloor());
      world.setMovableVobsEnabled(movable);
      const auto origin=at(100,50,0);
      auto fence=world.addMovableVob(proto,"NW_NATURE_FENCE_01.MRM",origin,.04f,true,"fence",true,zenId);
      require(!fence.isEmpty() && fence.mass()==0.f,"an intact fence must keep a static body with movable-vobs both off and on");
      require(!fence.addImpulse({10000,0,0}),"an intact fence must never receive free-body impulses");
      for(int i=0;i<120;++i) world.tick(17);
      requireMatrix(world.movableVobSnapshot()[0].transform,origin,"an intact fence must remain exactly at its authored pose");
      require(world.navigationRay({0,50,0},{200,50,0},nullptr).hasCol,"an intact fence must obstruct native navigation rays");
      world.meleeMovableVobs({0,50,0},{1,0,0},150,40,100,FractureRule::Melee::OneHand);
      const auto damaged=saveArchive([&](Serialize& output) { world.saveMovableVobs(output); });
      DynamicWorld damagedWorld(nullptr,movableFloor());
      damagedWorld.setMovableVobsEnabled(movable);
      auto damagedFence=damagedWorld.addMovableVob(proto,"NW_NATURE_FENCE_01.MRM",origin,.04f,true,"fence",true,zenId);
      loadMovables(damagedWorld,damaged);
      requireNear(damagedWorld.movableVobSnapshot()[0].health,30.f,.001f,"partially damaged static fences must retain health in saves");
      damagedWorld.meleeMovableVobs({0,50,0},{1,0,0},150,40,100,FractureRule::Melee::OneHand);
      damagedWorld.tick(17);
      require(damagedFence.isBroken() && damagedWorld.fragmentSnapshot().size()==8 &&
              !damagedWorld.navigationRay({0,50,0},{200,50,0},nullptr).hasCol,
              "a broken static fence must remove both intact obstacles and open the passage");
      const auto saved=saveArchive([&](Serialize& output) { damagedWorld.saveMovableVobs(output); });
      DynamicWorld loaded(nullptr,movableFloor());
      loaded.setMovableVobsEnabled(movable);
      auto loadedFence=loaded.addMovableVob(proto,"NW_NATURE_FENCE_01.MRM",origin,.04f,true,"fence",true,zenId);
      loadMovables(loaded,saved);
      require(loadedFence.isBroken() && loaded.fragmentSnapshot().size()==8,
              "broken fences must restore even when movable-vobs is disabled");
      loaded.setMovableVobsEnabled(!movable);
      require(loadedFence.isBroken(),"toggling movable-vobs must not resurrect a broken static fence");
      }
    for(const bool fence:{false,true}) {
      const auto origin=at(100,50,0);
      const auto add=[&](DynamicWorld& world) {
        return world.addMovableVob(proto,fence ? "NW_NATURE_FENCE_01.MRM" : "NW_HARBOUR_CRATE_01.MRM",
                                   origin,.04f,true,fence ? "fence" : "crate",true,zenId);
        };
      const auto passage=[&](DynamicWorld& world,bool blocked) {
        const Tempest::Vec3 from(0,50,0),to(200,50,0);
        require(world.ray(from,to).hasCol==blocked,
                "only an intact source mesh may obstruct a visual ray through the original volume");
        require(world.navigationRay(from,to,nullptr).hasCol==blocked,
                "only an intact native hull may obstruct a navigation ray through the original volume");
        require(world.rayArrow(from,to,[](Npc&,const auto&,const auto&,float&) { return false; }).hasCol==blocked,
                "arrow aiming must agree with the surviving source collision after destruction or load");
        struct ArrowContact final : DynamicWorld::BulletCallback {
          bool stopsAtWorldHit() const override { return true; }
          void onWorldHit(const DynamicWorld::BulletWorldHit&) override { ++hits; }
          int hits=0;
          } contact;
        auto* arrow=world.bulletObj(&contact);
        arrow->setPosition(from);
        arrow->setDirection({3,0,0});
        for(int i=0;i<5;++i) world.tick(17);
        require(contact.hits==(blocked ? 1 : 0),
                "live arrows must stop on intact source collision and pass through a broken original");
        if(!blocked)
          require(arrow->position().x>to.x,"a live arrow must cross the entire former obstacle without stopping");
        world.deleteObj(arrow);

        zenkit::ModelHierarchy hierarchy{};
        // Legacy walking tests the upper capsule, with its feet above the
        // logical root. Keep that capsule overlapping the 100 cm fixture.
        hierarchy.collision_bbox={{-25,0,-25},{25,140,25}};
        Skeleton shape(asset::fromZenKit(hierarchy),nullptr,"DESTRUCTION_PASSAGE_WALKER");
        auto walker=world.addGhostObj(&shape);
        walker.setPosition({0,0,0});
        DynamicWorld::CollisionTest collision;
        require(walker.testMove({60,0,0},collision)!=blocked,
                "legacy walking must retain intact collision and cross the former obstacle after destruction or load");
        if(!blocked) {
          for(float x=20;x<=200;x+=20)
            require(walker.tryMove({x,0,0},collision)==DynamicWorld::MC_OK,
                    "legacy walking must complete each step through a destroyed obstacle");
          requireNear(walker.position().x,200,.001f,"legacy walking must reach the far side of the opened passage");
          walker.setPosition({0,0,0});
          for(int i=0;i<50;++i) walker.moveVirtual({5,0,0},17,50,.785398163f,80);
          require(walker.position().x>200,
                  "native walking must cross the original hull after destruction, cleanup and fresh load");
          }
        };
      DynamicWorld world(nullptr,movableFloor());
      world.setTraceTime(1500);
      world.setFragmentObserver({0,0,0});
      auto object=add(world);
      passage(world,true);
      require(world.meleeMovableVobs({0,50,0},{1,0,0},150,40,1000,FractureRule::Melee::OneHand)==1,
              "the passage fixture must strike its intact obstacle");
      world.tick(17);
      require(object.isBroken() && world.fragmentSnapshot().size()==8,
              "the passage fixture must install fragments before checking collision consumers");
      passage(world,false);
      const auto saved=saveArchive([&](Serialize& output) { world.saveMovableVobs(output); });
      DynamicWorld restored(nullptr,movableFloor());
      restored.setTraceTime(1500);
      restored.setFragmentObserver({0,0,0});
      auto restoredObject=add(restored);
      loadMovables(restored,saved);
      require(restoredObject.isBroken(),"the passage fixture must restore destruction in a fresh world");
      passage(restored,false);
      for(int i=0;i<900;++i) restored.tick(17);
      restored.setTraceTime(1530);
      restored.tick(17);
      require(restored.fragmentSnapshot().size()==3,
              "the passage fixture must retire sleeping debris before saving the cleaned state");
      passage(restored,false);
      const auto cleanedSave=saveArchive([&](Serialize& output) { restored.saveMovableVobs(output); });
      DynamicWorld cleaned(nullptr,movableFloor());
      auto cleanedObject=add(cleaned);
      loadMovables(cleaned,cleanedSave);
      require(cleanedObject.isBroken() && cleaned.fragmentSnapshot().size()==3,
              "loading cleaned debris must retain the opened passage and only its three remaining pieces");
      passage(cleaned,false);
      }
    {
      DynamicWorld world(nullptr,asset::WorldMesh{});
      auto object=make(world,at(0,1000,0));
      int arrows=0;
      for(;arrows<8 && !object.isBroken();++arrows) {
        const auto before=world.movableVobSnapshot()[0].transform;
        const Tempest::Vec3 center(before.at(3,0),before.at(3,1),before.at(3,2));
        DynamicWorld::BulletCallback contact;
        auto* arrow=world.bulletObj(&contact);
        arrow->setPosition(center+Tempest::Vec3(-80,0,0));
        arrow->setDirection({3,0,0});
        world.tick(17);
        world.deleteObj(arrow);
        }
      std::printf("Destruction arrows: %d\n",arrows);
      require(object.isBroken() && arrows==4,"four standard-speed arrows must break the crate through real flight contacts");
      }
    {
      DynamicWorld world(nullptr,movableFloor());
      auto object=make(world,at(100,50,0));
      auto walker=world.movableVobSnapshot()[0];
      zenkit::ModelHierarchy hierarchy{};
      hierarchy.collision_bbox={{-25,0,-25},{25,180,25}};
      Skeleton shape(asset::fromZenKit(hierarchy),nullptr,"DESTRUCTION_WALKER");
      auto hero=world.addGhostObj(&shape);
      hero.setPosition({0,0,0});
      for(int i=0;i<180;++i) {
        hero.moveVirtual({5,0,0},17,50,.785398163f,80);
        world.tick(17);
        }
      require(!object.isBroken() && world.movableVobSnapshot()[0].health==walker.health,
              "walking into a crate must not consume structural health");
      }
    {
      DynamicWorld world(nullptr,waterBasin());
      const auto birth=at(0,110,0);
      auto object=make(world,birth);
      require(object.addImpulse({0,-object.mass()*.1f,0}),"the floating destruction fixture must enter water dynamically");
      tickWater(world,1800);
      require(object.isSleeping(),"a wooden crate must be floating and settled before destruction");
      requireNear(waterBody(world.waterBodySnapshot(),zenId).submergedFraction,.5f,.04f,
                  "the destruction fixture must settle with half of its crate volume submerged");
      requireNear(object.position().y,0.f,2.5f,"the half-submerged destruction crate's centre must settle at the surface");
      const auto fracture=object.fractureMesh();
      require(fracture!=nullptr,"the floating crate must retain its preloaded native fragment hulls");
      require(world.meleeMovableVobs({-140,object.position().y,0},{1,0,0},150,40,1000,FractureRule::Melee::OneHand)==1,
              "the water fixture must destroy its floating crate through the normal damage path");
      world.tick(17);
      require(object.isBroken() && world.fragmentSnapshot().size()==8,
              "breaking a floating crate must install every native fragment");
      std::array<int,8> firstSleep;
      firstSleep.fill(-1);
      for(int frame=0;frame<1800;++frame) {
        world.tick(frame%3==2 ? 16 : 17);
        for(const auto& piece:world.fragmentSnapshot())
          if(piece.piece<firstSleep.size() && piece.sleeping && firstSleep[piece.piece]<0)
            firstSleep[piece.piece]=frame+1;
        }
      const auto pieces=world.fragmentSnapshot();
      const auto floatingWater=world.waterBodySnapshot();
      require(pieces.size()==8,"nearby floating fragments must remain present after settling");
      for(const auto& piece:pieces) {
        auto center=fracture->pieces[piece.piece].center;
        piece.transform.project(center);
        const auto& body=waterBody(floatingWater,zenId,piece.piece);
        std::printf("Water crate fragment %zu: centre %.6f cm, submerged %.6f, sleeping %d, speed %.6f cm/s, first sleep %.3f s\n",
                    piece.piece,double(center.y),double(body.submergedFraction),int(piece.sleeping),
                    double(piece.velocity.length()),double(firstSleep[piece.piece])/60.);
        }
      for(const auto& piece:pieces) {
        auto center=fracture->pieces[piece.piece].center;
        piece.transform.project(center);
        require(piece.sleeping && center.y>-50.f && center.y<100.f,
                "every wooden crate fragment must float and sleep instead of falling to the bottom");
        require(firstSleep[piece.piece]>0,
                "every wooden crate fragment must first sleep within thirty seconds");
        requireNear(waterBody(floatingWater,zenId,piece.piece).submergedFraction,.6f,.05f,
                    "every wooden crate fragment must settle with sixty percent of its actual hull submerged");
        }
      const auto saved=saveArchive([&](Serialize& output) { world.saveMovableVobs(output); });
      DynamicWorld disabledRestore(nullptr,waterBasin());
      disabledRestore.setBuoyancyEnabled(false);
      auto disabledObject=make(disabledRestore,birth);
      loadMovables(disabledRestore,saved);
      require(disabledObject.isBroken() && disabledRestore.fragmentSnapshot().size()==pieces.size(),
              "loading buoyancy=0 must preserve the broken parent and all saved floating fragments");
      tickWater(disabledRestore,1800);
      const auto sunken=disabledRestore.fragmentSnapshot();
      require(sunken.size()==pieces.size(),"all buoyancy=0 fragments must survive while settling near the observer");
      for(const auto& piece:sunken) {
        float highest=-std::numeric_limits<float>::infinity();
        for(const auto& triangle:fracture->pieces[piece.piece].triangles)
          for(const auto& vertex:triangle.vertices) {
            auto point=vertex.position;
            piece.transform.project(point);
            highest=std::max(highest,point.y);
            }
        // Cut pieces can stack at the bottom. They must all become fully
        // submerged and stop, rather than each touching the same floor plane.
        require(piece.sleeping && highest<0.f,
                "loading floating fragment sleepers into a fresh buoyancy=0 world must let every piece sink and settle underwater");
        }
      tickWater(disabledRestore,1800);
      const auto stillSunken=disabledRestore.fragmentSnapshot();
      require(stillSunken.size()==sunken.size(),"settled buoyancy=0 fragments must remain present for thirty more seconds");
      for(size_t i=0;i<sunken.size();++i) {
        require(stillSunken[i].sleeping,"sunken buoyancy=0 fragment stacks must remain asleep");
        requireMatrix(stillSunken[i].transform,sunken[i].transform,
                      "sunken buoyancy=0 fragment stacks must remain completely motionless for thirty more seconds");
        }
      DynamicWorld restored(nullptr,waterBasin());
      auto restoredObject=make(restored,birth);
      loadMovables(restored,saved);
      require(restoredObject.isBroken(),"loading floating debris must preserve the broken parent");
      tickWater(restored,1800);
      const auto loaded=restored.fragmentSnapshot();
      const auto loadedWater=restored.waterBodySnapshot();
      require(loaded.size()==pieces.size(),"fresh-world load must restore every sleeping floating fragment");
      for(size_t i=0;i<pieces.size();++i) {
        require(loaded[i].sleeping,"loaded floating fragments must remain asleep over thirty seconds");
        requireMatrix(loaded[i].transform,pieces[i].transform,
                      "loaded floating fragments must retain their complete settled poses over thirty seconds");
        requireNear(waterBody(loadedWater,zenId,loaded[i].piece).submergedFraction,.6f,.05f,
                    "restored wooden crate fragments must retain sixty percent hull immersion");
        }
      }
    {
      setenv("OPENGOTHIC_FRACTURE","",1);
      DynamicWorld world(nullptr,asset::WorldMesh{});
      auto object=make(world,at(0,1000,0));
      require(object.fractureMesh()==nullptr,"an explicitly empty cache path must disable cache lookup");
      object.addImpulse({object.mass()*20.f,0,0});
      world.tick(17);
      require(!object.isBroken() && world.fragmentSnapshot().empty(),
              "uncached visuals must preserve intact legacy physics even after a large impulse");
      }
    }

  void testRotatingFragments() {
    CollisionWorld world;
    // A single-sided terrain mesh, rather than a thick box that catches an
    // already penetrating piece from below. The wedge has no asset dependency.
    constexpr float slope=.00872686779f; // half a degree
    JPH::TriangleList triangles;
    // Triangle seams on mildly uneven ground reproduce rotational tunnelling;
    // a single flat pair of triangles misses the failure even at the same spin.
    const auto terrain=[](float x,float z) { return slope*x+.03f*std::sin(2.3f*x)*std::cos(1.7f*z); };
    for(int x=-40;x<40;++x) for(int z=-40;z<40;++z) {
      const auto point=[&](int dx,int dz) {
        const float px=float(x+dx)*.5f,pz=float(z+dz)*.5f;
        return JPH::Float3(px,terrain(px,pz),pz);
        };
      triangles.emplace_back(point(0,0),point(0,1),point(1,0));
      triangles.emplace_back(point(1,0),point(0,1),point(1,1));
      }
    const auto floorShape=JPH::MeshShapeSettings(triangles).Create();
    require(!floorShape.HasError(),"the fragment rotation fixture needs one-sided terrain");
    auto floor=world.addCollisionBody(floorShape.Get(),at(0,0,0),.8f,DynamicWorld::C_Landscape);
    const std::vector<std::vector<JPH::Vec3>> clouds={
      {{-.15f,-.08f,-.16f},{.15f,-.08f,-.16f},{-.15f,.40f,-.16f},
       {.15f,.40f,-.16f},{-.15f,.40f,.16f},{.15f,.40f,.16f}},
      // Narrow pieces and long boards have a much larger sweep radius relative
      // to their inner sphere than the original fixture's compact wedge.
      {{-.02f,-.08f,-.16f},{.02f,-.08f,-.16f},{-.02f,.40f,-.16f},
       {.02f,.40f,-.16f},{-.02f,.40f,.16f},{.02f,.40f,.16f}},
      {{-.08f,-.60f,-.12f},{.08f,-.60f,-.12f},{-.08f,.60f,-.12f},
       {.08f,.60f,-.12f},{-.08f,.60f,.12f},{.08f,.60f,.12f}}
      };
    std::vector<std::unique_ptr<CollisionWorld::DynamicBody>> fragments;
    std::vector<size_t> cloudByBody;
    std::unique_ptr<CollisionWorld::DynamicBody> ordinary;
    for(size_t kind=0;kind<clouds.size();++kind) {
      const auto& points=clouds[kind];
      const auto shape=JPH::ConvexHullShapeSettings(points.data(),int(points.size()),.001f).Create();
      require(!shape.HasError(),"the fragment rotation wedge must have a native convex hull");
      if(kind==0) {
        ordinary=world.addDynamicBody(shape.Get(),at(0,5000,0),.8f,1.f,DynamicWorld::C_Item);
        ordinary->setVelocity({0,0,0},{47,0,0});
        }
      for(int axis=0;axis<3;++axis) for(int orientation=0;orientation<8;++orientation) {
        const float x=float(orientation-4)*250.f,z=float(axis-1)*250.f+(float(kind)-1.f)*800.f;
        auto rotation=Tempest::Matrix4x4::mkIdentity();
        rotation.rotateOX(float(orientation)*45.f);
        rotation.rotateOZ(float(axis)*37.f);
        float lowest=std::numeric_limits<float>::max();
        for(const auto& point:points) {
          Tempest::Vec3 rotated(point.GetX()*100.f,point.GetY()*100.f,point.GetZ()*100.f);
          rotation.project(rotated);
          lowest=std::min(lowest,rotated.y-slope*rotated.x);
          }
        auto matrix=at(x,slope*x-lowest+4.f,z);
        matrix.mul(rotation);
        auto body=world.addDynamicBody(shape.Get(),matrix,.8f,1.f,DynamicWorld::C_Fragment);
        const Tempest::Vec3 omega=axis==0 ? Tempest::Vec3(47.f,0,0) :
                                  axis==1 ? Tempest::Vec3(0,0,47.f) : Tempest::Vec3(27.f,27.f,27.f);
        body->setVelocity({200,-2000,100},omega);
        fragments.push_back(std::move(body));
        cloudByBody.push_back(kind);
        }
    }
    float lowestCenter=std::numeric_limits<float>::max();
    float lowestVertex=std::numeric_limits<float>::max();
    for(int i=0;i<300;++i) {
      world.tick(17);
      if(i==0)
        require(ordinary->angularVelocity().length()>40.f,
                "fragment stability must not limit ordinary item angular motion");
      for(size_t piece=0;piece<fragments.size();++piece) {
        const auto& body=fragments[piece];
        const auto center=body->centerOfMass();
        lowestCenter=std::min(lowestCenter,center.y-terrain(center.x*.01f,center.z*.01f)*100.f);
        const auto matrix=body->matrix();
        for(const auto& point:clouds[cloudByBody[piece]]) {
          Tempest::Vec3 vertex(point.GetX()*100.f,point.GetY()*100.f,point.GetZ()*100.f);
          matrix.project(vertex);
          lowestVertex=std::min(lowestVertex,vertex.y-terrain(vertex.x*.01f,vertex.z*.01f)*100.f);
          }
        }
      }
    std::printf("Rotating fragments: %zu convex wedges/boards, minimum centre %.3f cm, vertex %.3f cm over one-sided floor\n",
                fragments.size(),double(lowestCenter),double(lowestVertex));
    require(lowestCenter>=0.f,"a rapidly rotating convex fragment must never fall through one-sided terrain");
    require(lowestVertex>=-6.f,"rotating fragment vertices must stay within the native contact band of uneven terrain");
    }

  void testMovableVobLegacyCollision() {
    // Exercise the real mesh cache and rebuild/toggle path, rather than constructing
    // query proxies directly. No game assets or graphics device are required.
    Resources resources;
    ProtoMesh proto(PackedMesh(movableCube(),PackedMesh::PK_Visual),"CONTRACT_CUBE.MRM");
    zenkit::ModelHierarchy hierarchy{};
    hierarchy.collision_bbox = {{-5,-10,-5},{5,10,5}};
    Skeleton walkerShape(asset::fromZenKit(hierarchy),nullptr,"CONTRACT_WALKER");
    for(const bool blocksWalking:{false,true}) {
      for(const bool legacyCollision:{false,true}) {
        DynamicWorld world(nullptr,asset::WorldMesh{});
        world.setMovableVobsEnabled(false);
        auto object = world.addMovableVob(proto,"CONTRACT_CUBE.MRM",at(0,100,0),.25f,
                                          blocksWalking,blocksWalking ? "table" : "utensil",legacyCollision);
        require(!object.isEmpty(),"disabled movable vobs must retain their handle for re-enabling");
        for(const bool enabled:{false,true,false,true}) {
          world.setMovableVobsEnabled(enabled);
          const auto snapshot = world.movableVobSnapshot();
          require(snapshot.size()==1 && snapshot[0].legacyCollision==legacyCollision,
                  "movable snapshots must retain the original cd_dynamic flag across toggles");
          require(snapshot[0].enabled==enabled && (object.mass()>0)==enabled,
                  "cd_dynamic must not suppress an enabled movable simulation hull");
          require(world.physicsStats().bodies==size_t(enabled)+size_t(legacyCollision),
                  "only cd_dynamic vobs may create query bodies in either mode");
          require(object.isSleeping(),"rebuilding a movable vob must start its simulation asleep");
          const Tempest::Vec3 from{-30,100,0},to{30,100,0};
          require(world.ray(from,to).hasCol==legacyCollision,
                  "camera, foot IK and visibility rays must preserve original mesh collision eligibility");
          require(world.rayArrow(from,to,[](Npc&,const Tempest::Vec3&,const Tempest::Vec3&,float&) { return false; }).hasCol==legacyCollision,
                  "arrow rays must preserve original mesh collision eligibility");
          const bool groundHit = legacyCollision && (!enabled || blocksWalking);
          require(world.landRay({0,130,0},60).hasCol==groundHit,
                  "landRay must ignore non-colliding vobs and enabled small utensils");
          require(world.sweep({-30,100,0},{30,100,0},2).hasCol==groundHit,
                  "movement sweeps must preserve original mesh collision eligibility");
          auto walker = world.addGhostObj(&walkerShape);
          walker.setPosition({12,70,0});
          require(walker.hasCollision()==groundHit,
                  "walking must ignore cd_dynamic=0 vobs for large furniture and utensils alike");
          if(enabled) {
            require(object.addImpulse({object.mass(),0,0}),
                    "both colliding and non-colliding movable vobs must accept physical impulses");
            require(!object.isSleeping() && object.velocity().x>0,
                    "cd_dynamic=0 hulls must remain active simulation bodies after an impulse");
            }
          }
        }

      DynamicWorld world(nullptr,asset::WorldMesh{});
      auto moving = world.addMovableVob(proto,"CONTRACT_CUBE.MRM",at(0,100,0),.25f,
                                        blocksWalking,"impactor",false);
      auto target = world.addMovableVob(proto,"CONTRACT_CUBE.MRM",at(30,100,0),.25f,
                                        blocksWalking,"target",false);
      require(!moving.isEmpty() && !target.isEmpty() && target.isSleeping(),
              "physical contact fixture must start with two sleeping cd_dynamic=0 hulls");
      require(moving.addImpulse({moving.mass()*2.f,0,0}),"contact fixture must wake the impactor");
      for(int i=0;i<15;++i)
        world.tick(17);
      require(!target.isSleeping() && target.velocity().x>1.f,
              "a cd_dynamic=0 movable hull must wake and push another hull through physical contact");
      require(world.physicsStats().bodies==2,
              "physical contact between cd_dynamic=0 hulls must not require query proxies");
      }
    }

  void testMovableVobBodies() {
    CollisionWorld world;
    const JPH::Vec3 points[] = {
      {-.1f,-.1f,-.1f},{-.1f,-.1f,.1f},{-.1f,.1f,-.1f},{-.1f,.1f,.1f},
      {.1f,-.1f,-.1f},{.1f,-.1f,.1f},{.1f,.1f,-.1f},{.1f,.1f,.1f}
      };
    const auto hull = JPH::ConvexHullShapeSettings(points,8,0.f).Create();
    require(!hull.HasError(),"movable vob convex hull must be constructible");
    const float mass = hull.Get()->GetVolume()*DynamicWorld::materialDensity(asset::MaterialGroup::WOOD)*.25f;
    requireNear(mass,.008f*700.f*.25f,.00001f,"mass must use convex hull volume and class multiplier");
    auto floor = world.addCollisionBody(new JPH::BoxShape({10,.1f,10}),at(0,-10,0),.65f,DynamicWorld::C_Landscape);
    auto movable = world.addDynamicBody(hull.Get(),at(0,100,0),.5f,mass,DynamicWorld::C_MovableVob,true);
    const auto initial = movable->position();
    for(int i=0;i<3600;++i)
      world.tick(17);
    require(movable->isSleeping(),"fresh movable vobs must stay asleep for 60 seconds");
    require((movable->position()-initial).length()<=.001f,"sleeping movable vobs must not drift under gravity");
    require(world.bodyCount()==2,"sleeping movable vobs must remain registered bodies");
    Tempest::Vec3 normal;
    Interactive* vob = nullptr;
    JPH::SphereShape walker(.05f);
    require(!world.hasCollision(&walker,{0,100,0},normal,vob),"convex simulation hull must not replace legacy walking queries");

    JPH::TriangleList triangles;
    triangles.emplace_back(JPH::Float3(-.2f,0,-.2f),JPH::Float3(0,0,.2f),JPH::Float3(.2f,0,-.2f));
    const auto queryMesh = JPH::MeshShapeSettings(triangles).Create();
    require(!queryMesh.HasError(),"movable source query mesh must be constructible");
    auto smallQuery = world.addCollisionBody(queryMesh.Get(),at(0,100,0),.5f,DynamicWorld::C_SmallVobQuery,true,true);
    require(!world.hasCollision(&walker,{0,101,0},normal,vob),"small utensil mesh must not obstruct walking");
    auto largeQuery = world.addCollisionBody(queryMesh.Get(),at(50,100,0),.5f,DynamicWorld::C_MovableQuery,true,true);
    require(world.hasCollision(&walker,{50,101,0},normal,vob),"large furniture must obstruct walking through its source mesh");
    const uint32_t queryMask = (1u<<DynamicWorld::C_MovableQuery)|(1u<<DynamicWorld::C_SmallVobQuery);
    require(world.rayCast({0,130,0},{0,90,0},queryMask).hasHit,"small vob source mesh must remain aimable");
    const std::vector<const CollisionWorld::CollisionBody*> excluded = {smallQuery.get()};
    require(!world.rayCast({0,130,0},{0,90,0},queryMask,false,nullptr,&excluded).hasHit,
            "spell visibility must exclude the target's own source mesh");
    require(movable->addImpulse({mass*2.f,mass*.5f,0}),"spell impulse must wake a movable vob");
    require(!movable->isSleeping(),"an impulse must activate a sleeping movable vob");
    for(int i=0;i<900;++i)
      world.tick(17);
    require(std::abs(movable->position().x-initial.x)>20.f,"a movable vob impulse must produce physical displacement");
    require(movable->isSleeping(),"movable vob must settle within 15 seconds after an impulse");
    require(world.bodyCount()==4,"settled movable vob must retain its registered body");

    // Source vertices keep attachment-local transforms before hull creation.
    std::vector<JPH::Vec3> source = {{1,2,3},{2,2,3},{1,3,3}};
    PhysicVbo sourceMesh(&source);
    sourceMesh.addIndex({0,1,2},0,3,asset::MaterialGroup::WOOD);
    auto local = at(100,200,300);
    local.scale(2,3,4);
    std::vector<JPH::Vec3> transformed;
    sourceMesh.appendVertices(transformed,local);
    require(transformed.size()==3,"hull input must retain every referenced source vertex");
    requireNear(transformed[0].GetX(),3.f,.00001f,"hull source input must apply local attachment X transform");
    requireNear(transformed[0].GetY(),8.f,.00001f,"hull source input must apply local attachment Y transform");
    requireNear(transformed[0].GetZ(),15.f,.00001f,"hull source input must apply local attachment Z transform");
    }

  struct SpherePrecisionCollector final : JPH::CastShapeCollector {
    void AddHit(const JPH::ShapeCastResult& result) override {
      if(!hasHit || result.mFraction<hit.mFraction) {
        hit = result;
        hasHit = true;
        }
      }
    JPH::ShapeCastResult hit;
    bool hasHit = false;
    };

  SpherePrecisionCollector castSphereTriangle(const std::array<JPH::Vec3,3>& triangle,
                                              JPH::Vec3Arg start, JPH::Vec3Arg direction,
                                              float radius, const JPH::ShapeCastSettings& settings,
                                              JPH::Vec3Arg scale = JPH::Vec3::sOne(),
                                              JPH::Mat44Arg transform = JPH::Mat44::sIdentity(),
                                              float cutoff = 1.f+FLT_EPSILON) {
    JPH::SphereShape sphere(radius);
    const JPH::ShapeCast cast(&sphere,JPH::Vec3::sOne(),JPH::Mat44::sTranslation(start),direction);
    SpherePrecisionCollector collector;
    collector.ResetEarlyOutFraction(cutoff);
    JPH::CastSphereVsTriangles query(cast,settings,scale,transform,JPH::SubShapeIDCreator(),collector);
    query.Cast(triangle[0],triangle[1],triangle[2],0b111,JPH::SubShapeID());
    return collector;
    }

  void testSphereQueryPrecision() {
    JPH::ShapeCastSettings defaults;
    defaults.SetBackFaceMode(JPH::EBackFaceMode::CollideWithBackFaces);
    defaults.mActiveEdgeMode = JPH::EActiveEdgeMode::CollideWithAll;
    require(!defaults.mUsePreciseSphereTriangleCast,"native sphere triangle precision must remain an explicit opt-in");
    auto disabled = defaults;
    disabled.mUsePreciseSphereTriangleCast = false;
    auto precise = defaults;
    precise.mUsePreciseSphereTriangleCast = true;

    // Query 2179, with the actual decoded child-local input. The complete segment
    // misses this triangle by 2.54 mm beyond the 5 cm sphere radius. Float products
    // make the edge quadratic discriminant positive; the precise query rejects it.
    const std::array<JPH::Vec3,3> separatedEdge = {
      JPH::Vec3(-39.3174744f,-4.08496571f,15.2057877f),
      JPH::Vec3(-39.835434f,-2.79672146f,16.8955078f),
      JPH::Vec3(-38.9809036f,-3.76561832f,15.1747742f)
      };
    const JPH::Vec3 edgeStart(61.133728f,1.71995163f,12.8343124f);
    const JPH::Vec3 edgeDirection(-217.075928f,-11.5681381f,5.39838409f);
    const auto nativeEdge = castSphereTriangle(separatedEdge,edgeStart,edgeDirection,.049999997f,defaults);
    const auto explicitNativeEdge = castSphereTriangle(separatedEdge,edgeStart,edgeDirection,.049999997f,disabled);
    require(nativeEdge.hasHit && explicitNativeEdge.hasHit,
            "default native edge behavior must remain available to callers without the precision opt-in");
    require(nativeEdge.hit.mFraction==explicitNativeEdge.hit.mFraction &&
            nativeEdge.hit.mContactPointOn2==explicitNativeEdge.hit.mContactPointOn2 &&
            nativeEdge.hit.mPenetrationAxis==explicitNativeEdge.hit.mPenetrationAxis,
            "default and explicitly disabled precision must preserve the full native edge result");
    require(!castSphereTriangle(separatedEdge,edgeStart,edgeDirection,.049999997f,precise).hasHit,
            "a sphere segment separated from an edge must reject a rounded false discriminant");

    // Query 37252 is a very narrow face. Float Gram products report an interior
    // point while the entire segment is 60.5 cm beyond the one-meter radius.
    const std::array<JPH::Vec3,3> separatedThinFace = {
      JPH::Vec3(.549634933f,3.00050545f,64.0834045f),
      JPH::Vec3(1.74020004f,3.00050545f,59.6401825f),
      JPH::Vec3(3.03430557f,3.00050545f,54.8105469f)
      };
    const JPH::Vec3 thinStart(1.00256348f,17.6009464f,56.1907692f);
    const JPH::Vec3 thinDirection(0,-20.4721184f,0);
    require(castSphereTriangle(separatedThinFace,thinStart,thinDirection,1.f,defaults).hasHit,
            "default precision must preserve the native narrow-face result");
    require(!castSphereTriangle(separatedThinFace,thinStart,thinDirection,1.f,precise).hasHit,
            "a sphere cast outside a narrow face must reject rounded interior barycentrics");

    const std::array<JPH::Vec3,3> triangle = {
      JPH::Vec3(0,0,0),JPH::Vec3(0,0,2),JPH::Vec3(2,0,0)
      };
    const auto edge = castSphereTriangle(triangle,{-.25f,2,1},{0,-4,0},.5f,precise);
    require(edge.hasHit,"precise casts must retain a genuine edge contact");
    requireNear(edge.hit.mFraction,(2.f-std::sqrt(.5f*.5f-.25f*.25f))/4.f,0.000001f,
                "edge entry fraction must match the sphere cross section");
    const auto tangent = castSphereTriangle(triangle,{-.5f,2,1},{0,-4,0},.5f,precise);
    require(tangent.hasHit,"precise casts must retain exact edge tangencies");
    requireNear(tangent.hit.mFraction,.5f,0.000001f,"tangent contact must lie at the triangle plane");
    require(!castSphereTriangle(triangle,{-std::nextafter(.5f,1.f),2,1},{0,-4,0},.5f,precise).hasHit,
            "one representable step beyond the tangent must miss");
    const auto vertex = castSphereTriangle(triangle,{-.25f,2,-.25f},{0,-4,0},.5f,precise);
    require(vertex.hasHit,"precise casts must retain a genuine vertex contact");
    requireNear(vertex.hit.mFraction,(2.f-std::sqrt(.5f*.5f-2.f*.25f*.25f))/4.f,0.000001f,
                "vertex entry fraction must match the sphere cross section");

    // The game's sweep collector accepts fractions strictly below one.
    require(!castSphereTriangle(triangle,{.5f,2,.5f},{0,-1.5f,0},.5f,precise,
                               JPH::Vec3::sOne(),JPH::Mat44::sIdentity(),1.f).hasHit,
            "the precise query must respect an exclusive endpoint cutoff");
    const auto beforeEnd = castSphereTriangle(triangle,{.5f,2,.5f},{0,-1.5001f,0},.5f,precise,
                                              JPH::Vec3::sOne(),JPH::Mat44::sIdentity(),1.f);
    require(beforeEnd.hasHit && beforeEnd.hit.mFraction<1.f,
            "a real contact immediately before the endpoint must survive");
    require(!castSphereTriangle(triangle,{.5f,2,.5f},{0,-1.4999f,0},.5f,precise,
                               JPH::Vec3::sOne(),JPH::Mat44::sIdentity(),1.f).hasHit,
            "a contact beyond the segment endpoint must miss");

    const auto transform = JPH::Mat44::sRotationTranslation(
        JPH::Quat::sRotation(JPH::Vec3::sAxisY(),.5f*JPH::JPH_PI),JPH::Vec3(10,20,-30));
    const auto mirrored = castSphereTriangle(triangle,{-1,2,1.5f},{0,-4,0},.25f,precise,
                                             JPH::Vec3(-2,.5f,3),transform);
    require(mirrored.hasHit,"precise sphere queries must retain contacts with mirrored nonuniform meshes");
    requireNear(mirrored.hit.mFraction,.4375f,.000001f,"mesh scale must preserve the plane entry fraction");
    requireNear(mirrored.hit.mContactPointOn2.GetX(),11.5f,.00001f,"converted contact must preserve rotated world X");
    requireNear(mirrored.hit.mContactPointOn2.GetY(),20.f,.00001f,"converted contact must preserve world Y");
    requireNear(mirrored.hit.mContactPointOn2.GetZ(),-29.f,.00001f,"converted contact must preserve rotated world Z");
    }

  // These 12 original terrain faces reproduce all 107 logged Bullet normals.
  // Source log values used %.9g, which round-trip to the exact binary32 bits.
  void testSourceTerrainNormals() {
    struct Fixture {
      JPH::Vec3 vertices[3];
      uint32_t expected[3];
      };
    const Fixture fixtures[] = {
      { { {88.6986008f,5.17765617f,121.866676f},
          {91.4136581f,2.34256101f,112.210762f},
          {86.9217529f,5.54688644f,119.338539f} },
        { 0x3ece733du,0x3f67077eu,0xbe1b3b85u } }, // original source triangle 243603
      { { {86.9217529f,5.54688644f,119.338539f},
          {83.5601044f,7.12607956f,127.19075f},
          {88.6986008f,5.17765617f,121.866676f} },
        { 0x3e95ea55u,0x3f743014u,0xbd882751u } }, // original source triangle 243690
      { { {83.5601044f,7.12607956f,127.19075f},
          {86.9217529f,5.54688644f,119.338539f},
          {81.0413666f,7.49531031f,124.353905f} },
        { 0x3e78f42au,0x3f773e8au,0xbdb8a232u } }, // original source triangle 243717
      { { {86.9217529f,5.54688644f,119.338539f},
          {91.4136581f,2.34256101f,112.210762f},
          {86.245079f,5.06495762f,118.298805f} },
        { 0xbc2262c0u,0x3f68e0a7u,0xbed49493u } }, // original source triangle 243511
      { { {80.3646851f,7.01338053f,123.314186f},
          {86.9217529f,5.54688644f,119.338539f},
          {86.245079f,5.06495762f,118.298805f} },
        { 0xbd1a570cu,0x3f6a6f8cu,0xbeccc585u } }, // original source triangle 243513
      { { {86.245079f,3.88401794f,118.298828f},
          {79.8359985f,4.35840225f,123.528473f},
          {80.3646851f,5.83244085f,123.314186f} },
        { 0xbf1f942bu,0x3de37085u,0xbf4625b5u } }, // original source triangle 50088
      { { {79.8359985f,4.35840225f,123.528473f},
          {86.245079f,3.88401794f,118.298828f},
          {83.1608734f,3.53122926f,116.41391f} },
        { 0xbd08c995u,0x3f7da809u,0xbe05f250u } }, // original source triangle 243510
      { { {86.9217529f,5.54688644f,119.338539f},
          {80.3646851f,7.01338053f,123.314186f},
          {81.0413666f,7.49531031f,124.353905f} },
        { 0xbd1a59e8u,0x3f6a6f6du,0xbeccc606u } }, // original source triangle 243512
      { { {79.8359985f,4.35840225f,123.528473f},
          {83.1608734f,3.53122926f,116.41391f},
          {78.0155869f,3.55711174f,116.974243f} },
        { 0xbc02ca6au,0x3f7e2aefu,0xbdf40bdeu } }, // original source triangle 243504
      { { {83.1608734f,3.53122926f,116.41391f},
          {83.4825745f,3.9189806f,108.036804f},
          {78.0155869f,3.55711174f,116.974243f} },
        { 0x3c258252u,0x3f7fb563u,0x3d3ef6feu } }, // original source triangle 243502
      { { {78.0155869f,3.55711174f,116.974243f},
          {72.7686081f,1.26712513f,120.608086f},
          {72.7628021f,4.55114794f,122.417816f} },
        { 0xbf21fc20u,0x3ebee96du,0xbf2dbd0eu } }, // original source triangle 50059
      { { {72.7628021f,4.55114794f,122.417816f},
          {79.8359985f,4.35840225f,123.528473f},
          {78.0155869f,3.55711174f,116.974243f} },
        { 0x3d44e327u,0x3f7d615au,0xbe099467u } }, // original source triangle 243531
      };
    const std::vector<uint32_t> indices = {0,2,1}; // addIndex preserves Gothic winding by swapping 1/2.
    for(const auto& fixture:fixtures) {
      const std::vector<JPH::Vec3> vertices(fixture.vertices,fixture.vertices+3);
      PhysicVbo mesh(&vertices);
      mesh.addIndex(indices,0,3,asset::MaterialGroup::STONE);
      JPH::Vec3 normal;
      require(mesh.rayTriangleNormal(0,normal),"source terrain normal must be finite and nonzero");
      for(JPH::uint axis=0;axis<3;++axis)
        require(std::bit_cast<uint32_t>(normal[axis])==fixture.expected[axis],
                "source terrain normal must retain original Bullet float bits");
      require(!mesh.rayTriangleNormal(1,normal),"invalid source triangle must retain the native normal");
      }

    const float inf = std::numeric_limits<float>::infinity();
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const std::vector<JPH::Vec3> invalid[] = {
      {{0,0,0},{0,0,0},{0,0,0}},
      {{0,0,0},{1,1,1},{2,2,2}},
      {{0,0,0},{1e-20f,0,0},{0,1e-20f,0}},
      {{0,0,0},{3e38f,0,0},{0,3e38f,0}},
      {{0,0,0},{inf,0,0},{0,1,0}},
      {{0,0,0},{nan,0,0},{0,1,0}},
      };
    for(const auto& vertices:invalid) {
      PhysicVbo mesh(&vertices);
      mesh.addIndex(indices,0,3,asset::MaterialGroup::STONE);
      JPH::Vec3 normal(7,8,9);
      require(!mesh.rayTriangleNormal(0,normal),"unusable source planes must retain native output");
      require(normal==JPH::Vec3(7,8,9),"invalid source normal must not overwrite native output");
      }
    }

  void testTerrainRayNormalScope() {
    const std::vector<JPH::Vec3> vertices = {
      {88.6986008f,5.17765617f,121.866676f},
      {91.4136581f,2.34256101f,112.210762f},
      {86.9217529f,5.54688644f,119.338539f},
      };
    const std::vector<uint32_t> indices = {0,2,1};
    PhysicVbo mesh(&vertices);
    mesh.addIndex(indices,0,3,asset::MaterialGroup::STONE);
    CollisionWorld world;
    auto landscape = world.addCollisionBody(mesh.shape(),at(0,0,0),.75f,
                                            DynamicWorld::C_Landscape,false,false,&mesh);
    auto object = world.addCollisionBody(mesh.shape(),at(0,0,0),.75f,
                                         DynamicWorld::C_Object,false,false,&mesh);
    const auto from = CollisionWorld::toCentimeters(JPH::Vec3(87.7866058f,7.30819273f,120.217987f));
    const auto to = CollisionWorld::toCentimeters(JPH::Vec3(87.7866058f,2.80819273f,120.217987f));
    const auto closest = world.rayCast(from,to,landscapeMask);
    JPH::Vec3 expected;
    require(mesh.rayTriangleNormal(0,expected),"terrain scope fixture must have a valid source normal");
    require(closest.hasHit && closest.body==landscape.get(),"identity terrain ray must hit its source face");
    const float normal[] = {closest.normal.x,closest.normal.y,closest.normal.z};
    for(JPH::uint axis=0;axis<3;++axis)
      require(std::bit_cast<uint32_t>(normal[axis])==std::bit_cast<uint32_t>(expected[axis]),
              "closest identity terrain ray must export the source plane normal");
    std::vector<CollisionWorld::QueryResult> all;
    world.rayCast(from,to,landscapeMask,false,&all);
    const auto objectHit = world.rayCast(from,to,objectMask);
    require(all.size()==1 && objectHit.hasHit && objectHit.body==object.get(),
            "native all-hit and object controls must hit the same geometry");
    const float native[] = {all[0].normal.x,all[0].normal.y,all[0].normal.z};
    const float objectNormal[] = {objectHit.normal.x,objectHit.normal.y,objectHit.normal.z};
    for(size_t axis=0;axis<3;++axis)
      require(std::bit_cast<uint32_t>(native[axis])==std::bit_cast<uint32_t>(objectNormal[axis]),
              "all-hit terrain and object queries must retain native normals");
    }

  void testMeshRayEdges() {
    CollisionWorld world;
    JPH::TriangleList triangles;
    triangles.emplace_back(JPH::Vec3(0,0,0),JPH::Vec3(0,0,1),JPH::Vec3(1,0,0));
    // This anchor admits the ray through the body's broad phase without hitting it.
    triangles.emplace_back(JPH::Vec3(-2,0,2),JPH::Vec3(-2,0,3),JPH::Vec3(-1,0,2));
    for(int i=1;i<=14;++i) {
      const float offset = float(i)*2.f;
      triangles.emplace_back(JPH::Vec3(offset,0,offset),JPH::Vec3(offset,0,offset+1),
                             JPH::Vec3(offset+1,0,offset));
      }
    JPH::MeshShapeSettings meshSettings(triangles);
    meshSettings.mMaxTrianglesPerLeaf = 1;
    const auto mesh = meshSettings.Create();
    require(!mesh.HasError(),"mesh ray edge test must construct its 16-leaf BVH");
    auto body = world.addCollisionBody(mesh.Get(),at(0,0,0),.75f,DynamicWorld::C_Landscape);

    // The target leaf has x >= 0; this point lies just outside that triangle with
    // one barycentric weight -5e-5. Both the edge test and BVH traversal need tolerance.
    const JPH::RayCast ray(JPH::Vec3(-5.0e-5f,1,.5f),JPH::Vec3(0,-2,0));
    const auto planePoint = ray.GetPointOnRay(.5f);
    JPH::Vec3 boundsMin, boundsMax;
    body->getAabb(boundsMin,boundsMax);
    require(JPH::AABox(boundsMin,boundsMax).Contains(planePoint),
            "anchor geometry must put the edge ray inside the overall body bounds");

    JPH::RayCastSettings exactSettings;
    JPH::ClosestHitCollisionCollector<JPH::CastRayCollector> exact;
    mesh.Get()->CastRay(ray,exactSettings,JPH::SubShapeIDCreator(),exact);
    require(!exact.HadHit(),"native default mesh rays must retain the exact triangle edge test");

    JPH::RayCastSettings compatibleSettings;
    compatibleSettings.mTriangleEdgeTolerance = 1.0e-4f;
    JPH::ClosestHitCollisionCollector<JPH::CastRayCollector> compatible;
    mesh.Get()->CastRay(ray,compatibleSettings,JPH::SubShapeIDCreator(),compatible);
    require(compatible.HadHit(),"mesh edge tolerance must reach triangles beyond their BVH leaf bounds");
    requireNear(compatible.mHit.mFraction,.5f,.000001f,"tolerant native mesh ray must hit the triangle plane");

    const auto gameHit = world.rayCast(CollisionWorld::toCentimeters(ray.mOrigin),
                                     CollisionWorld::toCentimeters(ray.GetPointOnRay(1)),landscapeMask);
    require(gameHit.hasHit && gameHit.body==body.get(),
            "game mesh rays must opt into the previous backend's edge tolerance");
    requireNear(gameHit.point.y,0,.001f,"compatible game mesh ray must report the triangle plane");
    require(!world.rayCast({-.02f,100,50},{-.02f,-100,50},landscapeMask).hasHit,
            "game mesh edge tolerance must reject points beyond its barycentric allowance");
    }

  void testCoincidentMaterials() {
    CollisionWorld world;
    // Two pairs of cyclic copies from the original city mesh. Their float plane
    // fractions prefer wood for the first ray and stone for the second; a fixed
    // material or source-order preference therefore cannot preserve both cases.
    const std::vector<JPH::Vec3> vertices = {
      CollisionWorld::toMeters({6181.77686f,1425.00293f,8387.23633f}),
      CollisionWorld::toMeters({6415.78125f,1242.17615f,8519.63574f}),
      CollisionWorld::toMeters({6371.84814f,1237.50293f,8494.77344f}),
      CollisionWorld::toMeters({5791.45264f,1425.0033f,5789.07715f}),
      CollisionWorld::toMeters({5923.85352f,1242.17651f,5555.07324f}),
      CollisionWorld::toMeters({5898.98828f,1237.5033f,5599.00586f}),
      };
    const std::vector<uint32_t> wood = {2,1,0,5,4,3};
    const std::vector<uint32_t> stone = {0,2,1,3,5,4};
    PhysicVbo mesh(&vertices);
    mesh.addIndex(wood,0,wood.size(),asset::MaterialGroup::WOOD,"wood-face");
    mesh.addIndex(stone,0,stone.size(),asset::MaterialGroup::STONE,"stone-face");
    auto body = world.addCollisionBody(mesh.shape(),at(0,0,0),.75f,
                                       DynamicWorld::C_Landscape,false,false,&mesh);
    const Tempest::Vec3 from[] = {{6477.52686f,1279.26453f,7595.64355f},
                                 {3314.40405f,1255.13806f,5864.34229f}};
    const Tempest::Vec3 to[] = {{5786.84668f,1274.92175f,11801.4824f},
                               {65382.3594f,2494.34717f,376.108551f}};
    const uint32_t expected[] = {0,3};
    for(size_t i=0;i<2;++i) {
      require(mesh.hasRayDuplicates(uint32_t(i)) && mesh.hasRayDuplicates(uint32_t(i+2)),
              "cyclic faces must retain their original triangle IDs and duplicate groups");
      for(const auto triangle:{uint32_t(i),uint32_t(i+2)})
        require(mesh.resolveRayTriangle(triangle,CollisionWorld::toMeters(from[i]),
                                        CollisionWorld::toMeters(to[i]))==expected[i],
                "coincident ray metadata must use original float plane fractions");
      const auto hit = world.rayCast(from[i],to[i],landscapeMask);
      require(hit.hasHit && hit.body==body.get(),"coincident material ray must hit its native mesh");
      require(hit.material==mesh.materialId(expected[i]),"game rays must resolve coincident face materials");
      require(hit.sector!=nullptr && std::string(hit.sector)==mesh.sectorName(expected[i]),
              "game rays must resolve coincident face sectors with their materials");
      }
    require(mesh.materialId(0)==asset::MaterialGroup::WOOD && mesh.materialId(2)==asset::MaterialGroup::STONE,
            "non-ray callers must retain each original triangle's material");
    }

  void testSharedRayEdge() {
    CollisionWorld world;
    // Original city faces from ray 97350 share an edge. Compressed vertices put
    // the wood face first, while the original planes put the stone face first.
    const std::vector<JPH::Vec3> vertices = {
      {652.533203f,44.7002907f,149.346542f},
      {653.475281f,44.2155151f,154.1633f},
      {652.22406f,46.061451f,149.986694f},
      {648.160217f,48.0254517f,146.484192f},
      };
    PhysicVbo mesh(&vertices);
    mesh.addIndex({0,2,1},0,3,asset::MaterialGroup::WOOD,"first-native-face");
    mesh.addIndex({2,0,3},0,3,asset::MaterialGroup::STONE,"first-original-face");
    auto body = world.addCollisionBody(mesh.shape(),at(0,0,0),.75f,
                                       DynamicWorld::C_Landscape,false,false,&mesh);
    const Tempest::Vec3 from(64818.9414f,4486.26904f,15276.9766f);
    const Tempest::Vec3 to(68676.2578f,5028.1626f,12438.8008f);
    std::vector<CollisionWorld::QueryResult> all;
    const auto first = world.rayCast(from,to,landscapeMask,false,&all);
    require(all.size()==2 && first.body==body.get(),"shared-edge ray must retain both native face hits");
    require(all[0].material==asset::MaterialGroup::WOOD && all[1].material==asset::MaterialGroup::STONE &&
            all[0].fraction<all[1].fraction && first.fraction==all[0].fraction,
            "all-hit queries must preserve native fraction ordering at shared edges");
    const auto closest = world.rayCast(from,to,landscapeMask);
    require(closest.hasHit && closest.body==body.get() && closest.material==asset::MaterialGroup::STONE,
            "closest shared-edge ray must choose the valid nearest original plane");
    require(std::string(closest.sector)=="first-original-face",
            "shared-edge correction must keep the chosen native face's metadata");
    requireNear(closest.fraction,.108205222f,.00000001f,
                "closest mesh rays must export the chosen original plane fraction");
    requireNear(closest.normal.x,all[1].normal.x,.000001f,"shared-edge correction must retain the chosen native normal x");
    requireNear(closest.normal.y,all[1].normal.y,.000001f,"shared-edge correction must retain the chosen native normal y");
    requireNear(closest.normal.z,all[1].normal.z,.000001f,"shared-edge correction must retain the chosen native normal z");
    requireNear(closest.point.x,65236.3203f,.005f,"closest mesh rays must export the source-plane point x");
    requireNear(closest.point.y,4544.90479f,.001f,"closest mesh rays must export the source-plane point y");
    requireNear(closest.point.z,14969.8701f,.005f,"closest mesh rays must export the source-plane point z");

    // A ray through the first face's interior must keep its ordinary closest hit.
    const auto center = (vertices[0]+vertices[1]+vertices[2])/3.f;
    const auto normal = (vertices[1]-vertices[0]).Cross(vertices[2]-vertices[0]).Normalized();
    const auto interior = world.rayCast(CollisionWorld::toCentimeters(center+normal),
                                       CollisionWorld::toCentimeters(center-normal),landscapeMask);
    require(interior.hasHit && interior.material==asset::MaterialGroup::WOOD,
            "interior mesh rays must retain their closest native face");
    }

  void testOriginalFloorProjection() {
    CollisionWorld world;
    constexpr float height = 37.1234818f;
    const std::vector<JPH::Vec3> vertices = {
      {0,height,0},{0,height,1},{1,height,0},
      // Distant triangles widen the codec's bounds without intersecting the ray.
      {10,0,10},{11,0,10},{10,0,11},
      {10,100,10},{10,100,11},{11,100,10},
      };
    const std::vector<uint32_t> indices = {0,2,1,3,5,4,6,8,7};
    PhysicVbo mesh(&vertices);
    mesh.addIndex(indices,0,indices.size(),asset::MaterialGroup::STONE,"original-floor");
    auto body = world.addCollisionBody(mesh.shape(),at(0,0,0),.75f,
                                       DynamicWorld::C_Landscape,false,false,&mesh);
    const Tempest::Vec3 from(25,5000,25), to(25,-5000,25);
    std::vector<CollisionWorld::QueryResult> all;
    const auto native = world.rayCast(from,to,landscapeMask,false,&all);
    require(all.size()==1 && native.body==body.get(),"floor projection must retain its selected native face");
    require(std::abs(native.point.y-height*100.f)>.001f,
            "floor fixture must expose the compressed native height through the all-hit API");
    const auto closest = world.rayCast(from,to,landscapeMask);
    require(closest.hasHit && closest.body==body.get() && closest.material==asset::MaterialGroup::STONE,
            "source-plane projection must preserve the selected native body and material");
    requireNear(closest.point.y,height*100.f,.0005f,
                "interior floor rays must recover their original geometric height");
    requireNear(closest.point.x,25,.00001f,"floor projection must keep the ray's x coordinate");
    requireNear(closest.point.z,25,.00001f,"floor projection must keep the ray's z coordinate");
    requireNear(closest.normal.y,native.normal.y,0,"floor projection must preserve its native normal");
    requireNear(closest.fraction,.128765181f,.00000001f,
                "floor projection must export the fraction consistent with its original height");
    }

  void testShallowOverlapProjection() {
    CollisionWorld world;
    const Tempest::Vec3 from(71271.125f,3618.42529f,-12934.1797f);
    const Tempest::Vec3 to(66624.5391f,3618.77539f,-17199.3848f);
    // The sphere initially overlaps this small box and moves toward it, while
    // its center ray misses. The bottom projection extrapolates by about -286.
    auto body = world.addCollisionBody(new JPH::BoxShape(JPH::Vec3(.01f,.01f,.01f)),
                                       at(from.x-70,from.y,from.z+60),.75f,DynamicWorld::C_Landscape);
    const auto hit = world.raySweep(from,to,100,landscapeMask);
    require(hit.hasHit && hit.body==body.get(),"shallow initial overlap must retain its sphere hit");
    requireNear(hit.fraction,1,.000001f,"failed center ray must reset only the exported fraction");
    requireNear(hit.point.x,1399596.875f,.01f,"shallow overlap must preserve endpoint interpolation in x");
    requireNear(hit.point.y,3518.45703125f,.005f,"shallow overlap must preserve the projected bottom in y");
    requireNear(hit.point.z,1206365.625f,.01f,"shallow overlap must preserve endpoint interpolation in z");
    }

  void testNpcContactDistance() {
    auto contact = [](float diagonal, bool outside, bool rotated) {
      CollisionWorld world;
      const std::vector<JPH::Vec3> vertices = outside ? std::vector<JPH::Vec3>{
        {.104f,-.3f,-.2f},{.104f,.3f,-.2f},{.104f,0,.2f},
        {100,100,100},{101,100,100},{100,101,100},
        } : std::vector<JPH::Vec3>{
        {diagonal,-.3f,diagonal},{diagonal,.3f,diagonal},{diagonal+.2f,0,diagonal-.2f},
        {100,100,100},{101,100,100},{100,101,100},
        };
      PhysicVbo mesh(&vertices);
      const std::vector<uint32_t> indices = {0,1,2,3,4,5};
      mesh.addIndex(indices,0,indices.size(),asset::MaterialGroup::STONE,"contact-test");
      auto matrix = at(0,0,0);
      if(rotated)
        matrix.rotateOY(45);
      auto body = world.addCollisionBody(mesh.shape(),matrix,.75f,DynamicWorld::C_Landscape,false,false,&mesh);
      JPH::CapsuleShape capsule(.1f,.1f);
      Tempest::Vec3 normal;
      Interactive* vob = nullptr;
      const bool result = world.hasCollision(&capsule,{0,0,0},normal,vob);
      if(result)
        require(normal.x<-.5f && normal.z<-.5f,"separated diagonal contact must face away from its triangle");
      return result;
      };
    // The previous contact callback reported this 40 mm gap even though the
    // relative contact threshold is only 4.9 mm. Its final GJK distance bound
    // included the capsule radius. Movement consumes these separated contacts.
    require(contact(.099f,false,false),"NPC query must preserve separated contacts inside the original triangle AABB");
    // Expanding the distance query also reaches this 4 mm gap, but the original
    // triangle candidate box excluded it. The distant anchor admits broad phase.
    require(!contact(0,true,false),"NPC proximity must retain the unexpanded triangle candidate box");
    require(!contact(0,true,true),"rotated NPC mesh candidates must use the original mesh-local box");

    CollisionWorld world;
    const std::vector<JPH::Vec3> vertices = {
      {.0207614303f,.400143862f,-.305011988f},
      {.250788718f,.276874304f,.318776399f},
      {.225168914f,-.396727622f,-.0763103366f},
      {100,100,100},{101,100,100},{100,101,100},
      };
    PhysicVbo mesh(&vertices);
    const std::vector<uint32_t> indices = {0,1,2,3,4,5};
    mesh.addIndex(indices,0,indices.size(),asset::MaterialGroup::STONE,"bounded-contact-test");
    auto body = world.addCollisionBody(mesh.shape(),at(0,0,0),.75f,DynamicWorld::C_Landscape,false,false,&mesh);
    JPH::CapsuleShape capsule(.3f,.08f);
    Tempest::Vec3 normal;
    Interactive* vob = nullptr;
    // Its true surface gap is 50.8 mm, inside the 87.93 mm distance envelope,
    // and its triangle AABB overlaps. The old bounded GJK stopped with a 98.7 mm
    // simplex approximation and excluded it; accepting the exact gap blocks early.
    require(!world.hasCollision(&capsule,{0,0,0},normal,vob),
            "NPC contact eligibility must preserve the bounded query's previous simplex");
    }

  void testNpcScalarWitness() {
    CollisionWorld world;
    // This source face stopped the player one frame early in ik-sweep. The
    // scalar witness keeps the tiny vertical direction that selects the next
    // capsule support endpoint; the selected source geometry stays intact.
    const std::vector<JPH::Vec3> vertices = {
      {368.17807f,44.3249741f,-322.601501f},
      {366.899994f,45.7681885f,-329.008514f},
      {368.358978f,43.3533669f,-322.625275f},
      };
    PhysicVbo mesh(&vertices);
    mesh.addIndex({0,2,1},0,3,asset::MaterialGroup::STONE,"npc-witness-face");
    auto body = world.addCollisionBody(mesh.shape(),at(0,0,0),.75f,
                                       DynamicWorld::C_Landscape,false,false,&mesh);
    JPH::CapsuleShape capsule(.231234491f,.0817193016f);
    Tempest::Vec3 normal;
    Interactive* vob = nullptr;
    require(!world.hasCollision(&capsule,{36718.7188f,4488.04932f,-32690.3535f},normal,vob),
            "NPC sweep frame 19 must retain the unblocked source-face pose");
    require(world.hasCollision(&capsule,{36720.9766f,4488.3374f,-32688.2539f},normal,vob),
            "NPC sweep frame 20 must block at the next source-face pose");
    // Frozen original scalar callback direction from this accepted pose.
    requireNear(normal.x,-.976384342f,.000002f,"NPC separated contact must preserve the original conservative X direction");
    requireNear(normal.y,-.126087219f,.000002f,"NPC separated contact must preserve the original conservative Y direction");
    requireNear(normal.z,.175430045f,.000002f,"NPC separated contact must preserve the original conservative Z direction");

    // These neighboring poses exercise the tetrahedron's BDC/ADB face weights.
    // Changing interpolation order along with face evaluation regresses the second.
    require(!world.hasCollision(&capsule,{36717.918f,4487.64941f,-32690.9551f},normal,vob),
            "tetrahedron face weights must reject the original boundary pose");
    require(world.hasCollision(&capsule,{36717.7148f,4488.04932f,-32691.7539f},normal,vob),
            "tetrahedron face weights must preserve the neighboring accepted pose");
    }

  void testNpcSeparatedContactResponse() {
    CollisionWorld world;
    // Frozen tick 80027 of ik-sweep: the same rejected downward movement touched
    // an edge and an interior face. A tiny normal change redirected the fall,
    // then changed the next slope heading and the later collision frame.
    const std::vector<JPH::Vec3> vertices = {
      {86.245079f,5.06495762f,118.298805f},
      {80.3646851f,5.83244085f,123.314186f},
      {80.3646851f,7.01338053f,123.314186f},
      {80.3646851f,7.01338053f,123.314186f},
      {86.9217529f,5.54688644f,119.338539f},
      {86.245079f,5.06495762f,118.298805f},
      };
    PhysicVbo mesh(&vertices);
    mesh.addIndex({0,2,1,3,5,4},0,6,asset::MaterialGroup::STONE,"npc-fall-contact");
    auto body = world.addCollisionBody(mesh.shape(),at(0,0,0),.75f,
                                       DynamicWorld::C_Landscape,false,false,&mesh);
    JPH::CapsuleShape capsule(.231234491f,.0817193016f);
    Tempest::Vec3 normal;
    Interactive* vob = nullptr;
    require(world.hasCollision(&capsule,{8081.23877f,719.952087f,12290.1709f},normal,vob),
            "NPC fall contact must block the frozen downward sample");
    requireNear(normal.x,.00157767383f,.0000002f,"NPC fall contact must preserve original edge/face normal X");
    requireNear(normal.y,.932768404f,.0000002f,"NPC fall contact must preserve original edge/face normal Y");
    requireNear(normal.z,-.360472828f,.0000002f,"NPC fall contact must preserve original edge/face normal Z");

    // Recorded gravity-controller response to this public collision normal.
    // Test the branch's slope redirection without requiring NPC/world assets.
    const float horizontal = std::sqrt(normal.x*normal.x+normal.z*normal.z);
    const float complement = std::sqrt(1.f-horizontal*horizontal);
    const auto redirected = Tempest::Vec3(complement*normal.x/horizontal,
                                          horizontal*normal.y/complement,
                                          complement*normal.z/horizontal)*.35966f;
    requireNear(redirected.x,.00146827195f,.0000002f,"NPC fall response must preserve original horizontal velocity");
    requireNear(redirected.y,.129648894f,.0000002f,"NPC fall response must preserve original vertical velocity");
    requireNear(redirected.z,-.335476279f,.0000002f,"NPC fall response must preserve original forward velocity");
    }

  void testFixedStepCarry() {
    CollisionWorld world;
    auto first = world.addDynamicBody(new JPH::BoxShape(JPH::Vec3(.1f,.1f,.1f)),at(0,700,0),.4f,1);
    world.tick(8);
    requireNear(first->position().y,700,.001f,"eight milliseconds must remain below the fixed step");
    first.reset();
    // An empty world ignores elapsed time while retaining the existing remainder.
    world.tick(1000);
    auto next = world.addDynamicBody(new JPH::BoxShape(JPH::Vec3(.1f,.1f,.1f)),at(0,700,0),.4f,1);
    world.tick(9);
    const float step = 1.f/60.f;
    requireNear(next->position().y,700.f-DynamicWorld::gravityMS*100.f*step*step,.005f,
                "fixed-step remainder must survive removal and addition of dynamic bodies");
    }

  void testFixedStepCadence() {
    CollisionWorld world;
    auto body = world.addDynamicBody(new JPH::BoxShape(JPH::Vec3(.1f,.1f,.1f)),at(0,700,0),.4f,1);
    const float step = 1.f/60.f;
    const float acceleration = DynamicWorld::gravityMS*100.f*step*step;
    world.tick(16);
    requireNear(body->position().y,700,.005f,"sixteen milliseconds must not advance a new body");
    world.tick(17);
    requireNear(body->position().y,700.f-acceleration,.005f,"first seventeen millisecond frame must advance one step");
    world.tick(17);
    requireNear(body->position().y,700.f-3.f*acceleration,.005f,"second seventeen millisecond frame must preserve the previous float cadence");
    world.tick(16);
    requireNear(body->position().y,700.f-6.f*acceleration,.005f,"next sixteen millisecond frame must advance the retained step");
    }

  void testCharacterVirtualSkippedMove() {
    Resources resources;
    ProtoMesh wall(PackedMesh(movableCollisionBox({20,400,600}),PackedMesh::PK_Visual),"VIRTUAL_SKIP_WALL.MRM");
    zenkit::ModelHierarchy hierarchy{};
    hierarchy.collision_bbox = {{-25,0,-25},{25,180,25}};
    Skeleton shape(asset::fromZenKit(hierarchy),nullptr,"VIRTUAL_SKIP_CONTRACT");
    for(bool obstructed:{false,true}) {
      DynamicWorld world(nullptr,movableFloor());
      auto structure=obstructed ? world.addStaticObj(wall.attach[0].shape.get(),at(50,200,0),"VIRTUAL_SKIP_WALL.MRM") :
                                 DynamicWorld::Item();
      auto walker=world.addGhostObj(&shape);
      const Tempest::Vec3 initial(14,0,0);
      for(const Tempest::Vec3 request:{Tempest::Vec3(.11f,0,0),Tempest::Vec3(1.9f,0,1.9f)}) {
        walker.setPosition(initial);
        DynamicWorld::CollisionTest collision;
        require(walker.tryMove(initial+request,collision)==DynamicWorld::MC_Skip,
                "legacy skip must remain component-wise even when the diagonal length exceeds two centimetres");
        const auto moved=walker.moveVirtual(request,17,50,.785398163f,80);
        require(!moved.blocked,"a legacy-skipped animation sway must not report virtual movement failure, with or without a wall");
        }
      walker.setPosition(initial);
      DynamicWorld::CollisionTest collision;
      require(walker.tryMove(initial+Tempest::Vec3(2,0,0),collision)!=DynamicWorld::MC_Skip,
              "a component exactly two centimetres must retain the legacy non-skip boundary");
      walker.setPosition(initial);
      const auto step=walker.moveVirtual({5,0,0},17,50,.785398163f,80);
      require(step.blocked==obstructed,"a real walking step must report a blocking wall while remaining free on an unobstructed floor");
      if(obstructed)
        require(!structure.isEmpty() && step.collision.landCol,"a blocked virtual walking step must retain its world collision notification");
      else
        require(step.position.x>initial.x+4.f,"suppressing animation-sway failures must preserve native walking displacement");
      }
    }

  void testCharacterVirtualInteractiveStep() {
    constexpr float slope=.785398163f;
    for(bool movable:{false,true}) for(bool interactive:{false,true}) {
      CollisionWorld world;
      auto floor=world.addCollisionBody(new JPH::BoxShape(JPH::Vec3(20,.5f,20)),at(0,-50,0),.7f,
                                        DynamicWorld::C_Landscape);
      // A chair seat and an architectural tread can have the same geometry.
      // The interactive classification determines whether walking may step up.
      auto seat=world.addCollisionBody(new JPH::BoxShape(JPH::Vec3(.5f,.2f,1)),at(100,20,0),.7f,
                                       DynamicWorld::C_Object,movable);
      if(interactive)
        seat->setPayload(nullptr,DynamicWorld::P_Interactive);
      auto walker=world.createVirtualCharacter(25,180,{0,0,0});
      float highest=0.f;
      CollisionWorld::CharacterMoveResult state;
      for(int i=0;i<60;++i) {
        state=walker->move({5,0,0},17,50,slope,80,nullptr);
        highest=std::max(highest,state.position.y);
        }
      std::printf("CharacterVirtual 40 cm edge: movable %d, interactive %d, max feet %.4f cm, final x %.3f cm\n",
                  movable,interactive,double(highest),double(state.position.x));
      if(interactive) {
        require(highest<1.f && state.position.x<30.f,
                "automatic world stairs must not step onto or pass through interactive furniture");
        walker->setPosition({100,40,0});
        state=walker->move({},17,50,slope,80,nullptr);
        require(state.grounded,"interactive furniture must retain support for feet explicitly placed on its top");
        requireNear(state.position.y,40.f,1.f,"disabling automatic furniture stepping must not drop existing top support");
        } else {
        require(highest>39.f && state.position.x>200.f,
                "ordinary architectural C_Object treads must retain automatic world stair walking");
        }
      }
    }

  void testCharacterVirtualMovingUtensil() {
    float maximumJump=0.f;
    for(bool kicked:{false,true}) {
      CollisionWorld world;
      auto floor=world.addCollisionBody(new JPH::BoxShape(JPH::Vec3(20,.5f,20)),at(0,-50,0),.7f,
                                        DynamicWorld::C_Landscape);
      auto matrix=at(150,25,0);
      if(!kicked)
        matrix.rotateOZ(45.f);
      // Real dropped items have a centred proxy padded to 15 cm per half-axis.
      // Its mass is still calculated from the smaller visual, like ITFO_BEER.
      auto mug=world.addDynamicBody(new JPH::BoxShape(JPH::Vec3(.15f,.15f,.15f)),matrix,.5f,5.80991459f,
                                     DynamicWorld::C_Item,false);
      for(int i=0;i<90;++i)
        world.tick(17);
      auto walker=world.createVirtualCharacter(25,180,{0,0,0});
      float highest=0.f,jump=0.f,last=0.f;
      CollisionWorld::CharacterMoveResult state;
      for(int frame=0;frame<65;++frame) {
        if(kicked && frame==8) {
          const auto point=mug->position()+Tempest::Vec3(0,10,10);
          require(mug->addImpulseAtPoint({-5,0,0},point),
                  "moving utensil fixture must apply its off-centre native impulse");
          }
        state=walker->move({11,0,0},33,50,.785398163f,80,nullptr);
        highest=std::max(highest,state.position.y);
        jump=std::max(jump,state.position.y-last);
        last=state.position.y;
        world.tick(33);
        }
      maximumJump=std::max(maximumJump,std::max(highest,jump));
      std::printf("CharacterVirtual moving 5.81 kg utensil: kicked %d, max feet %.4f cm, max jump %.4f cm, final x %.3f cm\n",
                  kicked,double(highest),double(jump),double(state.position.x));
      require(mug->touchedByNpc() && mug->position().x>200.f,
              "a rotating loose utensil must retain its native character contact and walking push");
      require(state.position.x>600.f,"a moving loose utensil must not prevent ordinary walking progress");
      }
    require(maximumJump<1.f,
            "a moving or rotating light item's side velocity must not lift feet through the floor contact solver");
    }

  void testCharacterVirtualIdleSlopes() {
    constexpr float maxSlope=.785398163f;
    const auto slopeNormal=[](float degrees) {
      const float angle=degrees*.01745329252f;
      const auto heading=JPH::Vec3(.120f,0,-.071f).Normalized();
      return heading*std::sin(angle)+JPH::Vec3::sAxisY()*std::cos(angle);
      };
    // The first normal is the reported farm-path support; the others approach
    // the walkable limit and cross it. Exercise both human capsule sizes.
    const std::array<JPH::Vec3,4> normals={JPH::Vec3(.120f,.990f,-.071f).Normalized(),
                                          slopeNormal(25),slopeNormal(44),slopeNormal(60)};
    for(float radius:{25.f,20.f}) for(const auto normal:normals) {
      CollisionWorld world;
      const auto vertex=[&](float x,float z) {return JPH::Vec3(x,-(normal.GetX()*x+normal.GetZ()*z)/normal.GetY(),z);};
      JPH::TriangleList triangles;
      triangles.emplace_back(vertex(-20,-20),vertex(-20,20),vertex(20,20));
      triangles.emplace_back(vertex(-20,-20),vertex(20,20),vertex(20,-20));
      const auto mesh=JPH::MeshShapeSettings(triangles).Create();
      require(!mesh.HasError(),"idle-slope fixture must construct its landscape plane");
      auto floor=world.addCollisionBody(mesh.Get(),at(0,0,0),.7f,DynamicWorld::C_Landscape);
      auto walker=world.createVirtualCharacter(radius,180,{0,radius*(1.f/normal.GetY()-1.f),0});
      auto reference=world.createVirtualCharacter(radius,180,{0,radius*(1.f/normal.GetY()-1.f),0});
      const Tempest::Vec3 initialFeet(0,radius*(1.f/normal.GetY()-1.f),0);
      for(bool teleport:{false,true}) {
        if(teleport) {
          walker->setPosition(initialFeet);
          reference->setPosition(initialFeet);
          walker->move({},0,50,maxSlope,80,nullptr);
          }
        const auto idleContact=walker->move({},33,50,maxSlope,80,nullptr);
        // An infinitesimal vertical request uses the native gravity solve.
        // Acquiring an idle contact must be continuous with that solve,
        // including after a teleport followed by a zero-dt contact refresh.
        const auto nativeContact=reference->move({0,.0001f,0},33,50,maxSlope,80,nullptr);
        requireNear((idleContact.position-nativeContact.position).length(),0,.001f,
                    "initial idle contact recovery must preserve the native contact pose used for stairs");
        }
      CollisionWorld::CharacterMoveResult state;
      for(int frame=0;frame<30;++frame)
        state=walker->move({},17,50,maxSlope,80,nullptr);
      const auto initial=state.position;
      const bool walkable=normal.GetY()>std::cos(maxSlope);
      require(walkable ? state.grounded : state.steep,"idle-slope fixture must identify walkable and sliding support");
      for(int frame=0;frame<180;++frame)
        state=walker->move({},frame%3==0 ? 16 : 17,50,maxSlope,80,nullptr);
      const float drift=(state.position-initial).length();
      std::printf("CharacterVirtual idle slope: radius %.0f cm, incline %.3f degrees, 3 s drift %.5f cm, grounded %d, steep %d\n",
                  double(radius),double(std::acos(normal.GetY())*57.2957795f),double(drift),state.grounded,state.steep);
      if(walkable) {
        require(state.grounded && drift<.5f,"idle human capsules must drift less than half a centimetre in three seconds below the slide threshold");
        const auto jumped=walker->move({0,20,0},17,50,maxSlope,80,nullptr);
        require(!jumped.grounded && jumped.position.y>state.position.y+10.f,
                "static slope friction must preserve an explicit jump away from support");
        } else {
        require(state.steep && state.position.y<initial.y-1.f,
                "idle human capsules must retain downhill sliding above the slide threshold");
        }
      }
    }

  void testCharacterVirtualSupports() {
    constexpr float slope=.785398163f;
    // Sample every rider immediately after move(), against the support pose
    // that move() actually observed. Comparing a pre-physics rider with a
    // post-physics body hides a one-frame error inside the drift tolerance.
    const auto frameDt=[](int frame) -> uint64_t { return frame%3==2 ? 16 : 17; };
    const auto carriedPoint=[](const Tempest::Matrix4x4& matrix, Tempest::Vec3 local) {
      matrix.project(local);
      return local;
      };
    for(bool gamePhase:{false,true}) for(bool rotating:{false,true}) {
      CollisionWorld world;
      auto floor=world.addCollisionBody(new JPH::BoxShape(JPH::Vec3(20,.5f,20)),at(0,-50,0),.7f,
                                        DynamicWorld::C_Landscape);
      auto platform=world.addCollisionBody(new JPH::BoxShape(JPH::Vec3(2,.1f,2)),at(0,90,0),.7f,
                                           DynamicWorld::C_Object,true);
      // Hero and a second human-sized controller exercise distinct points of
      // the same support, including a nonzero radius for the turning platform.
      std::array<std::unique_ptr<CollisionWorld::VirtualCharacter>,2> riders={
        world.createVirtualCharacter(25,180,{100,100,-60}),
        world.createVirtualCharacter(25,180,{-80,100,70})};
      std::array<CollisionWorld::CharacterMoveResult,2> state;
      std::array<Tempest::Vec3,2> initial,local;
      std::array<float,2> maximumDrift={};
      for(size_t rider=0;rider<riders.size();++rider) {
        state[rider]=riders[rider]->move({},17,50,slope,80,nullptr);
        require(state[rider].grounded,"both virtual riders must acquire the initial kinematic support");
        initial[rider]=state[rider].position;
        local[rider]=initial[rider]-platform->position();
        }
      uint64_t elapsed=0;
      const int frames=rotating ? 180 : 240;
      bool supported=true;
      for(int frame=0;frame<frames;++frame) {
        const auto dt=frameDt(frame);
        if(gamePhase)
          for(size_t rider=0;rider<riders.size();++rider)
            state[rider]=riders[rider]->move({},dt,50,slope,80,nullptr);
        elapsed+=dt;
        auto matrix=rotating ? at(0,90,0) : at(.08f*float(elapsed),90.f+.0045f*float(elapsed),0);
        if(rotating)
          matrix.rotateOY(.04f*float(elapsed)); // 120 degrees in three seconds.
        platform->setObjMatrix(matrix);
        world.tick(dt);
        for(size_t rider=0;rider<riders.size();++rider) {
          if(gamePhase) {
            const auto carried=riders[rider]->carry(dt);
            require(carried.supportMoved,"a mover updated after NPC movement must exercise the late carry sweep");
            state[rider]=carried;
            } else {
            state[rider]=riders[rider]->move({},dt,50,slope,80,nullptr);
            }
          supported&=state[rider].grounded;
          maximumDrift[rider]=std::max(maximumDrift[rider],
                                      (state[rider].position-carriedPoint(matrix,local[rider])).length());
          }
        }
      const auto matrix=platform->matrix();
      for(size_t rider=0;rider<riders.size();++rider) {
        const auto expected=carriedPoint(matrix,local[rider]);
        const auto drift=state[rider].position-expected;
        const auto displacement=state[rider].position-initial[rider];
        std::printf("CharacterVirtual %s %s support rider %zu: support travel %.6f cm, angle %.3f deg, "
                    "rider displacement (%.6f, %.6f, %.6f) cm, drift (%.6f, %.6f, %.6f) cm, "
                    "final %.6f / maximum %.6f cm, ground speed %.6f cm/s\n",
                    gamePhase ? "NPC->mover->physics->carry" : "physics->NPC",
                    rotating ? "turning kinematic" : "linear kinematic",rider,
                    double((platform->position()-Tempest::Vec3(0,90,0)).length()),
                    rotating ? .04*double(elapsed) : 0.,
                    double(displacement.x),double(displacement.y),double(displacement.z),
                    double(drift.x),double(drift.y),double(drift.z),double(drift.length()),
                    double(maximumDrift[rider]),double(state[rider].groundVelocity.length()));
        }
      require(supported,"both virtual riders must retain kinematic support throughout movement");
      if(rotating)
        require(.04f*float(elapsed)>=90.f,"the turning support contract must rotate at least ninety degrees");
      else
        require(platform->position().x>=300.f,"the linear support contract must travel at least three metres");
      for(const float drift:maximumDrift)
        requireNear(drift,0,rotating ? 2.f : 1.f,
                    rotating ? "a turning support must retain each rider's local point within two centimetres" :
                               "a three-metre kinematic translation must carry each rider within one centimetre");
      require(state[0].groundVelocity.length()>50.f,
              "kinematic support must expose its actual support-point velocity for a later jump");
      // No residual inferred velocity is allowed after the transform stops.
      for(size_t rider=0;rider<riders.size();++rider) {
        const auto stopped=riders[rider]->move({},17,50,slope,80,nullptr);
        requireNear((stopped.position-state[rider].position).length(),0,.1f,
                    "stopping a direct-matrix support must not retain its previous carried displacement");
        if(gamePhase) {
          world.tick(17);
          const auto carried=riders[rider]->carry(17);
          require(!carried.supportMoved,"a stopped mover must not schedule another carry sweep");
          requireNear((carried.position-stopped.position).length(),0,.001f,
                      "the late carry phase must not move feet after their support has stopped");
          }
        }
      platform.reset();
      if(gamePhase)
        require(!riders[0]->carry(17).supportMoved,"removing a support must invalidate its late carry sweep");
      const auto falling=riders[0]->move({},17,50,slope,80,nullptr);
      require(!falling.grounded && falling.position.y<state[0].position.y,
              "virtual feet must lose support and fall after the moving platform disappears");
      }
    for(bool gamePhase:{false,true}) for(bool variableCadence:{false,true}) {
      if(!gamePhase && variableCadence)
        continue; // Preserve the original nominal physics-before-NPC contract.
      CollisionWorld world;
      auto floor=world.addCollisionBody(new JPH::BoxShape(JPH::Vec3(20,.5f,20)),at(0,-50,0),.01f,
                                        DynamicWorld::C_Landscape);
      // A heavy, broad platform has an accepted horizontal top under the step-1
      // support policy. Low floor friction makes the impulse travel observable;
      // a nearly stationary body cannot establish a carry-distance contract.
      auto platform=world.addDynamicBody(new JPH::BoxShape(JPH::Vec3(2,.1f,2)),at(0,10,0),.01f,122.f,
                                         DynamicWorld::C_MovableVob,true);
      auto rider=world.createVirtualCharacter(25,180,{0,20,0});
      auto state=rider->move({},17,50,slope,80,nullptr);
      require(state.grounded,"virtual feet must acquire the heavy dynamic support before its impulse");
      require(platform->mass()>=10.f,"the dynamic carry contract must use an accepted support mass");
      const auto bodyStart=platform->position(),riderStart=state.position;
      const auto local=riderStart-bodyStart;
      require(platform->addImpulse({1.8f*platform->mass(),0,0}),
              "the dynamic support must receive one native linear impulse");
      float maximumDrift=0.f,maximumTravel=0.f;
      Tempest::Vec3 endpointPrediction;
      float maximumPredictionError=0.f,spikeObserved=0.f,spikePredicted=0.f;
      bool supported=true;
      for(int frame=0;frame<120;++frame) {
        constexpr std::array<uint64_t,4> cadence={25,45,16,65};
        const auto dt=variableCadence ? cadence[size_t(frame)%cadence.size()] : frameDt(frame);
        if(gamePhase)
          state=rider->move({},dt,50,slope,80,nullptr);
        // Physics advances before this character sample, just as the preceding
        // World::tick has already advanced bodies before the next NPC tick.
        const auto previous=platform->position();
        world.tick(dt);
        const auto velocityBeforeCarry=platform->velocity();
        const auto angularBeforeCarry=platform->angularVelocity();
        endpointPrediction+=velocityBeforeCarry*(float(dt)*.001f);
        maximumPredictionError=std::max(maximumPredictionError,
                                        (endpointPrediction-(platform->position()-bodyStart)).length());
        if(dt==65) {
          spikeObserved=(platform->position()-previous).length();
          spikePredicted=velocityBeforeCarry.length()*.065f;
          }
        if(gamePhase) {
          const auto carried=rider->carry(dt);
          if(carried.supportMoved)
            state=carried;
          requireNear((platform->velocity()-velocityBeforeCarry).length(),0,.0001f,
                      "a late carry sweep must not apply a second weight or contact impulse to its support");
          requireNear((platform->angularVelocity()-angularBeforeCarry).length(),0,.000001f,
                      "a late carry sweep must not apply torque to its support");
          } else {
          state=rider->move({},dt,50,slope,80,nullptr);
          }
        supported&=state.grounded;
        maximumTravel=std::max(maximumTravel,(platform->position()-bodyStart).length());
        maximumDrift=std::max(maximumDrift,(state.position-carriedPoint(platform->matrix(),local)).length());
        }
      const auto drift=state.position-carriedPoint(platform->matrix(),local);
      std::printf("CharacterVirtual %s dynamic support %s: mass %.6f kg, impulse %.6f Ns, "
                  "support travel %.6f cm, rider travel %.6f cm, drift (%.6f, %.6f, %.6f) cm, "
                  "final %.6f / maximum %.6f cm; endpoint-v*dt prediction error %.6f cm, "
                  "last 65 ms observed %.6f vs predicted %.6f cm\n",
                  gamePhase ? "NPC->physics->carry" : "physics->NPC",variableCadence ? "25/45/16/65 ms" : "17/17/16 ms",
                  double(platform->mass()),double(1.8f*platform->mass()),
                  double(maximumTravel),double((state.position-riderStart).length()),
                  double(drift.x),double(drift.y),double(drift.z),double(drift.length()),double(maximumDrift),
                  double(maximumPredictionError),double(spikeObserved),double(spikePredicted));
      require(maximumTravel>=50.f,"the impulse-driven dynamic support must travel at least fifty centimetres");
      require(supported,"the rider must remain grounded throughout the dynamic carry contract");
      requireNear(maximumDrift,0,2.f,"a heavy impulse-driven support must carry virtual feet within two centimetres");
      if(variableCadence)
        require(maximumPredictionError>2.f,
                "the variable cadence must expose endpoint velocity integration differing from native fixed-step displacement");
      }
    {
      CollisionWorld world;
      auto floor=world.addCollisionBody(new JPH::BoxShape(JPH::Vec3(20,.5f,20)),at(0,-50,0),.7f,
                                        DynamicWorld::C_Landscape);
      auto platform=world.addDynamicBody(new JPH::BoxShape(JPH::Vec3(2,.1f,2)),at(0,90,0),.7f,122.f,
                                          DynamicWorld::C_MovableVob);
      auto rider=world.createVirtualCharacter(25,180,{0,100,0});
      auto state=rider->move({},17,50,slope,80,nullptr);
      require(state.grounded,"the reversing support fixture must acquire its dynamic deck");
      const auto bodyStart=platform->position();
      auto local=state.position;
      auto inverse=platform->matrix();inverse.inverse();inverse.project(local);
      require(platform->addImpulse({0,platform->mass()*(1.8f-platform->velocity().y*.01f),0}),
              "the dynamic deck must accept an upward impulse before reversing under gravity");
      float maximumDrift=0.f, maximumRise=0.f;
      bool descended=false;
      for(int frame=0;frame<180;++frame) {
        const uint64_t dt=frame%3==2 ? 34 : 33;
        state=rider->move({},dt,50,slope,80,nullptr);
        const auto previous=platform->position();
        world.tick(dt);
        const auto carried=rider->carry(dt);
        if(carried.supportMoved)
          state=carried;
        require(state.grounded,"an idle rider must retain support while its deck reverses vertical direction");
        maximumRise=std::max(maximumRise,platform->position().y-bodyStart.y);
        descended|=platform->position().y<previous.y-.01f;
        maximumDrift=std::max(maximumDrift,(state.position-carriedPoint(platform->matrix(),local)).length());
        }
      std::printf("CharacterVirtual reversing dynamic deck: rise %.6f cm, descended %d, maximum carry error %.6f cm\n",
                  double(maximumRise),descended,double(maximumDrift));
      require(maximumRise>1.f && descended,"the dynamic carry fixture must actually rise and reverse");
      require(maximumDrift<=.1f,
              "an idle rider must not add predicted support motion before being carried by the observed transform");
      }
    {
      CollisionWorld world;
      auto platform=world.addCollisionBody(new JPH::BoxShape(JPH::Vec3(4,.1f,2)),at(0,90,0),.7f,
                                           DynamicWorld::C_Object,true);
      auto wall=world.addCollisionBody(new JPH::BoxShape(JPH::Vec3(.1f,2,2)),at(100,200,0),.7f,
                                       DynamicWorld::C_Object);
      auto rider=world.createVirtualCharacter(25,180,{0,100,0});
      auto state=rider->move({},17,50,slope,80,nullptr);
      require(state.grounded,"the blocked carry fixture must begin on its kinematic support");
      for(int frame=1;frame<=160;++frame) {
        state=rider->move({},17,50,slope,80,nullptr);
        platform->setObjMatrix(at(2.f*float(frame),90,0));
        world.tick(17);
        const auto carried=rider->carry(17);
        if(carried.supportMoved)
          state=carried;
        require(state.position.x<=65.1f,"a late carry sweep must stop at a world wall instead of pinning feet to the support");
        }
      std::printf("CharacterVirtual late carry blocked by wall: support %.6f cm, rider %.6f cm, slip %.6f cm\n",
                  double(platform->position().x),double(state.position.x),double(platform->position().x-state.position.x));
      require(state.position.x>50.f && platform->position().x-state.position.x>250.f,
              "the wall fixture must carry the rider to the obstruction while the support continues underneath");
      }
    }

  void testCharacterVirtualSlopeRest() {
    constexpr float radians=.01745329252f, maxSlope=50.f*radians;
    constexpr float radius=25.f, padding=1.f;
    for(unsigned planeKind:{0u,1u,2u}) for(float angle:{14.f,25.f,40.f,49.f,55.f}) {
      const float sine=std::sin(angle*radians), cosine=std::cos(angle*radians);
      CollisionWorld world;
      std::unique_ptr<CollisionWorld::CollisionBody> support;
      if(planeKind>0) {
        const JPH::Vec3 along(cosine,sine,0), across(0,0,1);
        const auto a=-20.f*along-20.f*across, b=-20.f*along+20.f*across;
        const auto c=20.f*along+20.f*across, d=20.f*along-20.f*across;
        JPH::TriangleList triangles;
        triangles.emplace_back(a,b,c);
        triangles.emplace_back(a,c,d);
        const auto shape=JPH::MeshShapeSettings(triangles).Create();
        require(!shape.HasError(),"slope rest fixture must construct a planar mesh");
        JPH::ShapeRefC floorShape=shape.Get();
        if(planeKind==2) {
          // The real landscape is chunked into compound mesh children. Keep
          // both hierarchy bits and the triangle ID in the support subshape.
          JPH::StaticCompoundShapeSettings inner;
          inner.AddShape(JPH::Vec3::sZero(),JPH::Quat::sIdentity(),floorShape.GetPtr());
          inner.AddShape(JPH::Vec3(100,0,0),JPH::Quat::sIdentity(),new JPH::BoxShape(JPH::Vec3(1,1,1)));
          const auto child=inner.Create();
          require(!child.HasError(),"slope fixture must construct a mesh chunk");
          JPH::StaticCompoundShapeSettings outer;
          outer.AddShape(JPH::Vec3::sZero(),JPH::Quat::sIdentity(),child.Get());
          outer.AddShape(JPH::Vec3(200,0,0),JPH::Quat::sIdentity(),new JPH::BoxShape(JPH::Vec3(1,1,1)));
          const auto compound=outer.Create();
          require(!compound.HasError(),"slope fixture must construct a chunked landscape");
          floorShape=compound.Get();
          }
        support=world.addCollisionBody(floorShape,at(0,0,0),.7f,DynamicWorld::C_Landscape);
        } else {
        // Put the rotated box's upper face through the origin.
        auto matrix=at(50.f*sine,-50.f*cosine,0);
        matrix.rotateOZ(angle);
        support=world.addCollisionBody(new JPH::BoxShape(JPH::Vec3(20,.5f,20)),matrix,.7f,
                                       DynamicWorld::C_Landscape);
        }
      // The lower capsule sphere, including native character padding, touches
      // the plane at t=0. No settling frames may hide first-second movement.
      const Tempest::Vec3 initial(0,(radius+padding)/cosine-radius-padding,0);
      auto walker=world.createVirtualCharacter(radius,180,initial);
      auto state=walker->move({},0,50,maxSlope,80,nullptr);
      requireNear((state.position-initial).length(),0.f,.001f,"zero-dt slope refresh must preserve the analytical initial pose");
      requireNear(state.groundNormal.y,cosine,.001f,"initial slope contact must use the actual plane normal");
      require(angle>50.f ? state.steep : state.grounded,"slope fixture must acquire its initial support without a warmup");
      Tempest::Vec3 atOneSecond;
      float firstSecond=0.f, later=0.f;
      // Include the game's 33 ms acquisition step: one native-gravity solve
      // alone exceeds 0.5 cm at 49 degrees even from an exact tangent pose.
      for(int frame=1;frame<=300;++frame) {
        state=walker->move({},frame%3==0 ? 34 : 33,50,maxSlope,80,nullptr);
        if(frame<=30)
          firstSecond=std::max(firstSecond,(state.position-initial).length());
        if(frame==30)
          atOneSecond=state.position;
        if(frame>30)
          later=std::max(later,(state.position-atOneSecond).length());
        }
      std::printf("CharacterVirtual %s slope %.0f deg: 0-1 s %.6f cm, 1-10 s %.6f cm, final (%.6f,%.6f,%.6f) cm\n",
                  planeKind==2?"compound mesh":planeKind==1?"mesh":"box",double(angle),double(firstSecond),double(later),
                  double(state.position.x),double(state.position.y),double(state.position.z));
      if(angle<=50.f) {
        require(firstSecond<=.5f,"an idle virtual human must move at most half a centimetre during the first second on a walkable slope");
        require(later<=.5f,"an idle virtual human must move at most half a centimetre from one to ten seconds on a walkable slope");
        require(state.grounded,"rest correction must preserve walkable slope support");
        } else {
        require(state.steep && state.position.x<initial.x-5.f && state.position.y<initial.y-5.f,
                "a non-walkable slope must retain downhill sliding instead of acquiring idle friction");
        }
      }
    }

  void testCharacterVirtualIdleStairIntent() {
    constexpr float radians=.01745329252f, angle=14.f*radians, maxSlope=50.f*radians;
    constexpr float radius=25.f, padding=1.f;
    const float sine=std::sin(angle), cosine=std::cos(angle);
    CollisionWorld world;
    auto matrix=at(50.f*sine,-50.f*cosine,0);
    matrix.rotateOZ(14.f);
    auto incline=world.addCollisionBody(new JPH::BoxShape(JPH::Vec3(20,.5f,20)),matrix,.7f,
                                        DynamicWorld::C_Landscape);
    // Touch an architectural riser above the supporting volume. The incline
    // remains the real ground, while the riser could trigger automatic stairs
    // if the uphill component of inward gravity were mistaken for user input.
    auto riser=world.addCollisionBody(new JPH::BoxShape(JPH::Vec3(.5f,.2f,2)),
                                      at(radius+padding+50.f,20.f,0),.7f,DynamicWorld::C_Object);
    riser->setLabel("idle-slope-riser");
    const Tempest::Vec3 initial(0,(radius+padding)/cosine-radius-padding,0);
    auto walker=world.createVirtualCharacter(radius,180,initial);
    walker->setDiagnosticsEnabled(true);
    auto state=walker->move({},0,50,maxSlope,80,nullptr);
    require(state.grounded,"idle stair intent fixture must start supported on its incline");
    requireNear(state.groundNormal.y,cosine,.001f,"the incline, rather than the touching riser, must remain the initial ground");
    CharacterVirtualDiagnostics diagnostic;
    require(walker->diagnostics(diagnostic),"idle stair intent fixture must retain its initial contacts");
    const bool touchesRiser=std::any_of(diagnostic.afterStairs.contacts.begin(),diagnostic.afterStairs.contacts.end(),
                                       [](const auto& contact) {
      return contact.body.label=="idle-slope-riser" && contact.hadCollision && !contact.discarded &&
             !contact.withinSupportingVolume && !contact.walkableSlope;
      });
    require(touchesRiser,"the idle incline fixture must touch a steep riser outside its supporting volume");
    float maximumDrift=0.f, maximumHorizontalDrift=0.f;
    for(int frame=0;frame<1000;++frame) {
      state=walker->move({},10,50,maxSlope,80,nullptr);
      require(walker->diagnostics(diagnostic),"idle stair intent must retain movement diagnostics");
      require(!diagnostic.stairAttempted,"inward gravity must never request an automatic stair walk without horizontal movement input");
      const auto displacement=state.position-initial;
      maximumDrift=std::max(maximumDrift,displacement.length());
      maximumHorizontalDrift=std::max(maximumHorizontalDrift,std::hypot(displacement.x,displacement.z));
      }
    std::printf("CharacterVirtual idle 14 deg incline at riser: maximum drift %.6f cm, horizontal %.6f cm\n",
                double(maximumDrift),double(maximumHorizontalDrift));
    require(state.grounded && maximumDrift<=.5f && maximumHorizontalDrift<=.5f,
            "an idle character touching a riser on an incline must stay supported within half a centimetre for ten seconds");
    }

  void testCharacterVirtualSlopedRiser() {
    constexpr float radians=.01745329252f, maxSlope=50.f*radians;
    constexpr float radius=8.17f, height=125.f, stepHeight=60.f;
    const float run=40.f/std::tan(54.5f*radians);
    for(bool narrowTread:{false,true}) {
      CollisionWorld world;
      auto floor=world.addCollisionBody(new JPH::BoxShape(JPH::Vec3(20,.5f,20)),at(0,-50,0),.7f,
                                        DynamicWorld::C_Landscape);
      JPH::TriangleList triangles;
      const auto surface=[&](float x0,float y0,float x1,float y1) {
        const JPH::Vec3 a(x0*.01f,y0*.01f,-2), b(x0*.01f,y0*.01f,2);
        const JPH::Vec3 c(x1*.01f,y1*.01f,2), d(x1*.01f,y1*.01f,-2);
        triangles.emplace_back(a,b,c);
        triangles.emplace_back(a,c,d);
        };
      surface(0,0,run,40);
      if(narrowTread) {
        // Both the old 15 cm endpoint and the maximum lookahead can hit a
        // steep face. The short tread between them must still be discoverable.
        surface(run,40,run+18,40);
        surface(run+18,40,2.f*run+18,80);
        surface(2.f*run+18,80,220,80);
        } else {
        surface(run,40,220,40);
        }
      const auto mesh=JPH::MeshShapeSettings(triangles).Create();
      require(!mesh.HasError(),"sloped riser fixture must construct its architectural mesh");
      auto stairs=world.addCollisionBody(mesh.Get(),at(0,0,0),.7f,DynamicWorld::C_Object);
      auto walker=world.createVirtualCharacter(radius,height,{-40,0,0});
      walker->setDiagnosticsEnabled(true);
      auto state=walker->move({},0,stepHeight,maxSlope,80,nullptr);
      require(state.grounded,"sloped riser traversal must start supported on the flat approach");
      const float target=narrowTread?120.f:100.f, landing=narrowTread?80.f:40.f;
      bool sawLongProbe=false;
      float progress=state.position.x;
      uint64_t stalled=0;
      int frames=0;
      for(;frames<180 && state.position.x<target;++frames) {
        state=walker->move({4,0,0},17,stepHeight,maxSlope,80,nullptr);
        CharacterVirtualDiagnostics diagnostic;
        require(walker->diagnostics(diagnostic),"sloped riser traversal must retain native stair diagnostics");
        if(diagnostic.stairSucceeded && (diagnostic.stairProbe.testPosition-diagnostic.stairProbe.upPosition).length()>15.5f)
          sawLongProbe=true;
        if(state.position.x-progress>=1.f) {
          progress=state.position.x;
          stalled=0;
          } else {
          stalled+=17;
          }
        require(stalled<=1000,"a traversable finite sloped riser must not stall for more than one second");
        }
      std::printf("CharacterVirtual 54.5 deg riser %s tread: frames %d, extended probe %d, feet (%.5f,%.5f,%.5f) cm\n",
                  narrowTread?"narrow":"wide",frames,sawLongProbe,double(state.position.x),double(state.position.y),double(state.position.z));
      require(state.grounded && state.position.x>=target,"a finite steep riser must reach its horizontal architectural landing");
      requireNear(state.position.y,landing,2.f,"finite sloped riser traversal must finish on the expected tread");
      require(sawLongProbe,"the finite riser contract must exercise a landing beyond the fixed fifteen-centimetre stair probe");
      }
    {
      CollisionWorld world;
      const float angle=55.f*radians, sine=std::sin(angle), cosine=std::cos(angle);
      const JPH::Vec3 along(cosine,sine,0), across(0,0,1);
      const auto a=-20.f*along-20.f*across, b=-20.f*along+20.f*across;
      const auto c=20.f*along+20.f*across, d=20.f*along-20.f*across;
      JPH::TriangleList triangles;
      triangles.emplace_back(a,b,c);
      triangles.emplace_back(a,c,d);
      const auto mesh=JPH::MeshShapeSettings(triangles).Create();
      require(!mesh.HasError(),"continuous steep slope control must construct its planar mesh");
      auto plane=world.addCollisionBody(mesh.Get(),at(0,0,0),.7f,DynamicWorld::C_Landscape);
      const Tempest::Vec3 initial(0,(radius+1.f)/cosine-radius-1.f,0);
      auto walker=world.createVirtualCharacter(radius,height,initial);
      walker->setDiagnosticsEnabled(true);
      auto state=walker->move({},0,stepHeight,maxSlope,80,nullptr);
      float highest=initial.y, furthest=initial.x;
      for(int frame=0;frame<120;++frame) {
        state=walker->move({4,0,0},17,stepHeight,maxSlope,80,nullptr);
        highest=std::max(highest,state.position.y);
        furthest=std::max(furthest,state.position.x);
        CharacterVirtualDiagnostics diagnostic;
        require(walker->diagnostics(diagnostic) && !diagnostic.stairSucceeded,
                "a continuous non-walkable slope without a tread must never become an automatic stair");
        }
      std::printf("CharacterVirtual continuous 55 deg slope: uphill progress %.6f cm, height gain %.6f cm\n",
                  double(furthest-initial.x),double(highest-initial.y));
      require(furthest<=initial.x+.5f && highest<=initial.y+.5f,
              "extending stair probes must not allow requested movement to climb a continuous steep slope");
      }
    }

  void testMonsterCharacterVirtualEligibility() {
    struct Case {
      const char* name;
      bool down, flying, allowedWhenEnabled;
      };
    const Case cases[]={
      {"a living ground monster must receive an enabled CV",false,false,true},
      {"a living flying monster must always retain legacy movement",false,true,false},
      {"a down ground monster must never receive CV",true,false,false},
      {"a down flying monster must never receive CV",true,true,false}};
    Gothic::PhysicsFlags flags;
    require(!flags.monsterCharacterVirtual,"an unconfigured PhysicsFlags must start with monster CV off; [GAME] supplies the game default");
    // Exercise the actual MoveAlgo policy for every independent hero/human
    // flag combination, including an on-the-fly monster flag round trip.
    for(bool hero:{false,true}) for(bool humans:{false,true}) {
      flags.characterVirtual=hero;
      flags.npcCharacterVirtual=humans;
      for(const auto& sample:cases)
        require(flags.allowsMonsterCharacterVirtual(sample.down,sample.flying)==false,
                "a disabled monster CV must reject every state regardless of hero/human flags");
      flags.monsterCharacterVirtual=true;
      for(const auto& sample:cases)
        require(flags.allowsMonsterCharacterVirtual(sample.down,sample.flying)==sample.allowedWhenEnabled,sample.name);
      flags.monsterCharacterVirtual=false;
      for(const auto& sample:cases)
        require(flags.allowsMonsterCharacterVirtual(sample.down,sample.flying)==false,
                "toggling monster CV off must restore legacy eligibility for every state");
      }
    std::printf("Monster CharacterVirtual eligibility: unconfigured OFF; four living/down/flying cases "
                "and four independent hero/human flag combinations passed OFF/ON/OFF\n");
    }

  void testMonsterCharacterVirtualIsolatedLedges() {
    struct Ledge {
      const char* monster;
      float radius, height, stepHeight, rise;
      float bottom=0;
      };
    constexpr std::array<Ledge,6> ledges={{{"WOLF",15,83,60,20},
                                            {"WOLF",15,83,60,40},
                                            {"WOLF",15,83,60,55},
                                            {"TROLL",68,272,100,40},
                                            {"TROLL",68,272,100,80},
                                            {"WOLF",15,83,60,40,-100}}};
    struct Result {
      bool supported=false, diagnostics=true, success=false;
      uint64_t elapsed=0;
      unsigned stalls=0, maximumStall=0;
      int stairAttempts=0, steepBounces=0;
      float maximumRise=0;
      };
    std::array<std::array<Result,2>,ledges.size()> results;
    constexpr float maxSlope=50.f*.01745329252f;
    Resources resources;
    for(size_t index=0;index<ledges.size();++index) {
      const auto& ledge=ledges[index];
      auto terrain=movableFloor();
      terrain.vertices.clear();
      terrain.polygons.vertex_indices.clear();
      terrain.polygons.material_indices.clear();
      const auto surface=[&](float x0,float y0,float x1,float y1) {
        const auto first=uint32_t(terrain.vertices.size());
        terrain.vertices.insert(terrain.vertices.end(),{{x0,y0,-1000},{x0,y0,1000},
                                                        {x1,y1,1000},{x1,y1,-1000}});
        for(uint32_t vertex:{0u,2u,1u,0u,3u,2u})
          terrain.polygons.vertex_indices.push_back(first+vertex);
        terrain.polygons.material_indices.insert(terrain.polygons.material_indices.end(),{0,0});
        };
      // One vertical face with a broad flat landing, rather than a flight of
      // short risers. The approach leaves both capsules clear of the ledge.
      surface(-2000,0,100,0);
      surface(100,ledge.bottom,100,ledge.rise);
      surface(100,ledge.rise,2000,ledge.rise);
      zenkit::ModelHierarchy hierarchy{};
      hierarchy.collision_bbox={{-ledge.radius,0,-ledge.radius},
                                {ledge.radius,ledge.height,ledge.radius}};
      Skeleton skeleton(asset::fromZenKit(hierarchy),nullptr,ledge.monster);
      const float mass=Ragdoll::speciesMass(ledge.monster);
      for(int policy=0;policy<2;++policy) {
        const bool limitStairAdvance=policy!=0;
        auto& result=results[index][size_t(policy)];
        DynamicWorld world(nullptr,terrain);
        auto monster=world.addGhostObj(&skeleton);
        monster.setPosition({-200,0,0});
        monster.setVirtualDiagnosticsEnabled(true);
        auto state=monster.moveVirtual({},0,ledge.stepHeight,maxSlope,mass,nullptr,limitStairAdvance,limitStairAdvance);
        result.supported=state.grounded;
        const float target=100.f+ledge.radius+80.f;
        float progress=state.position.x;
        unsigned stalled=0;
        int frames=0;
        for(;frames<300 && state.position.x<target;++frames) {
          const uint64_t dt=frames%3==0 ? 34 : 33;
          const auto previous=state.position;
          // The same running approach for both actual controller policies:
          // 210 cm/s at the video bench's 30 Hz update cadence.
          state=monster.moveVirtual({.21f*float(dt),0,0},dt,ledge.stepHeight,maxSlope,mass,nullptr,limitStairAdvance,limitStairAdvance);
          result.steepBounces+=state.blocked && state.steepContact ? 1 : 0;
          result.elapsed+=dt;
          result.maximumRise=std::max(result.maximumRise,state.position.y-previous.y);
          if(state.position.x-progress>=1.f) {
            progress=state.position.x;
            stalled=0;
            } else {
            const unsigned next=stalled+unsigned(dt);
            if(stalled<=1000 && next>1000)
              ++result.stalls;
            stalled=next;
            result.maximumStall=std::max(result.maximumStall,stalled);
            }
          CharacterVirtualDiagnostics diagnostic;
          const bool hasDiagnostic=monster.virtualDiagnostics(diagnostic);
          result.diagnostics&=hasDiagnostic;
          if(hasDiagnostic && diagnostic.stairAttempted)
            ++result.stairAttempts;
          }
        result.success=state.grounded && state.position.x>=target && std::abs(state.position.y-ledge.rise)<=2.f;
        std::printf("Monster CharacterVirtual %s isolated %.0f cm ledge, bottom %.0f cm, policy %s: "
                    "success %d, time %llu ms, frames %d, stalls %u, max stall %u ms, "
                    "max root rise %.6f cm, stair attempts %d, steep bounces %d, feet (%.6f,%.6f,%.6f) cm\n",
                    ledge.monster,double(ledge.rise),double(ledge.bottom),limitStairAdvance ? "monster" : "old",
                    int(result.success),static_cast<unsigned long long>(result.elapsed),frames,
                    result.stalls,result.maximumStall,double(result.maximumRise),result.stairAttempts,result.steepBounces,
                    double(state.position.x),double(state.position.y),double(state.position.z));
        }
      }
    // Finish all twelve runs before asserting, so a failed policy still leaves
    // the measurements of every requested ledge available to the caller.
    for(const auto& pair:results) for(const auto& result:pair) {
      require(result.supported,"isolated monster ledges must start with native flat-ground support");
      require(result.diagnostics,"isolated monster ledges must retain actual controller diagnostics");
      }
    for(const auto& pair:results)
      require(pair[0].success,"the old controller must establish that each isolated ledge is traversable within the guild step");
    for(const auto& pair:results) {
      require(pair[1].success,"the monster stair policy must preserve traversal of an isolated ledge within the guild step");
      require(pair[1].stalls==0,"an isolated ledge within the monster guild step must not stall walking for over one second");
      require(pair[1].steepBounces==0,"a traversable isolated ledge must not notify the monster AI of a steep-wall bounce");
      }
    }

  void testMonsterCharacterVirtualShortRisers() {
    constexpr float radius=15.f, height=83.f, stepHeight=60.f;
    constexpr float maxSlope=50.f*.01745329252f;
    constexpr int stepCount=12;
    constexpr float riser=10.f;
    Resources resources;
    zenkit::ModelHierarchy hierarchy{};
    hierarchy.collision_bbox={{-radius,0,-radius},{radius,height,radius}};
    Skeleton skeleton(asset::fromZenKit(hierarchy),nullptr,"WOLF");
    const float mass=Ragdoll::speciesMass("WOLF");
    // Both stair pitches are below the fifty-degree limit. Include a narrow
    // tread so the requested stair advance is checked with closely spaced risers too.
    for(float tread:{10.f,20.f}) {
      auto terrain=movableFloor();
      terrain.vertices.clear();
      terrain.polygons.vertex_indices.clear();
      terrain.polygons.material_indices.clear();
      const auto surface=[&](float x0,float y0,float x1,float y1) {
        const auto first=uint32_t(terrain.vertices.size());
        terrain.vertices.insert(terrain.vertices.end(),{{x0,y0,-1000},{x0,y0,1000},
                                                        {x1,y1,1000},{x1,y1,-1000}});
        for(uint32_t index:{0u,2u,1u,0u,3u,2u})
          terrain.polygons.vertex_indices.push_back(first+index);
        terrain.polygons.material_indices.insert(terrain.polygons.material_indices.end(),{0,0});
        };
      surface(-1000,0,100,0);
      for(int step=0;step<stepCount;++step) {
        const float x=100.f+tread*float(step), y=riser*float(step);
        surface(x,y,x,y+riser);
        surface(x,y+riser,step+1==stepCount ? 1000.f : x+tread,y+riser);
        }
      for(bool limitStairAdvance:{false,true}) {
        DynamicWorld world(nullptr,terrain);
        auto wolf=world.addGhostObj(&skeleton);
        wolf.setPosition({50,0,0});
        wolf.setVirtualDiagnosticsEnabled(true);
        // Use the monster policy of the actual walking controller. The 60 cm
        // guild step is deliberately much higher than these repeated city-sized
        // risers; limiting a test-only stepHeight would conceal the old shortcut.
        auto state=wolf.moveVirtual({},0,stepHeight,maxSlope,mass,nullptr,limitStairAdvance,limitStairAdvance);
        require(state.grounded,"wolf short-riser contract must start supported on the flat approach");
        const float target=100.f+float(stepCount-1)*tread+80.f;
        float maximumRise=0.f, maximumDrop=0.f, progress=state.position.x;
        unsigned stalled=0, maximumStall=0, airborne=0, maximumAirborne=0;
        int stairAttempts=0, steepBounces=0, frames=0;
        for(;frames<240 && state.position.x<target;++frames) {
          const uint64_t dt=frames%3==0 ? 34 : 33;
          const auto previous=state.position;
          // 210 cm/s at the same 30 Hz cadence as the paired video bench.
          state=wolf.moveVirtual({.21f*float(dt),0,0},dt,stepHeight,maxSlope,mass,nullptr,limitStairAdvance,limitStairAdvance);
          steepBounces+=state.blocked && state.steepContact ? 1 : 0;
          maximumRise=std::max(maximumRise,state.position.y-previous.y);
          maximumDrop=std::max(maximumDrop,previous.y-state.position.y);
          if(state.position.x-progress>=1.f) {
            progress=state.position.x;
            stalled=0;
            } else {
            stalled+=unsigned(dt);
            maximumStall=std::max(maximumStall,stalled);
            }
          airborne=state.grounded ? 0 : airborne+unsigned(dt);
          maximumAirborne=std::max(maximumAirborne,airborne);
          CharacterVirtualDiagnostics diagnostic;
          require(wolf.virtualDiagnostics(diagnostic),"wolf short-riser traversal must retain native stair diagnostics");
          stairAttempts+=diagnostic.stairAttempted ? 1 : 0;
          }
        std::printf("Monster CharacterVirtual WOLF 10 cm risers / %.0f cm treads at 30 Hz, limited %d: "
                    "frames %d, stair attempts %d, steep bounces %d, max root rise %.6f cm, max drop %.6f cm, "
                    "max stall %u ms, max airborne %u ms, feet (%.6f,%.6f,%.6f) cm\n",
                    double(tread),int(limitStairAdvance),frames,stairAttempts,steepBounces,double(maximumRise),double(maximumDrop),maximumStall,maximumAirborne,
                    double(state.position.x),double(state.position.y),double(state.position.z));
        if(!limitStairAdvance)
          continue;
        require(stairAttempts>0,"the ten-centimetre wolf staircase must exercise native automatic stair walking");
        require(steepBounces==0,"a traversable short-riser staircase must not notify the monster AI of a steep-wall bounce");
        require(maximumRise<=12.01f,"a walking monster must not raise its physical root by over twelve centimetres per city-stair update");
        require(state.grounded && state.position.x>=target,"the wolf must finish the repeated ten-centimetre staircase on its upper landing");
        requireNear(state.position.y,float(stepCount)*riser,2.f,"the wolf must finish at the twelve-riser landing height");
        require(maximumStall<=1000,"the limited monster stair walk must not stall for more than one second");
        require(maximumDrop<=2.f && maximumAirborne<=100,"ascending short risers must not make the wolf fall or lose support for over one tenth of a second");
        }
      }
    }

  void testMonsterCharacterVirtualSteepBlocking() {
    constexpr float radius=15.f, height=83.f, stepHeight=60.f;
    constexpr float radians=.01745329252f, maxSlope=50.f*radians;
    Resources resources;
    zenkit::ModelHierarchy hierarchy{};
    hierarchy.collision_bbox={{-radius,0,-radius},{radius,height,radius}};
    Skeleton skeleton(asset::fromZenKit(hierarchy),nullptr,"WOLF");
    const float mass=Ragdoll::speciesMass("WOLF");
    ProtoMesh wall(PackedMesh(movableCollisionBox({20,400,2000}),PackedMesh::PK_Visual),"MONSTER_BOUNCE_WALL.MRM");
    // A 45-degree request retains half its projected advancement while sliding
    // along this face. That is deliberately above the old ten-percent gate.
    for(bool kinematic:{false,true}) for(bool bounce:{false,true}) {
      DynamicWorld world(nullptr,movableFloor());
      auto structure=kinematic ? world.addMovableObj(wall.attach[0].shape.get(),at(100,200,0),"MONSTER_BOUNCE_WALL.MRM") :
                                 world.addStaticObj(wall.attach[0].shape.get(),at(100,200,0),"MONSTER_BOUNCE_WALL.MRM");
      require(!structure.isEmpty(),"monster bounce fixture must install its static or kinematic wall");
      auto wolf=world.addGhostObj(&skeleton);
      wolf.setPosition({50,0,0});
      auto state=wolf.moveVirtual({},0,stepHeight,maxSlope,mass,nullptr,true,bounce);
      require(state.grounded,"monster wall approach must begin with native ground support");
      unsigned slidingFrames=0, blockedFrames=0;
      for(int frame=0;frame<24;++frame) {
        state=wolf.moveVirtual({5,0,5},17,stepHeight,maxSlope,mass,nullptr,true,bounce);
        if(!state.steepContact || state.advancementRatio<=.2f || state.advancementRatio>=.60f)
          continue;
        ++slidingFrames;
        blockedFrames+=state.blocked ? 1u : 0u;
        require(state.grounded,"a wall contact must preserve the wolf's flat-ground support");
        requireNear(state.steepContactNormal.x,-1.f,.001f,"a steep contact must expose the wall normal opposing the requested X motion");
        requireNear(state.steepContactNormal.y,0.f,.001f,"a steep wall normal must not use the support floor's vertical normal");
        requireNear(state.steepContactNormal.z,0.f,.001f,"the wall normal must not follow the remaining sideways displacement");
        require(state.blocked==bounce,"only the enabled monster policy must turn diagonal steep-wall sliding into a movement failure");
        if(bounce) {
          require(state.collision.landCol && state.collision.npcCol==0.f,
                  "a blocked steep wall must notify MoveAlgo through the world branch rather than its NPC branch");
          requireNear((state.collision.normal-state.steepContactNormal).length(),0.f,.001f,
                      "MoveAlgo must receive the actual steep face normal for its native bounce");
          }
        }
      std::printf("Monster CharacterVirtual 45 deg %s wall, bounce %d: sliding frames %u, blocked %u, final advancement %.6f\n",
                  kinematic ? "kinematic" : "static",int(bounce),slidingFrames,blockedFrames,double(state.advancementRatio));
      require(slidingFrames>=10,"the diagonal wall contract must exercise sustained 20-60 percent advancement instead of a head-on stop");
      }
    {
      constexpr float angle=49.f*radians;
      auto terrain=movableFloor();
      for(auto& vertex:terrain.vertices)
        vertex.y=vertex.x*std::tan(angle);
      DynamicWorld world(nullptr,terrain);
      auto wolf=world.addGhostObj(&skeleton);
      const Tempest::Vec3 initial(0,(radius+1.f)/std::cos(angle)-radius-1.f,0);
      wolf.setPosition(initial);
      auto state=wolf.moveVirtual({},0,stepHeight,maxSlope,mass,nullptr,true,true);
      require(state.grounded,"walkable monster slope control must begin with support near the guild angle limit");
      for(int frame=0;frame<30;++frame) {
        state=wolf.moveVirtual({5,0,5},17,stepHeight,maxSlope,mass,nullptr,true,true);
        require(!state.blocked && !state.steepContact,
                "the enabled monster bounce policy must never classify a walkable slope as a steep obstruction");
        }
      require(state.grounded && state.position.x>initial.x+40.f && state.position.y>initial.y+40.f,
              "the wolf must continue diagonally uphill on a walkable slope with bouncing enabled");
      }
    {
      // Keep the light box taller than the wolf's guild step. A first diagonal
      // contact therefore resembles wall sliding, but a real dynamic hull is
      // excluded from the new blocker and must still receive walking impulses.
      const Tempest::Vec3 dimensions(40,83,400);
      ProtoMesh box(PackedMesh(movableBox(dimensions),PackedMesh::PK_Visual),"MONSTER_BOUNCE_BOX.MRM");
      const float multiplier=1.f/(.4f*.83f*4.f*DynamicWorld::materialDensity(asset::MaterialGroup::WOOD));
      DynamicWorld world(nullptr,movableFloor());
      auto prop=world.addMovableVob(box,"MONSTER_BOUNCE_BOX.MRM",at(100,height*.5f,0),multiplier,true,"crate",true,47);
      require(!prop.isEmpty() && prop.mass()>.9f && prop.mass()<1.1f,
              "dynamic monster bounce control must use a real lightweight native box");
      auto wolf=world.addGhostObj(&skeleton);
      wolf.setPosition({64,0,0});
      auto state=wolf.moveVirtual({},0,stepHeight,maxSlope,mass,nullptr,true,true);
      require(state.grounded,"dynamic box contact must begin with native floor support");
      for(int frame=0;frame<60;++frame) {
        state=wolf.moveVirtual({5,0,5},17,stepHeight,maxSlope,mass,nullptr,true,true);
        require(!state.blocked && !state.steepContact,
                "a pushable dynamic box must never become a steep-wall bounce while the wolf slides along and pushes it");
        world.tick(17);
        }
      const float travel=prop.position().x-100.f;
      std::printf("Monster CharacterVirtual 45 deg dynamic %.6f kg box: pushed %.6f cm, touched %d\n",
                  double(prop.mass()),double(travel),int(prop.touchedByNpc()));
      require(prop.touchedByNpc() && travel>20.f,
              "the enabled monster bounce policy must retain actual walking impulses and movement of a light dynamic box");
      }
    {
      DynamicWorld world(nullptr,movableFloor());
      auto wolf=world.addGhostObj(&skeleton);
      auto neighbour=world.addGhostObj(&skeleton);
      wolf.setPosition({0,0,0});
      neighbour.setPosition({100,0,0});
      world.tick(0);
      world.tick(0);
      bool blockedByNpc=false;
      for(int frame=0;frame<30;++frame) {
        const auto state=wolf.moveVirtual({5,0,0},17,stepHeight,maxSlope,mass,nullptr,true,true);
        require(!state.steepContact,"another NPC must never supply the static steep-face bounce contact");
        if(state.blocked) {
          require(state.collision.npcCol>0.f && !state.collision.landCol,
                  "monster bouncing must preserve the separate native NPC collision branch");
          blockedByNpc=true;
          }
        }
      require(blockedByNpc,"the enabled monster bounce control must exercise its native neighbouring-NPC collision");
      requireNear(neighbour.position().x,100.f,0.f,"a monster's NPC collision must not move its neighbour");
      }
    }

  void testMonsterCharacterVirtual() {
    testMonsterCharacterVirtualEligibility();
    testMonsterCharacterVirtualShortRisers();
    testMonsterCharacterVirtualIsolatedLedges();
    testMonsterCharacterVirtualSteepBlocking();
    constexpr float radians=.01745329252f;
    struct Monster {
      const char* name;
      float radius, height, stepHeight, slopeDegrees;
      };
    // Radius and full height come from the wolf/troll MDH collision bbox. The
    // CV uses that full height (obj->h), just as the existing human CV path.
    // The shorter legacy query capsule removes height/2 as ghost padding;
    // capsuleHeight() reports that query shape rather than the CV's height.
    // Guild limits are the installed GOTHIC.DAT Gil_Values: wolf 60 cm,
    // troll 100 cm; both use a fifty-degree slide angle.
    const Monster monsters[]={{"WOLF",15,83,60,50},{"TROLL",68,272,100,50}};
    Resources resources;
    for(const auto& monster:monsters) {
      zenkit::ModelHierarchy hierarchy{};
      hierarchy.collision_bbox={{-monster.radius,0,-monster.radius},
                                {monster.radius,monster.height,monster.radius}};
      Skeleton skeleton(asset::fromZenKit(hierarchy),nullptr,monster.name);
      const float mass=Ragdoll::speciesMass(monster.name);
      const float maxSlope=monster.slopeDegrees*radians;
      {
        const float rise=.8f*monster.stepHeight;
        auto terrain=movableFloor();
        terrain.vertices.clear();
        terrain.polygons.vertex_indices.clear();
        terrain.polygons.material_indices.clear();
        const auto surface=[&](float x0,float y0,float x1,float y1) {
          const auto first=uint32_t(terrain.vertices.size());
          terrain.vertices.insert(terrain.vertices.end(),{{x0,y0,-1000},{x0,y0,1000},
                                                          {x1,y1,1000},{x1,y1,-1000}});
          for(uint32_t index:{0u,2u,1u,0u,3u,2u})
            terrain.polygons.vertex_indices.push_back(first+index);
          terrain.polygons.material_indices.insert(terrain.polygons.material_indices.end(),{0,0});
          };
        surface(-2000,0,100,0);
        for(int step=0;step<3;++step) {
          const float x=100.f+200.f*float(step), y=rise*float(step);
          surface(x,y,x,y+rise);
          surface(x,y+rise,step==2 ? 2000.f : x+200.f,y+rise);
          }
        DynamicWorld world(nullptr,terrain);
        auto walker=world.addGhostObj(&skeleton);
        requireNear(walker.capsuleRadius(),monster.radius,.001f,
                    "monster CV radius must use the legacy MDH landscape capsule");
        requireNear(walker.capsuleHeight(),std::max(monster.height*.5f,2.f*monster.radius),.001f,
                    "monster legacy query height must retain MDH ghost padding without changing the full-height CV");
        walker.setPosition({-100,0,0});
        auto state=walker.moveVirtual({},0,monster.stepHeight,maxSlope,mass);
        const auto traverse=[&](bool ascending) {
          float progress=state.position.x;
          unsigned stalled=0, maximumStall=0;
          int frames=0;
          for(;frames<360 && (ascending ? state.position.x<700.f : state.position.x>-100.f);++frames) {
            state=walker.moveVirtual({ascending ? 5.f : -5.f,0,0},17,monster.stepHeight,maxSlope,mass);
            if((ascending ? state.position.x-progress : progress-state.position.x)>=1.f) {
              progress=state.position.x;
              stalled=0;
              } else {
              stalled+=17;
              maximumStall=std::max(maximumStall,stalled);
              }
            }
          std::printf("Monster CharacterVirtual %s stairs %s: radius %.3f cm, height %.3f cm, "
                      "step limit %.3f cm, rise %.3f cm, frames %d, max stall %u ms, feet (%.6f,%.6f,%.6f) cm\n",
                      monster.name,ascending ? "up" : "down",double(monster.radius),double(monster.height),
                      double(monster.stepHeight),double(rise),frames,maximumStall,
                      double(state.position.x),double(state.position.y),double(state.position.z));
          require(state.grounded && (ascending ? state.position.x>=700.f : state.position.x<=-100.f),
                  "wolf and troll MDH capsules must finish architectural stairs within their step limit");
          require(maximumStall<=1000,"monster stairs must not stall requested walking for over one second");
          requireNear(state.position.y,ascending ? 3.f*rise : 0.f,2.f,
                      "monster feet must finish at the expected stair landing");
          };
        traverse(true);
        traverse(false);
        const auto saved=state.position;
        walker.resetVirtual();
        const auto restored=walker.moveVirtual({},0,monster.stepHeight,maxSlope,mass);
        requireNear((restored.position-saved).length(),0.f,.5f,
                    "recreating a monster's transient CV must preserve its persisted feet within half a centimetre");
        }
      for(float angle:{std::min(25.f,monster.slopeDegrees-1.f),monster.slopeDegrees-1.f}) {
        const float cosine=std::cos(angle*radians), tangent=std::tan(angle*radians);
        auto terrain=movableFloor();
        for(auto& vertex:terrain.vertices)
          vertex.y=vertex.x*tangent;
        DynamicWorld world(nullptr,terrain);
        auto walker=world.addGhostObj(&skeleton);
        const Tempest::Vec3 initial(0,(monster.radius+1.f)/cosine-monster.radius-1.f,0);
        walker.setPosition(initial);
        auto state=walker.moveVirtual({},0,monster.stepHeight,maxSlope,mass);
        require(state.grounded,"monster slope fixture must acquire walkable support without settling");
        requireNear(state.groundNormal.y,cosine,.001f,"monster slope fixture must retain its actual plane normal");
        for(int frame=0;frame<90;++frame)
          state=walker.moveVirtual({4,0,0},17,monster.stepHeight,maxSlope,mass);
        const auto reached=state.position;
        require(state.grounded && reached.x>initial.x+100.f && reached.y>initial.y+40.f,
                "wolf and troll MDH capsules must walk uphill below their guild slope limit");
        float maximumDrift=0;
        for(int frame=0;frame<300;++frame) {
          state=walker.moveVirtual({},frame%3==0 ? 34 : 33,monster.stepHeight,maxSlope,mass);
          maximumDrift=std::max(maximumDrift,(state.position-reached).length());
          }
        std::printf("Monster CharacterVirtual %s slope %.3f deg / limit %.3f deg: "
                    "uphill %.6f cm, height gain %.6f cm, ten-second idle drift %.6f cm\n",
                    monster.name,double(angle),double(monster.slopeDegrees),double(reached.x-initial.x),
                    double(reached.y-initial.y),double(maximumDrift));
        require(state.grounded && maximumDrift<=.5f,
                "idle wolf and troll capsules must remain supported without sliding more than half a centimetre in ten seconds");
        }
      const auto push=[&](float bodyMass,int category) {
        CollisionWorld world;
        auto floor=world.addCollisionBody(new JPH::BoxShape(JPH::Vec3(20,.5f,20)),at(0,-50,0),.7f,
                                          DynamicWorld::C_Landscape);
        const float start=monster.radius+55.f;
        auto prop=world.addDynamicBody(new JPH::BoxShape(JPH::Vec3(.2f,monster.height*.005f,2.f)),
                                       at(start,monster.height*.5f,0),.7f,bodyMass,category,true);
        auto walker=world.createVirtualCharacter(monster.radius,monster.height,{0,0,0});
        CollisionWorld::CharacterMoveResult state;
        for(int frame=0;frame<60;++frame) {
          state=walker->move({5,0,0},17,monster.stepHeight,maxSlope,mass,nullptr);
          // Sample before integration: rotating the box later cannot disguise
          // a character that has already passed through the blocking side.
          require(state.position.x<prop->position().x-monster.radius-10.f,
                  "neither a light nor heavy native dynamic hull may be crossed by a walking monster");
          world.tick(17);
          }
        const float travel=prop->position().x-start;
        std::printf("Monster CharacterVirtual %s push %.3f kg: object %.6f cm, feet %.6f cm\n",
                    monster.name,double(bodyMass),double(travel),double(state.position.x));
        require(prop->touchedByNpc(),"monster walking must attribute a real native prop contact");
        return travel;
        };
      const float light=push(1,DynamicWorld::C_Item), heavy=push(122,DynamicWorld::C_MovableVob);
      require(light>20.f && heavy<light*.5f,
              "the shared walking effort must move a light prop faster than heavy furniture for both monster sizes");
      {
        CollisionWorld world;
        // Isolate the walking impulse from gravity and friction, as in the
        // human contract. Both bodies demand effort from the same move.
        constexpr float bodyMass=20.f, seconds=.1f;
        const float x=monster.radius+20.f, y=100.f+monster.height*.5f;
        const auto dimensions=JPH::Vec3(.1f,monster.height*.005f,monster.radius*.004f);
        auto a=world.addDynamicBody(new JPH::BoxShape(dimensions),at(x,y,-monster.radius*.6f),.4f,bodyMass,
                                    DynamicWorld::C_MovableVob,true);
        auto b=world.addDynamicBody(new JPH::BoxShape(dimensions),at(x,y,monster.radius*.6f),.4f,bodyMass,
                                    DynamicWorld::C_MovableVob,true);
        auto walker=world.createVirtualCharacter(monster.radius,monster.height,{0,100,0});
        walker->move({100,0,0},100,monster.stepHeight,maxSlope,mass,nullptr);
        const float impulseA=a->velocity().length()*.01f*bodyMass;
        const float impulseB=b->velocity().length()*.01f*bodyMass;
        const float strength=std::min(mass*DynamicWorld::gravityMS,450.f);
        const float totalLimit=strength*seconds, bodyLimit=totalLimit*mass/(mass+bodyMass);
        std::printf("Monster CharacterVirtual %s shared push: mass %.3f kg, %.6f + %.6f Ns, "
                    "total limit %.6f Ns, per-body %.6f Ns\n",monster.name,double(mass),double(impulseA),
                    double(impulseB),double(totalLimit),double(bodyLimit));
        require(impulseA>1.f && impulseB>1.f,"each monster push fixture must contact two distinct native bodies");
        require(impulseA<=bodyLimit+.01f && impulseB<=bodyLimit+.01f,
                "monster contact impulses must preserve the same continuous character/body mass share as humans");
        require(impulseA+impulseB<=totalLimit+.01f,
                "all monster contacts and stair probes must share the existing walking force budget capped at 450 newtons");
        }
      }
    }

  void testCharacterVirtual() {
    testCharacterVirtualSkippedMove();
    testCharacterVirtualInteractiveStep();
    testCharacterVirtualMovingUtensil();
    testCharacterVirtualIdleSlopes();
    testCharacterVirtualSupports();
    testCharacterVirtualSlopeRest();
    testCharacterVirtualIdleStairIntent();
    testCharacterVirtualSlopedRiser();
    testMonsterCharacterVirtual();
    constexpr float slope = .785398163f;
    const auto printState=[](const char* stage, int frames, const CollisionWorld::CharacterMoveResult& state) {
      std::fprintf(stderr,"CharacterVirtual %s: frames=%d feet=(%.3f,%.3f,%.3f) velocity=(%.3f,%.3f,%.3f) grounded=%d steep=%d normal=(%.3f,%.3f,%.3f)\n",
                   stage,frames,double(state.position.x),double(state.position.y),double(state.position.z),
                   double(state.velocity.x),double(state.velocity.y),double(state.velocity.z),state.grounded,state.steep,
                   double(state.groundNormal.x),double(state.groundNormal.y),double(state.groundNormal.z));
      };
    {
      CollisionWorld world;
      auto floor=world.addCollisionBody(new JPH::BoxShape(JPH::Vec3(20,.5f,20)),at(0,-50,0),.7f,
                                        DynamicWorld::C_Landscape);
      std::vector<std::unique_ptr<CollisionWorld::CollisionBody>> steps;
      for(int i=1;i<=3;++i)
        steps.push_back(world.addCollisionBody(new JPH::BoxShape(JPH::Vec3(.5f,.1f*float(i),2),0.f),
                                               at(100.f*float(i)+50.f,10.f*float(i),0),.7f,DynamicWorld::C_Object));
      struct Approach { const char* name; float side, heading, edgeAngle; };
      const Approach approaches[]={
        {"center",0,0,0},{"left",-10,0,0},{"right",10,0,0},
        {"heading-left",0,-15,0},{"heading-right",0,15,0},
        {"edge-walkable",0,15,44},{"edge-steep",0,-15,46}};
      for(const auto& approach:approaches) {
        constexpr float radians=.01745329252f;
        const float edgeAngle=approach.edgeAngle*radians;
        // With r=25 cm and 1 cm padding, this puts the lower sphere exactly
        // against the first tread's upper front edge. These two starts straddle
        // the 45-degree slope gate without relying on a lucky approach frame.
        const Tempest::Vec3 initial=approach.edgeAngle>0.f ?
          Tempest::Vec3(100.f-26.f*std::sin(edgeAngle),20.f+26.f*std::cos(edgeAngle)-25.f-1.f,approach.side) :
          Tempest::Vec3(0,0,approach.side);
        auto walker=world.createVirtualCharacter(25,180,initial);
        walker->setDiagnosticsEnabled(approach.edgeAngle>0.f);
        auto state=walker->move({},0,50,slope,80,nullptr);
        if(approach.edgeAngle>0.f) {
          CharacterVirtualDiagnostics diagnostic;
          require(walker->diagnostics(diagnostic),"edge approach must retain its initial native contacts");
          const bool nearThreshold=std::any_of(diagnostic.afterStairs.contacts.begin(),diagnostic.afterStairs.contacts.end(),
                                              [&](const auto& contact) {
            return contact.hadCollision && !contact.discarded &&
                   std::abs(contact.position.x-100.f)<.1f && std::abs(contact.position.y-20.f)<.1f &&
                   std::abs(contact.contactNormal.y-std::cos(edgeAngle))<.01f;
            });
          require(nearThreshold,"edge fixture must exercise a capsule contact normal next to the walkable slope threshold");
          }
        const Tempest::Vec3 request(5.f*std::cos(approach.heading*radians),0,5.f*std::sin(approach.heading*radians));
        const auto traverse=[&](bool ascending) {
          float progress=state.position.x;
          uint64_t stalled=0, maximumStall=0;
          int frames=0;
          for(;frames<180 && (ascending ? state.position.x<350.f : state.position.x>20.f);++frames) {
            state=walker->move(ascending?request:request*-1.f,17,50,slope,80,nullptr);
            if((ascending ? state.position.x-progress : progress-state.position.x)>=1.f) {
              progress=state.position.x;
              stalled=0;
              } else {
              stalled+=17;
              maximumStall=std::max(maximumStall,stalled);
              }
            if(stalled>1000) {
              std::fprintf(stderr,"CharacterVirtual stairs %s %s stalled: feet (%.4f,%.4f,%.4f), normal (%.4f,%.4f,%.4f)\n",
                           approach.name,ascending?"up":"down",double(state.position.x),double(state.position.y),double(state.position.z),
                           double(state.groundNormal.x),double(state.groundNormal.y),double(state.groundNormal.z));
              require(false,"twenty-centimetre stairs must never block a requested approach for more than one second");
              }
            }
          std::printf("CharacterVirtual stairs %s %s: frames %d, max stall %llu ms, feet (%.4f,%.4f,%.4f) cm\n",
                      approach.name,ascending?"up":"down",frames,static_cast<unsigned long long>(maximumStall),
                      double(state.position.x),double(state.position.y),double(state.position.z));
          require(state.grounded && (ascending ? state.position.x>=350.f : state.position.x<=20.f),
                  "virtual walking must finish twenty-centimetre stairs from every side, heading and threshold-edge approach");
          requireNear(state.position.y,ascending?60.f:0.f,2.f,"virtual capsule feet must finish on the expected stair landing");
          };
        traverse(true);
        traverse(false);
        }
      auto hero=world.createVirtualCharacter(25,180,{0,0,0});
      CollisionWorld::CharacterMoveResult state;
      auto wall=world.addCollisionBody(new JPH::BoxShape(JPH::Vec3(.1f,3,10)),at(600,300,0),.7f,
                                       DynamicWorld::C_Object);
      for(int i=0;i<150;++i)
        state=hero->move({5,0,5},17,50,slope,80,nullptr);
      printState("wall-slide-end",150,state);
      require(state.position.x<567 && state.position.z>650,
              "virtual walking must stop at the wall while retaining motion along it");
      hero->setPosition({-300,0,0});
      state=hero->move({},17,50,slope,80,nullptr);
      printState("teleport",1,state);
      requireNear(state.position.x,-300,.01f,"a virtual teleport must not retain old ground displacement");
      require(state.grounded,"a teleported virtual character must refresh its support");
    }
    auto push=[&](float mass, int category) {
      CollisionWorld world;
      auto floor=world.addCollisionBody(new JPH::BoxShape(JPH::Vec3(20,.5f,20)),at(0,-50,0),.7f,
                                        DynamicWorld::C_Landscape);
      auto prop=world.addDynamicBody(new JPH::BoxShape(JPH::Vec3(.2f,.4f,.4f)),at(80,40,0),.7f,mass,category,true);
      auto hero=world.createVirtualCharacter(25,180,{0,0,0});
      CollisionWorld::CharacterMoveResult state;
      for(int i=0;i<60;++i) {
        state=hero->move({5,0,0},17,10,slope,80,nullptr);
        world.tick(17);
        }
      printState("push-end",60,state);
      std::fprintf(stderr,"CharacterVirtual pushed body: mass=%.3f kg x=%.3f cm\n",double(mass),double(prop->position().x));
      require(state.position.x<prop->position().x-30,
              "virtual contacts must not allow the hero through a dynamic hull");
      return prop->position().x-80.f;
      };
    const float light=push(1,DynamicWorld::C_Item);
    const float heavy=push(122,DynamicWorld::C_MovableVob);
    require(light>20 && heavy<light*.5f,"the same character strength must push a light item faster than heavy furniture");
    std::printf("CharacterVirtual push: 1 kg %.2f cm, 122 kg %.2f cm\n",double(light),double(heavy));
    for(const int category:{DynamicWorld::C_Item,DynamicWorld::C_MovableVob}) {
      CollisionWorld world;
      auto floor=world.addCollisionBody(new JPH::BoxShape(JPH::Vec3(20,.5f,20)),at(0,-50,0),.7f,
                                        DynamicWorld::C_Landscape);
      auto mug=world.addDynamicBody(new JPH::BoxShape(JPH::Vec3(.07f,.08f,.07f)),at(80,8,0),.7f,5.8f,category,true);
      auto hero=world.createVirtualCharacter(25,180,{0,0,0});
      float highest=0.f;
      CollisionWorld::CharacterMoveResult state;
      for(int i=0;i<60;++i) {
        state=hero->move({5,0,0},17,50,slope,80,nullptr);
        highest=std::max(highest,state.position.y);
        world.tick(17);
        }
      std::printf("CharacterVirtual loose utensil: category %d moved %.3f cm, max feet %.3f cm\n",
                  category,double(mug->position().x-80.f),double(highest));
      require(highest<1.f,"walking into a loose item or small movable utensil must not lift the feet");
      require(mug->position().x>100.f && mug->touchedByNpc(),
              "a loose utensil excluded as a support must still receive native contact impulses");
      require(state.position.x>250.f,"a loose utensil must be pushed aside while the hero keeps walking");
    }
    {
      CollisionWorld world;
      auto mug=world.addDynamicBody(new JPH::BoxShape(JPH::Vec3(.2f,.08f,.2f)),at(0,8,0),.7f,5.8f,
                                     DynamicWorld::C_Item,true);
      auto hero=world.createVirtualCharacter(25,180,{0,16,0});
      CollisionWorld::CharacterMoveResult state;
      // Leave the rigid body suspended without ticking it: the feet can only
      // remain at 16 cm if this light body is erroneously accepted as a floor.
      for(int i=0;i<10;++i)
        state=hero->move({},17,50,slope,80,nullptr);
      require(!state.grounded && state.position.y<14.f,
              "even feet initially placed on a light item must fall rather than acquire it as support");
      requireNear(mug->velocity().y,0,.001f,"an excluded light support must not receive character support weight");
    }
    {
      CollisionWorld world;
      auto floor=world.addCollisionBody(new JPH::BoxShape(JPH::Vec3(20,.5f,20)),at(0,-50,0),.7f,
                                        DynamicWorld::C_Landscape);
      // A low table is deliberately within the 50 cm stair limit, including
      // the capsule's rounded toe. It must remain furniture rather than a step.
      auto table=world.addDynamicBody(new JPH::BoxShape(JPH::Vec3(.2f,.1f,.6f)),at(100,10,0),1.f,122.f,
                                       DynamicWorld::C_MovableVob,true);
      auto hero=world.createVirtualCharacter(25,180,{0,0,0});
      CollisionWorld::CharacterMoveResult state;
      float highest=0.f;
      for(int i=0;i<90;++i) {
        state=hero->move({5,0,0},17,50,slope,80,nullptr);
        highest=std::max(highest,state.position.y);
        world.tick(17);
        }
      std::printf("CharacterVirtual low table: hero %.3f cm, table %.3f cm, max feet %.3f cm\n",
                  double(state.position.x),double(table->position().x),double(highest));
      require(highest<1.f,"ordinary walking must neither auto-step nor roll the capsule up a movable table");
      require(state.position.x<table->position().x-35.f,
              "a table excluded from automatic stepping must still block the capsule");
    }
    {
      CollisionWorld world;
      // No floor or simulation tick: measured linear momentum comes only from
      // character contact impulses, without gravity, support weight or friction.
      // Two narrow bodies block the same capsule. Each can demand a full force
      // allowance; repeated contacts / stair probes must share the move budget.
      constexpr float mass=20.f,seconds=.1f;
      auto a=world.addDynamicBody(new JPH::BoxShape(JPH::Vec3(.1f,.9f,.1f)),at(50,190,-20),.4f,mass,
                                  DynamicWorld::C_MovableVob,true);
      auto b=world.addDynamicBody(new JPH::BoxShape(JPH::Vec3(.1f,.9f,.1f)),at(50,190,20),.4f,mass,
                                  DynamicWorld::C_MovableVob,true);
      auto hero=world.createVirtualCharacter(25,180,{0,100,0});
      hero->move({100,0,0},100,50,slope,80,nullptr);
      const float impulseA=a->velocity().length()*.01f*mass;
      const float impulseB=b->velocity().length()*.01f*mass;
      constexpr float totalLimit=450.f*seconds;
      constexpr float bodyLimit=450.f*80.f/(80.f+mass)*seconds;
      std::printf("CharacterVirtual shared push: %.3f + %.3f Ns, total limit %.3f Ns, per-body %.3f Ns\n",
                  double(impulseA),double(impulseB),double(totalLimit),double(bodyLimit));
      require(impulseA>1.f && impulseB>1.f,"the shared push fixture must exercise two native contact bodies");
      require(impulseA<=bodyLimit+.01f && impulseB<=bodyLimit+.01f,
              "all contacts of one body must share its mass-dependent walking effort");
      require(impulseA+impulseB<=totalLimit+.01f,
              "all bodies and repeated contacts must share one 450 newton walking budget per move");
    }
    {
      zenkit::ModelHierarchy hierarchy{};
      hierarchy.collision_bbox = {{-25,0,-25},{25,180,25}};
      Skeleton shape(asset::fromZenKit(hierarchy),nullptr,"VIRTUAL_NPC_CONTRACT");
      DynamicWorld world(nullptr,asset::WorldMesh{});
      auto hero=world.addGhostObj(&shape);
      auto npc=world.addGhostObj(&shape);
      hero.setPosition({0,0,0});
      npc.setPosition({100,0,0});
      world.tick(0);
      world.tick(0); // leave the other NPC in the frozen broad-phase list
      for(int i=0;i<30;++i)
        hero.moveVirtual({5,0,0},17,50,slope,80);
      require(hero.position().x<55,"virtual hero walking must retain existing NPC ellipsoid obstacles");
      requireNear(npc.position().x,100,0,"virtual hero contacts must never move another NPC");
      hero.setPosition({-200,50,30});
      hero.resetVirtual();
      const auto after=hero.moveVirtual({},0,50,slope,80);
      requireNear(after.position.x,-200,.001f,"controller recreation must retain the saved or teleported feet x");
      requireNear(after.position.y,50,.001f,"controller recreation must retain the saved or teleported feet y");
      requireNear(after.position.z,30,.001f,"controller recreation must retain the saved or teleported feet z");
    }
    {
      zenkit::ModelHierarchy hierarchy{};
      hierarchy.collision_bbox = {{-25,0,-25},{25,180,25}};
      Skeleton shape(asset::fromZenKit(hierarchy),nullptr,"VIRTUAL_NPC_PAIR_CONTRACT");
      DynamicWorld world(nullptr,movableFloor());
      auto left=world.addGhostObj(&shape);
      auto right=world.addGhostObj(&shape);
      left.setPosition({0,0,0});
      right.setPosition({200,0,0});
      world.tick(0);
      world.tick(0);
      for(int i=0;i<40;++i) {
        left.moveVirtual({5,0,0},17,50,slope,80);
        right.moveVirtual({-5,0,0},17,50,slope,80);
        require(right.position().x-left.position().x>=49.9f,
                "two virtual NPCs moving towards each other must never cross or interpenetrate");
        }
      require(left.position().x>60.f && right.position().x<140.f,
              "both virtual NPCs must advance before their mutual blocking contact");
      requireNear(left.position().y,0.f,1.f,"an NPC contact must not lift virtual feet onto another NPC");
      requireNear(right.position().y,0.f,1.f,"an opposing NPC contact must retain floor support");
      const auto stoppedRight=right.position();
      const auto blocked=left.moveVirtual({5,0,0},17,50,slope,80);
      require(blocked.blocked && blocked.collision.npcCol>0.f,
              "unsuccessful virtual NPC advancement must expose its blocking character contact to MoveAlgo");
      requireNear(right.position().x,stoppedRight.x,0.f,"a virtual NPC contact must never push its neighbour");
      const auto sway=left.moveVirtual({1.9f,0,0},17,50,slope,80);
      require(!sway.blocked,"a legacy-skipped sway into another NPC must not notify AI of movement failure");
      requireNear(right.position().x,stoppedRight.x,0.f,"a skipped sway must preserve the neighbouring NPC obstacle");
      right.setEnable(false);
      for(int i=0;i<20;++i)
        left.moveVirtual({5,0,0},17,50,slope,80);
      require(left.position().x>150.f,"a seated character with disabled collision must not leave a virtual obstacle");
    }
    {
      zenkit::ModelHierarchy hierarchy{};
      hierarchy.collision_bbox = {{-25,0,-25},{25,180,25}};
      Skeleton shape(asset::fromZenKit(hierarchy),nullptr,"VIRTUAL_NPC_OVERLAP_CONTRACT");
      DynamicWorld world(nullptr,movableFloor());
      auto walker=world.addGhostObj(&shape);
      auto overlapping=world.addGhostObj(&shape);
      auto clearNeighbour=world.addGhostObj(&shape);
      walker.setPosition({0,0,0});
      overlapping.setPosition({40,0,0});
      clearNeighbour.setPosition({-55,0,0});
      for(int i=0;i<4;++i)
        walker.moveVirtual({-5,0,0},17,50,slope,80);
      require(walker.position().x>=-5.01f && walker.position().x<-4.f,
              "separating a teleported overlap must not transfer penetration to a previously clear neighbour");
      requireNear(overlapping.position().x,40.f,0.f,"overlap recovery must not displace the original neighbour");
      requireNear(clearNeighbour.position().x,-55.f,0.f,"overlap recovery must not displace the clear neighbour");
    }
    {
      Resources resources;
      ProtoMesh wall(PackedMesh(movableCollisionBox({20,400,600}),PackedMesh::PK_Visual),"VIRTUAL_NPC_WALL.MRM");
      zenkit::ModelHierarchy hierarchy{};
      hierarchy.collision_bbox = {{-25,0,-25},{25,180,25}};
      Skeleton shape(asset::fromZenKit(hierarchy),nullptr,"VIRTUAL_NPC_WALL_CONTRACT");
      DynamicWorld world(nullptr,movableFloor());
      auto structure=world.addStaticObj(wall.attach[0].shape.get(),at(50,200,0),"VIRTUAL_NPC_WALL.MRM");
      auto walker=world.addGhostObj(&shape);
      auto obstacle=world.addGhostObj(&shape);
      walker.setPosition({0,0,0});
      obstacle.setPosition({15,0,150});
      // The diagonal desired motion misses the NPC, while wall sliding would
      // encounter it. The post-solve clip must retain a world-safe pose.
      const auto stopped=walker.moveVirtual({200,0,200},100,50,slope,80);
      require(!structure.isEmpty(),"virtual NPC wall fixture must install its static collision mesh");
      require(stopped.position.x<=15.1f && stopped.position.z<100.f,
              "a wall-resolved virtual move must not cross a character or interpolate feet into the wall");
      requireNear(obstacle.position().x,15.f,0.f,"wall sliding must not push a neighbouring NPC through the wall");
      requireNear(obstacle.position().z,150.f,0.f,"wall sliding must leave the neighbouring NPC at its own feet");
    }
    }

  void testVirtualFloorGuard() {
    constexpr float slope=.785398163f;
    zenkit::ModelHierarchy hierarchy{};
    hierarchy.collision_bbox={{-25,0,-25},{25,180,25}};
    Skeleton shape(asset::fromZenKit(hierarchy),nullptr,"VIRTUAL_FLOOR_GUARD_CONTRACT");
    auto ledge=movableFloor();
    for(auto& vertex:ledge.vertices)
      vertex.x=vertex.x>0 ? 0.f : vertex.x;
    for(bool lowerFloor:{false,true}) {
      auto mesh=ledge;
      if(lowerFloor) {
        auto lower=movableFloor();
        for(auto vertex:lower.vertices) {
          vertex.y=-500.f;
          mesh.vertices.push_back(vertex);
          }
        for(auto index:lower.polygons.vertex_indices)
          mesh.polygons.vertex_indices.push_back(index+4);
        mesh.polygons.material_indices.insert(mesh.polygons.material_indices.end(),{0,0});
        }
      DynamicWorld world(nullptr,mesh);
      auto walker=world.addGhostObj(&shape);
      walker.setPosition({-35,0,0});
      walker.moveVirtual({},17,50,slope,80);
      const auto start=walker.position();
      require(walker.hasFloorBelow(start),"the virtual floor guard fixture must start over supported world");
      require(walker.hasFloorBelow({45,80,20})==lowerFloor,
              "jump handoff must distinguish a bottomless lateral step from a lower landing floor");
      auto moved=walker.moveVirtual({80,0,20},100,50,slope,80);
      if(!lowerFloor) {
        require(moved.blocked && moved.collision.preFall,
                "a virtual step beyond all floor must report the legacy pre-fall failure");
        requireNear((moved.position-start).length(),0,.001f,
                    "a rejected bottomless virtual step must retain the known-safe feet");
        for(int i=0;i<180;++i)
          moved=walker.moveVirtual({5,0,1},17,50,slope,80);
        require(moved.position.x<=0.f && moved.position.y>-.5f,
                "repeated virtual steps must not enter a bottomless world boundary");
        } else {
        require(!moved.collision.preFall && moved.position.x>0.f,
                "a real drop with a lower floor must remain accessible to virtual walking");
        for(int i=0;i<180;++i)
          moved=walker.moveVirtual({0,moved.velocity.y*.017f,0},17,50,slope,80);
        require(moved.position.y<-450.f && moved.grounded,
                "virtual feet must be able to fall and land on a lower floor");
        }
      }
    }

  void testItemWake() {
    for(const bool legacy:{false,true}) {
      CollisionWorld world;
      std::vector<JPH::Vec3> points={{0,0,0},{1,0,0},{0,1,0},{0,0,1}};
      JPH::ShapeRefC shape;
      if(legacy) {
        points.clear();
        for(unsigned corner=0;corner<8;++corner)
          points.emplace_back(corner&1 ? .8f : .2f,corner&2 ? .3f : -.1f,corner&4 ? .1f : -.3f);
        shape=new JPH::RotatedTranslatedShape({.5f,.1f,-.1f},JPH::Quat::sIdentity(),
                                               new JPH::BoxShape({.3f,.2f,.2f}));
        }
      else {
        const auto hull=JPH::ConvexHullShapeSettings(points.data(),int(points.size()),0.f).Create();
        require(!hull.HasError(),"tight item bounds fixture requires an asymmetric convex hull");
        shape=hull.Get();
        }
      auto matrix=at(100,200,300);
      matrix.rotateOZ(225);
      matrix.scale(1.2f,.7f,1.1f);
      auto item=world.addDynamicBody(shape,matrix,.7f,1,DynamicWorld::C_Item,true);
      Tempest::Vec3 min(std::numeric_limits<float>::infinity()),max(-std::numeric_limits<float>::infinity());
      for(const auto& vertex:points) {
        auto point=CollisionWorld::toCentimeters(vertex);
        matrix.project(point);
        min.x=std::min(min.x,point.x); min.y=std::min(min.y,point.y); min.z=std::min(min.z,point.z);
        max.x=std::max(max.x,point.x); max.y=std::max(max.y,point.y); max.z=std::max(max.z,point.z);
        }
      const auto snapshot=world.waterBodySnapshot();
      require(snapshot.size()==1,"tight item bounds must describe the existing native body");
      const auto actualMin=CollisionWorld::toMeters(snapshot[0].collisionBounds[0]);
      const auto actualMax=CollisionWorld::toMeters(snapshot[0].collisionBounds[1]);
      const auto expectedMin=CollisionWorld::toMeters(min),expectedMax=CollisionWorld::toMeters(max);
      for(size_t axis=0;axis<3;++axis) {
        requireNear(actualMin[axis],expectedMin[axis],.00003f,
                    "item collision minima must match transformed support points including COM and scale");
        requireNear(actualMax[axis],expectedMax[axis],.00003f,
                    "item collision maxima must match transformed support points including COM and scale");
        }
      JPH::Vec3 broadMin,broadMax;
      item->getAabb(broadMin,broadMax);
      requireNear(snapshot[0].worldBounds[0].y,broadMin.GetY()*100.f,.003f,
                  "tight item diagnostics must leave the existing water bounds unchanged");
      if(!legacy)
        require(snapshot[0].collisionBounds[0].y-broadMin.GetY()*100.f>20.f,
                "a rotated asymmetric hull's empty broad-phase corner must not count as physical penetration");
      }
    Resources resources;
    ProtoMesh proto(PackedMesh(movableBox({30,30,30}),PackedMesh::PK_Visual),"CONTRACT_WAKE_ITEM.MRM");
    DynamicWorld shapes(nullptr,{});
    const auto& hull=shapes.itemHullShape(proto,"CONTRACT_WAKE_ITEM.MRM");
    require(hull.shape!=nullptr,"item wake must exercise a genuine mesh hull");
    for(const bool hulls:{false,true}) {
      const JPH::ShapeRefC shape=hulls ? hull.shape : JPH::ShapeRefC(new JPH::BoxShape({.15f,.15f,.15f}));
      for(const int category:{DynamicWorld::C_Ragdoll,DynamicWorld::C_Fragment,DynamicWorld::C_Item,
                              DynamicWorld::C_MovableVob}) {
        for(const bool enabled:{false,true}) {
          CollisionWorld world;
          world.setItemWakeEnabled(enabled);
          auto floor=world.addCollisionBody(new JPH::BoxShape({20,.5f,20}),at(0,-50,0),.7f,
                                            DynamicWorld::C_Landscape);
          std::unique_ptr<CollisionWorld::DynamicBody> item;
          const Tempest::Vec3 rest(0,16,0);
          world.setItemWakeCallback([&](const Tempest::Vec3& min,const Tempest::Vec3& max,size_t budget) {
            require(budget>0 && budget<=8,"item wake must bound new bodies per native step");
            if(rest.x<min.x || rest.x>max.x || rest.y<min.y || rest.y>max.y || rest.z<min.z || rest.z>max.z)
              return size_t(0);
            const bool missing=item==nullptr;
            if(missing)
              item=world.addDynamicBody(shape,at(rest.x,rest.y,rest.z),.7f,1,
                                         DynamicWorld::C_Item,true);
            const auto id=item->nativeId();
            const auto velocity=item->velocity();
            const bool sleeping=item->isSleeping();
            item->retainForItemWake();
            require(item->nativeId()==id && item->mass()==1.f && item->isSleeping()==sleeping &&
                    (item->velocity()-velocity).quadLength()==0.f,
                    "item wake retention must preserve existing mass, sleep, velocity and identity");
            return size_t(missing);
            });
          auto moving=world.addDynamicBody(new JPH::BoxShape({.2f,.2f,.2f}),at(-100,20,0),.7f,10,category);
          moving->setVelocity({500,0,0},{});
          for(int i=0;i<120;++i)
            world.tick(17);
          const float displacement=item ? (item->position()-rest).length() : 0.f;
          std::printf("Item wake hulls %d layer %d enabled %d: created %d, displacement %.6f cm\n",
                      int(hulls),category,int(enabled),int(item!=nullptr),double(displacement));
          require(enabled ? item!=nullptr && displacement>5.f : item==nullptr,
                  "every active dynamic body kind must create and displace a bodyless item only with item_wake ON");
          }
        }
    }
    {
      CollisionWorld world;
      std::vector<std::unique_ptr<CollisionWorld::DynamicBody>> sources;
      for(int i=0;i<40;++i)
        sources.push_back(world.addDynamicBody(new JPH::BoxShape({.1f,.1f,.1f}),at(1000.f*i,1000,0),.7f,1));
      size_t queries=0;
      std::vector<float> centers;
      world.setItemWakeCallback([&](const Tempest::Vec3& min,const Tempest::Vec3& max,size_t) {
        ++queries;
        requireNear(max.x-min.x,120,.01f,"wake bounds must pad the native AABB by fifty centimetres on each side");
        centers.push_back((min.x+max.x)*.5f);
        return size_t(0);
        });
      world.tick(17);
      require(queries==32,"a native step must issue no more than thirty-two item queries");
      world.tick(17);
      std::sort(centers.begin(),centers.end());
      centers.erase(std::unique(centers.begin(),centers.end()),centers.end());
      require(queries==64 && centers.size()==40,"bounded wake queries must rotate through all active bodies");
      size_t created=0;
      queries=0;
      world.setItemWakeCallback([&](const Tempest::Vec3&,const Tempest::Vec3&,size_t budget) {
        ++queries;
        created+=budget;
        return budget;
        });
      world.tick(17);
      require(queries==1 && created==8,"the per-step creation budget must be shared by all wake queries");
      }
    for(const bool enabled:{false,true}) {
      CollisionWorld world;
      world.setItemWakeEnabled(enabled);
      world.setBBox({-20,-1,-20},{20,20,20});
      auto initial=at(100,500,0);
      initial.rotateOY(30);
      auto item=world.addDynamicBody(new JPH::BoxShape({.1f,.2f,.3f}),initial,.7f,1);
      world.tick(17);
      item->setPosition({0,-10200,0});
      item->setVelocity({100,-200,300},{1,2,3});
      world.tick(17);
      requireMatrix(item->matrix(),initial,"an item that never slept must recover its creation transform");
      require(item->isSleeping() && item->velocity().quadLength()==0.f && item->angularVelocity().quadLength()==0.f,
              "underworld recovery must zero both velocities and sleep independently of item_wake");
      auto rest=at(300,400,200);
      rest.rotateOX(45);
      item->setObjMatrix(rest);
      world.tick(17);
      item->addImpulse({1,1,1});
      world.tick(17);
      item->setPosition({0,-10200,0});
      world.tick(17);
      requireMatrix(item->matrix(),rest,"recovery must use the last sleeping full transform instead of creation");
      const auto loaded=at(600,700,800);
      item->restoreTransform(loaded,false);
      item->setPosition({0,-10200,0});
      world.tick(17);
      requireMatrix(item->matrix(),loaded,"a loaded awake item's fallback must be its loaded transform");
      item->addImpulse({1,2,3});
      item->setPosition({std::numeric_limits<float>::quiet_NaN(),0,0});
      world.tick(17);
      requireMatrix(item->matrix(),loaded,"nonfinite item coordinates must recover before reaching the native broad phase");
      require(item->isSleeping() && item->velocity().quadLength()==0.f && item->angularVelocity().quadLength()==0.f,
              "nonfinite recovery must sleep and zero the restored body");
      item->restoreTransform(at(0,std::numeric_limits<float>::infinity(),0),false);
      world.tick(17);
      requireMatrix(item->matrix(),loaded,"rejecting a nonfinite restore must not replace the valid recovery transform");
      require(item->isSleeping(),"rejecting a nonfinite restore must preserve recovery sleep");
      auto broken=world.addDynamicBody(new JPH::BoxShape({.1f,.1f,.1f}),at(0,std::numeric_limits<float>::quiet_NaN(),0),.7f,1);
      world.tick(17);
      broken->setPosition({std::numeric_limits<float>::quiet_NaN(),0,0});
      world.tick(17);
      require(broken->isSleeping(),"an item created from a nonfinite pose must sleep instead of recursing into recovery");
      }
    for(const bool hulls:{false,true}) {
      DynamicWorld world(nullptr,{});
      Bounds bounds;
      bounds.assign(proto.bbox());
      const auto loaded=at(100,200,300);
      auto item=world.addDynamicObj(loaded,bounds,asset::MaterialGroup::WOOD,&proto,"CONTRACT_WAKE_ITEM.MRM",hulls);
      const float mass=item.mass();
      const auto id=world.waterBodySnapshot()[0].id;
      item.restorePhysics(loaded,true);
      item.retainForItemWake();
      require(item.isSleeping() && item.mass()==mass && item.velocity().quadLength()==0.f,
              "retaining either item shape must preserve its mass, sleep and zero velocity");
      item.restorePhysics(loaded,false,{100,200,300},{1,2,3});
      const auto velocity=item.velocity(),angular=item.angularVelocity();
      item.retainForItemWake();
      require(!item.isSleeping() && item.mass()==mass && world.waterBodySnapshot()[0].id==id &&
              (item.velocity()-velocity).quadLength()==0.f && (item.angularVelocity()-angular).quadLength()==0.f,
              "retaining either moving item shape must preserve body identity, mass and both velocities");
      item.setObjMatrix(at(0,-10200,0));
      world.tick(17);
      requireMatrix(item.matrix(),loaded,"either item shape must recover its loaded awake position");
      require(item.isSleeping() && item.velocity().quadLength()==0.f && item.angularVelocity().quadLength()==0.f,
              "either recovered item shape must sleep with zero velocities");
      item.restorePhysics(at(0,std::numeric_limits<float>::infinity(),0),false,{100,200,300},{1,2,3});
      requireMatrix(item.matrix(),loaded,"the Item restore wrapper must retain its last valid loaded transform");
      require(item.isSleeping() && item.velocity().quadLength()==0.f && item.angularVelocity().quadLength()==0.f,
              "the Item restore wrapper must not restore saved motion after recovering invalid coordinates");
      }
    {
      CollisionWorld world;
      world.setWaterSurfaceCallback([](const Tempest::Vec3&,float,float& surface) { surface=0; return true; });
      const auto floating=at(0,-8,0),dry=at(100,100,0);
      auto wetItem=world.addDynamicBody(new JPH::BoxShape({.2f,.2f,.2f}),floating,.7f,44.8f,
                                       DynamicWorld::C_Item,true);
      auto dryItem=world.addDynamicBody(new JPH::BoxShape({.2f,.2f,.2f}),dry,.7f,44.8f,
                                       DynamicWorld::C_Item,true);
      wetItem->restoreTransform(floating,true);
      dryItem->restoreTransform(dry,true);
      for(const bool enabled:{false,true}) {
        world.setItemWakeEnabled(enabled);
        for(int frame=0;frame<12;++frame)
          world.tick(frame%3==2 ? 16 : 17);
        require(wetItem->isSleeping() && wetItem->keepSleepingItem(wetItem->position()),
                "the production retirement rule must retain a sleeping water item with item_wake ON or OFF");
        require(dryItem->isSleeping() && !dryItem->keepSleepingItem(dryItem->position()),
                "a dry sleeper without a nearby character or wake request must still retire");
        requireMatrix(wetItem->matrix(),floating,"item_wake must not move an already sleeping water item");
        require(wetItem->velocity().quadLength()==0.f,"item_wake must preserve a sleeping water item's zero velocity");
        }
      }
    for(const bool hulls:{false,true}) {
      DynamicWorld world(nullptr,waterBasin(-15000));
      world.setItemWakeEnabled(true);
      Bounds bounds;
      bounds.assign(proto.bbox());
      auto item=world.addDynamicObj(at(0,110,0),bounds,asset::MaterialGroup::METAL,
                                    &proto,"CONTRACT_WAKE_ITEM.MRM",hulls);
      bool belowSurfaceHundredMeters=false;
      for(int frame=0;frame<1800;++frame) {
        world.tick(frame%3==2 ? 16 : 17);
        const auto y=item.position().y;
        if(belowSurfaceHundredMeters)
          require(y<-10000,"a sinking item inside a deep world must not return to its surface creation position");
        belowSurfaceHundredMeters|=y<-10100;
        }
      std::printf("Item wake deep water hulls %d: final %.6f cm, sleeping %d\n",
                  int(hulls),double(item.position().y),int(item.isSleeping()));
      require(belowSurfaceHundredMeters && item.position().y<-14950 && item.position().y>-15050 && item.isSleeping(),
              "a metal item must reach the deep basin floor beyond one hundred metres without underworld recovery");
      }
    }

  void testContracts() {
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

int main() {
  try {
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
    testNpcScalarWitness();
    testNpcSeparatedContactResponse();
    testFixedStepCarry();
    testFixedStepCadence();
    testItemWake();
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
    testRotatingFragments();
    testPhysicsNoise();
    testRagdollNoise();
    testOffCenterImpulse();
    std::puts("Physics contracts passed");
    return EXIT_SUCCESS;
    }
  catch(const std::exception& error) {
    std::fprintf(stderr,"FAIL: physics contract exception: %s\n",error.what());
    return EXIT_FAILURE;
    }
  }
