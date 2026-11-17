#include "physics/dynamicworld.h"
#include "physics/placedcensuscache.h"
#include "graphics/bounds.h"
#include "graphics/mesh/protomesh.h"
#include "graphics/mesh/submesh/packedmesh.h"
#include "resources.h"
#include "commandline.h"
#include "gamemusic.h"
#include "gothic.h"
#include "game/gamescript.h"
#include "game/definitions/basescripts.h"
#include "game/serialize.h"
#include "npcsnapshot.h"
#include "npcsnapshotfields.h"
#include "programdata.h"
#include "routinedefinitions.h"
#include "npcdefinitions.h"
#include "game/npcdefinitionruntime.h"
#include "loadprobe.h"
#include "world/objects/item.h"
#include "world/objects/npc.h"
#include "world/benchhooks.h"
#include "testbench.h"

#include <Tempest/MemReader>
#include <Tempest/MemWriter>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <unistd.h>
#include <vector>
#include <map>
#include <set>
#include <type_traits>

void npcLiveSynthetic(void (*compare)(World&), void (*disabled)(World&));

// Test-only access keeps the production creation entry point unchanged.
struct NpcDefinitionContract {
  static auto& definitions(GameScript& script) { return script.npcDefinitions; }
  static const auto& items(GameScript& script) { return script.itemDefinitions; }
  static const auto& routines(GameScript& script) { return script.routineDefinitions; }
  static auto& random(GameScript& script) { return script.randGen; }
  };

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
  require(std::abs(result[0].hullLowPointCm.x)<.01f && std::abs(result[0].hullLowPointCm.z)<.01f,
          "a flat bottom must select its deterministic supporting-face centroid");
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

void censusCache() {
  namespace fs = std::filesystem;
  Resources resources;
  constexpr const char* visual = "CONTRACT_PLACED_CACHE.MRM";
  ProtoMesh proto(PackedMesh(box({20,20,20}),PackedMesh::PK_Visual),visual);
  Bounds bounds;
  bounds.assign(proto.bboxMesh);
  DynamicWorld world(nullptr,floor());
  auto furniture = world.addMovableVob(proto,visual,at(300,10),.25f,false,"utensil",true,903);
  world.finalizeMovableVobs();
  const std::array<Tempest::Matrix4x4,3> origins = {{at(0,11),at(300,31),at(600,40)}};
  std::vector<DynamicWorld::PlacedItemCandidate> candidates;
  for(size_t i=0;i<origins.size();++i)
    candidates.push_back({nullptr,false,visual,uint32_t(100+i),i,&proto,origins[i],uint32_t(MAT_WOOD)});
  // The key is taken before any census body exists: a hit must not need one.
  const auto inputs = world.placedItemCensusInputs(candidates);
  require(inputs.size()==64 && inputs==world.placedItemCensusInputs(candidates),
          "census inputs must be a stable digest of the same world");
  std::vector<DynamicWorld::Item> bodies;
  bodies.reserve(origins.size());
  for(size_t i=0;i<origins.size();++i) {
    bodies.push_back(world.addDynamicObj(origins[i],bounds,asset::MaterialGroup::WOOD,&proto,visual,true));
    bodies.back().restorePhysics(origins[i],true);
    candidates[i].body = &bodies[i];
    }
  require(world.placedItemCensusInputs(candidates)==inputs,"census inputs must not depend on the census bodies");
  const auto live = world.placedItemCensus(candidates);

  const fs::path root = fs::current_path()/("placed-census-contract-"+
      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  struct Cleanup { fs::path path; ~Cleanup() { std::error_code e; fs::remove_all(path,e); } } cleanup{root};
  using Status = PlacedCensusCache::Status;
  const PlacedCensusCache::Key key{"0123abcd","CONTRACT","content",0,inputs};
  require(PlacedCensusCache::builderFingerprint().size()==64 && key.builder==PlacedCensusCache::builderFingerprint(),
          "the census key must include the build's nonempty SHA-256 builder fingerprint");
  PlacedCensusCache cache(root,key);
  std::vector<PlacedCensusCache::Entry> stored;
  require(cache.read(live.size(),stored)==Status::Absent,"cold census cache must miss as absent");
  cache.save(live);
  require(cache.read(live.size(),stored)==Status::Hit && stored.size()==live.size(),"saved census must hit");
  for(size_t i=0;i<live.size();++i)
    require(PlacedCensusCache::differences(stored[i],live[i]).empty(),"cached census must equal the live census bit for bit");
  auto lowerKey = key;
  lowerKey.world = "contract";
  require(PlacedCensusCache(root,lowerKey).path()==cache.path() &&
          PlacedCensusCache(root,lowerKey).read(live.size(),stored)==Status::Hit,
          "a save's lower-case world name must use the fresh start's census file");
  require(cache.read(live.size()+1,stored)==Status::Corrupt,"a different candidate count must not reuse the census");
  auto builderKey = key;
  builderKey.builder[0] = key.builder[0]=='0' ? '1' : '0';
  require(PlacedCensusCache(root,builderKey).read(live.size(),stored)==Status::Builder && stored.empty(),
          "a different builder fingerprint must miss as builder");
  require(PlacedCensusCache::statusName(Status::Builder)=="builder","builder misses must have a distinct log reason");

  // Moving one item must change the digest and miss the stored census.
  auto movedKey = key;
  auto moved = candidates;
  moved[0].transform = at(0,12);
  movedKey.inputs = world.placedItemCensusInputs(moved);
  require(movedKey.inputs!=inputs,"a changed item transform must change the census inputs");
  require(PlacedCensusCache(root,movedKey).read(live.size(),stored)==Status::Inputs,"a changed transform must miss");
  auto identity = candidates;
  identity[1].instance = 77;
  require(world.placedItemCensusInputs(identity)!=inputs,"a changed item instance must change the census inputs");
  // A different mesh under the same visual name changes the hull source.
  ProtoMesh taller(PackedMesh(box({20,30,20}),PackedMesh::PK_Visual),visual);
  auto mesh = candidates;
  for(auto& candidate:mesh)
    candidate.proto = &taller;
  require(world.placedItemCensusInputs(mesh)!=inputs,"a changed item mesh must change the census inputs");
  auto material = candidates;
  material[2].material = uint32_t(MAT_METAL);
  require(world.placedItemCensusInputs(material)!=inputs,"a changed item material must change the census inputs");

  auto versionKey = key;
  ++versionKey.version;
  require(PlacedCensusCache(root,versionKey).read(live.size(),stored)==Status::Version,"a census file format bump must miss");

  std::string bytes;
  {
    std::ifstream input(cache.path(),std::ios::binary);
    bytes.assign(std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>());
  }
  bytes.back() ^= 1;
  {
    std::ofstream output(cache.path(),std::ios::binary|std::ios::trunc);
    output.write(bytes.data(),std::streamsize(bytes.size()));
  }
  require(cache.read(live.size(),stored)==Status::Corrupt && stored.empty(),"a damaged census payload must be rejected");
  fs::remove(cache.path());

  setenv("OPENGOTHIC_PLACED_CENSUS_CACHE","0",1);
  PlacedCensusCache disabled(root,key);
  require(disabled.read(live.size(),stored)==Status::Disabled,"the disabled census cache must not read");
  disabled.save(live);
  require(!fs::exists(cache.path()),"the disabled census cache must not write");
  unsetenv("OPENGOTHIC_PLACED_CENSUS_CACHE");
  require(furniture.mass()>0,"census cache fixture must retain its movable vob");
  std::puts("Placed census cache: body-free key with builder fingerprint; hit equals live; world name case-insensitive; builder, transform, instance, mesh, material, version, count and damage miss; disabled bypass");
  }

void rootedAndStack() {
  Resources resources;
  constexpr const char* visual = "CONTRACT_PLACED_ROOTED.MRM";
  ProtoMesh proto(PackedMesh(box({20,20,20}),PackedMesh::PK_Visual),visual);
  Bounds bounds;
  bounds.assign(proto.bboxMesh);
  DynamicWorld world(nullptr,floor());
  auto furniture = world.addMovableVob(proto,visual,at(200,10),.25f,false,"utensil",true,902);
  world.finalizeMovableVobs();
  std::vector<DynamicWorld::Item> bodies;
  bodies.reserve(5);
  for(const auto& origin:{at(0,11),at(200,31),at(400,11),at(400,33),at(400,55)}) {
    bodies.push_back(world.addDynamicObj(origin,bounds,asset::MaterialGroup::WOOD,&proto,visual,true));
    bodies.back().restorePhysics(origin,true);
    }
  // Reverse the stack so propagation needs more than one pass.
  const auto result = world.placedItemCensus({{&bodies[0],false,"iTpL_Herb.MRM"},
    {&bodies[1],false,"ItPl_Herb.MRM"},{&bodies[4],false,visual},
    {&bodies[3],false,visual},{&bodies[2],true,visual}});
  require(result[0].disposition=="rooted","ItPl prefix must be case insensitive on static support");
  require(result[1].enabled && result[1].supportCategory==DynamicWorld::C_MovableVob,
          "ItPl items on movable furniture must remain eligible");
  require(result[4].disposition=="quest","mission reason must precede support-dependent reasons");
  require(result[3].disposition=="on_excluded" && result[2].disposition=="on_excluded",
          "exclusion must propagate through an Item support chain to a fixed point");
  require(result[2].supportItemIndex==3 && result[3].supportItemIndex==4,
          "census must retain exact candidate identities through a support chain");
  }

void slopes() {
  Resources resources;
  constexpr const char* visual = "CONTRACT_PLACED_SLOPE.MRM";
  ProtoMesh proto(PackedMesh(box({20,20,20}),PackedMesh::PK_Visual),visual);
  ProtoMesh platform(PackedMesh(box({200,20,200},true),PackedMesh::PK_Visual),"CONTRACT_PLACED_PLATFORM.MRM");
  Bounds bounds;
  bounds.assign(proto.bboxMesh);
  constexpr float radians = 3.14159265358979323846f/180.f;
  for(bool landscape:{true,false})
    for(float degrees:{10.f,20.f}) {
      auto mesh = floor();
      for(auto& vertex:mesh.vertices)
        vertex.y = landscape ? vertex.x*std::tan(degrees*radians) : -100.f;
      DynamicWorld world(nullptr,mesh);
      DynamicWorld::Item structure;
      if(!landscape) {
        auto transform = at(0,-10/std::cos(degrees*radians));
        transform.rotateOZ(degrees);
        structure = world.addStaticObj(platform.attach[0].shape.get(),transform,"CONTRACT_PLACED_PLATFORM.MRM");
        }
      auto item = world.addDynamicObj(at(0,14),bounds,asset::MaterialGroup::WOOD,&proto,visual,true);
      const auto result = world.placedItemCensus({{&item,false,visual}});
      std::printf("support slope: category=%d angle_deg=%.6f disposition=%s\n",result[0].supportCategory,
                  double(result[0].supportSlopeDeg),result[0].disposition.c_str());
      require(std::abs(result[0].supportSlopeDeg-degrees)<.01f,
              "slope must use the ray-hit static face normal below the actual lowest hull point");
      require(result[0].disposition==(degrees>15.f ? "slope" : "ok"),
              "only static support slopes above fifteen degrees must be excluded");
      require(result[0].supportFaceNormal.y>0,"the support face must retain the native upward ray normal");
      }
  }

void penetrationLimit() {
  Resources resources;
  constexpr const char* visual = "CONTRACT_PLACED_THIN.MRM";
  ProtoMesh proto(PackedMesh(box({40,2,80}),PackedMesh::PK_Visual),visual);
  ProtoMesh supportProto(PackedMesh(box({200,20,200}),PackedMesh::PK_Visual),"CONTRACT_PLACED_SUPPORT.MRM");
  Bounds bounds, supportBounds;
  bounds.assign(proto.bboxMesh);
  supportBounds.assign(supportProto.bboxMesh);
  for(int supportKind=0;supportKind<3;++supportKind) {
    DynamicWorld world(nullptr,floor());
    auto origin = at(0,100);
    origin.rotateOZ(20);
    auto item = world.addDynamicObj(origin,bounds,asset::MaterialGroup::WOOD,&proto,visual,true);
    require(!item.isEmpty() && world.itemHullShape(proto,visual).fallbackReason.empty(),
            "thin tilted fixture must use the actual item_hulls collision shape");
    const auto initial = world.waterBodySnapshot();
    require(initial.size()==1,"thin hull fixture must expose its native collision bounds");
    // Rotating the bounds can overestimate the envelope: use the recorded
    // actual hull low point rather than guessing its inflation from the mesh.
    const float low = world.placedItemCensus({{&item,false,visual}})[0].hullLowPointCm.y;
    require(low>=initial[0].collisionBounds[0].y-.001f,"actual hull bottom must remain within its native bounds");
    DynamicWorld::Item support;
    float top = 0;
    if(supportKind==1) {
      support = world.addMovableVob(supportProto,"CONTRACT_PLACED_SUPPORT.MRM",at(0,10),.25f,false,"utensil",true,903);
      world.finalizeMovableVobs();
      require(support.mass()>0,"penetration fixture must retain movable furniture");
      for(const auto& native:world.waterBodySnapshot())if(native.category==DynamicWorld::C_MovableVob)
        top = native.collisionBounds[1].y;
      require(top>0,"penetration fixture must find the furniture's native top");
    }
    else if(supportKind==2) {
      support = world.addDynamicObj(at(0,11),supportBounds,asset::MaterialGroup::WOOD,&supportProto,"CONTRACT_PLACED_SUPPORT.MRM",true);
      support.restorePhysics(at(0,11),true);
      for(const auto& native:world.waterBodySnapshot())if(std::abs(native.transform.at(3,1)-11.f)<.001f)
        top = native.collisionBounds[1].y;
      require(top>0,"penetration fixture must find the neighbouring Item's native top");
    }
    for(float depth:{1.5f,3.f}) {
      auto transform = at(0,100.f+top-depth-low);
      transform.rotateOZ(20);
      item.restorePhysics(transform,true);
      std::vector<DynamicWorld::PlacedItemCandidate> candidates = {{&item,false,visual}};
      if(supportKind==2)candidates.push_back({&support,false,"CONTRACT_PLACED_SUPPORT.MRM"});
      const auto result = world.placedItemCensus(candidates);
      const char* reason = supportKind==0 ? "static" : supportKind==1 ? "furniture" : "neighbour";
      const auto measured = supportKind==0 ? result[0].staticDepthCm :
                            supportKind==1 ? result[0].furnitureDepthCm : result[0].itemDepthCm;
      std::printf("thin penetration: support=%s requested_cm=%.6f measured_cm=%.6f disposition=%s\n",
                  reason,double(depth),double(measured),result[0].disposition.c_str());
      require(std::abs(measured-depth)<.05f,"penetration must be measured from the actual rotated padded hull");
      require(result[0].disposition==(depth>2.f ? reason : "ok"),
              "penetration above two centimetres must exclude static, furniture and neighbouring Item contacts");
      if(supportKind==2)
        require(result[1].disposition==(depth>2.f ? "neighbour" : "ok"),
                "the two-centimetre neighbour exclusion must reject both Items together");
    }
  }
}

void groundBelowSupport() {
  Resources resources;
  constexpr const char* visual = "CONTRACT_PLACED_GROUND.MRM";
  ProtoMesh proto(PackedMesh(box({20,20,20}),PackedMesh::PK_Visual),visual);
  ProtoMesh platform(PackedMesh(box({200,20,200},true),PackedMesh::PK_Visual),"CONTRACT_PLACED_GROUND_PLATFORM.MRM");
  Bounds bounds;
  bounds.assign(proto.bboxMesh);
  for(bool movable:{false,true})for(int groundKind=0;groundKind<3;++groundKind) {
    auto mesh = movable || groundKind==1 ? floor() : asset::WorldMesh{};
    if(groundKind==1)for(auto& vertex:mesh.vertices)vertex.y = 600.f;
    DynamicWorld world(nullptr,mesh);
    DynamicWorld::Item support, lower;
    if(movable) {
      support = world.addMovableVob(platform,"CONTRACT_PLACED_GROUND_PLATFORM.MRM",at(0,groundKind==1 ? 610.f : 10.f),
                                    .25f,false,"utensil",true,904);
      world.finalizeMovableVobs();
      require(support.mass()>0 && world.setMovableVobTransform(904,at(0,1000)),
              "ground fixture must retain a movable support above its lower ground");
    }
    else support = world.addStaticObj(platform.attach[0].shape.get(),at(0,1000),"CONTRACT_PLACED_GROUND_PLATFORM.MRM");
    if(groundKind==2)
      lower = world.addStaticObj(platform.attach[0].shape.get(),at(0,700),"CONTRACT_PLACED_LOWER_GROUND.MRM");
    auto item = world.addDynamicObj(at(0,1022),bounds,asset::MaterialGroup::WOOD,&proto,visual,true);
    const auto result = world.placedItemCensus({{&item,false,visual}});
    std::printf("ground below support: movable=%d lower_kind=%d support=%d disposition=%s\n",
                int(movable),groundKind,result[0].supportCategory,result[0].disposition.c_str());
    require(result[0].supportCategory==(movable ? DynamicWorld::C_MovableVob : DynamicWorld::C_Object),
            "ground check must retain the actual immediate supporting body");
    require(result[0].disposition==(groundKind==0 ? "no_ground" : "ok"),
            "support over a void must be excluded; lower landscape or another C_Object within five metres must admit it");
  }
}

void asymmetricLowestPoint() {
  Resources resources;
  auto source = box({20,20,20});
  for(auto& vertex:source.positions)
    vertex.x += 30;
  source.bbox.min.x += 30;
  source.bbox.max.x += 30;
  constexpr const char* visual = "CONTRACT_PLACED_ASYMMETRIC.MRM";
  ProtoMesh proto(PackedMesh(source,PackedMesh::PK_Visual),visual);
  Bounds bounds;
  bounds.assign(proto.bboxMesh);
  DynamicWorld world(nullptr,asset::WorldMesh{});
  auto transform = at(100,100);
  transform.rotateOZ(30);
  auto item = world.addDynamicObj(transform,bounds,asset::MaterialGroup::WOOD,&proto,visual,true);
  const auto result = world.placedItemCensus({{&item,false,visual}});
  const auto point = result[0].hullLowPointCm;
  float lowest = std::numeric_limits<float>::infinity();
  for(const auto& vertex:source.positions) {
    auto worldVertex = Tempest::Vec3(vertex.x,vertex.y,vertex.z);
    transform.project(worldVertex);
    lowest = std::min(lowest,worldVertex.y);
    }
  auto center = Tempest::Vec3(30,0,0);
  transform.project(center);
  require(point.y<=lowest+.001f && point.y>=lowest-1.1f,
          "rotated asymmetric lowest point must include the actual padded hull envelope");
  require(std::abs(point.x-center.x)>1.f && std::abs(point.z)<.01f,
          "rotated asymmetric support must use its lowest edge centroid rather than its bounds center");
  }

void irregularSupport() {
  Resources resources;
  constexpr const char* visual = "CONTRACT_PLACED_IRREGULAR.MRM";
  ProtoMesh proto(PackedMesh(box({20,20,20}),PackedMesh::PK_Visual),visual);
  ProtoMesh ledge(PackedMesh(box({10,10,100},true),PackedMesh::PK_Visual),"CONTRACT_PLACED_LEDGE.MRM");
  Bounds bounds;
  bounds.assign(proto.bboxMesh);
  DynamicWorld world(nullptr,asset::WorldMesh{});
  // The ledge supports the right side of the bottom hull. The ray through
  // that hull's actual lowest-face centroid misses the ledge at x=5..15 cm.
  auto support = world.addStaticObj(ledge.attach[0].shape.get(),at(10,-5),"CONTRACT_PLACED_LEDGE.MRM");
  require(!support.isEmpty(),"irregular support fixture must install its static ledge");
  std::vector<DynamicWorld::Item> bodies;
  bodies.reserve(3);
  for(const auto& origin:{at(0,11),at(0,33),at(0,55)}) {
    bodies.push_back(world.addDynamicObj(origin,bounds,asset::MaterialGroup::WOOD,&proto,visual,true));
    bodies.back().restorePhysics(origin,true);
    }
  const auto result = world.placedItemCensus({{&bodies[2],false,visual},
    {&bodies[1],false,visual},{&bodies[0],false,visual}});
  require(result[2].supportCategory==DynamicWorld::C_Object && result[2].supportGapCm<=4.f &&
          !std::isfinite(result[2].supportSlopeDeg),
          "irregular support must retain a valid overlap but no static face below its true lowest point");
  require(result[2].disposition=="irregular",
          "a missing static support-face ray must exclude an otherwise admitted item as irregular");
  require(result[1].disposition=="on_excluded" && result[0].disposition=="on_excluded",
          "an irregular support must exclude the entire Item support chain to a fixed point");
  }

void benchHelpers(Npc& npc) {
  require(BenchProbe::get()==nullptr,"bench helpers start without an adapter");
  const std::string text = std::string("\"\\\n\r\t\b\f\x01\x1f\0",10)+"UTF-8: \xc3\xa9\xe7\x8c\xab";
  const std::string json = "\"\\\"\\\\\\n\\r\\t\\u0008\\u000c\\u0001\\u001f\\u0000UTF-8: \xc3\xa9\xe7\x8c\xab\"";
  const auto* symbol = npc.world().script().findSymbol(size_t(npc.instanceSymbol()));
  require(symbol!=nullptr && !symbol->name().empty(),"real NPC has a script symbol");
  const std::string expectedName(symbol->name()); // The former TestBench::who lookup.
  for(bool installed:{false,true}) {
    if(installed)
      TestBench::install();
    require(Bench::bodyJson(text)==json,"JSON escaping preserves the legacy bytes with and without an adapter");
    require(Bench::who(nullptr)=="none","a null NPC has no name with and without an adapter");
    require(Bench::who(&npc)==expectedName,"NPC symbol matches the former TestBench lookup with and without an adapter");
    require(npc.world().npcSymbolName(&npc)==expectedName,"physics uses the same NPC symbol name");
    }
  BenchProbe::install(nullptr);
  require(Bench::frameDt(37)==37,"frame dt is unchanged without an adapter");
  require(Bench::bodyBlockVariant(npc,2)==2,"block variant falls back without an adapter");
  std::FILE* output = std::tmpfile();
  require(output!=nullptr,"open bench event output capture");
  std::fflush(nullptr);
  const int savedOut = dup(STDOUT_FILENO), savedErr = dup(STDERR_FILENO);
  require(savedOut>=0 && savedErr>=0,"save bench event output streams");
  const int redirectOut = dup2(fileno(output),STDOUT_FILENO);
  const int redirectErr = dup2(fileno(output),STDERR_FILENO);
  require(redirectOut>=0 && redirectErr>=0,"capture bench event output streams");
  Bench::note("uninstalled bench");
  Bench::arrowAt(&npc,1,2,3);
  Bench::bodyEvent(npc,nullptr,"uninstalled",0);
  Bench::destructionBreak(1,2,3);
  std::fflush(nullptr);
  const auto written = std::ftell(output);
  const int restoreOut = dup2(savedOut,STDOUT_FILENO);
  const int restoreErr = dup2(savedErr,STDERR_FILENO);
  close(savedOut);
  close(savedErr);
  std::fclose(output);
  require(restoreOut>=0 && restoreErr>=0,"restore bench event output streams");
  require(written==0,"bench events must not write stdout or stderr without an adapter");
  std::puts("Bench helpers: JSON bytes, null and real NPC names, physics name, dt, block fallback and silent events passed");
  }

void routineTables(Npc& npc) {
  auto& script=npc.world().script();
  auto& vm=script.getVm();
  const auto base=baseProgramIndex().dir.parent_path();
  const auto program=NpcSnapshot::load(base);
  const auto library=RoutineDefinitions::read(base,program.sha256);
  require(!library.routines.empty(),"the real program must have captured routine definitions");
  struct Row {
    gtime start,end;
    ScriptFn state;
    std::string point;
    const WayPoint* waypoint;
    };
  auto rows=[](const Npc& who) {
    std::vector<Row> result;
    who.forEachRoutine([&](const Npc::RoutineInfo& row) {
      result.push_back({row.start,row.end,row.state,std::string(row.point),row.waypoint});
      });
    return result;
    };
  auto compare=[](const std::vector<Row>& expected,const std::vector<Row>& actual,const std::string& name) {
    if(actual.size()!=expected.size())
      throw std::runtime_error(name+": NPC routine table row count differs");
    for(size_t index=0;index<actual.size();++index) {
      const auto& a=actual[index];
      const auto& e=expected[index];
      if(a.start!=e.start || a.end!=e.end || a.state!=e.state || a.point!=e.point || a.waypoint!=e.waypoint)
        throw std::runtime_error(name+": NPC routine table row "+std::to_string(index+1)+" differs");
      }
    };
  const auto previous=rows(npc);
  const auto oldSelf=vm.global_self()->get_instance();
  vm.global_self()->set_instance(std::static_pointer_cast<daedalus::INpc>(npc.handlePtr()));
  size_t count=0,rowCount=0;
  const auto* configured=std::getenv("OPENGOTHIC_ROUTINE_RECORDS");
  const bool recordsEnabled=configured==nullptr || std::strcmp(configured,"0")!=0;
  size_t instructions=0;
  try {
    for(const auto& [name,definition]:library.routines) {
      auto* function=vm.find_symbol_by_name(name);
      require(function!=nullptr,"captured routine must exist in the game's VM");
      npc.clearRoutine();
      vm.observe_instructions([&](const daedalus::DaedalusInstruction&) { ++instructions; });
      instructions=0;
      vm.call_function<void>(function);
      require(instructions>0,"the routine reference must execute the VM");
      const auto expected=rows(npc);
      instructions=0;
      npc.excRoutine(function->index());
      if(recordsEnabled ? instructions!=0 : instructions==0)
        throw std::runtime_error(name+": routine exchange chose the wrong record/VM path");
      vm.observe_instructions({});
      const auto actual=rows(npc);
      if(actual.size()!=definition.size())
        throw std::runtime_error(name+": NPC routine table row count differs");
      compare(expected,actual,name);
      ++count;
      rowCount+=actual.size();
      }
    }
  catch(...) { vm.observe_instructions({}); vm.global_self()->set_instance(oldSelf); throw; }
  vm.global_self()->set_instance(oldSelf);
  npc.clearRoutine();
  for(const auto& row:previous)
    npc.addRoutine(row.start,row.end,uint32_t(row.state.ptr),row.point);
  require(count==library.routines.size(),"every captured routine must be compared on a live NPC");
  std::printf("NPC routine table equality: %zu captured functions, %zu rows; times, state, point pointers, fallback names and order match\n",
              count,rowCount);

  auto* xardas=npc.world().findNpcByInstance(script.findSymbolIndex("NONE_100_XARDAS"));
  auto* ritual=vm.find_symbol_by_name("RTN_RITUALINNOSEYE_100");
  auto* exchange=vm.find_symbol_by_name("NPC_EXCHANGEROUTINE");
  require(xardas && ritual && exchange,"the story routine fixture must contain Xardas and his Innos Eye ritual");
  require(library.routines.at(ritual->name()).size()==2,"the Innos Eye ritual must contain two captured rows");
  const auto beforeRitual=rows(*xardas);
  const auto beforeRecord=xardas->handle();
  vm.global_self()->set_instance(std::static_pointer_cast<daedalus::INpc>(xardas->handlePtr()));
  size_t exchanges=0;
  try {
    xardas->clearRoutine();
    vm.call_function<void>(ritual);
    const auto expected=rows(*xardas);
    require(expected.size()==2 && (beforeRitual.empty() || expected[0].state!=beforeRitual[0].state ||
                                  expected[0].point!=beforeRitual[0].point),
            "the story fixture must change Xardas's current routine");
    xardas->clearRoutine();
    for(const auto& row:beforeRitual)
      xardas->addRoutine(row.start,row.end,uint32_t(row.state.ptr),row.point);
    vm.observe_instructions([&](const daedalus::DaedalusInstruction& instruction) {
      if(instruction.op==daedalus::DaedalusOpcode::BE && instruction.symbol==exchange->index()) ++exchanges;
      });
    vm.call_function<void>("B_STARTOTHERROUTINE",std::static_pointer_cast<daedalus::INpc>(xardas->handlePtr()),
                           std::string_view("RITUALINNOSEYE"));
    vm.observe_instructions({});
    require(exchanges==1,"the story helper must call Npc_ExchangeRoutine exactly once");
    compare(expected,rows(*xardas),"B_STARTOTHERROUTINE(Xardas,RITUALINNOSEYE)");
    }
  catch(...) { vm.observe_instructions({}); vm.global_self()->set_instance(oldSelf); throw; }
  xardas->handle()=beforeRecord;
  xardas->clearRoutine();
  for(const auto& row:beforeRitual)
    xardas->addRoutine(row.start,row.end,uint32_t(row.state.ptr),row.point);
  vm.global_self()->set_instance(oldSelf);
  std::puts("Story routine change: B_StartOtherRoutine(Xardas,RITUALINNOSEYE) -> Npc_ExchangeRoutine; two live rows match the VM reference");
  }

using ArchiveEntries = std::map<std::string,std::vector<uint8_t>>;

ArchiveEntries archiveEntries(const std::vector<uint8_t>& bytes) {
  mz_zip_archive zip{};
  require(mz_zip_reader_init_mem(&zip,bytes.data(),bytes.size(),0),"open NPC equality archive");
  struct Close { mz_zip_archive& zip; ~Close() { mz_zip_reader_end(&zip); } } close{zip};
  ArchiveEntries entries;
  for(mz_uint index=0;index<mz_zip_reader_get_num_files(&zip);++index) {
    mz_zip_archive_file_stat stat{};
    require(mz_zip_reader_file_stat(&zip,index,&stat),"read NPC equality archive entry");
    auto& payload=entries[stat.m_filename];
    payload.resize(size_t(stat.m_uncomp_size));
    require(mz_zip_reader_extract_to_mem(&zip,index,payload.data(),payload.size(),0),"extract NPC equality archive entry");
    }
  return entries;
  }

ArchiveEntries npcState(Npc& npc) {
  std::vector<uint8_t> bytes;
  {
    Tempest::MemWriter writer(bytes);
    Serialize out(writer);
    out.setContext(&npc.world());
    npc.save(out,0,"/npc/");
    }
  // Compare entry payloads: ZIP modification times are outside NPC state.
  return archiveEntries(bytes);
  }

ArchiveEntries itemState(GameScript& script) {
  const auto item=script.getVm().global_item()->get_instance();
  if(!item) return {};
  require(script.getVm().global_item()->is_instance_of<daedalus::IItem>(),"global ITEM has the item class");
  std::vector<uint8_t> bytes;
  {
    Tempest::MemWriter writer(bytes);
    Serialize out(writer);
    out.setEntry("item");
    out.write(item->symbol_index(),static_cast<const ItemRecord&>(*std::static_pointer_cast<daedalus::IItem>(item)));
    }
  return archiveEntries(bytes);
  }

std::map<std::string,std::string> npcRecordFields(Npc& npc) {
  std::map<std::string,std::string> fields;
  auto append=[&](auto&& again,const auto& value,const std::string& name) -> void {
    using Type=std::remove_cvref_t<decltype(value)>;
    if constexpr(std::is_array_v<Type>) {
      for(size_t index=0;index<std::extent_v<Type>;++index)
        again(again,value[index],name+"["+std::to_string(index)+"]");
      }
    else if constexpr(std::is_same_v<Type,std::string>) fields[name]=Bench::bodyJson(value);
    else fields[name]=std::to_string(int64_t(value));
    };
  auto native=std::static_pointer_cast<daedalus::INpc>(npc.handlePtr());
  snapshotFields(*native,[&](const char* name,const auto& value,bool,const char*) { append(append,value,name); });
  return fields;
  }

void npcDefinitions(World& world) {
  auto& script=world.script();
  auto& vm=script.getVm();
  const auto base=baseProgramIndex().dir.parent_path();
  const auto program=NpcSnapshot::load(base);
  const auto library=NpcDefinitions::read(base,program.sha256);
  const auto routines=RoutineDefinitions::read(base,program.sha256);
  require(!library.npcs.empty(),"the real program must have captured NPC definitions");
  auto loaded=std::move(NpcDefinitionContract::definitions(script));
  const auto random=NpcDefinitionContract::random(script);
  auto records=NpcDefinitionRuntime::resolve(library,vm);
  const auto oldSelf=vm.global_self()->get_instance();
  const auto oldItem=vm.global_item()->get_instance();
  auto prepareSyntheticPhysics=[&](Npc& npc) {
    // These script fixtures have no model. Native NPC saves require a physical object.
    if(library.npcs.contains("NPC_NO_ID"))
      npc.setPhysic(world.physic()->addGhostObj(nullptr));
    };
  std::vector<std::string> names;
  for(const auto& [name,definition]:library.npcs) { (void)definition; names.push_back(name); }
  const std::string templateName=library.npcs.contains("NPC_NO_ID") ? "NPC_NO_ID" : "NONE_100_XARDAS";
  require(library.npcs.contains(templateName),"NPC template fixture must be captured");
  std::string error;
  const auto* entry=script.ownSymbols().find("DIR_NPC_RECORD_CONTRACT");
  const auto* own=entry ? vm.find_symbol_by_index(entry->index) :
                         script.ownSymbols().addNpc("DIR_NPC_RECORD_CONTRACT",templateName,error);
  if(!own) throw std::runtime_error("create NPC record template fixture: "+error);
  names.push_back(own->name());
  std::set<uint32_t> routineAddresses;
  for(const auto& [name,rows]:routines.routines) {
    (void)rows;
    routineAddresses.insert(vm.find_symbol_by_name(name)->address());
    }
  const auto* configured=std::getenv("OPENGOTHIC_ROUTINE_RECORDS");
  const bool routineRecords=configured==nullptr || std::strcmp(configured,"0")!=0;
  size_t count=0,dailyCount=0;
  std::vector<std::string> differences;
  std::set<std::string> differentNpcs;
  auto difference=[&](const std::string& name,const std::string& field,const std::string& expected,const std::string& actual) {
    if(expected==actual) return;
    differentNpcs.insert(name);
    differences.push_back(name+": "+field+" expected="+expected+" actual="+actual);
    };
  auto archiveDifference=[&](const std::string& name,const std::string& field,const ArchiveEntries& expected,const ArchiveEntries& actual) {
    for(const auto& [entry,payload]:expected) {
      const auto found=actual.find(entry);
      if(found==actual.end()) { difference(name,field+"."+entry,"present","absent"); continue; }
      difference(name,field+"."+entry+".size",std::to_string(payload.size()),std::to_string(found->second.size()));
      for(size_t index=0;index<std::min(payload.size(),found->second.size());++index)
        if(payload[index]!=found->second[index])
          difference(name,field+"."+entry+".byte["+std::to_string(index)+"]",
                     std::to_string(payload[index]),std::to_string(found->second[index]));
      }
    for(const auto& [entry,payload]:actual) {
      (void)payload;
      if(!expected.contains(entry)) difference(name,field+"."+entry,"absent","present");
      }
    };
  try {
    for(const auto& name:names) {
      auto* symbol=vm.find_symbol_by_name(name);
      require(symbol!=nullptr,"captured NPC must exist in the game's VM");
      const auto previous=symbol->get_instance();
      const auto previousContext=vm.unsafe_get_gi();
      std::unique_ptr<Npc> expected,actual;
      bool recordsActive=false;
      try {
        const auto policy=name=="PC_HERO" ? NpcProcessPolicy::Player : NpcProcessPolicy::AiNormal;
        const bool syntheticEquip=library.npcs.contains("NPC_NO_ID") &&
                                  (name=="NPC_NO_ID" || name=="DIR_NPC_RECORD_CONTRACT");
        const auto* onEquip=vm.find_symbol_by_name("ON_EQUIP");
        if(syntheticEquip) require(onEquip!=nullptr,"synthetic equipment has an on_equip callback");
        const auto onEquipAddress=onEquip ? onEquip->address() : uint32_t(-1);
        size_t routineInstructions=0,constructorInstructions=0,onEquipCalls=0;
        const auto constructorAddress=symbol->address();
        vm.observe_instructions([&](const daedalus::DaedalusInstruction&) {
          if(routineAddresses.contains(vm.pc())) ++routineInstructions;
          if(vm.pc()==constructorAddress) ++constructorInstructions;
          if(vm.pc()==onEquipAddress) ++onEquipCalls;
          });
        NpcDefinitionContract::definitions(script)={};
        script.setSeed(0x12345678);
        vm.global_item()->set_instance(oldItem);
        expected=std::make_unique<Npc>(world,symbol->index(),world.startPoint().name,policy);
        difference(name,"reference constructor executed","yes",constructorInstructions>0 ? "yes" : "no");
        if(syntheticEquip) {
          difference(name,"reference on_equip executed","yes",onEquipCalls>0 ? "yes" : "no");
          difference(name,"reference final attribute[0]","40",std::to_string(expected->handle().attribute[0]));
          difference(name,"reference helper result","30",std::to_string(expected->handle().exp));
          difference(name,"reference final level","30",std::to_string(expected->handle().level));
          }
        prepareSyntheticPhysics(*expected);
        const auto expectedFields=npcRecordFields(*expected);
        const auto expectedState=npcState(*expected);
        const auto expectedItem=itemState(script);
        const auto expectedRandom=script.randomState(nullptr);
        auto temporaryStrings=[&] {
          std::vector<std::string> result;
          for(const auto& value:vm.symbols())
            if(value.name()=="$PHOENIX_FAKE_STRINGS")
              for(uint16_t index=0;index<value.count();++index) result.push_back(value.get_string(index));
          return result;
          };
        const auto expectedStrings=temporaryStrings();
        const auto expectedDaily=vm.find_symbol_by_index(uint32_t(expected->handle().daily_routine));
        const bool capturedDaily=expectedDaily && routines.routines.contains(expectedDaily->name());
        if(capturedDaily) {
          ++dailyCount;
          difference(name,"NPC startup daily_routine must use the configured routine record/VM path",
                     routineRecords ? "record" : "VM",routineInstructions==0 ? "record" : "VM");
          }
        NpcDefinitionContract::definitions(script).npcs.swap(records.npcs);
        recordsActive=true;
        script.setSeed(0x12345678);
        vm.global_item()->set_instance(oldItem);
        routineInstructions=0; constructorInstructions=0; onEquipCalls=0;
        actual=std::make_unique<Npc>(world,symbol->index(),world.startPoint().name,policy);
        difference(name,"record constructor calls","0",std::to_string(constructorInstructions));
        if(syntheticEquip) {
          difference(name,"record on_equip executed","yes",onEquipCalls>0 ? "yes" : "no");
          difference(name,"record final level","30",std::to_string(actual->handle().level));
          }
        const auto actualStrings=temporaryStrings();
        difference(name,"VM temporary string count",std::to_string(expectedStrings.size()),std::to_string(actualStrings.size()));
        for(size_t index=0;index<std::min(expectedStrings.size(),actualStrings.size());++index)
          difference(name,"VM temporary string["+std::to_string(index)+"]",Bench::bodyJson(expectedStrings[index]),Bench::bodyJson(actualStrings[index]));
        difference(name,"game random generator differs",expectedRandom,script.randomState(nullptr));
        prepareSyntheticPhysics(*actual);
        const auto actualFields=npcRecordFields(*actual);
        for(const auto& [field,value]:expectedFields) difference(name,field,value,actualFields.at(field));
        archiveDifference(name,"NPC",expectedState,npcState(*actual));
        archiveDifference(name,"ITEM",expectedItem,itemState(script));
        if(capturedDaily)
          difference(name,"record NPC startup daily_routine must use the configured routine record/VM path",
                     routineRecords ? "record" : "VM",routineInstructions==0 ? "record" : "VM");
        difference(name,"SELF restored","yes",vm.global_self()->get_instance()==oldSelf ? "yes" : "no");
        }
      catch(const std::exception& error) { difference(name,"creation exception","none",error.what()); }
      vm.observe_instructions({});
      if(recordsActive) NpcDefinitionContract::definitions(script).npcs.swap(records.npcs);
      actual.reset(); expected.reset();
      symbol->set_instance(previous);
      vm.global_self()->set_instance(oldSelf);
      vm.global_item()->set_instance(oldItem);
      vm.unsafe_set_gi(previousContext);
      ++count;
      }
    }
  catch(...) {
    vm.observe_instructions({});
    NpcDefinitionContract::definitions(script)=std::move(loaded);
    vm.global_self()->set_instance(oldSelf);
    vm.global_item()->set_instance(oldItem);
    NpcDefinitionContract::random(script)=random;
    throw;
    }
  NpcDefinitionContract::definitions(script)=std::move(loaded);
  vm.global_item()->set_instance(oldItem);
  NpcDefinitionContract::random(script)=random;
  require(count==library.npcs.size()+1 && dailyCount>0,"all captured NPCs and actual startup routines must be compared");
  for(const auto& value:differences) std::fprintf(stderr,"[npc-definition-difference] %s\n",value.c_str());
  std::fprintf(stderr,"NPC definition differences: %zu NPCs, %zu fields\n",differentNpcs.size(),differences.size());
  if(!differences.empty())
    throw std::runtime_error("live NPC differs: "+std::to_string(differences.size())+" differences; "+differences.front());
  std::printf("Live NPC equality: %zu captured NPCs and one director template; %zu startup daily routines; record, appearance, ordered inventory, equipped and active items, talents, attitudes, routines, ITEM and RNG match\n",
              count-1,dailyCount);
  }

void npcDefinitionsDisabled(World& world) {
  auto& script=world.script();
  auto& vm=script.getVm();
  require(NpcDefinitionContract::definitions(script).npcs.empty(),"NPC switch must leave the game's definitions empty");
  require(NpcDefinitionContract::items(script).items.empty(),"item switch must leave the game's definitions empty");
  require(NpcDefinitionContract::routines(script).routines.empty(),"routine switch must leave the game's definitions empty");
  const auto* npcSymbol=vm.find_symbol_by_name("NPC_NO_ID");
  const auto* itemSymbol=vm.find_symbol_by_name("IT_NPC_RING");
  const auto* routine=vm.find_symbol_by_name("RTN_START_123");
  require(npcSymbol && itemSymbol && routine,"disabled creation fixtures must exist");
  size_t constructors=0,itemConstructors=0,dailyRoutines=0;
  vm.observe_instructions([&](const daedalus::DaedalusInstruction&) {
    if(vm.pc()==npcSymbol->address()) ++constructors;
    if(vm.pc()==itemSymbol->address()) ++itemConstructors;
    if(vm.pc()==routine->address()) ++dailyRoutines;
    });
  try {
    Npc npc(world,npcSymbol->index(),world.startPoint().name,NpcProcessPolicy::AiNormal);
    require(constructors>0,"disabled NPC records must execute a constructor in the game VM");
    require(itemConstructors>0,"disabled item records must execute a constructor in the game VM");
    require(dailyRoutines>0,"disabled routine records must execute the startup routine in the game VM");
    require(npc.handle().attribute[0]==40 && npc.handle().exp==30 && npc.handle().level==30,
            "disabled game creation must preserve helper and field order");
    vm.observe_instructions({});
    }
  catch(...) { vm.observe_instructions({}); throw; }
  std::printf("Live record switches: empty NPC/item/routine sections; VM constructors %zu NPC/%zu item, startup routines %zu\n",
              constructors,itemConstructors,dailyRoutines);
  }

void moverChildSave() {
  const auto* game=std::getenv("GOTHIC_DIR");
  require(game!=nullptr && *game!='\0',"GOTHIC_DIR must point to prepared Gothic II settings");
  const char* args[]={"gothic-placed-items-contract","-g",game};
  CommandLine commandLine(3,args);
  Resources resources;
  Gothic gothic;
  gothic.isNpcInDialogFn=[](const Npc*) { return false; };
  gothic.setupGlobalScripts();
  GameMusic music;
  gothic.setGame(std::make_unique<GameSession>("NEWWORLD.ZEN"));
  require(gothic.player()!=nullptr,"bench helper fixture has a player");
  routineTables(*gothic.player());
  npcDefinitions(*gothic.world());
  if(LoadProbe::enabled()) {
    auto& script=gothic.world()->script();
    auto& vm=script.getVm();
    auto* gold=vm.find_symbol_by_name("ITMI_GOLD");
    auto* broken=vm.find_symbol_by_name("ITMI_INNOSEYE_BROKEN_MIS");
    require(gold && broken,"query probe fixtures must contain captured and uncaptured items");
    const auto goldBefore=gold->get_instance(),brokenBefore=broken->get_instance();
    const auto part=std::find_if(std::begin(LoadProbe::Vobs::partNames),std::end(LoadProbe::Vobs::partNames),
                                 [](const char* name) { return std::strcmp(name,"script_item")==0; });
    require(part!=std::end(LoadProbe::Vobs::partNames),"item loading probe must exist");
    auto& vobs=LoadProbe::Vobs::parts[size_t(part-std::begin(LoadProbe::Vobs::partNames))];
    const bool previousVobs=LoadProbe::Vobs::active.exchange(true);
    {
      LoadProbe::Startup::Scope loading(LoadProbe::Startup::Other,true,"placed-items");
      const auto count=vobs.count.load(),ns=vobs.ns.load(),inclusiveNs=vobs.inclusiveNs.load();
      auto& startup=LoadProbe::Startup::counters[LoadProbe::Startup::Script];
      const auto startupBefore=startup;
      const auto constructors=LoadProbe::Items::constructors.load();
      script.createItemForQuery(gold->index());
      script.createItemForQuery(broken->index());
      require(vobs.count.load()==count && vobs.ns.load()==ns && vobs.inclusiveNs.load()==inclusiveNs &&
              startup.count==startupBefore.count && startup.ns==startupBefore.ns && startup.inclusiveNs==startupBefore.inclusiveNs,
              "item queries must not count or time world loading");
      require(LoadProbe::Items::constructors.load()==constructors+1,"only the uncaptured query must execute a VM constructor");
      script.createItem(gold->index());
      require(vobs.count.load()==count+1 && startup.count==startupBefore.count+1,
              "world item creation must increment both armed loading probes");
    }
    gold->set_instance(goldBefore);
    broken->set_instance(brokenBefore);
    LoadProbe::Vobs::active.store(previousVobs);
    std::puts("Item query probes: loading counts and times unchanged; one VM fallback; world creation counted");
  }
  else std::puts("SKIP item query probes: OPENGOTHIC_LOADPROBE=1 is not enabled");
  benchHelpers(*gothic.player());
  const auto findChild=[](World& world) -> const WorldObjects::PlacedItem& {
    const auto& catalogue=world.placedItemCensus();
    const auto child=std::find_if(catalogue.begin(),catalogue.end(),[](const auto& row) { return row.zenId==2839; });
    require(child!=catalogue.end() && child->item!=nullptr,
            "the authored catalogue must contain Item 2839 below mover 2838 on both startup and reload");
    const auto attachments=world.vobAttachmentSnapshot(2838);
    require(std::any_of(attachments.begin(),attachments.end(),[](const auto& row) {
              return row.zenId==2839 && row.parentZenId==2838;
              }),"the save fixture must contain an actual mover-child Item");
    return *child;
    };
  const auto& child=findChild(*gothic.world());
  gothic.world()->script().getVm().global_item()->set_instance(
    std::static_pointer_cast<daedalus::IItem>(child.item->handlePtr()));
  const auto catalogueSize=gothic.world()->placedItemCensus().size();
  const auto countItems=[](World& world) {
    uint32_t count=0;
    while(world.itmById(count)!=nullptr)
      ++count;
    return count;
    };
  const auto itemCount=countItems(*gothic.world());
  const auto instance=child.instance;
  const auto verdict=child.validation.disposition;
  auto savedPose=child.origin;
  savedPose.translate(25,0,0);
  child.item->setObjMatrix(savedPose);
  std::vector<uint8_t> bytes;
  {
    Tempest::MemWriter writer(bytes);
    Serialize out(writer);
    Tempest::Pixmap preview(1,1,Tempest::TextureFormat::RGBA8);
    std::memset(preview.data(),0,4);
    gothic.gameSession()->save(out,"placed mover child",preview);
  }
  {
    Tempest::MemReader reader(bytes);
    Serialize in(reader);
    require(in.setEntry("worlds/NEWWORLD.ZEN/placed-items"),"mover-child identities must be written to /placed-items");
  }
  gothic.clearGame().reset();
  {
    Tempest::MemReader reader(bytes);
    Serialize in(reader);
    gothic.setGame(std::make_unique<GameSession>(in));
  }
  const auto& restored=findChild(*gothic.world());
  require(gothic.world()->script().getVm().global_item()->get_instance()==
          std::static_pointer_cast<daedalus::IItem>(restored.item->handlePtr()),
          "the saved global Item reference must resolve to the restored world Item");
  require(gothic.world()->placedItemCensus().size()==catalogueSize && countItems(*gothic.world())==itemCount,
          "loading a mover-child identity must preserve the catalogue and avoid duplicate Items");
  require(restored.instance==instance && restored.validation.disposition==verdict &&
          restored.item->placedItemZenId()==2839,
          "the saved mover-child identity must restore its original census verdict");
  for(size_t i=0;i<16;++i)
    require(restored.item->localTransform().data()[i]==savedPose.data()[i],
            "a moved mover-child Item must recover its exact saved pose through /placed-items");
  std::printf("Mover child 2838 -> 2839: catalogue %zu, items %u, identity and saved pose restored\n",catalogueSize,itemCount);
  gothic.clearGame().reset();
  }

}

int main() {
  try {
    census();
    rootedAndStack();
    censusCache();
    slopes();
    penetrationLimit();
    groundBelowSupport();
    asymmetricLowestPoint();
    irregularSupport();
    npcLiveSynthetic(npcDefinitions,npcDefinitionsDisabled);
    const auto* noGameData=std::getenv("OPENGOTHIC_CONTRACT_NO_GAME_DATA");
    if(noGameData && std::strcmp(noGameData,"1")==0)
      std::puts("SKIP real game corpus and mover-child save: OPENGOTHIC_CONTRACT_NO_GAME_DATA=1");
    else moverChildSave();
    std::puts("Placed item census contracts passed");
    return EXIT_SUCCESS;
    }
  catch(const std::exception& error) {
    std::fprintf(stderr,"FAIL: placed item census contract: %s\n",error.what());
    return EXIT_FAILURE;
    }
  }
