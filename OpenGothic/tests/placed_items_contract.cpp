#include "physics/dynamicworld.h"
#include "graphics/bounds.h"
#include "graphics/mesh/protomesh.h"
#include "graphics/mesh/submesh/packedmesh.h"
#include "resources.h"
#include "commandline.h"
#include "gamemusic.h"
#include "gothic.h"
#include "game/serialize.h"
#include "world/objects/item.h"

#include <Tempest/MemReader>
#include <Tempest/MemWriter>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

void moverChildSave() {
  const auto* game=std::getenv("GOTHIC_DIR");
  require(game!=nullptr && *game!='\0',"GOTHIC_DIR must point to prepared Gothic II settings");
  const char* args[]={"gothic-placed-items-contract","-g",game,"-g2"};
  CommandLine commandLine(4,args);
  Resources resources;
  Gothic gothic;
  gothic.isNpcInDialogFn=[](const Npc*) { return false; };
  gothic.setupGlobalScripts();
  GameMusic music;
  gothic.setGame(std::make_unique<GameSession>("NEWWORLD.ZEN"));
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
    slopes();
    penetrationLimit();
    groundBelowSupport();
    asymmetricLowestPoint();
    irregularSupport();
    moverChildSave();
    std::puts("Placed item census contracts passed");
    return EXIT_SUCCESS;
    }
  catch(const std::exception& error) {
    std::fprintf(stderr,"FAIL: placed item census contract: %s\n",error.what());
    return EXIT_FAILURE;
    }
  }
