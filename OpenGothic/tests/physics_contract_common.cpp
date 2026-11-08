#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#include "physics_contract_common.h"
#include "game/serialize.h"

#include <Tempest/MemReader>
#include <Tempest/MemWriter>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace PhysicsContract {
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

  asset::WorldMesh waterBasin(float floorY, bool cave) {
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
      const std::vector<DynamicWorld::WaterBodySnapshot>& bodies,uint32_t zenId,size_t piece) {
    const auto found=std::find_if(bodies.begin(),bodies.end(),[&](const auto& body) {
      return body.zenId==zenId && body.piece==piece;
      });
    require(found!=bodies.end(),"water snapshots must retain each native body's parent and piece identity");
    return *found;
    }
  }
