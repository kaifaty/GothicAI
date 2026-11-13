#pragma once

// The static collision of a DynamicWorld, probed through what reads it, for the contracts that compare two worlds
// built from the same geometry (worldcollision_chunks_contract.cpp: the stitched landscape against the collision
// mesh; collisioncache_cpu_contract.cpp: a cold start against a warm one and the cache off):
//   - the native navigation geometry (DynamicWorld::navigationGeometry, byte for byte);
//   - a 64 x 64 grid of vertical land and water rays over the chunk table's bounds, then short vertical land rays
//     through every 16th roughly level triangle of a named sector material (a ':' in the name): hit, point and
//     normal bit for bit, material, sector;
//   - PhysicVbo's queries on the land and water meshes over a sample of triangles and the first ray duplicates:
//     triangleVertices, materialId, sectorName, hasRayDuplicates, and resolveRayTriangle for a ray through the
//     triangle's centre along its normal.
// Also the collision contract's fixture world with disable_collision, water and two named sectors, and the sources
// of its fixtures.

#include "physics/collisionworld.h"
#include "physics/dynamicworld.h"
#include "physics/physicvbo.h"
#include "worldchunks.h"
#include "worldchunksruntime.h"
#include "worldcloth_fixtures.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace WorldChunkFixtures {

inline const WorldChunkSource stripSource     = {"CONTRACT.ZEN",3,0x00C0FFEEu};
inline const WorldChunkSource collisionSource = {"COLLISION_CONTRACT.ZEN",13,0x0C0111DEu};

// 8 x 4 quads of 32 m over cells -1..2 x 0..1, each of one of five materials in turn — ground, a material with
// disable_collision, water and two named sector materials (a ':' in the name, the same group) — so each lies in
// several cells; a city banner across the border of cells 0,0 and 1,0 goes to cloth; one position is unused.
inline asset::WorldMesh collisionWorld() {
  asset::WorldMesh mesh;
  const uint32_t materials[5] = {addMaterial(mesh,"COLLISION_GROUND","GROUND.TGA"),
    addMaterial(mesh,"COLLISION_FOLIAGE","FOLIAGE.TGA"),addMaterial(mesh,"COLLISION_LAKE","LAKE.TGA"),
    addMaterial(mesh,"SECTOR:HALL_A","HALL_A.TGA"),addMaterial(mesh,"SECTOR:HALL_B","HALL_B.TGA")};
  mesh.materials[materials[0]].group = asset::MaterialGroup::EARTH;
  mesh.materials[materials[1]].disable_collision = true;
  mesh.materials[materials[2]].group = asset::MaterialGroup::WATER;
  mesh.materials[materials[3]].group = asset::MaterialGroup::STONE;
  mesh.materials[materials[4]].group = asset::MaterialGroup::STONE;
  const auto base = uint32_t(mesh.vertices.size());
  for(uint32_t row=0;row<=4;++row)
    for(uint32_t col=0;col<=8;++col)
      mesh.vertices.push_back({-6400.f+float(col)*3200.f,float(row*7+col*3),float(row)*3200.f});
  for(uint32_t row=0;row<4;++row)
    for(uint32_t col=0;col<8;++col) {
      const auto material = materials[(row+col)%5];
      const auto a = base+row*9+col, b = a+1, c = a+9, d = c+1;
      addTriangle(mesh,material,{a,b,d});
      addTriangle(mesh,material,{a,d,c});
      }
  addBanner(mesh,addMaterial(mesh,"NW_CITY_FLAG_01","SYNTHETIC_CITY.TGA"),6300);
  mesh.vertices.push_back({1.f,2.f,3.f}); // referenced by nothing: unreferenced.bin
  return mesh;
  }

}

// A friend of DynamicWorld: the rays go straight to its collision world with one category mask each, the queries
// to its static meshes.
struct StaticCollisionProbe {
  struct Ray {
    bool                 hit = false;
    uint32_t             point[3] = {}, normal[3] = {};
    asset::MaterialGroup material = asset::MaterialGroup::UNDEFINED;
    std::string          sector;
    bool operator==(const Ray&) const = default;
    };
  struct Query {
    uint32_t             mesh = 0, triangle = 0; // mesh: 0 land, 1 water
    bool                 valid = false, named = false, duplicates = false;
    uint32_t             vertices[9] = {};
    asset::MaterialGroup material = asset::MaterialGroup::UNDEFINED;
    std::string          sector;
    uint32_t             resolved = 0;
    bool operator==(const Query&) const = default;
    };
  struct Result {
    NavMesh::Geometry  navigation;
    std::vector<Ray>   rays;
    size_t             grid = 0;       // the grid's rays come first
    std::vector<Query> queries;
    size_t             duplicates = 0; // queries of triangles with ray duplicates
    size_t             triangles[2] = {}; // of the land and the water mesh
    double             buildMs = 0;
    };

  static void require(bool condition, const std::string& message) {
    if(!condition)
      throw std::runtime_error(message);
    }

  static uint32_t bits(float v) { return std::bit_cast<uint32_t>(v); }

  static bool sameBytes(const auto& a, const auto& b) {
    return a.size()==b.size() && (a.empty() || std::memcmp(a.data(),b.data(),a.size()*sizeof(a[0]))==0);
    }

  static bool sameNavigation(const NavMesh::Geometry& a, const NavMesh::Geometry& b) {
    return sameBytes(a.vertices,b.vertices) && sameBytes(a.triangles,b.triangles) &&
           sameBytes(a.waterVertices,b.waterVertices) && sameBytes(a.waterTriangles,b.waterTriangles);
    }

  // A DynamicWorld without a cache from mesh, as the tools build one, timed.
  static Result probe(const asset::WorldMesh& mesh, const asset::WorldMesh& original, const ChunkTable& table) {
    const auto started = std::chrono::steady_clock::now();
    const DynamicWorld physics(nullptr,mesh,&original);
    const auto built = std::chrono::steady_clock::now();
    auto out = probe(physics,mesh,table);
    out.buildMs = std::chrono::duration<double,std::milli>(built-started).count();
    return out;
    }

  // mesh: the input the world was built from, or would be: it picks the named sector triangles.
  static Result probe(const DynamicWorld& physics, const asset::WorldMesh& mesh, const ChunkTable& table) {
    Result out;
    out.navigation = physics.navigationGeometry();
    const float infinity = std::numeric_limits<float>::infinity();
    asset::Vec3 lo {infinity,infinity,infinity}, hi {-infinity,-infinity,-infinity};
    for(const auto& entry:table.entries)
      if(entry.triangleCount!=0)
        for(unsigned axis=0;axis<3;++axis) {
          lo[axis] = std::min(lo[axis],entry.min[axis]);
          hi[axis] = std::max(hi[axis],entry.max[axis]);
          }
    require(lo.x<hi.x && lo.z<hi.z,"the ray grid needs the island's bounds");
    auto cast = [&](const Tempest::Vec3& from, const Tempest::Vec3& to, uint32_t mask) {
      const auto hit = physics.world->rayCast(from,to,mask);
      Ray ray;
      ray.hit = hit.hasHit;
      const float point[3] = {hit.point.x,hit.point.y,hit.point.z}, normal[3] = {hit.normal.x,hit.normal.y,hit.normal.z};
      for(unsigned axis=0;axis<3;++axis) {
        ray.point[axis]  = bits(point[axis]);
        ray.normal[axis] = bits(normal[axis]);
        }
      ray.material = hit.material;
      ray.sector   = hit.sector==nullptr ? "" : hit.sector;
      out.rays.push_back(std::move(ray));
      };
    // The grid: land and water rays in turn.
    constexpr int side = 64;
    for(int i=0;i<side;++i)
      for(int k=0;k<side;++k) {
        const float x = lo.x+(hi.x-lo.x)*(float(i)+.5f)/float(side), z = lo.z+(hi.z-lo.z)*(float(k)+.5f)/float(side);
        cast({x,hi.y+1000.f,z},{x,lo.y-1000.f,z},1u<<DynamicWorld::C_Landscape);
        cast({x,hi.y+1000.f,z},{x,lo.y-1000.f,z},1u<<DynamicWorld::C_Water);
        }
    // Then short vertical land rays through every 16th roughly level triangle of a named sector, from the same input.
    out.grid = out.rays.size();
    const auto& p = mesh.polygons;
    for(size_t t=0,named=0;t<p.material_indices.size();++t) {
      const auto& material = mesh.materials[p.material_indices[t]];
      if(material.disable_collision || material.name.find(':')==std::string::npos || named++%16!=0)
        continue;
      Tempest::Vec3 v[3];
      for(unsigned k=0;k<3;++k) {
        const auto& x = mesh.vertices[p.vertex_indices[t*3+k]];
        v[k] = {x.x,x.y,x.z};
        }
      const auto n = Tempest::Vec3::crossProduct(v[1]-v[0],v[2]-v[0]);
      if(std::abs(n.y)*2.f<n.length())
        continue;
      const auto c = (v[0]+v[1]+v[2])/3.f;
      cast(c+Tempest::Vec3(0,10,0),c-Tempest::Vec3(0,10,0),1u<<DynamicWorld::C_Landscape);
      }
    const PhysicVbo* meshes[2] = {physics.statics->land.get(),physics.statics->water.get()};
    for(uint32_t m=0;m<2;++m) {
      out.triangles[m] = meshes[m]->triangleCount();
      query(*meshes[m],m,out);
      }
    return out;
    }

  // Every k-th triangle (at most 4096), then the first 4096 triangles with ray duplicates.
  static void query(const PhysicVbo& mesh, uint32_t index, Result& out) {
    const auto triangles = mesh.triangleCount();
    const size_t step = std::max<size_t>(1,triangles/4096);
    std::vector<uint32_t> picked;
    for(size_t t=0;t<triangles;t+=step)
      picked.push_back(uint32_t(t));
    for(size_t t=0,found=0;t<triangles && found<4096;++t)
      if(mesh.hasRayDuplicates(uint32_t(t))) {
        picked.push_back(uint32_t(t));
        ++found;
        }
    for(const auto t:picked) {
      Query q;
      q.mesh = index;
      q.triangle = t;
      JPH::Vec3 v[3];
      q.valid = mesh.triangleVertices(t,v);
      for(unsigned k=0;q.valid && k<3;++k)
        for(unsigned axis=0;axis<3;++axis)
          q.vertices[3*k+axis] = bits(v[k][axis]);
      q.material = mesh.materialId(t);
      const char* sector = mesh.sectorName(t);
      q.named = sector!=nullptr;
      q.sector = sector==nullptr ? "" : sector;
      q.duplicates = mesh.hasRayDuplicates(t);
      out.duplicates += q.duplicates;
      JPH::Vec3 normal;
      if(q.valid && mesh.rayTriangleNormal(t,normal)) {
        const auto centre = (v[0]+v[1]+v[2])/3.f;
        q.resolved = mesh.resolveRayTriangle(t,centre+normal*.5f,centre-normal*.5f);
        }
      out.queries.push_back(std::move(q));
      }
    }
  };
