#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
// Format 3 bonds of debris (reports/physics/2026-11-destruction-bonds): a blow breaks only
// the bonds near it, joined pieces fall as one native body. Part of gothic-physics-contract.
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
