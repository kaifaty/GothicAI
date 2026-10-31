#include "commandline.h"
#include "game/gamesession.h"
#include "gamemusic.h"
#include "gothic.h"
#include "physics/collisioncache.h"
#include "physics/collisionworld.h"
#include "physics/dynamicworld.h"
#include "physics/physicvbo.h"
#include "resources.h"
#include "world/wayheights.h"
#include "world/world.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
namespace fs=std::filesystem;
using Bits=std::array<uint32_t,3>;

void require(bool condition, const std::string& message) {
  if(!condition)
    throw std::runtime_error(message);
  }

Bits bits(const Tempest::Vec3& value) {
  return {std::bit_cast<uint32_t>(value.x),std::bit_cast<uint32_t>(value.y),std::bit_cast<uint32_t>(value.z)};
  }

struct Environment {
  std::string name;
  std::optional<std::string> previous;
  Environment(std::string name, const std::string& value):name(std::move(name)) {
    if(const auto* old=std::getenv(this->name.c_str())) previous=old;
    setenv(this->name.c_str(),value.c_str(),1);
    }
  ~Environment() {
    if(previous) setenv(name.c_str(),previous->c_str(),1); else unsetenv(name.c_str());
    }
  };

struct TemporaryDirectory {
  fs::path path=fs::current_path()/
    ("wayheights-contract-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  ~TemporaryDirectory() {
    std::error_code error;
    fs::remove_all(path,error);
    }
  };

void write(const fs::path& file, std::string_view bytes) {
  fs::create_directories(file.parent_path());
  std::ofstream output(file,std::ios::binary|std::ios::trunc);
  output.write(bytes.data(),std::streamsize(bytes.size()));
  require(bool(output),"cannot write "+file.string());
  }

std::string read(const fs::path& file) {
  std::ifstream input(file,std::ios::binary);
  return {std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>()};
  }

void collisionFiles(const fs::path& root) {
  const auto content=root/"content";
  const auto layer=content/"worlds/SMALL/vobs.d/fixture.json";
  write(layer,"[]");
  Environment ownContent("OPENGOTHIC_CONTENT",content.string());
  Environment enabled("OPENGOTHIC_COLLISION_CACHE","1");
  const auto inputs=Resources::wayHeightInputs("SMALL.ZEN");
  require(!inputs.baseFingerprint.empty() && !inputs.contentFingerprint.empty(),"collision inputs unavailable");
  const CollisionCache::Key key{inputs.baseFingerprint,"SMALL",inputs.contentFingerprint};
  const std::vector<JPH::Vec3> vertices={{-1,0,-1},{-1,0,1},{1,0,1},{1,0,-1}};
  PhysicVbo mesh(&vertices);
  mesh.addIndex({0,2,1,0,3,2},0,6,asset::MaterialGroup::STONE,"cache-fixture");
  const auto shape=mesh.shape();
  CollisionCache cache(root,key);
  JPH::ShapeRefC land,water;
  require(!cache.read(land,water),"cold collision cache must miss");
  cache.save(shape.GetPtr(),shape.GetPtr());
  require(cache.read(land,water) && land!=nullptr && water!=nullptr,"collision cache must restore both shapes");
  const auto saved=read(cache.path());
  require(!saved.empty(),"collision save must write a file");
  const auto rebuild=[&](const CollisionCache::Key& changed,const char* reason) {
    CollisionCache invalid(root,changed);
    land=water=nullptr;
    require(!invalid.read(land,water),reason);
    invalid.save(shape.GetPtr(),shape.GetPtr());
    require(invalid.read(land,water),"collision miss must be replaceable by a valid bake");
    };
  auto changed=key;
  changed.content="foreign content";
  rebuild(changed,"foreign collision key must miss");
  rebuild(key,"original key must reject a foreign bake at the same path");
  write(layer,"[ ]");
  changed.content=Resources::wayHeightInputs("SMALL.ZEN").contentFingerprint;
  require(changed.content!=key.content,"world content edit must change the collision input fingerprint");
  rebuild(changed,"content file edit must force collision rebuild");
  changed=key;
  changed.collisionFlags^=1;
  rebuild(changed,"collision flags must force rebuild");
  changed=key;
  ++changed.version;
  rebuild(changed,"collision bake version must force rebuild");
  write(cache.path(),"broken collision cache");
  rebuild(key,"truncated collision file must rebuild");
  auto corrupt=saved;
  corrupt.back()^=1;
  write(cache.path(),corrupt);
  rebuild(key,"damaged Jolt payload must rebuild before deserialization");
  {
    Environment disabled("OPENGOTHIC_COLLISION_CACHE","0");
    CollisionCache bypass(root,key);
    land=water=nullptr;
    const auto before=read(cache.path());
    require(!bypass.read(land,water),"disabled collision cache must not read a valid bake");
    bypass.save(nullptr,nullptr);
    require(read(cache.path())==before,"disabled collision cache must not overwrite an existing bake");
    fs::remove(cache.path());
    bypass.save(shape.GetPtr(),shape.GetPtr());
    require(!fs::exists(cache.path()),"disabled collision cache must not create a bake");
  }
  std::puts("Collision files: both shapes restored; foreign key, world content edit, flags, bake version, truncated file and checksum rebuild; disabled read/write bypass passed");
  }

asset::WorldMesh floorMesh() {
  asset::WorldMesh mesh;
  mesh.vertices={{-2000,0,-2000},{-2000,0,2000},{2000,0,2000},{2000,0,-2000}};
  mesh.materials.resize(1);
  mesh.materials[0].group=asset::MaterialGroup::STONE;
  mesh.polygons.vertex_indices={0,2,1,0,3,2};
  mesh.polygons.material_indices={0,0};
  return mesh;
  }

std::vector<WayPoint> smallPoints() {
  std::vector<WayPoint> points;
  points.emplace_back(Tempest::Vec3(0,100,0),"REPEATED");
  points.emplace_back(Tempest::Vec3(100,200,0),"REPEATED");
  points.emplace_back(Tempest::Vec3(4000,100,0),"MISS");
  return points;
  }

void smallWorld(const fs::path& root) {
  const auto content=root/"content";
  const auto layer=content/"worlds/SMALL/vobs.d/fixture.json";
  write(layer,"[]");
  Environment ownContent("OPENGOTHIC_CONTENT",content.string());
  Environment enabled("OPENGOTHIC_WAYHEIGHT_CACHE","1");
  const auto inputs=Resources::wayHeightInputs("SMALL.ZEN");
  require(!inputs.baseFingerprint.empty() && !inputs.contentFingerprint.empty(),"small-world inputs unavailable");
  const WayHeights::Key key{inputs.baseFingerprint,"SMALL",inputs.contentFingerprint};
  DynamicWorld physics(nullptr,floorMesh());
  auto expected=smallPoints();
  for(auto& point:expected) {
    const auto ray=physics.landRay(point.position());
    point.groundPos=ray.hasCol ? ray.v : point.pos;
    }
  require(std::abs(expected[0].groundPos.y)<.001f && std::abs(expected[1].groundPos.y)<.001f,
          "fixture rays must hit the floor");
  require(bits(expected.back().groundPos)==bits(expected.back().pos),"miss must retain source position");

  WayHeights miss(root,key);
  auto points=smallPoints();
  for(auto& point:points) miss.adjust(point,physics);
  require(miss.hits()==0 && miss.misses()==3,"cold small world must cast three rays");
  miss.save();
  require(fs::is_regular_file(miss.path()),"cache miss must write a file");
  for(size_t i=0;i<points.size();++i)
    require(bits(points[i].groundPos)==bits(expected[i].groundPos),"miss changed ground position");

  WayHeights hit(root,key);
  points=smallPoints();
  std::reverse(points.begin(),points.end());
  for(size_t i=0;i<points.size();++i) {
    hit.adjust(points[i],physics);
    require(bits(points[i].groundPos)==bits(expected[expected.size()-1-i].groundPos),
            "cache must match name and original position, independently of point order");
    }
  require(hit.hits()==3 && hit.misses()==0,"warm small world must cast zero rays");
  WayPoint added(Tempest::Vec3(300,150,0),"REPEATED");
  hit.adjust(added,physics);
  require(hit.hits()==3 && hit.misses()==1,"only a missing name/position pair may cast a ray");
  hit.save();
  WayHeights merged(root,key);
  for(auto& point:points) merged.adjust(point,physics);
  merged.adjust(added,physics);
  require(merged.hits()==4 && merged.misses()==0,"partial bake must preserve existing entries and save the missing point");

  const auto checkMiss=[&](const WayHeights::Key& changed, const char* reason) {
    WayHeights cache(root,changed);
    WayPoint point(Tempest::Vec3(0,100,0),"REPEATED");
    cache.adjust(point,physics);
    require(cache.hits()==0 && cache.misses()==1,reason);
    require(bits(point.groundPos)==bits(expected[0].groundPos),"invalidated cache must use the original ray");
    };
  write(layer,"[ ]");
  auto changed=key;
  changed.content=Resources::wayHeightInputs("SMALL.ZEN").contentFingerprint;
  require(changed.content!=key.content,"content file change must alter the actual input fingerprint");
  checkMiss(changed,"content file change must miss");
  const auto beforeChunk=changed.content;
  write(content/"worlds/SMALL/chunks/0_0.glb","replacement chunk");
  changed.content=Resources::wayHeightInputs("SMALL.ZEN").contentFingerprint;
  require(changed.content!=beforeChunk,"replacement chunk must alter the actual input fingerprint");
  checkMiss(changed,"replacement chunk change must miss");
  changed=key;
  changed.collisionFlags^=1;
  checkMiss(changed,"collision flag change must miss");
  changed=key;
  ++changed.version;
  checkMiss(changed,"bake version change must miss");
  changed=key;
  changed.base=std::string(64,'a');
  checkMiss(changed,"foreign base must miss");

  write(miss.path(),"broken way heights");
  checkMiss(key,"corrupt file must miss without a crash");
  WayHeights rebuilt(root,key);
  points=smallPoints();
  for(auto& point:points) rebuilt.adjust(point,physics);
  rebuilt.save();
  {
    Environment disabled("OPENGOTHIC_WAYHEIGHT_CACHE","0");
    WayHeights cache(root,key);
    for(auto& point:points) cache.adjust(point,physics);
    require(cache.hits()==0 && cache.misses()==3,"disabled cache must always cast rays");
    cache.save();
  }
  std::puts("Small world: miss/write, exact hit, repeated names/order, partial miss, content/chunk/flag/version/base invalidation, corruption and disabled rays passed");
  }
}

struct WayHeightsContract {
  struct CollisionRay {
    bool hit=false;
    Tempest::Vec3 point;
    Bits normal;
    asset::MaterialGroup material=asset::MaterialGroup::UNDEFINED;
    std::string sector;
    };
  struct CollisionSnapshot {
    std::vector<CollisionRay> rays;
    std::vector<CollisionWorld::CharacterMoveResult> movement;
    NavMesh::Geometry navigation;
    DynamicWorld::StaticCollisionLoadStats stats;
    };
  struct Ground {
    std::string name;
    Bits source, ground;
    };
  struct Snapshot {
    WayMatrix::BuildStats stats;
    std::vector<Ground> points;
    };

  static Snapshot load(Gothic& gothic, const char* mode) {
    gothic.setGame(std::make_unique<GameSession>("NEWWORLD.ZEN"));
    const auto& matrix=*gothic.world()->wmatrix;
    Snapshot result{matrix.stats,{}};
    for(const auto* list:{&matrix.wayPoints,&matrix.freePoints,&matrix.startPoints})
      for(const auto& point:*list)
        result.points.push_back({point.name,bits(point.pos),bits(point.groundPos)});
    const auto& s=result.stats;
    std::printf("NEWWORLD %s: %zu points (%zu waypoints, %zu freepoints, %zu startpoints), %zu hits, %zu rays; key %.3f ms, adjust %.3f ms, index %.3f ms, ladders %.3f ms, write %.3f ms, total %.3f ms\n",
                mode,result.points.size(),matrix.wayPoints.size(),matrix.freePoints.size(),matrix.startPoints.size(),
                s.hits,s.misses,s.keyMs,s.adjustMs,s.indexMs,s.laddersMs,s.writeMs,s.totalMs);
    gothic.clearGame().reset();
    return result;
    }

  static void compare(const Snapshot& rays, const Snapshot& cache, const char* mode) {
    require(rays.points.size()==cache.points.size(),"NEWWORLD point count differs");
    double difference=0;
    for(size_t i=0;i<rays.points.size();++i) {
      const auto& expected=rays.points[i];
      const auto& actual=cache.points[i];
      require(expected.name==actual.name && expected.source==actual.source,"NEWWORLD source point differs");
      for(size_t axis=0;axis<3;++axis)
        difference=std::max(difference,std::abs(double(std::bit_cast<float>(expected.ground[axis]))-
                                                double(std::bit_cast<float>(actual.ground[axis]))));
      require(expected.ground==actual.ground,"NEWWORLD ground position differs at "+expected.name);
      }
    std::printf("NEWWORLD %s equality: %zu/5249 ground positions byte-identical, maximum difference %.9g cm\n",
                mode,cache.points.size(),difference);
    }

  static CollisionSnapshot collisionLoad(Gothic& gothic, const char* mode) {
    gothic.setGame(std::make_unique<GameSession>("NEWWORLD.ZEN"));
    auto& scene=*gothic.world();
    auto& physics=*scene.wdynamic;
    CollisionSnapshot result;
    result.stats=physics.staticCollisionLoadStats();
    result.navigation=physics.navigationGeometry();
    const float infinity=std::numeric_limits<float>::infinity();
    Tempest::Vec3 minimum(infinity),maximum(-infinity);
    const auto& table=scene.chunkTable();
    for(const auto& entry:table.entries)
      if(entry.triangleCount!=0) {
        minimum.x=std::min(minimum.x,entry.min.x); maximum.x=std::max(maximum.x,entry.max.x);
        minimum.y=std::min(minimum.y,entry.min.y); maximum.y=std::max(maximum.y,entry.max.y);
        minimum.z=std::min(minimum.z,entry.min.z); maximum.z=std::max(maximum.z,entry.max.z);
        }
    require(std::isfinite(minimum.x) && std::isfinite(minimum.y) && std::isfinite(minimum.z) &&
            std::isfinite(maximum.x) && std::isfinite(maximum.y) && std::isfinite(maximum.z) &&
            maximum.x>minimum.x && maximum.y>minimum.y && maximum.z>minimum.z,
            "collision ray matrix requires finite nonempty island bounds");
    const float top=maximum.y+1000,bottom=minimum.y-1000;
    const auto sample=[&](float x,float z,float tilt,uint32_t mask) {
      const auto ray=physics.world->rayCast({x-tilt,top,z+tilt},{x+tilt,bottom,z-tilt},mask);
      result.rays.push_back({ray.hasHit,ray.point,bits(ray.normal),ray.material,ray.sector==nullptr ? "" : ray.sector});
      };
    constexpr uint32_t landscapeMask=1u<<DynamicWorld::C_Landscape,waterMask=1u<<DynamicWorld::C_Water;
    for(float x=minimum.x;x<=maximum.x;x+=1600)
      for(float z=minimum.z;z<=maximum.z;z+=1600)
        for(const float tilt:{0.f,3200.f}) {
          sample(x,z,tilt,landscapeMask);
          sample(x,z,tilt,waterMask);
          }
    const auto gridRays=result.rays.size();
    require(gridRays>4,"collision grid must cover the island");
    // Retain #616's 1 m samples on every 64 m boundary, at -1/0/+1 mm.
    for(const auto& chunk:table.entries) {
      if(chunk.triangleCount==0)continue;
      const float x=float(chunk.id.x)*ChunkId::Size,z=float(chunk.id.z)*ChunkId::Size;
      for(int along=0;along<=ChunkId::Size;along+=100)
        for(const float offset:{-.1f,0.f,.1f})
          for(const float tilt:{0.f,3200.f}) {
            sample(x+offset,z+float(along),tilt,landscapeMask);
            sample(x+float(along),z+offset,tilt,landscapeMask);
            }
      }

    size_t routes=0;
    for(const char* name:{"NW_CITY_MERCHANT_TRADE_04","NW_CITY_HABOUR_BENCH_01","NW_FOREST_PATH_16"}) {
      const auto* start=scene.findWayPoint(name);
      require(start!=nullptr && !start->connections().empty(),"collision CV route requires a connected waypoint");
      const auto target=start->connections().front().point->groundPos;
      auto direction=target-start->groundPos;
      direction.y=0;
      require(direction.length()>1,"collision CV route requires a nonzero horizontal segment");
      direction/=direction.length();
      for(const float speed:{180.f,400.f}) {
        auto walker=physics.world->createVirtualCharacter(25,180,start->groundPos);
        for(int frame=0;frame<180;++frame)
          result.movement.push_back(walker->move(direction*(speed*.017f),17,50,.785398163f,80,nullptr));
        ++routes;
        }
      }
    const auto& s=result.stats;
    std::printf("NEWWORLD collision %s: %s; key/read/build/write %.3f/%.3f/%.3f/%.3f ms; %zu rays (%zu grid land+water, %zu seam), %zu CV routes/%zu frames, navigation %zu+%zu triangles\n",
                mode,s.cacheHit ? "cache" : "build",s.keyMs,s.readMs,s.buildMs,s.writeMs,
                result.rays.size(),gridRays,result.rays.size()-gridRays,routes,result.movement.size(),
                result.navigation.triangles.size()/3,result.navigation.waterTriangles.size()/3);
    gothic.clearGame().reset();
    return result;
    }

  static void collisionCompare(const CollisionSnapshot& built,const CollisionSnapshot& restored,const char* mode) {
    require(built.rays.size()==restored.rays.size(),"collision ray count differs");
    float maxPointError=0;
    size_t hits=0;
    for(size_t i=0;i<built.rays.size();++i) {
      const auto& a=built.rays[i];
      const auto& b=restored.rays[i];
      require(a.hit==b.hit,"cached collision ray hit differs at "+std::to_string(i));
      if(!a.hit)continue;
      ++hits;
      const float error=(a.point-b.point).length();
      maxPointError=std::max(maxPointError,error);
      require(error<=.1f && a.normal==b.normal && a.material==b.material && a.sector==b.sector,
              "cached collision point/normal/material/sector differs at "+std::to_string(i));
      }
    require(built.movement.size()==restored.movement.size(),"collision CV frame count differs");
    for(size_t i=0;i<built.movement.size();++i) {
      const auto& a=built.movement[i];
      const auto& b=restored.movement[i];
      require(bits(a.position)==bits(b.position) && bits(a.velocity)==bits(b.velocity) &&
              bits(a.groundNormal)==bits(b.groundNormal) && bits(a.groundVelocity)==bits(b.groundVelocity) &&
              bits(a.steepContactNormal)==bits(b.steepContactNormal) && a.grounded==b.grounded &&
              a.steep==b.steep && a.steepContact==b.steepContact && a.supportMoved==b.supportMoved,
              "cached collision CV sweep differs at frame "+std::to_string(i));
      }
    const auto& a=built.navigation;
    const auto& b=restored.navigation;
    const auto key=NavMesh::cacheKey(a,NavMesh::Params{},"NEWWORLD.ZEN");
    require(a.vertices==b.vertices && a.triangles==b.triangles &&
            a.waterVertices==b.waterVertices && a.waterTriangles==b.waterTriangles &&
            key==NavMesh::cacheKey(b,NavMesh::Params{},"NEWWORLD.ZEN"),
            "cached collision must preserve byte-identical navigation input and cache key");
    std::printf("NEWWORLD collision %s equality: %zu rays/%zu hits, max point error %.9g cm; normals/materials/sectors and %zu CV frames identical; native navigation geometry/order and key %llu identical (NavMesh::buildOrLoad selects the same serialized navmesh)\n",
                mode,built.rays.size(),hits,double(maxPointError),built.movement.size(),
                static_cast<unsigned long long>(key));
    }

  static void collisionRun(Gothic& gothic,const fs::path& root) {
    collisionFiles(root/"small");
    Environment wayHeightRays("OPENGOTHIC_WAYHEIGHT_CACHE","0");
    Environment cacheRoot("OPENGOTHIC_BASE_CACHE",(root/"base-cache").string());
    CollisionSnapshot built;
    {
      Environment disabled("OPENGOTHIC_COLLISION_CACHE","0");
      built=collisionLoad(gothic,"disabled");
      require(!built.stats.cacheHit && built.stats.buildMs>0 && built.stats.readMs==0 && built.stats.writeMs==0,
              "disabled world collision cache must build without reading or writing");
      require(!fs::exists(root/"base-cache/derived"),"disabled world load must not create collision files");
    }
    Environment enabled("OPENGOTHIC_COLLISION_CACHE","1");
    {
      const auto cold=collisionLoad(gothic,"cold");
      require(!cold.stats.cacheHit && cold.stats.buildMs>0,"cold world collision cache must build");
      collisionCompare(built,cold,"cold");
    }
    const auto warm=collisionLoad(gothic,"warm");
    require(warm.stats.cacheHit && warm.stats.buildMs==0 && warm.stats.writeMs==0,
            "warm world collision cache must restore without rebuilding or writing");
    collisionCompare(built,warm,"warm");
    }

  static void run(Gothic& gothic, const fs::path& root) {
    Environment cacheRoot("OPENGOTHIC_BASE_CACHE",(root/"base-cache").string());
    Snapshot rays;
    {
      Environment disabled("OPENGOTHIC_WAYHEIGHT_CACHE","0");
      rays=load(gothic,"disabled");
    }
    require(rays.points.size()==5249 && rays.stats.misses==5249,"NEWWORLD must cast 5249 disabled rays");
    Environment enabled("OPENGOTHIC_WAYHEIGHT_CACHE","1");
    const auto miss=load(gothic,"miss");
    require(miss.stats.hits==0 && miss.stats.misses==5249,"NEWWORLD cold cache must cast 5249 rays");
    compare(rays,miss,"miss");
    const auto hit=load(gothic,"hit");
    require(hit.stats.hits==5249 && hit.stats.misses==0,"NEWWORLD cache hit must cast zero rays");
    compare(rays,hit,"hit");
    require(hit.stats.keyMs<=20,"NEWWORLD cache key must take at most 20 ms");
    require(hit.stats.totalMs<=100,"NEWWORLD warm buildIndex must take at most 100 ms");
    write(root/"blocked-cache","not a directory");
    for(const auto& path:{std::string(),(root/"blocked-cache/child").string()}) {
      Environment unavailableRoot("OPENGOTHIC_BASE_CACHE",path);
      const auto unavailable=load(gothic,path.empty() ? "empty cache root" : "unavailable cache root");
      require(unavailable.stats.hits==0 && unavailable.stats.misses==5249,
              "empty or unavailable cache root must cast 5249 rays");
      compare(rays,unavailable,"unavailable cache root");
      }
    }
  };

int main(int argc, char* argv[]) {
  try {
    if(argc>2 || (argc==2 && std::string_view(argv[1])!="--baseline" && std::string_view(argv[1])!="--collision"))
      throw std::runtime_error("usage: gothic-wayheights-contract [--baseline|--collision]");
    const auto* collisionOnly=std::getenv("OPENGOTHIC_COLLISION_CONTRACT_ONLY");
    const bool collision=(argc==2 && std::string_view(argv[1])=="--collision") ||
                         (collisionOnly!=nullptr && std::string_view(collisionOnly)=="1");
    const auto* game=std::getenv("GOTHIC_DIR");
    if(game==nullptr || *game=='\0')
      throw std::runtime_error("GOTHIC_DIR must point to prepared Gothic II settings");
    const char* args[]={"gothic-wayheights-contract","-g",game,"-g2"};
    CommandLine commandLine(4,args);
    std::optional<Environment> ownContent;
    if(!collision)ownContent.emplace("OPENGOTHIC_CONTENT","");
    Resources resources;
    Gothic gothic;
    gothic.isNpcInDialogFn=[](const Npc*) { return false; };
    gothic.setupGlobalScripts();
    GameMusic music;
    if(collision) {
      TemporaryDirectory temporary;
      WayHeightsContract::collisionRun(gothic,temporary.path);
      std::puts("Collision cache contract passed");
      }
    else if(argc==2) {
      Environment disabled("OPENGOTHIC_WAYHEIGHT_CACHE","0");
      WayHeightsContract::load(gothic,"baseline");
      }
    else {
      TemporaryDirectory temporary;
      smallWorld(temporary.path/"small");
      WayHeightsContract::run(gothic,temporary.path);
      std::puts("Way heights contract passed");
      }
    return EXIT_SUCCESS;
    }
  catch(const std::exception& error) {
    std::fprintf(stderr,"Way heights contract: %s\n",error.what());
    return EXIT_FAILURE;
    }
  }
