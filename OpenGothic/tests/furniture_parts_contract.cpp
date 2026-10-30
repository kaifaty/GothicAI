#include "physics/convexparts.h"
#include "physics/dynamicworld.h"
#include "graphics/mesh/protomesh.h"
#include "graphics/mesh/submesh/packedmesh.h"
#include "graphics/bounds.h"
#include "game/serialize.h"
#include "resources.h"

#include <Tempest/MemReader>
#include <Tempest/MemWriter>

#include <array>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <stdexcept>
#include <vector>

namespace {
  constexpr const char* visual = "CONTRACT_TABLE.MRM";
  constexpr uint32_t zenId = 801;

  void require(bool condition, const char* message) {
    if(!condition)
      throw std::runtime_error(message);
    }

  void near(float value, float expected, float tolerance, const char* message) {
    if(!std::isfinite(value) || std::abs(value-expected)>tolerance) {
      std::fprintf(stderr,"FAIL: %s: %.9g expected %.9g +/- %.9g\n",message,
                   double(value),double(expected),double(tolerance));
      throw std::runtime_error(message);
      }
    }

  Tempest::Matrix4x4 at(float x, float y, float z) {
    auto result = Tempest::Matrix4x4::mkIdentity();
    result.translate(x,y,z);
    return result;
    }

  struct Box {
    Tempest::Vec3 min, max;
    };
  const std::array<Box,5> tableBoxes = {{{{-90,90,-50},{90,100,50}},
    {{-80,0,-45},{-70,90,-35}},{{70,0,-45},{80,90,-35}},
    {{-80,0,35},{-70,90,45}},{{70,0,35},{80,90,45}}}};

  std::array<Tempest::Vec3,8> vertices(const Box& box) {
    const auto& a=box.min;
    const auto& b=box.max;
    return {{{a.x,a.y,a.z},{b.x,a.y,a.z},{b.x,b.y,a.z},{a.x,b.y,a.z},
             {a.x,a.y,b.z},{b.x,a.y,b.z},{b.x,b.y,b.z},{a.x,b.y,b.z}}};
    }

  asset::StaticMesh mesh(const std::vector<Box>& boxes) {
    asset::StaticMesh result;
    result.sub_meshes.resize(1);
    auto& sub=result.sub_meshes[0];
    sub.mat.group=asset::MaterialGroup::WOOD;
    constexpr uint16_t faces[12][3]={{0,2,1},{0,3,2},{4,5,6},{4,6,7},
      {0,4,7},{0,7,3},{1,2,6},{1,6,5},{0,1,5},{0,5,4},{3,7,6},{3,6,2}};
    for(const auto& box:boxes) {
      const auto base=uint16_t(result.positions.size());
      for(const auto& point:vertices(box)) {
        sub.wedges.push_back({{0,1,0},{0,0},uint16_t(result.positions.size())});
        result.positions.push_back({point.x,point.y,point.z});
        }
      for(const auto& face:faces)
        sub.triangles.push_back({{uint16_t(base+face[0]),uint16_t(base+face[1]),uint16_t(base+face[2])}});
      }
    result.bbox={result.positions[0],result.positions[0]};
    for(const auto& p:result.positions) {
      result.bbox.min.x=std::min(result.bbox.min.x,p.x);
      result.bbox.min.y=std::min(result.bbox.min.y,p.y);
      result.bbox.min.z=std::min(result.bbox.min.z,p.z);
      result.bbox.max.x=std::max(result.bbox.max.x,p.x);
      result.bbox.max.y=std::max(result.bbox.max.y,p.y);
      result.bbox.max.z=std::max(result.bbox.max.z,p.z);
      }
    return result;
    }

  asset::WorldMesh floor() {
    asset::WorldMesh result;
    result.materials.resize(1);
    result.materials[0].group=asset::MaterialGroup::STONE;
    for(int x=-4;x<4;++x)
      for(int z=-4;z<4;++z) {
        const auto first=uint32_t(result.vertices.size());
        result.vertices.insert(result.vertices.end(),{{float(x*100),0,float(z*100)},
          {float(x*100),0,float((z+1)*100)},{float((x+1)*100),0,float((z+1)*100)},
          {float((x+1)*100),0,float(z*100)}});
        result.polygons.vertex_indices.insert(result.polygons.vertex_indices.end(),
          {first,first+2,first+1,first,first+3,first+2});
        result.polygons.material_indices.insert(result.polygons.material_indices.end(),{0,0});
        }
    return result;
    }

  struct Cache {
    std::filesystem::path directory;
    std::string previous;
    bool hadPrevious=false;
    Cache() {
      if(const auto* value=std::getenv("OPENGOTHIC_CONVEX_PARTS")) {
        previous=value;
        hadPrevious=true;
        }
      directory=std::filesystem::temp_directory_path()/
        ("gothic-furniture-parts-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
      std::filesystem::create_directories(directory);
      std::ofstream output(directory/"CONTRACT_TABLE.convex-parts");
      output << "GOTHIC_CONVEX_PARTS 1\nsource 0 0\nparts 5\n";
      for(const auto& box:tableBoxes) {
        output << "part 8\n";
        for(const auto& p:vertices(box))
          output << p.x << ' ' << p.y << ' ' << p.z << '\n';
        }
      require(bool(output),"synthetic table cache must be writable");
      setenv("OPENGOTHIC_CONVEX_PARTS",directory.string().c_str(),1);
      }
    ~Cache() {
      if(hadPrevious) setenv("OPENGOTHIC_CONVEX_PARTS",previous.c_str(),1);
      else unsetenv("OPENGOTHIC_CONVEX_PARTS");
      std::filesystem::remove_all(directory);
      }
    };

  std::vector<uint8_t> archive(const std::function<void(Serialize&)>& write) {
    std::vector<uint8_t> bytes;
    Tempest::MemWriter writer(bytes);
    { Serialize output(writer); write(output); }
    return bytes;
    }

  void load(DynamicWorld& world,const std::vector<uint8_t>& bytes) {
    Tempest::MemReader reader(bytes);
    Serialize input(reader);
    world.loadMovableVobs(input);
    }

  void mechanics(const ProtoMesh& table,const ProtoMesh& item,bool enabled) {
    DynamicWorld world(nullptr,floor());
    require(world.furniturePartsEnabled(),"furniture parts must be enabled by default");
    world.setFurniturePartsEnabled(enabled);
    auto furniture=world.addMovableVob(table,visual,at(0,0,0),.04f,true,"table",true,zenId);
    require(!furniture.isEmpty(),"table must have a native body");
    require(world.movableVobSnapshot()[0].collisionParts==(enabled?5u:1u),"native table must use the selected cache shape");
    Bounds bounds;
    bounds.assign(std::pair<Tempest::Vec3,Tempest::Vec3>{{-5,-5,-5},{5,5,5}});
    auto lower=world.addDynamicObj(at(0,40,-80),bounds,asset::MaterialGroup::WOOD,&item,"CONTRACT_ITEM.MRM");
    lower.restorePhysics(at(0,40,-80),false,{0,0,180},{});
    auto upper=world.addDynamicObj(at(35,140,0),bounds,asset::MaterialGroup::WOOD,&item,"CONTRACT_ITEM.MRM");
    for(int i=0;i<360;++i)
      world.tick(17);
    const auto low=lower.position();
    const auto high=upper.position();
    std::printf("furniture_parts=%d lower_cm=(%.3f,%.3f,%.3f) upper_cm=(%.3f,%.3f,%.3f) mass_kg=%.6f\n",
      int(enabled),double(low.x),double(low.y),double(low.z),double(high.x),double(high.y),double(high.z),double(furniture.mass()));
    if(enabled) {
      near(low.y,6.f,2.f,"thrown item must settle on the floor under the tabletop");
      require(std::abs(low.x)<65.f && std::abs(low.z)<32.f,"thrown item must remain inside the tabletop footprint");
      near(high.y,106.f,3.f,"top item must remain supported by the tabletop");
      }
    else
      require(low.y>80.f || std::abs(low.x)>90.f || std::abs(low.z)>50.f,
              "single hull must continue to exclude the under-table space");
    }

  void toggleAndSave(const ProtoMesh& table) {
    DynamicWorld world(nullptr,asset::WorldMesh{});
    const auto origin=at(0,10000,0);
    auto furniture=world.addMovableVob(table,visual,origin,.04f,true,"table",true,zenId);
    const float mass=furniture.mass();
    const auto moved=at(20,10040,30);
    furniture.restorePhysics(moved,false,{12,34,56},{.1f,.2f,.3f});
    for(bool enabled:{false,true}) {
      world.setFurniturePartsEnabled(enabled);
      near(furniture.mass(),mass,0.f,"compound toggle must retain the exact old hull mass");
      near(furniture.position().y,10040.f,.001f,"compound toggle must retain the visual transform");
      near(furniture.velocity().z,56.f,.001f,"compound toggle must retain linear velocity");
      near(furniture.angularVelocity().z,.3f,.00001f,"compound toggle must retain angular velocity");
      require(!furniture.isSleeping(),"compound toggle must retain active state");
      }
    const auto old=archive([&](Serialize& output) {
      output.setEntry("worlds/_/movable-vobs");
      output.write(uint32_t(0x31564f4d),uint16_t(1),std::string_view("_"),uint32_t(1));
      output.write(zenId,std::string_view(visual),origin,moved,true);
      });
    load(world,old);
    near(furniture.position().y,10040.f,.001f,"pre-compound schema 1 save must restore its moved pose");
    require(furniture.isSleeping(),"pre-compound save must retain sleep state");
    require(world.movableVobSnapshot()[0].collisionParts==5,"old save must rebuild the current compound without serialized shapes");
    const auto current=archive([&](Serialize& output) { world.saveMovableVobs(output); });
    DynamicWorld off(nullptr,asset::WorldMesh{});
    off.setFurniturePartsEnabled(false);
    auto oldHull=off.addMovableVob(table,visual,origin,.04f,true,"table",true,zenId);
    load(off,current);
    near(oldHull.position().y,10040.f,.001f,"compound save must also load with the flag disabled");
    near(oldHull.mass(),mass,0.f,"save shape selection must retain exact hull-derived mass");
    require(off.movableVobSnapshot()[0].collisionParts==1,"save must not force serialized collision geometry");
    DynamicWorld barrelWorld(nullptr,asset::WorldMesh{});
    auto barrel=barrelWorld.addMovableVob(table,visual,origin,.04f,true,"barrel",true,zenId);
    require(!barrel.isEmpty() && barrelWorld.movableVobSnapshot()[0].collisionParts==1,
            "non-furniture classes must retain one hull even with a multi-part sidecar");
    barrelWorld.setFurniturePartsEnabled(false);
    barrelWorld.setFurniturePartsEnabled(true);
    require(barrelWorld.movableVobSnapshot()[0].collisionParts==1,
            "furniture-parts toggle must leave non-furniture classes on one hull");
    std::printf("toggle/save: schema1=passed schema2=passed mass_kg=%.6f\n",double(mass));
    }
  }

int main() {
  try {
    Cache cache;
    const auto parts=ConvexParts::read(cache.directory/"CONTRACT_TABLE.convex-parts",0,0);
    require(parts!=nullptr && parts->partCount==5,"synthetic table must load five convex parts from disk");
    require(ConvexParts::read(cache.directory/"CONTRACT_TABLE.convex-parts",1,0)==nullptr,
            "cache must reject a changed source size");
    require(ConvexParts::read(cache.directory/"CONTRACT_TABLE.convex-parts",0,1)==nullptr,
            "cache must reject a changed source CRC32");
    Resources resources;
    ProtoMesh table(PackedMesh(mesh(std::vector<Box>(tableBoxes.begin(),tableBoxes.end())),PackedMesh::PK_Visual),visual);
    ProtoMesh item(PackedMesh(mesh({{{-5,-5,-5},{5,5,5}}}),PackedMesh::PK_Visual),"CONTRACT_ITEM.MRM");
    mechanics(table,item,true);
    mechanics(table,item,false);
    toggleAndSave(table);
    std::puts("Furniture parts contracts passed");
    return EXIT_SUCCESS;
    }
  catch(const std::exception& error) {
    std::fprintf(stderr,"FAIL: furniture parts contract: %s\n",error.what());
    return EXIT_FAILURE;
    }
  }
