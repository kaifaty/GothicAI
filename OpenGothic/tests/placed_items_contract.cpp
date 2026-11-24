#include "physics/dynamicworld.h"
#include "physics/physicsnoise.h"
#include "physics/placedcensuscache.h"
#include "graphics/bounds.h"
#include "graphics/mesh/protomesh.h"
#include "graphics/mesh/skeleton.h"
#include "graphics/mesh/submesh/packedmesh.h"
#include "resources.h"
#include "commandline.h"
#include "camera.h"
#include "gamemusic.h"
#include "gothic.h"
#include "game/gamescript.h"
#include "game/definitions/basescripts.h"
#include "game/definitions/baseprogram.h"
#include "settings.h"
#include "game/serialize.h"
#include "npcsnapshot.h"
#include "npcsnapshotfields.h"
#include "programdata.h"
#include "routinedefinitions.h"
#include "npcdefinitions.h"
#include "npchelperdefinitions.h"
#include "game/npchelperruntime.h"
#include "placementdefinitions.h"
#include "game/npcdefinitionruntime.h"
#include "game/placementdefinitionruntime.h"
#include "game/activitydefinitionruntime.h"
#include "game/npcactivityhost.h"
#include "activitydefinitions.h"
#include "npcdayjournal.h"
#include "npcdaymeasurement.h"
#include "world/activityevents.h"
#include "world/mobreservations.h"
#include "world/npcactivityprobe.h"
#include "world/objects/interactive.h"
#include "world/waypoint.h"
#include "game/gamesession.h"
#include "game/savegameheader.h"
#include "sha256.h"
#include "loadprobe.h"
#include "world/objects/item.h"
#include "graphics/itemview.h"
#include "world/objects/npc.h"
#include "world/benchhooks.h"
#include "testbench.h"
#include "testbench_internal.h"
#include "gameservices.h"
#include "graphics/visualfx.h"
#include "graphics/pfx/particlefx.h"
#include "dialoggraph.h"
#include "npccontroller.h"
#include "storygraph/bytecode.h"
#include "storygraph/exprjson.h"
#include "world/bullet.h"
#include "game/physicssave.h"

#include <rapidjson/document.h>
#include <Tempest/MemReader>
#include <Tempest/MemWriter>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <array>
#include <bit>
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
#include <functional>
#include <optional>
#include <tuple>

void npcLiveSynthetic(void (*compare)(World&), void (*disabled)(World&),
                      void (*placement)(World&), void (*placementDisabled)(World&), void (*activities)(World&));
#include "npc_world_contract_access.h"
namespace {

using NpcWorldFixture::activityNpc;
using NpcWorldFixture::activityQueue;
using NpcWorldFixture::companionPlayerPerceptions;
using NpcWorldFixture::oldWorldNpcCases;
using NpcWorldFixture::fightSoundPerceptions;
using NpcWorldFixture::loadSavedNpc;
using NpcWorldFixture::require;
using NpcWorldFixture::savedNpc;

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
      NpcDefinitionContract::runRoutine(script,npc,function->index());
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
  // Compare entry payloads: ZIP modification times are outside NPC state.
  return archiveEntries(savedNpc(npc));
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

struct HelperLocal {
  daedalus::DaedalusSymbol* symbol;
  uint16_t index;
  std::variant<int32_t,float,std::string,std::shared_ptr<daedalus::DaedalusInstance>> value;
  };
std::vector<HelperLocal> helperLocals(daedalus::DaedalusVm& vm,bool saveTalents) {
  using Type=daedalus::DaedalusDataType;
  std::vector<HelperLocal> values;
  for(const auto& entry:vm.symbols()) {
    const auto& name=entry.name();
    if(entry.is_member() || entry.is_const() ||
       !(name.starts_with("B_SETFIGHTSKILLS.") || name.starts_with("B_ADDFIGHTSKILL.") ||
         name.starts_with("B_RAISEFIGHTTALENT.") || name.starts_with("B_RAISEREALFIGHTTALENTPERCENT.") ||
         name.starts_with("B_SETNPCVISUAL.") || name.starts_with("B_SETATTRIBUTESTOCHAPTER.") ||
         name.starts_with("B_CREATEAMBIENTINV.") || name.starts_with("B_CREATEAMBIENTINV_") ||
         (saveTalents && NpcDefinitions::isPlayerTalentArray(name)))) continue;
    auto* symbol=vm.find_symbol_by_index(entry.index());
    for(uint16_t index=0;index<entry.count();++index) {
      if(entry.type()==Type::INT) values.push_back({symbol,index,entry.get_int(index)});
      else if(entry.type()==Type::FLOAT) values.push_back({symbol,index,entry.get_float(index)});
      else if(entry.type()==Type::STRING) values.push_back({symbol,index,entry.get_string(index)});
      else if(entry.type()==Type::INSTANCE) values.push_back({symbol,index,symbol->get_instance()});
      }
    }
  return values;
  }
void restoreHelperLocals(const std::vector<HelperLocal>& values) {
  for(const auto& entry:values)
    std::visit([&](const auto& value) {
      using Type=std::decay_t<decltype(value)>;
      if constexpr(std::is_same_v<Type,int32_t>) entry.symbol->set_int(value,entry.index);
      else if constexpr(std::is_same_v<Type,float>) entry.symbol->set_float(value,entry.index);
      else if constexpr(std::is_same_v<Type,std::string>) entry.symbol->set_string(value,entry.index);
      else entry.symbol->set_instance(value);
      },entry.value);
  }
std::map<std::string,std::string> helperLocalText(daedalus::DaedalusVm& vm,const Npc& npc,bool saveTalents) {
  std::map<std::string,std::string> result;
  for(const auto& entry:helperLocals(vm,saveTalents))
    result[entry.symbol->name()+"["+std::to_string(entry.index)+"]"]=std::visit([&](const auto& value) -> std::string {
      using Type=std::decay_t<decltype(value)>;
      if constexpr(std::is_same_v<Type,std::string>) return Bench::bodyJson(value);
      else if constexpr(std::is_same_v<Type,int32_t>) return std::to_string(value);
      else if constexpr(std::is_same_v<Type,float>) return std::to_string(std::bit_cast<uint32_t>(value));
      else return value==std::static_pointer_cast<daedalus::INpc>(npc.handlePtr()) ? "self" : value ? "symbol "+std::to_string(value->symbol_index()) : "null";
      },entry.value);
  return result;
  }

void compareNpcDefinitions(World& world, bool helpersOnly, bool isolated) {
  auto& script=world.script();
  auto& vm=script.getVm();
  const auto base=baseProgramIndex().dir.parent_path();
  const auto program=NpcSnapshot::load(base);
  const auto library=NpcDefinitions::read(base,program.sha256);
  const auto routines=RoutineDefinitions::read(base,program.sha256);
  require(!library.npcs.empty(),"the real program must have captured NPC definitions");
  auto records=NpcDefinitionRuntime::resolve(library,vm);
  const auto helpers=NpcHelperDefinitions::read(base,program.sha256);
  require(helpers.helpers.size()==6,"all six helper forms must be captured in this program");
  constexpr uint32_t contractSeed=0x12345679u;
  if(library.npcs.contains("NPC_NO_ID")) {
    const auto* previous=vm.find_symbol_by_name("DRAW_BEFORE_AMBIENT");
    require(previous!=nullptr,"the random-order fixture must have its preceding helper");
    const auto firstInstruction=vm.instruction_at(previous->address());
    require(firstInstruction.op==daedalus::DaedalusOpcode::PUSHI && firstInstruction.immediate>0,
            "the preceding helper must declare a positive random bound");
    const auto bound=uint32_t(firstInstruction.immediate);
    std::mt19937 stream(contractSeed);
    if(isolated) {
      std::seed_seq sequence{contractSeed,0x415u,0x5C21D7u};
      stream.seed(sequence);
      }
    const auto first=uint32_t(stream()),second=uint32_t(stream());
    const auto& ambient=std::get<NpcHelperDefinitions::AmbientInventory>(helpers.helpers.at("B_CREATEAMBIENTINV"));
    const auto& table=ambient.guilds.at(2);
    const auto normalRoll=second%uint32_t(ambient.randomBound),reorderedRoll=first%uint32_t(ambient.randomBound);
    require(first%bound!=second%bound,
            "the random-order fixture seed must produce different AIVAR[1] values when the helper moves");
    require(table.rolls.at(normalRoll)!=table.rolls.at(reorderedRoll),
            "the random-order fixture seed must select different ambient items when the helper moves");
    std::printf("Random-order fixture (%s): AIVAR[1] %u/%u, ambient rolls %u/%u; both outcomes differ\n",
                isolated ? "NPC" : "shared",first%bound,second%bound,normalRoll,reorderedRoll);
    }
  const auto& gameRecords=NpcDefinitionContract::definitions(script).npcs;
  require(!gameRecords.empty(),"the game must load NPC definitions when records are enabled");
  require(gameRecords.size()==records.npcs.size(),"the game must load all captured NPC definitions");
  for(const auto& [symbol,steps]:records.npcs) {
    require(gameRecords.contains(symbol),"the game must load each captured NPC definition");
    require(gameRecords.at(symbol).size()==steps.size(),"the game's NPC definition step count must match its section");
    }
  const auto* itemSetting=std::getenv("OPENGOTHIC_ITEM_RECORDS");
  if(itemSetting==nullptr || std::strcmp(itemSetting,"0")!=0) {
    const auto items=ItemDefinitions::read(base,program.sha256);
    const auto& gameItems=NpcDefinitionContract::items(script).items;
    require(!gameItems.empty() && gameItems.size()==items.items.size(),"the game must load all captured item definitions");
    for(const auto& [name,item]:items.items) {
      (void)item;
      require(gameItems.contains(vm.find_symbol_by_name(name)->index()),"the game must load each captured item definition");
      }
    }
  const auto* routineSetting=std::getenv("OPENGOTHIC_ROUTINE_RECORDS");
  if(routineSetting==nullptr || std::strcmp(routineSetting,"0")!=0) {
    const auto& gameRoutines=NpcDefinitionContract::routines(script).routines;
    require(!gameRoutines.empty() && gameRoutines.size()==routines.routines.size(),"the game must load all captured routine definitions");
    for(const auto& [name,rows]:routines.routines) {
      const auto symbol=vm.find_symbol_by_name(name)->index();
      require(gameRoutines.contains(symbol) && gameRoutines.at(symbol).size()==rows.size(),
              "the game's routine definitions must match their section");
      }
    }
  auto loaded=std::move(NpcDefinitionContract::definitions(script));
  NpcDefinitionContract::definitions(script).helpers=NpcHelperRuntime::resolve(helpers,vm);
  const auto random=NpcDefinitionContract::random(script);
  const auto isolatedRandom=NpcDefinitionContract::isolatedRandom(script);
  NpcDefinitionContract::isolatedRandom(script).clear();
  const auto* helperSetting=std::getenv("OPENGOTHIC_NPC_HELPER_RECORDS");
  const std::optional<std::string> savedHelperSetting=helperSetting ? std::optional<std::string>(helperSetting) : std::nullopt;
  auto helperRecords=[](bool enabled) {
    if(enabled) unsetenv("OPENGOTHIC_NPC_HELPER_RECORDS");
    else setenv("OPENGOTHIC_NPC_HELPER_RECORDS","0",1);
    };
  struct RestoreHelperSetting {
    const std::optional<std::string>& saved;
    ~RestoreHelperSetting() {
      if(saved) setenv("OPENGOTHIC_NPC_HELPER_RECORDS",saved->c_str(),1);
      else unsetenv("OPENGOTHIC_NPC_HELPER_RECORDS");
      }
    } restoreHelperSetting{savedHelperSetting};
  // The map stores only the address. Reserve the NPC's storage before construction so its
  // very first Hlp_Random can use a private stream without a production creation hook.
  auto create=[&](uint32_t symbol,NpcProcessPolicy policy) {
    auto* storage=static_cast<Npc*>(::operator new(sizeof(Npc)));
    if(isolated) {
      std::seed_seq sequence{contractSeed,0x415u,0x5C21D7u};
      NpcDefinitionContract::isolatedRandom(script)[storage].seed(sequence);
      }
    try { return std::unique_ptr<Npc>(::new (static_cast<void*>(storage)) Npc(world,symbol,world.startPoint().name,policy)); }
    catch(...) {
      script.releaseIsolatedRandom(storage);
      ::operator delete(storage);
      throw;
      }
    };
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
                         script.ownSymbols().addNpc(vm,"DIR_NPC_RECORD_CONTRACT",templateName,error);
  NpcDefinitionContract::checkDirectorNpc(script,own,error);
  names.push_back(own->name());
  std::set<uint32_t> routineAddresses;
  for(const auto& [name,rows]:routines.routines) {
    (void)rows;
    routineAddresses.insert(vm.find_symbol_by_name(name)->address());
    }
  const auto* configured=std::getenv("OPENGOTHIC_ROUTINE_RECORDS");
  const bool routineRecords=configured==nullptr || std::strcmp(configured,"0")!=0;
  std::set<uint32_t> helperAddresses;
  for(const auto& [name,definition]:helpers.helpers) {
    (void)definition;
    helperAddresses.insert(vm.find_symbol_by_name(name)->address());
    }
  // Equipment callbacks remain scripts, including the unequip/equip pair when the
  // constructor's final auto-equip selects an item already in its weapon slot.
  std::set<uint32_t> itemCallbackAddresses;
  for(const auto& [symbol,item]:NpcDefinitionContract::items(script).items) {
    (void)symbol;
    for(const auto function:{item.on_equip,item.on_unequip})
      if(const auto* callback=vm.find_symbol_by_index(uint32_t(function)); callback && callback->type()==daedalus::DaedalusDataType::FUNCTION)
        itemCallbackAddresses.insert(callback->address());
    }
  size_t count=0,dailyCount=0,referenceHelperCalls=0;
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
      const bool saveTalents=program.sha256==NpcDefinitions::TalentWriteProgram && NpcDefinitions::writesPlayerTalents(name);
      const auto localBaseline=helperLocals(vm,saveTalents);
      try {
        const auto policy=name=="PC_HERO" ? NpcProcessPolicy::Player : NpcProcessPolicy::AiNormal;
        const bool syntheticEquip=library.npcs.contains("NPC_NO_ID") &&
                                  (name=="NPC_NO_ID" || name=="DIR_NPC_RECORD_CONTRACT");
        const auto* onEquip=vm.find_symbol_by_name("ON_EQUIP");
        if(syntheticEquip) require(onEquip!=nullptr,"synthetic equipment has an on_equip callback");
        const auto onEquipAddress=onEquip ? onEquip->address() : uint32_t(-1);
        size_t routineInstructions=0,constructorInstructions=0,onEquipCalls=0,helperCalls=0,itemCallbackDepth=0;
        int32_t joraAfterEquip=-1;
        const auto* fightSkills=vm.find_symbol_by_name("B_SETFIGHTSKILLS");
        const auto constructorAddress=symbol->address();
        vm.observe_instructions([&](const daedalus::DaedalusInstruction& instruction) {
          if(itemCallbackAddresses.contains(vm.pc())) ++itemCallbackDepth;
          if(routineAddresses.contains(vm.pc())) ++routineInstructions;
          if(vm.pc()==constructorAddress) ++constructorInstructions;
          if(vm.pc()==onEquipAddress) ++onEquipCalls;
          if(helperAddresses.contains(vm.pc()) && itemCallbackDepth==0) ++helperCalls;
          if(itemCallbackDepth>0 && instruction.op==daedalus::DaedalusOpcode::BL) ++itemCallbackDepth;
          if(itemCallbackDepth>0 && instruction.op==daedalus::DaedalusOpcode::RSR) --itemCallbackDepth;
          if(name=="VLK_408_JORA" && fightSkills && vm.pc()==fightSkills->address()) {
            const auto self=std::static_pointer_cast<daedalus::INpc>(vm.global_self()->get_instance());
            joraAfterEquip=self->hitchance[1];
            }
          });
        if(helpersOnly) NpcDefinitionContract::definitions(script).npcs.swap(records.npcs);
        else NpcDefinitionContract::definitions(script).npcs.clear();
        recordsActive=helpersOnly;
        helperRecords(false);
        script.setSeed(contractSeed);
        vm.global_item()->set_instance(oldItem);
        expected=create(symbol->index(),policy);
        referenceHelperCalls+=helperCalls;
        difference(name,"reference constructor executed",helpersOnly ? "no" : "yes",constructorInstructions>0 ? "yes" : "no");
        if(syntheticEquip) {
          difference(name,"reference on_equip executed","yes",onEquipCalls>0 ? "yes" : "no");
          difference(name,"reference final attribute[0]","40",std::to_string(expected->handle().attribute[0]));
          difference(name,"reference helper result","30",std::to_string(expected->handle().exp));
          difference(name,"reference final level","30",std::to_string(expected->handle().level));
          difference(name,"reference equipment and B_SetFightSkills hitchance[1]","40",std::to_string(expected->handle().hitchance[1]));
          }
        if(name=="VLK_408_JORA") {
          difference(name,"hitchance[1] after on_equip","10",std::to_string(joraAfterEquip));
          difference(name,"VM hitchance[1] after B_SetFightSkills","40",std::to_string(expected->handle().hitchance[1]));
          }
        prepareSyntheticPhysics(*expected);
        const auto expectedFields=npcRecordFields(*expected);
        const auto expectedState=npcState(*expected);
        const auto expectedItem=itemState(script);
        const auto expectedRandom=script.randomState(nullptr);
        const auto expectedActorRandom=script.randomState(expected.get());
        auto temporaryStrings=[&] {
          std::vector<std::string> result;
          for(const auto& value:vm.symbols())
            if(value.name()=="$PHOENIX_FAKE_STRINGS")
              for(uint16_t index=0;index<value.count();++index) result.push_back(value.get_string(index));
          return result;
          };
        const auto expectedStrings=temporaryStrings();
        const auto expectedLocals=helperLocalText(vm,*expected,saveTalents);
        const auto expectedDaily=vm.find_symbol_by_index(uint32_t(expected->handle().daily_routine));
        const bool capturedDaily=expectedDaily && routines.routines.contains(expectedDaily->name());
        if(capturedDaily) {
          ++dailyCount;
          difference(name,"NPC startup daily_routine must use the configured routine record/VM path",
                     routineRecords ? "record" : "VM",routineInstructions==0 ? "record" : "VM");
          }
        if(!helpersOnly) NpcDefinitionContract::definitions(script).npcs.swap(records.npcs);
        recordsActive=true;
        helperRecords(true);
        restoreHelperLocals(localBaseline);
        script.setSeed(contractSeed);
        vm.global_item()->set_instance(oldItem);
        routineInstructions=0; constructorInstructions=0; onEquipCalls=0; helperCalls=0; itemCallbackDepth=0;
        actual=create(symbol->index(),policy);
        difference(name,"record constructor calls","0",std::to_string(constructorInstructions));
        difference(name,"captured helper VM calls","0",std::to_string(helperCalls));
        if(syntheticEquip) {
          difference(name,"record on_equip executed","yes",onEquipCalls>0 ? "yes" : "no");
          difference(name,"record final level","30",std::to_string(actual->handle().level));
          difference(name,"Jora equipment and B_SetFightSkills hitchance[1]","40",std::to_string(actual->handle().hitchance[1]));
          }
        if(name=="VLK_408_JORA")
          difference(name,"record hitchance[1] after B_SetFightSkills","40",std::to_string(actual->handle().hitchance[1]));
        const auto actualLocals=helperLocalText(vm,*actual,saveTalents);
        for(const auto& [local,value]:expectedLocals)
          if(helpersOnly || (saveTalents && local.starts_with("PLAYER_TALENT_")))
            difference(name,"constructor symbol "+local,value,actualLocals.at(local));
        const auto actualStrings=temporaryStrings();
        difference(name,"VM temporary string count",std::to_string(expectedStrings.size()),std::to_string(actualStrings.size()));
        for(size_t index=0;index<std::min(expectedStrings.size(),actualStrings.size());++index)
          difference(name,"VM temporary string["+std::to_string(index)+"]",Bench::bodyJson(expectedStrings[index]),Bench::bodyJson(actualStrings[index]));
        difference(name,"game random generator differs",expectedRandom,script.randomState(nullptr));
        difference(name,"NPC random generator differs",expectedActorRandom,script.randomState(actual.get()));
        prepareSyntheticPhysics(*actual);
        const auto actualFields=npcRecordFields(*actual);
        for(const auto& [field,value]:expectedFields) difference(name,field,value,actualFields.at(field));
        archiveDifference(name,"NPC",expectedState,npcState(*actual));
        archiveDifference(name,"ITEM",expectedItem,itemState(script));
        if(capturedDaily)
          difference(name,"record NPC startup daily_routine must use the configured routine record/VM path",
                     routineRecords ? "record" : "VM",routineInstructions==0 ? "record" : "VM");
        difference(name,"SELF restored","yes",vm.global_self()->get_instance()==oldSelf ? "yes" : "no");
        difference(name,"VM member context restored","yes",vm.unsafe_get_gi()==previousContext ? "yes" : "no");
        if(name=="NPC_NO_ID") {
          actual->handle().aivar[0]=73;
          constructorInstructions=0;
          require(actual->transformInto(symbol->index()),"the synthetic NPC must transform");
          require(constructorInstructions>0,"transformation must execute its constructor in the game VM");
          require(actual->handle().exp_next==73,"transformation must read the reused NPC's untouched field");
          }
        }
      catch(const std::exception& error) { difference(name,"creation exception","none",error.what()); }
      vm.observe_instructions({});
      if(recordsActive) NpcDefinitionContract::definitions(script).npcs.swap(records.npcs);
      restoreHelperLocals(localBaseline);
      script.releaseIsolatedRandom(actual.get());
      script.releaseIsolatedRandom(expected.get());
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
    NpcDefinitionContract::isolatedRandom(script)=isolatedRandom;
    throw;
    }
  NpcDefinitionContract::definitions(script)=std::move(loaded);
  vm.global_item()->set_instance(oldItem);
  NpcDefinitionContract::random(script)=random;
  NpcDefinitionContract::isolatedRandom(script)=isolatedRandom;
  std::fprintf(stderr,"NPC definition coverage (%s, %s): %zu/%zu NPCs, %zu startup routines, %zu reference helper calls\n",
               helpersOnly ? "helpers" : "constructors",isolated ? "NPC" : "shared",count,library.npcs.size()+1,dailyCount,referenceHelperCalls);
  for(const auto& value:differences) std::fprintf(stderr,"[npc-definition-difference] %s\n",value.c_str());
  std::fprintf(stderr,"NPC definition differences: %zu NPCs, %zu fields\n",differentNpcs.size(),differences.size());
  if(!differences.empty())
    throw std::runtime_error("live NPC differs: "+std::to_string(differences.size())+" differences; "+differences.front());
  require(count==library.npcs.size()+1 && dailyCount>0,"all captured NPCs and actual startup routines must be compared");
  require(referenceHelperCalls>0,"the helpers-off reference must actually execute the captured helpers in the VM");
  std::printf("Live NPC equality (%s, %s): %zu captured NPCs and one director template; %zu startup daily routines; record, appearance, ordered inventory, equipped and active items, talents, attitudes, routines, ITEM and RNG match\n",
              helpersOnly ? "helper records vs helper VM" : "records vs constructors",isolated ? "NPC streams" : "shared stream",count-1,dailyCount);
  }

void npcDefinitions(World& world) {
  compareNpcDefinitions(world,false,false);
  compareNpcDefinitions(world,true,false);
  compareNpcDefinitions(world,true,true);
  }

void npcDefinitionsDisabled(World& world) {
  auto& script=world.script();
  auto& vm=script.getVm();
  require(NpcDefinitionContract::definitions(script).npcs.empty(),"NPC switch must leave the game's definitions empty");
  require(NpcDefinitionContract::items(script).items.empty(),"item switch must leave the game's definitions empty");
  require(NpcDefinitionContract::routines(script).routines.empty(),"routine switch must leave the game's definitions empty");
  require(NpcDefinitionContract::activities(script).phases.empty(),"activity switch must leave the game's definitions empty");
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

std::string placementInstance(const std::shared_ptr<daedalus::DaedalusInstance>& instance,World& world) {
  if(!instance) return "null";
  auto result=std::to_string(instance->symbol_index());
  if(const auto npc=std::dynamic_pointer_cast<daedalus::INpc>(instance))
    return result+":npc:"+std::to_string(world.npcId(static_cast<Npc*>(npc->user_ptr)));
  if(const auto item=std::dynamic_pointer_cast<daedalus::IItem>(instance)) {
    const auto id=world.itmId(static_cast<const ItemRecord*>(item.get()));
    if(id!=uint32_t(-1)) return result+":world-item:"+std::to_string(id);
    for(uint32_t index=0;index<world.npcCount();++index) {
      auto* inventoryItem=world.npcById(index)->getItem(item->symbol_index());
      if(inventoryItem && inventoryItem->handlePtr().get()==item.get())
        return result+":inventory:"+std::to_string(index);
      }
    }
  return result;
  }

std::string placementArgument(daedalus::DaedalusVm& vm,const daedalus::DaedalusSymbol& parameter,uint16_t depth,World& world) {
  using Type=daedalus::DaedalusDataType;
  const auto& frame=vm.stack_frame(depth);
  auto* symbol=frame.reference ? std::get<daedalus::DaedalusSymbol*>(frame.value) : nullptr;
  switch(parameter.type()) {
    case Type::STRING:
      require(symbol!=nullptr,"string trace argument must reference a VM symbol");
      return Bench::bodyJson(symbol->get_string(frame.index,frame.context.get()));
    case Type::INSTANCE:
      return placementInstance(symbol ? symbol->get_instance() :
          std::get<std::shared_ptr<daedalus::DaedalusInstance>>(frame.value),world);
    case Type::FLOAT: {
      const auto bits=symbol ? std::bit_cast<uint32_t>(symbol->get_float(frame.index,frame.context.get())) :
          std::holds_alternative<float>(frame.value) ? std::bit_cast<uint32_t>(std::get<float>(frame.value)) :
          uint32_t(std::get<int32_t>(frame.value));
      return "float:"+std::to_string(bits);
      }
    case Type::INT:
    case Type::FUNCTION:
      return std::to_string(symbol ? symbol->get_int(frame.index,frame.context.get()) : std::get<int32_t>(frame.value));
    default: throw std::runtime_error("unsupported placement trace parameter "+parameter.name());
    }
  }

std::map<std::string,std::string> placementSymbols(GameScript& script,bool locals) {
  using Type=daedalus::DaedalusDataType;
  auto& vm=script.getVm();
  std::map<std::string,std::string> result;
  for(const auto& symbol:vm.symbols()) {
    if(symbol.is_member() || symbol.is_const()) continue;
    const auto dot=symbol.name().find('.');
    const auto* owner=dot==std::string::npos ? nullptr : vm.find_symbol_by_name(symbol.name().substr(0,dot));
    const auto* parent=vm.find_symbol_by_index(symbol.parent());
    const bool local=(owner && owner->type()==Type::FUNCTION) || (parent && parent->type()==Type::FUNCTION);
    if(local!=locals) continue;
    if(symbol.type()==Type::INSTANCE)
      result[symbol.name()]=placementInstance(vm.find_symbol_by_index(symbol.index())->get_instance(),script.world());
    else for(uint16_t index=0;index<symbol.count();++index) {
      const auto key=symbol.name()+"["+std::to_string(index)+"]";
      if(symbol.type()==Type::STRING) result[key]=Bench::bodyJson(symbol.get_string(index));
      else if(symbol.type()==Type::INT || symbol.type()==Type::FUNCTION) result[key]=std::to_string(symbol.get_int(index));
      else if(symbol.type()==Type::FLOAT) result[key]=std::to_string(std::bit_cast<uint32_t>(symbol.get_float(index)));
      }
    }
  return result;
  }

std::vector<uint8_t> placementSnapshot(World& world,bool variables) {
  // Synthetic NPCs have no model, but their native save path still needs a ghost body.
  if(world.script().getVm().find_symbol_by_name("NPC_NO_ID"))
    for(uint32_t index=0;index<world.npcCount();++index)
      world.npcById(index)->setPhysic(world.physic()->addGhostObj(nullptr));
  std::vector<uint8_t> bytes;
  {
    Tempest::MemWriter writer(bytes);
    Serialize out(writer);
    world.save(out);
    out.setEntry("placement/quests"); world.script().saveQuests(out);
    out.setEntry("placement/perceptions"); world.script().savePerc(out);
    if(variables) {
      out.setEntry("placement/variables"); world.script().saveVar(out);
      }
    }
  return bytes;
  }

void placementRestore(World& world,const std::vector<uint8_t>& bytes) {
  auto& script=world.script();
  script.resetVarPointers();
  script.getVm().unsafe_set_gi(nullptr);
  Tempest::MemReader reader(bytes);
  Serialize in(reader);
  world.load(in);
  in.setEntry("placement/variables"); script.loadVar(in);
  in.setEntry("placement/quests"); script.loadQuests(in);
  in.setEntry("placement/perceptions"); script.loadPerc(in);
  }

void placementPrepareNpc(World& world,std::set<Npc*>& prepared) {
  auto& vm=world.script().getVm();
  if(!vm.find_symbol_by_name("NPC_NO_ID")) return;
  const auto self=std::dynamic_pointer_cast<daedalus::INpc>(vm.global_self()->get_instance());
  auto* npc=self ? static_cast<Npc*>(self->user_ptr) : nullptr;
  // Wld_InsertNpc sets direction immediately after construction, before a save can
  // supply the model-less fixture's body. Supply it during the first constructor call.
  if(npc && prepared.insert(npc).second) npc->setPhysic(world.physic()->addGhostObj(nullptr));
  }

void placementDefinitions(World& world) {
  auto& script=world.script();
  auto& vm=script.getVm();
  const auto base=baseProgramIndex().dir.parent_path();
  const auto program=NpcSnapshot::load(base);
  const auto library=PlacementDefinitions::read(base,program.sha256);
  require(!library.placements.empty(),"placement section must contain captured startup functions");
  const auto& gameRecords=NpcDefinitionContract::placements(script).placements;
  require(gameRecords.size()==library.placements.size() && !gameRecords.empty(),
          "the game must load every captured placement by default");
  const auto baseline=placementSnapshot(world,true);
  const auto baselineGlobals=placementSymbols(script,false);
  const auto random=NpcDefinitionContract::random(script);
  std::vector<std::string> differences;
  std::set<std::string> differentLocals;
  size_t functions=0,calls=0;
  auto difference=[&](const std::string& name,const std::string& field,const auto& expected,const auto& actual) {
    if(expected!=actual) differences.push_back(name+": "+field+" expected="+expected+" actual="+actual);
    };
  auto compareSymbols=[&](const std::string& name,const auto& expected,const auto& actual) {
    for(const auto& [field,value]:expected) {
      const auto found=actual.find(field);
      difference(name,field,value,found==actual.end() ? "<absent>" : found->second);
      }
    for(const auto& [field,value]:actual)
      if(!expected.contains(field)) difference(name,field,std::string("<absent>"),value);
    };
  auto compareWorld=[&](const std::string& name,const auto& expected,const auto& actual) {
    auto digest=[](const std::vector<uint8_t>& bytes) {
      return std::to_string(bytes.size())+":"+Sha256::hash({reinterpret_cast<const char*>(bytes.data()),bytes.size()});
      };
    for(const auto& [entry,payload]:expected) {
      const auto found=actual.find(entry);
      difference(name,entry,digest(payload),found==actual.end() ? "<absent>" : digest(found->second));
      }
    for(const auto& [entry,payload]:actual)
      if(!expected.contains(entry)) difference(name,entry,std::string("<absent>"),digest(payload));
    };
  try {
    for(const auto& [name,definition]:library.placements) {
      const auto* function=vm.find_symbol_by_name(name);
      require(function!=nullptr && gameRecords.contains(function->index()),"game placement must resolve its startup symbol");
      std::map<std::pair<std::string,uint16_t>,const daedalus::DaedalusSymbol*> writtenGlobals;
      for(const auto& step:definition.steps)
        if(const auto* write=std::get_if<PlacementDefinitions::GlobalWrite>(&step)) {
          const auto* symbol=vm.find_symbol_by_name(write->symbol);
          require(symbol!=nullptr,"recorded global must resolve in the game's VM");
          writtenGlobals.emplace(std::make_pair(write->symbol,write->index),symbol);
          }
      std::vector<std::string> expectedTrace,actualTrace;
      auto observe=[&](std::vector<std::string>& trace) {
        vm.observe_externals([&,target=&trace,prepared=std::set<Npc*>{}](const daedalus::DaedalusSymbol& external) mutable {
          placementPrepareNpc(world,prepared);
          auto line=external.name()+"(";
          const auto parameters=vm.find_parameters_for_function(&external);
          for(size_t index=0;index<parameters.size();++index) {
            if(index) line+=",";
            line+=placementArgument(vm,parameters[index],uint16_t(parameters.size()-index-1),world);
            }
          line+=") SELF="+placementInstance(vm.global_self()->get_instance(),world)+
                " OTHER="+placementInstance(vm.global_other()->get_instance(),world)+
                " ITEM="+placementInstance(vm.global_item()->get_instance(),world)+
                " context="+placementInstance(vm.unsafe_get_gi(),world);
          for(const auto& [target,symbol]:writtenGlobals) {
            line+=" "+target.first+"["+std::to_string(target.second)+"]=";
            switch(symbol->type()) {
              case daedalus::DaedalusDataType::INT: line+=std::to_string(symbol->get_int(target.second)); break;
              case daedalus::DaedalusDataType::FLOAT:
                line+="float:"+std::to_string(std::bit_cast<uint32_t>(symbol->get_float(target.second))); break;
              case daedalus::DaedalusDataType::STRING: line+=Bench::bodyJson(symbol->get_string(target.second)); break;
              default: throw std::runtime_error("unsupported global type in placement trace: "+target.first);
              }
            }
          target->push_back(std::move(line));
          });
        };
      placementRestore(world,baseline);
      NpcDefinitionContract::random(script)=random;
      script.startupFloor().begin();
      observe(expectedTrace);
      vm.push_int(0x12345678);
      const auto expectedPc=vm.pc();
      vm.call_function(name);
      const auto expectedStack=vm.pop_int();
      difference(name,"reference stack sentinel",std::to_string(0x12345678),std::to_string(expectedStack));
      difference(name,"reference VM program counter",std::to_string(expectedPc),std::to_string(vm.pc()));
      vm.observe_externals({});
      world.resetPositionToTA(); script.startupFloor().end();
      const auto expectedState=archiveEntries(placementSnapshot(world,false));
      const auto expectedGlobals=placementSymbols(script,false),expectedLocals=placementSymbols(script,true);
      const auto expectedRandom=script.randomState(nullptr);
      const auto expectedItem=itemState(script);
      const auto expectedCount=world.npcCount();
      placementRestore(world,baseline);
      NpcDefinitionContract::random(script)=random;
      script.startupFloor().begin();
      size_t rootInstructions=0;
      vm.observe_instructions([&](const daedalus::DaedalusInstruction&) {
        if(vm.pc()==function->address()) ++rootInstructions;
        });
      observe(actualTrace);
      vm.push_int(0x12345678);
      const auto actualPc=vm.pc();
      script.runStartup(name);
      difference(name,"record stack sentinel",std::to_string(expectedStack),std::to_string(vm.pop_int()));
      difference(name,"record VM program counter",std::to_string(actualPc),std::to_string(vm.pc()));
      vm.observe_externals({}); vm.observe_instructions({});
      world.resetPositionToTA(); script.startupFloor().end();
      difference(name,"record root VM instructions",std::string("0"),std::to_string(rootInstructions));
      difference(name,"external call count",std::to_string(expectedTrace.size()),std::to_string(actualTrace.size()));
      for(size_t index=0;index<std::min(expectedTrace.size(),actualTrace.size());++index)
        difference(name,"step "+std::to_string(index+1),expectedTrace[index],actualTrace[index]);
      compareWorld(name,expectedState,archiveEntries(placementSnapshot(world,false)));
      compareWorld(name+".ITEM",expectedItem,itemState(script));
      const auto actualGlobals=placementSymbols(script,false);
      compareSymbols(name,expectedGlobals,actualGlobals);
      if(name=="STARTUP_MUTABLE_ROOT")
        for(const auto& [global,value]:baselineGlobals)
          if(global.starts_with("$PHOENIX_FAKE_STRINGS[")) {
            difference(name,"reference "+global+" unchanged by string assignment",value,expectedGlobals.at(global));
            difference(name,"record "+global+" unchanged by string assignment",value,actualGlobals.at(global));
            }
      const auto actualLocals=placementSymbols(script,true);
      for(const auto& [local,value]:expectedLocals)
        if(actualLocals.contains(local) && value!=actualLocals.at(local)) differentLocals.insert(local);
      difference(name,"NPC count",std::to_string(expectedCount),std::to_string(world.npcCount()));
      difference(name,"random generator",expectedRandom,script.randomState(nullptr));
      ++functions; calls+=expectedTrace.size();
      }
    }
  catch(...) {
    vm.observe_externals({}); vm.observe_instructions({});
    script.startupFloor().end(); placementRestore(world,baseline);
    NpcDefinitionContract::random(script)=random;
    throw;
    }
  placementRestore(world,baseline); NpcDefinitionContract::random(script)=random;
  for(const auto& value:differences) std::fprintf(stderr,"[placement-difference] %s\n",value.c_str());
  for(const auto& local:differentLocals) std::printf("Placement VM local differs: %s\n",local.c_str());
  std::printf("Live placement equality: %zu functions, %zu external calls, %zu differences; %zu omitted-function local symbols differ\n",
              functions,calls,differences.size(),differentLocals.size());
  if(!differences.empty()) throw std::runtime_error("live placement differs: "+std::to_string(differences.size())+" differences; "+differences.front());
  }

void placementDefinitionsDisabled(World& world) {
  auto& script=world.script();
  auto& vm=script.getVm();
  require(NpcDefinitionContract::placements(script).placements.empty(),"placement switch must leave the game's definitions empty");
  const auto* function=vm.find_symbol_by_name("STARTUP_TEST");
  require(function!=nullptr,"disabled placement fixture must exist");
  size_t instructions=0;
  vm.observe_externals([&,prepared=std::set<Npc*>{}](const daedalus::DaedalusSymbol&) mutable {
    placementPrepareNpc(world,prepared);
    });
  vm.observe_instructions([&](const daedalus::DaedalusInstruction&) {
    if(vm.pc()==function->address()) ++instructions;
    });
  try { script.runStartup("STARTUP_TEST"); vm.observe_instructions({}); vm.observe_externals({}); }
  catch(...) { vm.observe_instructions({}); vm.observe_externals({}); throw; }
  require(instructions>0,"disabled placement records must execute the root function in the game's VM");
  std::printf("Live placement switch: empty section; root VM executions %zu\n",instructions);
  }

// D4.1: a routine activity from its record against the VM, at the same positions. The stream
// interleaves the engine calls (VM externals; the template's calls of a record phase, whose
// perception helper still runs in the VM), the queued commands and the phase results.
class ActivityStream final : public NpcActivityProbe {
  public:
    enum class Externals { Trace, Skip };

    ActivityStream(World& world,Npc& npc,Externals externals=Externals::Trace,std::function<void(uint32_t)> onInstruction={})
      :npc(npc),vm(world.script().getVm()) {
      previous=NpcActivityProbe::instance;
      NpcActivityProbe::instance=this;
      if(externals==Externals::Trace)
        vm.observe_externals([this](const daedalus::DaedalusSymbol& symbol) { external(symbol); });
      else
        vm.observe_externals({});
      vm.observe_instructions([this,onInstruction=std::move(onInstruction)](const daedalus::DaedalusInstruction&) {
        // Read the external return before the next instruction changes the stack.
        finish();
        if(onInstruction)
          onInstruction(vm.pc());
        });
      ActivityDefinitionRuntime::observer=[this](const Npc& who,std::string_view event) {
        if(&who==&this->npc && depth>0 && !event.starts_with("deferred ")) events.emplace_back(event);
        };
      }
    ~ActivityStream() override {
      NpcActivityProbe::instance=previous;
      vm.observe_externals({});
      vm.observe_instructions({});
      ActivityDefinitionRuntime::observer=nullptr;
      }
    std::vector<std::string> events;
    size_t records=0;

  private:
    using Type=daedalus::DaedalusDataType;
    std::string instance(const std::shared_ptr<daedalus::DaedalusInstance>& value) const {
      if(value==nullptr) return "null";
      const auto* self=static_cast<const daedalus::DaedalusInstance*>(std::static_pointer_cast<daedalus::INpc>(npc.handlePtr()).get());
      return value.get()==self ? "self" : "npc";
      }
    std::string value(const daedalus::DaedalusStackFrame& frame,Type type) const {
      auto* symbol=frame.reference ? std::get<daedalus::DaedalusSymbol*>(frame.value) : nullptr;
      switch(type) {
        case Type::STRING: return ActivityDefinitions::Recorder::text(symbol ? symbol->get_string(frame.index,frame.context.get()) : "");
        case Type::INSTANCE:
          return instance(symbol ? symbol->get_instance() : std::get<std::shared_ptr<daedalus::DaedalusInstance>>(frame.value));
        case Type::FLOAT:
          return std::to_string(symbol ? symbol->get_float(frame.index,frame.context.get()) :
                                std::holds_alternative<float>(frame.value) ? std::get<float>(frame.value) : float(std::get<int32_t>(frame.value)));
        default:
          return std::to_string(symbol ? symbol->get_int(frame.index,frame.context.get()) : std::get<int32_t>(frame.value));
        }
      }
    void external(const daedalus::DaedalusSymbol& symbol) {
      finish();
      if(depth==0) return;
      const auto parameters=vm.find_parameters_for_function(&symbol);
      std::string text=symbol.name()+"(";
      for(size_t i=0;i<parameters.size();++i)
        text+=(i ? "," : "")+value(vm.stack_frame(uint16_t(parameters.size()-1-i)),parameters[i].type());
      events.push_back(text+")");
      if(symbol.has_return()) { pending=events.size()-1; pendingType=symbol.rtype(); }
      }
    void finish() {
      if(!pending) return;
      events[*pending]+="->"+value(vm.stack_frame(0),pendingType);
      pending.reset();
      }
    void phaseBegin(const Npc* who,std::string_view symbol) override {
      ours.push_back(who==&npc);
      if(!ours.back()) return;
      ++depth;
      events.push_back("phase "+std::string(symbol));
      }
    void phaseEnd(int result) override {
      const bool own=ours.back();
      ours.pop_back();
      if(!own) return;
      finish();
      events.push_back("result "+std::to_string(result));
      --depth;
      }
    void phaseRecord() override { if(!ours.empty() && ours.back()) ++records; }
    void random(const Npc*,uint32_t bound,uint32_t result,bool) override {
      if(depth>0) events.push_back("random "+std::to_string(bound)+" "+std::to_string(result));
      }
    void queued(const Npc* who,const AiQueue::AiAction& a) override {
      if(who!=&npc) return;
      events.push_back(std::string("queue ")+AiQueue::actionName(uint32_t(a.act))+" "+std::to_string(a.i0)+" "+std::to_string(a.i1)+" "+
                       std::to_string(a.i2)+" "+a.s0+" "+a.s1+" "+(a.point ? a.point->name : "")+" "+
                       (a.target ? std::string(a.target->displayName()) : "")+" "+std::to_string(a.func.ptr));
      }
    void point(const Npc& who,const WayPoint* from,const WayPoint* to) override {
      if(&who==&npc) events.push_back("point "+(from ? from->name : "")+" "+(to ? to->name : ""));
      }
    void mob(const Npc& who,const Interactive& object,bool occupied) override {
      if(&who==&npc) events.push_back("mob "+std::to_string(object.zenId())+" "+std::to_string(occupied));
      }
    Npc&                     npc;
    daedalus::DaedalusVm&    vm;
    NpcActivityProbe*        previous=nullptr;
    std::vector<bool>        ours;
    size_t                   depth=0;
    std::optional<size_t>    pending;
    Type                     pendingType=Type::INT;
  };

struct ActivityPhases {
  size_t init=0,loop=0,end=0;
  };
ActivityPhases activityPhases(GameScript& script,const char* state) {
  const auto& st=script.aiState(script.findSymbolIndex(state));
  require(st.funcIni!=0 && st.funcLoop!=0 && st.funcEnd!=0,"activity state must have Init, Loop and End");
  return {st.funcIni,st.funcLoop,st.funcEnd};
  }

using ActivityAct = std::function<void()>;
ActivityAct activityInvoke(World& world,Npc& npc,std::vector<size_t> functions) {
  return [&world,&npc,functions] {
    for(auto function:functions)
      world.script().invokeState(&npc,nullptr,nullptr,uint32_t(function));
    };
  }
size_t activityCount(const std::vector<std::string>& events,const std::string& event) {
  return size_t(std::count(events.begin(),events.end(),event));
  }

// One run from a clean queue: `record` keeps the game's own definitions, otherwise the VM runs.
std::vector<std::string> activityRun(World& world,Npc& npc,const ActivityAct& act,bool record,size_t& records) {
  auto& live=NpcDefinitionContract::activities(world.script());
  ActivityDefinitionRuntime::Definitions none;
  struct Swap {
    ActivityDefinitionRuntime::Definitions& a; ActivityDefinitionRuntime::Definitions& b; bool active;
    ~Swap() { if(active) std::swap(a,b); }
    } swap{live,none,!record};
  if(!record) std::swap(live,none);
  npc.clearAiQueue();
  ActivityStream stream(world,npc);
  act();
  records=stream.records;
  return std::move(stream.events);
  }

// Both runs start from `prepare`; every phase of the NPC must run from its record and give the VM's stream.
// The phases of `vmState`, a state without a record (the interruption), run in the VM in both.
std::vector<std::string> activityParity(World& world,Npc& npc,const char* position,const ActivityAct& act,
                                        const std::function<void()>& prepare,const std::vector<std::string>& expectedEvents,
                                        const std::string& vmState="") {
  size_t vmRecords=0,records=0;
  prepare();
  const auto expected=activityRun(world,npc,act,false,vmRecords);
  prepare();
  const auto actual=activityRun(world,npc,act,true,records);
  require(vmRecords==0,"the switched-off run must execute every phase in the VM");
  const auto phases=size_t(std::count_if(expected.begin(),expected.end(),[&](const auto& e) {
    return e.starts_with("phase ") && (vmState.empty() || !e.starts_with("phase "+vmState));
    }));
  if(records!=phases || phases==0)
    throw std::runtime_error(std::string("activity ")+position+": "+std::to_string(records)+" of "+std::to_string(phases)+
                             " phases ran from the record");
  for(size_t i=0;i<std::max(expected.size(),actual.size());++i)
    if(i>=expected.size() || i>=actual.size() || expected[i]!=actual[i])
      throw std::runtime_error(std::string("activity ")+position+": record stream differs at "+std::to_string(i+1)+": VM "+
                               (i<expected.size() ? expected[i] : "<none>")+", record "+(i<actual.size() ? actual[i] : "<none>"));
  for(const auto& event:expectedEvents)
    if(std::find(actual.begin(),actual.end(),event)==actual.end())
      throw std::runtime_error(std::string("activity ")+position+": stream lacks "+event);
  std::printf("Activity %s: %zu phases from the record, %zu events equal to the VM\n",position,records,actual.size());
  return actual;
  }

// Through the engine's state machine: Init and Loop by the routine tick; the interruption reaches
// the NPC as a perception handler sends it, an AI_StartState in the queue taken by the engine, and
// its start runs End. With `routine` (a time inside the activity's routine row) the interruption
// ends by its time and the routine tick returns the NPC to the activity; without it the test
// restarts the activity. The end of the routine row runs the last Loop and End.
std::vector<std::string> activityEngine(World& world,Npc& npc,const char* position,const ActivityPhases& anvil,
                                        size_t interruption,const std::function<void()>& place,const std::string& point,
                                        std::optional<gtime> routine={}) {
  auto& session=*Gothic::inst().gameSession();
  const auto time=world.time();
  auto prepare=[&] {
    if(routine) session.setTime(*routine);
    place();
    npc.clearAiQueue();
    NpcDefinitionContract::aiState(npc)={};
    NpcDefinitionContract::previousState(npc)={};
    require(npc.startState(anvil.init,point),"the NPC must enter the activity");
    };
  auto act=[&] {
    NpcDefinitionContract::tickRoutine(npc);
    NpcDefinitionContract::tickRoutine(npc);
    npc.clearAiQueue();
    npc.aiPush(AiQueue::aiStartState(ScriptFn(interruption),1,nullptr,nullptr,""));
    NpcDefinitionContract::nextAiAction(npc);
    require(npc.isInState(interruption),"the queued interruption must switch the state");
    if(routine) {
      NpcDefinitionContract::aiState(npc).eTime=world.time();
      for(int i=0;i<3 && !npc.isInState(anvil.init);++i)
        NpcDefinitionContract::tickRoutine(npc);
      require(npc.isInState(anvil.init),"the routine tick must return the NPC to the activity");
      } else {
      require(npc.startState(anvil.init,point),"the NPC must return to the activity");
      }
    // The routine tick that returns the NPC runs Init at once; a restart runs it on the next tick.
    if(!NpcDefinitionContract::aiState(npc).started)
      NpcDefinitionContract::tickRoutine(npc);
    NpcDefinitionContract::tickRoutine(npc);
    NpcDefinitionContract::aiState(npc).eTime=world.time();
    NpcDefinitionContract::tickRoutine(npc);
    };
  if(routine) {
    // The interruption's own phases run in the VM in both runs and write script globals (ZS_Talk_Loop
    // its zsTalkBugfix): one run first, so that both compared runs start from the same globals.
    size_t ignored=0;
    prepare();
    activityRun(world,npc,act,true,ignored);
    }
  const auto events=activityParity(world,npc,position,act,prepare,{},
                                   std::string(world.script().symbolName(interruption)));
  if(routine) session.setTime(time);
  const auto name=std::string(world.script().symbolName(anvil.init));
  require(activityCount(events,"phase "+name)==2 && activityCount(events,"phase "+name+"_LOOP")==3 &&
          activityCount(events,"phase "+name+"_END")==2,
          "the engine must run Init twice, Loop three times and End at the interruption and at the end of the row");
  require(!npc.isInState(anvil.init),"the end of the routine row must leave the activity");
  return events;
  }

// Commands the queue received for walking to `point`.
size_t activityWalks(const std::vector<std::string>& events,const std::string& point) {
  return size_t(std::count_if(events.begin(),events.end(),[&](const auto& e) {
    return e.starts_with("queue GoToPoint ") && e.find(" "+point+" ")!=std::string::npos;
    }));
  }

// A distance approach on both sides of its threshold: where the engine reports exactly the threshold
// the script neither asks to walk nor queues a walk, one unit more does both, once.
size_t activityThreshold(World& world,Npc& npc,const std::string& label,size_t init,int32_t threshold,
                         const std::string& point,const std::function<void()>& place) {
  const auto* wp=world.findWayPoint(point);
  require(wp!=nullptr,("the approach point must exist: "+point).c_str());
  std::optional<float> at,past;
  for(float dx=0;dx<4*float(threshold) && !(at && past);dx+=0.25f) {
    npc.setPosition(wp->position()+Tempest::Vec3(dx,0,0));
    const auto distance=NpcEngineCalls::distanceToWaypoint(world,npc,point);
    if(!at && distance==threshold) at=dx;
    if(!past && distance==threshold+1) past=dx;
    }
  require(at && past,"the threshold distances must be reachable along the point's x axis");
  size_t events=0;
  for(const auto& [dx,distance]:{std::pair{*at,threshold},std::pair{*past,threshold+1}}) {
    const auto position=label+(distance==threshold ? " at the approach threshold" : " just past the approach threshold");
    const auto stream=activityParity(world,npc,position.c_str(),activityInvoke(world,npc,{init}),[&,dx=dx] {
      place();
      npc.setPosition(wp->position()+Tempest::Vec3(dx,0,0));
      },{"NPC_GETDISTTOWP(self,\""+point+"\")->"+std::to_string(distance)});
    const size_t walks=distance==threshold ? 0 : 1;
    require(activityCount(stream,"AI_GOTOWP(self,\""+point+"\")")==walks && activityWalks(stream,point)==walks,
            "past the threshold, and only there, the NPC must ask once to walk and queue the walk");
    events+=stream.size();
    }
  return events;
  }

void activitySwitch(GameScript& script,const std::vector<std::string>& accepted,const char* other,size_t pointPhases=0);

// The phases the engine starts and the uses it queues, per NPC, seen from the engine's side.
class ActivityCalls final : public NpcActivityProbe {
  public:
    const Npc* lastScriptRoll=nullptr;
    ActivityCalls() { previous=instance; instance=this; }
    ~ActivityCalls() override { instance=previous; }
    size_t phase(const Npc& npc,const std::string& symbol) const {
      const auto it=phases.find({&npc,symbol});
      return it==phases.end() ? 0 : it->second;
      }
    size_t uses(const Npc& npc) const {
      const auto it=useCount.find(&npc);
      return it==useCount.end() ? 0 : it->second;
      }

  private:
    void phaseBegin(const Npc* npc,std::string_view symbol) override { ++phases[{npc,std::string(symbol)}]; }
    void phaseEnd(int) override {}
    void random(const Npc* npc,uint32_t,uint32_t,bool script) override { lastScriptRoll=script ? npc : nullptr; }
    void queued(const Npc* npc,const AiQueue::AiAction& a) override { if(a.act==AI_UseMob) ++useCount[npc]; }
    void point(const Npc&,const WayPoint*,const WayPoint*) override {}
    void mob(const Npc&,const Interactive&,bool) override {}
    NpcActivityProbe*                                     previous=nullptr;
    std::map<std::pair<const Npc*,std::string>,size_t>     phases;
    std::map<const Npc*,size_t>                           useCount;
  };

// Runs the queued actions of Init (weapon away, walk mode) as the engine takes them; nothing to walk to.
void activityDrain(Npc& npc) {
  for(int i=0;i<16 && NpcDefinitionContract::queue(npc).size()>0;++i)
    NpcDefinitionContract::nextAiAction(npc);
  require(NpcDefinitionContract::queue(npc).size()==0,"Init's queued actions must finish at the point");
  }

// E1 without game data: no object exists. The routine tick runs Init, then the automaton instead of
// Loop: it looks for the object once the queue is empty and waits without waking; its watchdog restarts
// it once. With OPENGOTHIC_ACTIVITY_EVENTS=0 the same state polls its Loop again.
void activityEventsSynthetic(World& world,Npc& npc,const ActivityPhases& anvil,const ActivityPhases& roast) {
  using ActivityEvents::Stage;
  auto& script=world.script();
  auto& session=*Gothic::inst().gameSession();
  const auto configured=std::getenv("OPENGOTHIC_ACTIVITY_EVENTS");
  const std::optional<std::string> previous=configured ? std::optional<std::string>(configured) : std::nullopt;
  for(const auto& [value,on]:{std::pair<const char*,bool>{nullptr,true},{"1",true},{"0",false}}) {
    if(value) setenv("OPENGOTHIC_ACTIVITY_EVENTS",value,1); else unsetenv("OPENGOTHIC_ACTIVITY_EVENTS");
    require(ActivityDefinitionRuntime::events()==on,"OPENGOTHIC_ACTIVITY_EVENTS: only 0 turns the events off");
    }
  if(previous) setenv("OPENGOTHIC_ACTIVITY_EVENTS",previous->c_str(),1); else unsetenv("OPENGOTHIC_ACTIVITY_EVENTS");
  auto& on=NpcDefinitionContract::activityEventsOn(script);
  on=true;
  require(script.eventActivity(ScriptFn(anvil.init))!=nullptr && script.eventActivity(ScriptFn(anvil.loop))==nullptr &&
          script.eventActivity(ScriptFn(roast.init))==nullptr,"only the Init of an accepted record starts an activity by events");
  {
    // OPENGOTHIC_ACTIVITY_RECORDS=all runs every captured record, events stay with the accepted states only.
    const auto* records=std::getenv("OPENGOTHIC_ACTIVITY_RECORDS");
    const std::optional<std::string> kept=records ? std::optional<std::string>(records) : std::nullopt;
    setenv("OPENGOTHIC_ACTIVITY_RECORDS","all",1);
    const auto& sha=selectBaseProgram(baseProgramIndex(),Gothic::inst().defaultGameDatFile(),
        Settings::getI("GAME","language")).source.recordSha256;
    auto all=ActivityDefinitionRuntime::load(baseProgramIndex().dir.parent_path(),script.getVm(),sha);
    if(kept) setenv("OPENGOTHIC_ACTIVITY_RECORDS",kept->c_str(),1); else unsetenv("OPENGOTHIC_ACTIVITY_RECORDS");
    auto& live=NpcDefinitionContract::activities(script);
    std::swap(live,all);
    const bool roastRecord=ActivityDefinitionRuntime::find(live,uint32_t(roast.init))!=nullptr;
    const bool roastEvents=script.eventActivity(ScriptFn(roast.init))!=nullptr;
    const bool anvilEvents=script.eventActivity(ScriptFn(anvil.init))!=nullptr;
    std::swap(live,all);
    require(roastRecord && !roastEvents && anvilEvents,
            "OPENGOTHIC_ACTIVITY_RECORDS=all must keep the polled Loop for a captured state outside the accepted list");
  }
  ActivityCalls calls;
  const auto time=world.time();
  ActivityEvents::reset();
  npc.clearAiQueue();
  NpcDefinitionContract::aiState(npc)={};
  npc.handle().wp="TEST_WP";
  auto end=time;
  end.addMilis(6*60*60*1000);
  require(npc.startState(ScriptFn(anvil.init),"TEST_WP",end,false),"the NPC must enter the activity");
  const std::string state=NpcDefinitionContract::aiState(npc).hint;
  NpcDefinitionContract::tickRoutine(npc);
  require(npc.activityStage()==Stage::Approach,"after Init the activity waits for its queue");
  activityDrain(npc);
  for(int i=0;i<10;++i)
    NpcDefinitionContract::tickRoutine(npc);
  const auto& stats=ActivityEvents::stats().states.at(state);
  require(npc.activityStage()==Stage::Wait && stats.steps==1 && stats.wakes[2]==1,
          "with no object the activity looks once, after its queue, then waits without waking");
  require(calls.phase(npc,state)==1 && calls.phase(npc,state+"_LOOP")==0 && calls.phase(npc,state+"_END")==0,
          "the activity by events runs Init and no Loop");
  auto late=time;
  late.addMilis(ActivityEvents::WatchdogMs+60*1000);
  session.setTime(late);
  NpcDefinitionContract::tickRoutine(npc);
  NpcDefinitionContract::tickRoutine(npc);
  session.setTime(time);
  require(ActivityEvents::stats().watchdog==1 && stats.steps==2 && npc.activityStage()==Stage::Wait,
          "the watchdog restarts a wait once, after WatchdogMs of world time");
  on=false;
  npc.clearState(true);
  NpcDefinitionContract::aiState(npc)={};
  require(npc.startState(ScriptFn(anvil.init),"TEST_WP",end,false),"the NPC must enter the activity");
  NpcDefinitionContract::tickRoutine(npc);
  npc.clearAiQueue();
  NpcDefinitionContract::tickRoutine(npc);
  require(npc.activityStage()==Stage::Off && calls.phase(npc,state+"_LOOP")==1,"OPENGOTHIC_ACTIVITY_EVENTS=0 polls the Loop again");
  npc.clearState(true);
  npc.clearAiQueue();
  NpcDefinitionContract::aiState(npc)={};
  std::printf("Activity events without game data: Init, one look after the queue, a quiet wait, one watchdog; 0 polls Loop\n");
  }

void activityDefinitionsSynthetic(World& world) {
  auto& script=world.script();
  // The record phases against the VM: the polled Loop of OPENGOTHIC_ACTIVITY_EVENTS=0.
  NpcDefinitionContract::activityEventsOn(script)=false;
  activitySwitch(script,{"ZS_SMITH_ANVIL","ZS_SMOKE_WATERPIPE"},"ZS_TEST_ROAST");
  // The game's accepted list itself, without game data.
  for(const char* state:{"ZS_SMITH_ANVIL","ZS_SMITH_COOL","ZS_SMITH_FIRE","ZS_SMITH_SHARP","ZS_READ_BOOKSTAND",
                         "ZS_STOMP_HERB","ZS_SMOKE_WATERPIPE"})
    require(ActivityDefinitionRuntime::accepted(state),(std::string("the game must accept ")+state).c_str());
  require(!ActivityDefinitionRuntime::accepted("ZS_TEST_ROAST"),"ZS_TEST_ROAST is a captured but unaccepted test record");
  auto* npc=world.addNpc(script.findSymbolIndex("NPC_NO_ID"),Tempest::Vec3{});
  require(npc!=nullptr,"activity NPC must exist");
  const auto anvil=activityPhases(script,"ZS_SMITH_ANVIL"),roast=activityPhases(script,"ZS_TEST_ROAST");
  const auto& phases=NpcDefinitionContract::activities(script).phases;
  require(phases.contains(anvil.init) && phases.contains(anvil.loop) && phases.contains(anvil.end),
          "the game must load the accepted activity record");
  require(!phases.contains(roast.init) && !phases.contains(roast.loop) && !phases.contains(roast.end),
          "a captured activity outside the accepted list must stay in the VM");
  auto at=[&](const char* wp) { return [npc,wp] { npc->handle().wp=wp; }; };
  size_t events=0;
  events+=activityParity(world,*npc,"at its point",activityInvoke(world,*npc,{anvil.init,anvil.loop,anvil.end}),at("TEST_WP"),
                         {"NPC_GETNEARESTWP(self)->\"TEST_WP\"","WLD_ISMOBAVAILABLE(self,\"BSANVIL\")->0",
                          "AI_USEMOB(self,\"BSANVIL\",-1)->0","queue UseMob -1 0 0 BSANVIL    0","result 0"}).size();
  events+=activityParity(world,*npc,"away from its point",activityInvoke(world,*npc,{anvil.init,anvil.loop}),at("CONTRACT_FAR_WP"),
                         {"AI_GOTOWP(self,\"CONTRACT_FAR_WP\")"}).size();
  events+=activityEngine(world,*npc,"interrupted, resumed, row over",anvil,script.findSymbolIndex("ZS_TEST"),
                         at("TEST_WP"),"TEST_WP").size();
  const auto pipe=activityPhases(script,"ZS_SMOKE_WATERPIPE");
  events+=activityThreshold(world,*npc,"ZS_SMOKE_WATERPIPE",pipe.init,500,"TEST_WP",at("TEST_WP"));
  // No object exists without game data: the game's host must still queue the use with its state.
  npc->clearAiQueue();
  NpcActivityHost(world,*npc,[](std::string_view) {}).useMob("BSANVIL",1);
  const auto* use=npc->aiQueueFront();
  require(use!=nullptr && use->act==AI_UseMob && use->s0=="BSANVIL" && use->i0==1,
          "the game's host must queue AI_UseMob with the state of the use");
  npc->clearAiQueue();
  const auto directory=std::filesystem::temp_directory_path()/("gothic-activity-day-"+
      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(directory);
  std::string error;
  const NpcDayJournal::Options options{.directory=directory.string(),.isolate=false,.scriptProbe=true};
  if(!NpcDayMeasurement::start(world,options,error)) throw std::runtime_error(error);
  for(auto function:{anvil.init,anvil.loop,anvil.end,roast.init,roast.loop,roast.end})
    script.invokeState(npc,nullptr,nullptr,uint32_t(function));
  NpcDayMeasurement::stop(world);
  std::ifstream file(directory/"npc-day-script.json");
  const std::string bytes{std::istreambuf_iterator<char>(file),{}};
  std::filesystem::remove_all(directory);
  rapidjson::Document document;
  document.Parse(bytes.c_str());
  require(!document.HasParseError(),"activity day counters must be valid JSON");
  const auto& states=document["stats"]["states"];
  auto phase=[&](const char* state,const char* name,const char* field) {
    return states[state]["phases"][name][field].GetUint64();
    };
  for(const char* name:{"init","loop","end"}) {
    require(phase("ZS_SMITH_ANVIL",name,"engine")==1 && phase("ZS_SMITH_ANVIL",name,"records")==1,
            "the accepted phase must be counted once, from its record");
    require(phase("ZS_TEST_ROAST",name,"engine")==1 && phase("ZS_TEST_ROAST",name,"records")==0,
            "the outside phase must be counted once, from the VM");
    }
  require(phase("ZS_SMITH_ANVIL","init","nested")==1 && phase("ZS_SMITH_ANVIL","loop","nested")==0 &&
          phase("ZS_SMITH_ANVIL","end","nested")==0,"a record phase runs only its deferred perception helper in the VM");
  require(phase("ZS_TEST_ROAST","init","nested")==3 && phase("ZS_TEST_ROAST","loop","nested")==1,
          "the VM phase runs its helpers");
  require(phase("ZS_SMITH_ANVIL","init","instructions")==9 && phase("ZS_SMITH_ANVIL","loop","instructions")==0 &&
          phase("ZS_SMITH_ANVIL","end","instructions")==0 && phase("ZS_TEST_ROAST","init","instructions")>9,
          "a record phase counts only the instructions of its deferred helper");
  activityEventsSynthetic(world,*npc,anvil,roast);
  world.removeNpc(*npc);
  std::printf("Activity counters: ZS_SMITH_ANVIL 3 phases from the record, ZS_TEST_ROAST 3 from the VM; %zu equal events\n",events);
  }

// The accepted activities of NEWWORLD, with the object scheme and the approach threshold their
// scripts pass (0: the nearest-waypoint approach). Written from the scripts, not from the record,
// so a record with another state's parameters fails the stream comparison with the VM.
struct WorldActivitySpec {
  const char* state;
  const char* scheme;
  int32_t     approachDistance;
  size_t      objects; // free objects of the scheme the actor reaches from its point in NEWWORLD
  const char* actor;   // a fixed actor, or null for the first NPC with a row of the state
  };
constexpr WorldActivitySpec worldActivities[]={
  {"ZS_SMITH_ANVIL","BSANVIL",0,1,"VLK_412_HARAD"},
  {"ZS_SMITH_COOL","BSCOOL",0,2,nullptr},
  {"ZS_SMITH_FIRE","BSFIRE",0,1,nullptr},
  {"ZS_SMITH_SHARP","BSSHARP",0,1,nullptr},
  {"ZS_READ_BOOKSTAND","BOOK",0,1,nullptr},
  {"ZS_STOMP_HERB","HERB",0,1,nullptr},
  {"ZS_SMOKE_WATERPIPE","SMOKE",500,3,nullptr},
  };

struct WorldActivity {
  const WorldActivitySpec* spec=nullptr;
  ActivityPhases           phases;
  Npc*                     actor=nullptr;
  std::string              actorName;
  const WayPoint*          point=nullptr;
  std::string              pointName;
  gtime                    rowStart;
  Interactive*             mob=nullptr;
  std::vector<Npc*>        occupants; // occupy every free object of the scheme near the point
  };

std::string npcName(World& world,const Npc& npc) { return std::string(world.script().symbolName(npc.instanceSymbol())); }

}

#include "npc_activity_world_fixture.h"
#include "npc_placement_world_fixture.h"

namespace {

// Every position of one activity at its real object: at the object, far from the point, the object
// taken by other NPCs, already at the object, both sides of a distance approach, and the engine's
// interruption, return and end of the row. Then the saved state after record phases.
size_t activityAtObject(World& world,WorldActivity& activity,const std::set<Npc*>& reserved) {
  auto& script=world.script();
  auto& actor=*activity.actor;
  const auto& phases=activity.phases;
  const auto* point=activity.point;
  const auto& pointName=activity.pointName;
  const std::string scheme="\""+std::string(activity.spec->scheme)+"\"";
  auto atPoint=[&] {
    for(auto* other:activity.occupants)
      other->setInteraction(nullptr,true);
    actor.setInteraction(nullptr,true);
    actor.setPosition(point->position());
    actor.handle().wp=pointName;
    };
  atPoint();
  activity.mob=world.availableMob(actor,activity.spec->scheme);
  require(activity.mob!=nullptr,(activity.actorName+"'s point must have a free "+activity.spec->scheme).c_str());
  // Other NPCs, preferably with the same activity, take every free object of the scheme in reach.
  std::vector<Npc*> candidates;
  for(int pass=0;pass<2;++pass)
    for(uint32_t i=0;i<world.npcCount();++i) {
      auto* npc=world.npcById(i);
      if(reserved.contains(npc) || !activityCandidate(*npc)) continue;
      std::string ignored;
      const bool same=activityRow(*npc,phases.init,ignored)!=nullptr;
      if(same==(pass==0) && std::find(candidates.begin(),candidates.end(),npc)==candidates.end())
        candidates.push_back(npc);
      }
  auto occupy=[&] {
    atPoint();
    size_t used=0;
    while(auto* free=world.availableMob(actor,activity.spec->scheme)) {
      require(used<candidates.size(),("more free "+std::string(activity.spec->scheme)+" objects than NPCs to take them").c_str());
      auto* other=candidates[used++];
      other->setInteraction(nullptr,true);
      require(other->setInteraction(free,true),"another NPC must occupy the object");
      if(std::find(activity.occupants.begin(),activity.occupants.end(),other)==activity.occupants.end())
        activity.occupants.push_back(other);
      }
    // Every object of the scheme in reach, not only the nearest: a search that stops at the first
    // taken object would leave the others free and still answer "none".
    require(used==activity.spec->objects,(activity.actorName+" must reach "+std::to_string(activity.spec->objects)+" "+
            activity.spec->scheme+" objects, took "+std::to_string(used)).c_str());
    };
  const auto label=[&](const char* position) { return activity.spec->state+std::string(" ")+position; };
  std::vector<std::string> labels;
  size_t events=0;
  auto parity=[&](const char* position,const ActivityAct& act,const std::function<void()>& prepare,
                  const std::vector<std::string>& expected) {
    labels.push_back(label(position));
    const auto stream=activityParity(world,actor,labels.back().c_str(),act,prepare,expected);
    events+=stream.size();
    return stream;
    };
  auto gotoCount=[&](const std::vector<std::string>& stream) {
    return size_t(std::count_if(stream.begin(),stream.end(),[](const auto& e) { return e.starts_with("AI_GOTOWP("); }));
    };
  parity("at the object",activityInvoke(world,actor,{phases.init,phases.loop,phases.end}),atPoint,
         {"WLD_ISMOBAVAILABLE(self,"+scheme+")->1","AI_USEMOB(self,"+scheme+",1)->0","AI_USEMOB(self,"+scheme+",-1)->0"});
  const auto far=parity("far from the point",activityInvoke(world,actor,{phases.init,phases.loop}),[&] {
    atPoint();
    actor.setPosition(point->position()+Tempest::Vec3(4000,0,0));
    },{"AI_GOTOWP(self,\""+pointName+"\")","WLD_ISMOBAVAILABLE(self,"+scheme+")->0"});
  require(gotoCount(far)==1 && activityWalks(far,pointName)==1,"far from the point the NPC must queue one walk there");
  parity("object taken by another NPC",activityInvoke(world,actor,{phases.loop}),occupy,
         {"WLD_ISMOBAVAILABLE(self,"+scheme+")->0"});
  parity("already at the object",activityInvoke(world,actor,{phases.loop,phases.end}),[&] {
    atPoint();
    require(actor.setInteraction(activity.mob,true),"the actor must use the object");
    },{"WLD_ISMOBAVAILABLE(self,"+scheme+")->1"});
  if(const auto threshold=activity.spec->approachDistance; threshold>0)
    events+=activityThreshold(world,actor,activity.spec->state,phases.init,threshold,pointName,atPoint);
  const auto talk=script.findSymbolIndex("ZS_TALK");
  labels.push_back(label("interrupted, resumed, row over"));
  gtime routine(world.time().day(),int32_t(activity.rowStart.hour()),int32_t(activity.rowStart.minute()));
  routine.addMilis(60*1000);
  events+=activityEngine(world,actor,labels.back().c_str(),phases,talk,atPoint,pointName,routine).size();
  // The saved state after the record phases: the actor's state is equal; the VM differs only in the
  // locals of the script helpers the record does not call. Each is assigned before it is read.
  // Another NPC's VM phases run first, as in the game some other NPC called the helpers last.
  auto* other=candidates.front();
  size_t ignored=0;
  atPoint();
  activityRun(world,*other,activityInvoke(world,*other,{phases.init,phases.loop}),false,ignored);
  activityRun(world,actor,activityInvoke(world,actor,{phases.init,phases.loop}),true,ignored);
  const auto recordNpc=npcState(actor);
  const auto recordLocals=placementSymbols(script,true);
  atPoint();
  activityRun(world,*other,activityInvoke(world,*other,{phases.init,phases.loop}),false,ignored);
  activityRun(world,actor,activityInvoke(world,actor,{phases.init,phases.loop}),false,ignored);
  other->clearAiQueue();
  require(npcState(actor)==recordNpc,(activity.actorName+"'s saved state after the record phases must equal the VM's").c_str());
  const std::set<std::string> helperLocals={"B_RESETALL.SLF","B_STOPLOOKAT.SLF","B_STOPLOOKAT.TARGET",
                                            "C_BODYSTATECONTAINS.SLF","C_BODYSTATECONTAINS.BODYSTATE"};
  std::set<std::string> differing;
  for(const auto& [name,value]:placementSymbols(script,true))
    if(!recordLocals.contains(name) || recordLocals.at(name)!=value) differing.insert(name.substr(0,name.find('[')));
  for(const auto& name:differing)
    require(helperLocals.contains(name),("a saved VM symbol differs outside the skipped helpers: "+name).c_str());
  std::printf("Activity %s at %s (%s, %zu occupants from %s): %zu equal events, VM locals differ in %zu skipped-helper symbols\n",
              activity.spec->state,pointName.c_str(),activity.actorName.c_str(),activity.occupants.size(),
              npcName(world,*activity.occupants.front()).c_str(),events,differing.size());
  return events;
  }

// Every accepted activity at its real object in NEWWORLD, each with its own actor; then one save
// with every actor in the middle of its Loop, and after the load each continues with its restored Loop.
void activityDefinitionsWorld(Gothic& gothic) {
  auto& world=*gothic.world();
  auto& script=world.script();
  std::vector<std::string> accepted;
  for(const auto& spec:worldActivities)
    accepted.emplace_back(spec.state);
  accepted.emplace_back("ZS_SIT_THRONE");
  accepted.emplace_back("ZS_ROAST_SCAVENGER");
  for(const auto* state:{"ZS_COOK_CAULDRON","ZS_COOK_PAN","ZS_COOK_STOVE","ZS_PICK_ORE","ZS_POTION_ALCHEMY"})
    accepted.emplace_back(state);
  constexpr size_t eventPhases=10*2; // Init and End of five point activities and five timed MOB activities.
  activitySwitch(script,accepted,nullptr,eventPhases);
  std::vector<WorldActivity> activities;
  std::set<Npc*> actors;
  for(const auto& spec:worldActivities) {
    WorldActivity activity;
    activity.spec=&spec;
    activity.phases=activityPhases(script,spec.state);
    if(spec.actor!=nullptr) {
      activity.actor=activityNpc(world,spec.actor);
      require(activity.actor!=nullptr,(std::string("NEWWORLD must contain ")+spec.actor).c_str());
      activity.point=activityRow(*activity.actor,activity.phases.init,activity.pointName,&activity.rowStart);
      } else {
      for(uint32_t i=0;i<world.npcCount() && activity.point==nullptr;++i) {
        auto* npc=world.npcById(i);
        if(actors.contains(npc) || !activityCandidate(*npc)) continue;
        if((activity.point=activityRow(*npc,activity.phases.init,activity.pointName,&activity.rowStart))!=nullptr)
          activity.actor=npc;
        }
      }
    require(activity.actor!=nullptr && activity.point!=nullptr,(std::string("NEWWORLD must have an NPC with a ")+spec.state+" row").c_str());
    activity.actorName=npcName(world,*activity.actor);
    actors.insert(activity.actor);
    activities.push_back(std::move(activity));
    }
  require(NpcDefinitionContract::activities(script).phases.size()==3*accepted.size()+eventPhases,
          "the game must load the accepted mob phases and the point activities' Init and End");
  size_t events=0;
  for(auto& activity:activities)
    events+=activityAtObject(world,activity,actors);
  // A save in the middle of every activity, after the engine's Init and Loop.
  for(auto& activity:activities) {
    auto& actor=*activity.actor;
    for(auto* other:activity.occupants) { other->setInteraction(nullptr,true); other->clearAiQueue(); }
    actor.setInteraction(nullptr,true);
    actor.setPosition(activity.point->position());
    actor.handle().wp=activity.pointName;
    actor.clearAiQueue();
    NpcDefinitionContract::aiState(actor)={};
    NpcDefinitionContract::previousState(actor)={};
    }
  for(auto& activity:activities) {
    auto& actor=*activity.actor;
    require(actor.startState(activity.phases.init,activity.pointName),(activity.actorName+" must enter "+activity.spec->state).c_str());
    NpcDefinitionContract::tickRoutine(actor);
    NpcDefinitionContract::tickRoutine(actor);
    require(NpcDefinitionContract::aiState(actor).started,"the engine must have started the activity before the save");
    }
  std::vector<std::vector<std::string>> savedQueues;
  for(const auto& activity:activities) {
    savedQueues.push_back(activityQueue(*activity.actor));
    require(std::any_of(savedQueues.back().begin(),savedQueues.back().end(),[&](const auto& a) {
              return a.starts_with("UseMob 1 0 "+std::string(activity.spec->scheme)+" "); }),
            (activity.actorName+"'s Loop must have queued the use of its object before the save").c_str());
    }
  std::vector<uint8_t> bytes;
  {
    Tempest::MemWriter writer(bytes);
    Serialize out(writer);
    Tempest::Pixmap preview(1,1,Tempest::TextureFormat::RGBA8);
    std::memset(preview.data(),0,4);
    gothic.gameSession()->save(out,"activity mid-loop",preview);
  }
  gothic.clearGame().reset();
  {
    Tempest::MemReader reader(bytes);
    Serialize in(reader);
    gothic.setGame(std::make_unique<GameSession>(in));
  }
  // `world` and every actor belong to the session replaced by the load.
  auto& loaded=*gothic.world();
  for(size_t i=0;i<activities.size();++i) {
    const auto& activity=activities[i];
    const auto& phases=activity.phases;
    const std::string state=activity.spec->state;
    auto* restored=activityNpc(loaded,activity.actorName.c_str());
    require(restored!=nullptr && restored->isInState(phases.init),("the loaded "+activity.actorName+" must be in "+state).c_str());
    require(activityQueue(*restored)==savedQueues[i],("the loaded "+activity.actorName+" must keep the queue of the save").c_str());
    require(NpcDefinitionContract::activities(loaded.script()).phases.contains(phases.loop),"the loaded game must load the record");
    const auto savedState=NpcDefinitionContract::aiState(*restored);
    const auto savedPrevious=NpcDefinitionContract::previousState(*restored);
    const auto position=state+" after loading mid-activity";
    const auto afterLoad=activityParity(loaded,*restored,position.c_str(),[&] {
      NpcDefinitionContract::tickRoutine(*restored);
      NpcDefinitionContract::aiState(*restored).eTime=loaded.time();
      NpcDefinitionContract::tickRoutine(*restored);
      },[&] {
      restored->clearAiQueue();
      NpcDefinitionContract::aiState(*restored)=savedState;
      NpcDefinitionContract::previousState(*restored)=savedPrevious;
      },{"WLD_ISMOBAVAILABLE(self,\""+std::string(activity.spec->scheme)+"\")->1"});
    require(activityCount(afterLoad,"phase "+state)==0 && activityCount(afterLoad,"phase "+state+"_LOOP")==2 &&
            activityCount(afterLoad,"phase "+state+"_END")==1,
            ("the loaded "+activity.actorName+" must continue with the restored Loop, then end the row").c_str());
    events+=afterLoad.size();
    }
  std::printf("Activities at NEWWORLD objects: %zu states, %zu equal events\n",activities.size(),events);
  }

// E1 (world/activityevents.h): the activities at objects by events, through the engine's own world
// tick, which drives the bodies, queues, objects and the clock. Every other NPC is under external
// control, so only the actors of a position run their routines and perceptions. Exits that need the
// short reservation window (death, reset, a reset queue) stop between the look and the queued use.
void activityEventsWorld(Gothic& gothic) {
  using ActivityEvents::Stage;
  // Events on in this session and in the one its load creates, whatever the environment says.
  const auto* configured=std::getenv("OPENGOTHIC_ACTIVITY_EVENTS");
  const std::optional<std::string> previous=configured ? std::optional<std::string>(configured) : std::nullopt;
  struct Restore {
    const std::optional<std::string>& previous;
    ~Restore() { if(previous) setenv("OPENGOTHIC_ACTIVITY_EVENTS",previous->c_str(),1); else unsetenv("OPENGOTHIC_ACTIVITY_EVENTS"); }
    } restore{previous};
  unsetenv("OPENGOTHIC_ACTIVITY_EVENTS");
  ActivityEventsWorld test(gothic);
  NpcDefinitionContract::activityEventsOn(test.world().script())=true;
  ActivityEvents::reset();
  std::optional<ActivityCalls> calls;
  calls.emplace();
  auto& stats=ActivityEvents::stats();
  std::vector<EventActor> actors;
  for(const auto& spec:worldActivities)
    actors.push_back(test.actor(spec.state,spec.scheme,spec.actor!=nullptr ? activityNpc(test.world(),spec.actor) : nullptr));
  auto& anvil=actors[0];
  auto& cool=actors[1];
  auto& pipe=actors[6];
  require(anvil.state=="ZS_SMITH_ANVIL" && cool.state=="ZS_SMITH_COOL" && pipe.state=="ZS_SMOKE_WATERPIPE",
          "the accepted activities keep their order");
  auto& harad=*anvil.npc;
  auto* other=test.extra();
  ActivityEventsWorld::detach(harad);
  harad.setPosition(anvil.point->position());
  auto* mob=test.world().availableMob(harad,anvil.scheme);
  require(mob!=nullptr,"Harad's point must reach a free anvil");
  const auto at=anvil.point->position();
  const std::string state=anvil.state;
  const auto ownerless=stats.ownerless;

  // Every accepted activity at its object: attached within a bound, then holding runs no activity code.
  std::string attaches;
  for(auto& a:actors) {
    auto& npc=*a.npc;
    ActivityEventsWorld::detach(npc);
    npc.setPosition(a.point->position());
    auto* object=test.world().availableMob(npc,a.scheme);
    require(object!=nullptr,(a.state+": the actor's point must reach a free "+a.scheme).c_str());
    test.start(a,a.point->position());
    test.only({&npc});
    const auto ticks=test.stepUntil(600,[&] { return test.holds(a,npc,object); },a.state+" at the object: attach");
    const auto held=test.stats(a.state);
    test.step(100);
    require(ActivityEventsWorld::quiet(held,test.stats(a.state)) && test.holds(a,npc,object),
            (a.state+": holding the object must not wake the activity:"+ActivityEventsWorld::woke(held,test.stats(a.state))).c_str());
    require(calls->phase(npc,a.state+"_LOOP")==0,(a.state+": the activity by events must run no Loop").c_str());
    require(test.places().reserved(npc)==nullptr,(a.state+": an attach turns the reservation into the occupied place").c_str());
    attaches+=" "+a.state+" "+std::to_string(ticks);
    test.stop(npc);
    }

  test.only({&harad});
  // Far from the point: the walk first, the reservation only after the arrival.
  const auto* away=test.awayFrom(*anvil.point);
  // A reset queue on the way (Npc_ClearAIQueue) is no arrival: the walk starts over.
  test.start(anvil,away->position());
  float walked=0,arrival=-1;
  size_t walking=0;
  test.stepUntil(1500,[&] { return test.holds(anvil,harad,mob); },"far from the point: walk, reserve, attach",[&] {
    const float d=std::sqrt(harad.qDistTo(anvil.point));
    if(ActivityEventsWorld::stage(harad)==Stage::Approach) {
      if(++walking==30)
        harad.clearAiQueue();
      walked=std::max(walked,d);
      require(test.places().reserved(harad)==nullptr && harad.interactive()==nullptr,
              "no reservation and no object during the walk to the point");
      }
    else if(ActivityEventsWorld::stage(harad)!=Stage::Off && arrival<0) arrival=d;
    });
  require(walking>30,"after a reset queue the NPC must walk on to its point");
  if(!(walked>300.f && arrival>=0 && arrival<200.f))
    throw std::runtime_error("activity events: the reservation must come after the arrival at the point: walked "+
                             std::to_string(walked)+" cm, at the look "+std::to_string(arrival)+" cm from "+anvil.pointName+
                             " (start "+away->name+")");

  // Interrupted on the approach through the queue: End runs, the previous state is the activity and the commands
  // left behind are the same as with OPENGOTHIC_ACTIVITY_EVENTS=0. Compare engine calls, queued commands and phases,
  // comparing the perception preset by the state it installs since its native calls bypass the VM observer.
  // The same one routine tick after a reset queue on the approach
  // (Npc_ClearAIQueue), which starts the walk over: the polled Loop looks for the object instead, so the stream
  // does not compare there.
  auto approachInterrupted=[&](bool events,bool reset) {
    NpcDefinitionContract::activityEventsOn(test.world().script())=events;
    ActivityCalls phases;
    test.start(anvil,away->position());
    NpcDefinitionContract::previousState(harad)={};
    std::optional<ActivityStream> stream;
    if(!reset)
      stream.emplace(test.world(),harad);
    NpcDefinitionContract::tickRoutine(harad);
    require(!events || ActivityEventsWorld::stage(harad)==Stage::Approach,"after Init far from the point the activity must approach");
    if(reset) {
      harad.clearAiQueue();
      NpcDefinitionContract::tickRoutine(harad);
      }
    const auto preset=activityApproachPerceptions(test.world(),harad,state,stream && !events ? &stream->events : nullptr);
    require(!harad.wasInState(ScriptFn(anvil.phases.init)),"before the interruption the activity is no previous state");
    NpcDefinitionContract::pushFront(harad,AiQueue::aiStartState(ScriptFn(cool.phases.init),1,nullptr,nullptr,""));
    NpcDefinitionContract::nextAiAction(harad);
    require(harad.isInState(ScriptFn(cool.phases.init)),"the queued interruption must switch the state");
    const auto ends=stream ? activityCount(stream->events,"phase "+state+"_END") : phases.phase(harad,state+"_END");
    const auto result=std::tuple{ends,harad.wasInState(ScriptFn(anvil.phases.init)),
                                 stream ? stream->events : std::vector<std::string>{},activityQueue(harad),preset};
    stream.reset();
    NpcDefinitionContract::activityEventsOn(test.world().script())=true;
    return result;
    };
  for(const bool reset:{false,true}) {
    const auto polled=approachInterrupted(false,reset),byEvents=approachInterrupted(true,reset);
    // the first difference of the stream, else of the commands left behind
    const auto firstDifference=[&](const std::vector<std::string>& a,const std::vector<std::string>& b) {
      const auto at=std::mismatch(a.begin(),a.end(),b.begin(),b.end());
      return at.first==a.end() && at.second==b.end() ? std::string() :
             ": polled '"+(at.first!=a.end() ? *at.first : "")+"', by events '"+(at.second!=b.end() ? *at.second : "")+"'";
      };
    require(std::get<4>(polled)==std::get<4>(byEvents),
            "the polled and event approaches must install the same senses, callback bindings and perception interval");
    auto difference=firstDifference(std::get<2>(polled),std::get<2>(byEvents));
    if(difference.empty())
      difference=firstDifference(std::get<3>(polled),std::get<3>(byEvents));
    require(std::get<0>(byEvents)==1 && std::get<1>(byEvents) && byEvents==polled,
            (std::string(reset ? "an interruption after a reset queue on the approach must run End and leave the commands "
                                 "as the polled Loop does"
                               : "an interruption on the approach must give the polled Loop's stream: run End, leave the state "
                                 "and the commands")+difference).c_str());
    }

  // The watchdog of a walk: the routine tick never runs while the NPC walks, the watchdog still fires and is
  // counted on the tick it drops the walk.
  test.start(anvil,away->position());
  test.stepUntil(60,[&] { return ActivityEventsWorld::stage(harad)==Stage::Approach && !harad.isAiQueueEmpty(); },"walk: approach");
  test.step(20);
  require(ActivityEventsWorld::stage(harad)==Stage::Approach && !harad.isAiQueueEmpty(),"the NPC must still walk to its point");
  const auto walkWatchdog=stats.watchdog;
  const auto walkInits=calls->phase(harad,state);
  auto walkLate=test.world().time();
  walkLate.addMilis(ActivityEvents::WatchdogMs+60*1000);
  gothic.gameSession()->setTime(walkLate);
  test.step();
  require(stats.watchdog==walkWatchdog+1 && calls->phase(harad,state)==walkInits+1 && ActivityEventsWorld::stage(harad)==Stage::Approach &&
          test.places().reserved(harad)==nullptr && !harad.isAiQueueEmpty(),
          "the watchdog of a walk must start the walk to the point over with Init, counted on that tick, with no reservation");
  float rearrival=-1;
  test.stepUntil(1500,[&] { return test.holds(anvil,harad,mob); },"after the walk's watchdog: reach the object",[&] {
    if(ActivityEventsWorld::stage(harad)==Stage::Approach)
      require(test.places().reserved(harad)==nullptr,"no reservation before the arrival after the walk's watchdog");
    else if(rearrival<0) rearrival=std::sqrt(harad.qDistTo(anvil.point));
    });
  require(rearrival>=0 && rearrival<200.f,"after the walk's watchdog the reservation must come at the point");

  // The object taken: a quiet wait, then the freed place wakes the activity, not its watchdog.
  test.stop(harad);
  harad.setPosition(away->position());
  test.occupy(*other,*mob,anvil.scheme,at);
  test.start(anvil,at);
  test.stepUntil(600,[&] { return ActivityEventsWorld::stage(harad)==Stage::Wait; },"object taken: wait");
  const auto waiting=test.stats(state);
  test.step(150);
  require(ActivityEventsWorld::quiet(waiting,test.stats(state)) && test.places().reserved(harad)==nullptr,
          ("waiting for a taken object must not wake:"+test.woke(waiting,test.stats(state))).c_str());
  // Places freed elsewhere do not wake it: an object of another scheme near it, one of its own scheme far away.
  auto* nearOther=test.object([&](Interactive& o) { return !o.checkMobName(anvil.scheme) && harad.qDistTo(o)<800.f*800.f; });
  auto* farSame=test.object([&](Interactive& o) { return o.checkMobName(anvil.scheme) && harad.qDistTo(o)>1500.f*1500.f; });
  auto* stranger=test.extra();
  for(auto* o:{nearOther,farSame}) {
    const auto before=test.stats(state);
    require(stranger->setInteraction(o,true),"another NPC must take the unrelated object");
    ActivityEventsWorld::detach(*stranger);
    test.step(5);
    require(ActivityEventsWorld::quiet(before,test.stats(state)),"a place freed elsewhere must not wake the waiting NPC");
    }
  // Busy at another object, it waits even for a free place; leaving that object wakes it.
  require(harad.setInteraction(nearOther,true),"the waiting NPC must take another object");
  const auto watchdog=stats.watchdog,freed=test.stats(state).wakes[6];
  test.leave(*other,anvil.scheme,*away);
  test.step(5);
  require(ActivityEventsWorld::stage(harad)==Stage::Wait && test.places().isFree(*mob,*other) && harad.interactive()==nearOther,
          "an NPC busy at another object must keep waiting when its place frees");
  ActivityEventsWorld::detach(harad);
  test.stepUntil(600,[&] { return test.holds(anvil,harad,mob); },"object freed: attach");
  require(stats.watchdog==watchdog && test.stats(state).wakes[6]==freed+1,"the freed place, not the watchdog, must wake the waiting NPC");

  // Interrupted through the queue: the place is left; the routine brings the NPC back to it.
  harad.aiPush(AiQueue::aiStartState(ScriptFn(cool.phases.init),1,nullptr,nullptr,""));
  test.stepUntil(600,[&] { return !harad.isInState(ScriptFn(anvil.phases.init)) && harad.interactive()!=mob; },"interrupted: leave");
  test.left(anvil,harad,*mob,*other,ownerless,"interrupted");
  test.rowTime(anvil);
  NpcDefinitionContract::aiState(harad).eTime=test.world().time();
  test.stepUntil(1500,[&] { return harad.isInState(ScriptFn(anvil.phases.init)) && test.holds(anvil,harad,mob); },
                 "the routine returns the NPC to its object");

  // The end of the routine row.
  const auto rowEnds=test.stats(state).wakes[7];
  NpcDefinitionContract::aiState(harad).eTime=test.world().time();
  test.stepUntil(300,[&] { return harad.interactive()!=mob; },"row over: leave");
  require(test.stats(state).wakes[7]==rowEnds+1,"the end of the row must wake the activity once");
  test.left(anvil,harad,*mob,*other,ownerless,"row over");

  // An interruption through the queue, a reset queue, resetPositionToTA and a death, each while the reservation is held.
  test.start(anvil,at);
  test.reserveNow(anvil,harad);
  other->setPosition(at);
  require(test.world().availableMob(*other,anvil.scheme)!=mob,"a reserved object must not be found for another NPC");
  NpcDefinitionContract::pushFront(harad,AiQueue::aiStartState(ScriptFn(cool.phases.init),1,nullptr,nullptr,""));
  require(test.places().reserved(harad)==mob,"the reservation must be held when the interruption is taken");
  NpcDefinitionContract::nextAiAction(harad);
  require(harad.isInState(ScriptFn(cool.phases.init)),"the queued interruption must switch the state");
  test.left(anvil,harad,*mob,*other,ownerless,"interrupted while reserved");
  test.start(anvil,at);
  test.reserveNow(anvil,harad);
  harad.clearAiQueue();
  test.left(anvil,harad,*mob,*other,ownerless,"queue reset");
  test.start(anvil,at);
  test.reserveNow(anvil,harad);
  require(harad.resetPositionToTA(),"Harad must have a routine point");
  test.left(anvil,harad,*mob,*other,ownerless,"resetPositionToTA");
  test.stop(harad);
  harad.setPosition(away->position());
  auto* victim=test.extra();
  test.start(anvil,at,victim);
  test.reserveNow(anvil,*victim);
  victim->changeAttribute(ATR_HITPOINTS,-victim->attribute(ATR_HITPOINTS),false);
  require(victim->isDead(),"the reserving NPC must die");
  test.left(anvil,*victim,*mob,*other,ownerless,"owner died");

  // A death, resetPositionToTA and the removal from the world of an NPC holding the object: the place is free
  // and the NPC waiting for it wakes.
  for(const std::string how:{"died","resetPositionToTA","removed"}) {
    auto* holder=test.extra();
    test.stop(harad);
    harad.setPosition(away->position());
    test.start(anvil,at,holder);
    test.only({holder});
    test.stepUntil(600,[&] { return test.holds(anvil,*holder,mob); },how+" while holding: hold");
    test.start(anvil,at);
    test.only({holder,&harad});
    test.stepUntil(600,[&] { return ActivityEventsWorld::stage(harad)==Stage::Wait; },how+" while holding: wait");
    const auto freedBefore=test.stats(state).wakes[6];
    if(how=="died") {
      holder->changeAttribute(ATR_HITPOINTS,-holder->attribute(ATR_HITPOINTS),false);
      require(holder->isDead(),"the holder must die");
      }
    else if(how=="resetPositionToTA")
      holder->resetPositionToTA();
    else
      test.world().removeNpc(*holder);
    require(holder->interactive()==nullptr && mob->userCount()==0,(how+" while holding: the object must be left").c_str());
    test.stepUntil(5,[&] { return test.stats(state).wakes[6]>freedBefore; },how+" while holding: the waiting NPC wakes");
    test.places().audit(true);
    require(stats.ownerless==ownerless,(how+" while holding: no reservation may lose its owner").c_str());
    }

  // A use still queued, the NPC walking to the object: a reservation never turned into an attach expires after
  // ReservationMs on the world tick; the watchdog then drops only the activity's own use, counted on that tick,
  // and the AI_StartState queued behind it runs.
  const auto* nearMob=test.nearObject(anvil,harad,*mob);
  // The second NPC's own point is a walk from the object too: its reservation shows before it sits down.
  const auto* nearMob2=test.nearObject(anvil,harad,*mob,nearMob);
  auto walkToObject=[&](const char* what) {
    test.start(anvil,nearMob->position());
    harad.handle().wp=nearMob->name;
    test.stepUntil(60,[&] {
      return ActivityEventsWorld::stage(harad)==Stage::Attach && test.places().reserved(harad)==mob &&
             harad.interactive()==nullptr && !harad.isAiQueueEmpty();
      },what);
    };
  walkToObject("walk to the object: reserve");
  // The reservation came on the tick walkToObject saw it, at that tick's clock: it ends ReservationMs later.
  auto reservedUntil=test.world().time();
  reservedUntil.addMilis(ActivityEvents::ReservationMs);
  // The clock 600 ms short of `end`: the next world tick (about 478 ms of game time) is the last one before it, the
  // tick after it the first to reach it.
  const auto shortOf=[&](gtime end) {
    gtime t(int64_t(0),int32_t(0),int32_t(0));
    t.addMilis(uint64_t(end.toInt()-600));
    gothic.gameSession()->setTime(t);
    };
  // A second NPC of the activity waits for the reserved object: the expiry must free the place (not renew the old
  // reservation) and drop the first NPC's use, so the waiting one wakes and takes the place; whoever sits down then
  // holds a reservation of its own first.
  test.start(anvil,nearMob2->position(),other);
  other->handle().wp=nearMob2->name;
  test.only({&harad,other});
  test.stepUntil(30,[&] { return ActivityEventsWorld::stage(*other)==Stage::Wait; },"a second NPC waits for the reserved object");
  require(ActivityEventsWorld::stage(harad)==Stage::Attach && test.places().reserved(harad)==mob && harad.interactive()==nullptr,
          "the first NPC must still walk to its reserved object");
  const auto expired=stats.expired,useWatchdog=stats.watchdog,freedAtExpiry=test.stats(state).wakes[6];
  // The first NPC sits these ticks out: a reservation renewed instead of freed would keep the place from the other.
  test.only({other});
  shortOf(reservedUntil);
  test.step();
  require(test.world().time()<reservedUntil && stats.expired==expired && test.places().reserved(harad)==mob,
          "a reservation must hold on the last tick before its end");
  test.step();
  require(!(test.world().time()<reservedUntil) && stats.expired==expired+1 && test.stats(state).wakes[6]==freedAtExpiry+1 && test.places().reserved(*other)==mob &&
          other->interactive()==nullptr && test.places().reserved(harad)==nullptr && NpcDefinitionContract::queue(harad).size()==0,
          "an expired reservation must free the place and drop its use: the NPC waiting for it reserves it");
  test.only({&harad,other});
  std::array<bool,2> reservedLast={},attachedBare={};
  const std::array<Npc*,2> rivals={&harad,other};
  test.stepUntil(900,[&] { return test.holds(anvil,harad,mob) || test.holds(anvil,*other,mob); },"after the expired reservation: attach",[&] {
    for(size_t k=0;k<rivals.size();++k) {
      if(rivals[k]->interactive()==nullptr)
        reservedLast[k]=test.places().reserved(*rivals[k])==mob;
      else if(!reservedLast[k])
        attachedBare[k]=true;
      }
    });
  require(!attachedBare[0] && !attachedBare[1],"after an expired reservation an NPC must reserve again before it sits down");
  test.stop(*other);
  other->setPosition(away->position());
  test.only({&harad});
  walkToObject("walk to the object, a command behind: reserve");
  harad.aiPush(AiQueue::aiStartState(ScriptFn(cool.phases.init),1,nullptr,nullptr,""));
  auto later=test.world().time();
  later.addMilis(ActivityEvents::WatchdogMs+60*1000);
  gothic.gameSession()->setTime(later);
  test.step();
  require(stats.watchdog==useWatchdog+1 && harad.isInState(ScriptFn(cool.phases.init)),
          "the watchdog must drop only the activity's own use, counted at once, and keep the command behind it");
  // The end of the row while the use is queued: not before the perception clock, with the reservation kept until
  // then; on its first tick at once.
  walkToObject("walk to the object again: reserve");
  const auto ends=calls->phase(harad,state+"_END");
  const auto due=test.world().tickCount()+330;
  NpcDefinitionContract::aiState(harad).eTime=test.world().time();
  NpcDefinitionContract::aiState(harad).loopNextTime=due;
  test.stepUntil(40,[&] { return !harad.isInState(ScriptFn(anvil.phases.init)); },"the end of the row on the perception clock",[&] {
    require(test.world().tickCount()>=due || (harad.isInState(ScriptFn(anvil.phases.init)) && test.places().reserved(harad)==mob),
            "the end of the row must wait for the perception clock, the reservation held");
    });
  require(test.world().tickCount()-due<33 && harad.interactive()==nullptr && test.places().reserved(harad)==nullptr &&
          calls->phase(harad,state+"_END")==ends+1,
          ("the end of the row must end an activity whose use is still queued on the first tick of the perception clock: "+
           std::to_string(test.world().tickCount()-due)+" ms late").c_str());
  // Another command's walk goes on through the end of the row, also to the very point the activity names: the row
  // is over before that walk starts, and on no tick its goal vanishes before it arrives.
  walkToObject("walk to the object, a walk behind: reserve");
  harad.aiPush(AiQueue::aiGoToPoint(*nearMob));
  test.stepUntil(600,[&] { return harad.interactive()==mob; },"a walk behind: sit down");
  NpcDefinitionContract::aiState(harad).eTime=test.world().time();
  NpcDefinitionContract::aiState(harad).loopNextTime=0;
  bool left=false;
  // arrived as the walk itself ends (Npc::implGoTo): within its threshold after a bounce, or a shared point's
  const auto arrived=[&] {
    const float d=nearMob->useCounter()>1 ? float(MAX_AI_USE_DISTANCE) : MoveAlgo::closeToPointThreshold*1.5f;
    return MoveAlgo::isClose(harad,*nearMob,d);
    };
  test.stepUntil(1500,[&] { return left && harad.isAiQueueEmpty() && arrived(); },"a walk behind: reach its point",[&] {
    left=left || harad.interactive()==nullptr;
    require(!left || !harad.isAiQueueEmpty() || arrived(),
            ("the end of the row must keep the walk of another command until it arrives: it stopped "+
             std::to_string(int(std::sqrt(harad.qDistTo(nearMob))))+" cm from "+nearMob->name).c_str());
    });
  // A leave queued behind the use stays when the watchdog fires.
  walkToObject("walk to the object, a leave behind: reserve");
  harad.aiPush(AiQueue::aiUseMob(anvil.scheme,-1));
  test.stepUntil(600,[&] {
    const auto* front=NpcDefinitionContract::queue(harad).front();
    return harad.interactive()==mob && front!=nullptr && front->act==AI_UseMob && front->i0==-1;
    },"a leave behind: sit down");
  later=test.world().time();
  later.addMilis(ActivityEvents::WatchdogMs+60*1000);
  gothic.gameSession()->setTime(later);
  test.stepUntil(600,[&] { return harad.interactive()==nullptr; },"the watchdog must keep a queued leave");
  test.stop(harad);

  // Watchdog: the attach event is lost, so the activity restarts once WatchdogMs later and holds.
  test.start(anvil,at);
  test.stepUntil(600,[&] { return ActivityEventsWorld::stage(harad)==Stage::Attach; },"watchdog: reserve");
  // The wait for the attach began on the tick the stage changed, at that tick's clock: its watchdog is WatchdogMs later.
  auto attachEnd=test.world().time();
  attachEnd.addMilis(ActivityEvents::WatchdogMs);
  test.stepUntil(600,[&] { return harad.interactive()==mob && harad.isAiQueueEmpty(); },"watchdog: attach unseen",[&] {
    NpcDefinitionContract::activityEvents(harad).pending&=uint16_t(~ActivityEvents::Attached);
    });
  const auto unseen=test.stats(state);
  test.step(60);
  require(ActivityEventsWorld::stage(harad)==Stage::Attach && ActivityEventsWorld::quiet(unseen,test.stats(state)),
          "a lost event must leave the activity waiting");
  const auto watchdogs=stats.watchdog;
  shortOf(attachEnd);
  test.step();
  require(test.world().time()<attachEnd && stats.watchdog==watchdogs && ActivityEventsWorld::stage(harad)==Stage::Attach,
          "the watchdog must wait to the last tick before its time");
  test.step();
  require(!(test.world().time()<attachEnd) && test.holds(anvil,harad,mob) && stats.watchdog==watchdogs+1 &&
          test.stats(state).steps==unseen.steps+1,
          "the watchdog must restart the activity on the first tick at its time, once, with one check of the place");

  // A reservation whose owner does not walk to use it (made here by hand for an NPC without an activity) goes on the
  // next world tick, whether or not a journal counts the wakes. Its count is this check's, not a defect's.
  test.stop(*stranger);
  stats.counting=false;
  require(test.places().reserve(*stranger,*farSame),"a free object must take a reservation");
  test.step();
  stats.counting=true;
  require(test.places().reserved(*stranger)==nullptr && stats.ownerless==ownerless+1,
          "a reservation without its owner must go on the next world tick, with no journal too");
  // The day journal writes what the audit counts: the ownerless reservation just counted, and two on one place
  // (counted here by hand: the places never let it happen).
  const auto doubled=stats.doubled;
  stats.doubled=doubled+7;
  const auto journal=ActivityEvents::json();
  require(journal.find("\"reservation_ownerless\":"+std::to_string(stats.ownerless)+",")!=std::string::npos &&
          journal.find("\"double_occupancy\":"+std::to_string(stats.doubled)+",")!=std::string::npos,
          "the day journal must carry the reservations without an owner and two on one place");
  stats.doubled=doubled;
  stats.ownerless=ownerless;

  // Two NPCs for one free object: exactly one gets it, the other waits and takes the second object once it is free.
  auto& first=*cool.npc;
  auto* second=test.extra();
  auto* blocker=test.extra();
  ActivityEventsWorld::detach(first);
  first.setPosition(cool.point->position());
  auto* coolA=test.world().availableMob(first,cool.scheme);
  require(coolA!=nullptr,"the cool point must reach a BSCOOL object");
  first.setPosition(test.awayFrom(*cool.point)->position());
  test.occupy(*blocker,*coolA,cool.scheme,cool.point->position());
  first.setPosition(cool.point->position());
  auto* coolB=test.world().availableMob(first,cool.scheme);
  require(coolB!=nullptr && coolB!=coolA,"the cool point must reach a second BSCOOL object");
  test.start(cool,cool.point->position());
  test.start(cool,cool.point->position()+Tempest::Vec3(80,0,0),second);
  test.only({&first,second});
  test.stepUntil(900,[&] {
    return (test.holds(cool,first,coolB) && ActivityEventsWorld::stage(*second)==Stage::Wait) ||
           (test.holds(cool,*second,coolB) && ActivityEventsWorld::stage(first)==Stage::Wait);
    },"two NPCs, one object",[&] {
    const auto* a=test.places().reserved(first),*b=test.places().reserved(*second);
    require(a==nullptr || a!=b,"two NPCs must never hold one reservation");
    require(first.interactive()==nullptr || first.interactive()!=second->interactive(),"two NPCs must never use one object");
    });
  test.leave(*blocker,cool.scheme,*test.awayFrom(*cool.point));
  test.stepUntil(900,[&] { return test.holds(cool,first) && test.holds(cool,*second); },"the second NPC takes the freed object");
  require(first.interactive()!=second->interactive(),"two NPCs must end on two objects");

  // The water pipe's distance approach: exactly 500 walks nowhere, 501 walks to the point. The distance is measured
  // here, from the body's centre to the point, not by the engine call the activity uses.
  auto& smoker=*pipe.npc;
  for(const int32_t distance:{500,501}) {
    std::optional<float> dx;
    for(float x=0;x<2000.f && !dx;x+=0.25f) {
      smoker.setPosition(pipe.point->position()+Tempest::Vec3(x,0,0));
      if(int32_t((pipe.point->position()-smoker.centerPosition()).length())==distance) dx=x;
      }
    require(dx.has_value(),"the pipe's threshold distances must be reachable");
    test.start(pipe,pipe.point->position()+Tempest::Vec3(*dx,0,0));
    NpcDefinitionContract::tickRoutine(smoker);
    const auto queue=activityQueue(smoker);
    const auto walks=std::count_if(queue.begin(),queue.end(),[&](const auto& a) {
      return a.starts_with("GoToPoint ") && a.find(" "+pipe.pointName+" ")!=std::string::npos;
      });
    require(walks==(distance==500 ? 0 : 1),"the pipe walks to its point past 500, and only there");
    test.stop(smoker);
    }

  // After a load an NPC attached to an object of its activity's scheme holds it, even before the object reaches the
  // use's state, and queues no use. To the automaton a load is a started state without it, made here by hand.
  {
    const auto* smoke=test.world().script().eventActivity(ScriptFn(pipe.phases.init));
    require(smoke!=nullptr,"the water pipe must run by events");
    test.start(pipe,pipe.point->position());
    NpcDefinitionContract::tickRoutine(smoker);
    activityDrain(smoker);
    auto* pipeMob=test.world().availableMob(smoker,pipe.scheme);
    require(pipeMob!=nullptr && smoker.setInteraction(pipeMob,true) && pipeMob->stateId()!=smoke->mob->useState &&
            !ActivityDefinitions::bodyStateContains(int32_t(smoker.bodyState()),smoke->mob->busyBodyState),
            "the smoker must sit at a pipe not yet in the use's state, not busy");
    NpcDefinitionContract::activityEvents(smoker)={};
    const auto uses=calls->uses(smoker);
    NpcDefinitionContract::tickRoutine(smoker);
    require(ActivityEventsWorld::stage(smoker)==Stage::Hold && calls->uses(smoker)==uses && NpcDefinitionContract::queue(smoker).size()==0,
            "after a load an NPC at an object of its activity must hold it and queue no use");
    test.stop(smoker);
  }

  // A save while one NPC holds its object, one waits, one walks to its point with another walk queued behind (a
  // second AI_GoToPoint), one walks to its reserved object with its use queued and a look behind (AI_LookAtNpc), and
  // one is saved right after Init, its commands still queued and another state behind them (AI_StartState).
  // One clock for all: each state ends by its own eTime, hours away.
  const auto stand=activityPhases(test.world().script(),"ZS_STAND_ARMSCROSSED");
  test.start(anvil,at);
  test.only({&harad});
  test.stepUntil(600,[&] { return test.holds(anvil,harad,mob); },"before the save: hold");
  test.stop(first);
  test.stop(*second);
  test.stop(smoker);
  const auto* coolAway=test.awayFrom(*cool.point);
  // the walker's second walk ends near the cool objects, not where the cool NPC's next state goes
  const WayPoint* walkerTo=nullptr;
  for(auto* object:{coolA,coolB})
    try { if(walkerTo==nullptr) walkerTo=test.nearObject(cool,*second,*object,coolAway); }
    catch(const std::runtime_error&) {}
  require(walkerTo!=nullptr,"a way point 3-9 m from a cool object");
  const std::string walkerToName=walkerTo->name;
  const auto* pipeAway=test.awayFrom(*pipe.point);
  const std::string pipeAwayName=pipeAway->name;
  std::vector<Npc*> smokers;
  smoker.setPosition(pipe.point->position());
  while(auto* free=test.world().availableMob(smoker,pipe.scheme)) {
    require(smokers.size()<8,"more SMOKE objects in reach than expected");
    auto* blockerPipe=test.extra();
    smoker.setPosition(pipeAway->position());
    test.occupy(*blockerPipe,*free,pipe.scheme,pipe.point->position());
    smoker.setPosition(pipe.point->position());
    smokers.push_back(blockerPipe);
    }
  EventActor* user=nullptr;
  const WayPoint* userPoint=nullptr;
  Interactive* userMob=nullptr;
  for(size_t k=2;k<6 && user==nullptr;++k) {
    auto& a=actors[k];
    test.stop(*a.npc);
    a.npc->setPosition(a.point->position());
    auto* object=test.world().availableMob(*a.npc,a.scheme);
    if(object==nullptr)
      continue;
    try {
      userPoint=test.nearObject(a,*a.npc,*object);
      user=&a;
      userMob=object;
      }
    catch(const std::runtime_error&) {}
    }
  require(user!=nullptr,"an accepted activity must have a way point 3-9 m from its object");
  auto& userNpc=*user->npc;
  test.start(pipe,pipe.point->position(),nullptr,false);
  test.start(*user,userPoint->position(),nullptr,false);
  userNpc.handle().wp=userPoint->name;
  // The second cool NPC walks to its point from far away, its Init's commands taken: only the walk is left.
  test.start(cool,test.awayFrom(*cool.point,1200.f)->position(),second,false);
  test.only({&harad,&first,&smoker,&userNpc,second});
  test.stepUntil(300,[&] {
    return ActivityEventsWorld::stage(smoker)==Stage::Wait && ActivityEventsWorld::stage(userNpc)==Stage::Attach &&
           test.places().reserved(userNpc)==userMob && userNpc.interactive()==nullptr && !userNpc.isAiQueueEmpty() &&
           ActivityEventsWorld::stage(*second)==Stage::Approach && NpcDefinitionContract::queue(*second).size()==0 &&
           !second->isAiQueueEmpty();
    },"before the save: waiting, walking to the point and walking to a reserved object");
  second->aiPush(AiQueue::aiGoToPoint(*walkerTo));
  userNpc.aiPush(AiQueue::aiLookAtNpc(&harad));
  // The cool NPC saved right after Init: its walk mode, then its walk, still queued, and another state behind.
  test.start(cool,coolAway->position(),nullptr,false);
  NpcDefinitionContract::tickRoutine(first);
  const auto coolQueue=activityQueue(first);
  require(ActivityEventsWorld::stage(first)==Stage::Approach && !coolQueue.empty() && !coolQueue.front().starts_with("GoToPoint ") &&
          std::any_of(coolQueue.begin(),coolQueue.end(),[](const auto& a) { return a.starts_with("GoToPoint "); }),
          "before the save the cool NPC's walk must wait behind its other Init commands");
  first.aiPush(AiQueue::aiStartState(ScriptFn(stand.init),1,nullptr,nullptr,coolAway->name));
  const auto anvilId=mob->zenId();
  std::vector<uint8_t> bytes;
  {
    Tempest::MemWriter writer(bytes);
    Serialize out(writer);
    Tempest::Pixmap preview(1,1,Tempest::TextureFormat::RGBA8);
    std::memset(preview.data(),0,4);
    gothic.gameSession()->save(out,"activity events",preview);
  }
  const auto names=std::array{npcName(test.world(),harad),npcName(test.world(),first),npcName(test.world(),smoker),
                              npcName(test.world(),*smokers.front()),npcName(test.world(),userNpc),npcName(test.world(),*second)};
  using Runs=std::array<std::vector<std::string>,std::tuple_size_v<decltype(names)>>;
  constexpr size_t walker=5,pipeBlocker=3;
  // The save loaded afresh, with the activities by events or polled: OPENGOTHIC_ACTIVITY_EVENTS is set before the
  // session is made, as a game takes it. Each NPC runs with its own routine and perceptions (the pipe's blocker stands
  // still), the clock past the watchdog's time meanwhile. Its saved commands and walk run until its routine takes over:
  // the queue is empty, or its Loop or the activity's check has run. By events the activity must not wake before that.
  // The run until then, as distinct states of its queue, walk, object, look and state, must equal the polled run, and
  // the walker must stand at its first point when its second walk starts. `routine` sees each NPC as its routine takes
  // over.
  const auto restoredRuns=[&](bool events,const std::function<void(size_t,Npc&)>& routine) {
    gothic.clearGame().reset();
    // The old world and its NPCs are gone: observe the loaded session afresh, its restoration too.
    calls.reset();
    calls.emplace();
    if(events) unsetenv("OPENGOTHIC_ACTIVITY_EVENTS"); else setenv("OPENGOTHIC_ACTIVITY_EVENTS","0",1);
    {
      Tempest::MemReader reader(bytes);
      Serialize in(reader);
      gothic.setGame(std::make_unique<GameSession>(in));
    }
    unsetenv("OPENGOTHIC_ACTIVITY_EVENTS");
    ActivityEventsWorld session(gothic);
    require(NpcDefinitionContract::activityEventsOn(session.world().script())==events,
            "a loaded session must take OPENGOTHIC_ACTIVITY_EVENTS when it is made");
    session.world().script().setSeed(73106);
    std::array<Npc*,std::tuple_size_v<decltype(names)>> npcs={};
    std::array<std::string,std::tuple_size_v<decltype(names)>> loops;
    for(size_t i=0;i<names.size();++i) {
      require((npcs[i]=activityNpc(session.world(),names[i].c_str()))!=nullptr,"the loaded world must have the actors");
      loops[i]=std::string(NpcDefinitionContract::aiState(*npcs[i]).hint)+"_LOOP";
      }
    session.only({npcs[0],npcs[1],npcs[2],npcs[4],npcs[5]});
    const auto* firstPoint=session.world().findWayPoint(cool.pointName);
    require(firstPoint!=nullptr,"the loaded world must have the cool point");
    const auto snapshot=[&](Npc& n) {
      const auto queue=activityQueue(n);
      const auto* look=n.lookAtTarget();
      return std::to_string(queue.size())+" "+(queue.empty() ? "-" : queue.front())+" | "+
             (NpcDefinitionContract::walking(n) ? "walk" : "stand")+" | "+
             (n.interactive()!=nullptr ? std::to_string(n.interactive()->zenId()) : "-")+" | "+
             (look!=nullptr ? npcName(session.world(),*look) : "-")+" | "+NpcDefinitionContract::aiState(n).hint;
      };
    Runs runs;
    std::array<bool,std::tuple_size_v<decltype(names)>> done={},queued={};
    for(size_t i=0;i<npcs.size();++i) {
      runs[i].push_back(snapshot(*npcs[i]));
      queued[i]=NpcDefinitionContract::queue(*npcs[i]).size()>0;
      if((done[i]=npcs[i]->isAiQueueEmpty()))
        routine(i,*npcs[i]);
      }
    bool secondWalk=false;
    size_t tick=0;
    for(;tick<900 && !std::all_of(done.begin(),done.end(),[](bool d) { return d; });++tick) {
      if(tick==10) {
        auto late=session.world().time();
        late.addMilis(ActivityEvents::WatchdogMs+60*1000);
        gothic.gameSession()->setTime(late);
        }
      session.step();
      for(size_t i=0;i<npcs.size();++i) {
        auto& n=*npcs[i];
        if(done[i])
          continue;
        const bool took=events ? n.activityStage()!=Stage::Off : calls->phase(n,loops[i])>0;
        // the routine runs only on a tick that starts with an empty queue
        require(!took || !queued[i],("after the load "+names[i]+"'s routine must wait for its saved queue").c_str());
        if(took || n.isAiQueueEmpty()) {
          done[i]=true;
          routine(i,n);
          continue;
          }
        const size_t size=NpcDefinitionContract::queue(n).size();
        if(i==walker && queued[i] && size==0) {
          // its second walk taken: at its first point, by the walk's own threshold (Npc::implGoTo), a shared one wider
          const float arrive=firstPoint->useCounter()>0 ? float(MAX_AI_USE_DISTANCE) : MoveAlgo::closeToPointThreshold*1.5f;
          require(MoveAlgo::isClose(n,*firstPoint,arrive+15.f),"the walk queued behind a saved walk must start at its point");
          secondWalk=true;
          }
        auto now=snapshot(n);
        if(runs[i].back()!=now)
          runs[i].push_back(std::move(now));
        queued[i]=size>0;
        }
      }
    require(tick>10 && tick<900 && secondWalk,"after the load the saved queues and walks must run on, then end");
    return runs;
    };
  const auto polled=restoredRuns(false,[](size_t,Npc&) {});
  const auto beforeLoad=test.stats(state);
  const auto userLoads=test.stats(user->state).wakes[1];
  const auto walkerChecks=test.stats(cool.state).steps;
  bool walkerLooked=false;
  const auto byEvents=restoredRuns(true,[&](size_t i,Npc& n) {
    if(i!=walker)
      return;
    // its saved walks done, the walker looks for its place once where they ended, without Init
    const auto* to=gothic.world()->findWayPoint(walkerToName);
    require(to!=nullptr && MoveAlgo::isClose(n,*to,MoveAlgo::closeToPointThreshold*1.5f+15.f) &&
            calls->phase(n,cool.state)==0 && test.stats(cool.state).steps==walkerChecks+1,
            "after its saved queue the activity must check its place once, where the queue left it, without Init");
    walkerLooked=true;
    });
  for(size_t i=0;i<names.size();++i) {
    const auto at=std::mismatch(polled[i].begin(),polled[i].end(),byEvents[i].begin(),byEvents[i].end());
    require(at.first==polled[i].end() && at.second==byEvents[i].end(),
            ("after the load "+names[i]+" must run its saved queue and walk as the polled Loop does: polled '"+
             (at.first!=polled[i].end() ? *at.first : "")+"', by events '"+(at.second!=byEvents[i].end() ? *at.second : "")+"'").c_str());
    }
  require(walkerLooked && polled[walker].size()>=2 && polled[4].size()>=3 && polled[1].size()>=4 && polled[pipeBlocker].size()==1,
          "the saved queues must run whole");

  ActivityEventsWorld loaded(gothic);
  auto* lHarad=activityNpc(loaded.world(),names[0].c_str());
  auto* lCool=activityNpc(loaded.world(),names[1].c_str());
  auto* lSmoker=activityNpc(loaded.world(),names[2].c_str());
  auto* lBlocker=activityNpc(loaded.world(),names[3].c_str());
  auto* lUser=activityNpc(loaded.world(),names[4].c_str());
  auto* lWalker=activityNpc(loaded.world(),names[5].c_str());
  require(lHarad->interactive()!=nullptr && lHarad->interactive()->zenId()==anvilId,"the loaded Harad must be at his anvil");
  require(lCool->isInState(ScriptFn(stand.init)),"the state queued behind the cool NPC's Init must run after its walk");
  auto* lMob=lHarad->interactive();
  loaded.stepUntil(60,[&] { return loaded.holds(*user,*lUser); },"loaded mid-use: hold");
  require(lUser->lookAtTarget()==lHarad && loaded.stats(user->state).wakes[1]==userLoads+1 && calls->uses(*lUser)==0 &&
          loaded.places().reserved(*lUser)==nullptr,
          "a load in the middle of a use keeps the object the saved use took past the reservations: one look, no new use");
  loaded.stepUntil(60,[&] { return loaded.holds(anvil,*lHarad,lMob); },"loaded mid-hold: hold");
  require(loaded.places().reserved(*lHarad)==nullptr && loaded.stats(state).wakes[3]==beforeLoad.wakes[3] &&
          loaded.stats(state).wakes[1]==beforeLoad.wakes[1]+1,
          "a load in the middle of holding keeps the place: one look after the load, no new use");
  require(!loaded.places().isFree(*lMob,*lCool),"the loaded occupied place must not be free for another NPC");
  const auto held=loaded.stats(state);
  loaded.step(60);
  require(lHarad->interactive()==lMob && ActivityEventsWorld::quiet(held,loaded.stats(state)),"the loaded holder must not wake again");
  require(calls->uses(*lHarad)==0 && calls->phase(*lHarad,state+"_LOOP")==0,"the loaded holder must queue no use and run no Loop");
  loaded.stepUntil(1500,[&] { return loaded.holds(cool,*lWalker); },"loaded mid-approach: reach the object");
  loaded.stepUntil(300,[&] { return ActivityEventsWorld::stage(*lSmoker)==Stage::Wait; },"loaded mid-wait: wait");
  // The old session and its way net are gone: the point again, from the loaded world.
  const auto* lPipeAway=loaded.world().findWayPoint(pipeAwayName);
  require(lPipeAway!=nullptr,"the loaded world must have the same way point");
  loaded.leave(*lBlocker,pipe.scheme,*lPipeAway);
  loaded.stepUntil(900,[&] { return loaded.holds(pipe,*lSmoker); },"loaded mid-wait: take the freed object");
  const auto smoking=loaded.stats(pipe.state);
  loaded.step(60);
  require(ActivityEventsWorld::quiet(smoking,loaded.stats(pipe.state)),"the loaded water pipe must hold without waking");
  loaded.places().audit(true);
  require(stats.ownerless==ownerless && stats.doubled==0 && stats.expired==2,"no reservation lost or doubled, two expired on purpose");
  require(stats.watchdog>0 && ActivityEvents::json().starts_with("{\"watchdog\":"+std::to_string(stats.watchdog)+",") &&
          ActivityEvents::json().find("\"reservation_expired\":2,")!=std::string::npos,
          "the day journal must carry the watchdog and the expired reservations");
  for(auto* npc:{lHarad,lCool,lSmoker,lUser,lWalker})
    loaded.stop(*npc);
  loaded.release();
  std::printf("Activity events at NEWWORLD objects: attach in ticks:%s; quiet hold and wait, freed place, exits, "
              "watchdog, two on one, pipe 500/501, load mid-hold/approach/wait/use; %llu place checks of the anvil\n",
              attaches.c_str(),(unsigned long long)loaded.stats(state).steps);
  }

// Item effects (graphics/itemview, reports/engine/2026-11-19-item-effects): an item's own effect lives on its mesh
// lying in the world and on a body, goes with the mesh, and is nowhere else.
void itemEffects(World& world) {
  auto& script=world.script();
  const auto symbol=[&](const char* name) {
    const auto id=script.findSymbolIndex(name);
    require(id!=size_t(-1),"item effect fixtures must be items of the base");
    return id;
    };
  const auto potion=symbol("ITPO_HEALTH_01"),gold=symbol("ITMI_GOLD"),claw=symbol("ITMW_BELIARWEAPON_1H_01"),
             firebow=symbol("ITRW_ADDON_FIREBOW");
  // the default is checked by itemEffectsDefault on empty INIs; here it is 1 whatever the installation's Gothic.ini says
  const int switchBefore=Settings::getI("GAME","itemEffects");
  Settings::setI("GAME","itemEffects",1);
  auto* npc=activityNpc(world,"VLK_412_HARAD");
  require(npc!=nullptr,"item effects need Harad of NEWWORLD");
  npc->setInteraction(nullptr,true);
  npc->clearAiQueue();
  const auto near=[](const Tempest::Vec3& a,const Tempest::Vec3& b,float cm) { return (a-b).length()<cm; };
  const auto meshAt=[](const ItemView& v) {
    const auto m=v.view().node(0).position();
    return Tempest::Vec3(m.at(3,0),m.at(3,1),m.at(3,2));
    };
  const auto viewOf=[](const Item* item) { return item!=nullptr ? NpcDefinitionContract::lying(*item) : nullptr; };
  // born: the emitter is active and has the item's mesh to take its points from
  const auto emitting=[](const ItemView& v) {
    return v.isEmitting() && v.effectMesh()==&v.view() && v.view().toMeshEmitter()!=nullptr;
    };
  const auto lay=[&](size_t id) {
    auto m=Tempest::Matrix4x4::mkIdentity();
    m.translate(npc->position()+Tempest::Vec3(100,50,0));
    return world.addItemDyn(id,m,size_t(-1));
    };
  // lying in the world: the potion has its effect on its own mesh, gold has none; the physics moves both together
  const auto alive=ItemView::census(world).alive;
  auto* lying=lay(potion);
  auto* plain=lay(gold);
  require(viewOf(lying)!=nullptr && viewOf(plain)!=nullptr,"items put in the world must have views");
  const auto& potionView=*viewOf(lying);
  require(lying->handle().effect=="SPELLFX_HEALTHPOTION" && !potionView.isEmpty() && emitting(potionView) &&
          ItemView::census(world).alive==alive+1,
          "ITPO_HEALTH_01 put in the world must have its live effect on its own mesh");
  require(plain->handle().effect.empty() && !viewOf(plain)->isEmpty() && !viewOf(plain)->hasEffect(),
          "an item without C_ITEM.effect must have no effect");
  auto fallen=lying->transform();
  fallen.translate(0,-40,25);
  lying->setPhysicsMatrix(fallen);
  Tempest::Vec3 at;
  require(potionView.effectPosition(at) && near(at,meshAt(potionView),.01f) &&
          near(at,Tempest::Vec3(fallen.at(3,0),fallen.at(3,1),fallen.at(3,2)),.01f),
          "a lying item moved by the physics must take its effect along");
  // the chunk streamed out: the mesh goes, its effect with it
  NpcDefinitionContract::visual(*lying).releaseView();
  require(potionView.isEmpty() && !potionView.hasEffect() && ItemView::census(world).alive==alive,
          "a lying item's released view must have no effect");
  world.removeItem(*lying);
  world.removeItem(*plain);

  // Beliar's claw: in the hand once drawn, the effect where the hand is, also after the npc moved
  const auto& sword=NpcDefinitionContract::sword(*npc);
  npc->setToFightMode(claw);
  npc->updateTransform();
  require(npc->weaponState()==WeaponState::W1H && npc->inventory().itemCount(claw)==1,"Harad must draw Beliar's claw");
  require(emitting(sword) && sword.effectPosition(at) && near(at,meshAt(sword),.01f) &&
          near(at,npc->mapBone("ZS_RIGHTHAND"),15.f),"Beliar's claw drawn must have its effect in the hand");
  const auto hand=npc->mapBone("ZS_RIGHTHAND");
  npc->setPosition(npc->position()+Tempest::Vec3(300,0,0));
  npc->updateTransform();
  Tempest::Vec3 after;
  const auto handAfter=npc->mapBone("ZS_RIGHTHAND");
  std::printf("Item effects: claw in the hand at %.1f cm of ZS_RIGHTHAND, moved %.1f cm with it (hand %.1f cm)\n",
              double((at-hand).length()),double((sword.effectPosition(after) ? after-at : Tempest::Vec3()).length()),
              double((handAfter-hand).length()));
  require(sword.effectPosition(after) && (handAfter-hand).length()>250.f && near(after-at,handAfter-hand,1.f) &&
          near(after,meshAt(sword),.01f),"the claw's effect must follow the hand bone");
  // on the belt it keeps the effect, taken off into the inventory it has none
  npc->closeWeapon(true);
  npc->updateTransform();
  require(npc->weaponState()==WeaponState::NoWeapon && emitting(sword) && sword.effectPosition(at) &&
          near(at,npc->mapBone("ZS_SWORD"),15.f),"Beliar's claw put away must keep its effect on the belt");
  npc->unequipItem(claw);
  require(npc->inventory().itemCount(claw)==1 && sword.isEmpty() && !sword.hasEffect(),
          "Beliar's claw taken off into the inventory must have no effect");
  // the fire bow on the back
  const auto& bow=NpcDefinitionContract::bow(*npc);
  npc->addItem(firebow,1);
  npc->useItem(firebow,Item::NSLOT,true);
  npc->updateTransform();
  require(emitting(bow) && bow.effectPosition(at) && near(at,meshAt(bow),.01f) &&
          near(at,npc->mapBone("ZS_BOW"),15.f),"ITRW_ADDON_FIREBOW on the back must have its effect");
  // switched off: no new view has an effect
  Settings::setI("GAME","itemEffects",0);
  npc->unequipItem(firebow);
  npc->useItem(firebow,Item::NSLOT,true);
  npc->setToFightMode(claw);
  npc->updateTransform();
  auto* off=lay(potion);
  require(!bow.isEmpty() && !bow.hasEffect() && !sword.isEmpty() && !sword.hasEffect() && off!=nullptr &&
          viewOf(off)!=nullptr && !viewOf(off)->isEmpty() && !viewOf(off)->hasEffect() && ItemView::census(world).alive==alive,
          "with [GAME] itemEffects 0 no item may have an effect");
  world.removeItem(*off);
  npc->closeWeapon(true);
  Settings::setI("GAME","itemEffects",switchBefore);
  std::puts("Item effects: lying potion on its mesh and with the physics, gold none, released none; claw in the hand, "
            "on the belt, none in the inventory; fire bow on the back; none with itemEffects 0");
  }

// Effects of the animations' events (docs/map/engine.md, «Частицы анимации»): the forge's looped FIRE_HOT burns while
// the smith works the bellows and goes out with his animation out or with the forge's rest without one; the anvil's
// ANVIL_SPARKS live their own time.
void animationEffects(Gothic& gothic) {
  // a fresh game: the session before comes from a load of activityEventsWorld's world, where the forge's worker left it
  // in S1 without its fire, which only T_S0_2_S1 lights
  gothic.clearGame().reset();
  gothic.setGame(std::make_unique<GameSession>("NEWWORLD.ZEN"));
  auto& world=*gothic.world();
  auto& script=world.script();
  auto* session=gothic.gameSession();
  const auto step=[&](uint64_t dt) { session->tick(dt); session->updateAnimation(dt); };
  const auto* fire=GameServices::inst().loadParticleFx("FIRE_HOT");
  const auto* sparks=GameServices::inst().loadParticleFx("ANVIL_SPARKS");
  require(fire!=nullptr && fire->ppsIsLooping && fire->effectPrefferedTime()<5000 && sparks!=nullptr && !sparks->ppsIsLooping,
          "FIRE_HOT must loop with its own time under 5 s, ANVIL_SPARKS must not loop");
  // a worker of the scheme at his point, his routine off: the AI queue only
  const auto worker=[&](const char* state,const char* scheme,Npc*& npc) {
    const auto init=activityPhases(script,state).init;
    std::string pointName;
    const WayPoint* point=nullptr;
    for(uint32_t i=0;i<world.npcCount() && point==nullptr;++i)
      if(activityCandidate(*world.npcById(i)) && (point=activityRow(*world.npcById(i),init,pointName))!=nullptr)
        npc=world.npcById(i);
    require(point!=nullptr,(std::string("NEWWORLD must have an NPC with a ")+state+" row").c_str());
    npc->setInteraction(nullptr,true);
    npc->clearAiQueue();
    npc->setExternalControl(true);
    npc->setPosition(point->position());
    auto* mob=world.availableMob(*npc,scheme);
    require(mob!=nullptr,(npcName(world,*npc)+"'s point must have a free "+scheme).c_str());
    return mob;
    };
  const auto tickUntil=[&](const std::function<bool()>& done,uint64_t ms) {
    for(uint64_t t=0;t<ms && !done();t+=50)
      step(50);
    return done();
    };
  const auto useMob=[&](Npc& npc,const char* scheme,int32_t state) {
    NpcActivityHost(world,npc,[](std::string_view) {}).useMob(scheme,state);
    };
  const auto slot=[&](const Interactive& mob,int id) -> const Effect* {
    for(const auto& e:NpcDefinitionContract::effects(NpcDefinitionContract::model(mob)))
      if(e.id==id)
        return &e.view;
    return nullptr;
    };

  Npc* smith=nullptr;
  auto* forge=worker("ZS_SMITH_FIRE","BSFIRE",smith);
  const auto& model=NpcDefinitionContract::model(*forge);
  const auto fireBone=NpcDefinitionContract::bone(model,"BIP01 PFX_FIRE");
  require(fireBone!=size_t(-1),"BSFIRE_OC must have BIP01 PFX_FIRE");
  // the number of the effect: FIRE_HOT on the bone in T_S0_2_S1, its stop in T_S1_2_S0
  const auto& solver=NpcDefinitionContract::solver(model);
  const auto* in=solver.solveFrm("T_S0_2_S1");
  const auto* out=solver.solveFrm("T_S1_2_S0");
  require(in!=nullptr && out!=nullptr,"BSFIRE_OC must have T_S0_2_S1 and T_S1_2_S0");
  const auto lighting=std::find_if(in->data->pfx.begin(),in->data->pfx.end(),[](const auto& e) {
    return !e.pfxStop && e.name=="FIRE_HOT" && e.position=="BIP01 PFX_FIRE"; });
  require(lighting!=in->data->pfx.end(),"T_S0_2_S1 must light FIRE_HOT at BIP01 PFX_FIRE");
  const int fireSlot=lighting->index;
  require(std::any_of(out->data->pfx.begin(),out->data->pfx.end(),[&](const auto& e) { return e.pfxStop && e.index==fireSlot; }),
          "T_S1_2_S0 must stop FIRE_HOT by its number");
  const auto bellows=[&](bool lit) {
    useMob(*smith,"BSFIRE",1);
    return tickUntil([&] { return forge->stateId()==forge->stateCount() && (!lit || slot(*forge,fireSlot)!=nullptr); },20000);
    };
  const auto inAnim=[&](const char* name) { return model.pose().isInAnim(name); };
  // FIRE_HOT burns on its bone with its emitter alive past its own time and its last particle's life
  const auto aliveFor=fire->effectPrefferedTime()+fire->maxLifetime();
  const auto burns=[&](uint64_t since) {
    Tempest::Vec3 at;
    const auto* burning=slot(*forge,fireSlot);
    return burning!=nullptr && burning->isEmitting() && burning->isAlive() && world.tickCount()>since+aliveFor &&
           burning->emitterPosition(at) && (at-model.mapBone(fireBone)).length()<1.f;
    };
  // FIRE_HOT with an own light (particles.d "light"), set on its declaration for the test: the forge's effect gives
  // the world's light its range, colours and keys going round, at its "at" in the axes of BIP01 PFX_FIRE
  auto& fireDecl=const_cast<ParticleFx&>(*fire);
  const auto fireLight=std::make_tuple(fire->lightRange,fire->lightColor,fire->lightKeys,fire->lightColors,fire->lightFps,
                                       fire->lightAt,fire->lightLoop,fire->node);
  fireDecl.lightRange=300; fireDecl.lightColor={255,140,60}; fireDecl.lightKeys={0.6f,1.f,0.7f};
  fireDecl.lightColors={{255,120,40},{255,190,110}}; fireDecl.lightFps=12.5f; fireDecl.lightAt={-8,77,1}; fireDecl.lightLoop=true;
  const auto ownLight=[&](const Effect* e) {
    Effect::OwnLight own;
    if(e==nullptr || !e->ownLight(own))
      return false;
    const auto& m=model.pose().bone(fireBone);
    const Tempest::Vec3 at=Tempest::Vec3(m.at(3,0),m.at(3,1),m.at(3,2))+Tempest::Vec3(m.at(0,0),m.at(0,1),m.at(0,2))*-8.f+
                           Tempest::Vec3(m.at(1,0),m.at(1,1),m.at(1,2))*77.f+Tempest::Vec3(m.at(2,0),m.at(2,1),m.at(2,2));
    return own.loop && own.preset.range==300.f && own.preset.color==asset::Color(255,140,60,255) &&
           own.preset.range_animation_scale==fire->lightKeys && own.preset.range_animation_fps==12.5f &&
           own.preset.color_animation_list==std::vector<asset::Color>{asset::Color(255,120,40,255),asset::Color(255,190,110,255)} &&
           own.preset.color_animation_fps==12.5f && (own.at-at).length()<0.1f && (own.at-model.mapBone(fireBone)).length()>70.f;
    };
  require(bellows(true),(npcName(world,*smith)+" must work the bellows of BSFIRE with FIRE_HOT burning").c_str());
  const auto lit=world.tickCount();
  require(ownLight(slot(*forge,fireSlot)),"FIRE_HOT's own light must stand at its at in the axes of BIP01 PFX_FIRE, its keys going round");
  require(tickUntil([&] { return inAnim("S_S1"); },5000),"the forge must reach the bellows' cycle S_S1");
  const auto cycle=world.tickCount();
  // without "node" the emitter goes with BIP01 PFX_FIRE, which the bellows toss up and down
  float fireLow=1e9f,fireHigh=-1e9f;
  bool  follows=true;
  require(!tickUntil([&] {
            Tempest::Vec3 at;
            const auto* burning=slot(*forge,fireSlot);
            if(burning==nullptr)
              return true;
            const auto bone=model.mapBone(fireBone);
            follows=follows && burning->emitterPosition(at) && (at-bone).length()<1.f;
            fireLow=std::min(fireLow,bone.y); fireHigh=std::max(fireHigh,bone.y);
            return false;
            },5000) && world.tickCount()>=cycle+5000 &&
          forge->stateId()==forge->stateCount() && burns(lit),
          "FIRE_HOT must burn at BIP01 PFX_FIRE with its emitter alive after 5 s of S_S1, not its own time");
  std::printf("Animation effects: BIP01 PFX_FIRE goes %.1f cm up and down in S_S1\n",double(fireHigh-fireLow));
  require(follows && fireHigh-fireLow>30.f,"without node FIRE_HOT must go with BIP01 PFX_FIRE up and down in S_S1");
  require(ownLight(slot(*forge,fireSlot)),"FIRE_HOT's own light must follow BIP01 PFX_FIRE while it burns");
  // its pulse keeps the beat: a block taken again near after it was freed far away counts the looped keys from the
  // effect's birth by the world's clock (PfxBucket::implTickCommon); a burst's keys count from its block as before
  {
  const uint64_t period=fire->prefferedTime,dt=16,now=lit+7*period+period/3;
  const auto beat=fire->keySpan(true,lit,now,dt,0);
  const auto burst=sparks->keySpan(true,lit,now,dt,0);
  std::printf("Animation effects: FIRE_HOT's keys %llu ms after its birth: %llu..%llu ms of its %llu ms period\n",
              (unsigned long long)(now-lit),(unsigned long long)beat.first,(unsigned long long)beat.second,(unsigned long long)period);
  require(period/3>=dt && beat.second==(now-lit)%period && beat.first+dt==beat.second &&
          burst==std::pair<uint64_t,uint64_t>(0,dt) && fire->keySpan(false,lit,now,dt,40)==std::pair<uint64_t,uint64_t>(40,56),
          "a looped emitter's new block must count its keys from its birth by the world's clock, a burst's from 0");
  }
  std::printf("Animation effects: FIRE_HOT of BSFIRE burns %llu ms of S_S1, %llu ms from its lighting (its own time and "
              "particle life %llu ms)\n",(unsigned long long)(world.tickCount()-cycle),(unsigned long long)(world.tickCount()-lit),
              (unsigned long long)aliveFor);
  // the smith goes: the stop at its frame of T_S1_2_S0 puts FIRE_HOT out while the transition still plays
  const auto stopFrame=std::find_if(out->data->pfx.begin(),out->data->pfx.end(),[&](const auto& e) { return e.pfxStop && e.index==fireSlot; });
  // an event fires once the frame after its own begins, a reversed sequence counts from its end (Sequence::extractFrames)
  const auto stopFrom=std::max(0,stopFrame->frame-int32_t(out->data->firstFrame));
  const auto stopFrames=out->reverse ? int32_t(out->data->numFrames)-stopFrom : stopFrom+1;
  const auto stopAt=uint64_t(float(stopFrames)*1000.f/out->data->fpsRate);
  useMob(*smith,"BSFIRE",-1);
  uint64_t outFrom=0,outGone=0;
  bool outPlays=false;
  for(uint64_t t=0;t<20000 && outGone==0;t+=10) {
    session->tick(10);
    session->updateAnimation(10);
    if(outFrom==0 && inAnim("T_S1_2_S0"))
      outFrom=world.tickCount();
    if(outFrom!=0 && slot(*forge,fireSlot)==nullptr) {
      outGone=world.tickCount();
      outPlays=inAnim("T_S1_2_S0");
      }
    }
  std::printf("Animation effects: FIRE_HOT out %llu ms into T_S1_2_S0 (%s), its stop at frame %d of %u from %u%s is at %llu ms\n",
              (unsigned long long)(outGone-outFrom),outPlays ? "playing" : "over",stopFrame->frame,out->data->numFrames,
              out->data->firstFrame,out->reverse ? " reversed" : "",(unsigned long long)stopAt);
  require(outFrom!=0 && outGone!=0 && outPlays && outGone+10>=outFrom+stopAt && outGone<=outFrom+stopAt+50,
          "T_S1_2_S0's stop must put FIRE_HOT out at its frame while the transition plays");
  require(tickUntil([&] { return smith->interactive()==nullptr && forge->stateId()<=0; },20000) && slot(*forge,fireSlot)==nullptr,
          "the smith must leave the forge at rest without FIRE_HOT");
  // particles.d "node": FIRE_HOT with node BIP01 FPLACE stands on the forge's still root, while BIP01 PFX_FIRE, the
  // event's node, goes up and down with the bellows
  {
  const auto rootBone=NpcDefinitionContract::bone(model,"BIP01 FPLACE");
  require(rootBone!=size_t(-1),"BSFIRE_OC must have BIP01 FPLACE");
  fireDecl.node="BIP01 FPLACE";
  require(bellows(true) && tickUntil([&] { return inAnim("S_S1"); },5000),"the smith must light FIRE_HOT on BIP01 FPLACE and work the bellows");
  Tempest::Vec3 first,at;
  float low=1e9f,high=-1e9f,moved=0;
  bool  onRoot=true;
  for(int i=0;i<60;++i) { // 3 s: more than a cycle of the bellows
    step(50);
    const auto* burning=slot(*forge,fireSlot);
    require(burning!=nullptr && burning->emitterPosition(at),"FIRE_HOT on BIP01 FPLACE must burn through S_S1");
    if(i==0)
      first=at;
    moved =std::max(moved,(at-first).length());
    onRoot=onRoot && (at-model.mapBone(rootBone)).length()<1.f;
    low   =std::min(low,model.mapBone(fireBone).y);
    high  =std::max(high,model.mapBone(fireBone).y);
    }
  std::printf("Animation effects: FIRE_HOT on BIP01 FPLACE moves %.2f cm while BIP01 PFX_FIRE goes %.1f cm\n",
              double(moved),double(high-low));
  require(inAnim("S_S1") && high-low>30.f && onRoot && moved<1.f,
          "FIRE_HOT with node BIP01 FPLACE must stay on it while BIP01 PFX_FIRE goes up and down in S_S1");
  useMob(*smith,"BSFIRE",-1);
  require(tickUntil([&] { return smith->interactive()==nullptr && forge->stateId()<=0; },20000) && slot(*forge,fireSlot)==nullptr,
          "the smith must leave the forge at rest after FIRE_HOT on BIP01 FPLACE");
  fireDecl.node.clear();
  }
  // the forge back at rest without its animation out right after the lighting (the routine's reset)
  require(bellows(true) && inAnim("T_S0_2_S1"),"the smith must light FIRE_HOT again in T_S0_2_S1");
  smith->setInteraction(nullptr,true);
  forge->resetPositionToTA(0);
  step(50);
  require(forge->stateId()<=0 && slot(*forge,fireSlot)==nullptr,"FIRE_HOT must go out at once with the forge at rest without its animation out");
  // lit far from the viewer: it burns when the viewer comes
  world.setHeadlessViewer(forge->position()+Tempest::Vec3(1.e6f,0,0));
  require(!world.isInPfxRange(forge->position()),"the forge must be out of the particles' range");
  require(bellows(false) && tickUntil([&] { return inAnim("S_S1"); },5000),"the smith must work the bellows far from the viewer");
  const auto farCycle=world.tickCount();
  tickUntil([] { return false; },std::max<uint64_t>(aliveFor,1000)+100);
  world.setHeadlessViewer(forge->position());
  step(50);
  require(burns(farCycle),"FIRE_HOT lit far from the viewer must burn when the viewer comes");
  smith->setInteraction(nullptr,true);
  forge->resetPositionToTA(0);
  step(50);
  std::tie(fireDecl.lightRange,fireDecl.lightColor,fireDecl.lightKeys,fireDecl.lightColors,fireDecl.lightFps,fireDecl.lightAt,
           fireDecl.lightLoop,fireDecl.node)=fireLight;

  // the anvil's sparks of each blow go by their own time
  Npc* harad=nullptr;
  auto* anvil=worker("ZS_SMITH_ANVIL","BSANVIL",harad);
  world.setHeadlessViewer(anvil->position());
  const auto* hammering=NpcDefinitionContract::solver(NpcDefinitionContract::model(*anvil)).solveFrm("S_S1");
  require(hammering!=nullptr,"BSANVIL_OC must have S_S1");
  const auto striking=std::find_if(hammering->data->pfx.begin(),hammering->data->pfx.end(),[](const auto& e) {
    return !e.pfxStop && e.name=="ANVIL_SPARKS"; });
  require(striking!=hammering->data->pfx.end(),"S_S1 of BSANVIL_OC must strike ANVIL_SPARKS");
  const int blow=striking->index;
  useMob(*harad,"BSANVIL",1);
  require(tickUntil([&] { return slot(*anvil,blow)!=nullptr; },20000),(npcName(world,*harad)+"'s blows must strike ANVIL_SPARKS").c_str());
  const auto struck=world.tickCount();
  uint64_t until=0;
  for(const auto& e:NpcDefinitionContract::effects(NpcDefinitionContract::model(*anvil)))
    if(e.id==blow)
      until=e.timeUntil;
  require(until==struck+sparks->effectPrefferedTime(),"ANVIL_SPARKS must live their own time");
  Effect::OwnLight none;
  require(!slot(*anvil,blow)->ownLight(none),"ANVIL_SPARKS without an own light must bring none");
  const auto there=[&] {
    for(const auto& e:NpcDefinitionContract::effects(NpcDefinitionContract::model(*anvil)))
      if(e.id==blow && e.timeUntil==until)
        return true;
    return false;
    };
  // to the end of their time in steps that land on it, then one step more
  while(world.tickCount()<until)
    step(std::min<uint64_t>(until-world.tickCount(),10));
  require(world.tickCount()==until && there(),"ANVIL_SPARKS must be there at the end of their own time");
  step(1);
  require(!there(),"ANVIL_SPARKS must be gone right after their own time");
  std::printf("Animation effects: ANVIL_SPARKS of a blow gone in %llu ms (own time %llu ms)\n",
              (unsigned long long)(world.tickCount()-struck),(unsigned long long)sparks->effectPrefferedTime());
  }

// Arrows (reports/engine/2026-11-19-item-effects): a shot arrow's effect at KEY_INVEST_2, at its first hit KEY_INVEST_3;
// stuck it goes with the arrow, in a body with its bone, and fades in 20 s of the game's time while the arrow stays.
// A save keeps the stuck arrows as before (main's layout), loaded they have no effect.
void arrowEffects(Gothic& gothic) {
  auto* world=gothic.world();
  auto& script=world->script();
  const auto fireArrow=script.findSymbolIndex("ITRW_ADDON_FIREARROW"),arrow=script.findSymbolIndex("ITRW_ARROW");
  require(fireArrow!=size_t(-1) && arrow!=size_t(-1),"arrow effects need ITRW_ADDON_FIREARROW and ITRW_ARROW");
  const auto* fx=GameServices::inst().loadVisualFx("SPELLFX_FIREARROW");
  require(fx!=nullptr,"SPELLFX_FIREARROW must be known");
  const auto* shot=fx->key(SpellFxKey::Invest,2);
  const auto* hit=fx->key(SpellFxKey::Invest,3);
  require(shot!=nullptr && hit!=nullptr,"SPELLFX_FIREARROW must have KEY_INVEST_2 and KEY_INVEST_3");
  std::printf("Arrow effects: SPELLFX_FIREARROW light %.0f cm shot, %.0f cm hit\n",double(shot->lightRange),double(hit->lightRange));
  require(shot->lightRange==300.f && hit->lightRange==400.f,"the fire arrow's light must be 300 cm shot and 400 cm hit");
  // arrows stick for this scenario whatever the installation's Gothic.ini says
  const int stickingBefore=Settings::getI("GAME","arrow_sticking");
  Settings::setI("GAME","arrow_sticking",1);
  Settings::inst().refreshPhysicsFlags();
  require(Settings::physicsFlags().arrowSticking,"arrows must stick (GAME arrow_sticking)");
  const int switchBefore=Settings::getI("GAME","itemEffects");
  Settings::setI("GAME","itemEffects",1);
  auto* session=gothic.gameSession();
  const auto near=[](const Tempest::Vec3& a,const Tempest::Vec3& b,float cm) { return (a-b).length()<cm; };
  const auto meshAt=[](const ItemView& v) {
    const auto m=v.view().node(0).position();
    return Tempest::Vec3(m.at(3,0),m.at(3,1),m.at(3,2));
    };
  const auto emitting=[](const ItemView& v) {
    return v.isEmitting() && v.effectMesh()==&v.view() && v.view().toMeshEmitter()!=nullptr;
    };
  const auto onMesh=[&](const ItemView& v) {
    Tempest::Vec3 at;
    return v.effectPosition(at) && near(at,meshAt(v),.01f);
    };
  auto* npc=activityNpc(*world,"VLK_412_HARAD");
  require(npc!=nullptr,"arrow effects need Harad of NEWWORLD");
  // the arrow item a bullet is made of; shot by nobody: no damage, no fight
  const auto shoot=[&](size_t id,const Tempest::Vec3& from,const Tempest::Vec3& dir) -> Bullet& {
    auto m=Tempest::Matrix4x4::mkIdentity();
    m.translate(from+Tempest::Vec3(0,5000,0));
    auto* item=world->addItemDyn(id,m,size_t(-1));
    require(item!=nullptr,"the arrow item must be made");
    auto& b=world->shootArrow(*item,from,dir,DynamicWorld::bulletSpeed);
    world->removeItem(*item);
    return b;
    };
  const auto tickUntil=[&](const std::function<bool()>& done,uint64_t dt,size_t max) {
    for(size_t i=0;i<max && !done();++i)
      session->tick(dt);
    return done();
    };

  // the ground by Harad, found from above: stone throws an arrow back, other ground (not metal) takes it
  const auto findSurface=[&](const std::function<bool(asset::MaterialGroup)>& take,Tempest::Vec3& out) {
    for(float r=150;r<=3000;r+=150)
      for(int a=0;a<12;++a) {
        const float an=float(a)*float(M_PI)/6.f;
        const auto p=npc->position()+Tempest::Vec3(std::cos(an)*r,0,std::sin(an)*r);
        const auto land=world->physic()->ray(p+Tempest::Vec3(0,300,0),p-Tempest::Vec3(0,700,0));
        if(!land.hasCol || land.n.y<.7f || !take(land.mat))
          continue;
        out=land.v;
        std::printf("Arrow effects: ground of material %d %.0f cm from Harad\n",int(land.mat),double(r));
        return true;
        }
    return false;
    };
  const auto findGround=[&](bool stone,Tempest::Vec3& out) {
    return findSurface([stone](asset::MaterialGroup m) {
      return (m==asset::MaterialGroup::STONE)==stone && m!=asset::MaterialGroup::METAL &&
             m!=asset::MaterialGroup::UNDEFINED && m!=asset::MaterialGroup::NONE;
      },out);
    };
  // How deep a stuck arrow's tip is (reports/physics/2026-11-19-arrow-pin-depth): its mesh lies along X, the tip at
  // its box's −X, −X is the flight. The tip's depth past the entry along the flight, its distance off that line,
  // the mesh's origin before the entry, a point of the mesh's X along the flight.
  struct Depth { float tip=0,length=0,off=0,origin=0; Tempest::Vec3 axis; };
  const auto depthOf=[](const Tempest::Matrix4x4& m,const ProtoMesh* mesh,const Tempest::Vec3& entry) {
    Depth d;
    const Tempest::Vec3 x(m.at(0,0),m.at(0,1),m.at(0,2)),at(m.at(3,0),m.at(3,1),m.at(3,2));
    d.length=mesh!=nullptr ? -mesh->bbox()[0].x : 0.f;
    d.axis=Tempest::Vec3::normalize(-x);
    const auto tip=at-x*d.length;
    d.tip=Tempest::Vec3::dotProduct(tip-entry,d.axis);
    d.off=(tip-entry-d.axis*d.tip).length();
    d.origin=Tempest::Vec3::dotProduct(entry-at,d.axis);
    return d;
    };
  const auto meshPoint=[](const Tempest::Matrix4x4& m,float x) {
    return Tempest::Vec3(m.at(3,0)+m.at(0,0)*x,m.at(3,1)+m.at(0,1)*x,m.at(3,2)+m.at(0,2)*x);
    };
  constexpr float tow=-52.f; // the fire arrow's tow on its mesh's X (SPELLFX_FIREARROW)
  // one arrow into the world: shot, ticked until it sticks; its pin, its mesh's depth and where it entered
  const auto pinWorld=[&](size_t id,const Tempest::Vec3& from,const Tempest::Vec3& dir,Tempest::Matrix4x4& m,
                          const char* what) {
    const auto before=world->stuckArrowCount();
    auto& b=shoot(id,from,dir);
    for(size_t i=0;i<3000 && world->stuckArrowCount()==before && !b.isFinished();++i)
      session->tick(4);
    require(world->stuckArrowCount()==before+1 && world->stuckArrowTransform(before,m),what);
    const auto d=depthOf(m,world->stuckArrowView(before)->protoMesh(),b.impactPoint());
    std::printf("Arrow depth: %s, mesh tip %.1f cm, tip %.2f cm past the hit, %.2f cm off its line, origin %.2f cm "
                "before it\n",what,double(d.length),double(d.tip),double(d.off),double(d.origin));
    // along the flight it came in, not only along the pinned matrix's own −X
    const auto flight=Tempest::Vec3::normalize(b.direction());
    require(d.length>10.f && d.off<1.f && std::abs(d.origin-(d.length-d.tip))<.05f &&
            Tempest::Vec3::dotProduct(d.axis,flight)>.9999f,what);
    return std::make_pair(d,b.impactPoint());
    };
  Tempest::Vec3 ground;
  require(findGround(false,ground),"ground an arrow sticks in must be found by Harad");

  // in the earth the tip 15 cm past the hit, the mesh's origin its length less 15 before it; a bolt by its own mesh;
  // the fire arrow's tow above the ground. In wood 8 cm, the tow outside.
  {
    Tempest::Vec3 earth,wood;
    require(findSurface([](asset::MaterialGroup m) { return m==asset::MaterialGroup::EARTH; },earth),
            "earth an arrow sticks in must be found by Harad");
    const auto bolt=script.findSymbolIndex("ITRW_BOLT");
    require(bolt!=size_t(-1),"arrow depth needs ITRW_BOLT");
    Tempest::Matrix4x4 m;
    const auto inEarth=pinWorld(arrow,earth+Tempest::Vec3(0,120,0),Tempest::Vec3(.3f,-1,0),m,
                                "ITRW_ARROW in the earth");
    require(std::abs(inEarth.first.tip-15.f)<=1.f && std::abs(inEarth.first.origin-(inEarth.first.length-15.f))<=1.f,
            "an arrow in the earth must have its tip 15 cm past the hit, its mesh's origin its length less 15 before");
    const auto boltDepth=pinWorld(bolt,earth+Tempest::Vec3(10,120,10),Tempest::Vec3(0,-1,0),m,
                                "ITRW_BOLT in the earth");
    require(std::abs(boltDepth.first.length-inEarth.first.length)>1.f && std::abs(boltDepth.first.tip-15.f)<=1.f,
            "a bolt must be pinned by the length of its own mesh");
    const auto fireDepth=pinWorld(fireArrow,earth+Tempest::Vec3(-10,120,-10),Tempest::Vec3(0,-1,0),m,
                                "ITRW_ADDON_FIREARROW in the earth");
    const auto towAt=meshPoint(m,tow);
    std::printf("Arrow depth: the fire arrow's tow %.2f cm over the earth\n",double(towAt.y-fireDepth.second.y));
    require(std::abs(fireDepth.first.tip-15.f)<=1.f && towAt.y>=fireDepth.second.y,"the fire arrow's tow in the earth must not be under it");
    require(findSurface([](asset::MaterialGroup m) { return m==asset::MaterialGroup::WOOD; },wood),
            "wood an arrow sticks in must be found by Harad");
    const auto inWood=pinWorld(fireArrow,wood+Tempest::Vec3(0,120,0),Tempest::Vec3(0,-1,0),m,
                                "ITRW_ADDON_FIREARROW in wood");
    const auto towOut=Tempest::Vec3::dotProduct(meshPoint(m,tow)-inWood.second,inWood.first.axis);
    require(std::abs(inWood.first.tip-8.f)<=1.f && towOut<0.f,"an arrow in wood must have its tip 8 cm past the hit, the tow outside");
    // the matrix it was pinned with is drawn on: no tick moves a world arrow
    session->tick(16);
  }
  // shot: the effect at KEY_INVEST_2 on the arrow's mesh
  const auto stuckBefore=world->stuckArrowCount();
  auto& fire=shoot(fireArrow,ground+Tempest::Vec3(0,120,0),Tempest::Vec3(0,-1,0));
  require(emitting(fire.itemView()) && fire.itemView().effectKey()==shot && onMesh(fire.itemView()),
          "a shot ITRW_ADDON_FIREARROW must fly with its live effect at KEY_INVEST_2");
  // the plain arrow and the switch: no effect
  auto& plain=shoot(arrow,ground+Tempest::Vec3(0,120,5),Tempest::Vec3(0,-1,0));
  require(!plain.itemView().isEmpty() && !plain.itemView().hasEffect(),"a shot ITRW_ARROW must have no effect");
  Settings::setI("GAME","itemEffects",0);
  auto& off=shoot(fireArrow,ground+Tempest::Vec3(0,120,-5),Tempest::Vec3(0,-1,0));
  require(!off.itemView().isEmpty() && !off.itemView().hasEffect(),"with [GAME] itemEffects 0 a shot arrow must have no effect");
  // one step of the flight: the fire arrow's effect goes with the bullet
  const auto shotFrom=fire.position();
  session->tick(10);
  Tempest::Vec3 flying;
  require(!fire.isFinished() && (fire.position()-shotFrom).length()>20.f && fire.itemView().effectPosition(flying) &&
          near(flying,fire.position(),.5f) && onMesh(fire.itemView()),"a flying fire arrow's effect must go with the bullet");
  // stuck in the ground, the switch still off: the effect at KEY_INVEST_3 on the fire arrow only; when it stuck
  size_t stuckIndex=0,burning=0;
  uint64_t stuckAt=0;
  const auto burningPins=[&] {
    burning=0;
    for(size_t i=stuckBefore;i<world->stuckArrowCount();++i)
      if(world->stuckArrowView(i)->hasEffect()) {
        stuckIndex=i;
        ++burning;
        }
    if(burning>0 && stuckAt==0)
      stuckAt=world->tickCount();
    return world->stuckArrowCount()>=stuckBefore+3;
    };
  require(tickUntil(burningPins,16,120),"the three arrows must stick in the ground, also the one shot with itemEffects 0");
  Settings::setI("GAME","itemEffects",1);
  // by its index: the vector of the stuck arrows may grow while the world ticks
  const auto stuckView=[&] { return world->stuckArrowView(stuckIndex); };
  const auto* stuck=stuckView();
  require(burning==1 && stuckAt!=0 && emitting(*stuck) && stuck->effectKey()==hit && onMesh(*stuck),
          "the fire arrow stuck in the ground must have its effect at KEY_INVEST_3 on its mesh, the others none");
  // 20 s of the game's time from its hit: the effect goes, the arrow stays
  const auto tickTo=[&](uint64_t at) {
    for(size_t i=0;i<400 && world->tickCount()<at;++i)
      session->tick(std::min<uint64_t>(100,at-world->tickCount()));
    return world->tickCount()==at;
    };
  require(tickTo(stuckAt+19999) && stuckView()->hasEffect(),"the stuck fire arrow must keep its effect for 20 s");
  require(tickTo(stuckAt+20000) && !stuckView()->hasEffect() && !stuckView()->isEmpty() &&
          world->stuckArrowCount()>=stuckBefore+3,"20 s after the hit the stuck fire arrow must have no effect, the arrow must stay");

  // on stone: the arrow glances off and flies on, at KEY_INVEST_3 since that first hit
  Tempest::Vec3 stone;
  require(findGround(true,stone),"stone ground must be found by Harad");
  auto& glancing=shoot(fireArrow,stone+Tempest::Vec3(0,120,0),Tempest::Vec3(0,-1,0));
  bool glanced=false;
  for(size_t i=0;i<100 && !glanced && !glancing.isFinished();++i) {
    session->tick(4);
    // a finished bullet is removed on the next tick only
    // thrown back: no longer straight down (the stone's n.y>=.7 turns a straight down flight to y>=-.02)
    const auto d=glancing.direction();
    glanced=!glancing.isFinished() && d.y>-.5f*d.length() &&
            glancing.itemView().level()==3 && glancing.itemView().effectKey()==hit && emitting(glancing.itemView());
    }
  require(glanced,"a fire arrow glancing off stone must fly on at KEY_INVEST_3");
  // flat stone by Harad, found from above as the ground: each point to `take` until it is done with one
  const auto onFlatStone=[&](const std::function<bool(const Tempest::Vec3&)>& take) {
    for(float r=150;r<=3000;r+=150)
      for(int a=0;a<12;++a) {
        const float an=float(a)*float(M_PI)/6.f;
        const auto p=npc->position()+Tempest::Vec3(std::cos(an)*r,0,std::sin(an)*r);
        const auto land=world->physic()->ray(p+Tempest::Vec3(0,300,0),p-Tempest::Vec3(0,700,0));
        if(land.hasCol && land.mat==asset::MaterialGroup::STONE && land.n.y>=.98f && take(land.v))
          return true;
        }
    return false;
    };
  // A touch of stone at the very start of a step is a glance like any other (DynamicWorld::moveBullet): four arrows
  // fall straight down onto one point of flat stone from 0.05 to 0.3 cm, so their first 4 ms step meets it at a
  // fraction of a few hundredths. Each finishes its flight on the stone, neither standing still unfinished nor
  // falling through it. Bullets do not collide with each other: shot at once.
  {
    struct Touch { float height; Bullet* b; Tempest::Vec3 last,end; size_t still=0,steps=0; bool done=false; };
    std::vector<Touch> touches;
    Tempest::Vec3 at;
    require(onFlatStone([&](const Tempest::Vec3& p) {
      at=p;
      for(float h:{.05f,.1f,.2f,.3f}) {
        auto& b=shoot(arrow,at+Tempest::Vec3(0,h,0),Tempest::Vec3(0,-1,0));
        touches.push_back({h,&b,b.position(),{}});
        }
      return true;
      }),"flat stone an arrow touches must be found by Harad");
    for(size_t i=0;i<3000;++i) {
      bool flying=false;
      for(auto& t:touches)
        flying|=!t.done;
      if(!flying)
        break;
      session->tick(4);
      for(auto& t:touches) {
        // a finished bullet is removed on the next tick: not looked at after
        if(t.done)
          continue;
        ++t.steps;
        if(t.steps==1) {
          // the first 4 ms step touches the stone: it glances up and flies on
          const auto msg="an arrow shot down onto stone from "+std::to_string(t.height)+" cm must glance up at its "
                         "first touch";
          require(!t.b->isFinished() && t.b->direction().y>0.f,msg.c_str());
          }
        if(t.b->isFinished()) {
          t.done=true;
          t.end=t.b->position();
          continue;
          }
        t.still=(t.b->position()-t.last).length()<1.e-4f ? t.still+1 : 0;
        t.last=t.b->position();
        if(t.still>=50) {
          const auto msg="an arrow shot down onto stone from "+std::to_string(t.height)+" cm hung at the stone: "+
                         std::to_string(t.still)+" steps still, unfinished after "+std::to_string(t.steps)+" steps";
          require(false,msg.c_str());
          }
        }
      }
    for(auto& t:touches) {
      const auto msg="an arrow shot down onto stone from "+std::to_string(t.height)+" cm hung at the stone: unfinished "
                     "after "+std::to_string(t.steps)+" steps";
      require(t.done,msg.c_str());
      const auto under=world->physic()->ray(t.end+Tempest::Vec3(0,5,0),t.end-Tempest::Vec3(0,20,0));
      std::printf("Arrow touch: from %.2f cm finished in %zu steps at %.1f %.1f %.1f on material %d\n",double(t.height),
                  t.steps,double(t.end.x),double(t.end.y),double(t.end.z),under.hasCol ? int(under.mat) : -1);
      const auto fell="an arrow shot down onto stone from "+std::to_string(t.height)+" cm fell through the stone: "
                      "stopped at y "+std::to_string(t.end.y)+", the stone at y "+std::to_string(at.y);
      require(t.end.y>=at.y-1.f && under.hasCol && under.mat==asset::MaterialGroup::STONE,fell.c_str());
      }
  }

  // Stopped on stone (its fourth glance, DynamicWorld::moveBullet): the tip 2 cm in it, the arrow turned into the
  // stone. Shot straight down onto flat stone it glances in place until it stops; if it stopped off the stone, the
  // next point is tried.
  {
    const bool stopped=onFlatStone([&](const Tempest::Vec3& at) {
      const auto before=world->stuckArrowCount();
      auto& b=shoot(arrow,at+Tempest::Vec3(0,120,0),Tempest::Vec3(0,-1,0));
      auto last=b.position();
      for(size_t i=0,still=0;i<3000 && still<50 && !b.isFinished();++i) {
        session->tick(4);
        still=(b.position()-last).length()<1.e-4f ? still+1 : 0;
        last=b.position();
        }
      require(b.isFinished(),"an arrow shot down onto flat stone from 120 cm hung at the stone");
      Tempest::Matrix4x4 m;
      if(world->stuckArrowCount()!=before+1 || !world->stuckArrowTransform(before,m))
        return false;
      const auto entry=b.impactPoint();
      const auto d=depthOf(m,world->stuckArrowView(before)->protoMesh(),entry);
      const auto under=world->physic()->ray(entry-d.axis*20.f,entry+d.axis*5.f);
      std::printf("Arrow depth: stopped on material %d %.0f cm from where it was shot, tip %.2f cm past the hit, %.2f cm "
                  "off its line, flight %.2f %.2f %.2f\n",int(under.mat),double((entry-at).length()),double(d.tip),
                  double(d.off),double(d.axis.x),double(d.axis.y),double(d.axis.z));
      if(!under.hasCol || under.mat!=asset::MaterialGroup::STONE)
        return false;
      // shot straight down, it came into the stone straight down: so it stands
      require(std::abs(d.tip-2.f)<=1.f && d.off<1.f && d.axis.y<-.9999f,
              "an arrow stopped on stone must have its tip 2 cm in it");
      return true;
      });
    require(stopped,"an arrow must stop on flat stone by Harad");
  }
  // in Harad: the effect goes with the bone
  const auto inBody=npc->stuckArrowCount();
  // along a line into one of his zones: through his box's centre an arrow may pass between an arm and the trunk, then
  // the zones do not tell where it entered and the tip is put at that centre. His pose where he stands now: far from
  // the hero it is not solved every tick, and he was moved here
  npc->updateTransform();
  const auto center=npc->collosionCenter();
  Tempest::Vec3 aim=center;
  bool inZone=false;
  for(float dy:{0.f,20.f,35.f,-15.f,10.f,50.f})
    for(float dz:{0.f,8.f,-8.f}) {
      float t=0;
      const auto from=center+Tempest::Vec3(-80,dy,dz);
      if(inZone || npc->rayHitZone(from,from+Tempest::Vec3(160,0,0),t)==HitZones::None)
        continue;
      aim=center+Tempest::Vec3(0,dy,dz);
      inZone=true;
      }
  require(inZone,"a line into Harad's zones must be found");
  auto& atHarad=shoot(fireArrow,aim+Tempest::Vec3(-80,0,0),Tempest::Vec3(1,0,0));
  require(tickUntil([&] { return npc->stuckArrowCount()>inBody; },4,240),"the fire arrow must stick in Harad");
  const auto& pinned=NpcDefinitionContract::stuckArrow(*npc,npc->stuckArrowCount()-1);
  Npc::ArrowPinProbe pin;
  require(npc->stuckArrowProbe(npc->stuckArrowCount()-1,pin) && emitting(pinned) && pinned.effectKey()==hit &&
          onMesh(pinned),"the fire arrow stuck in Harad must have its effect at KEY_INVEST_3 on its mesh");
  // the tip 12 cm past the entry into the hit zone, the tow outside
  {
    // where the shot's line enters his zones now, asked apart from the pin: the entry, the bone
    float t=0;
    size_t zoneBone=size_t(-1);
    const auto from=aim+Tempest::Vec3(-80,0,0);
    require(npc->rayHitZone(from,from+Tempest::Vec3(160,0,0),t,&zoneBone)!=HitZones::None,
            "the shot's line must still enter Harad's zones");
    const auto entry=from+Tempest::Vec3(t,0,0);
    const auto d=depthOf(pin.world,pinned.protoMesh(),entry);
    const auto flight=Tempest::Vec3::normalize(atHarad.direction());
    const auto towOut=Tempest::Vec3::dotProduct(meshPoint(pin.world,tow)-entry,d.axis);
    std::printf("Arrow depth: in Harad's bone %zu the tip %.2f cm past the entry %.0f cm from his centre, %.2f cm off its "
                "line, the tow %.2f cm before it\n",pin.bone,double(d.tip),double((entry-center).length()),double(d.off),
                double(-towOut));
    require((atHarad.impactPoint()-entry).length()<1.f && pin.bone==zoneBone && std::abs(d.tip-12.f)<=1.f && d.off<1.f &&
            Tempest::Vec3::dotProduct(d.axis,flight)>.9999f && towOut<0.f,
            "an arrow in a body must have its tip 12 cm past the entry, the tow outside");
  }
  // the pose, the arrow and its effect synced where Harad stands now, then he moves
  npc->updateTransform();
  Tempest::Vec3 at,after;
  pinned.effectPosition(at);
  const auto bone=NpcDefinitionContract::pinAt(*npc,npc->stuckArrowCount()-1);
  npc->setPosition(npc->position()+Tempest::Vec3(0,0,300));
  npc->updateTransform();
  const auto moved=NpcDefinitionContract::pinAt(*npc,npc->stuckArrowCount()-1)-bone;
  std::printf("Arrow effects: the fire arrow in Harad's bone %zu moved %.1f cm with it (bone %.1f cm)\n",pin.bone,
              double((pinned.effectPosition(after) ? after-at : Tempest::Vec3()).length()),double(moved.length()));
  require(pinned.effectPosition(after) && moved.length()>250.f && near(after-at,moved,2.f) && onMesh(pinned),
          "the fire arrow's effect in Harad must follow its bone");
  // as he animates: the effect where its bone and the hit's offset put the arrow
  const auto pinIndex=npc->stuckArrowCount()-1;
  for(int i=0;i<10;++i)
    session->tick(16);
  require(pinned.effectPosition(after) && near(after,NpcDefinitionContract::pinAt(*npc,pinIndex),.5f) && onMesh(pinned),
          "the fire arrow's effect in Harad must be at its bone times the hit's offset");

  // saved: the stuck arrows in main's layout (world/world.cpp saveStuckArrows, schema 2); loaded: no effects
  const auto worldPins=world->stuckArrowCount(),bodyPins=npc->stuckArrowCount();
  const auto sym=npc->instanceSymbol();
  // where they are: loaded, they stay there, not sunk again
  std::vector<Tempest::Matrix4x4> savedWorld(worldPins),savedLocal(bodyPins);
  std::vector<size_t> savedBone(bodyPins);
  for(size_t i=0;i<worldPins;++i)
    require(world->stuckArrowTransform(i,savedWorld[i]),"a stuck arrow must have its matrix");
  for(size_t i=0;i<bodyPins;++i) {
    Npc::ArrowPinProbe p;
    require(npc->stuckArrowProbe(i,p),"an arrow in a body must have its matrix");
    savedLocal[i]=p.local;
    savedBone[i]=p.bone;
    }
  std::vector<uint8_t> bytes;
  {
    Tempest::MemWriter writer(bytes);
    Serialize out(writer);
    Tempest::Pixmap preview(1,1,Tempest::TextureFormat::RGBA8);
    std::memset(preview.data(),0,4);
    session->save(out,"arrow effects",preview);
  }
  {
    Tempest::MemReader reader(bytes);
    Serialize in(reader);
    require(in.setEntry("worlds/NEWWORLD.ZEN/stuck-arrows"),"the stuck arrows must be written to /stuck-arrows");
    uint32_t magic=0,count=0;
    uint16_t schema=0;
    uint64_t nextId=0;
    std::string name;
    in.read(magic,schema,name,count,nextId);
    require(magic==0x31524157 && schema==2 && name=="NEWWORLD.ZEN" && count==worldPins,
            "the stuck arrows must be saved with main's header (magic, schema 2)");
    for(uint32_t i=0;i<count;++i) {
      std::string visual,culprit,surface;
      Tempest::Matrix4x4 local;
      bool attached=false;
      in.read(visual,local,attached);
      if(attached) {
        uint32_t zenId=0;
        std::string movable;
        Tempest::Matrix4x4 origin;
        in.read(zenId,movable,origin);
        }
      uint64_t traceId=0,appeared=0;
      Tempest::Vec3 sight;
      in.read(traceId,appeared,culprit,surface,sight);
      require(!visual.empty() && traceId>0 && traceId<nextId,"a saved stuck arrow must read in main's layout");
      // an arrow in no movable: its matrix in the world, as main writes it
      require(attached || PhysicsSave::sameTransform(local,savedWorld[i]),"a saved stuck arrow must carry its drawn matrix");
      }
  }
  gothic.clearGame().reset();
  {
    Tempest::MemReader reader(bytes);
    Serialize in(reader);
    gothic.setGame(std::make_unique<GameSession>(in));
  }
  world=gothic.world();
  session=gothic.gameSession();
  auto* loaded=activityNpc(*world,"VLK_412_HARAD");
  require(loaded!=nullptr && loaded->instanceSymbol()==sym && world->stuckArrowCount()==worldPins &&
          loaded->stuckArrowCount()==bodyPins,"the stuck arrows must load back");
  for(size_t i=0;i<world->stuckArrowCount();++i)
    require(!world->stuckArrowView(i)->isEmpty() && !world->stuckArrowView(i)->hasEffect(),
            "a loaded stuck arrow must have no effect");
  for(size_t i=0;i<loaded->stuckArrowCount();++i)
    require(!NpcDefinitionContract::stuckArrow(*loaded,i).isEmpty() && !NpcDefinitionContract::stuckArrow(*loaded,i).hasEffect(),
            "a loaded arrow in a body must have no effect");
  for(size_t i=0;i<worldPins;++i) {
    Tempest::Matrix4x4 m;
    require(world->stuckArrowTransform(i,m) && PhysicsSave::sameTransform(m,savedWorld[i]),"a loaded stuck arrow must be where it was saved");
    }
  for(size_t i=0;i<bodyPins;++i) {
    Npc::ArrowPinProbe p;
    require(loaded->stuckArrowProbe(i,p) && p.bone==savedBone[i] && PhysicsSave::sameTransform(p.local,savedLocal[i]),
            "a loaded arrow in a body must keep its bone and its offset from it");
    }
  // an arrow in a body written by main (APR1 version 1) loads, without effect, at the offset it was written with
  {
    Npc::ArrowPinProbe p;
    require(NpcDefinitionContract::loadMainArrow(*loaded,*world,0) && loaded->stuckArrowCount()==1 &&
            !NpcDefinitionContract::stuckArrow(*loaded,0).isEmpty() && !NpcDefinitionContract::stuckArrow(*loaded,0).hasEffect() &&
            loaded->stuckArrowProbe(0,p) && p.bone==savedBone[0] && PhysicsSave::sameTransform(p.local,savedLocal[0]),
            "an arrow in a body saved by main must load without effect on its bone at its offset");
  }

  // stuck in something that moves: the effect goes with the arrow
  {
    auto m=Tempest::Matrix4x4::mkIdentity();
    m.translate(loaded->position()+Tempest::Vec3(0,5000,0));
    auto* item=world->addItemDyn(fireArrow,m,size_t(-1));
    require(item!=nullptr,"the arrow item must be made");
    ItemView view(*world,item->handle());
    world->removeItem(*item);
    view.setLevel(3);
    auto movable=std::make_shared<DynamicWorld::MovableVobPose>();
    movable->transform=Tempest::Matrix4x4::mkIdentity();
    movable->transform.translate(loaded->position()+Tempest::Vec3(0,100,0));
    auto pinMatrix=movable->transform;
    pinMatrix.translate(0,20,0);
    require(world->stickArrow(std::move(view),pinMatrix,"ITRW_ADDON_FIREARROW",movable),"the arrow must stick in the movable");
    const auto* moving=world->stuckArrowView(world->stuckArrowCount()-1);
    Tempest::Vec3 before,later;
    require(moving->effectPosition(before) && emitting(*moving),"the arrow in the movable must have its effect");
    movable->transform.translate(0,0,150);
    session->tick(16);
    moving=world->stuckArrowView(world->stuckArrowCount()-1);
    require(moving->effectPosition(later) && near(later-before,Tempest::Vec3(0,0,150),.5f) && onMesh(*moving),
            "the effect of an arrow in a movable must go with it");
    movable.reset();
    session->tick(16);
  }

  // `item put` of a symbol that is no C_ITEM instance: «no item», no VM exception; its answer in a bench.log of its own
  {
    const auto dir=std::filesystem::temp_directory_path()/("item-put-"+std::to_string(getpid()));
    std::filesystem::create_directories(dir);
    TestBench::retarget(dir.string());
    for(const char* notItem:{"C_ITEM","VLK_412_HARAD","ITRW_ARROW_NOT_A_SYMBOL"})
      require(testbench_detail::itemEffectsCommand({"item","put",notItem,"100","0"},*world,*loaded),
              "item put must take the command");
    TestBench::flushOutput();
    std::ifstream log(dir/"bench.log");
    const std::string text((std::istreambuf_iterator<char>(log)),std::istreambuf_iterator<char>());
    const auto* out=std::getenv("OPENGOTHIC_BENCH_OUT");
    TestBench::retarget(out!=nullptr ? out : ".");
    std::filesystem::remove_all(dir);
    require(text=="item put: no item C_ITEM\nitem put: no item VLK_412_HARAD\nitem put: no item ITRW_ARROW_NOT_A_SYMBOL\n",
            "item put must answer «no item» for a symbol that is no C_ITEM instance");
  }
  Settings::setI("GAME","itemEffects",switchBefore);
  Settings::setI("GAME","arrow_sticking",stickingBefore);
  Settings::inst().refreshPhysicsFlags();
  std::printf("Arrow effects: shot KEY_INVEST_2, stuck KEY_INVEST_3 in the ground and in a bone, gone in 20 s, arrow stays; "
              "ITRW_ARROW and itemEffects 0 none; %zu+%zu arrows saved in main's layout, loaded without effects\n",
              worldPins,bodyPins);
  }

void compareLegacyPlacedStartup(Gothic& gothic) {
  pointActivitiesInBothModes(gothic);
  dragonIslandPlacement(gothic);
  const auto placementWorldDifferences=placementFirstWorld(gothic);
  require(gothic.player()!=nullptr,"bench helper fixture has a player");
  routineTables(*gothic.player());
  npcDefinitions(*gothic.world());
  placementDefinitions(*gothic.world());
  if(!placementWorldDifferences.empty())
    throw std::runtime_error("first-world placement differs: "+std::to_string(placementWorldDifferences.size())+" differences; "+placementWorldDifferences.front());
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
  talkPhases(gothic);
  }

void moverChildSave(bool nativeOnly) {
  itemEffectsDefault();
  const auto* game=std::getenv("GOTHIC_DIR");
  require(game!=nullptr && *game!='\0',"GOTHIC_DIR must point to prepared Gothic II settings");
  const char* args[]={"gothic-placed-items-contract","-g",game};
  CommandLine commandLine(3,args);
  Resources resources;
  Gothic gothic;
  gothic.isNpcInDialogFn=[](const Npc*) { return false; };
  gothic.setupGlobalScripts();
  GameMusic music;
  if(nativeOnly)
    startNativePlacementWorld(gothic);
  else
    compareLegacyPlacedStartup(gothic);
  const auto& child=placedMoverChild(*gothic.world());
  gothic.world()->script().getVm().global_item()->set_instance(
    std::static_pointer_cast<daedalus::IItem>(child.item->handlePtr()));
  const auto catalogueSize=gothic.world()->placedItemCensus().size();
  const auto itemCount=placedWorldItemCount(*gothic.world());
  const auto instance=child.instance;
  const auto verdict=child.validation.disposition;
  auto savedPose=child.origin;
  savedPose.translate(25,0,0);
  child.item->setObjMatrix(savedPose);

  const auto requireStory=storyAndPointRollsBeforeSave(*gothic.world(),nativeOnly);
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
  requireStory(*gothic.world());
  const auto& restored=placedMoverChild(*gothic.world());
  require(gothic.world()->script().getVm().global_item()->get_instance()==
          std::static_pointer_cast<daedalus::IItem>(restored.item->handlePtr()),
          "the saved global Item reference must resolve to the restored world Item");
  require(gothic.world()->placedItemCensus().size()==catalogueSize && placedWorldItemCount(*gothic.world())==itemCount,
          "loading a mover-child identity must preserve the catalogue and avoid duplicate Items");
  require(restored.instance==instance && restored.validation.disposition==verdict &&
          restored.item->placedItemZenId()==2839,
          "the saved mover-child identity must restore its original census verdict");
  for(size_t i=0;i<16;++i)
    require(restored.item->localTransform().data()[i]==savedPose.data()[i],
            "a moved mover-child Item must recover its exact saved pose through /placed-items");
  std::printf("Mover child 2838 -> 2839: catalogue %zu, items %u, identity and saved pose restored\n",catalogueSize,itemCount);
  if(nativeOnly) {
    std::puts("Native stock contract: prepared Lares exchange, story state and mover-child pose restored");
    return;
    }
  {
    // The record phases against the VM: the polled Loop of OPENGOTHIC_ACTIVITY_EVENTS=0, also after its load.
    const auto* configured=std::getenv("OPENGOTHIC_ACTIVITY_EVENTS");
    const std::optional<std::string> previous=configured ? std::optional<std::string>(configured) : std::nullopt;
    setenv("OPENGOTHIC_ACTIVITY_EVENTS","0",1);
    NpcDefinitionContract::activityEventsOn(gothic.world()->script())=false;
    // Last: it replaces the session with its own load.
    activityDefinitionsWorld(gothic);
    if(previous) setenv("OPENGOTHIC_ACTIVITY_EVENTS",previous->c_str(),1); else unsetenv("OPENGOTHIC_ACTIVITY_EVENTS");
  }
  // It replaces the session with its own load too.
  activityEventsWorld(gothic);
  guardActivityWorld(gothic);
  itemEffects(*gothic.world());
  // It replaces the session with its own load.
  arrowEffects(gothic);
  // It moves Harad between his forge and his anvil.
  animationEffects(gothic);
  guardCampfireChange(gothic);
  gothic.clearGame().reset();
  remainingRoutineWitnesses(gothic);
  }

}

int main(int argc, const char** argv) {
  try {
    const auto* native=std::getenv("OPENGOTHIC_CONTRACT_NATIVE_STOCK");
    const bool nativeOnly=native && std::strcmp(native,"1")==0;
    if(runStoryContract(argc,argv))
      return EXIT_SUCCESS;
    census();
    rootedAndStack();
    censusCache();
    slopes();
    penetrationLimit();
    groundBelowSupport();
    asymmetricLowestPoint();
    irregularSupport();
    if(nativeOnly) {
      moverChildSave(true);
      } else {
      npcLiveSynthetic(npcDefinitions,npcDefinitionsDisabled,placementDefinitions,placementDefinitionsDisabled,
                       activityDefinitionsSynthetic);
      const auto* noGameData=std::getenv("OPENGOTHIC_CONTRACT_NO_GAME_DATA");
      if(noGameData && std::strcmp(noGameData,"1")==0)
        std::puts("SKIP real game corpus and mover-child save: OPENGOTHIC_CONTRACT_NO_GAME_DATA=1");
      else
        moverChildSave(false);
      }
    std::puts("Placed item census contracts passed");
    return EXIT_SUCCESS;
    }
  catch(const std::exception& error) {
    std::fprintf(stderr,"FAIL: placed item census contract: %s\n",error.what());
    return EXIT_FAILURE;
    }
  }
