// Static collision's input from the cooked chunk records (docs/landscape-plan.md, Д3, task 5;
// physics/worldcollisionmesh.h). Each world is loaded as the game loads it — readWorldChunks with the cooked set,
// then WorldCloth::extract — and the collision mesh the builder makes from the records equals the landscape in all
// PackedMesh PK_Physic reads (vertices bit for bit, vertex and material indices, every material's name, group and
// disable_collision); PackedMesh PK_Physic of both is identical, and a second build gives the same arrays:
//   - the synthetic cloth world with its three override sets (a moved cloth vertex, an added island, a removed one);
//   - the three-cell strip with the border override (ids renumbered across cells) and the override of cell 2,0 that
//     appends a material;
//   - a landscape with a material with disable_collision, water and two named sector materials over several cells;
//   - the base's NEWWORLD in a scratch copy of hard links, where two DynamicWorlds (stitched landscape, collision
//     mesh) also give byte-identical navigation geometry and the same answer to a grid of vertical rays.
// Every fallback leaves the stitched landscape in use with its log line.
//   gothic-worldcollision-chunks-contract               the contract; needs GOTHIC_DIR and a ready base for NEWWORLD
//   gothic-worldcollision-chunks-contract --time <dir>  times the builder and PackedMesh PK_Physic on both inputs of
//                                                       a world cooked beside its GLBs (5 runs, warm page cache;
//                                                       content-convert time-world cannot: it links no graphics code)

#include "commandline.h"
#include "graphics/mesh/submesh/packedmesh.h"
#include "physics/collisionworld.h"
#include "physics/dynamicworld.h"
#include "physics/worldcloth.h"
#include "physics/worldclothbundle.h"
#include "physics/worldcollisionmesh.h"
#include "resources.h"
#include "worldchunkcook.h"
#include "worldchunkcookedsource.h"
#include "worldchunks.h"
#include "worldcloth_fixtures.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
namespace fs = std::filesystem;
using namespace WorldChunkFixtures;
using Clock = std::chrono::steady_clock;

void require(bool condition, const std::string& message) {
  if(!condition)
    throw std::runtime_error(message);
  }

struct TemporaryDirectory {
  fs::path path = fs::temp_directory_path()/
    ("gothic-worldcollision-chunks-contract-"+std::to_string(Clock::now().time_since_epoch().count()));
  ~TemporaryDirectory() {
    std::error_code error;
    fs::remove_all(path,error);
    }
  };

const WorldChunkSource stripSource     = {"CONTRACT.ZEN",3,0x00C0FFEEu};
const WorldChunkSource collisionSource = {"COLLISION_CONTRACT.ZEN",13,0x0C0111DEu};

uint32_t bits(float v) { return std::bit_cast<uint32_t>(v); }

double ms(Clock::time_point a, Clock::time_point b) {
  return std::chrono::duration<double,std::milli>(b-a).count();
  }

// ---- loading as the game does ----

struct Loaded {
  asset::WorldMesh      landscape;
  ChunkTable            table;
  WorldCloth::Extracted cloth;
  WorldCloth::Path      path = WorldCloth::Path::None;
  WorldCollisionInput   input;
  std::string           log;
  };

// World's constructor: readWorldChunks with the cooked set, extract, then the collision input.
Loaded load(const fs::path& dir, const fs::path& over, bool cooked=true) {
  Loaded out;
  LogCapture log;
  setCookedPath(cooked);
  try {
    out.landscape = readWorldChunks(dir,over,&out.table);
    }
  catch(...) {
    setCookedPath(true);
    throw;
    }
  setCookedPath(true);
  out.cloth = WorldCloth::extract(out.landscape,&out.table);
  out.path  = WorldCloth::lastPath();
  out.input = worldCollisionInput(out.table,out.path==WorldCloth::Path::Bundle);
  out.log   = log.take();
  return out;
  }

// Empty when the collision mesh holds what PackedMesh PK_Physic reads of the landscape, and nothing else.
std::string compareInput(const asset::WorldMesh& e, const asset::WorldMesh& a) {
  if(e.vertices.size()!=a.vertices.size())
    return "vertices: "+std::to_string(e.vertices.size())+" vs "+std::to_string(a.vertices.size());
  for(size_t i=0;i<e.vertices.size();++i)
    if(bits(e.vertices[i].x)!=bits(a.vertices[i].x) || bits(e.vertices[i].y)!=bits(a.vertices[i].y) ||
       bits(e.vertices[i].z)!=bits(a.vertices[i].z))
      return "vertex "+std::to_string(i)+" differs";
  const auto& p = e.polygons;
  const auto& q = a.polygons;
  if(p.material_indices.size()!=q.material_indices.size() || p.vertex_indices.size()!=q.vertex_indices.size())
    return "triangles: "+std::to_string(p.material_indices.size())+" vs "+std::to_string(q.material_indices.size());
  for(size_t i=0;i<p.vertex_indices.size();++i)
    if(p.vertex_indices[i]!=q.vertex_indices[i])
      return "corner "+std::to_string(i)+" uses vertex "+std::to_string(p.vertex_indices[i])+" vs "+
             std::to_string(q.vertex_indices[i]);
  for(size_t t=0;t<p.material_indices.size();++t)
    if(p.material_indices[t]!=q.material_indices[t])
      return "triangle "+std::to_string(t)+" has material "+std::to_string(p.material_indices[t])+" vs "+
             std::to_string(q.material_indices[t]);
  if(e.materials.size()!=a.materials.size())
    return "materials: "+std::to_string(e.materials.size())+" vs "+std::to_string(a.materials.size());
  for(size_t m=0;m<e.materials.size();++m) {
    const auto& x = e.materials[m];
    const auto& y = a.materials[m];
    if(x.name!=y.name || x.group!=y.group || x.disable_collision!=y.disable_collision || !gothicglb::sameMaterial(x,y))
      return "material "+std::to_string(m)+" ("+x.name+") differs";
    }
  if(!a.features.empty() || !q.feature_indices.empty() || !q.lightmap_indices.empty() || !q.flags.empty())
    return "the collision mesh carries features, feature indices, lightmaps or flags";
  return {};
  }

struct Packed {
  std::string difference;
  size_t      vertices = 0, indices = 0, subMeshes = 0;
  };

// PackedMesh PK_Physic of both: vertices, indices, and every sub mesh's material name, group, offset and length.
Packed comparePacked(const asset::WorldMesh& landscape, const asset::WorldMesh& collision) {
  const PackedMesh e(landscape,PackedMesh::PK_Physic), a(collision,PackedMesh::PK_Physic);
  Packed out {{},e.vertices.size(),e.indices.size(),e.subMeshes.size()};
  if(e.vertices.size()!=a.vertices.size() ||
     (!e.vertices.empty() && std::memcmp(e.vertices.data(),a.vertices.data(),e.vertices.size()*sizeof(e.vertices[0]))!=0))
    out.difference = "packed vertices differ ("+std::to_string(e.vertices.size())+" vs "+
                     std::to_string(a.vertices.size())+")";
  else if(e.indices!=a.indices)
    out.difference = "packed indices differ";
  else if(e.subMeshes.size()!=a.subMeshes.size())
    out.difference = std::to_string(e.subMeshes.size())+" vs "+std::to_string(a.subMeshes.size())+" sub meshes";
  for(size_t i=0;out.difference.empty() && i<e.subMeshes.size();++i) {
    const auto& x = e.subMeshes[i];
    const auto& y = a.subMeshes[i];
    if(x.material.name!=y.material.name || x.material.group!=y.material.group || x.iboOffset!=y.iboOffset ||
       x.iboLength!=y.iboLength)
      out.difference = "sub mesh "+std::to_string(i)+" ('"+x.material.name+"') differs";
    }
  return out;
  }

// The game's load of dir with over gives the collision mesh, equal to the landscape after extract.
Loaded expectEqual(const std::string& label, const fs::path& dir, const fs::path& over={}) {
  auto loaded = load(dir,over);
  require(loaded.path==WorldCloth::Path::Bundle,label+": extract must take the bundle; log: "+loaded.log);
  require(loaded.input.mesh.has_value(),label+": the builder must give the collision mesh; log: "+loaded.log);
  const auto& mesh = *loaded.input.mesh;
  const auto line = "[world-collision] chunks "+cookedIndexPath(dir,over).string()+": "+
                    std::to_string(mesh.polygons.material_indices.size())+" triangles, "+
                    std::to_string(mesh.vertices.size())+" vertices, ";
  require(tagged(loaded.log,"[world-collision] ")==1 && loaded.log.find(line)!=std::string::npos,
          label+": the log must hold the line '"+line+"…' alone, got: "+loaded.log);
  auto difference = compareInput(loaded.landscape,mesh);
  require(difference.empty(),label+": the collision mesh differs from the landscape: "+difference);
  const auto packed = comparePacked(loaded.landscape,mesh);
  require(packed.difference.empty(),label+": PackedMesh PK_Physic differs: "+packed.difference);
  // Determinism: a second build of the same load gives the same arrays.
  LogCapture quiet;
  const auto again = worldCollisionInput(loaded.table,true);
  require(again.mesh.has_value(),label+": a second build must give the collision mesh too");
  difference = compareInput(mesh,*again.mesh);
  require(difference.empty(),label+": a second build differs from the first: "+difference);
  std::printf("%s: %zu triangles (%zu taken out as cloth), %zu vertices, %zu materials equal the landscape; "
              "PK_Physic %zu vertices, %zu indices, %zu sub meshes identical; a second build equal; %.1f ms\n",
              label.c_str(),mesh.polygons.material_indices.size(),loaded.cloth.original.polygons.material_indices.size(),
              mesh.vertices.size(),mesh.materials.size(),packed.vertices,packed.indices,packed.subMeshes,
              loaded.input.ms);
  return loaded;
  }

// ---- fixtures ----

void clothFixtures(const fs::path& root) {
  const auto world = clothWorld();
  const auto dir = root/"cloth";
  writeWorldChunks(world.mesh,clothSource,dir);
  cookWorldChunks(dir);
  const auto plain = expectEqual("synthetic cloth world",dir);
  require(plain.cloth.original.polygons.material_indices.size()==88,"the cloth world takes 88 triangles out as cloth");
  const auto moved = root/"cloth-moved", added = root/"cloth-added", removed = root/"cloth-removed";
  writeMovedClothCell(moved,world);
  writeAddedClothCell(added,world);
  writeRemovedClothCell(removed,world);
  const std::pair<const char*,fs::path> overrides[] = {{"override: cell 0,0 with a moved cloth vertex",moved},
    {"override: a new cloth island as added geometry in cell 2,0",added},{"override: cell 0,0 without the ship flag",removed}};
  for(const auto& [label,over]:overrides) {
    cookWorldChunks(dir,over);
    expectEqual(label,dir,over);
    }
  }

void stripFixtures(const fs::path& root) {
  const auto base = stripMesh();
  const auto strip = root/"strip";
  writeWorldChunks(base,stripSource,strip);
  cookWorldChunks(strip);
  expectEqual("strip",strip);
  // The border override: cell 1,0's copy of the vertex it shares with 0,0 moves, so 0,0's copy gets a new id.
  const auto border = writeStripOverride(root/"strip-border",base,true);
  cookWorldChunks(strip,border);
  {
  const auto index = CookedIndex::read(cookedIndexPath(strip,border));
  const auto cells = readWorldChunkManifest(strip).cells;
  size_t renumbered = 0;
  for(size_t i=0;i<cells.size();++i)
    if(cells[i].id==ChunkId{0,0})
      renumbered = index.vertexExceptions(i).size();
  require(renumbered>0 && index.appendedMaterials()==1,
          "the border override must renumber a vertex of cell 0,0 and append its material");
  }
  expectEqual("strip with the border override (a vertex of 0,0 renumbered, a material appended)",strip,border);
  const auto second = writeStripOverride(root/"strip-second",base,false,{2,0});
  cookWorldChunks(strip,second);
  require(CookedIndex::read(cookedIndexPath(strip,second)).appendedMaterials()==1,
          "the override of cell 2,0 must append its material");
  expectEqual("strip with the override of cell 2,0 (a material appended)",strip,second);
  }

// 8 x 4 quads of 32 m over cells -1..2 x 0..1, each of one of five materials in turn — ground, a material with
// disable_collision, water and two named sector materials (a ':' in the name, the same group) — so each lies in
// several cells; a city banner across the border of cells 0,0 and 1,0 goes to cloth; one position is unused.
asset::WorldMesh collisionWorld() {
  asset::WorldMesh mesh;
  const uint32_t materials[5] = {addMaterial(mesh,"COLLISION_GROUND","GROUND.TGA"),
    addMaterial(mesh,"COLLISION_FOLIAGE","FOLIAGE.TGA"),addMaterial(mesh,"COLLISION_LAKE","LAKE.TGA"),
    addMaterial(mesh,"SECTOR:HALL_A","HALL_A.TGA"),addMaterial(mesh,"SECTOR:HALL_B","HALL_B.TGA")};
  mesh.materials[materials[0]].group = asset::MaterialGroup::EARTH;
  mesh.materials[materials[1]].disable_collision = true;
  mesh.materials[materials[2]].group = asset::MaterialGroup::WATER;
  mesh.materials[materials[3]].group = asset::MaterialGroup::STONE;
  mesh.materials[materials[4]].group = asset::MaterialGroup::STONE;
  const auto base = uint32_t(mesh.vertices.size());
  for(uint32_t row=0;row<=4;++row)
    for(uint32_t col=0;col<=8;++col)
      mesh.vertices.push_back({-6400.f+float(col)*3200.f,float(row*7+col*3),float(row)*3200.f});
  for(uint32_t row=0;row<4;++row)
    for(uint32_t col=0;col<8;++col) {
      const auto material = materials[(row+col)%5];
      const auto a = base+row*9+col, b = a+1, c = a+9, d = c+1;
      addTriangle(mesh,material,{a,b,d});
      addTriangle(mesh,material,{a,d,c});
      }
  addBanner(mesh,addMaterial(mesh,"NW_CITY_FLAG_01","SYNTHETIC_CITY.TGA"),6300);
  mesh.vertices.push_back({1.f,2.f,3.f}); // referenced by nothing: unreferenced.bin
  return mesh;
  }

void collisionFixture(const fs::path& root) {
  const auto mesh = collisionWorld();
  const auto dir = root/"collision";
  writeWorldChunks(mesh,collisionSource,dir);
  cookWorldChunks(dir);
  const auto loaded = expectEqual("disable_collision, water and two named sector materials over 8 cells",dir);
  // Each special material lies in several cells, and each packs into its own kind of sub mesh.
  for(const auto* name:{"COLLISION_FOLIAGE","COLLISION_LAKE","SECTOR:HALL_A","SECTOR:HALL_B"}) {
    size_t material = 0;
    while(material<loaded.landscape.materials.size() && loaded.landscape.materials[material].name!=name)
      ++material;
    std::vector<uint32_t> cells;
    for(size_t t=0;t<loaded.landscape.polygons.material_indices.size();++t)
      if(loaded.landscape.polygons.material_indices[t]==material &&
         std::find(cells.begin(),cells.end(),loaded.table.triangleChunks[t])==cells.end())
        cells.push_back(loaded.table.triangleChunks[t]);
    require(cells.size()>=2,std::string(name)+" must lie in several cells, it lies in "+std::to_string(cells.size()));
    }
  const auto& collision = *loaded.input.mesh;
  size_t solid = 0;
  for(const auto material:collision.polygons.material_indices)
    solid += !collision.materials[material].disable_collision;
  const PackedMesh packed(collision,PackedMesh::PK_Physic);
  size_t water = 0, sectors = 0;
  for(const auto& sub:packed.subMeshes) {
    water += sub.material.group==asset::MaterialGroup::WATER;
    sectors += sub.material.name.find(':')!=std::string::npos;
    }
  require(solid<collision.polygons.material_indices.size() && packed.indices.size()==solid*3,
          "disable_collision must keep its triangles out of the package");
  require(water==1 && sectors==2,"the package must hold one water sub mesh and the two named sectors apart");
  }

// ---- fallbacks: each case in a fresh copy of the cooked cloth world ----

void expectLandscape(const std::string& label, const fs::path& dir, const fs::path& file, const std::string& reason,
                     bool cooked=true) {
  const auto loaded = load(dir,{},cooked);
  const auto lines = tagged(loaded.log,"[world-collision] ");
  const auto at = loaded.log.find("[world-collision] ");
  std::printf("%s: the stitched landscape; %s\n",label.c_str(),at==std::string::npos ? "no [world-collision] line" :
              loaded.log.substr(at,loaded.log.find('\n',at)-at).c_str());
  require(!loaded.input.mesh,label+": the stitched landscape must give the collision; log: "+loaded.log);
  if(reason.empty()) {
    require(lines==0,label+": nothing must be logged, got: "+loaded.log);
    return;
    }
  const auto head = "[world-collision] "+file.string()+": ";
  require(lines==1 && loaded.log.find(head)!=std::string::npos && loaded.log.find(reason)!=std::string::npos &&
          loaded.log.find("; the collision comes from the stitched landscape")!=std::string::npos,
          label+": the log must hold one line '"+head+"…"+reason+"…', got: "+loaded.log);
  }

void fallbacks(const fs::path& root) {
  const auto world = clothWorld();
  const auto cooked = root/"fallback-cooked";
  writeWorldChunks(world.mesh,clothSource,cooked);
  cookWorldChunks(cooked);
  size_t number = 0;
  auto copy = [&] {
    const auto dir = root/("fallback-"+std::to_string(++number));
    fs::copy(cooked,dir,fs::copy_options::recursive);
    return dir;
    };

  auto dir = copy();
  expectLandscape("OPENGOTHIC_WCB=0",dir,{},{},false);

  dir = copy();
  fs::remove(dir/wci::FileName);
  expectLandscape("no index",dir,{},{});

  dir = copy();
  fs::remove(dir/wcl::FileName);
  expectLandscape("no world cloth bundle",dir,dir/wcl::FileName,"the world cloth bundle is missing");

  dir = copy();
  auto bytes = fileBytes(dir/wcl::FileName);
  bytes[80] = uint8_t(bytes[80]^1u); // the rules' SHA-256
  writeBytes(dir/wcl::FileName,bytes);
  expectLandscape("a bundle of other rules",dir,dir/wcl::FileName,
                  "was cooked with other world cloth rules than this build's");

  dir = copy();
  fs::remove(dir/"chunks/2_0.wcb");
  expectLandscape("one .wcb missing",dir,dir/"chunks/2_0.wcb","the cell's .wcb is missing");

  dir = copy();
  bytes = fileBytes(dir/"chunks/2_0.wcb");
  bytes.pop_back();
  writeBytes(dir/"chunks/2_0.wcb",bytes);
  expectLandscape("one .wcb truncated",dir,dir/"chunks/2_0.wcb","truncated: ");

  dir = copy();
  fs::copy_file(dir/"chunks/1_0.wcb",dir/"chunks/2_0.wcb",fs::copy_options::overwrite_existing);
  expectLandscape("the .wcb of another cell copied over",dir,dir/"chunks/2_0.wcb",
                  "2_0.wcb was cooked for cell 1,0, world.json lists it for cell 2,0");

  dir = copy();
  writeBytes(dir/"chunks/2_0.glb",padJson(fileBytes(dir/"chunks/2_0.glb")));
  expectLandscape("a GLB of another length at load",dir,dir/wci::FileName,
                  "bytes at world load, the index lists the cell with a GLB of");
  }

}

// ---- the base's NEWWORLD: the same bodies from both inputs ----

// A friend of DynamicWorld: the rays go straight to its collision world with one category mask each.
struct WorldCollisionChunksContract {
  struct Ray {
    bool                 hit = false;
    uint32_t             point[3] = {}, normal[3] = {};
    asset::MaterialGroup material = asset::MaterialGroup::UNDEFINED;
    std::string          sector;
    bool operator==(const Ray&) const = default;
    };
  struct Result {
    NavMesh::Geometry navigation;
    std::vector<Ray>  rays;
    size_t            grid = 0; // the grid's rays come first
    double            buildMs = 0;
    };

  static Result probe(const asset::WorldMesh& mesh, const asset::WorldMesh& original, const ChunkTable& table) {
    Result out;
    const auto started = Clock::now();
    const DynamicWorld physics(nullptr,mesh,&original);
    out.buildMs = ms(started,Clock::now());
    out.navigation = physics.navigationGeometry();
    const float infinity = std::numeric_limits<float>::infinity();
    asset::Vec3 lo {infinity,infinity,infinity}, hi {-infinity,-infinity,-infinity};
    for(const auto& entry:table.entries)
      if(entry.triangleCount!=0)
        for(unsigned axis=0;axis<3;++axis) {
          lo[axis] = std::min(lo[axis],entry.min[axis]);
          hi[axis] = std::max(hi[axis],entry.max[axis]);
          }
    require(lo.x<hi.x && lo.z<hi.z,"the ray grid needs the island's bounds");
    auto cast = [&](const Tempest::Vec3& from, const Tempest::Vec3& to, uint32_t mask) {
      const auto hit = physics.world->rayCast(from,to,mask);
      Ray ray;
      ray.hit = hit.hasHit;
      const float point[3] = {hit.point.x,hit.point.y,hit.point.z}, normal[3] = {hit.normal.x,hit.normal.y,hit.normal.z};
      for(unsigned axis=0;axis<3;++axis) {
        ray.point[axis]  = bits(point[axis]);
        ray.normal[axis] = bits(normal[axis]);
        }
      ray.material = hit.material;
      ray.sector   = hit.sector==nullptr ? "" : hit.sector;
      out.rays.push_back(std::move(ray));
      };
    // The grid: land and water rays in turn.
    constexpr int side = 64;
    for(int i=0;i<side;++i)
      for(int k=0;k<side;++k) {
        const float x = lo.x+(hi.x-lo.x)*(float(i)+.5f)/float(side), z = lo.z+(hi.z-lo.z)*(float(k)+.5f)/float(side);
        cast({x,hi.y+1000.f,z},{x,lo.y-1000.f,z},1u<<DynamicWorld::C_Landscape);
        cast({x,hi.y+1000.f,z},{x,lo.y-1000.f,z},1u<<DynamicWorld::C_Water);
        }
    // Then short vertical land rays through every 16th roughly level triangle of a named sector, from the same input.
    out.grid = out.rays.size();
    const auto& p = mesh.polygons;
    for(size_t t=0,named=0;t<p.material_indices.size();++t) {
      const auto& material = mesh.materials[p.material_indices[t]];
      if(material.disable_collision || material.name.find(':')==std::string::npos || named++%16!=0)
        continue;
      Tempest::Vec3 v[3];
      for(unsigned k=0;k<3;++k) {
        const auto& x = mesh.vertices[p.vertex_indices[t*3+k]];
        v[k] = {x.x,x.y,x.z};
        }
      const auto n = Tempest::Vec3::crossProduct(v[1]-v[0],v[2]-v[0]);
      if(std::abs(n.y)*2.f<n.length())
        continue;
      const auto c = (v[0]+v[1]+v[2])/3.f;
      cast(c+Tempest::Vec3(0,10,0),c-Tempest::Vec3(0,10,0),1u<<DynamicWorld::C_Landscape);
      }
    return out;
    }

  static bool sameBytes(const auto& a, const auto& b) {
    return a.size()==b.size() && (a.empty() || std::memcmp(a.data(),b.data(),a.size()*sizeof(a[0]))==0);
    }

  static void compare(const Loaded& loaded) {
    const auto stitched = probe(loaded.landscape,loaded.cloth.original,loaded.table);
    const auto chunks   = probe(*loaded.input.mesh,loaded.cloth.original,loaded.table);
    const auto& a = stitched.navigation;
    const auto& b = chunks.navigation;
    require(sameBytes(a.vertices,b.vertices) && sameBytes(a.triangles,b.triangles) &&
            sameBytes(a.waterVertices,b.waterVertices) && sameBytes(a.waterTriangles,b.waterTriangles),
            "NEWWORLD: the navigation geometry of the two DynamicWorlds differs");
    require(stitched.rays.size()==chunks.rays.size() && stitched.grid==chunks.grid,"NEWWORLD: ray counts differ");
    size_t hits[2] = {}, sectors = 0;
    for(size_t i=0;i<stitched.rays.size();++i) {
      require(stitched.rays[i]==chunks.rays[i],"NEWWORLD: ray "+std::to_string(i)+" differs (hit, point, normal, "
              "material or sector)");
      if(i<stitched.grid)
        hits[i%2] += stitched.rays[i].hit;
      sectors += !stitched.rays[i].sector.empty();
      }
    require(hits[0]>0 && hits[1]>0 && stitched.grid>=4000 && sectors>0,
            "NEWWORLD: the rays must hit land, water and named sectors");
    std::printf("NEWWORLD: two DynamicWorlds (%.0f ms from the stitched landscape, %.0f ms from the collision mesh): "
                "navigation %zu+%zu triangles byte-identical; %zu vertical grid rays (%zu land, %zu water hits) and "
                "%zu short rays through named sector triangles identical, %zu rays in a named sector\n",
                stitched.buildMs,chunks.buildMs,a.triangles.size()/3,a.waterTriangles.size()/3,stitched.grid,hits[0],
                hits[1],stitched.rays.size()-stitched.grid,sectors);
    }

  static void newWorld(const fs::path& root, const fs::path& world) {
    const auto scratch = root/"NEWWORLD";
    const auto inBase = cookedFiles(world);
    linkWorld(world,scratch);
    const auto started = Clock::now();
    const auto stats = cookWorldChunks(scratch);
    std::printf("NEWWORLD cooked beside its GLBs in %s: %zu cells, %.1f s\n",scratch.string().c_str(),stats.cells,
                ms(started,Clock::now())/1000.0);
    require(cookedFiles(world)==inBase,"the base must not be written");
    const auto loaded = expectEqual("base NEWWORLD",scratch);
    compare(loaded);
    }
  };

namespace {

// ---- --time <world dir> ----

void printSeries(const char* name, std::vector<double> v) {
  std::sort(v.begin(),v.end());
  std::printf("| %-48s | %10.1f | %10.1f | %10.1f |\n",name,v[v.size()/2],v.front(),v.back());
  }

int timeWorld(const fs::path& dir) {
  constexpr int runs = 5;
  const auto loaded = load(dir,{});
  require(loaded.path==WorldCloth::Path::Bundle && loaded.input.mesh.has_value(),
          dir.string()+" must be cooked beside its GLBs with its bundle; log: "+loaded.log);
  const auto& mesh = *loaded.input.mesh;
  auto difference = compareInput(loaded.landscape,mesh);
  if(difference.empty())
    difference = comparePacked(loaded.landscape,mesh).difference;
  require(difference.empty(),"the collision mesh differs from the landscape: "+difference);
  std::vector<double> build, landscape, collision;
  size_t subMeshes = 0;
  {
  LogCapture quiet;
  for(int run=0;run<runs;++run) {
    const auto input = worldCollisionInput(loaded.table,true);
    require(input.mesh.has_value(),"a timed build gave no collision mesh");
    build.push_back(input.ms);
    // Each package alone, the order turning every run: the second of two live packages would pay for the memory.
    auto pack = [&](const asset::WorldMesh& mesh, std::vector<double>& out) {
      const auto t0 = Clock::now();
      const PackedMesh packed(mesh,PackedMesh::PK_Physic);
      out.push_back(ms(t0,Clock::now()));
      subMeshes = packed.subMeshes.size();
      };
    if(run%2==0) {
      pack(loaded.landscape,landscape);
      pack(*input.mesh,collision);
      }
    else {
      pack(*input.mesh,collision);
      pack(loaded.landscape,landscape);
      }
    }
  }
  std::printf("world %s: %zu triangles (%zu taken out as cloth), %zu vertices, %zu materials; PK_Physic %zu sub "
              "meshes, identical from both inputs\nwall ms over %d runs, warm page cache:\n"
              "| %-48s | %10s | %10s | %10s |\n|---|---:|---:|---:|\n",dir.string().c_str(),
              mesh.polygons.material_indices.size(),loaded.cloth.original.polygons.material_indices.size(),
              mesh.vertices.size(),mesh.materials.size(),subMeshes,runs,"stage","median","min","max");
  printSeries("collision mesh from chunk records (read + build)",build);
  printSeries("PackedMesh PK_Physic of the stitched landscape",landscape);
  printSeries("PackedMesh PK_Physic of the collision mesh",collision);
  return EXIT_SUCCESS;
  }

}

int main(int argc, char* argv[]) {
  try {
    if(argc==3 && std::string_view(argv[1])=="--time")
      return timeWorld(argv[2]);
    require(argc==1,"usage: gothic-worldcollision-chunks-contract [--time <world dir>]");
    TemporaryDirectory temporary;
    fs::create_directories(temporary.path);
    clothFixtures(temporary.path);
    stripFixtures(temporary.path);
    collisionFixture(temporary.path);
    fallbacks(temporary.path);

    const auto* gothic = std::getenv("GOTHIC_DIR");
    require(gothic!=nullptr && *gothic!='\0',"GOTHIC_DIR must point to a Gothic II installation");
    const char* args[] = {"gothic-worldcollision-chunks-contract","-g",gothic};
    CommandLine commandLine(3,args);
    Resources resources;
    Resources::loadBase();
    const auto base = Resources::worldBaseDir("NEWWORLD.ZEN");
    require(!base.empty() && fs::exists(base/"world.json"),"the base has no NEWWORLD");
    WorldCollisionChunksContract::newWorld(temporary.path,base);
    std::puts("World collision from chunk records contract passed");
    return EXIT_SUCCESS;
    }
  catch(const std::exception& error) {
    std::fprintf(stderr,"FAIL: %s\n",error.what());
    return EXIT_FAILURE;
    }
  }
