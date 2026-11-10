#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
// Format 3 bonds of debris (reports/physics/2026-11-destruction-bonds): a blow breaks only
// the bonds near it, joined pieces fall as one native body. Part of gothic-physics-contract.
#include "physics/collisionworld.h"
#include "physics/dynamicworld.h"
#include "physics/fracture.h"
#include "graphics/mesh/protomesh.h"
#include "graphics/mesh/submesh/packedmesh.h"
#include "game/serialize.h"
#include "basemesh.h"
#include "resources.h"

#include <Tempest/Log>
#include <Tempest/MemReader>
#include <Tempest/MemWriter>

#include <Jolt/Jolt.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {
void require(bool condition, const char* message) {
  if(condition) return;
  std::fprintf(stderr,"FAIL: %s\n",message);
  std::exit(EXIT_FAILURE);
  }

void requireNear(float actual, float expected, float tolerance, const char* message) {
  if(std::isfinite(actual) && std::abs(actual-expected)<=tolerance) return;
  std::fprintf(stderr,"FAIL: %s: actual %.9g, expected %.9g +/- %.9g\n",message,double(actual),double(expected),double(tolerance));
  std::exit(EXIT_FAILURE);
  }

bool sameMatrix(const Tempest::Matrix4x4& a, const Tempest::Matrix4x4& b, float tolerance) {
  for(int c=0;c<4;++c)
    for(int r=0;r<4;++r)
      if(!(std::abs(a.at(c,r)-b.at(c,r))<=tolerance)) return false;
  return true;
  }

Tempest::Matrix4x4 at(float x, float y, float z) {
  auto matrix=Tempest::Matrix4x4::mkIdentity();
  matrix.translate(x,y,z);
  return matrix;
  }

// Corners and faces of an axis-aligned box, centimetres.
std::vector<Tempest::Vec3> corners(const Tempest::Vec3& center, const Tempest::Vec3& size) {
  std::vector<Tempest::Vec3> points;
  for(int i=0;i<8;++i)
    points.emplace_back(center.x+(i&1 ? .5f : -.5f)*size.x,center.y+(i&2 ? .5f : -.5f)*size.y,center.z+(i&4 ? .5f : -.5f)*size.z);
  return points;
  }
constexpr int BoxFaces[12][3]={{0,2,1},{1,2,3},{4,5,6},{5,7,6},{0,1,4},{1,5,4},
                               {2,6,3},{3,6,7},{0,4,2},{2,4,6},{1,3,5},{3,7,5}};

// The intact fence: a 100 cm cube with Gothic source winding (clockwise; PhysicVbo reverses it).
asset::StaticMesh parentMesh() {
  asset::StaticMesh mesh;
  mesh.positions={{-50,-50,-50},{50,-50,-50},{50,50,-50},{-50,50,-50},{-50,-50,50},{50,-50,50},{50,50,50},{-50,50,50}};
  mesh.bbox={{-50,-50,-50},{50,50,50}};
  mesh.sub_meshes.resize(1);
  auto& part=mesh.sub_meshes[0];
  part.mat.name="WOOD";
  part.mat.group=asset::MaterialGroup::WOOD;
  for(size_t i=0;i<mesh.positions.size();++i)
    part.wedges.push_back({{0,1,0},{0,0},uint16_t(i)});
  part.triangles={{{0,1,2}},{{0,2,3}},{{4,6,5}},{{4,7,6}},{{0,7,4}},{{0,3,7}},
                  {{1,6,2}},{{1,5,6}},{{0,5,1}},{{0,4,5}},{{3,6,7}},{{3,2,6}}};
  return mesh;
  }

struct Box { Tempest::Vec3 center, size; const char* material; };

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

// Native bodies of a broken prop: one water entry per body.
std::vector<DynamicWorld::WaterBodySnapshot> bodiesOf(const DynamicWorld& world, uint32_t zenId) {
  std::vector<DynamicWorld::WaterBodySnapshot> result;
  for(const auto& body:world.waterBodySnapshot())
    if(body.zenId==zenId && body.piece!=size_t(-1)) result.push_back(body);
  return result;
  }

std::vector<uint32_t> groupsOf(const DynamicWorld& world) {
  std::vector<uint32_t> result;
  for(const auto& piece:world.fragmentSnapshot()) result.push_back(piece.bondGroup);
  return result;
  }

// Second step (repeat blows on remnants): helpers over the fixture of testFractureBonds.
using WriteFixture=std::function<void(const std::vector<Box>&, const std::string&)>;
using MakeFixture=std::function<DynamicWorld::Item(DynamicWorld&)>;

Tempest::Vec3 project(const Tempest::Matrix4x4& matrix, Tempest::Vec3 point) {
  matrix.project(point);
  return point;
  }

// A blade stopped by the body of `piece`'s group at `local`, a point in the frame of the pieces.
bool blowOn(DynamicWorld& world, size_t piece, const Tempest::Vec3& local, float weapon, float strength,
            FractureRule::Melee kind, std::vector<uintptr_t>& struck) {
  const auto target=world.fragmentSnapshot()[piece];
  MeleeBladeContact contact {};
  contact.world=contact.stopsSwing=true;
  contact.fragment=target.body;
  contact.point=project(target.transform,local);
  contact.velocityCmS={250,0,0};
  return world.meleeStoppedMovableVob(contact,weapon,strength,kind,nullptr,struck);
  }

bool blowOnProp(DynamicWorld& world, const Tempest::Vec3& point, FractureRule::Melee kind, std::vector<uintptr_t>& struck) {
  MeleeBladeContact contact {};
  contact.world=contact.stopsSwing=true;
  contact.movableVob=world.movableVobSnapshot()[0].id;
  contact.point=point;
  contact.velocityCmS={250,0,0};
  return world.meleeStoppedMovableVob(contact,40,1000,kind,nullptr,struck);
  }

void testFractureRemnants(const WriteFixture& write, const MakeFixture& make, uint32_t zenId) {
  using Melee=FractureRule::Melee;
  // The nailed row of eight boards; the one-handed side blow leaves boards 0..2 loose and the
  // remnant 3..7 (x -10..40 cm). Its reserve: 100 health x 5/8 of the volume x remnant 0.5 = 31.25;
  // a blade of weapon 0 and strength 40 deals (0 x 0.5 + 40 x 0.5) x 1 = 20.
  std::vector<Box> row;
  std::ostringstream nails;
  nails<<"bonds 7\n";
  for(int i=0;i<8;++i) {
    row.push_back({{-35.f+10.f*float(i),0,0},{10,20,10},"WOOD"});
    if(i<7) nails<<"bond "<<i<<' '<<i+1<<" NAIL "<<-30.f+10.f*float(i)<<" 0 0\n";
    }
  write(row,nails.str());
  const auto firstBreak=[&](DynamicWorld& world, std::vector<uintptr_t>& swing) {
    require(blowOnProp(world,{-50,1000,0},Melee::OneHand,swing),"the remnant fixture must take its first blow");
    world.tick(0);
    require(groupsOf(world)==std::vector<uint32_t>({0,1,2,3,3,3,3,3}),"the first blow leaves the remnant of boards 3..7");
    };
  // At x=-30 a one-handed blow (35 cm) reaches boards 3 and 4 (20 and 30 cm): bonds 3-4 and 4-5 go,
  // 5..7 stay one body.
  const Tempest::Vec3 nearEnd(-30,0,0);
  const std::vector<uint32_t> second={0,1,2,3,4,5,5,5};
  DynamicWorld world(nullptr,asset::WorldMesh{});
  auto object=make(world);
  std::vector<uintptr_t> swing;
  firstBreak(world,swing);
  require(!blowOn(world,3,nearEnd,40,1000,Melee::OneHand,swing),"the swing that broke the prop must not strike its remnant");
  std::vector<uintptr_t> weak;
  require(blowOn(world,3,nearEnd,0,40,Melee::OneHand,weak),"a stopped blade must strike the remnant");
  world.tick(0);
  require(bodiesOf(world,zenId).size()==4,"a blow below the remnant's reserve must not break it");
  require(!blowOn(world,3,nearEnd,0,40,Melee::OneHand,weak),"one swing must strike a remnant once");
  // Saved here, between the blows: 20 of 31.25 taken.
  const auto saved=saveArchive([&](Serialize& output) { world.saveMovableVobs(output); });
  std::vector<uintptr_t> again;
  require(blowOn(world,3,nearEnd,0,40,Melee::OneHand,again),"the remnant must take the second weak blow");
  world.tick(0);
  require(groupsOf(world)==second && bodiesOf(world,zenId).size()==6,
          "damage adds up: the second blow breaks the near bonds, the far boards stay one body");
  for(const auto& body:bodiesOf(world,zenId))
    if(body.piece<5)
      require(body.category==DynamicWorld::C_FragmentSmall,"a 20 cm board must take the small debris layer characters pass");
  require(!blowOn(world,5,nearEnd,0,40,Melee::OneHand,again),"one swing must not break a remnant twice");
  std::vector<uintptr_t> single;
  const auto before=world.fragmentSnapshot()[0].velocity;
  require(!blowOn(world,0,{-45,0,0},40,1000,Melee::OneHand,single) && bodiesOf(world,zenId).size()==6,
          "a single piece must not split");
  require((world.fragmentSnapshot()[0].velocity-before).length()>1.f,"a single piece a blade stops at only takes its push");
  // After loading, the same bonds, groups and damage: the same blow gives the same result.
  DynamicWorld loaded(nullptr,asset::WorldMesh{});
  auto loadedObject=make(loaded);
  loadMovables(loaded,saved);
  require(loadedObject.isBroken() && groupsOf(loaded)==std::vector<uint32_t>({0,1,2,3,3,3,3,3}),
          "a save between blows must restore the remnant");
  std::vector<uintptr_t> afterLoad;
  require(blowOn(loaded,3,nearEnd,0,40,Melee::OneHand,afterLoad),"the loaded remnant must take the blow");
  loaded.tick(0);
  require(groupsOf(loaded)==second,"a save must keep the remnant's damage: the next blow breaks it as without saving");

  // remnant=0: the first blow on a remnant breaks it.
  setenv("OPENGOTHIC_FRACTURE_BONDS","remnant=0",1);
  DynamicWorld loose(nullptr,asset::WorldMesh{});
  unsetenv("OPENGOTHIC_FRACTURE_BONDS");
  auto looseObject=make(loose);
  std::vector<uintptr_t> looseSwing,looseBlow;
  firstBreak(loose,looseSwing);
  require(blowOn(loose,3,nearEnd,0,1,Melee::Fist,looseBlow),"remnant=0 fixture must take the blow");
  loose.tick(0);
  require(bodiesOf(loose,zenId).size()>4,"with remnant=0 every blow must break the remnant");

  // An arrow (0.12 kg at 40 m/s: 4.8 Ns, 40 health) at board 3's face: 12 cm reach the bond 4-5.
  struct Arrow final : DynamicWorld::BulletCallback {
    void onCollide(asset::MaterialGroup) override { ++hits; }
    bool stopsAtWorldHit() const override { return true; }
    void onWorldHit(const DynamicWorld::BulletWorldHit&) override { ++pinned; }
    int hits=0,pinned=0;
    } arrowContact;
  DynamicWorld archery(nullptr,asset::WorldMesh{});
  auto archeryObject=make(archery);
  std::vector<uintptr_t> archerySwing;
  firstBreak(archery,archerySwing);
  auto* arrow=archery.bulletObj(&arrowContact);
  arrow->setPosition(project(archery.fragmentSnapshot()[3].transform,{-35,0,0}));
  arrow->setDirection({4,0,0});
  archery.tick(17);
  archery.deleteObj(arrow);
  require(arrowContact.hits==1 && groupsOf(archery)==second && bodiesOf(archery,zenId).size()==6,
          "an arrow must stop at the remnant and break the bonds near it");
  require(arrowContact.pinned==0,"an arrow stopped by debris must not be pinned to the static world");

  // The force of a spell breaks every bond of the remnant; the loose boards stay as they are.
  DynamicWorld blast(nullptr,asset::WorldMesh{});
  auto blastObject=make(blast);
  std::vector<uintptr_t> blastSwing;
  firstBreak(blast,blastSwing);
  const auto center=blast.fragmentSnapshot()[3].centerOfMass;
  require(blast.applyMovableVobImpulse(center,100.f,[](const Tempest::Vec3&,float mass) {
            return Tempest::Vec3(mass*1000.f,0,0);
            },nullptr,&center)==1,"a spell must reach the remnant only");
  blast.tick(0);
  require(groupsOf(blast)==std::vector<uint32_t>({0,1,2,3,4,5,6,7}),"a spell's force must break every bond of the remnant");

  // The ring counts the HOOP bonds broken by the first blow. Six staves under one hoop, joined
  // side by side by ACROSS (strength 2: a fist's 20 cm reach 10 cm). A fist at stave 0 frees it
  // and cuts stave 1's HOOP bond, stave 1 stays by its ACROSS bond: 2 of 6 lost, the hoop holds.
  // A fist at stave 5 cuts the HOOP bonds of 5 and 4: 4 of 6 lost, the hoop lets go of all.
  std::vector<Box> barrel;
  std::ostringstream ring;
  ring<<"bonds 11\n";
  for(int i=0;i<6;++i) {
    barrel.push_back({{-25.f+10.f*float(i),0,0},{10,60,10},"WOOD"});
    ring<<"bond "<<i<<" 6 HOOP "<<-25.f+10.f*float(i)<<" 30 0\n";
    if(i<5) ring<<"bond "<<i<<' '<<i+1<<" ACROSS "<<-20.f+10.f*float(i)<<" 0 0\n";
    }
  barrel.push_back({{0,32.5f,0},{60,5,10},"METAL"});
  write(barrel,ring.str());
  setenv("OPENGOTHIC_FRACTURE_BONDS","across=2",1);
  const auto ringBreak=[&](DynamicWorld& ringWorld) {
    std::vector<uintptr_t> first;
    require(blowOnProp(ringWorld,{-32,1000,0},Melee::Fist,first),"the ring fixture must take the fist");
    ringWorld.tick(0);
    require(groupsOf(ringWorld)==std::vector<uint32_t>({0,1,1,1,1,1,1}),"a fist at stave 0 must free it alone");
    };
  const auto ringBlow=[&](DynamicWorld& ringWorld) {
    std::vector<uintptr_t> blow;
    require(blowOn(ringWorld,1,{32,0,0},40,1000,Melee::Fist,blow),"the ring remnant must take the fist");
    ringWorld.tick(0);
    return groupsOf(ringWorld);
    };
  DynamicWorld ringWorld(nullptr,asset::WorldMesh{}),ringLoaded(nullptr,asset::WorldMesh{}),ringOld(nullptr,asset::WorldMesh{});
  unsetenv("OPENGOTHIC_FRACTURE_BONDS");
  auto ringObject=make(ringWorld);
  auto ringLoadedObject=make(ringLoaded);
  auto ringOldObject=make(ringOld);
  ringBreak(ringWorld);
  const auto ringSave=saveArchive([&](Serialize& output) { ringWorld.saveMovableVobs(output); });
  loadMovables(ringLoaded,ringSave);
  // Schema 3 has no bond fates: bonds inside a group are whole, so stave 1 holds the hoop again.
  const auto state=ringWorld.movableVobSnapshot()[0];
  const auto pieces=ringWorld.fragmentSnapshot();
  const auto legacy=saveArchive([&](Serialize& output) {
    output.setEntry("worlds/",output.worldName(),"/destruction-vobs");
    output.write(uint32_t(0x31534544),uint16_t(3),output.worldName(),uint32_t(1));
    output.write(zenId,state.visual,state.origin,state.transform,0.f,true,uint32_t(pieces.size()),uint64_t(1),std::string());
    output.write(false);
    for(const auto& piece:pieces) output.write(piece.bondGroup);
    output.write(uint32_t(2));
    for(const size_t piece:{size_t(0),size_t(1)})
      output.write(true,true,uint64_t(0),pieces[piece].transform,pieces[piece].velocity,pieces[piece].angularVelocity,false);
    });
  loadMovables(ringOld,legacy);
  require(ringOldObject.isBroken() && groupsOf(ringOld)==groupsOf(ringWorld),"a schema 3 save must load its groups");
  const auto ringGroups=ringBlow(ringWorld);
  require(ringGroups==std::vector<uint32_t>({0,1,1,1,1,2,3}),
          "the ring must count the HOOP bonds broken by the first blow: 4 of 6 lost lets the staves go");
  for(const auto& body:bodiesOf(ringWorld,zenId))
    require(body.category==DynamicWorld::C_Fragment,"a 60 cm stave or hoop must take the debris layer characters stop at");
  require(ringBlow(ringLoaded)==ringGroups,"a save must keep every bond's fate: the next blow gives the same result");
  require(ringBlow(ringOld)==std::vector<uint32_t>({0,1,1,1,1,2,1}),
          "a schema 3 save joins the pieces of one group by all their bonds");
  std::printf("Fracture remnants: weak blows 20+20 of 31.25 break the row's remnant into %zu bodies, the far boards one; "
              "arrow and spell break it; ring counts earlier HOOP losses; damage and bonds survive save; schema 3 loads\n",
              bodiesOf(world,zenId).size());
  }

// Debris from 0.5 m on stops a walker, which pushes it; a smaller piece is passed. A remnant that
// appears around a walker is passed until they separate, then blocks again.
void testCharacterDebris() {
  constexpr float slope=.785398163f,radius=25.f;
  const auto gap=[&](const CollisionWorld::CollisionBody& body, float walkerX, bool front) {
    JPH::Vec3 min,max;
    body.getAabb(min,max);
    return front ? min.GetX()*100.f-(walkerX+radius) : walkerX-radius-max.GetX()*100.f;
    };
  for(const float half:{30.f,10.f}) {
    CollisionWorld world;
    auto floor=world.addCollisionBody(new JPH::BoxShape(JPH::Vec3(20,.5f,20)),at(0,-50,0),.7f,DynamicWorld::C_Landscape);
    // The layer the engine gives debris of this size (addFragmentBody).
    auto debris=world.addDynamicBody(new JPH::BoxShape(JPH::Vec3(half*.01f,.15f,half*.01f)),at(150,15,0),.6f,20.f,
                                     2.f*half>=CollisionWorld::fragmentBlockSize*100.f ? DynamicWorld::C_Fragment
                                                                                       : DynamicWorld::C_FragmentSmall);
    for(int i=0;i<30;++i) world.tick(17);
    auto walker=world.createVirtualCharacter(radius,180,{0,0,0});
    CollisionWorld::CharacterMoveResult state;
    float deepest=1.e9f;
    for(int frame=0;frame<100;++frame) {
      state=walker->move({5,0,0},17,50,slope,80,nullptr);
      world.tick(17);
      deepest=std::min(deepest,gap(*debris,state.position.x,true));
      }
    std::printf("CharacterVirtual and debris %.0f cm: walker x %.1f cm, deepest into it %.2f cm, debris x %.1f cm\n",
                double(2*half),double(state.position.x),double(-deepest),double(debris->position().x));
    if(half*2.f>=CollisionWorld::fragmentBlockSize*100.f)
      require(deepest>-2.f && debris->position().x>155.f,"a walker must stop at a remnant and push it, not enter it");
    else
      require(state.position.x>450.f,"a walker must pass a piece under 0.5 m");
    }
  CollisionWorld world;
  auto floor=world.addCollisionBody(new JPH::BoxShape(JPH::Vec3(20,.5f,20)),at(0,-50,0),.7f,DynamicWorld::C_Landscape);
  auto walker=world.createVirtualCharacter(radius,180,{0,0,0});
  for(int i=0;i<10;++i) walker->move({},17,50,slope,80,nullptr);
  auto debris=world.addDynamicBody(new JPH::BoxShape(JPH::Vec3(.3f,.15f,.3f)),at(0,15,0),.6f,20.f,DynamicWorld::C_Fragment);
  world.passCharacterOverlaps(*debris);
  const auto start=debris->position();
  for(int frame=0;frame<20;++frame) {
    walker->move({},17,50,slope,80,nullptr);
    world.tick(17);
    }
  const float pushed=(debris->position()-start).length();
  CollisionWorld::CharacterMoveResult state;
  for(int frame=0;frame<40;++frame) {
    state=walker->move({5,0,0},17,50,slope,80,nullptr);
    world.tick(17);
    }
  float deepest=1.e9f;
  for(int frame=0;frame<60;++frame) {
    state=walker->move({-5,0,0},17,50,slope,80,nullptr);
    world.tick(17);
    deepest=std::min(deepest,gap(*debris,state.position.x,false));
    }
  std::printf("CharacterVirtual inside a new remnant: pushed %.2f cm while inside; back from x 200: deepest %.2f cm\n",
              double(pushed),double(-deepest));
  require(pushed<1.f,"a remnant that appeared around a walker must be passed, not pushed away");
  {
    // A controller made anew while it still overlaps the remnant (a load, a teleport, a reset)
    // passes it the same way: neither the walker nor the remnant moves.
    CollisionWorld again;
    auto ground=again.addCollisionBody(new JPH::BoxShape(JPH::Vec3(20,.5f,20)),at(0,-50,0),.7f,DynamicWorld::C_Landscape);
    auto first=again.createVirtualCharacter(radius,180,{0,0,0});
    for(int i=0;i<10;++i) first->move({},17,50,slope,80,nullptr);
    auto remnant=again.addDynamicBody(new JPH::BoxShape(JPH::Vec3(.3f,.15f,.3f)),at(51,15,0),.6f,20.f,DynamicWorld::C_Fragment);
    again.passCharacterOverlaps(*remnant);
    const auto feet=first->move({},17,50,slope,80,nullptr).position;
    first.reset();
    auto remade=again.createVirtualCharacter(radius,180,feet);
    const auto from=remnant->position();
    CollisionWorld::CharacterMoveResult still;
    for(int frame=0;frame<20;++frame) {
      still=remade->move({},17,50,slope,80,nullptr);
      again.tick(17);
      }
    const float walkerShift=(still.position-feet).length(),remnantShift=(remnant->position()-from).length();
    std::printf("CharacterVirtual made anew in a remnant: walker %.3f cm, remnant %.3f cm\n",double(walkerShift),double(remnantShift));
    require(walkerShift<.5f && remnantShift<.5f,"a walker made anew inside a remnant must pass it, not push out of it");
  }
  require(deepest>-2.f,"after they separate the remnant must block the walker again");
  // A walker standing still on a stave, or against a board it walked into, from 0.5 m, leaves the
  // piece to fall asleep no later than 2 s after it would alone, and never wakes it again.
  struct Still { const char* what; float mass; bool onTop; };
  for(const Still still:{Still{"on a 15 kg stave",15.f,true},Still{"on a 3 kg stave",3.f,true},Still{"against a 15 kg board",15.f,false}}) {
    const auto run=[&](bool walker,int& first,int& wakes) {
      CollisionWorld world;
      auto ground=world.addCollisionBody(new JPH::BoxShape(JPH::Vec3(20,.5f,20)),at(0,-50,0),.7f,DynamicWorld::C_Landscape);
      // A stave 110 x 5 x 12 cm lies flat; a board 110 x 30 x 5 cm stands on its long edge.
      const JPH::Vec3 half=still.onTop ? JPH::Vec3(.55f,.025f,.06f) : JPH::Vec3(.55f,.15f,.025f);
      auto piece=world.addDynamicBody(new JPH::BoxShape(half),at(0,half.GetY()*100.f,0),.6f,still.mass,DynamicWorld::C_Fragment);
      std::unique_ptr<CollisionWorld::VirtualCharacter> character;
      if(walker)
        character=world.createVirtualCharacter(radius,180,still.onTop ? Tempest::Vec3(0,5,0) : Tempest::Vec3(0,0,-(2.5f+radius+20.f)));
      first=-1;wakes=0;
      bool was=false;
      for(int frame=0;frame<1800;++frame) {
        if(character!=nullptr)
          character->move({0,0,!still.onTop && frame<15 ? 2.f : 0.f},17,50,slope,80,nullptr);
        world.tick(17);
        const bool now=piece->isSleeping();
        if(now && first<0) first=frame;
        wakes+=was && !now;
        was=now;
        }
      return was;
      };
    int alone=0,first=0,wakes=0,unused=0;
    run(false,alone,unused);
    const bool asleep=run(true,first,wakes);
    std::printf("CharacterVirtual still %s: asleep after %d frames (alone %d), woken %d, asleep at the end %d\n",
                still.what,first,alone,wakes,int(asleep));
    require(first>=0 && first<=alone+120 && wakes==0 && asleep,
            "a walker standing still must let the debris under or beside it sleep and not wake it");
    }
  }
}

void testFractureBonds() {
  const auto root=std::filesystem::temp_directory_path()/
                  ("gothic-fracture-bonds-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(root/"meshes");
  const auto* oldContentValue=std::getenv("OPENGOTHIC_CONTENT");
  const bool hadContent=oldContentValue!=nullptr;
  const std::string oldContent=hadContent ? oldContentValue : "";
  const auto* oldBondsValue=std::getenv("OPENGOTHIC_FRACTURE_BONDS");
  const bool hadBonds=oldBondsValue!=nullptr;
  const std::string oldBonds=hadBonds ? oldBondsValue : "";
  setenv("OPENGOTHIC_CONTENT",root.c_str(),1);
  unsetenv("OPENGOTHIC_FRACTURE_BONDS");
  constexpr const char* visual="NW_NATURE_FENCE_01.MRM";
  constexpr uint32_t zenId=6404;
  writeBaseMesh(parentMesh(),"NW_NATURE_FENCE_01",root/"meshes/model.glb");
  std::ofstream(root/"meshes/meshes.json")<<R"({"NW_NATURE_FENCE_01":{"glb":"model.glb"}})";
  {
    Resources resources;
    bool replaced=false;
    const auto loaded=Resources::loadVisualMesh(visual,&replaced);
    Resources::MeshReplacementSource source;
    require(replaced && Resources::meshReplacementLoaded(visual,&source),"the bond fixture needs its own loaded GLB");
    ProtoMesh proto(PackedMesh(loaded,PackedMesh::PK_Visual,true),visual);
    const auto write=[&](const std::vector<Box>& boxes, const std::string& bonds) {
      std::ofstream file(root/"meshes/model.fracture");
      file<<"GOTHIC_FRACTURE 3\nsource "<<source.size<<' '<<std::hex<<source.crc32<<std::dec<<"\npieces "<<boxes.size()<<'\n';
      for(const auto& box:boxes) {
        const auto points=corners(box.center,box.size);
        file<<"piece "<<box.size.x*box.size.y*box.size.z<<' '<<box.center.x<<' '<<box.center.y<<' '<<box.center.z<<' '
            <<box.material<<" 1 12\nhull 8\n";
        for(const auto& p:points) file<<p.x<<' '<<p.y<<' '<<p.z<<'\n';
        for(const auto& face:BoxFaces) {
          file<<"tri WOOD.TGA\n";
          for(const int index:face) file<<points[size_t(index)].x<<' '<<points[size_t(index)].y<<' '<<points[size_t(index)].z<<" 0 1 0 0 0\n";
          }
        }
      file<<bonds;
      require(bool(file),"the bond fixture must write its sidecar");
      };
    const auto make=[&](DynamicWorld& world) {
      // A static fence: the break adds no parent momentum.
      return world.addMovableVob(proto,visual,at(0,1000,0),.04f,true,"fence",true,zenId);
      };
    const auto strike=[&](DynamicWorld& world, const Tempest::Vec3& point) {
      MeleeBladeContact contact {};
      contact.world=contact.stopsSwing=true;
      contact.movableVob=world.movableVobSnapshot()[0].id;
      contact.point=point;
      contact.velocityCmS={250,0,0};
      std::vector<uintptr_t> struck;
      return world.meleeStoppedMovableVob(contact,40,1000,FractureRule::Melee::OneHand,nullptr,struck);
      };

    // Eight boards in a row along x, nailed to their neighbours. A one-handed blow (35 cm)
    // from the -x side at x=-50: boards 0..2 lie within reach and come off, 3..7 stay one body.
    std::vector<Box> row;
    std::ostringstream nails;
    nails<<"bonds 7\n";
    for(int i=0;i<8;++i) {
      row.push_back({{-35.f+10.f*float(i),0,0},{10,20,10},"WOOD"});
      if(i<7) nails<<"bond "<<i<<' '<<i+1<<" NAIL "<<-30.f+10.f*float(i)<<" 0 0\n";
      }
    write(row,nails.str());
    const auto mesh=FractureMesh::load(visual);
    require(mesh!=nullptr && mesh->bonds.size()==7 && mesh->bonds[3].a==3 && mesh->bonds[3].b==4 &&
            mesh->bonds[3].kind==FractureMesh::BondKind::Nail,"a format 3 sidecar must load its bond block");
    DynamicWorld world(nullptr,asset::WorldMesh{});
    auto object=make(world);
    require(strike(world,{-50,1000,0}),"the one-handed blow must break the nailed row");
    world.tick(0);
    const auto pieces=world.fragmentSnapshot();
    const auto bodies=bodiesOf(world,zenId);
    require(object.isBroken() && pieces.size()==8 && bodies.size()==4,
            "a one-handed side blow must leave three loose boards and one body of the other five");
    for(size_t i=0;i<8;++i)
      require(pieces[i].bondGroup==(i<3 ? i : 3),"boards within reach come off, the far boards stay one group");
    for(size_t i=4;i<8;++i)
      require(sameMatrix(pieces[i].transform,pieces[3].transform,0.f) && pieces[i].sleeping==pieces[3].sleeping,
              "pieces of one group share the pose of one body");
    const auto single=std::find_if(bodies.begin(),bodies.end(),[](const auto& body) { return body.piece==0; });
    const auto joined=std::find_if(bodies.begin(),bodies.end(),[](const auto& body) { return body.piece==3; });
    require(single!=bodies.end() && joined!=bodies.end(),"a group body reports its first piece");
    requireNear(joined->mass,5.f*single->mass,single->mass*1.e-4f,"the mass of a joined group is the sum of its pieces");
    require(pieces[3].velocity.x>0 && pieces[3].angularVelocity.length()<1.e-4f,
            "a joined group takes the blow without added spin");

    // Save and load: groups, poses and the per-group state return.
    const auto saved=saveArchive([&](Serialize& output) { world.saveMovableVobs(output); });
    DynamicWorld restored(nullptr,asset::WorldMesh{});
    auto restoredObject=make(restored);
    loadMovables(restored,saved);
    const auto restoredPieces=restored.fragmentSnapshot();
    require(restoredObject.isBroken() && groupsOf(restored)==groupsOf(world) && bodiesOf(restored,zenId).size()==4,
            "save and load must restore every piece in its bond group");
    for(size_t i=0;i<8;++i)
      require(sameMatrix(restoredPieces[i].transform,pieces[i].transform,.001f) && restoredPieces[i].permanent==pieces[i].permanent,
              "save and load must restore the pose and permanence of each group");
    // A break struck just before saving: the blow is saved and breaks the same way after loading.
    DynamicWorld pending(nullptr,asset::WorldMesh{});
    auto pendingSource=make(pending);
    require(strike(pending,{-50,1000,0}),"the pending fixture must take the blow");
    const auto pendingSave=saveArchive([&](Serialize& output) { pending.saveMovableVobs(output); });
    DynamicWorld pendingLoaded(nullptr,asset::WorldMesh{});
    auto pendingObject=make(pendingLoaded);
    loadMovables(pendingLoaded,pendingSave);
    pendingLoaded.tick(0);
    require(pendingObject.isBroken() && groupsOf(pendingLoaded)==groupsOf(world),
            "a pending break must keep its blow kind and point through save and load");
    // A schema 2 save has no groups: each piece is its own group.
    const auto state=world.movableVobSnapshot()[0];
    const auto legacy=saveArchive([&](Serialize& output) {
      output.setEntry("worlds/",output.worldName(),"/destruction-vobs");
      output.write(uint32_t(0x31534544),uint16_t(2),output.worldName(),uint32_t(1));
      output.write(zenId,state.visual,state.origin,state.transform,0.f,true,uint32_t(pieces.size()),uint64_t(1),std::string());
      for(size_t i=0;i<pieces.size();++i)
        output.write(true,i<3,uint64_t(0),pieces[i].transform,pieces[i].velocity,pieces[i].angularVelocity,false);
      });
    DynamicWorld old(nullptr,asset::WorldMesh{});
    auto oldObject=make(old);
    loadMovables(old,legacy);
    require(oldObject.isBroken() && groupsOf(old)==std::vector<uint32_t>({0,1,2,3,4,5,6,7}) && bodiesOf(old,zenId).size()==8,
            "a save without bond groups must load each piece as its own group");

    // The force of a spell breaks every bond.
    DynamicWorld blast(nullptr,asset::WorldMesh{});
    auto blastObject=make(blast);
    const Tempest::Vec3 blastPoint(-50,1000,0);
    require(blast.applyMovableVobImpulse({0,1000,0},500.f,[](const Tempest::Vec3&,float mass) {
              return Tempest::Vec3(mass*1000.f,0,0);
              },nullptr,&blastPoint)==1,"the spell fixture must reach the row");
    blast.tick(0);
    require(blastObject.isBroken() && groupsOf(blast)==std::vector<uint32_t>({0,1,2,3,4,5,6,7}) &&
            bodiesOf(blast,zenId).size()==8,"a spell's force must break every bond");

    // The ring: tall staves side by side and a hoop across their tops, the blow on the hoop at
    // the -x end. The hoop is the nearest piece, yet the blade strikes the stave under it and the
    // hoop counts only its staves: the blow reaches three of them; of six that is half and the
    // hoop holds the other three; of five it is more than half and the hoop lets go of all.
    for(const int staves:{6,5}) {
      std::vector<Box> barrel;
      std::ostringstream hoops;
      hoops<<"bonds "<<staves<<'\n';
      for(int i=0;i<staves;++i) {
        barrel.push_back({{-25.f+10.f*float(i),0,0},{10,60,10},"WOOD"});
        hoops<<"bond "<<i<<' '<<staves<<" HOOP "<<-25.f+10.f*float(i)<<" 30 0\n";
        }
      barrel.push_back({{-30.f+5.f*float(staves),32.5f,0},{10.f*float(staves),5,10},"METAL"});
      write(barrel,hoops.str());
      DynamicWorld ring(nullptr,asset::WorldMesh{});
      auto ringObject=make(ring);
      require(strike(ring,{-44,1000+32,0}),"the ring fixture must take the blow");
      ring.tick(0);
      const auto groups=groupsOf(ring);
      require(ringObject.isBroken() && groups.size()==size_t(staves+1),"the ring fixture must keep every piece");
      if(staves==6)
        require(groups==std::vector<uint32_t>({0,1,2,3,3,3,3}),"a hoop that lost half of its staves holds the rest");
      else
        require(groups==std::vector<uint32_t>({0,1,2,3,4,5}),"a hoop that lost more than half of its staves lets go of all");
      }

    testFractureRemnants(write,make,zenId);
    testCharacterDebris();

    // A wrong block does not load, with one line in the log that names the fault.
    const std::pair<const char*,const char*> invalid[]={
      {"bonds 1\nbond 0 1 SCREW 0 0 0\n","unknown kind 'SCREW'"},
      {"bonds 1\nbond 1 0 NAIL 0 0 0\n","must be less than"},
      {"bonds 1\nbond 3 3 NAIL 0 0 0\n","must be less than"},
      {"bonds 1\nbond 0 8 NAIL 0 0 0\n","outside 0..7"},
      {"bonds 2\nbond 0 1 NAIL 0 0 0\nbond 0 1 ALONG 0 0 0\n","repeats"},
      {"bonds 1025\n","exceed 1024"},
      {"bonds 2\nbond 0 1 NAIL 0 0 0\n","expected 'bond A B KIND x y z'"},
      };
    for(const auto& [block,reason]:invalid) {
      write(row,block);
      std::vector<std::string> errors;
      Tempest::Log::setOutputCallback([&](Tempest::Log::Mode mode,const char* line) {
        if(mode==Tempest::Log::Error && std::string_view(line).find("invalid bonds block")!=std::string_view::npos)
          errors.emplace_back(line);
        });
      const auto rejected=FractureMesh::load(visual);
      Tempest::Log::setOutputCallback({});
      require(rejected==nullptr,"an invalid bond block must not load");
      require(errors.size()==1 && errors[0].find(reason)!=std::string::npos,"an invalid bond block must log its fault");
      }
    write(row,"bonds 1\nbond 0 1 NAIL 0 0 0\nextra\n");
    require(FractureMesh::load(visual)==nullptr,"text after the bond block must not load");

    // Numbers without a build: OPENGOTHIC_FRACTURE_BONDS; an invalid entry keeps the defaults.
    setenv("OPENGOTHIC_FRACTURE_BONDS","one=5",1);
    const auto tuned=FractureBonds::load();
    requireNear(tuned.radius[1],5.f,0.f,"OPENGOTHIC_FRACTURE_BONDS must set the one-handed radius");
    setenv("OPENGOTHIC_FRACTURE_BONDS","one=5,nail=0",1);
    const auto fallback=FractureBonds::load();
    requireNear(fallback.radius[1],35.f,0.f,"an invalid bond override must keep every default");
    unsetenv("OPENGOTHIC_FRACTURE_BONDS");
    std::printf("Fracture bonds: one-handed side blow 4 bodies of 8 boards (joined mass %.4f = 5 x %.4f kg); "
                "spell 8 bodies; ring 6/5 staves; groups survive save, pending save and schema 2; %zu invalid blocks rejected\n",
                double(joined->mass),double(single->mass),std::size(invalid));
  }
  if(hadContent) setenv("OPENGOTHIC_CONTENT",oldContent.c_str(),1); else unsetenv("OPENGOTHIC_CONTENT");
  if(hadBonds) setenv("OPENGOTHIC_FRACTURE_BONDS",oldBonds.c_str(),1); else unsetenv("OPENGOTHIC_FRACTURE_BONDS");
  std::filesystem::remove_all(root);
  }
