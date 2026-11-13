#pragma once

// Synthetic world meshes and world.json helpers shared by the chunk contracts
// (worldbase_contract.cpp, wcb_contract.cpp).

#include "asset/worldmesh.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>

namespace WorldChunkFixtures {

// One triangle in cell 0,0.
inline asset::WorldMesh selectionMesh() {
  asset::WorldMesh mesh;
  mesh.materials.resize(1);
  mesh.materials[0].name="CONTRACT";
  mesh.materials[0].group=asset::MaterialGroup::STONE;
  mesh.vertices={{100,100,100},{100,200,100},{200,100,100}};
  mesh.features={{{0,0},0xffffffffu,{0,0,1}},{{0,1},0xffffffffu,{0,0,1}},{{1,0},0xffffffffu,{0,0,1}}};
  mesh.polygons.vertex_indices={0,1,2};
  mesh.polygons.feature_indices={0,1,2};
  mesh.polygons.material_indices={0};
  mesh.polygons.lightmap_indices={-1};
  mesh.polygons.flags.resize(1);
  return mesh;
  }

// Eight triangles over four cells, negative ones and overhangs included: an
// equivalent material alias, a distinct material and four cloth triangles.
inline asset::WorldMesh chunkFixtureMesh() {
  auto mesh=selectionMesh();
  mesh.materials.push_back(mesh.materials[0]);
  mesh.materials[1].name="EQUIVALENT_ALIAS";
  mesh.materials.push_back(mesh.materials[0]);
  mesh.materials[2].name="DISTINCT_MATERIAL";
  mesh.materials[2].group=asset::MaterialGroup::WOOD;
  mesh.vertices.insert(mesh.vertices.end(),{{-6401,0,-100},{-6401,100,-100},{-6398,0,200},
                                          {-100,10,100},{6500,20,100},{200,30,200},
                                          {6500,0,-100},{6500,100,-100},{6600,0,-200}});
  for(uint32_t triangle=1;triangle<4;++triangle) {
    mesh.polygons.vertex_indices.insert(mesh.polygons.vertex_indices.end(),{triangle*3,triangle*3+1,triangle*3+2});
    mesh.polygons.feature_indices.insert(mesh.polygons.feature_indices.end(),{0,1,2});
    mesh.polygons.material_indices.push_back(triangle==2 ? 2u : 1u);
    mesh.polygons.lightmap_indices.push_back(-1);
    mesh.polygons.flags.emplace_back();
    }
  mesh.materials.push_back(mesh.materials[0]);
  mesh.materials[3].name="NW_CITY_FLAG_01";
  mesh.vertices.insert(mesh.vertices.end(),{{1200,600,100},{1400,600,100},{1400,300,100},{1200,300,100}});
  for(const auto& triangle:std::array<std::array<uint32_t,3>,4>{{{12,13,14},{12,14,15},{12,14,13},{12,15,14}}}) {
    mesh.polygons.vertex_indices.insert(mesh.polygons.vertex_indices.end(),triangle.begin(),triangle.end());
    mesh.polygons.feature_indices.insert(mesh.polygons.feature_indices.end(),{0,1,2});
    mesh.polygons.material_indices.push_back(3);
    mesh.polygons.lightmap_indices.push_back(-1);
    mesh.polygons.flags.emplace_back();
    }
  return mesh;
  }

inline std::string readText(const std::filesystem::path& path) {
  std::ifstream file(path,std::ios::binary);
  if(!file)
    throw std::runtime_error("cannot read "+path.string());
  return {std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>()};
  }

inline void writeText(const std::filesystem::path& path, const std::string& text) {
  std::ofstream file(path,std::ios::binary|std::ios::trunc);
  file << text;
  if(!file)
    throw std::runtime_error("cannot write "+path.string());
  }

// The writer's world.json with the body of its "chunks" array replaced.
inline std::string withChunks(const std::string& json, const std::string& list) {
  const std::string key="\"chunks\": [";
  const auto begin=json.find(key);
  const auto end=begin==std::string::npos ? begin : json.find(']',begin);
  if(end==std::string::npos)
    throw std::runtime_error("world.json must list its chunks");
  return json.substr(0,begin+key.size())+list+json.substr(end);
  }

}
