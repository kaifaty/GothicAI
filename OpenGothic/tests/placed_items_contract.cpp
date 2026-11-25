#include "physics/dynamicworld.h"
#include "physics/placedcensuscache.h"
#include "graphics/bounds.h"
#include "graphics/itemview.h"
#include "graphics/mesh/protomesh.h"
#include "graphics/mesh/skeleton.h"
#include "graphics/mesh/submesh/packedmesh.h"
#include "graphics/pfx/particlefx.h"
#include "graphics/visualfx.h"
#include "resources.h"
#include "commandline.h"
#include "gamemusic.h"
#include "gothic.h"
#include "settings.h"
#include "game/definitions/basescripts.h"
#include "game/gamescript.h"
#include "game/gamesession.h"
#include "game/npcactivityhost.h"
#include "game/physicssave.h"
#include "game/serialize.h"
#include "gameservices.h"
#include "testbench.h"
#include "testbench_internal.h"
#include "world/benchhooks.h"
#include "world/bullet.h"
#include "world/objects/interactive.h"
#include "world/objects/item.h"
#include "world/objects/npc.h"
#include "world/records/liveinstances.h"
#include "world/waypoint.h"
#include "world/world.h"

#include <Tempest/MemReader>
#include <Tempest/MemWriter>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include <unistd.h>

struct NpcDefinitionContract {
  static bool mobGestureDue(const Npc& npc) { return npc.mobGestureDue(); }
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
  // an object's model and its effects of the animations' events (MdlVisual::startEffect)
  static const MdlVisual& model(const Interactive& mob) { return mob.objVisual().mdl.view; }
  static const auto& effects(const MdlVisual& view) { return view.effects; }
  static const auto& effects(const Npc& npc) { return effects(npc.visual); }
  static size_t bone(const MdlVisual& view, std::string_view name) { return view.skeleton->findNode(name); }
  static const AnimationSolver& solver(const MdlVisual& view) { return view.solver; }
  static auto& itemAlias(GameScript& script) { return script.storyActorSlot("ITEM"); }
  static const auto& transferVariables(GameScript& script) { return script.inventoryTransferVariables; }
  };

namespace {
void require(bool condition,const char* message) {
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
  const auto expectedName = npc.world().script().storyNpcName(uint32_t(npc.instanceSymbol()));
  require(!expectedName.empty(),"the live NPC has an archetype name");
  for(bool installed:{false,true}) {
    if(installed)
      TestBench::install();
    require(Bench::bodyJson(text)==json,"JSON escaping preserves the legacy bytes with and without an adapter");
    require(Bench::who(nullptr)=="none","a null NPC has no name with and without an adapter");
    require(Bench::who(&npc)==expectedName,"NPC name matches with and without an adapter");
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

inline Npc* activityNpc(World& world,const char* name) {
  const auto symbol=world.script().findSymbolIndex(name);
  for(uint32_t i=0;i<world.npcCount();++i)
    if(world.npcById(i)->instanceSymbol()==symbol)
      return world.npcById(i);
  return nullptr;
  }


struct ActivityPhases {
  size_t init=0,loop=0,end=0;
  };
ActivityPhases activityPhases(GameScript& script,const char* state) {
  const auto& st=script.aiState(script.findSymbolIndex(state));
  require(st.funcIni!=0 && st.funcLoop!=0 && st.funcEnd!=0,"activity state must have Init, Loop and End");
  return {st.funcIni,st.funcLoop,st.funcEnd};
  }

const WayPoint* activityRow(Npc& npc,size_t init,std::string& name) {
  const WayPoint* point=nullptr;
  npc.forEachRoutine([&](const Npc::RoutineInfo& row) {
    if(point==nullptr && row.state.ptr==init && row.waypoint!=nullptr) {
      point=row.waypoint; name=std::string(row.point);
      }
    });
  return point;
  }

bool activityCandidate(Npc& npc) { return !npc.isPlayer() && npc.isHuman() && !npc.isDead(); }

std::string npcName(World& world,const Npc& npc) { return std::string(world.script().symbolName(npc.instanceSymbol())); }

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

const WorldObjects::PlacedItem& placedMoverChild(World& world) {
  const auto& catalogue=world.placedItemCensus();
  const auto child=std::find_if(catalogue.begin(),catalogue.end(),[](const auto& row) { return row.zenId==2839; });
  require(child!=catalogue.end() && child->item!=nullptr,
          "the authored catalogue must contain Item 2839 below mover 2838 on both startup and reload");
  const auto attachments=world.vobAttachmentSnapshot(2838);
  require(std::any_of(attachments.begin(),attachments.end(),[](const auto& row) {
            return row.zenId==2839 && row.parentZenId==2838;
            }),"the save fixture must contain an actual mover-child Item");
  return *child;
  }

uint32_t placedWorldItemCount(World& world) {
  uint32_t count=0;
  while(world.itmById(count)!=nullptr)
    ++count;
  return count;
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
  // A fresh forge lets its entrance animation light the fire.
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

void furniturePoseAdmission(Gothic& gothic) {
  gothic.clearGame().reset();
  gothic.setGame(std::make_unique<GameSession>("NEWWORLD.ZEN"));
  auto& world=*gothic.world();
  auto* session=gothic.gameSession();
  auto* sitter=activityNpc(world,"MIL_313_BOLTAN");
  auto* rival=activityNpc(world,"MIL_319_PABLO");
  auto* sleeper=activityNpc(world,"VLK_416_MATTEO");
  const auto* bedPoint=world.findWayPoint("NW_CITY_BED_MATTEO");
  require(sitter && rival && sleeper && bedPoint,"furniture pose test needs the authored chair, bed and their actors");

  const auto advanceUntil=[&](const auto& ready) {
    for(int i=0; i<900 && !ready(); ++i) {
      session->tick(33);
      session->updateAnimation(33);
      }
    return ready();
    };
  for(uint32_t i=0; i<world.npcCount(); ++i) {
    auto& npc=*world.npcById(i);
    if(!npc.isPlayer() && &npc!=sitter)
      npc.setExternalControl(true);
    }
  require(advanceUntil([&]{ return sitter->settledFurniturePose() && sitter->societyFurnitureHold(); }),
          "the native chair routine must reach a settled pose eligible for Society");
  auto& chair=*sitter->interactive();
  require(!rival->setInteraction(&chair) && chair.isAttached(*sitter) && chair.userCount()==1,
          "a settled chair stays occupied by its original sitter");
  sitter->setStateTime(5999);
  require(!NpcDefinitionContract::mobGestureDue(*sitter),"the chair timer waits until six whole seconds");
  sitter->setStateTime(6000);
  require(NpcDefinitionContract::mobGestureDue(*sitter),"the chair timer is due at six whole seconds");
  sitter->setExternalControl(true);
  sitter->aiPush(AiQueue::aiUseMob("CHAIR",-1));
  require(advanceUntil([&]{ return !sitter->settledFurniturePose() && chair.isAvailable(); }) &&
          !sitter->societyFurnitureHold(),"leaving the chair releases its place and both eligibility checks");

  sleeper->setPosition(bedPoint->position());
  const auto bedtime=world.script().findFunction("ZS_GOTOBED");
  require(bedtime.isValid() && sleeper->startState(bedtime,bedPoint->name),"the native sleep routine must start");
  sleeper->setExternalControl(false);
  require(advanceUntil([&]{ return sleeper->settledFurniturePose(); }) && !sleeper->societyFurnitureHold(),
          "the occupied bed is eligible for pose LOD while native sleep continues");
  std::puts("Furniture poses: chair ownership, gesture deadline and native sleep admission passed");
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
  require(baseNativeCatalog()!=nullptr,"placement and save require the native base catalog");
  gothic.setGame(std::make_unique<GameSession>("NEWWORLD.ZEN"));
  require(gothic.world()!=nullptr && gothic.player()!=nullptr,"native base must start a live world and player");
  benchHelpers(*gothic.player());
  const auto& child=placedMoverChild(*gothic.world());
  auto& script=gothic.world()->script();
  auto* lares=activityNpc(*gothic.world(),"VLK_449_LARES");
  auto* hero=gothic.player();
  const auto ornament=script.findSymbolIndex("ITMI_ORNAMENT_ADDON_VATRAS");
  require(lares && ornament!=size_t(-1),"the native Lares dialog needs Lares and his ornament");
  lares->delItem(ornament,uint32_t(lares->itemCount(ornament)));
  hero->delItem(ornament,uint32_t(hero->itemCount(ornament)));
  const auto offered=script.callStoryFunction("DIA_ADDON_LARES_PATCH_INFO",lares,hero,{});
  require(offered.ok && hero->itemCount(ornament)==1 && lares->itemCount(ornament)==0,
          "the native Lares dialog must grant the ornament from an empty giver");
  const auto itemText=NpcDefinitionContract::transferVariables(script).itemName;
  require(!itemText.empty(),"Lares's transfer must keep the localized item name");
  constexpr std::string_view director="DIR_RUNTIME_SAVE_CONTRACT";
  require(script.addStoryVariable(director,-41).empty(),"the save fixture must declare a director integer");
  const auto directorId=script.findSymbolIndex(director);
  require(directorId!=size_t(-1),"the fixture must have a stable director field ID");
  constexpr std::array pointRolls={
    std::pair{"ZS_STAND_GUARDING_LOOP.RANDOM",5}, std::pair{"ZS_STAND_ARMSCROSSED_LOOP.RANDOM",2},
    std::pair{"ZS_STAND_WP_LOOP.RANDOM",7}, std::pair{"ZS_STAND_ARMSCROSSED_LOOP.EVENTRANDY",1},
    std::pair{"B_EVENT_PORTAL_EARTHQUAKE.RANDY",3},
    };
  for(const auto& [name,value]:pointRolls)
    require(script.setGlobalIntSymbol(name,value),"the point roll must be in the native session state");
  const std::string journalTopic="Native placed-item contract";
  const std::string journalEntry="mover child and ornament";
  const size_t journalIndex=script.questLog().questCount();
  require(script.callStoryFunction("LOG_CREATETOPIC",lares,hero,{journalTopic,int32_t(QuestLog::Note)}).ok &&
          script.callStoryFunction("LOG_ADDENTRY",lares,hero,{journalTopic,journalEntry}).ok,
          "the native journal operations must accept the checkpoint note");
  NpcDefinitionContract::itemAlias(script)=std::static_pointer_cast<ItemInstance>(child.item->handlePtr());
  const auto catalogueSize=gothic.world()->placedItemCensus().size();
  const auto itemCount=placedWorldItemCount(*gothic.world());
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
  const auto& restored=placedMoverChild(*gothic.world());
  auto& loaded=gothic.world()->script();
  for(const auto& [name,value]:pointRolls)
    require(loaded.globalIntSymbol(name)==value,"the reload must restore the point and earthquake rolls");
  require(loaded.questLog().questCount()==journalIndex+1 &&
          loaded.questLog().quest(journalIndex).name==journalTopic &&
          loaded.questLog().quest(journalIndex).entry==std::vector{journalEntry},
          "the native journal entry must survive the ordinary save");
  require(gothic.player()->itemCount(ornament)==1 && loaded.findSymbolIndex(director)==directorId &&
          loaded.globalIntSymbol(director)==-41 && NpcDefinitionContract::transferVariables(loaded).itemName==itemText,
          "the reload must recover the ornament, transfer text and director field with its ID");
  require(NpcDefinitionContract::itemAlias(loaded)==std::static_pointer_cast<ItemInstance>(restored.item->handlePtr()),
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
  itemEffects(*gothic.world());
  arrowEffects(gothic);
  furniturePoseAdmission(gothic);
  animationEffects(gothic);
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
    const auto* noGameData=std::getenv("OPENGOTHIC_CONTRACT_NO_GAME_DATA");
    if(noGameData && std::strcmp(noGameData,"1")==0)
      std::puts("SKIP stock world and v57 reload: OPENGOTHIC_CONTRACT_NO_GAME_DATA=1");
    else
      moverChildSave();
    std::puts("Placed item census contracts passed");
    return EXIT_SUCCESS;
    }
  catch(const std::exception& error) {
    std::fprintf(stderr,"FAIL: placed item census contract: %s\n",error.what());
    return EXIT_FAILURE;
    }
  }
