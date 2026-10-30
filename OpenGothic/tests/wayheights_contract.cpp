#include "commandline.h"
#include "game/gamesession.h"
#include "gamemusic.h"
#include "gothic.h"
#include "physics/dynamicworld.h"
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
    if(argc>2 || (argc==2 && std::string_view(argv[1])!="--baseline"))
      throw std::runtime_error("usage: gothic-wayheights-contract [--baseline]");
    const auto* game=std::getenv("GOTHIC_DIR");
    if(game==nullptr || *game=='\0')
      throw std::runtime_error("GOTHIC_DIR must point to prepared Gothic II settings");
    const char* args[]={"gothic-wayheights-contract","-g",game,"-g2"};
    CommandLine commandLine(4,args);
    Environment ownContent("OPENGOTHIC_CONTENT","");
    Resources resources;
    Gothic gothic;
    gothic.isNpcInDialogFn=[](const Npc*) { return false; };
    gothic.setupGlobalScripts();
    GameMusic music;
    if(argc==2) {
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
