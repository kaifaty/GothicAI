// The collision cache holds the CPU query data with the shapes (docs/landscape-plan.md, Д3, task 6;
// physics/collisioncache.h, physics/staticcollision.h). Each world is loaded as the game loads it — readWorldChunks
// with the cooked set, WorldCloth::extract — and DynamicWorld gets the world load's lazy input
// (WorldCollisionSource, as World passes it) with a cache root of this contract:
//   1. a cold start (miss, write), a warm start (hit) and a start with the cache off give byte-identical navigation
//      geometry, the same grid and named sector rays and the same PhysicVbo answers (staticcollision_probe.h);
//   2. on a hit the input is not called, no [world-collision] line is logged and the load mark says input=cache;
//   3. a truncated file, a flipped byte in the CPU part or in the shapes, a version 1 file, another key (content,
//      flags, cloth rules), an index, a segment or a sector reference out of range with the checksum recomputed,
//      and a leftover temporary file beside a missing cache: each a miss with its one log line, a rebuild equal to
//      the cold start and a hit on the next start;
//   4. OPENGOTHIC_COLLISION_CACHE=0 builds from the input and leaves the cache directory untouched.
// On the collision contract's fixtures (the cloth world and its three overrides, the strip and its two overrides,
// the world with disable_collision, water and named sectors) and the base's NEWWORLD in a scratch copy, where it
// also prints the set-up's wall time cold, warm and as main's warm start (PackedMesh, a shapes-only file read,
// prepareTriangleData), the file's size and the peak RSS.
//
//   gothic-collisioncache-cpu-contract                needs GOTHIC_DIR and a ready base for NEWWORLD
//   gothic-collisioncache-cpu-contract --world DIR    only 1 and the numbers, on a cooked world directory (×16)

#include "commandline.h"
#include "graphics/mesh/submesh/packedmesh.h"
#include "physics/collisioncache.h"
#include "physics/collisionworld.h"
#include "physics/dynamicworld.h"
#include "physics/physicvbo.h"
#include "physics/staticcollision.h"
#include "physics/worldcloth.h"
#include "physics/worldcollisionmesh.h"
#include "resources.h"
#include "staticcollision_probe.h"
#include "worldchunkcook.h"
#include "worldchunkcookedsource.h"
#include "worldchunks.h"
#include "worldcloth_fixtures.h"

#include <Jolt/Core/StreamWrapper.h>
#include <Jolt/Physics/Collision/PhysicsMaterial.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/resource.h>
#endif

namespace {
namespace fs = std::filesystem;
using namespace WorldChunkFixtures;
using Clock = std::chrono::steady_clock;
using Probe = StaticCollisionProbe;

void require(bool condition, const std::string& message) {
  if(!condition)
    throw std::runtime_error(message);
  }

double ms(Clock::time_point a, Clock::time_point b) {
  return std::chrono::duration<double,std::milli>(b-a).count();
  }

struct TemporaryDirectory {
  fs::path path = fs::temp_directory_path()/
    ("gothic-collisioncache-cpu-contract-"+std::to_string(Clock::now().time_since_epoch().count()));
  ~TemporaryDirectory() {
    std::error_code error;
    fs::remove_all(path,error);
    }
  };

struct Environment {
  std::string name;
  std::optional<std::string> previous;
  Environment(std::string name, const std::string& value):name(std::move(name)) {
    if(const auto* old=std::getenv(this->name.c_str()))
      previous=old;
    setenv(this->name.c_str(),value.c_str(),1);
    }
  ~Environment() {
    if(previous)
      setenv(name.c_str(),previous->c_str(),1);
    else
      unsetenv(name.c_str());
    }
  };

// Peak RSS of a stage: Linux resets the high-water mark through /proc/self/clear_refs; elsewhere the process peak.
struct Memory {
  static uint64_t statusKb(const char* field) {
    std::ifstream status("/proc/self/status");
    std::string line;
    while(std::getline(status,line))
      if(line.rfind(field,0)==0)
        return std::stoull(line.substr(std::strlen(field)));
    return 0;
    }
  static void resetPeak() {
    std::ofstream clear("/proc/self/clear_refs");
    if(clear)
      clear<<"5";
    }
  static double peakMb() {
    if(const auto kb=statusKb("VmHWM:"))
      return double(kb)/1024.0;
#if defined(__APPLE__)
    rusage usage {};
    getrusage(RUSAGE_SELF,&usage);
    return double(usage.ru_maxrss)/1024.0/1024.0;
#else
    return 0;
#endif
    }
  static double currentMb() { return double(statusKb("VmRSS:"))/1024.0; }
  };

// ---- loading as the game does ----

struct Loaded {
  asset::WorldMesh      landscape;
  ChunkTable            table;
  WorldCloth::Extracted cloth;
  bool                  bundle = false;
  };

Loaded load(const fs::path& dir, const fs::path& over) {
  Loaded out;
  setCookedPath(true);
  out.landscape = readWorldChunks(dir,over,&out.table);
  out.cloth = WorldCloth::extract(out.landscape,&out.table);
  out.bundle = WorldCloth::lastPath()==WorldCloth::Path::Bundle;
  return out;
  }

// One start of the world: DynamicWorld with the world load's lazy input and the given cache, the log it wrote.
struct Start {
  std::unique_ptr<WorldCollisionSource> source;
  std::unique_ptr<DynamicWorld>         physics;
  std::string                           log;
  StaticCollision::Stats                stats;
  };

Start start(const Loaded& world, const StaticCollision::Cache& cache) {
  Start out;
  LogCapture log;
  out.source  = std::make_unique<WorldCollisionSource>(world.table,world.bundle,world.landscape);
  out.physics = std::make_unique<DynamicWorld>(nullptr,std::ref(*out.source),&world.cloth.original,&cache);
  out.log     = log.take();
  out.stats   = out.physics->staticCollisionLoadStats();
  return out;
  }

StaticCollision::Cache cacheOf(const fs::path& root, const std::string& world) {
  return {root,{std::string(64,'c'),world,"collisioncache-cpu-contract",0b101u,StaticCollision::clothRules()}};
  }

std::string firstLine(const std::string& log, const std::string& tag) {
  const auto at = log.find(tag);
  return at==std::string::npos ? std::string("none") : log.substr(at,log.find('\n',at)-at);
  }

// ---- 1: three starts, one answer ----

struct Reference {
  Probe::Result result;
  };

void expectSame(const std::string& label, const Probe::Result& e, const Probe::Result& a) {
  require(Probe::sameNavigation(e.navigation,a.navigation),label+": the navigation geometry differs");
  require(e.rays.size()==a.rays.size() && e.grid==a.grid,label+": ray counts differ");
  for(size_t i=0;i<e.rays.size();++i)
    require(e.rays[i]==a.rays[i],label+": ray "+std::to_string(i)+" differs (hit, point, normal, material or sector)");
  require(e.queries.size()==a.queries.size(),label+": query counts differ");
  for(size_t i=0;i<e.queries.size();++i)
    require(e.queries[i]==a.queries[i],label+": the PhysicVbo answers for "+(e.queries[i].mesh==0 ? "land" : "water")+
            " triangle "+std::to_string(e.queries[i].triangle)+" differ");
  }

// The cold start writes the cache, the warm start reads it without calling the input; both equal the cache off.
Reference coldWarmOff(const std::string& label, const Loaded& world, const StaticCollision::Cache& cache,
                      bool chunks) {
  Reference out;
  {
  const auto off = start(world,StaticCollision::Cache{});
  require(!off.stats.cacheHit && off.source->calls()==1,label+": the cache off must build from the input");
  out.result = Probe::probe(*off.physics,world.landscape,world.table);
  }
  const CollisionCache file(cache.root,cache.key);
  std::error_code error;
  fs::remove(file.path(),error);
  {
  const auto cold = start(world,cache);
  require(!cold.stats.cacheHit && cold.source->calls()==1 && fs::is_regular_file(file.path()),
          label+": the cold start must call the input once and write "+file.path().string());
  require(tagged(cold.log,"[collision-cache] ")==0,label+": the cold start must log no cache line: "+cold.log);
  require(!chunks || tagged(cold.log,"[world-collision] chunks ")==1,
          label+": the cold start must build the collision mesh from the chunk records: "+cold.log);
  expectSame(label+", cold against the cache off",out.result,Probe::probe(*cold.physics,world.landscape,world.table));
  }
  const auto warm = start(world,cache);
  require(warm.stats.cacheHit && warm.source->calls()==0 && warm.stats.buildMs==0 && warm.stats.writeMs==0,
          label+": the warm start must read the cache without calling the input");
  require(tagged(warm.log,"[world-collision]")==0 && tagged(warm.log,"[collision-cache]")==0,
          label+": the warm start must log neither the collision input nor the cache: "+warm.log);
  require(warm.source->probe()==" input=cache input_ms=0",label+": the load mark must say input=cache, got '"+
          warm.source->probe()+"'");
  const auto result = Probe::probe(*warm.physics,world.landscape,world.table);
  expectSame(label+", warm against the cache off",out.result,result);
  size_t hits = 0;
  for(size_t i=0;i<result.grid;++i)
    hits += result.rays[i].hit;
  std::printf("%s: cache off, cold (input called once, %s) and warm (input not called, mark%s) give the same "
              "navigation (%zu+%zu triangles), %zu rays (%zu grid hits, %zu through named sectors) and %zu PhysicVbo "
              "answers (%zu on ray duplicates) over %zu land and %zu water triangles; %ju-byte file\n",
              label.c_str(),chunks ? "[world-collision] chunks" : "landscape",warm.source->probe().c_str(),
              result.navigation.triangles.size()/3,result.navigation.waterTriangles.size()/3,result.rays.size(),hits,
              result.rays.size()-result.grid,result.queries.size(),result.duplicates,result.triangles[0],result.triangles[1],
              uintmax_t(fs::file_size(file.path())));
  return out;
  }

// ---- 3: damage and staleness ----

// Offsets of the payload's sections, from the header's sizes.
struct Layout {
  static constexpr uint64_t each[12] = {1,0,12,4,24,4,4,4,24,4,4,1}; // bytes per entry; names: in bytes
  uint64_t size[12] = {}, at[12] = {};
  explicit Layout(const std::vector<uint8_t>& bytes) {
    std::memcpy(size,bytes.data()+96,sizeof(size));
    uint64_t offset = CollisionCache::HeaderSize;
    for(size_t i=0;i<12;++i) {
      at[i] = offset;
      offset += size[i]*each[i];
      }
    }
  enum { NameBytes, Names, Vertices, LandIndices, LandSegments, LandGroups, LandMembers, WaterIndices, WaterSegments,
         WaterGroups, WaterMembers, Shapes };
  };

void rechecksum(std::vector<uint8_t>& bytes) {
  const auto sum = CollisionCache::checksum(bytes.data()+CollisionCache::HeaderSize,
                                            bytes.size()-CollisionCache::HeaderSize);
  std::memcpy(bytes.data()+88,&sum,8);
  }

struct Damage {
  const Loaded&                 world;
  const StaticCollision::Cache& cache;
  const Probe::Result&          cold;
  fs::path                      file;
  std::vector<uint8_t>          good;

  // A start with this file (or key) misses with one line naming the file and the reason, rebuilds what the cold
  // start built, and the next start hits.
  void expectMiss(const std::string& label, const std::string& reason, const StaticCollision::Cache* key = nullptr) {
    const auto& use = key!=nullptr ? *key : cache;
    const auto miss = start(world,use);
    const auto line = "[collision-cache] "+file.string()+": ";
    std::printf("%s: %s\n",label.c_str(),firstLine(miss.log,"[collision-cache] ").c_str());
    require(!miss.stats.cacheHit && miss.source->calls()==1,label+": must miss and call the input once");
    require(tagged(miss.log,"[collision-cache] ")==1 && miss.log.find(line)!=std::string::npos &&
            miss.log.find(reason)!=std::string::npos &&
            miss.log.find("; rebuilding from the collision input")!=std::string::npos,
            label+": the log must hold one line '"+line+"…"+reason+"…', got: "+miss.log);
    expectSame(label+", the rebuild against the cold start",cold,Probe::probe(*miss.physics,world.landscape,world.table));
    const auto next = start(world,use);
    require(next.stats.cacheHit && next.source->calls()==0 && tagged(next.log,"[collision-cache] ")==0,
            label+": the next start must hit");
    expectSame(label+", the next start against the cold start",cold,Probe::probe(*next.physics,world.landscape,
                                                                                 world.table));
    }

  void edited(const std::string& label, const std::string& reason, const std::function<void(std::vector<uint8_t>&)>& edit) {
    auto bytes = good;
    edit(bytes);
    writeBytes(file,bytes);
    expectMiss(label,reason);
    }
  };

void damage(const Loaded& world, const StaticCollision::Cache& cache, const Probe::Result& cold) {
  const CollisionCache file(cache.root,cache.key);
  Damage d {world,cache,cold,file.path(),fileBytes(file.path())};
  const Layout layout(d.good);
  require(layout.size[Layout::WaterIndices]>0 && layout.size[Layout::Names]>2,
          "the damaged fixture must have water and named sectors");
  d.edited("a truncated file","truncated or padded: ",[](auto& bytes) { bytes.pop_back(); });
  d.edited("a flipped byte in the vertices","checksum mismatch",[&](auto& bytes) {
    auto& byte = bytes[size_t(layout.at[Layout::Vertices])+5];
    byte = uint8_t(byte^0x10u);
    });
  d.edited("a flipped byte in the shapes","checksum mismatch",[&](auto& bytes) {
    auto& byte = bytes[size_t(layout.at[Layout::Shapes]+layout.size[Layout::Shapes]/2)];
    byte = uint8_t(byte^0x10u);
    });
  d.edited("a version 1 file","format GOTHIC-COLLISION-1, this build reads GOTHIC-COLLISION-2",[&](auto& bytes) {
    // Version 1: its magic, the identity, a CRC-32 and the shapes alone.
    const std::string magic = "GOTHIC-COLLISION-1\n";
    std::vector<uint8_t> v1(magic.begin(),magic.end());
    v1.insert(v1.end(),bytes.begin()+19,bytes.begin()+19+64);
    v1.insert(v1.end(),4,uint8_t(0));
    v1.insert(v1.end(),bytes.begin()+std::ptrdiff_t(layout.at[Layout::Shapes]),bytes.end());
    bytes = v1;
    });
  d.edited("a land index out of range, checksum recomputed","land index 0 is ",[&](auto& bytes) {
    const auto vertices = uint32_t(layout.size[Layout::Vertices]);
    std::memcpy(bytes.data()+layout.at[Layout::LandIndices],&vertices,4);
    rechecksum(bytes);
    });
  d.edited("a land segment out of range, checksum recomputed","land segment 0 runs past its ",[&](auto& bytes) {
    const uint64_t triangles = layout.size[Layout::LandIndices];
    std::memcpy(bytes.data()+layout.at[Layout::LandSegments]+8,&triangles,8);
    rechecksum(bytes);
    });
  d.edited("a sector reference out of range, checksum recomputed","land segment 0 names sector ",[&](auto& bytes) {
    const auto names = uint32_t(layout.size[Layout::Names]);
    std::memcpy(bytes.data()+layout.at[Layout::LandSegments]+20,&names,4);
    rechecksum(bytes);
    });
  const char* keys[] = {"another content fingerprint","other collision flags","other cloth rules"};
  for(size_t k=0;k<3;++k) {
    writeBytes(d.file,d.good);
    auto other = cache;
    if(k==0)
      other.key.content += "-edited";
    else if(k==1)
      other.key.collisionFlags ^= 2u;
    else
      other.key.clothRules = std::string(64,'0')+"/"+std::to_string(WorldCloth::Algorithm);
    d.expectMiss(std::string("another key: ")+keys[k],"built for another key",&other);
    }
  // A leftover of an interrupted write beside a missing cache: named, removed when it is old, rebuilt.
  fs::remove(d.file);
  auto leftover = d.file;
  leftover += ".tmp-1-2";
  writeBytes(leftover,std::vector<uint8_t>(d.good.begin(),d.good.begin()+1000));
  fs::last_write_time(leftover,fs::file_time_type::clock::now()-std::chrono::hours(1));
  d.expectMiss("a leftover temporary file beside a missing cache",
               "missing; leftover temporary files of an interrupted write: "+leftover.filename().string()+" (removed)");
  require(!fs::exists(leftover),"the old leftover must be removed");
  }

// ---- 4: the cache off ----

std::map<std::string,std::pair<uintmax_t,fs::file_time_type>> listing(const fs::path& root) {
  std::map<std::string,std::pair<uintmax_t,fs::file_time_type>> out;
  std::error_code error;
  if(fs::exists(root,error))
    for(const auto& entry:fs::recursive_directory_iterator(root))
      out[entry.path().string()] = {entry.is_regular_file() ? entry.file_size() : 0,entry.last_write_time()};
  return out;
  }

void cacheOff(const Loaded& world, const StaticCollision::Cache& cache, const Probe::Result& cold) {
  Environment off("OPENGOTHIC_COLLISION_CACHE","0");
  const auto fresh = StaticCollision::Cache{cache.root.parent_path()/"never-created",cache.key};
  for(const auto* use:{&cache,&fresh}) {
    const auto before = listing(use->root);
    const auto s = start(world,*use);
    require(!s.stats.cacheHit && s.source->calls()==1 && s.stats.readMs==0 && s.stats.writeMs==0,
            "OPENGOTHIC_COLLISION_CACHE=0 must build from the input without reading or writing");
    expectSame("OPENGOTHIC_COLLISION_CACHE=0",cold,Probe::probe(*s.physics,world.landscape,world.table));
    require(listing(use->root)==before,"OPENGOTHIC_COLLISION_CACHE=0 must leave "+use->root.string()+" untouched");
    }
  require(!fs::exists(fresh.root),"OPENGOTHIC_COLLISION_CACHE=0 must not create a cache directory");
  std::printf("OPENGOTHIC_COLLISION_CACHE=0: built from the input, equal to the cold start; the cache directory with "
              "a valid file untouched, a new root not created\n");
  }

// ---- the numbers ----

// Main's warm start: PackedMesh PK_Physic into the two meshes, the shapes-only file of version 1 read and checked
// with its CRC-32 and restored, prepareTriangleData of both meshes.
struct MainWarm {
  double packMs = 0, shapesMs = 0, prepareMs = 0;
  };

std::string shapesOnly(const StaticCollision& statics) {
  std::stringstream bytes;
  JPH::StreamOutWrapper stream(bytes);
  JPH::Shape::ShapeToIDMap shapes;
  JPH::Shape::MaterialToIDMap materials;
  for(const auto* shape:{statics.landShape.GetPtr(),statics.waterShape.GetPtr()}) {
    if(shape!=nullptr)
      shape->SaveWithChildren(stream,shapes,materials);
    else
      stream.Write(~uint32_t(0));
    }
  const auto payload = bytes.str();
  const uint32_t crc = worldChunkCrc32(payload.data(),payload.size());
  std::string file = "GOTHIC-COLLISION-1\n"+std::string(64,'0');
  file.append(reinterpret_cast<const char*>(&crc),4);
  return file+payload;
  }

MainWarm mainWarm(const asset::WorldMesh& input, const fs::path& v1) {
  MainWarm out;
  auto t = Clock::now();
  PackedMesh pkg(input,PackedMesh::PK_Physic);
  std::vector<std::string> sectors(pkg.subMeshes.size());
  for(size_t i=0;i<sectors.size();++i)
    sectors[i] = pkg.subMeshes[i].material.name;
  std::vector<JPH::Vec3> vertices(pkg.vertices.size());
  for(size_t i=0;i<pkg.vertices.size();++i)
    vertices[i] = CollisionWorld::toMeters(Tempest::Vec3(pkg.vertices[i].pos[0],pkg.vertices[i].pos[1],
                                                         pkg.vertices[i].pos[2]));
  PhysicVbo land(&vertices), water(&vertices);
  for(size_t i=0;i<pkg.subMeshes.size();++i) {
    const auto& sm = pkg.subMeshes[i];
    if(!sm.material.disable_collision && sm.iboLength>0) {
      if(sm.material.group==asset::MaterialGroup::WATER)
        water.addIndex(pkg.indices,sm.iboOffset,sm.iboLength,sm.material.group);
      else
        land.addIndex(pkg.indices,sm.iboOffset,sm.iboLength,sm.material.group,sectors[i].c_str());
      }
    }
  auto now = Clock::now();
  out.packMs = ms(t,now);
  t = now;
  {
  std::ifstream file(v1,std::ios::binary);
  std::string bytes(size_t(fs::file_size(v1)),'\0');
  file.read(bytes.data(),std::streamsize(bytes.size()));
  constexpr size_t header = 19+64+4;
  uint32_t crc = 0;
  std::memcpy(&crc,bytes.data()+header-4,4);
  require(worldChunkCrc32(bytes.data()+header,bytes.size()-header)==crc,"the version 1 file must check");
  std::stringstream payload(bytes.substr(header));
  JPH::StreamInWrapper stream(payload);
  JPH::Shape::IDToShapeMap shapes;
  JPH::Shape::IDToMaterialMap materials;
  const auto a = JPH::Shape::sRestoreWithChildren(stream,shapes,materials);
  const auto b = JPH::Shape::sRestoreWithChildren(stream,shapes,materials);
  require(!a.HasError() && !b.HasError(),"the version 1 shapes must restore");
  }
  now = Clock::now();
  out.shapesMs = ms(t,now);
  t = now;
  // The first query prepares the triangle data, as main's constructor does explicitly.
  (void)land.materialId(0);
  (void)water.materialId(0);
  out.prepareMs = ms(t,Clock::now());
  return out;
  }

double median(std::vector<double> v) {
  std::sort(v.begin(),v.end());
  return v[v.size()/2];
  }

void numbers(const std::string& label, const Loaded& world, const StaticCollision::Cache& cache) {
  const CollisionCache file(cache.root,cache.key);
  std::error_code error;
  fs::remove(file.path(),error);
  WorldCollisionSource source(world.table,world.bundle,world.landscape);
  double inputMs = 0;
  const StaticCollision::Input input = [&]() -> const asset::WorldMesh& {
    const auto t = Clock::now();
    const auto& mesh = source();
    inputMs = ms(t,Clock::now());
    return mesh;
    };
  // Cold: miss, input, PackedMesh, shapes, write.
  Memory::resetPeak();
  auto base = Memory::currentMb();
  auto t = Clock::now();
  std::optional<StaticCollision> cold;
  cold.emplace(input,cache,true);
  const double coldMs = ms(t,Clock::now());
  const double coldPeak = Memory::peakMb(), coldAbove = coldPeak-base;
  require(!cold->stats.cacheHit,label+": the timed cold start must miss");
  const auto v1 = cache.root/"shapes-only.collision";
  {
  const auto bytes = shapesOnly(*cold);
  std::ofstream out(v1,std::ios::binary|std::ios::trunc);
  out.write(bytes.data(),std::streamsize(bytes.size()));
  }
  const auto stats = cold->stats;
  cold.reset();
  const auto after = fs::file_size(file.path()), before = fs::file_size(v1);
  // Warm: hit, three runs.
  std::vector<double> warm, warmRead;
  double warmPeak = 0, warmAbove = 0;
  for(int run=0;run<3;++run) {
    Memory::resetPeak();
    base = Memory::currentMb();
    t = Clock::now();
    const StaticCollision hit(input,cache,true);
    warm.push_back(ms(t,Clock::now()));
    warmRead.push_back(hit.stats.readMs);
    require(hit.stats.cacheHit && !hit.stats.inputCalled,label+": the timed warm start must hit");
    warmPeak = std::max(warmPeak,Memory::peakMb());
    warmAbove = std::max(warmAbove,Memory::peakMb()-base);
    }
  // Main's warm start, three runs, on the input it packs.
  const auto& mesh = source();
  std::vector<double> pack, shapes, prepare, total;
  double mainPeak = 0, mainAbove = 0;
  for(int run=0;run<3;++run) {
    Memory::resetPeak();
    base = Memory::currentMb();
    const auto m = mainWarm(mesh,v1);
    pack.push_back(m.packMs);
    shapes.push_back(m.shapesMs);
    prepare.push_back(m.prepareMs);
    total.push_back(m.packMs+m.shapesMs+m.prepareMs);
    mainPeak = std::max(mainPeak,Memory::peakMb());
    mainAbove = std::max(mainAbove,Memory::peakMb()-base);
    }
  fs::remove(v1);
  std::printf("%s numbers: static collision set-up, ms — cold %.1f (input %.1f, key %.1f, PackedMesh and shapes %.1f, "
              "write %.1f), warm %.1f (median of 3; file read and check %.1f), main's warm %.1f (median of 3: "
              "PackedMesh %.1f + shapes-only file %.1f + prepareTriangleData %.1f); cache file %ju bytes on main "
              "(shapes only), %ju bytes now; peak RSS, MB — cold %.0f (%.0f above its start), warm %.0f (%.0f), main's "
              "warm %.0f (%.0f)\n",
              label.c_str(),coldMs,inputMs,stats.keyMs,stats.buildMs,stats.writeMs,median(warm),median(warmRead),
              median(total),median(pack),median(shapes),median(prepare),uintmax_t(before),uintmax_t(after),coldPeak,
              coldAbove,warmPeak,warmAbove,mainPeak,mainAbove);
  {
  const auto bytes = fileBytes(file.path());
  const Layout layout(bytes);
  std::printf("%s file sections, bytes: header %zu, sector names %ju (%ju names), vertices %ju, land indices %ju, "
              "segments %ju, duplicate groups %ju (%ju members), water indices %ju, segments %ju, duplicate groups "
              "%ju (%ju members), shapes %ju\n",label.c_str(),CollisionCache::HeaderSize,
              uintmax_t(layout.size[Layout::NameBytes]),uintmax_t(layout.size[Layout::Names]),
              uintmax_t(layout.size[Layout::Vertices]*12),uintmax_t(layout.size[Layout::LandIndices]*4),
              uintmax_t(layout.size[Layout::LandSegments]*24),uintmax_t(layout.size[Layout::LandGroups]*4+
                                                                         layout.size[Layout::LandMembers]*4),
              uintmax_t(layout.size[Layout::LandMembers]),uintmax_t(layout.size[Layout::WaterIndices]*4),
              uintmax_t(layout.size[Layout::WaterSegments]*24),uintmax_t(layout.size[Layout::WaterGroups]*4+
                                                                          layout.size[Layout::WaterMembers]*4),
              uintmax_t(layout.size[Layout::WaterMembers]),uintmax_t(layout.size[Layout::Shapes]));
  }
  }

// ---- worlds ----

void fixtures(const fs::path& root) {
  size_t number = 0;
  auto check = [&](const std::string& label, const fs::path& dir, const fs::path& over) {
    const auto world = load(dir,over);
    require(world.bundle,label+": extract must take the bundle");
    coldWarmOff(label,world,cacheOf(root/("cache-"+std::to_string(++number)),"FIXTURE"),true);
    };
  const auto cloth = clothWorld();
  const auto clothDir = root/"cloth";
  writeWorldChunks(cloth.mesh,clothSource,clothDir);
  cookWorldChunks(clothDir);
  check("synthetic cloth world",clothDir,{});
  const auto moved = root/"cloth-moved", added = root/"cloth-added", removed = root/"cloth-removed";
  writeMovedClothCell(moved,cloth);
  writeAddedClothCell(added,cloth);
  writeRemovedClothCell(removed,cloth);
  const std::pair<const char*,fs::path> overrides[] = {{"cloth world, override: a moved cloth vertex",moved},
    {"cloth world, override: a new cloth island",added},{"cloth world, override: a cell without the ship flag",removed}};
  for(const auto& [label,over]:overrides) {
    cookWorldChunks(clothDir,over);
    check(label,clothDir,over);
    }

  const auto base = stripMesh();
  const auto strip = root/"strip";
  writeWorldChunks(base,stripSource,strip);
  cookWorldChunks(strip);
  check("strip",strip,{});
  const auto border = writeStripOverride(root/"strip-border",base,true);
  cookWorldChunks(strip,border);
  check("strip with the border override",strip,border);
  const auto second = writeStripOverride(root/"strip-second",base,false,{2,0});
  cookWorldChunks(strip,second);
  check("strip with the override of cell 2,0",strip,second);

  // The world with disable_collision, water and two named sectors: also damage, staleness and the cache off.
  const auto collision = root/"collision";
  writeWorldChunks(collisionWorld(),collisionSource,collision);
  cookWorldChunks(collision);
  const auto world = load(collision,{});
  require(world.bundle,"the collision fixture: extract must take the bundle");
  const auto cache = cacheOf(root/"cache-collision","COLLISION");
  const auto cold = coldWarmOff("disable_collision, water and two named sectors",world,cache,true);
  damage(world,cache,cold.result);
  cacheOff(world,cache,cold.result);
  }

void bigWorld(const std::string& label, const fs::path& dir, const fs::path& root) {
  auto started = Clock::now();
  const auto world = load(dir,{});
  std::printf("%s loaded from %s in %.1f s (%zu triangles after cloth, %s)\n",label.c_str(),dir.string().c_str(),
              ms(started,Clock::now())/1000.0,world.landscape.polygons.material_indices.size(),
              world.bundle ? "cloth bundle" : "island search");
  const auto cache = cacheOf(root/"cache-big",fs::path(dir).filename().string());
  const auto reference = coldWarmOff(label,world,cache,world.bundle);
  require(reference.result.duplicates>0,label+": the probe must reach triangles with ray duplicates");
  size_t sectors = 0;
  for(size_t i=reference.result.grid;i<reference.result.rays.size();++i)
    sectors += !reference.result.rays[i].sector.empty();
  require(sectors>0,label+": the probe must hit named sectors");
  numbers(label,world,cache);
  }

}

int main(int argc, char* argv[]) {
  try {
    if(argc!=1 && !(argc==3 && std::string(argv[1])=="--world"))
      throw std::runtime_error("usage: gothic-collisioncache-cpu-contract [--world DIR]");
    Environment enabled("OPENGOTHIC_COLLISION_CACHE","1");
    TemporaryDirectory temporary;
    fs::create_directories(temporary.path);
    if(argc==3) {
      bigWorld(argv[2],argv[2],temporary.path);
      std::puts("Collision cache CPU data contract passed (one world)");
      return EXIT_SUCCESS;
      }
    fixtures(temporary.path);

    const auto* gothic = std::getenv("GOTHIC_DIR");
    require(gothic!=nullptr && *gothic!='\0',"GOTHIC_DIR must point to a Gothic II installation");
    const char* args[] = {"gothic-collisioncache-cpu-contract","-g",gothic};
    CommandLine commandLine(3,args);
    Resources resources;
    Resources::loadBase();
    const auto base = Resources::worldBaseDir("NEWWORLD.ZEN");
    require(!base.empty() && fs::exists(base/"world.json"),"the base has no NEWWORLD");
    const auto scratch = temporary.path/"NEWWORLD";
    const auto inBase = cookedFiles(base);
    linkWorld(base,scratch);
    cookWorldChunks(scratch);
    require(cookedFiles(base)==inBase,"the base must not be written");
    bigWorld("base NEWWORLD",scratch,temporary.path);
    std::puts("Collision cache CPU data contract passed");
    return EXIT_SUCCESS;
    }
  catch(const std::exception& error) {
    std::fprintf(stderr,"FAIL: %s\n",error.what());
    return EXIT_FAILURE;
    }
  }
