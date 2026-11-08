#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#include "physics_contract_common.h"
#include "physics/collisionworld.h"
#include "physics/physicvbo.h"
#include "graphics/mesh/protomesh.h"
#include "graphics/bounds.h"
#include "graphics/mesh/submesh/packedmesh.h"
#include "game/serialize.h"

#include <Tempest/Log>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <iterator>
#include <limits>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

namespace PhysicsContract {
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

  void testItemWeaponWake() {
    Resources resources;
    ProtoMesh appleMesh(PackedMesh(movableBox({8,8,8}),PackedMesh::PK_Visual),"CONTRACT_WAKE_APPLE.MRM");
    Bounds appleBounds;
    appleBounds.assign(appleMesh.bbox());
    for(const bool enabled:{false,true}) {
      DynamicWorld world(nullptr,asset::WorldMesh{});
      world.setItemWakeEnabled(enabled);
      const auto rest=at(0,100,0);
      auto apple=world.addDynamicObj(rest,appleBounds,asset::MaterialGroup::WOOD,
                                     &appleMesh,"CONTRACT_WAKE_APPLE.MRM");
      apple.restorePhysics(rest,true);
      struct ArrowContact final : DynamicWorld::BulletCallback {
        bool stopsAtWorldHit() const override { return true; }
        void onWorldHit(const DynamicWorld::BulletWorldHit&) override { ++attachments; }
        void onCollide(asset::MaterialGroup) override { ++collisions; }
        void onStop() override { ++stops; }
        int attachments=0,collisions=0,stops=0;
        } contact;
      auto* arrow=world.bulletObj(&contact);
      arrow->setPosition({-40,100,0});
      arrow->setDirection({3,0,0});
      world.tick(17);
      const float transferred=apple.velocity().x*.01f*apple.mass();
      std::printf("Item arrow wake enabled %d: momentum %.6f Ns, displacement %.6f cm\n",
                  int(enabled),double(transferred),double((apple.position()-Tempest::Vec3(0,100,0)).length()));
      if(enabled) {
        require(!apple.isSleeping() && apple.position().x>1.f,
                "an arrow must wake and move a sleeping apple");
        requireNear(transferred,3.6f,.04f,"an Item must receive the furniture arrow momentum at actual flight speed");
        require(contact.collisions==1 && contact.stops==1 && contact.attachments==0,
                "an arrow stopped by a moving Item must not leave an attachment pinned to the world");
        }
      else require(apple.isSleeping() && apple.position().x==0.f,
                   "item_wake OFF must keep sleeping Items outside the arrow collision ray");
      world.deleteObj(arrow);
      }

    for(const bool fist:{false,true})for(const bool enabled:{false,true}) {
      CollisionWorld world;
      world.setItemWakeEnabled(enabled);
      std::vector<std::unique_ptr<CollisionWorld::DynamicBody>> items(3);
      const Tempest::Vec3 rest[]{ {-40,115,0},{0,115,0},{40,115,0} };
      world.setItemWakeCallback([&](const Tempest::Vec3& min,const Tempest::Vec3& max,size_t budget) {
        size_t created=0;
        for(size_t i=0;i<items.size();++i) {
          const auto& p=rest[i];
          if(p.x<min.x || p.x>max.x || p.y<min.y || p.y>max.y || p.z<min.z || p.z>max.z)continue;
          if(items[i]==nullptr && created<budget) {
            items[i]=world.addDynamicBody(new JPH::BoxShape({.04f,.04f,.04f}),at(p.x,p.y,p.z),.7f,.2f,
                                           DynamicWorld::C_Item,true);
            ++created;
            }
          if(items[i])items[i]->retainForItemWake();
          }
        return created;
        });
      int actorId=0;
      auto* actor=reinterpret_cast<Npc*>(&actorId);
      const MeleeBladeSegment before{{-100,115,0},{-100,fist?115.f:140.f,0},{0,0,1},fist?6.f:2.f};
      const MeleeBladeSegment after{{100,115,0},{100,fist?115.f:140.f,0},{0,0,1},fist?6.f:2.f};
      const auto weapon=fist?MeleeImpact::Weapon::Fist:MeleeImpact::Weapon::OneHand;
      world.updateShadowBlade(actor,1,{before},0,0,100,.7f,{},weapon);
      world.updateShadowBlade(actor,1,{after},100,0,100,.7f,{},weapon);
      world.flushShadowBlades();
      size_t awake=0;
      for(const auto& item:items)awake+=item!=nullptr && !item->isSleeping() && item->velocity().x>50.f;
      require(awake==(enabled?3u:0u),
              "only item_wake ON must create and push all sleeping Items swept by a blade or fist");
      require(world.takeShadowBladeContacts(actor).empty(),
              "small Item hits must not alter the melee damage journal or stop the swing");
      world.tick(17);
      size_t moved=0;
      for(size_t i=0;i<items.size();++i)moved+=items[i] && items[i]->position().x-rest[i].x>1.f;
      std::printf("Item melee wake fist %d enabled %d: awake %zu/3, moved %zu/3\n",int(fist),int(enabled),awake,moved);
      require(moved==(enabled?3u:0u),"a shadow contact must physically displace the sleeping Items");
      }
    {
      CollisionWorld world;
      JPH::VertexList vertices={{.15f,0,-1},{.15f,0,1},{.15f,2,1},{.15f,2,-1}};
      JPH::IndexedTriangleList triangles={{0,1,2},{0,2,3}};
      const auto face=JPH::MeshShapeSettings(vertices,triangles).Create();
      require(!face.HasError(),"the newly woken Item requires a thin sensor face");
      auto wall=world.addCollisionBody(face.Get(),at(0,0,0),.7f,DynamicWorld::C_Object,false,true);
      auto item=world.addDynamicBody(new JPH::BoxShape({.04f,.04f,.04f}),at(0,100,0),.7f,.2f,DynamicWorld::C_Item,true);
      int actorId=0;
      auto* actor=reinterpret_cast<Npc*>(&actorId);
      world.updateShadowBlade(actor,1,{{{-10,100,0},{-10,125,0},{0,0,1},2}},0,0,100);
      world.updateShadowBlade(actor,1,{{{10,100,0},{10,125,0},{0,0,1},2}},100,0,100);
      world.tick(17);
      require(world.rayCount()==1,"the sleeping Item must actually move during the blade's native step");
      require(item->isSleeping(),"an Item woken by a blade during Update must recover a crossing in that same step");
      requireNear(item->position().x,0.f,.001f,"a newly woken Item must return to its previous resting position");
      }
    {
      CollisionWorld world;
      std::vector<std::unique_ptr<CollisionWorld::DynamicBody>> sources;
      for(int i=0;i<40;++i)
        sources.push_back(world.addDynamicBody(new JPH::BoxShape({.1f,.1f,.1f}),at(1000.f*i,1000,0),.7f,1));
      size_t queries=0;
      world.setItemWakeCallback([&](const Tempest::Vec3&,const Tempest::Vec3&,size_t) {++queries;return size_t(0);});
      world.wakeItemsInBounds({-1,-1,-1},{1,1,1});
      world.tick(17);
      require(queries==32,"projectile and active-body wakes must share thirty-two queries before the native step");
      world.tick(17);
      require(queries==64,"the next native step must renew the shared wake budget exactly once");
      size_t created=0;
      world.setItemWakeCallback([&](const Tempest::Vec3&,const Tempest::Vec3&,size_t budget) {
        created+=budget;return budget;
        });
      world.wakeItemsInBounds({-1,-1,-1},{1,1,1});
      world.tick(17);
      require(created==8,"projectile and active-body wakes must share eight new native bodies per step");
      }
    {
      CollisionWorld world;
      size_t queries=0;
      world.setItemWakeCallback([&](const Tempest::Vec3&,const Tempest::Vec3&,size_t) {++queries;return size_t(0);});
      for(size_t i=0;i<33;++i)
        world.wakeItemsInBounds({-1,-1,-1},{1,1,1});
      require(queries==32,"an empty world must retain the shared limit before its tick");
      world.tick(17);
      world.wakeItemsInBounds({-1,-1,-1},{1,1,1});
      require(queries==33,"a bodyless world must renew projectile wake queries after its tick");
      size_t created=0;
      world.setItemWakeCallback([&](const Tempest::Vec3&,const Tempest::Vec3&,size_t budget) {
        created+=budget;return budget;
        });
      world.wakeItemsInBounds({-1,-1,-1},{1,1,1});
      world.wakeItemsInBounds({-1,-1,-1},{1,1,1});
      require(created==8,"an empty world must retain the shared creation limit before its tick");
      world.tick(17);
      world.wakeItemsInBounds({-1,-1,-1},{1,1,1});
      require(created==16,"a bodyless world must renew the shared creation budget after its tick");
      }
    {
      CollisionWorld world;
      auto source=world.addDynamicBody(new JPH::BoxShape({.1f,.1f,.1f}),at(10000,1000,0),.7f,1,
                                       DynamicWorld::C_Item,true);
      size_t queries=0;
      world.setItemWakeCallback([&](const Tempest::Vec3&,const Tempest::Vec3&,size_t) {++queries;return size_t(0);});
      int actorId=0;
      auto* actor=reinterpret_cast<Npc*>(&actorId);
      world.updateShadowBlade(actor,1,{{{-10,100,0},{-10,125,0},{0,0,1},2}},0,0,100);
      world.updateShadowBlade(actor,1,{{{10,100,0},{10,125,0},{0,0,1},2}},100,0,100);
      world.tick(17);
      require(world.simulationStep()==1,"the blade query fixture must run one native step");
      require(queries==1,"one active blade must consume only one wake query in the frame");
      }
    }

  void testItemDeepestContact() {
    require(Gothic::PhysicsFlags{}.itemDeepestContact,"item deepest contact must default to ON");
    require(Gothic::PhysicsFlags{}.ragdollBirthGuard,"ragdoll birth guard must default to ON");
    // Inflated body-frame hull vertices (metres), poses and original volumes
    // from Claude's thin-blade dumps. No game assets or hull inflation run here.
    const JPH::Vec3 canePoints[] = {
      {0.0000100f,-0.0394365f,-0.0001233f},{0.0953004f,-0.0176432f,-0.0763430f},{0.1580164f,-0.0275274f,0.0000100f},
      {0.0000100f,0.0386679f,-0.0001233f},{0.1580164f,0.0275474f,0.0000100f},{0.0953004f,0.0172531f,-0.0763430f},
      {-0.9935228f,0.0117074f,0.0000100f},{-1.0002762f,0.0067635f,-0.0067435f},{-1.0002762f,0.0067635f,0.0067635f},
      {-1.0052202f,0.0000100f,0.0000100f},{-0.9935228f,0.0000100f,0.0117074f},{-1.0002762f,-0.0067435f,0.0067635f},
      {-0.9935228f,-0.0116874f,0.0000100f},{-1.0002762f,-0.0067435f,-0.0067435f},{-0.9935228f,0.0000100f,-0.0116874f},
      {0.1853235f,0.0067635f,-0.0067435f},{0.1902674f,0.0000100f,0.0000100f},{0.1853235f,-0.0067435f,-0.0067435f},
      {0.1853235f,0.0067635f,0.0067635f},{0.1853235f,-0.0067435f,0.0067635f},{0.1647699f,0.0067635f,0.0326835f},
      {0.1647699f,-0.0067435f,0.0326835f},{0.1580164f,0.0000100f,0.0376274f},{0.0000100f,-0.0314245f,0.0281609f},
      {-0.0315445f,-0.0160990f,0.0629631f},{0.0000100f,0.0306559f,0.0281610f},{-0.0315445f,0.0161190f,0.0629631f},
      {-0.0247910f,0.0111751f,0.0697166f},{0.1647698f,0.0226035f,0.0067635f},{-0.0247910f,-0.0111551f,0.0697166f},
      {0.1647698f,-0.0225835f,0.0067635f},{0.1392130f,-0.0110891f,-0.0620342f},{0.1407028f,-0.0069485f,-0.0636713f},
      {0.1020538f,-0.0126993f,-0.0830965f},{0.1647698f,0.0226035f,-0.0067435f},{0.1392130f,0.0106990f,-0.0620342f},
      {0.1647698f,-0.0225835f,-0.0067435f},{0.0525214f,-0.0176432f,-0.0780567f},{0.0525214f,0.0172531f,-0.0780567f},
      {0.1020538f,0.0123092f,-0.0830965f},{0.1407028f,0.0065584f,-0.0636713f},{0.1339493f,-0.0001950f,-0.0686152f},
      {-0.9234520f,0.0000100f,0.0178850f},{-0.9302055f,0.0067635f,0.0129411f},{-0.9302055f,-0.0067435f,0.0129411f},
      {0.1027695f,-0.0069485f,-0.0856545f},{0.0960160f,-0.0001950f,-0.0905984f},{0.1027695f,0.0065584f,-0.0856545f},
      {-0.9234520f,0.0000100f,-0.0178650f},{0.0520127f,-0.0001950f,-0.0923611f},{0.0525214f,-0.0059458f,-0.0897541f},
      {0.0525214f,0.0055557f,-0.0897541f},{0.1324595f,-0.0160330f,-0.0552807f},{0.1324595f,0.0156429f,-0.0552807f},
      {-0.0315445f,0.0000100f,0.0778565f},{-0.0382980f,-0.0111551f,0.0697166f},{-0.0382980f,0.0111751f,0.0697166f},
      {0.0953004f,-0.0059458f,-0.0880404f},{0.0953004f,0.0055557f,-0.0880404f},{-0.9234520f,0.0117074f,0.0061876f},
      {-0.9234520f,-0.0116874f,0.0061876f},{-0.9302055f,0.0067635f,-0.0129211f},{0.0457679f,0.0123092f,-0.0848102f},
      {-0.9302055f,-0.0067435f,-0.0129211f},{0.0457679f,-0.0126993f,-0.0848102f},{-0.0247910f,0.0067635f,0.0729126f},
      {-0.0247910f,-0.0067435f,0.0729126f},{0.0000100f,-0.0314245f,-0.0284076f},{0.0592749f,-0.0126993f,-0.0848102f},
      {0.0592749f,0.0123092f,-0.0848102f},{0.0000100f,0.0306559f,-0.0284076f},{-0.9234520f,0.0117074f,-0.0061676f},
      {-0.9234520f,-0.0116874f,-0.0061676f},{-0.0382980f,-0.0067435f,0.0729126f},{-0.0382980f,0.0067635f,0.0729126f},
      };
    const JPH::Vec3 rapierPoints[] = {
      {-0.6599900f,-0.0099900f,-0.0049900f},{-0.6249900f,-0.0147319f,0.0200100f},{-0.6749900f,-0.0099900f,0.0200100f},
      {-0.0140601f,0.0000100f,-0.1101174f},{0.0773733f,0.0105005f,-0.0943249f},{0.0773733f,-0.0112228f,-0.0943249f},
      {0.1165810f,-0.0148450f,-0.0775646f},{0.1165810f,0.0141227f,-0.0775646f},{0.1410690f,0.0102463f,-0.0521023f},
      {0.1410690f,-0.0109685f,-0.0521023f},{-0.6749900f,0.0000100f,0.0300100f},{-0.6807635f,0.0057835f,0.0257835f},
      {-0.6807635f,-0.0057635f,0.0257835f},{-0.6849900f,0.0000100f,0.0200100f},{-0.0144463f,-0.0178263f,0.0845428f},
      {0.1193156f,-0.0414022f,-0.0320298f},{-0.0082866f,-0.0057635f,0.1059109f},{0.1425866f,-0.0257067f,-0.0216935f},
      {0.1391145f,-0.0057635f,0.0068040f},{0.0807833f,0.0536168f,-0.0419428f},{-0.0144463f,0.0178463f,0.0845428f},
      {0.1135421f,0.0449064f,-0.0378032f},{0.1368131f,0.0292109f,-0.0274670f},{0.1425866f,0.0249844f,-0.0332405f},
      {0.1193156f,0.0406799f,-0.0435767f},{0.1572153f,0.0000100f,-0.0119868f},{0.1529888f,0.0057835f,-0.0062133f},
      {0.1529888f,-0.0057635f,-0.0062133f},{0.1391145f,0.0057835f,0.0068040f},{-0.6807635f,0.0057835f,0.0142365f},
      {-0.6749900f,0.0100100f,0.0200100f},{-0.6807635f,-0.0057635f,0.0142365f},{-0.6657635f,-0.0057635f,-0.0107635f},
      {0.1425866f,0.0249844f,-0.0216935f},{-0.0082866f,0.0057835f,0.1059109f},{-0.6249900f,0.0000100f,-0.0299900f},
      {-0.6307635f,0.0057835f,-0.0257635f},{-0.0198336f,0.0057835f,-0.1058909f},{-0.6307635f,-0.0057635f,-0.0257635f},
      {-0.0198336f,-0.0057635f,-0.1058909f},{-0.6657635f,0.0057835f,-0.0107635f},{-0.6599900f,0.0100100f,-0.0049900f},
      {-0.6249900f,0.0100100f,-0.0199900f},{-0.6249900f,0.0147519f,0.0200100f},{0.1135421f,-0.0456287f,-0.0378033f},
      {0.0807833f,-0.0543391f,-0.0419428f},{0.0450249f,-0.0543391f,-0.0390658f},{0.0450249f,0.0536168f,-0.0390658f},
      {0.0428319f,0.0400725f,-0.0663219f},{-0.6249900f,-0.0099900f,-0.0199900f},{0.0428319f,-0.0407948f,-0.0663219f},
      {0.1175570f,0.0298183f,-0.0654344f},{0.1416106f,0.0189566f,-0.0453706f},{0.1193156f,-0.0414022f,-0.0435768f},
      {0.1175570f,-0.0305405f,-0.0654344f},{0.1416106f,-0.0196789f,-0.0453706f},{0.1425866f,-0.0257067f,-0.0332405f},
      {0.0831468f,0.0162740f,-0.0900984f},{0.0843638f,0.0358460f,-0.0749724f},{0.0843638f,-0.0365683f,-0.0749724f},
      {0.0831468f,-0.0169963f,-0.0900984f},{0.1108075f,-0.0090715f,-0.0817911f},{-0.6599900f,0.0000100f,-0.0149900f},
      {0.1108075f,0.0083492f,-0.0817911f},{0.1452955f,0.0044728f,-0.0463288f},{0.1452955f,-0.0051950f,-0.0463288f},
      {-0.0140601f,0.0100100f,0.1001374f},{-0.0140601f,-0.0099900f,0.1001374f},{-0.0140601f,0.0000100f,0.1101374f},
      {-0.0198336f,0.0057835f,0.1059109f},{-0.0198336f,-0.0057635f,0.1059109f},{0.0785903f,0.0400725f,-0.0691989f},
      {0.0785903f,-0.0407948f,-0.0691989f},{0.1193156f,0.0406799f,-0.0320297f},{0.1468131f,-0.0199332f,-0.0274670f},
      {0.1458371f,-0.0139054f,-0.0395971f},{0.1458371f,0.0131831f,-0.0395971f},{0.1468131f,0.0192109f,-0.0274670f},
      {-0.0082866f,0.0057835f,-0.1058909f},{-0.0082866f,-0.0057635f,-0.1058909f},{0.0728168f,-0.0365683f,-0.0749724f},
      {-0.6699900f,0.0000100f,-0.0049900f},{0.0728168f,0.0358460f,-0.0749724f},{0.0370584f,-0.0365683f,-0.0720954f},
      {0.0865568f,-0.0501126f,-0.0477163f},{0.0865568f,0.0493903f,-0.0477163f},{0.0370584f,0.0358460f,-0.0720954f},
      {-0.6307635f,-0.0105054f,0.0257835f},{-0.6307635f,0.0105254f,0.0257835f},{0.1333411f,0.0000100f,0.0110305f},
      {-0.0159459f,0.0319281f,0.0337302f},{0.1368131f,-0.0299332f,-0.0274670f},
      };

    const auto check=[](const char* name, const auto& points, JPH::Vec3Arg position, JPH::QuatArg rotation,
                       float volume, const JPH::VertexList& vertices, int supportLayer) {
      JPH::ConvexHullShapeSettings hullSettings(points,int(std::size(points)),.00025f);
      hullSettings.mHullTolerance=.0001f;
      hullSettings.mMaxErrorConvexRadius=.0001f;
      const auto hull=hullSettings.Create();
      require(!hull.HasError(),"thin-blade dump must create its native convex hull");
      const auto support=JPH::MeshShapeSettings(vertices,{{0,1,2}}).Create();
      require(!support.HasError(),"thin-blade fixture must create one horizontal support triangle");
      const auto transform=JPH::Mat44::sRotationTranslation(rotation,position);
      auto matrix=Tempest::Matrix4x4::mkIdentity();
      for(int c=0;c<4;++c)
        for(int r=0;r<3;++r)
          matrix.set(c,r,transform.GetColumn4(c)[r]*(c==3 ? 100.f : 1.f));
      const float planeY=vertices[0].y*100.f;
      for(const bool enabled:{false,true}) {
        CollisionWorld world;
        world.setItemDeepestContactEnabled(enabled);
        // OFF must expose the native crossing before the safety net resets it.
        // ON keeps the production recovery path and counts every reset.
        world.setItemWakeEnabled(enabled);
        auto floor=world.addCollisionBody(support.Get(),at(0,0,0),.5f,supportLayer);
        auto item=world.addDynamicBody(hull.Get(),matrix,.5f,7800.f*volume,DynamicWorld::C_Item);
        const auto start=item->position();
        float maxShift=0,minComAbove=item->centerOfMass().y-planeY;
        uint64_t crossed=0;
        size_t recovered=0;
        Tempest::Log::setOutputCallback([&](Tempest::Log::Mode,const char* line) {
          if(std::string_view(line).find("item recovered:")!=std::string_view::npos)
            ++recovered;
          });
        uint64_t sampled=0;
        while(world.simulationStep()<(enabled ? 300u : 20u)) {
          world.tick(16);
          if(sampled==world.simulationStep())continue;
          require(world.simulationStep()==sampled+1,"thin-blade fixture must sample every native 1/60 step");
          sampled=world.simulationStep();
          maxShift=std::max(maxShift,(item->position()-start).length());
          const float above=item->centerOfMass().y-planeY;
          minComAbove=std::min(minComAbove,above);
          if(above<0 && crossed==0)crossed=sampled;
          }
        Tempest::Log::setOutputCallback({});
        std::printf("Thin blade %s deepest %d: steps %llu, mass %.9g kg, max shift %.6f cm, "
                    "min COM above %.6f cm, crossing step %llu, recovered %zu\n",
                    name,int(enabled),static_cast<unsigned long long>(sampled),double(item->mass()),
                    double(maxShift),double(minComAbove),static_cast<unsigned long long>(crossed),recovered);
        if(enabled) {
          require(maxShift<5.f,"deepest contact must hold each blade within five centimetres for 300 steps");
          require(minComAbove>=0,"deepest contact must keep blade COM above the support on every step");
          require(recovered==0,"deepest contact must hold each blade without recovery");
          }
        else
          require(crossed>0 && crossed<=20,"upstream manifold must reproduce the blade crossing within twenty steps");
        }
      };
    check("OLDWORLD 1644",canePoints,{-126.0437775f,29.5590572f,-336.4428711f},
          {-.000000022f,.939692676f,-.000000008f,.342020154f},.00205267756f,
          {{-126.625645f,29.5296582f,-336.858516f},
           {-125.506016f,29.5296582f,-335.660703f},
           {-125.335527f,29.5296582f,-335.977812f}},DynamicWorld::C_Object);
    check("ADDONWORLD 6863",rapierPoints,{42.7198410f,-32.5431252f,258.7348328f},
          {0,0,0,1},.00332153379f,
          {{42.4582031f,-32.5876465f,257.429688f},
           {40.8759204f,-32.5876465f,260.700645f},
           {43.5833301f,-32.5876465f,259.642266f}},DynamicWorld::C_Landscape);
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
      const auto rays=world.rayCount();
      world.tick(17);
      require(queries==32,"a native step must issue no more than thirty-two item queries");
      require(world.rayCount()-rays==32,"tunneling must cast at most thirty-two center rays per native step");
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
    for(const bool enabled:{false,true}) {
      for(const int category:{DynamicWorld::C_Landscape,DynamicWorld::C_Object}) {
        CollisionWorld world;
        world.setItemWakeEnabled(enabled);
        JPH::VertexList vertices={{-2,0,-2},{-2,0,2},{2,0,2},{2,0,-2}};
        JPH::IndexedTriangleList triangles={{0,1,2},{0,2,3}};
        const auto mesh=JPH::MeshShapeSettings(vertices,triangles).Create();
        require(!mesh.HasError(),"tunneling requires a thin upward-facing mesh");
        // Disable the solver response to inject the missed contact that this
        // safety net must catch, while retaining the real triangle ray query.
        auto face=world.addCollisionBody(mesh.Get(),at(0,0,0),.7f,category,false,true);
        auto movingFace=world.addCollisionBody(mesh.Get(),at(0,5,0),.7f,DynamicWorld::C_Object,true,true);
        auto movingOverHole=world.addCollisionBody(mesh.Get(),at(300,5,0),.7f,DynamicWorld::C_Object,true,true);
        auto item=world.addDynamicBody(new JPH::BoxShape({.02f,.02f,.02f}),at(0,10,0),.7f,1);
        item->setVelocity({0,-2000,0},{});
        world.tick(17);
        if(enabled) {
          require(item->isSleeping(),"a thin-face crossing must sleep in the same native step with item_wake ON");
          requireNear(item->position().y,10.f,.001f,"a thin-face crossing must restore the last resting position");
          }
        else require(item->position().y<0.f,"item_wake OFF must not recover a thin-face crossing");
        auto hole=world.addDynamicBody(new JPH::BoxShape({.02f,.02f,.02f}),at(300,10,0),.7f,1);
        hole->setVelocity({0,-2000,0},{});
        auto cliff=world.addDynamicBody(new JPH::BoxShape({.02f,.02f,.02f}),at(190,10,100),.7f,1);
        cliff->setVelocity({3000,-2000,0},{});
        auto back=world.addDynamicBody(new JPH::BoxShape({.02f,.02f,.02f}),at(0,-10,100),.7f,1);
        back->setVelocity({0,2000,0},{});
        world.tick(17);
        require(hole->position().y<0.f && cliff->position().y<0.f && back->position().y>0.f &&
                !hole->isSleeping() && !cliff->isSleeping() && !back->isSleeping(),
                "a real hole, cliff edge or back-to-front crossing must not recover");
        }
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
  }
