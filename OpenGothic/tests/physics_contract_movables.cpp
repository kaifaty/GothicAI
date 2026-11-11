#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#include "physics_contract_common.h"
#include "asset/fromzenkit.h"
#include "physics/collisionworld.h"
#include "physics/physicvbo.h"
#include "graphics/mesh/protomesh.h"
#include "graphics/mesh/skeleton.h"
#include "graphics/mesh/submesh/packedmesh.h"
#include "game/serialize.h"
#include "basemesh.h"

#include <Tempest/Log>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/CollisionGroup.h>
#include <zenkit/ModelHierarchy.hh>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace PhysicsContract {
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
    for(const bool movable:{false,true}) {
      // A blade stopped by an intact fence bites into it: the board takes that blow at
      // the contact, once per swing, and breaks when its health runs out.
      DynamicWorld world(nullptr,movableFloor());
      world.setMovableVobsEnabled(movable);
      auto fence=world.addMovableVob(proto,"NW_NATURE_FENCE_01.MRM",at(100,50,0),.04f,true,"fence",true,zenId);
      const auto fenceId=world.movableVobSnapshot()[0].id;
      int actorId=0;
      auto* actor=reinterpret_cast<Npc*>(&actorId);
      const auto blow=[&](uint64_t swing,float z) {
        const uint64_t t=swing*1000;
        world.updateShadowBlade(actor,swing,{{{-10,30,z},{-10,70,z},{0,0,1},2}},t,double(t),double(t+100));
        world.flushShadowBlades();
        world.updateShadowBlade(actor,swing,{{{200,30,z},{200,70,z},{0,0,1},2}},t+100,double(t),double(t+100));
        world.flushShadowBlades();
        const auto contacts=world.takeShadowBladeContacts(actor);
        const auto stop=std::find_if(contacts.begin(),contacts.end(),[](const MeleeBladeContact& c) {
          return c.world && c.stopsSwing;
          });
        require(stop!=contacts.end(),"an intact fence must stop the blow");
        return *stop;
        };
      const auto health=[&]() { return world.movableVobSnapshot()[0].health; };
      std::vector<uintptr_t> struck;
      const auto first=blow(1,0);
      require(first.movableVob==fenceId,"a blow stopped by a fence must carry the struck movable vob");
      require(world.meleeStoppedMovableVob(first,40,100,FractureRule::Melee::OneHand,nullptr,struck) &&
              struck.size()==1 && struck[0]==fenceId,
              "a stopped blow must strike the destructible fence it stopped at");
      requireNear(health(),30.f,.001f,"a stopped blow must use the hit-frame damage rule");
      require(!world.meleeStoppedMovableVob(first,40,100,FractureRule::Melee::OneHand,nullptr,struck),
              "one swing must strike a stopping fence only once");
      require(world.meleeMovableVobs({0,50,0},{1,0,0},150,40,100,FractureRule::Melee::OneHand,nullptr,&struck)==0,
              "the hit frame of a swing that already struck a fence must not strike it again");
      requireNear(health(),30.f,.001f,"a repeated strike in one swing must leave health untouched");
      MeleeBladeContact wall=first;
      wall.movableVob=0;
      std::vector<uintptr_t> none;
      require(!world.meleeStoppedMovableVob(wall,40,100,FractureRule::Melee::OneHand,nullptr,none) && none.empty(),
              "a blow stopped by a plain wall must deliver no strike");
      std::vector<uintptr_t> next;
      require(world.meleeStoppedMovableVob(blow(2,10),40,100,FractureRule::Melee::OneHand,nullptr,next),
              "the next swing must strike the fence again");
      world.tick(17);
      require(fence.isBroken() && world.fragmentSnapshot().size()==8,
              "two stopped one-handed blows must break the fence");
      require(!world.meleeStoppedMovableVob(first,40,100,FractureRule::Melee::OneHand,nullptr,none),
              "a broken fence must not take stopped blows");
      // Disabling destruction restores the intact fence, which must still ignore stopped blows.
      world.setDestructionEnabled(false);
      require(!fence.isBroken() && !world.meleeStoppedMovableVob(blow(3,0),40,100,FractureRule::Melee::OneHand,nullptr,none) &&
              none.empty() && world.movableVobSnapshot()[0].health>99.f,
              "destruction=0 must ignore stopped blows");
      if(movable) {
        DynamicWorld plain(nullptr,movableFloor());
        plain.setMovableVobsEnabled(true);
        // No fracture table entry for this visual: a large crate that cannot break.
        auto crate=plain.addMovableVob(proto,"CONTRACT_PLAIN_CRATE.MRM",at(100,50,0),.04f,true,"crate",true,zenId+1);
        require(!crate.isEmpty() && !plain.movableVobSnapshot()[0].destructible,
                "the plain crate fixture must be a non-destructible movable vob");
        plain.updateShadowBlade(actor,1,{{{-10,30,0},{-10,70,0},{0,0,1},2}},0,0,100);
        plain.flushShadowBlades();
        plain.updateShadowBlade(actor,1,{{{200,30,0},{200,70,0},{0,0,1},2}},100,0,100);
        plain.flushShadowBlades();
        const auto contacts=plain.takeShadowBladeContacts(actor);
        const auto stop=std::find_if(contacts.begin(),contacts.end(),[](const MeleeBladeContact& c) {
          return c.world && c.stopsSwing;
          });
        require(stop!=contacts.end() && stop->movableVob!=0,"a large movable vob must still stop the blow");
        std::vector<uintptr_t> plainStruck;
        require(!plain.meleeStoppedMovableVob(*stop,40,100,FractureRule::Melee::OneHand,nullptr,plainStruck) &&
                plainStruck.empty(),"a non-destructible movable vob must stop the blow without a strike");
        }
      std::printf("Stopped blow fence movable %d: health 100 -> 30 -> broken\n",int(movable));
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
          // The 50 cm pieces are debris a character stops at and pushes (from 0.5 m, the bonds
          // note's second step): the world steps while the walker pushes them aside.
          walker.setPosition({0,0,0});
          for(int i=0;i<80;++i) {
            walker.moveVirtual({5,0,0},17,50,.785398163f,80);
            world.tick(17);
            }
          require(walker.position().x>200,
                  "native walking must cross the original hull, pushing its debris, after destruction, cleanup and fresh load");
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

  void testOwnFractureMotion() {
    const auto root=std::filesystem::temp_directory_path()/
                    ("gothic-fracture-motion-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root/"meshes");
    const auto* oldContentValue=std::getenv("OPENGOTHIC_CONTENT");
    const bool hadContent=oldContentValue!=nullptr;
    const std::string oldContent=hadContent ? oldContentValue : "";
    const auto* oldMotionValue=std::getenv("OPENGOTHIC_FRACTURE_MOTION");
    const bool hadMotion=oldMotionValue!=nullptr;
    const std::string oldMotion=hadMotion ? oldMotionValue : "";
    setenv("OPENGOTHIC_CONTENT",root.c_str(),1);
    // Direction and falloff are checked with the earlier set, where the hit outweighs separation.
    setenv("OPENGOTHIC_FRACTURE_MOTION","hit=250,falloff=45,outward=60,spin=4,cap=450",1);
    constexpr const char* visual="NW_NATURE_FENCE_01.MRM";
    constexpr uint32_t zenId=6403;
    auto parent=movableCollisionBox({100,100,100});
    parent.sub_meshes[0].mat.name="WOOD";
    writeBaseMesh(parent,"NW_NATURE_FENCE_01",root/"meshes/model.glb");
    std::ofstream(root/"meshes/meshes.json")<<R"({"NW_NATURE_FENCE_01":{"glb":"model.glb"}})";
    {
      Resources resources;
      bool replaced=false;
      const auto loaded=Resources::loadVisualMesh(visual,&replaced);
      Resources::MeshReplacementSource source;
      require(replaced && Resources::meshReplacementLoaded(visual,&source),
              "motion fixture must bind its sidecar to a successfully loaded own GLB");
      ProtoMesh proto(PackedMesh(loaded,PackedMesh::PK_Visual,true),visual);
      const auto write=[&](size_t version,size_t count) {
        std::ofstream file(root/"meshes/model.fracture");
        file<<"GOTHIC_FRACTURE "<<version<<"\nsource "<<source.size<<' '<<std::hex<<source.crc32<<std::dec
            <<"\npieces "<<count<<'\n';
        for(size_t i=0;i<count;++i) {
          const Tempest::Vec3 center=version==3 ? Tempest::Vec3(-42.f+12.f*float(i%8),-24.f+12.f*float(i/8),0) :
                                               Tempest::Vec3(i&1 ? 25.f : -25.f,i&2 ? 25.f : -25.f,i&4 ? 25.f : -25.f);
          auto piece=movableBox(version==3 ? Tempest::Vec3(10,10,10) : Tempest::Vec3(50,50,50));
          for(auto& p:piece.positions) { p.x+=center.x; p.y+=center.y; p.z+=center.z; }
          file<<"piece "<<(version==3 ? 1000 : 125000)<<' '<<center.x<<' '<<center.y<<' '<<center.z<<' ';
          if(version==3) {
            file<<(i%2==0 ? "WOOD" : "METAL")<<" 1 12\nhull 8\n";
            for(const auto& p:piece.positions) file<<p.x<<' '<<p.y<<' '<<p.z<<'\n';
            }
          else file<<"12\n";
          for(const auto& triangle:piece.sub_meshes[0].triangles) {
            file<<"tri WOOD.TGA\n";
            for(const auto index:triangle.wedges) {
              const auto& p=piece.positions[index];
              file<<p.x<<' '<<p.y<<' '<<p.z<<" 0 1 0 0 0\n";
              }
            }
          }
        require(bool(file),"motion fixture must write the complete sidecar");
        };
      const auto birth=at(0,1000,0);
      const auto make=[&](DynamicWorld& world) {
        // A static fence isolates the added fracture velocity from transferred
        // parent momentum, damping and integration.
        return world.addMovableVob(proto,visual,birth,.04f,true,"fence",true,zenId);
        };
      const auto strike=[&](DynamicWorld& target) {
        MeleeBladeContact contact {};
        contact.world=contact.stopsSwing=true;
        contact.movableVob=target.movableVobSnapshot()[0].id;
        contact.point={-50,1000,0};
        contact.velocityCmS={250,0,0};
        std::vector<uintptr_t> struck;
        return target.meleeStoppedMovableVob(contact,40,1000,FractureRule::Melee::OneHand,nullptr,struck);
        };
      write(3,40);
      DynamicWorld world(nullptr,asset::WorldMesh{});
      auto object=make(world);
      require(object.fractureMesh()!=nullptr && object.fractureMesh()->pieces.size()==40,
              "version 3 physics must preload all forty authored pieces");
      require(strike(world),"the own fence must break through the stopped blade's directed melee path");
      world.tick(0);
      const auto pieces=world.fragmentSnapshot();
      require(object.isBroken() && pieces.size()==40,"version 3 must replace the parent with forty native fragments");
      float mean=0;
      const auto water=world.waterBodySnapshot();
      for(const auto& piece:pieces) {
        mean+=piece.velocity.x/40.f;
        requireNear(piece.friction,piece.piece%2==0 ? .4f : 1.1f,.000001f,
                    "fragment friction must use the authored wood or metal group");
        requireNear(waterBody(water,zenId,piece.piece).targetFraction,piece.piece%2==0 ? .6f : 7.8f,.000001f,
                    "fragment buoyancy must use the authored wood or metal group");
        require(piece.velocity.length()<=450.001f && piece.angularVelocity.length()<=4.00001f,
                "the added motion and deterministic spin must respect their limits");
        }
      require(mean>0 && pieces[16].velocity.x>pieces[23].velocity.x,
              "the directed mean must be positive and the near central piece faster than the far central piece");
      const auto saved=saveArchive([&](Serialize& output) { world.saveMovableVobs(output); });
      DynamicWorld restored(nullptr,asset::WorldMesh{});
      auto restoredObject=make(restored);
      loadMovables(restored,saved);
      const auto restoredPieces=restored.fragmentSnapshot();
      require(restoredObject.isBroken() && restoredPieces.size()==40,
              "save and load must restore all forty version 3 fragments");
      const auto restoredWater=restored.waterBodySnapshot();
      for(size_t i=0;i<pieces.size();++i) {
        requireMatrix(restoredPieces[i].transform,pieces[i].transform,"forty-piece saves must restore each pose");
        requireNear((restoredPieces[i].velocity-pieces[i].velocity).length(),0,.001f,
                    "forty-piece saves must restore each velocity");
        requireNear((restoredPieces[i].angularVelocity-pieces[i].angularVelocity).length(),0,.001f,
                    "forty-piece saves must restore each spin");
        requireNear(restoredPieces[i].friction,i%2==0 ? .4f : 1.1f,.000001f,
                    "restored fragments must retain per-piece friction");
        requireNear(waterBody(restoredWater,zenId,i).targetFraction,i%2==0 ? .6f : 7.8f,.000001f,
                    "restored fragments must retain per-piece buoyancy");
        }
      write(3,39);
      DynamicWorld mismatched(nullptr,asset::WorldMesh{});
      auto mismatchedObject=make(mismatched);
      loadMovables(mismatched,saved);
      require(!mismatchedObject.isBroken() && mismatched.fragmentSnapshot().empty(),
              "a changed sidecar piece count must skip saved debris without breaking the intact object");

      write(3,40);
      unsetenv("OPENGOTHIC_FRACTURE_MOTION");
      DynamicWorld tuned(nullptr,asset::WorldMesh{});
      auto tunedObject=make(tuned);
      require(strike(tuned),"the default motion fixture must break through the stopped blade");
      tuned.tick(0);
      const auto defaults=tuned.fragmentSnapshot();
      require(tunedObject.isBroken() && defaults.size()==pieces.size(),"default motion must preserve every fragment");
      for(const auto& piece:defaults)
        require(piece.velocity.length()<=200.001f && piece.angularVelocity.length()<=3.00001f,
                "the tuned default motion must respect its 200 cm/s cap and 3 rad/s spin");
      // The far central piece, 92 cm from the blow along it: outward 120 plus hit 150*exp(-92/60).
      requireNear(defaults[23].velocity.x,120.f+150.f*std::exp(-92.f/60.f),.5f,
                  "hit=150,falloff=60,outward=120 must be the defaults without the variable");
      for(const char* config:{"hit=150, falloff = 60,outward=120 ,spin=3, cap=200","hit=abc"}) {
        setenv("OPENGOTHIC_FRACTURE_MOTION",config,1);
        std::vector<std::string> errors;
        Tempest::Log::setOutputCallback([&](Tempest::Log::Mode mode,const char* line) {
          if(mode==Tempest::Log::Error && std::string_view(line).find("OPENGOTHIC_FRACTURE_MOTION")!=std::string_view::npos)
            errors.emplace_back(line);
          });
        try {
          DynamicWorld configured(nullptr,asset::WorldMesh{});
          auto configuredObject=make(configured);
          setenv("OPENGOTHIC_FRACTURE_MOTION","cap=0",1);
          require(strike(configured),"whitespace or an invalid motion override must not prevent destruction");
          configured.tick(0);
          const auto configuredPieces=configured.fragmentSnapshot();
          require(configuredObject.isBroken() && configuredPieces.size()==pieces.size(),
                  "whitespace and invalid motion overrides must preserve every fragment");
          for(size_t i=0;i<pieces.size();++i) {
            requireNear((configuredPieces[i].velocity-defaults[i].velocity).length(),0,.0001f,
                        "trimmed defaults and invalid overrides must retain the full default linear motion");
            requireNear((configuredPieces[i].angularVelocity-defaults[i].angularVelocity).length(),0,.0001f,
                        "trimmed defaults and invalid overrides must retain the full default spin");
            }
          }
        catch(...) { Tempest::Log::setOutputCallback({}); throw; }
        Tempest::Log::setOutputCallback({});
        require(errors.size()==(std::string_view(config)=="hit=abc" ? 1u : 0u),
                "invalid motion overrides must log one error per world; whitespace must be accepted silently");
        if(!errors.empty()) {
          constexpr std::string_view prefix="Invalid OPENGOTHIC_FRACTURE_MOTION entry 'hit=abc': ";
          const auto fallback=errors[0].find("; using defaults");
          require(errors[0].starts_with(prefix) && fallback!=std::string::npos && fallback>prefix.size(),
                  "an invalid motion override must identify its entry and explain why defaults were selected");
          }
        }
      setenv("OPENGOTHIC_FRACTURE_MOTION","hit=500,falloff=90,outward=0,spin=0,cap=50",1);
      DynamicWorld capped(nullptr,asset::WorldMesh{});
      auto cappedObject=make(capped);
      require(strike(capped),"the environment override fixture must receive a directed melee hit");
      capped.tick(0);
      require(cappedObject.isBroken(),"motion overrides must preserve destruction");
      for(const auto& piece:capped.fragmentSnapshot()) {
        requireNear(piece.velocity.x,50.f,.0001f,"motion overrides must cap the added hit velocity");
        requireNear(piece.velocity.y,0.f,.0001f,"outward=0 must suppress vertical separation");
        requireNear(piece.angularVelocity.length(),0.f,.0001f,"spin=0 must suppress added spin");
        }

      write(2,8);
      DynamicWorld legacy(nullptr,asset::WorldMesh{});
      auto legacyObject=make(legacy);
      require(legacy.meleeMovableVobs({-100,1000,0},{1,0,0},150,40,1000,FractureRule::Melee::OneHand)==1,
              "version 2 must retain the original melee destruction path");
      legacy.tick(0);
      const auto legacyPieces=legacy.fragmentSnapshot();
      require(legacyObject.isBroken() && legacyPieces.size()==8,"version 2 must retain its original eight pieces");
      for(const auto& piece:legacyPieces) {
        const auto expected=Tempest::Vec3::normalize(legacyObject.fractureMesh()->pieces[piece.piece].center)*40.f;
        requireNear((piece.velocity-expected).length(),0,.0001f,
                    "version 2 must retain exactly forty centimetres per second outward despite motion overrides");
        requireNear(piece.angularVelocity.length(),0.f,.0001f,"version 2 must not receive added spin");
        }
      // Hoops and staves: METAL and WOOD of one format 3 break never touch, so a
      // hoop falls through its wood to the ground; equal materials and v2 stack.
      const auto writeStack=[&](size_t version) {
        struct Slab { Tempest::Vec3 size; float x, y; const char* group; };
        // Version 2 needs eight pieces: four loose blocks lie apart from the stack.
        const Slab slabs[]={{{40,4,40},0,-28,"WOOD"},{{20,2,20},0,-25,"METAL"},{{20,2,20},0,-23,"METAL"},{{10,2,10},0,-21,"WOOD"},
                            {{10,4,10},80,-28,"WOOD"},{{10,4,10},100,-28,"WOOD"},{{10,4,10},120,-28,"WOOD"},{{10,4,10},140,-28,"WOOD"}};
        const size_t count=version==3 ? 4 : 8;
        std::ofstream file(root/"meshes/model.fracture");
        file<<"GOTHIC_FRACTURE "<<version<<"\nsource "<<source.size<<' '<<std::hex<<source.crc32<<std::dec
            <<"\npieces "<<count<<'\n';
        for(size_t k=0;k<count;++k) {
          const auto& slab=slabs[k];
          auto piece=movableBox(slab.size);
          for(auto& p:piece.positions) { p.x+=slab.x; p.y+=slab.y; }
          file<<"piece "<<slab.size.x*slab.size.y*slab.size.z<<' '<<slab.x<<' '<<slab.y<<" 0 ";
          if(version==3) {
            file<<slab.group<<" 1 12\nhull 8\n";
            for(const auto& p:piece.positions) file<<p.x<<' '<<p.y<<' '<<p.z<<'\n';
            }
          else file<<"12\n";
          for(const auto& triangle:piece.sub_meshes[0].triangles) {
            file<<"tri WOOD.TGA\n";
            for(const auto index:triangle.wedges) {
              const auto& p=piece.positions[index];
              file<<p.x<<' '<<p.y<<' '<<p.z<<" 0 1 0 0 0\n";
              }
            }
          }
        require(bool(file),"the hoop fixture must write the complete sidecar");
        };
      setenv("OPENGOTHIC_FRACTURE_MOTION","hit=0,outward=0,spin=0",1);
      float stacked[2][4]={};
      for(const size_t version:{size_t(3),size_t(2)}) {
        writeStack(version);
        DynamicWorld ground(nullptr,movableFloor());
        auto stackObject=ground.addMovableVob(proto,visual,at(0,30,0),.04f,true,"fence",true,zenId);
        if(version==3) {
          MeleeBladeContact contact {};
          contact.world=contact.stopsSwing=true;
          contact.movableVob=ground.movableVobSnapshot()[0].id;
          contact.point={-50,30,0};
          contact.velocityCmS={250,0,0};
          std::vector<uintptr_t> struck;
          require(ground.meleeStoppedMovableVob(contact,40,1000,FractureRule::Melee::OneHand,nullptr,struck),
                  "the hoop fixture must break through the stopped blade");
          }
        else require(ground.meleeMovableVobs({-100,30,0},{1,0,0},150,40,1000,FractureRule::Melee::OneHand)==1,
                     "the version 2 stack must break through the original melee path");
        tickWater(ground,300);
        const auto settled=ground.fragmentSnapshot();
        require(stackObject.isBroken() && settled.size()==(version==3 ? 4u : 8u),"the hoop fixture must keep every piece");
        for(const auto& piece:settled) {
          if(piece.piece>=4) continue;
          Tempest::Vec3 center(0,piece.piece==0 ? -28.f : piece.piece==1 ? -25.f : piece.piece==2 ? -23.f : -21.f,0);
          piece.transform.project(center);
          stacked[version==3 ? 0 : 1][piece.piece]=center.y;
          }
        }
      // Centres over the floor at y=0: the wood slab 0..4, hoop 1 on the ground, hoop 2 on hoop 1,
      // the small wood slab through both hoops onto the large one. Version 2 keeps the authored stack.
      const float expected[2][4]={{2,1,3,5},{2,5,7,9}};
      const float slop=CollisionWorld::fragmentPenetrationSlop*100.f+.1f;
      for(int version=0;version<2;++version)
        for(int piece=0;piece<4;++piece)
          requireNear(stacked[version][piece],expected[version][piece],slop,
                      version==0 ? "format 3 METAL and WOOD of one break must not collide, each with the ground and its own material must" :
                                   "version 2 pieces of one break must keep colliding with each other");
      // Another break of the same mesh, or a plain body, still meets the hoop.
      FractureMesh hoopMesh;
      hoopMesh.pieces.resize(2);
      hoopMesh.pieces[0].group=asset::MaterialGroup::METAL;
      hoopMesh.pieces[1].group=asset::MaterialGroup::WOOD;
      const auto filter=fractureGroupFilter(hoopMesh);
      const JPH::CollisionGroup hoop(filter,0x80000000u,0);
      require(!hoop.CanCollide(JPH::CollisionGroup(filter,0x80000000u,1)) &&
              hoop.CanCollide(JPH::CollisionGroup(filter,0x80000001u,1)) && hoop.CanCollide(JPH::CollisionGroup()),
              "the hoop filter must separate only METAL and WOOD of the same break");
      std::printf("Own fracture v3: forty pieces, WOOD 0.4 / METAL 1.1 friction; hit mean %.6f, near %.6f > far %.6f cm/s; "
                  "save/load and changed-count skip; whitespace and invalid override fallback; override cap 50; v2 outward 40 cm/s; "
                  "hoops v3 %.2f/%.2f/%.2f/%.2f, v2 %.2f/%.2f/%.2f/%.2f cm passed\n",
                  double(mean),double(pieces[16].velocity.x),double(pieces[23].velocity.x),
                  double(stacked[0][0]),double(stacked[0][1]),double(stacked[0][2]),double(stacked[0][3]),
                  double(stacked[1][0]),double(stacked[1][1]),double(stacked[1][2]),double(stacked[1][3]));
      }
    if(hadContent) setenv("OPENGOTHIC_CONTENT",oldContent.c_str(),1); else unsetenv("OPENGOTHIC_CONTENT");
    if(hadMotion) setenv("OPENGOTHIC_FRACTURE_MOTION",oldMotion.c_str(),1); else unsetenv("OPENGOTHIC_FRACTURE_MOTION");
    std::filesystem::remove_all(root);
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

    // Fine slop: a 4 cm board born 1.5 cm deep in the floor is pushed out to 0.5 cm as a fragment;
    // as an item it stays within Jolt's 2 cm slop and is not pushed at all.
    CollisionWorld flat;
    JPH::TriangleList ground;
    ground.emplace_back(JPH::Float3(-5,0,-5),JPH::Float3(-5,0,5),JPH::Float3(5,0,-5));
    ground.emplace_back(JPH::Float3(5,0,-5),JPH::Float3(-5,0,5),JPH::Float3(5,0,5));
    const auto groundShape=JPH::MeshShapeSettings(ground).Create();
    require(!groundShape.HasError(),"the fine slop fixture needs a one-sided floor");
    auto groundBody=flat.addCollisionBody(groundShape.Get(),at(0,0,0),.8f,DynamicWorld::C_Landscape);
    const auto board=JPH::BoxShapeSettings(JPH::Vec3(.5f,.02f,.1f),.001f).Create();
    require(!board.HasError(),"the fine slop fixture needs a thin board");
    auto fragment=flat.addDynamicBody(board.Get(),at(-150,.5f,0),.8f,8.f,DynamicWorld::C_Fragment);
    auto item=flat.addDynamicBody(board.Get(),at(150,.5f,0),.8f,8.f,DynamicWorld::C_Item);
    for(int i=0;i<120;++i) flat.tick(16);
    float depth[2]={};
    for(int k=0;k<2;++k)
      for(const auto& contact:flat.overlaps(k==0 ? *fragment : *item,0.f,1u<<DynamicWorld::C_Landscape))
        depth[k]=std::max(depth[k],contact.depth);
    std::printf("Fine slop: board born 1.5 cm deep, after 2 s fragment %.3f cm, item %.3f cm\n",double(depth[0]),double(depth[1]));
    require(fragment->position().y>0.f && depth[0]<=CollisionWorld::fragmentPenetrationSlop*100.f+.05f,
            "a fragment must be pushed out to its 0.5 cm slop");
    require(item->position().y>0.f && depth[1]>1.f && depth[1]<2.f,
            "an item must keep Jolt's 2 cm slop: only C_Fragment uses the fine slop");
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
  }
