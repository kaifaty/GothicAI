#include "physics/dynamicworld.h"
#include "graphics/bounds.h"
#include "graphics/mesh/protomesh.h"
#include "graphics/mesh/submesh/packedmesh.h"
#include "resources.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <vector>

namespace {

void require(bool condition, const char* message) {
  if(!condition)
    throw std::runtime_error(message);
  }

Tempest::Matrix4x4 at(float x, float y, float z = 0) {
  auto result = Tempest::Matrix4x4::mkIdentity();
  result.translate(x,y,z);
  return result;
  }

asset::StaticMesh box(const Tempest::Vec3& size, bool collision = false) {
  asset::StaticMesh result;
  result.positions = {{-10,-10,-10},{10,-10,-10},{10,10,-10},{-10,10,-10},
                      {-10,-10,10},{10,-10,10},{10,10,10},{-10,10,10}};
  for(auto& point:result.positions) {
    point.x *= size.x/20.f;
    point.y *= size.y/20.f;
    point.z *= size.z/20.f;
    }
  result.bbox = {{-.5f*size.x,-.5f*size.y,-.5f*size.z},{.5f*size.x,.5f*size.y,.5f*size.z}};
  result.sub_meshes.resize(1);
  auto& part = result.sub_meshes[0];
  part.mat.group = asset::MaterialGroup::WOOD;
  for(size_t i=0;i<result.positions.size();++i)
    part.wedges.push_back({{0,1,0},{0,0},uint16_t(i)});
  part.triangles = {{{0,2,1}},{{0,3,2}},{{4,5,6}},{{4,6,7}},
                    {{0,4,7}},{{0,7,3}},{{1,2,6}},{{1,6,5}},
                    {{0,1,5}},{{0,5,4}},{{3,7,6}},{{3,6,2}}};
  // PhysicVbo reverses Gothic source winding for static triangle collisions.
  if(collision)
    for(auto& face:part.triangles)
      std::swap(face.wedges[1],face.wedges[2]);
  return result;
  }

asset::WorldMesh floor() {
  asset::WorldMesh result;
  result.vertices = {{-2000,0,-2000},{-2000,0,2000},{2000,0,2000},{2000,0,-2000}};
  result.materials.resize(1);
  result.materials[0].group = asset::MaterialGroup::STONE;
  result.polygons.vertex_indices = {0,2,1,0,3,2};
  result.polygons.material_indices = {0,0};
  return result;
  }

void census() {
  Resources resources;
  constexpr const char* visual = "CONTRACT_PLACED_ITEM.MRM";
  ProtoMesh item(PackedMesh(box({20,20,20}),PackedMesh::PK_Visual),visual);
  ProtoMesh wall(PackedMesh(box({20,200,200},true),PackedMesh::PK_Visual),"CONTRACT_PLACED_WALL.MRM");
  Bounds bounds;
  bounds.assign(item.bboxMesh);
  DynamicWorld world(nullptr,floor());
  auto structure = world.addStaticObj(wall.attach[0].shape.get(),at(415,10),"CONTRACT_PLACED_WALL.MRM");
  auto furniture = world.addMovableVob(item,visual,at(600,10),.25f,false,"utensil",true,901);
  world.finalizeMovableVobs();
  require(!structure.isEmpty() && furniture.mass()>0,"census fixtures must retain static and movable collisions");

  const std::array<Tempest::Matrix4x4,9> origins = {{at(0,11),at(100,14),at(200,20),at(300,5),
    at(400,11),at(615,11),at(800,11),at(815,11),at(1000,50)}};
  std::vector<DynamicWorld::Item> bodies;
  bodies.reserve(origins.size());
  for(const auto& origin:origins) {
    bodies.push_back(world.addDynamicObj(origin,bounds,asset::MaterialGroup::WOOD,&item,visual,true));
    require(!bodies.back().isEmpty(),"placed candidates must use native item collision hulls");
    bodies.back().restorePhysics(origin,true);
    }
  std::vector<DynamicWorld::PlacedItemCandidate> candidates;
  for(size_t i=0;i<bodies.size();++i)
    candidates.push_back({&bodies[i],i==8});
  const auto result = world.placedItemCensus(candidates);
  require(result.size()==origins.size(),"census must preserve one decision per source item");
  const char* expected[] = {"ok","ok","hangs","static","static","furniture","neighbour","neighbour","quest"};
  for(size_t i=0;i<result.size();++i) {
    std::printf("candidate %zu: %s gap_cm=%.6f static_cm=%.6f furniture_cm=%.6f item_cm=%.6f\n",i,
                result[i].disposition.c_str(),double(result[i].supportGapCm),double(result[i].staticDepthCm),
                double(result[i].furnitureDepthCm),double(result[i].itemDepthCm));
    require(result[i].disposition==expected[i],"native census must classify hanging, embedded, pair, mission and admitted items");
    require(result[i].enabled==(i<2),"only admitted census candidates may receive resident bodies");
    require((bodies[i].position()-Tempest::Vec3(origins[i].at(3,0),origins[i].at(3,1),origins[i].at(3,2))).length()<.001f,
            "census must not move or simulate any candidate");
    require(bodies[i].isSleeping(),"census must not wake candidate bodies");
    }
  require(std::abs(result[1].supportGapCm-3.f)<.01f,"support gap must be measured from the padded native item hull");
  require(result[3].staticDepthCm>4.f && result[4].staticDepthCm>4.f,
          "both landscape and C_Object penetration must exclude items");
  require(result[5].furnitureDepthCm>4.f,"movable furniture penetration must exclude items");
  require(result[6].itemDepthCm>4.f && result[7].itemDepthCm>4.f,
          "deep neighbour pairs must exclude both original items");
  std::reverse(candidates.begin(),candidates.end());
  const auto reversed = world.placedItemCensus(candidates);
  for(size_t i=0;i<result.size();++i)
    require(result[i].disposition==reversed[result.size()-1-i].disposition,
            "simultaneous census decisions must not depend on candidate iteration order");
  }

}

int main() {
  try {
    census();
    std::puts("Placed item census contracts passed");
    return EXIT_SUCCESS;
    }
  catch(const std::exception& error) {
    std::fprintf(stderr,"FAIL: placed item census contract: %s\n",error.what());
    return EXIT_FAILURE;
    }
  }
