#include "physics/dynamicworld.h"
#include "physics/placedcensuscache.h"
#include "graphics/bounds.h"
#include "graphics/mesh/protomesh.h"
#include "graphics/mesh/skeleton.h"
#include "graphics/mesh/submesh/packedmesh.h"
#include "resources.h"
#include "commandline.h"
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
#include "npcdaymeasurement.h"
#include "world/npcactivityprobe.h"
#include "world/objects/interactive.h"
#include "world/waypoint.h"
#include "game/gamesession.h"
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
#include "world/bullet.h"

#include <rapidjson/document.h>

#include <Tempest/MemReader>
#include <Tempest/MemWriter>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
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

void npcLiveSynthetic(void (*compare)(World&), void (*disabled)(World&),
                      void (*placement)(World&), void (*placementDisabled)(World&), void (*activities)(World&));

// Test-only access keeps the production creation entry point unchanged.
struct NpcDefinitionContract {
  static auto& definitions(GameScript& script) { return script.npcDefinitions; }
  static auto& placements(GameScript& script) { return script.placementDefinitions; }
  static auto& activities(GameScript& script) { return script.activityDefinitions; }
  static auto& aiState(Npc& npc) { return npc.aiState; }
  static auto& previousState(Npc& npc) { return npc.aiPrevState; }
  // The engine's own routine step, without waiting for the perception period.
  static void tickRoutine(Npc& npc) { npc.aiState.loopNextTime=0; npc.tickRoutine(); }
  // The engine's next queued action, as the NPC's tick takes it.
  static void nextAiAction(Npc& npc) { npc.nextAiAction(npc.aiQueue,0); }
  static const AiQueue& queue(const Npc& npc) { return npc.aiQueue; }
  static const auto& items(GameScript& script) { return script.itemDefinitions; }
  static const auto& routines(GameScript& script) { return script.routineDefinitions; }
  static auto& random(GameScript& script) { return script.randGen; }
  // the views of items (graphics/itemview): the sword and the bow on a body, a lying item's
  static const ItemView& sword(const Npc& npc) { return npc.visual.sword.view; }
  static const ItemView& bow(const Npc& npc) { return npc.visual.bow.view; }
  static const ItemView* lying(const Item& item) {
    const auto& v=item.visual;
    return v.type==ObjVisual::M_Mesh && v.mesh.worldItem ? &v.mesh.item : nullptr;
    }
  static StreamedVisual& visual(Item& item) { return item.visual; }
  static const ItemView& stuckArrow(const Npc& npc, size_t i) { return npc.visual.stuckArrows[i].view; }
  // where a stuck arrow is by its own bone and offset: the bone's rendered matrix times the pin's local one
  static Tempest::Vec3 pinAt(const Npc& npc, size_t i) {
    const auto& pin=npc.visual.stuckArrows[i];
    const auto m=npc.visual.renderedBone(pin.bone)*pin.local;
    return {m.at(3,0),m.at(3,1),m.at(3,2)};
    }
  // a stuck arrow's record in main's own bytes (mdlvisual_quiver.cpp, APR1 version 1), read by the current loader
  static bool loadMainArrow(Npc& npc, World& world, size_t i) {
    const auto& pin=npc.visual.stuckArrows[i];
    const std::string skeleton(npc.visual.skeleton->name()),bone(npc.visual.skeleton->nodes[pin.bone].name),visual=pin.visual;
    const auto local=pin.local;
    std::vector<uint8_t> raw;
    {
      Tempest::MemWriter writer(raw);
      Serialize out(writer);
      out.setEntry("contract/arrows");
      const auto name=[&](const std::string& v) {
        const uint32_t n=uint32_t(v.size());
        out.writeBytes(&n,sizeof(n));
        out.writeBytes(v.data(),v.size());
        };
      const uint32_t magic=0x31525041,count=1;
      const uint16_t version=1;
      out.writeBytes(&magic,sizeof(magic));
      out.writeBytes(&version,sizeof(version));
      name(skeleton);
      out.writeBytes(&count,sizeof(count));
      name(visual);
      name(bone);
      out.writeBytes(&local,sizeof(local));
    }
    Tempest::MemReader reader(raw);
    Serialize in(reader);
    return in.setEntry("contract/arrows") && npc.visual.loadStuckArrows(in,world);
    }
  static auto& isolatedRandom(GameScript& script) { return script.isolatedRandom; }
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

struct HelperLocal {
  daedalus::DaedalusSymbol* symbol;
  uint16_t index;
  std::variant<int32_t,float,std::string,std::shared_ptr<daedalus::DaedalusInstance>> value;
  };
std::vector<HelperLocal> helperLocals(daedalus::DaedalusVm& vm) {
  using Type=daedalus::DaedalusDataType;
  std::vector<HelperLocal> values;
  for(const auto& entry:vm.symbols()) {
    const auto& name=entry.name();
    if(entry.is_member() || entry.is_const() ||
       !(name.starts_with("B_SETFIGHTSKILLS.") || name.starts_with("B_ADDFIGHTSKILL.") ||
         name.starts_with("B_RAISEFIGHTTALENT.") || name.starts_with("B_RAISEREALFIGHTTALENTPERCENT.") ||
         name.starts_with("B_SETNPCVISUAL.") || name.starts_with("B_SETATTRIBUTESTOCHAPTER.") ||
         name.starts_with("B_CREATEAMBIENTINV.") || name.starts_with("B_CREATEAMBIENTINV_"))) continue;
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
std::map<std::string,std::string> helperLocalText(daedalus::DaedalusVm& vm,const Npc& npc) {
  std::map<std::string,std::string> result;
  for(const auto& entry:helperLocals(vm))
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
      const auto localBaseline=helperLocals(vm);
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
        const auto expectedLocals=helperLocalText(vm,*expected);
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
        const auto actualLocals=helperLocalText(vm,*actual);
        if(helpersOnly)
          for(const auto& [local,value]:expectedLocals) difference(name,"helper local "+local,value,actualLocals.at(local));
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

std::vector<std::string> placementFirstWorld(Gothic& gothic) {
  const auto* configured=std::getenv("OPENGOTHIC_PLACEMENT_RECORDS");
  const bool configuredBefore=configured!=nullptr;
  const std::string previous=configured ? configured : "";
  auto state=[&] {
    auto& world=*gothic.world();
    auto result=placementSymbols(world.script(),false);
    result["NPC count"]=std::to_string(world.npcCount());
    uint32_t items=0;
    while(world.itmById(items)) ++items;
    result["item count"]=std::to_string(items);
    result["random generator"]=world.script().randomState(nullptr);
    auto entries=archiveEntries(placementSnapshot(world,false));
    const auto item=itemState(world.script());
    for(const auto& [name,payload]:item) entries.emplace("global-ITEM/"+name,payload);
    for(const auto& [name,payload]:entries)
      result["save/"+name]=std::to_string(payload.size())+":"+
          Sha256::hash({reinterpret_cast<const char*>(payload.data()),payload.size()});
    return result;
    };
  std::vector<std::string> differences;
  try {
    setenv("OPENGOTHIC_PLACEMENT_RECORDS","0",1);
    gothic.setGame(std::make_unique<GameSession>("NEWWORLD.ZEN"));
    require(NpcDefinitionContract::placements(gothic.world()->script()).placements.empty(),
            "disabled first-world placement must have no game definitions");
    const auto expected=state();
    const auto expectedLocals=placementSymbols(gothic.world()->script(),true);
    gothic.clearGame().reset();
    unsetenv("OPENGOTHIC_PLACEMENT_RECORDS");
    gothic.setGame(std::make_unique<GameSession>("NEWWORLD.ZEN"));
    require(!NpcDefinitionContract::placements(gothic.world()->script()).placements.empty(),
            "default first-world placement must load game definitions");
    const auto actual=state();
    for(const auto& [field,value]:expected) {
      const auto found=actual.find(field);
      if(found==actual.end() || value!=found->second)
        differences.push_back(field+" expected="+value+" actual="+(found==actual.end() ? "<absent>" : found->second));
      }
    for(const auto& [field,value]:actual)
      if(!expected.contains(field)) differences.push_back(field+" expected=<absent> actual="+value);
    const auto actualLocals=placementSymbols(gothic.world()->script(),true);
    for(const auto& [name,value]:expectedLocals) {
      const auto found=actualLocals.find(name);
      if(found==actualLocals.end() || value!=found->second)
        differences.push_back("VM local "+name+" expected="+value+" actual="+(found==actualLocals.end() ? "<absent>" : found->second));
      }
    for(const auto& [name,value]:actualLocals)
      if(!expectedLocals.contains(name)) differences.push_back("VM local "+name+" expected=<absent> actual="+value);
    const auto* startup=gothic.world()->script().getVm().find_symbol_by_name("STARTUP_NEWWORLD");
    require(startup && NpcDefinitionContract::placements(gothic.world()->script()).placements.contains(startup->index()),
            "default NEWWORLD startup must replay a captured placement record");
    std::printf("First-world NEWWORLD placement: %s NPCs, %s items; %zu saved/global state entries, %zu differences; STARTUP_NEWWORLD=%s\n",
                actual.at("NPC count").c_str(),actual.at("item count").c_str(),expected.size(),differences.size(),
                startup && NpcDefinitionContract::placements(gothic.world()->script()).placements.contains(startup->index()) ? "record" : "VM");
    }
  catch(...) {
    if(configuredBefore) setenv("OPENGOTHIC_PLACEMENT_RECORDS",previous.c_str(),1);
    else unsetenv("OPENGOTHIC_PLACEMENT_RECORDS");
    throw;
    }
  if(configuredBefore) setenv("OPENGOTHIC_PLACEMENT_RECORDS",previous.c_str(),1);
  else unsetenv("OPENGOTHIC_PLACEMENT_RECORDS");
  for(const auto& value:differences) std::fprintf(stderr,"[placement-world-difference] %s\n",value.c_str());
  return differences;
  }


// D4.1: a routine activity from its record against the VM, at the same positions. The stream
// interleaves the engine calls (VM externals; the template's calls of a record phase, whose
// perception helper still runs in the VM), the queued commands and the phase results.
class ActivityStream final : public NpcActivityProbe {
  public:
    ActivityStream(World& world,Npc& npc):npc(npc),vm(world.script().getVm()) {
      previous=NpcActivityProbe::instance;
      NpcActivityProbe::instance=this;
      vm.observe_externals([this](const daedalus::DaedalusSymbol& symbol) { external(symbol); });
      vm.observe_instructions([this](const daedalus::DaedalusInstruction&) { finish(); });
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

// The NPC's queue as text, front first, without taking it.
std::vector<std::string> activityQueue(const Npc& npc) {
  auto queue=NpcDefinitionContract::queue(npc);
  std::vector<std::string> actions;
  while(queue.size()>0) {
    const auto a=queue.pop();
    actions.push_back(std::string(AiQueue::actionName(uint32_t(a.act)))+" "+std::to_string(a.i0)+" "+std::to_string(a.i1)+" "+
                      a.s0+" "+(a.point ? a.point->name : "")+" "+(a.target ? std::string(a.target->displayName()) : "")+" "+
                      std::to_string(a.func.ptr));
    }
  return actions;
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

// The switch and the accepted list through the loader the game calls.
void activitySwitch(GameScript& script,const std::vector<std::string>& accepted,const char* other) {
  const auto base=baseProgramIndex().dir.parent_path();
  std::string sha;
  loadBaseProgram(Gothic::inst().defaultGameDatFile(),Settings::getI("GAME","language"),&sha);
  const auto* configured=std::getenv("OPENGOTHIC_ACTIVITY_RECORDS");
  const std::optional<std::string> previous=configured ? std::optional<std::string>(configured) : std::nullopt;
  struct Restore {
    const std::optional<std::string>& previous;
    ~Restore() { if(previous) setenv("OPENGOTHIC_ACTIVITY_RECORDS",previous->c_str(),1); else unsetenv("OPENGOTHIC_ACTIVITY_RECORDS"); }
    } restore{previous};
  // The phase lookup the game's dispatcher uses, for all three phases.
  auto runs=[&](const ActivityDefinitionRuntime::Definitions& definitions,const char* state) {
    if(state==nullptr) return false;
    const auto& st=script.aiState(script.findSymbolIndex(state));
    return ActivityDefinitionRuntime::find(definitions,uint32_t(st.funcIni))!=nullptr &&
           ActivityDefinitionRuntime::find(definitions,uint32_t(st.funcLoop))!=nullptr &&
           ActivityDefinitionRuntime::find(definitions,uint32_t(st.funcEnd))!=nullptr;
    };
  auto loaded=[&](const char* value) {
    if(value) setenv("OPENGOTHIC_ACTIVITY_RECORDS",value,1); else unsetenv("OPENGOTHIC_ACTIVITY_RECORDS");
    const auto definitions=ActivityDefinitionRuntime::load(base,script.getVm(),sha);
    size_t acceptedRuns=0;
    for(const auto& state:accepted)
      acceptedRuns+=runs(definitions,state.c_str()) ? 1 : 0;
    return std::tuple{acceptedRuns,runs(definitions,other),definitions.phases.size()};
    };
  const auto all=accepted.size(),captured=all+(other!=nullptr ? 1 : 0);
  require(loaded(nullptr)==std::tuple{all,false,3*all},"by default every accepted activity, and only those, runs from its record");
  require(loaded("1")==std::tuple{all,false,3*all},"another switch value keeps the accepted list");
  require(loaded("0")==std::tuple{size_t(0),false,size_t(0)},"OPENGOTHIC_ACTIVITY_RECORDS=0 must run every activity in the VM");
  require(loaded("all")==std::tuple{all,other!=nullptr,3*captured},
          "OPENGOTHIC_ACTIVITY_RECORDS=all must run every captured activity from its record");
  std::printf("Activity switch: default and 1 run the %zu accepted states only, 0 none, all every captured state\n",all);
  }

void activityDefinitionsSynthetic(World& world) {
  auto& script=world.script();
  activitySwitch(script,{"ZS_SMITH_ANVIL","ZS_SMOKE_WATERPIPE"},"ZS_ROAST_SCAVENGER");
  // The game's accepted list itself, without game data.
  for(const char* state:{"ZS_SMITH_ANVIL","ZS_SMITH_COOL","ZS_SMITH_FIRE","ZS_SMITH_SHARP","ZS_READ_BOOKSTAND",
                         "ZS_STOMP_HERB","ZS_SMOKE_WATERPIPE"})
    require(ActivityDefinitionRuntime::accepted(state),(std::string("the game must accept ")+state).c_str());
  require(!ActivityDefinitionRuntime::accepted("ZS_ROAST_SCAVENGER"),"ZS_ROAST_SCAVENGER has no day parity and stays in the VM");
  auto* npc=world.addNpc(script.findSymbolIndex("NPC_NO_ID"),Tempest::Vec3{});
  require(npc!=nullptr,"activity NPC must exist");
  const auto anvil=activityPhases(script,"ZS_SMITH_ANVIL"),roast=activityPhases(script,"ZS_ROAST_SCAVENGER");
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
  // The outside state stays in the VM; the counters see who ran each phase.
  const auto directory=std::filesystem::temp_directory_path()/("gothic-activity-day-"+
      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(directory);
  std::string error;
  if(!NpcDayMeasurement::start(world,directory.string(),0,false,{},error)) throw std::runtime_error(error);
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
    require(phase("ZS_ROAST_SCAVENGER",name,"engine")==1 && phase("ZS_ROAST_SCAVENGER",name,"records")==0,
            "the outside phase must be counted once, from the VM");
    }
  require(phase("ZS_SMITH_ANVIL","init","nested")==1 && phase("ZS_SMITH_ANVIL","loop","nested")==0 &&
          phase("ZS_SMITH_ANVIL","end","nested")==0,"a record phase runs only its deferred perception helper in the VM");
  require(phase("ZS_ROAST_SCAVENGER","init","nested")==3 && phase("ZS_ROAST_SCAVENGER","loop","nested")==1,
          "the VM phase runs its helpers");
  // The nine instructions of the fixture's Perception_Set_Normal stay counted in the record's Init.
  require(phase("ZS_SMITH_ANVIL","init","instructions")==9 && phase("ZS_SMITH_ANVIL","loop","instructions")==0 &&
          phase("ZS_SMITH_ANVIL","end","instructions")==0 && phase("ZS_ROAST_SCAVENGER","init","instructions")>9,
          "a record phase counts only the instructions of its deferred helper");
  world.removeNpc(*npc);
  std::printf("Activity counters: ZS_SMITH_ANVIL 3 phases from the record, ZS_ROAST_SCAVENGER 3 from the VM; %zu equal events\n",events);
  }

Npc* activityNpc(World& world,const char* name) {
  const auto symbol=world.script().findSymbolIndex(name);
  for(uint32_t i=0;i<world.npcCount();++i)
    if(world.npcById(i)->instanceSymbol()==symbol)
      return world.npcById(i);
  return nullptr;
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

const WayPoint* activityRow(Npc& npc,size_t init,std::string& name,gtime* start=nullptr) {
  const WayPoint* point=nullptr;
  npc.forEachRoutine([&](const Npc::RoutineInfo& row) {
    if(point==nullptr && row.state.ptr==init && row.waypoint!=nullptr) {
      point=row.waypoint; name=std::string(row.point);
      if(start) *start=row.start;
      }
    });
  return point;
  }

bool activityCandidate(Npc& npc) { return !npc.isPlayer() && npc.isHuman() && !npc.isDead(); }

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
  activitySwitch(script,accepted,"ZS_ROAST_SCAVENGER");
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
  require(NpcDefinitionContract::activities(script).phases.size()==3*activities.size(),
          "the game must load exactly the accepted activity records");
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
  const auto findGround=[&](bool stone,Tempest::Vec3& out) {
    for(float r=150;r<=1500;r+=150)
      for(int a=0;a<12;++a) {
        const float an=float(a)*float(M_PI)/6.f;
        const auto p=npc->position()+Tempest::Vec3(std::cos(an)*r,0,std::sin(an)*r);
        const auto land=world->physic()->ray(p+Tempest::Vec3(0,300,0),p-Tempest::Vec3(0,700,0));
        const bool isStone=land.mat==asset::MaterialGroup::STONE;
        if(!land.hasCol || land.n.y<.7f || isStone!=stone || land.mat==asset::MaterialGroup::METAL ||
           land.mat==asset::MaterialGroup::UNDEFINED || land.mat==asset::MaterialGroup::NONE)
          continue;
        out=land.v;
        std::printf("Arrow effects: ground of material %d %.0f cm from Harad\n",int(land.mat),double(r));
        return true;
        }
    return false;
    };
  Tempest::Vec3 ground;
  require(findGround(false,ground),"ground an arrow sticks in must be found by Harad");
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
    glanced=!glancing.isFinished() && glancing.itemView().level()==3 && glancing.itemView().effectKey()==hit &&
            emitting(glancing.itemView());
    }
  require(glanced,"a fire arrow glancing off stone must fly on at KEY_INVEST_3");

  // in Harad: the effect goes with the bone
  const auto inBody=npc->stuckArrowCount();
  const auto center=npc->collosionCenter();
  shoot(fireArrow,center+Tempest::Vec3(-80,0,0),Tempest::Vec3(1,0,0));
  require(tickUntil([&] { return npc->stuckArrowCount()>inBody; },16,60),"the fire arrow must stick in Harad");
  const auto& pinned=NpcDefinitionContract::stuckArrow(*npc,npc->stuckArrowCount()-1);
  Npc::ArrowPinProbe pin;
  require(npc->stuckArrowProbe(npc->stuckArrowCount()-1,pin) && emitting(pinned) && pinned.effectKey()==hit &&
          onMesh(pinned),"the fire arrow stuck in Harad must have its effect at KEY_INVEST_3 on its mesh");
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
  // an arrow in a body written by main (APR1 version 1) loads, without effect
  require(NpcDefinitionContract::loadMainArrow(*loaded,*world,0) && loaded->stuckArrowCount()==1 &&
          !NpcDefinitionContract::stuckArrow(*loaded,0).isEmpty() && !NpcDefinitionContract::stuckArrow(*loaded,0).hasEffect(),
          "an arrow in a body saved by main must load without effect");

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

// [GAME] itemEffects read the working way (Settings::getI, ItemView::enabled) when neither Gothic.ini has it: 1, as Gothic
void itemEffectsDefault() {
  require(!Settings::exists(),"the default of itemEffects needs Settings of its own");
  const auto dir=std::filesystem::temp_directory_path()/("item-effects-ini-"+std::to_string(getpid()));
  Settings settings;
  settings.load((dir/"SystemPack.ini").u16string(),(dir/"Gothic.ini").u16string());
  const auto* env=std::getenv("OPENGOTHIC_ITEM_EFFECTS");
  require(!Settings::has("GAME","itemEffects") && Settings::getI("GAME","itemEffects")==1 &&
          (ItemView::enabled() || (env!=nullptr && std::string_view(env)=="0")),
          "[GAME] itemEffects must be 1 by default, as in Gothic");
  }

void moverChildSave() {
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
  // Last: it replaces the session with its own load.
  activityDefinitionsWorld(gothic);
  itemEffects(*gothic.world());
  // Last: it replaces the session with its own load.
  arrowEffects(gothic);
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
    npcLiveSynthetic(npcDefinitions,npcDefinitionsDisabled,placementDefinitions,placementDefinitionsDisabled,
                     activityDefinitionsSynthetic);
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
