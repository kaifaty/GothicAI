#pragma once

// Synthetic world meshes, world.json helpers and edited chunk GLBs shared by
// the chunk contracts (worldbase_contract.cpp, wcb_contract.cpp,
// wcb_source_contract.cpp).

#include "asset/worldmesh.h"
#include "gothicglb.h"
#include "worldchunkbinary.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

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

// world.json object entries: a draw mesh and a cell with its meshes.
inline std::string drawMesh(const std::string& file, const std::string& level="0") {
  return "{\"file\":\""+file+"\",\"role\":\"draw\",\"level\":"+level+"}";
  }

inline std::string cellEntry(const std::string& id, const std::string& meshes) {
  return "{\"cell\":"+id+",\"meshes\":["+meshes+"]}";
  }

// A strip of 12 quads over cells 0,0 / 1,0 / 2,0: columns every 16 m, two rows.
inline uint32_t stripVertex(uint32_t column, uint32_t row) {
  return column*2+row;
  }

inline asset::WorldMesh stripMesh() {
  asset::WorldMesh mesh;
  mesh.materials.resize(2);
  mesh.materials[0].name="STRIP_GROUND";
  mesh.materials[0].group=asset::MaterialGroup::EARTH;
  mesh.materials[0].texture="STRIP_GROUND.TGA";
  mesh.materials[1].name="STRIP_ROCK";
  mesh.materials[1].group=asset::MaterialGroup::STONE;
  mesh.materials[1].detail_object="STRIP_DETAIL";
  for(uint32_t column=0;column<=12;++column)
    for(uint32_t row=0;row<2;++row) {
      mesh.vertices.push_back({float(column)*1600.f,float(column)+float(row)*7.f,float(row)*1600.f+100.f});
      mesh.features.push_back({{float(column),float(row)},0xFF000000u|(column*0x010203u),{0.f,1.f,-0.f}});
      }
  // Quad 4, the first of cell 1 at the border to cell 0, holds the lowest orders.
  for(const uint32_t quad:{4u,0u,1u,2u,3u,5u,6u,7u,8u,9u,10u,11u}) {
    const std::array<std::array<uint32_t,3>,2> triangles = {{
      {stripVertex(quad,0),stripVertex(quad+1,0),stripVertex(quad+1,1)},
      {stripVertex(quad,0),stripVertex(quad+1,1),stripVertex(quad,1)}}};
    for(const auto& triangle:triangles) {
      mesh.polygons.vertex_indices.insert(mesh.polygons.vertex_indices.end(),triangle.begin(),triangle.end());
      mesh.polygons.feature_indices.insert(mesh.polygons.feature_indices.end(),triangle.begin(),triangle.end());
      mesh.polygons.material_indices.push_back(quad%3==0 ? 1u : 0u);
      mesh.polygons.lightmap_indices.push_back(quad%2==0 ? -1 : int32_t(quad));
      mesh.polygons.flags.push_back(wcb::unpackFlags(uint16_t(0x208u|(quad&1u)),int16_t(int32_t(quad)-6)));
      }
    }
  mesh.vertices.push_back({1.f,2.f,3.f}); // referenced by nothing: unreferenced.bin
  mesh.features.push_back({{0.5f,0.5f},0x80FFFFFFu,{1.f,0.f,0.f}});
  return mesh;
  }

struct GlbCorner {
  asset::Vec3          position;
  asset::VertexFeature feature;
  float                vertexId, featureId; // -1 for geometry an edit added
  };

struct GlbPrimitive {
  asset::Material                      material;
  int64_t                              index = -1; // source material index; -1 for a new material
  std::vector<std::array<GlbCorner,3>> triangles;
  std::vector<float>                   order;      // empty: no order, as for geometry an edit added
  std::vector<uint16_t>                flags;      // two per triangle
  std::vector<float>                   lightmap;
  };

// A chunk GLB as an edit writes it: non-indexed primitives, the converter's 0.01 node scale.
inline void writeChunkGlb(const std::filesystem::path& path, const std::vector<GlbPrimitive>& primitives) {
  gothicglb::GlbBuilder glb;
  std::string materials="[", list="[";
  for(size_t i=0;i<primitives.size();++i) {
    const auto& p=primitives[i];
    std::vector<float> position,normal,uv,vid,fid;
    std::vector<uint8_t> color;
    std::array<float,3> lo,hi;
    lo.fill(std::numeric_limits<float>::infinity());
    hi.fill(-std::numeric_limits<float>::infinity());
    for(const auto& triangle:p.triangles)
      for(const auto& c:triangle) {
        const std::array<float,3> stored={-c.position.x,c.position.y,c.position.z};
        for(unsigned axis=0;axis<3;++axis) {
          position.push_back(stored[axis]);
          lo[axis]=std::min(lo[axis],stored[axis]);
          hi[axis]=std::max(hi[axis],stored[axis]);
          }
        normal.insert(normal.end(),{-c.feature.normal.x,c.feature.normal.y,c.feature.normal.z});
        uv.insert(uv.end(),{c.feature.texture.x,c.feature.texture.y});
        const auto light=c.feature.light;
        color.insert(color.end(),{uint8_t(light>>16),uint8_t(light>>8),uint8_t(light),uint8_t(light>>24)});
        vid.push_back(c.vertexId);
        fid.push_back(c.featureId);
        }
    const auto minMax=",\"min\":["+gothicglb::jsonFloat(lo[0])+","+gothicglb::jsonFloat(lo[1])+","+gothicglb::jsonFloat(lo[2])+
                      "],\"max\":["+gothicglb::jsonFloat(hi[0])+","+gothicglb::jsonFloat(hi[1])+","+gothicglb::jsonFloat(hi[2])+"]";
    const auto aPos=glb.accessor(position,5126,"VEC3",3,false,34962,minMax);
    const auto aNrm=glb.accessor(normal,5126,"VEC3",3,false,34962);
    const auto aUv =glb.accessor(uv,5126,"VEC2",2,false,34962);
    const auto aCol=glb.accessor(color,5121,"VEC4",4,true,34962);
    const auto aVid=glb.accessor(vid,5126,"SCALAR",1,false,34962);
    const auto aFid=glb.accessor(fid,5126,"SCALAR",1,false,34962);
    std::string extras="{\"flags\":"+std::to_string(glb.accessor(p.flags,5123,"VEC2",2,false,0))+
                       ",\"lightmap\":"+std::to_string(glb.accessor(p.lightmap,5126,"SCALAR",1,false,0));
    if(!p.order.empty())
      extras+=",\"order\":"+std::to_string(glb.accessor(p.order,5126,"SCALAR",1,false,0));
    extras+="}";
    auto record=gothicglb::materialJson(p.material,p.index<0 ? 0 : size_t(p.index));
    if(p.index<0)
      record="{"+record.substr(record.find(',')+1); // no "index": a material the edit added
    const std::string comma=i==0 ? "" : ",";
    materials+=comma+"{\"name\":"+gothicglb::jsonString(p.material.name)+",\"extras\":{\"gothic\":"+record+"}}";
    list+=comma+"{\"attributes\":{\"POSITION\":"+std::to_string(aPos)+",\"NORMAL\":"+std::to_string(aNrm)+
          ",\"TEXCOORD_0\":"+std::to_string(aUv)+",\"COLOR_0\":"+std::to_string(aCol)+",\"_VERTEX\":"+std::to_string(aVid)+
          ",\"_FEATURE\":"+std::to_string(aFid)+"},\"material\":"+std::to_string(i)+",\"mode\":4,\"extras\":{\"gothic\":"+
          extras+"}}";
    }
  const auto json="{\"asset\":{\"version\":\"2.0\",\"generator\":\"gothic-wcb-contract\"},\"scene\":0,"
                  "\"scenes\":[{\"nodes\":[0]}],\"nodes\":[{\"name\":\"edit\",\"mesh\":0,\"scale\":[0.01,0.01,0.01]}],"
                  "\"meshes\":[{\"primitives\":"+list+"]}],\"materials\":"+materials+"],\"accessors\":"+glb.accessorsJson()+
                  ",\"bufferViews\":"+glb.viewsJson()+",\"buffers\":[{\"byteLength\":"+std::to_string(glb.bin.size())+"}]}";
  const auto bytes=glb.finish(json);
  gothicglb::writeFile(path,bytes.data(),bytes.size());
  }

inline ChunkId centroidCell(const asset::WorldMesh& mesh, size_t triangle) {
  double x=0, z=0;
  for(unsigned c=0;c<3;++c) {
    const auto& v=mesh.vertices[mesh.polygons.vertex_indices[triangle*3+c]];
    x+=v.x;
    z+=v.z;
    }
  return ChunkId::fromPosition(float(x/3.),float(z/3.));
  }

// Cell 1,0 edited: an inner vertex moved, its last source triangle (order 15)
// replaced by two around a new vertex with a material the edit added, and,
// with moveBorder, the vertex it shares with cell 0,0 moved too, which splits it.
// With cell 2,0 the edit replaces its source triangle 20 the same way, with
// another new vertex, feature and material.
inline std::filesystem::path writeStripOverride(const std::filesystem::path& over, const asset::WorldMesh& base,
                                                bool moveBorder, ChunkId cell={1,0}) {
  std::filesystem::create_directories(over/"chunks");
  const bool second=cell==ChunkId{2,0};
  const auto& p=base.polygons;
  auto corner=[&](size_t t, unsigned c) {
    const auto v=p.vertex_indices[t*3+c], f=p.feature_indices[t*3+c];
    GlbCorner r {base.vertices[v],base.features[f],float(v),float(f)};
    if(v==stripVertex(6,1))
      r.position.y+=50.f;
    if(moveBorder && v==stripVertex(4,0))
      r.position.y+=30.f;
    return r;
    };
  std::vector<GlbPrimitive> primitives(3);
  for(uint32_t m=0;m<2;++m) {
    primitives[m].material=base.materials[m];
    primitives[m].index=m;
    }
  const size_t dropped=second ? 20 : 15;
  for(size_t t=0;t<p.material_indices.size();++t) {
    if(!(centroidCell(base,t)==cell) || t==dropped)
      continue;
    auto& primitive=primitives[p.material_indices[t]];
    primitive.triangles.push_back({corner(t,0),corner(t,1),corner(t,2)});
    primitive.order.push_back(float(t));
    primitive.flags.push_back(wcb::packFlags(p.flags[t]));
    primitive.flags.push_back(uint16_t(p.flags[t].sector_index));
    primitive.lightmap.push_back(float(p.lightmap_indices[t]));
    }
  auto& added=primitives[2];
  added.material=base.materials[0];
  added.material.name=second ? "STRIP_EDIT_2" : "STRIP_EDIT";
  added.material.group=second ? asset::MaterialGroup::METAL : asset::MaterialGroup::WOOD;
  const auto a=corner(dropped,0), b=corner(dropped,1), c=corner(dropped,2);
  GlbCorner middle {{(a.position.x+b.position.x)*0.5f,(a.position.y+b.position.y)*0.5f+20.f,(a.position.z+b.position.z)*0.5f},
                    {{0.25f,0.75f},second ? 0xFF608040u : 0xFF406080u,{0.f,0.8f,0.6f}},-1.f,-1.f};
  added.triangles={{a,middle,c},{middle,b,c}};
  added.flags={0,0,0,0};
  added.lightmap={-1.f,-1.f};
  writeChunkGlb(over/("chunks/"+std::to_string(cell.x)+"_"+std::to_string(cell.z)+".glb"),primitives);
  return over;
  }

}
