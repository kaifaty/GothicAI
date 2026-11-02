#include "asset/fromzenkit.h"
#include <zenkit/World.hh>
#include "commandline.h"
#include "graphics/mesh/submesh/packedmesh.h"
#include "physics/collisionworld.h"
#include "physics/dynamicworld.h"
#include "physics/navmesh.h"
#include "physics/physicvbo.h"
#include "physics/worldcloth.h"
#include "resources.h"
#include "worldbase.h"
#include "worldchunksruntime.h"
#include "worlddata.h"
#include "world/chunkresidency.h"
#include "world/npcsimtier.h"
#include "world/portalrooms.h"

#include <zenkit/Stream.hh>
#include <zenkit/Vfs.hh>

#include <array>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <stdexcept>
#include <string>

namespace WorldBasePhysicsContract {

inline void require(bool condition, const std::string& message) {
  if(!condition)
    throw std::runtime_error(message);
  }

inline std::array<float,9> packedTriangle(const PackedMesh& mesh, size_t triangle) {
  std::array<float,9> result;
  for(size_t vertex=0;vertex<3;++vertex) {
    const auto& point=mesh.vertices.at(mesh.indices.at(triangle*3+vertex));
    for(size_t axis=0;axis<3;++axis)
      result[vertex*3+axis]=point.pos[axis];
    }
  return result;
  }

inline void comparePacked(const PackedMesh& zen, const PackedMesh& base) {
  require(zen.subMeshes.size()==base.subMeshes.size(),"packPhysics submesh count differs");
  for(size_t sub=0;sub<zen.subMeshes.size();++sub) {
    const auto& a=zen.subMeshes[sub];
    const auto& b=base.subMeshes[sub];
    const auto where="packPhysics submesh "+std::to_string(sub)+" material '"+a.material.name+"': ";
    require(a.material.name==b.material.name && a.material.group==b.material.group &&
            a.material.disable_collision==b.material.disable_collision,where+"material differs");
    require(a.iboOffset==b.iboOffset && a.iboLength==b.iboLength,where+"index range differs");
    }
  require(zen.indices.size()==base.indices.size(),"packPhysics index count differs");
  // Compare the positions addressed by each packed triangle, preserving its
  // order and winding in the stream passed to Jolt.
  for(size_t triangle=0;triangle<zen.indices.size()/3;++triangle)
    require(packedTriangle(zen,triangle)==packedTriangle(base,triangle),
            "packPhysics dereferenced triangle "+std::to_string(triangle)+" differs");
  }

struct SplitPhysics {
  std::vector<JPH::Vec3> vertices;
  std::vector<std::string> sectors;
  PhysicVbo land{&vertices};
  PhysicVbo water{&vertices};

  explicit SplitPhysics(const PackedMesh& packed) {
    vertices.reserve(packed.vertices.size());
    for(const auto& vertex:packed.vertices)
      vertices.push_back(CollisionWorld::toMeters(Tempest::Vec3(vertex.pos[0],vertex.pos[1],vertex.pos[2])));
    sectors.reserve(packed.subMeshes.size());
    for(const auto& sub:packed.subMeshes)
      sectors.push_back(sub.material.name);
    // Keep the same collision filter, water split and sector lifetime as
    // DynamicWorld's landscape construction.
    for(size_t sub=0;sub<packed.subMeshes.size();++sub) {
      const auto& mesh=packed.subMeshes[sub];
      if(mesh.material.disable_collision || mesh.iboLength==0)
        continue;
      if(mesh.material.group==asset::MaterialGroup::WATER)
        water.addIndex(packed.indices,mesh.iboOffset,mesh.iboLength,mesh.material.group);
      else
        land.addIndex(packed.indices,mesh.iboOffset,mesh.iboLength,mesh.material.group,sectors[sub].c_str());
      }
    }
  };

inline void compareInput(const PhysicVbo& zen, const PhysicVbo& base, const std::string& kind) {
  require(zen.triangleCount()==base.triangleCount(),kind+" Jolt input triangle count differs");
  for(size_t triangle=0;triangle<zen.triangleCount();++triangle) {
    const auto sector=[](const PhysicVbo& mesh, size_t index) {
      const auto* name=mesh.sectorName(index);
      return std::string(name==nullptr ? "" : name);
      };
    const auto material=zen.materialId(triangle);
    const auto name=sector(zen,triangle);
    const auto where=kind+" Jolt input triangle "+std::to_string(triangle)+
                     " material "+std::to_string(static_cast<unsigned int>(material))+" sector '"+name+"': ";
    require(material==base.materialId(triangle),where+"material differs");
    require(name==sector(base,triangle),where+"sector differs");
    JPH::Vec3 a[3], b[3];
    require(zen.triangleVertices(static_cast<uint32_t>(triangle),a) &&
            base.triangleVertices(static_cast<uint32_t>(triangle),b),where+"invalid source triangle");
    for(size_t vertex=0;vertex<3;++vertex)
      require(a[vertex]==b[vertex],where+"vertex "+std::to_string(vertex)+" differs");
    }
  }

inline NavMesh::Geometry nativeGeometry(const SplitPhysics& mesh) {
  CollisionWorld world;
  Tempest::Matrix4x4 identity;
  identity.identity();
  std::unique_ptr<CollisionWorld::CollisionBody> land, water;
  if(!mesh.land.isEmpty())
    land=world.addCollisionBody(mesh.land.shape(),identity,0.f,DynamicWorld::C_Landscape,false,false,&mesh.land);
  if(!mesh.water.isEmpty())
    water=world.addCollisionBody(mesh.water.shape(),identity,0.f,DynamicWorld::C_Water,false,true,&mesh.water);
  NavMesh::Geometry result;
  // This is the exact native, quantized triangle stream consumed by
  // DynamicWorld::navigationGeometry(), including compound child transforms.
  world.staticTriangles([&](const CollisionWorld::StaticTriangle& triangle) {
    auto& vertices=triangle.water ? result.waterVertices : result.vertices;
    auto& indices=triangle.water ? result.waterTriangles : result.triangles;
    const auto first=static_cast<int>(vertices.size()/3);
    for(const auto& vertex:triangle.vertices) {
      vertices.push_back(vertex.x);
      vertices.push_back(vertex.y);
      vertices.push_back(vertex.z);
      }
    indices.insert(indices.end(),{first,first+1,first+2});
    });
  return result;
  }

inline void compareNative(const std::vector<float>& zen, const std::vector<float>& base, const std::string& kind) {
  require(zen.size()==base.size(),kind+" native Jolt triangle coordinate count differs: "+
          std::to_string(zen.size())+" vs "+std::to_string(base.size()));
  for(size_t coordinate=0;coordinate<zen.size();++coordinate)
    require(zen[coordinate]==base[coordinate],kind+" native Jolt triangle "+std::to_string(coordinate/9)+
            " vertex "+std::to_string(coordinate%9/3)+" axis "+std::to_string(coordinate%3)+" differs");
  }

inline void compare(const asset::WorldMesh& zen, const asset::WorldMesh& base) {
  Physics::initialize();
  PackedMesh zenPacked(zen,PackedMesh::PK_Physic), basePacked(base,PackedMesh::PK_Physic);
  comparePacked(zenPacked,basePacked);
  SplitPhysics zenInput(zenPacked), baseInput(basePacked);
  compareInput(zenInput.land,baseInput.land,"land");
  compareInput(zenInput.water,baseInput.water,"water");
  const auto zenNative=nativeGeometry(zenInput);
  const auto baseNative=nativeGeometry(baseInput);
  compareNative(zenNative.vertices,baseNative.vertices,"land");
  compareNative(zenNative.waterVertices,baseNative.waterVertices,"water");
  require(zenNative.triangles==baseNative.triangles && zenNative.waterTriangles==baseNative.waterTriangles,
          "native Jolt navigation triangle indices differ");
  require(!zenNative.triangles.empty(),"native Jolt navigation geometry is empty");
  const NavMesh::Params params;
  const auto zenKey=NavMesh::cacheKey(zenNative,params,"NEWWORLD.ZEN");
  const auto baseKey=NavMesh::cacheKey(baseNative,params,"NEWWORLD.ZEN");
  require(zenKey==baseKey,"native Jolt navigation cache key differs: "+
          std::to_string(zenKey)+" vs "+std::to_string(baseKey));
  std::printf("world base physics contract: packed=%zu land=%zu water=%zu native_land=%zu native_water=%zu nav_key=%llu\n",
              zenPacked.indices.size()/3,zenInput.land.triangleCount(),zenInput.water.triangleCount(),
              zenNative.triangles.size()/3,zenNative.waterTriangles.size()/3,
              static_cast<unsigned long long>(zenKey));
  }

} // namespace WorldBasePhysicsContract

inline void worldbasePhysicsContract(const asset::WorldMesh& zen, const asset::WorldMesh& base) {
  WorldBasePhysicsContract::compare(zen,base);
  }

namespace {
namespace fs = std::filesystem;

void require(bool condition, const std::string& message) {
  if(!condition)
    throw std::runtime_error(message);
  }

struct TemporaryDirectory {
  fs::path path = fs::temp_directory_path()/
    ("gothic-worldbase-contract-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  ~TemporaryDirectory() {
    std::error_code error;
    fs::remove_all(path,error);
    }
  };

bool sameFloat(float a, float b) {
  return std::bit_cast<uint32_t>(a)==std::bit_cast<uint32_t>(b);
  }

bool sameVector(const Tempest::Vec3& a, const Tempest::Vec3& b) {
  return sameFloat(a.x,b.x) && sameFloat(a.y,b.y) && sameFloat(a.z,b.z);
  }

bool sameVertex(const Resources::Vertex& a, const Resources::Vertex& b) {
  for(size_t axis=0;axis<3;++axis)
    if(!sameFloat(a.pos[axis],b.pos[axis]) || !sameFloat(a.norm[axis],b.norm[axis]))
      return false;
  return sameFloat(a.uv[0],b.uv[0]) && sameFloat(a.uv[1],b.uv[1]) && a.color==b.color;
  }

bool sameMaterial(const asset::Material& a, const asset::Material& b) {
  asset::WorldMesh left,right;
  left.materials.push_back(a);
  right.materials.push_back(b);
  return compareWorldMeshes(left,right).empty();
  }

void visualContract(const asset::WorldMesh& zen, const asset::WorldMesh& base, const std::string& label) {
  const PackedMesh expected(zen,PackedMesh::PK_VisualLnd),actual(base,PackedMesh::PK_VisualLnd);
  require(expected.subMeshes.size()==actual.subMeshes.size(),label+": GPU submesh count "+
          std::to_string(expected.subMeshes.size())+" vs "+std::to_string(actual.subMeshes.size()));
  for(size_t i=0;i<expected.subMeshes.size();++i) {
    const auto& a=expected.subMeshes[i];
    const auto& b=actual.subMeshes[i];
    const auto where=label+": GPU submesh "+std::to_string(i)+" ("+a.material.name+")";
    require(sameMaterial(a.material,b.material),where+" material differs");
    require(a.iboOffset==b.iboOffset && a.iboLength==b.iboLength,where+" ranges "+
            std::to_string(a.iboOffset)+"/"+std::to_string(a.iboLength)+" vs "+
            std::to_string(b.iboOffset)+"/"+std::to_string(b.iboLength)+
            "; meshlets "+std::to_string(a.iboLength/PackedMesh::MaxInd)+" vs "+
            std::to_string(b.iboLength/PackedMesh::MaxInd)+"; actual material '"+b.material.name+"'");
    require(a.alphaTest==b.alphaTest && a.alphaTestRequired==b.alphaTestRequired,where+" alpha flags differ");
    }
  require(expected.meshletBounds.size()==actual.meshletBounds.size(),label+": GPU meshlet count differs");
  for(size_t i=0;i<expected.meshletBounds.size();++i) {
    const auto& a=expected.meshletBounds[i];
    const auto& b=actual.meshletBounds[i];
    require(sameVector(a.pos,b.pos) && sameFloat(a.r,b.r),label+": GPU meshlet "+std::to_string(i)+" bounds differ");
    }
  require(expected.vertices.size()==actual.vertices.size(),label+": GPU vertex count "+
          std::to_string(expected.vertices.size())+" vs "+std::to_string(actual.vertices.size()));
  require(expected.indices.size()==actual.indices.size(),label+": GPU index count differs");
  require(expected.indices8.size()==actual.indices8.size(),label+": GPU byte index count differs");
  const auto meshlets=expected.meshletBounds.size();
  require(expected.vertices.size()==meshlets*PackedMesh::MaxVert &&
          expected.indices.size()==meshlets*PackedMesh::MaxInd &&
          expected.indices8.size()==meshlets*PackedMesh::MaxPrim*4,label+": incomplete GPU meshlet arrays");
  for(size_t i=0;i<expected.vertices.size();++i)
    require(sameVertex(expected.vertices[i],actual.vertices[i]),label+": GPU meshlet "+
            std::to_string(i/PackedMesh::MaxVert)+" vertex "+std::to_string(i%PackedMesh::MaxVert)+" differs");
  for(size_t i=0;i<expected.indices.size();++i)
    require(expected.indices[i]==actual.indices[i],label+": GPU meshlet "+std::to_string(i/PackedMesh::MaxInd)+
            " triangle "+std::to_string(i%PackedMesh::MaxInd/3)+" corner "+std::to_string(i%3)+
            " index "+std::to_string(expected.indices[i])+" vs "+std::to_string(actual.indices[i]));
  for(size_t i=0;i<expected.indices8.size();++i)
    require(expected.indices8[i]==actual.indices8[i],label+": GPU meshlet "+std::to_string(i/(PackedMesh::MaxPrim*4))+
            " triangle slot "+std::to_string(i%(PackedMesh::MaxPrim*4)/4)+" byte "+std::to_string(i%4)+
            " index or count "+std::to_string(expected.indices8[i])+" vs "+std::to_string(actual.indices8[i]));
  const auto a=expected.bbox(),b=actual.bbox();
  require(sameVector(a.first,b.first) && sameVector(a.second,b.second),label+": GPU bounding box differs");
  std::printf("%s GPU: %zu submeshes, %zu vertices, %zu indices, %zu meshlets equal\n",label.c_str(),
              expected.subMeshes.size(),expected.vertices.size(),expected.indices.size(),expected.meshletBounds.size());
  }

void clothContract(const WorldCloth::Extracted& zen, const WorldCloth::Extracted& base) {
  require(zen.surfaces.size()==base.surfaces.size(),"cloth surface count differs");
  for(size_t i=0;i<zen.surfaces.size();++i) {
    const auto& a=zen.surfaces[i];
    const auto& b=base.surfaces[i];
    const auto where="cloth surface "+std::to_string(i)+" ("+a.visual+")";
    require(a.id==b.id && a.visual==b.visual,where+" id or name differs");
    require(a.pins==b.pins && sameFloat(a.stiffness,b.stiffness) && sameFloat(a.linearDamping,b.linearDamping),
            where+" attachment or physics parameters differ");
    const auto difference=compareWorldMeshes(canonicalWorldMesh(a.mesh),canonicalWorldMesh(b.mesh));
    require(difference.empty(),where+": "+difference);
    }
  const auto difference=compareWorldMeshes(canonicalWorldMesh(zen.original),canonicalWorldMesh(base.original));
  require(difference.empty(),"original cloth triangles: "+difference);
  std::printf("Cloth: %zu surfaces with identical id, names, pins and triangles\n",zen.surfaces.size());
  }

asset::WorldMesh selectionMesh() {
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

asset::WorldMesh expandChunkMesh(const WorldChunkMesh& compact, const asset::WorldMesh& source) {
  require(compact.mesh.vertices.size()==compact.vertexIds.size() && compact.mesh.features.size()==compact.featureIds.size(),
          "compact chunk values must have matching source IDs");
  auto expanded=canonicalWorldMesh(source);
  expanded.materials=compact.mesh.materials;
  expanded.polygons=compact.mesh.polygons;
  for(size_t i=0;i<compact.vertexIds.size();++i)
    expanded.vertices.at(compact.vertexIds[i])=compact.mesh.vertices[i];
  for(size_t i=0;i<compact.featureIds.size();++i)
    expanded.features.at(compact.featureIds[i])=compact.mesh.features[i];
  for(auto& vertex:expanded.polygons.vertex_indices)
    vertex=compact.vertexIds.at(vertex);
  for(auto& feature:expanded.polygons.feature_indices)
    feature=compact.featureIds.at(feature);
  return expanded;
  }

void worldDataContract(const zenkit::World& world, const WorldChunkSource& source, const fs::path& dir) {
  const auto original=WorldData::fromWorld(world,source);
  WorldData::write(original,dir);
  const auto restored=WorldData::read(dir);
  const auto diff=WorldData::compare(original,restored);
  require(diff.empty(),"world data: "+diff);
  PortalRooms::Volumes rooms;
  rooms.reset(static_cast<asset::BspTreeType>(world.world_bsp_tree.mode),asset::roomVolumesFromZenKit(world.world_mesh,world.world_bsp_tree));
  const auto& bounds=world.world_bsp_tree.nodes.empty() ? world.world_mesh.bbox : world.world_bsp_tree.nodes.front().bbox;
  const auto nx=uint64_t(std::ceil(double(bounds.max.x-bounds.min.x)/400.));
  const auto nz=uint64_t(std::ceil(double(bounds.max.z-bounds.min.z)/400.));
  size_t samples=0, indoor=0;
  for(uint64_t x=0;x<=nx;++x) for(uint64_t z=0;z<=nz;++z) for(unsigned h=0;h<3;++h) {
    const zenkit::Vec3 p{bounds.min.x+(bounds.max.x-bounds.min.x)*float(double(x)/double(std::max(uint64_t(1),nx))),
                         bounds.min.y+(bounds.max.y-bounds.min.y)*float(h+1)/4.f,
                         bounds.min.z+(bounds.max.z-bounds.min.z)*float(double(z)/double(std::max(uint64_t(1),nz)))};
    const auto expected=rooms.isIndoor({p.x,p.y,p.z});
    const auto actual=WorldData::isIndoor(restored.bsp.mode,restored.rooms,p);
    require(expected==actual,"PortalRooms isIndoor differs at "+std::to_string(p.x)+","+
            std::to_string(p.y)+","+std::to_string(p.z));
    ++samples; indoor+=expected;
    }
  std::printf("World data: %zu vobs, %zu sectors; PortalRooms %zu grid samples (%zu indoor) equal\n",
              WorldData::vobCount(restored.vobs),restored.bsp.sectors.size(),samples,indoor);
  }

void selectionContract(const fs::path& root) {
  const std::vector<std::byte> bytes={std::byte{1},std::byte{2},std::byte{3}};
  const WorldChunkSource source={"NEWWORLD.ZEN",bytes.size(),worldChunkCrc32(bytes.data(),bytes.size())};
  const auto original=selectionMesh();
  const auto dir=root/"selection";
  const auto manifest=writeWorldChunks(original,source,dir);
  auto changed=original;
  changed.vertices[0].x+=1.f;
  const auto replacement=root/"replacement";
  const auto replacementManifest=writeWorldChunks(changed,source,replacement);
  require(manifest.chunks.size()==1 && replacementManifest.chunks==manifest.chunks,"override fixture must occupy one unchanged chunk");
  const auto overrideDir=root/"override";
  const auto chunk=fs::path(manifest.chunks.front());
  fs::create_directories((overrideDir/chunk).parent_path());
  fs::copy_file(replacement/chunk,overrideDir/chunk);
  const auto mesh=readWorldChunks(dir,overrideDir);
  const auto difference=compareWorldMeshes(canonicalWorldMesh(changed),mesh);
  require(difference.empty(),"override must replace base chunk: "+difference);
  require(!compareWorldMeshes(canonicalWorldMesh(original),mesh).empty(),"override fixture must change a vertex");
  auto moved=original;
  for(auto& vertex:moved.vertices)
    vertex.x+=4.f*ChunkId::Size;
  const auto movedDir=root/"moved";
  const auto movedManifest=writeWorldChunks(moved,source,movedDir);
  fs::copy_file(movedDir/movedManifest.chunks.front(),overrideDir/chunk,fs::copy_options::overwrite_existing);
  ChunkTable chunks;
  const auto movedMesh=readWorldChunks(dir,overrideDir,&chunks);
  require(compareWorldMeshes(canonicalWorldMesh(moved),movedMesh).empty(),"moved override geometry differs");
  require(chunks.entries.size()==1 && chunks.entries[0].id==ChunkId{0,0} &&
          chunks.entries[0].file==manifest.chunks.front() && chunks.entries[0].min.x>4.f*ChunkId::Size &&
          chunks.triangleChunks==std::vector<uint32_t>{0},"moved override must retain source chunk ownership and exact bounds");
  require(chunks.source!=nullptr && chunks.triangleSources.size()==1,"edited chunks must retain a file-backed source");
  const auto compact=chunks.source->read(0,chunks.triangleSources);
  require(compareWorldMeshes(canonicalWorldMesh(movedMesh),expandChunkMesh(compact,movedMesh)).empty(),
          "compact reads must use the selected override with the stitched vertex, feature and material IDs");
  std::puts("Selection: matching override chunk replaces base");
  }

using SourceTriangle = std::array<uint32_t,7>;

std::multiset<SourceTriangle> unpackLandscape(const PackedMesh& packed, const asset::WorldMesh& source) {
  std::multiset<SourceTriangle> result;
  for(const auto& sub:packed.subMeshes) {
    const auto material=std::find_if(source.materials.begin(),source.materials.end(),[&](const auto& candidate) {
      return sameMaterial(sub.material,candidate);
      });
    require(material!=source.materials.end(),"packed landscape material must retain its whole-mesh representative");
    for(size_t at=sub.iboOffset;at<sub.iboOffset+sub.iboLength;at+=3) {
      if(packed.indices[at]==packed.indices[at+1] && packed.indices[at]==packed.indices[at+2])
        continue;
      SourceTriangle triangle {};
      triangle[6]=uint32_t(material-source.materials.begin());
      for(size_t corner=0;corner<3;++corner) {
        const auto& vertex=packed.vertices[packed.indices[at+corner]];
        bool found=false;
        for(size_t id=0;id<source.vertices.size() && !found;++id) {
          const auto& position=source.vertices[id];
          if(position.x!=vertex.pos[0] || position.y!=vertex.pos[1] || position.z!=vertex.pos[2])
            continue;
          for(size_t feature=0;feature<source.features.size();++feature) {
            const auto& value=source.features[feature];
            if(value.texture.x!=vertex.uv[0] || value.texture.y!=vertex.uv[1] ||
               value.normal.x!=vertex.norm[0] || value.normal.y!=vertex.norm[1] || value.normal.z!=vertex.norm[2])
              continue;
            triangle[corner*2]=uint32_t(id);
            triangle[corner*2+1]=uint32_t(feature);
            found=true;
            break;
            }
          }
        require(found,"packed landscape corner must retain the source vertex/feature pair");
        }
      result.insert(triangle);
      }
    }
  return result;
  }

void chunkContract(const fs::path& root) {
  for(const auto& sample:std::array<std::pair<float,int32_t>,6>{{
      {-0.01f,-1},{0.f,0},{6399.99f,0},{6400.f,1},{-6400.f,-1},{-6400.01f,-2}}}) {
    require(ChunkId::fromPosition(sample.first,sample.first)==ChunkId{sample.second,sample.second},
            "chunk coordinate must floor at positive and negative boundaries");
    }

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
  const auto directory=root/"chunks";
  const auto manifest=writeWorldChunks(mesh,{"CONTRACT.ZEN",0,0},directory);
  ChunkTable table;
  auto restored=readWorldChunks(directory,{},&table);
  require(compareWorldMeshes(canonicalWorldMesh(mesh),restored).empty(),"chunk metadata must not reorder the stitched mesh");
  require(table.entries.size()==manifest.chunks.size() && table.triangleChunks.size()==8 && table.triangleSources.size()==8 &&
          table.source!=nullptr,"chunk table must cover the manifest and stitched/file-backed triangles");
  for(size_t i=0;i<table.entries.size();++i) {
    const auto& chunk=table.entries[i];
    require(chunk.file==manifest.chunks[i],"chunk table file must match manifest order");
    uint32_t count=0;
    zenkit::Vec3 minimum {},maximum {};
    for(size_t t=0;t<table.triangleChunks.size();++t) {
      const auto& a=mesh.vertices[mesh.polygons.vertex_indices[t*3]];
      const auto& b=mesh.vertices[mesh.polygons.vertex_indices[t*3+1]];
      const auto& c=mesh.vertices[mesh.polygons.vertex_indices[t*3+2]];
      const auto id=ChunkId::fromPosition(float((double(a.x)+b.x+c.x)/3.),float((double(a.z)+b.z+c.z)/3.));
      require(table.entries.at(table.triangleChunks[t]).id==id,"stitched triangle ownership must match the converter's centroid grid");
      if(table.triangleChunks[t]!=i)
        continue;
      if(count==0)
        minimum=maximum=a;
      for(size_t corner=0;corner<3;++corner) {
        const auto& vertex=mesh.vertices[mesh.polygons.vertex_indices[t*3+corner]];
        for(unsigned axis=0;axis<3;++axis) {
          minimum[axis]=std::min(minimum[axis],vertex[axis]);
          maximum[axis]=std::max(maximum[axis],vertex[axis]);
          }
        }
      ++count;
      }
    require(chunk.triangleCount==count && chunk.min==minimum && chunk.max==maximum,
            "chunk bounds and counts must use their actual triangles, including overhangs");
    }

  const auto beforeCloth=table.triangleChunks;
  const auto sourcesBeforeCloth=table.triangleSources;
  const auto extracted=WorldCloth::extract(restored,&table);
  require(extracted.surfaces.size()==1 && extracted.original.polygons.material_indices.size()==4 &&
          restored.polygons.material_indices.size()==4,"small chunk fixture must extract the four cloth triangles");
  require(table.triangleChunks==std::vector<uint32_t>(beforeCloth.begin(),beforeCloth.begin()+4),
          "cloth extraction must filter ownership alongside the exact retained source triangles");
  require(table.source!=nullptr && table.triangleSources==
          std::vector<uint32_t>(sourcesBeforeCloth.begin(),sourcesBeforeCloth.begin()+4),
          "cloth extraction must filter file-backed triangle IDs alongside ownership");
  require(restored.vertices==mesh.vertices && restored.polygons.vertex_indices==
          std::vector<uint32_t>(mesh.polygons.vertex_indices.begin(),mesh.polygons.vertex_indices.begin()+12) &&
          restored.polygons.feature_indices==std::vector<uint32_t>(mesh.polygons.feature_indices.begin(),mesh.polygons.feature_indices.begin()+12),
          "cloth extraction must preserve all source vertex and feature IDs");
  const auto lists=chunkTriangleLists(restored,table);
  const auto representatives=PackedMesh::landscapeMaterialMap(restored);
  const PackedMesh whole(restored,PackedMesh::PK_VisualLnd);
  std::multiset<SourceTriangle> expected,combined;
  for(size_t triangle=0;triangle<restored.polygons.material_indices.size();++triangle) {
    SourceTriangle value {};
    for(size_t corner=0;corner<3;++corner) {
      value[corner*2]=restored.polygons.vertex_indices[triangle*3+corner];
      value[corner*2+1]=restored.polygons.feature_indices[triangle*3+corner];
      }
    value[6]=representatives[restored.polygons.material_indices[triangle]];
    expected.insert(value);
    }
  for(size_t chunk=0;chunk<lists.size();++chunk) {
    require(lists[chunk].size()==table.entries[chunk].triangleCount,"filtered per-chunk count must match its source triangle list");
    const PackedMesh packed(restored,lists[chunk],representatives);
    const auto triangles=unpackLandscape(packed,restored);
    combined.insert(triangles.begin(),triangles.end());
    std::vector<uint32_t> sources;
    for(const auto triangle:lists[chunk])
      sources.push_back(table.triangleSources[triangle]);
    const auto compact=table.source->read(chunk,sources);
    auto expectedChunk=restored;
    retainWorldTriangles(expectedChunk,lists[chunk]);
    require(compareWorldMeshes(expectedChunk,expandChunkMesh(compact,restored)).empty(),
            "compact chunk reads must retain exact source values, polygon IDs and material records after cloth filtering");
    std::vector<uint32_t> localTriangles(sources.size());
    for(size_t i=0;i<localTriangles.size();++i)
      localTriangles[i]=uint32_t(i);
    const PackedMesh compactPacked(compact.mesh,localTriangles,representatives,compact.vertexIds,compact.featureIds);
    require(unpackLandscape(compactPacked,restored)==triangles,
            "file-backed chunk packing must match in-memory packing with global source IDs and whole-world material representatives");
    }
  require(unpackLandscape(whole,restored)==expected && combined==expected,
          "whole and chunk landscapes must contain the same triangles, original vertex/feature IDs and representative materials");
  auto sparse=restored;
  auto sparseTable=table;
  retainWorldTriangles(sparse,std::array<uint32_t,2>{1,3},&sparseTable);
  require(sparseTable.triangleChunks==std::vector<uint32_t>{table.triangleChunks[1],table.triangleChunks[3]} &&
          sparseTable.triangleSources==std::vector<uint32_t>{table.triangleSources[1],table.triangleSources[3]} &&
          sparse.polygons.vertex_indices==std::vector<uint32_t>{3,4,5,9,10,11},
          "the reusable filter must preserve source IDs and ownership for a noncontiguous subset");
  std::printf("Chunk landscape contract: %zu chunks, %zu triangles, 4 cloth triangles; disk/in-memory source IDs/materials equal\n",lists.size(),expected.size());

  ChunkTable grid;
  for(int32_t x=-4;x<=14;++x) for(int32_t z=-4;z<=4;++z)
    grid.entries.push_back({{x,z},{},{float(x*ChunkId::Size),0,float(z*ChunkId::Size)},
                           {float((x+1)*ChunkId::Size),100,float((z+1)*ChunkId::Size)},1});
  grid.entries.push_back({{20,20},{},{},{},0});
  const Tempest::Vec3 origin(100,0,100), distant(64100,0,100);
  ChunkResidency coincident(grid), split(grid);
  coincident.update(origin,origin,100000);
  split.update(origin,distant,100000);
  require(coincident.stats().near==25 && split.stats().near==50,"Near must be the union of hero and camera squares");
  require(coincident.desired(grid.entries.size()-1)==ChunkResidency::State::Outside,
          "empty chunk geometry must not use default origin bounds for Far residency");
  for(size_t i=0;i<grid.entries.size();++i)
    require(split.actualLoaded(i),"all chunks must remain actually loaded");

  ChunkTable single;
  single.entries.push_back({{0,0},{},{0,0,0},{6400,100,6400},1});
  using State=ChunkResidency::State;
  ChunkResidency near(single);
  size_t nearEvents=0;
  const auto subscription=near.subscribe([&, count=size_t(0)](ChunkId id,State,State) mutable {
    require(id==ChunkId{0,0},"state transition must identify the changed chunk");
    ++nearEvents;
    require(++count==nearEvents,"chunk listener must retain its mutable captured state between transitions");
    });
  auto nearAt=[&](float x) { near.update({x,0,100},{x,0,100},100000); };
  nearAt(2.f*ChunkId::Size+100);
  for(unsigned i=0;i<10;++i) {
    nearAt(3.f*ChunkId::Size-1);
    nearAt(3.f*ChunkId::Size+1);
    }
  require(near.desired(ChunkId{0,0})==State::Near && nearEvents==1,"Near boundary jitter must not emit transitions");
  nearAt(4.f*ChunkId::Size+1);
  nearAt(3.f*ChunkId::Size+1);
  require(near.desired(size_t(0))==State::Far && nearEvents==2,"Near must leave at radius four and stay out at radius three");
  nearAt(3.f*ChunkId::Size-1);
  require(near.desired(size_t(0))==State::Near && nearEvents==3,"Near must re-enter only at radius two");
  near.unsubscribe(subscription);
  nearAt(4.f*ChunkId::Size+1);
  require(nearEvents==3,"unsubscribed chunk listeners must not receive transitions");

  ChunkResidency far(single);
  auto farAt=[&](float x) { far.update({1000000,0,1000000},{x,0,100},100000); };
  farAt(106401);
  require(far.desired(size_t(0))==State::Outside,"Far must enter only within zFar of the actual AABB");
  farAt(106399);
  for(unsigned i=0;i<10;++i) { farAt(106401); farAt(106399); }
  require(far.desired(size_t(0))==State::Far && far.stats().totalTransitions==1,"Far boundary jitter must not emit transitions");
  farAt(112801);
  farAt(106401);
  require(far.desired(size_t(0))==State::Outside && far.stats().totalTransitions==2,"Far must leave beyond zFar plus one chunk and re-enter at zFar");

  const auto* previous=std::getenv("OPENGOTHIC_STREAMING");
  const std::string previousValue=previous==nullptr ? "" : previous;
  setenv("OPENGOTHIC_STREAMING","0",1);
  require(ChunkResidency::create(single)==nullptr,"OPENGOTHIC_STREAMING=0 must not create the manager");
  if(previous==nullptr)
    unsetenv("OPENGOTHIC_STREAMING");
  else
    setenv("OPENGOTHIC_STREAMING",previousValue.c_str(),1);
  std::puts("Chunks: negative boundaries, converter ownership/AABBs, independent rings, hysteresis and disabled manager passed");

  using Mode=NpcSimTiers::Mode;
  bool valid=false;
  require(NpcSimTiers::parseMode(nullptr,nullptr,&valid)==Mode::Auto && valid &&
          NpcSimTiers::parseMode("auto",nullptr)==Mode::Auto && NpcSimTiers::parseMode("full",nullptr)==Mode::Full &&
          NpcSimTiers::parseMode("simplified","1")==Mode::Simplified,
          "OPENGOTHIC_NPC_SIM_TIER must parse auto, full and simplified");
  require(NpcSimTiers::parseMode("bogus",nullptr,&valid)==Mode::Auto && !valid,
          "an unknown NPC tier must fall back to auto and be reported");
  require(NpcSimTiers::parseMode("simplified","0")==Mode::Full && NpcSimTiers::parseMode(nullptr,"0")==Mode::Full,
          "OPENGOTHIC_STREAMING=0 must keep every NPC Full");
  ChunkResidency tiers(single);
  tiers.update({100,0,100},{100,0,100},100000);
  const auto tier=[&](Mode mode, bool hero, bool keep, const ChunkResidency* residency, ChunkId cell, NpcSimTier previous) {
    return NpcSimTiers::desired(mode,hero,keep,residency,cell,previous);
    };
  for(const auto mode:{Mode::Auto,Mode::Full,Mode::Simplified})
    require(tier(mode,true,false,&tiers,ChunkId{50,50},NpcSimTier::Simplified)==NpcSimTier::Full,
            "the hero must be Full in every mode");
  require(tier(Mode::Full,false,false,&tiers,ChunkId{50,50},NpcSimTier::Simplified)==NpcSimTier::Full &&
          tier(Mode::Simplified,false,true,&tiers,ChunkId{0,0},NpcSimTier::Full)==NpcSimTier::Simplified,
          "full and simplified modes must override the residency for every NPC but the hero");
  require(tier(Mode::Auto,false,false,nullptr,ChunkId{50,50},NpcSimTier::Simplified)==NpcSimTier::Full,
          "auto without a residency manager must keep every NPC Full");
  require(tier(Mode::Auto,false,true,&tiers,ChunkId{50,50},NpcSimTier::Simplified)==NpcSimTier::Full,
          "training and arena actors must stay Full in auto mode");
  require(tier(Mode::Auto,false,false,&tiers,ChunkId{0,0},NpcSimTier::Simplified)==NpcSimTier::Full &&
          tier(Mode::Auto,false,false,&tiers,ChunkId{2,-2},NpcSimTier::Simplified)==NpcSimTier::Full,
          "an NPC in a Near chunk or cell must be Full");
  require(tier(Mode::Auto,false,false,&tiers,ChunkId{3,0},NpcSimTier::Full)==NpcSimTier::Full &&
          tier(Mode::Auto,false,false,&tiers,ChunkId{3,0},NpcSimTier::Simplified)==NpcSimTier::Simplified &&
          tier(Mode::Auto,false,false,&tiers,ChunkId{4,0},NpcSimTier::Full)==NpcSimTier::Simplified,
          "the NPC tier must use the Near enter radius 2 and exit radius 3");
  std::puts("NPC simulation tiers: modes, hero, streaming switch, Near ring and exit hysteresis passed");
  }
}

int main() {
  TemporaryDirectory temporary;
  try {
    const auto* gothic=std::getenv("GOTHIC_DIR");
    require(gothic!=nullptr && *gothic!='\0',"GOTHIC_DIR must point to a Gothic II installation");
    const char* args[]={"gothic-worldbase-contract","-g",gothic};
    CommandLine commandLine(3,args);
    Resources resources;
    Resources::loadBase();
    zenkit::Vfs sourceVfs;
    fs::path work;
    BaseImport::mountRuntime(sourceVfs,gothic,work);
    const auto* entry=sourceVfs.find("NEWWORLD.ZEN");
    require(entry!=nullptr,"NEWWORLD.ZEN is missing from the import reference VFS");
    auto input=entry->open_read();
    input->seek(0,zenkit::Whence::END);
    std::vector<std::byte> bytes(input->tell()); input->seek(0,zenkit::Whence::BEG);
    require(input->read(bytes.data(),bytes.size())==bytes.size(),"incomplete archive world");
    auto buffer=zenkit::Read::from(&bytes);
    zenkit::World world;
    world.load(buffer.get(),zenkit::GameVersion::GOTHIC_2);
    const WorldChunkSource source={"NEWWORLD.ZEN",bytes.size(),worldChunkCrc32(bytes.data(),bytes.size())};
    worldDataContract(world,source,temporary.path/"world-data");
    const auto rawPolygons=world.world_mesh.geometry.size();
    const auto rawVertices=world.world_mesh.polygon_vertex_indices;
    const auto rawFeatures=world.world_mesh.polygon_feature_indices;
    auto zen=asset::fromZenKit(world.world_mesh);
    const auto dir=temporary.path/"NEWWORLD";
    const auto manifest=writeWorldChunks(zen,source,dir);
    auto base=readWorldChunks(dir);
    const auto difference=compareWorldMeshes(canonicalWorldMesh(zen),base);
    require(difference.empty(),"canonical round trip: "+difference);
    std::printf("NEWWORLD.ZEN: %zu bytes, CRC32 %08x, %zu triangles, %zu chunks; canonical round trip equal\n",
                bytes.size(),source.crc32,zen.polygons.material_indices.size(),manifest.chunks.size());
    WorldData::Data loaded;
    Resources::loadWorld(loaded,base,"NEWWORLD.ZEN");
    require(base.name==zen.name && base.bbox.min==zen.bbox.min && base.bbox.max==zen.bbox.max,"base load must preserve source name and bounds");
    require(world.world_mesh.geometry.size()==rawPolygons && world.world_mesh.polygon_vertex_indices==rawVertices &&
            world.world_mesh.polygon_feature_indices==rawFeatures,"mesh conversion must preserve raw BSP geometry");
    const auto zenCloth=WorldCloth::extract(zen);
    const auto baseCloth=WorldCloth::extract(base);
    clothContract(zenCloth,baseCloth);
    selectionContract(temporary.path);
    chunkContract(temporary.path);
    worldbasePhysicsContract(zen,base);
    if(!zenCloth.original.polygons.material_indices.empty())
      visualContract(zenCloth.original,baseCloth.original,"Original cloth");
    visualContract(zen,base,"Landscape");
    std::puts("World base contract passed");
    return EXIT_SUCCESS;
    }
  catch(const std::exception& error) {
    std::fprintf(stderr,"FAIL: %s\n",error.what());
    return EXIT_FAILURE;
    }
  }
