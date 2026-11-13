// The world cloth bundle (docs/landscape-plan.md, Д3, task 4; physics/worldclothbundle.h): WorldCloth::extract on
// a world cooked beside its GLBs takes the cooked bundle and gives what the island search gives with
// OPENGOTHIC_WCB=0 — every surface, the original mesh, the landscape and the chunk table after the removal.
//   - The synthetic cloth world (the objects of worldcloth_contract.cpp as chunks: an island across a cell
//     border, a cell with several islands, cells without cloth), three override sets (a moved cloth vertex, a new
//     island of added geometry, a removed island) and the base's NEWWORLD in a scratch copy of hard links.
//   - Every fallback gives the island search's result and its log line.
//   - The cook: a second cook writes nothing, an override writes its cells, the bundle and the index, two cooks
//     at once both finish and verify; verify-cooked-world names the surface of a changed pin.
//   - One cook per directory: a cook stopped before its index holds back a second cook of the same directory
//     until it ends, and not a cook of another directory (closes #1284).
//   - Carried over from the review of task 3: a .wcb of another source world falls back alone.
//   - A digest of searchIslands' result on the fixture guards WorldCloth::Algorithm.
// Needs GOTHIC_DIR and a ready base for NEWWORLD.

#include "commandline.h"
#include "gothicglb.h"
#include "physics/worldcloth.h"
#include "physics/worldclothbundle.h"
#include "resources.h"
#include "sha256.h"
#include "worldchunkcook.h"
#include "worldchunk_fixtures.h"
#include "worldchunk_strip.h"
#include "worldchunks.h"

#include <Tempest/Log>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {
namespace fs = std::filesystem;
using namespace WorldChunkFixtures;
using Clock = std::chrono::steady_clock;

// SHA-256 of searchIslands' result on clothWorld() (the Digest below). Update it together with WorldCloth::Algorithm.
constexpr const char* resultDigest = "afc9f4cda11448f5e652611eac3da08b1170fefb09c8d8c07676569fab0b62bb";

void require(bool condition, const std::string& message) {
  if(!condition)
    throw std::runtime_error(message);
  }

struct TemporaryDirectory {
  fs::path path = fs::temp_directory_path()/
    ("gothic-worldcloth-cooked-contract-"+std::to_string(Clock::now().time_since_epoch().count()));
  ~TemporaryDirectory() {
    std::error_code error;
    fs::remove_all(path,error);
    }
  };

const WorldChunkSource clothSource = {"CLOTH_CONTRACT.ZEN",7,0x0C107FEEu};
const WorldChunkSource stripSource = {"CONTRACT.ZEN",3,0x00C0FFEEu};

void setCookedPath(bool on) {
#ifdef _WIN32
  _putenv_s("OPENGOTHIC_WCB",on ? "" : "0");
#else
  if(on)
    unsetenv("OPENGOTHIC_WCB");
  else
    setenv("OPENGOTHIC_WCB","0",1);
#endif
  }

// Everything the engine logs while it lives; Tempest splits long lines, so the pieces are joined.
struct LogCapture {
  std::mutex  sync;
  std::string text;
  LogCapture() {
    Tempest::Log::setOutputCallback([this](Tempest::Log::Mode,const char* piece) {
      std::lock_guard guard(sync);
      text+=piece;
      });
    }
  ~LogCapture() { Tempest::Log::setOutputCallback({}); }
  std::string take() {
    std::lock_guard guard(sync);
    return std::exchange(text,{});
    }
  };

size_t tagged(const std::string& log, const std::string& tag) {
  size_t count=0;
  for(auto at=log.find(tag);at!=std::string::npos;at=log.find(tag,at+1))
    ++count;
  return count;
  }

std::vector<uint8_t> fileBytes(const fs::path& path) {
  return gothicglb::readFile(path);
  }

void writeBytes(const fs::path& path, const std::vector<uint8_t>& bytes) {
  gothicglb::writeFile(path,bytes.data(),bytes.size());
  }

uint32_t get32(const std::vector<uint8_t>& bytes, size_t at) {
  uint32_t value=0;
  for(unsigned i=0;i<4;++i)
    value|=uint32_t(bytes[at+i])<<(8*i);
  return value;
  }

void put32(std::vector<uint8_t>& bytes, size_t at, uint32_t value) {
  for(unsigned i=0;i<4;++i)
    bytes[at+i]=uint8_t(value>>(8*i));
  }

// A GLB with four more bytes of JSON whitespace: the same mesh from a file of another length.
std::vector<uint8_t> padJson(std::vector<uint8_t> glb) {
  const auto json=get32(glb,12);
  glb.insert(glb.begin()+20+json,4,uint8_t(' '));
  put32(glb,12,json+4);
  put32(glb,8,uint32_t(glb.size()));
  return glb;
  }

// ---- the synthetic cloth world: the objects of worldcloth_contract.cpp ----

uint32_t addMaterial(asset::WorldMesh& mesh, const std::string& name, const std::string& texture) {
  asset::Material material;
  material.name = name;
  material.texture = texture;
  material.group = asset::MaterialGroup::WOOD;
  material.color = {12,34,56,178};
  material.smooth_angle = 47.25f;
  material.texture_scale = {128.f,256.f};
  material.texture_anim_fps = 7.5f;
  material.texture_anim_map_mode = asset::AnimationMapping::LINEAR;
  material.texture_anim_map_dir = {.25f,-.125f};
  material.disable_lightmap = true;
  material.dont_collapse = true;
  material.detail_object = "SYNTHETIC_DETAIL";
  material.detail_object_scale = .75f;
  material.force_occluder = true;
  material.environment_mapping = true;
  material.environment_mapping_strength = .125f;
  material.wave_mode = asset::WaveMode::WALL_AMBIENT;
  material.wave_speed = asset::WaveSpeed::SLOW;
  material.wave_max_amplitude = 11.5f;
  material.wave_grid_size = 91.f;
  material.ignore_sun = true;
  material.alpha_func = asset::AlphaFunction::BLEND;
  material.default_mapping = {.5f,.75f};
  const auto id = uint32_t(mesh.materials.size());
  mesh.materials.push_back(material);
  return id;
  }

void addTriangle(asset::WorldMesh& mesh, uint32_t material, const std::array<uint32_t,3>& indices) {
  const auto face = uint32_t(mesh.polygons.material_indices.size());
  // worldcloth_contract.cpp's flags (occluder, sector, relight, dynamically lit, LOD, normal axis), as .wcb packs them.
  const auto bits = (face%2)<<2 | ((face/2)%2)<<3 | ((face/3)%2)<<4 | ((face/5)%2)<<7 | ((face/7)%2)<<8 | (face%3)<<9;
  const auto flags = wcb::unpackFlags(uint16_t(bits),int16_t(100+face));
  mesh.polygons.material_indices.push_back(material);
  mesh.polygons.lightmap_indices.push_back(face%2 ? -1 : 0);
  mesh.polygons.flags.push_back(flags);
  for(size_t corner=0;corner<3;++corner) {
    const auto feature = uint32_t(mesh.features.size());
    mesh.features.push_back({{float(face)*.03125f+float(corner)*.125f,float(face)*.015625f-float(corner)*.25f},
                             0x10203040u+face*3+uint32_t(corner),{.25f,float(corner)*.125f,1.f}});
    mesh.polygons.vertex_indices.push_back(indices[corner]);
    mesh.polygons.feature_indices.push_back(feature);
    }
  }

// A double-sided quad x..x+200 wide, 300 high: four positions, four triangles.
uint32_t addBanner(asset::WorldMesh& mesh, uint32_t material, float x) {
  const auto base = uint32_t(mesh.vertices.size());
  mesh.vertices.insert(mesh.vertices.end(),{{x,600,0},{x+200,600,0},{x+200,300,0},{x,300,0}});
  for(const auto& tri:std::array<std::array<uint32_t,3>,4>{{{0,1,2},{0,2,3},{0,2,1},{0,3,2}}})
    addTriangle(mesh,material,{base+tri[0],base+tri[1],base+tri[2]});
  return base;
  }

void addFinial(asset::WorldMesh& mesh, uint32_t material) {
  const auto base = uint32_t(mesh.vertices.size());
  for(size_t ring=0;ring<2;++ring)
    for(size_t side=0;side<8;++side) {
      const float angle = float(side)*3.14159265358979323846f*.25f;
      mesh.vertices.push_back({100+18.f*std::cos(angle),650.f+float(ring)*90.f,18.f*std::sin(angle)});
      }
  for(uint32_t side=0;side<8;++side) {
    const auto next = (side+1)%8;
    addTriangle(mesh,material,{base+side,base+next,base+8+side});
    addTriangle(mesh,material,{base+next,base+8+next,base+8+side});
    }
  for(uint32_t corner=1;corner<7;++corner) {
    addTriangle(mesh,material,{base,base+corner+1,base+corner});
    addTriangle(mesh,material,{base+8,base+8+corner,base+8+corner+1});
    }
  }

uint32_t addGrid(asset::WorldMesh& mesh, uint32_t material, bool sail) {
  const uint32_t side = sail ? 5 : 3;
  const auto first = uint32_t(mesh.polygons.material_indices.size());
  const auto base = uint32_t(mesh.vertices.size());
  for(uint32_t row=0;row<side;++row)
    for(uint32_t col=0;col<side;++col) {
      if(sail)
        mesh.vertices.push_back({3000.f+float(col)*200.f,600.f+float(row)*30.f,
                                 20.f*std::sin(float(col)*3.14159265358979323846f*.25f)});
      else
        mesh.vertices.push_back({1200.f,200.f+float(row)*100.f,float(col)*150.f});
      }
  for(uint32_t row=0;row+1<side;++row)
    for(uint32_t col=0;col+1<side;++col) {
      const auto a = base+row*side+col, b = a+1, c = a+side, d = c+1;
      addTriangle(mesh,material,{a,b,c});
      addTriangle(mesh,material,{b,d,c});
      addTriangle(mesh,material,{a,c,b});
      addTriangle(mesh,material,{b,c,d});
      }
  return first;
  }

struct ClothWorld {
  asset::WorldMesh mesh;
  uint32_t city = 0, ground = 0;        // material indices
  uint32_t bannerVertex = 0;            // the first position of the city banner in cell 0,0
  uint32_t flagFirst = 0, flagEnd = 0;  // the ship flag's triangles
  };

// Cell -1,0: the unsupported quad. Cell 0,0: the sail, the ship flag, the rigid finial with the banner material (it
// reaches into cell 0,-1) and a city banner. A second city banner across the border of cells 0,0 and 1,0.
// Cell 2,0: a ground triangle. Cells -1,0, 0,-1 and 2,0 hold no cloth.
ClothWorld clothWorld() {
  ClothWorld w;
  auto& mesh = w.mesh;
  const auto other = addMaterial(mesh,"UNSUPPORTED_CLOTH","UNKNOWN_FLAG.TGA");
  const auto sail  = addMaterial(mesh,"MATERIAL_WITH_TEXTURE_RULE","nw_ship_sail_02.tga");
  const auto flag  = addMaterial(mesh,"nw_ship_flag_01","SYNTHETIC_FLAG.TGA");
  w.city   = addMaterial(mesh,"NW_CITY_FLAG_01","SYNTHETIC_CITY.TGA");
  w.ground = addMaterial(mesh,"CONTRACT_GROUND","CONTRACT_GROUND.TGA");
  mesh.materials[sail].wave_mode = asset::WaveMode::NONE;
  addBanner(mesh,other,-800);
  addGrid(mesh,sail,true);
  w.flagFirst = addGrid(mesh,flag,false);
  w.flagEnd = uint32_t(mesh.polygons.material_indices.size());
  addFinial(mesh,w.city);
  w.bannerVertex = addBanner(mesh,w.city,0);
  addBanner(mesh,w.city,6300);
  const auto base = uint32_t(mesh.vertices.size());
  mesh.vertices.insert(mesh.vertices.end(),{{13000,0,0},{13100,0,0},{13000,0,100}});
  addTriangle(mesh,w.ground,{base,base+1,base+2});
  return w;
  }

// A chunk GLB of one cell of the fixture as an edit writes it: the cell's triangles that keep accepts, with their
// orders, source ids and flags, each corner through move, then the added primitives.
void writeCell(const fs::path& path, const asset::WorldMesh& base, ChunkId cell,
               const std::function<bool(size_t)>& keep, const std::function<void(GlbCorner&,uint32_t)>& move,
               std::vector<GlbPrimitive> added={}) {
  const auto& p = base.polygons;
  std::vector<GlbPrimitive> primitives(base.materials.size());
  for(size_t m=0;m<primitives.size();++m) {
    primitives[m].material = base.materials[m];
    primitives[m].index = int64_t(m);
    }
  for(size_t t=0;t<p.material_indices.size();++t) {
    if(!(centroidCell(base,t)==cell) || !keep(t))
      continue;
    auto& primitive = primitives[p.material_indices[t]];
    std::array<GlbCorner,3> corners;
    for(unsigned c=0;c<3;++c) {
      const auto v = p.vertex_indices[t*3+c], f = p.feature_indices[t*3+c];
      corners[c] = {base.vertices[v],base.features[f],float(v),float(f)};
      move(corners[c],v);
      }
    primitive.triangles.push_back(corners);
    primitive.order.push_back(float(t));
    primitive.flags.push_back(wcb::packFlags(p.flags[t]));
    primitive.flags.push_back(uint16_t(p.flags[t].sector_index));
    primitive.lightmap.push_back(float(p.lightmap_indices[t]));
    }
  std::vector<GlbPrimitive> used;
  for(auto& primitive:primitives)
    if(!primitive.triangles.empty())
      used.push_back(std::move(primitive));
  for(auto& primitive:added)
    used.push_back(std::move(primitive));
  fs::create_directories(path.parent_path());
  writeChunkGlb(path,used);
  }

// ---- extract on a loaded world, with the cooked path and with OPENGOTHIC_WCB=0 ----

struct Loaded {
  asset::WorldMesh      mesh;
  ChunkTable            table;
  WorldCloth::Extracted cloth;
  WorldCloth::Path      path = WorldCloth::Path::None;
  };

// As the game loads a world: readWorldChunks, the world's name and box as worldbase.cpp sets them, then extract.
Loaded extractLoaded(const fs::path& dir, const fs::path& over, bool cooked) {
  Loaded out;
  setCookedPath(cooked);
  try {
    out.mesh = readWorldChunks(dir,over,&out.table);
    }
  catch(...) {
    setCookedPath(true);
    throw;
    }
  setCookedPath(true);
  out.mesh.name = "CLOTH_CONTRACT_MESH";
  out.mesh.bbox = {{-1,-2,-3},{4,5,6}};
  out.cloth = WorldCloth::extract(out.mesh,&out.table);
  out.path = WorldCloth::lastPath();
  return out;
  }

std::string compareLoaded(const Loaded& e, const Loaded& a) {
  auto difference = compareWorldCloth(e.cloth,a.cloth);
  if(!difference.empty())
    return "cloth: "+difference;
  difference = compareWorldMeshes(e.mesh,a.mesh);
  if(!difference.empty())
    return "landscape: "+difference;
  difference = compareChunkTables(e.table,a.table);
  if(!difference.empty())
    return "chunk table: "+difference;
  return {};
  }

struct Outcome {
  size_t           surfaces = 0, triangles = 0;
  WorldCloth::Path path = WorldCloth::Path::None;
  std::string      log;
  };

// extract on dir with over, by the island search (OPENGOTHIC_WCB=0) and with the cooked path: equal results.
Outcome compareSet(const std::string& label, const fs::path& dir, const fs::path& over) {
  LogCapture log;
  const auto searched = extractLoaded(dir,over,false);
  require(searched.path==WorldCloth::Path::Search,label+": with OPENGOTHIC_WCB=0 extract must run the island search");
  const auto cooked = extractLoaded(dir,over,true);
  const auto difference = compareLoaded(searched,cooked);
  require(difference.empty(),label+": extract on the cooked path differs from the island search: "+difference);
  return {cooked.cloth.surfaces.size(),cooked.cloth.original.polygons.material_indices.size(),cooked.path,log.take()};
  }

void expectBundle(const std::string& label, const fs::path& dir, const fs::path& over={}) {
  const auto o = compareSet(label,dir,over);
  const auto line = "[world-cloth] bundle "+cookedClothPath(dir,over).string()+": "+std::to_string(o.surfaces)+
                    " surfaces, "+std::to_string(o.triangles)+" triangles";
  std::printf("%s: the bundle gives the island search's %zu surfaces and %zu triangles\n",label.c_str(),o.surfaces,
              o.triangles);
  require(o.path==WorldCloth::Path::Bundle,label+": extract must take the bundle; log: "+o.log);
  require(tagged(o.log,"[world-cloth] ")==1 && o.log.find(line)!=std::string::npos,
          label+": the log must hold the line '"+line+"' alone, got: "+o.log);
  }

void expectSearch(const std::string& label, const fs::path& dir, const std::string& reason, const fs::path& over={}) {
  const auto o = compareSet(label,dir,over);
  std::printf("%s: island search; log: %s\n",label.c_str(),o.log.empty() ? "(none)" : o.log.c_str());
  require(o.path==WorldCloth::Path::Search,label+": extract must run the island search");
  require(tagged(o.log,"[world-cloth] ")==(reason.empty() ? 0u : 1u),label+": the bundle must log "+
          (reason.empty() ? std::string("nothing") : "one line")+", got: "+o.log);
  require(reason.empty() || o.log.find(reason)!=std::string::npos,label+": the log must say '"+reason+"', got: "+o.log);
  }

// ---- the synthetic world and its override sets ----

void fixture(const fs::path& root, const ClothWorld& world) {
  const auto dir = root/"cloth";
  writeWorldChunks(world.mesh,clothSource,dir);
  const auto stats = cookWorldChunks(dir);
  require(stats.clothWritten && stats.clothSurfaces==4 && stats.clothTriangles==88,
          "the fixture's bundle must hold two city banners, the ship flag and the sail (88 triangles), got "+
          std::to_string(stats.clothSurfaces)+" surfaces and "+std::to_string(stats.clothTriangles)+" triangles");
  // Its shape: an island across a cell border, several islands in one cell, cells without cloth.
  const auto manifest = readWorldChunkManifest(dir);
  const auto bundle = ClothBundle::read(dir/wcl::FileName).data();
  size_t clothCells = 0, plain = 0, across = 0, inZero = 0;
  for(size_t i=0;i<manifest.cells.size();++i) {
    const auto& id = manifest.cells[i].id;
    const auto n = bundle.exclusions[i].size();
    if(n>0)
      ++clothCells;
    else
      ++plain;
    if(id==ChunkId{1,0})
      across = n;
    if(id==ChunkId{0,0})
      inZero = n;
    }
  require(manifest.cells.size()==5 && clothCells==2 && plain==3 && across==2 && inZero==86,
          "the fixture must spread its cloth over cells 0,0 (86 triangles: four islands, one of them half) and 1,0 "
          "(the other half of the banner across the border), with three cells without cloth");
  expectBundle("synthetic cloth world",dir);
  require(verifyCookedWorld(dir,{},{},true).empty(),"the synthetic cloth world must verify");

  // The override replaces cell 0,0 and moves a lower corner of its city banner.
  const auto moved = root/"cloth-moved";
  writeCell(moved/"chunks/0_0.glb",world.mesh,{0,0},[](size_t) { return true; },[&](GlbCorner& c, uint32_t v) {
    if(v==world.bannerVertex+2)
      c.position.x += 10.f;
    });
  cookWorldChunks(dir,moved);
  expectBundle("override: cell 0,0 with a moved cloth vertex",dir,moved);
  require(verifyCookedWorld(dir,moved,{},true).empty(),"the moved vertex override must verify");

  // The override adds a city banner to cell 2,0 as new geometry: new positions, no source ids or orders.
  const auto added = root/"cloth-added";
  GlbPrimitive banner;
  banner.material = world.mesh.materials[world.city];
  banner.index = int64_t(world.city);
  const asset::Vec3 corners[4] = {{13200,600,0},{13400,600,0},{13400,300,0},{13200,300,0}};
  for(const auto& tri:std::array<std::array<uint32_t,3>,4>{{{0,1,2},{0,2,3},{0,2,1},{0,3,2}}}) {
    std::array<GlbCorner,3> t;
    for(unsigned c=0;c<3;++c)
      t[c] = {corners[tri[c]],{{float(tri[c])*.25f,.5f},0xFF405060u,{0.f,0.f,1.f}},-1.f,-1.f};
    banner.triangles.push_back(t);
    banner.flags.insert(banner.flags.end(),{0,0});
    banner.lightmap.push_back(-1.f);
    }
  writeCell(added/"chunks/2_0.glb",world.mesh,{2,0},[](size_t) { return true; },[](GlbCorner&,uint32_t) {},{banner});
  const auto addedStats = cookWorldChunks(dir,added);
  require(addedStats.clothSurfaces==5,"the added banner must be a fifth surface, got "+
          std::to_string(addedStats.clothSurfaces));
  expectBundle("override: a new cloth island as added geometry in cell 2,0",dir,added);
  require(verifyCookedWorld(dir,added,{},true).empty(),"the added island override must verify");

  // The override drops the ship flag's triangles from cell 0,0.
  const auto removed = root/"cloth-removed";
  writeCell(removed/"chunks/0_0.glb",world.mesh,{0,0},[&](size_t t) { return t<world.flagFirst || t>=world.flagEnd; },
            [](GlbCorner&,uint32_t) {});
  const auto removedStats = cookWorldChunks(dir,removed);
  require(removedStats.clothSurfaces==3,"without the flag three surfaces remain, got "+
          std::to_string(removedStats.clothSurfaces));
  expectBundle("override: cell 0,0 without the ship flag",dir,removed);
  require(verifyCookedWorld(dir,removed,{},true).empty(),"the removed island override must verify");
  }

// ---- fallbacks: each case in a fresh copy of the cooked fixture ----

void fallbacks(const fs::path& root, const ClothWorld& world) {
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
  fs::remove(dir/wcl::FileName);
  expectSearch("no bundle",dir,{});

  dir = copy();
  auto bytes = fileBytes(dir/wcl::FileName);
  put32(bytes,32,get32(bytes,32)^1u); // the source world's CRC32
  writeBytes(dir/wcl::FileName,bytes);
  expectSearch("a bundle of another source world",dir,"names another source world than CLOTH_CONTRACT.ZEN");

  // The set re-cooked after an edit of cell 0,0 at another length, the old bundle put back.
  dir = copy();
  const auto old = fileBytes(dir/wcl::FileName);
  writeCell(dir/"chunks/0_0.glb",world.mesh,{0,0},[](size_t) { return true; },[&](GlbCorner& c, uint32_t v) {
    if(v==world.bannerVertex+2)
      c.position.x += 10.f;
    });
  require(cookWorldChunks(dir).clothWritten,"the edit must change the bundle");
  writeBytes(dir/wcl::FileName,old);
  expectSearch("the old bundle beside the re-cooked set",dir,"was cooked with another index than");

  dir = copy();
  bytes = fileBytes(dir/wcl::FileName);
  bytes[80] = uint8_t(bytes[80]^1u); // the rules' SHA-256
  writeBytes(dir/wcl::FileName,bytes);
  expectSearch("a bundle of other rules",dir,"was cooked with other world cloth rules than this build's");

  dir = copy();
  bytes = fileBytes(dir/wcl::FileName);
  put32(bytes,44,get32(bytes,44)+1); // the algorithm
  writeBytes(dir/wcl::FileName,bytes);
  expectSearch("a bundle of another algorithm",dir,"was cooked by world cloth algorithm "+
               std::to_string(WorldCloth::Algorithm+1)+", this build runs "+std::to_string(WorldCloth::Algorithm));

  dir = copy();
  bytes = fileBytes(dir/wcl::FileName);
  bytes.pop_back();
  writeBytes(dir/wcl::FileName,bytes);
  expectSearch("a truncated bundle",dir,"truncated");

  // Exclusions edited through the codec: one past the cell's triangles, and one fewer than the original mesh has.
  const auto cells = readWorldChunkManifest(cooked).cells;
  size_t zero = cells.size();
  for(size_t i=0;i<cells.size();++i)
    if(cells[i].id==ChunkId{0,0})
      zero = i;
  require(zero<cells.size(),"the fixture has cell 0,0");
  dir = copy();
  auto data = ClothBundle::read(dir/wcl::FileName).data();
  data.exclusions[zero].back() = 1000000;
  writeBytes(dir/wcl::FileName,encodeClothBundle(data));
  expectSearch("an exclusion past a cell's triangles",dir,"cell 0,0 excludes triangle 1000000, past the cell's");

  dir = copy();
  data = ClothBundle::read(dir/wcl::FileName).data();
  data.exclusions[zero].pop_back();
  writeBytes(dir/wcl::FileName,encodeClothBundle(data));
  expectSearch("exclusions one fewer than the original mesh",dir,
               "its exclusions name 87 triangles, the loaded chunk table holds 87 of them, its original mesh has 88");

  // The set re-cooked between the world's load and extract: the new bundle is for the new index, not for the
  // landscape the load read, so the island search runs on that landscape.
  {
  dir = copy();
  LogCapture log;
  ChunkTable table;
  auto mesh = readWorldChunks(dir,{},&table);
  mesh.name = "CLOTH_CONTRACT_MESH";
  auto searchedMesh = mesh;
  auto searchedTable = table;
  writeCell(dir/"chunks/0_0.glb",world.mesh,{0,0},[](size_t) { return true; },[&](GlbCorner& c, uint32_t v) {
    if(v==world.bannerVertex+2)
      c.position.x += 10.f;
    });
  require(cookWorldChunks(dir).clothWritten,"the edit must change the bundle");
  const auto cloth = WorldCloth::extract(mesh,&table);
  const auto path = WorldCloth::lastPath();
  const auto searched = WorldCloth::searchIslands(searchedMesh,&searchedTable);
  auto difference = compareWorldCloth(searched,cloth);
  if(difference.empty())
    difference = compareWorldMeshes(searchedMesh,mesh);
  if(difference.empty())
    difference = compareChunkTables(searchedTable,table);
  const auto text = log.take();
  std::printf("the set re-cooked between load and extract: island search; log: %s\n",text.c_str());
  require(path==WorldCloth::Path::Search && difference.empty(),
          "a set re-cooked after the world's load must give the island search on the loaded landscape: "+difference);
  require(tagged(text,"[world-cloth] ")==1 && text.find("was cooked with another index than")!=std::string::npos,
          "the bundle of the re-cooked set must be refused by the index the load read, got: "+text);
  }

  // One GLB of another length at load: the set has no edits, so the index stays in use for the other cells.
  dir = copy();
  writeBytes(dir/"chunks/2_0.glb",padJson(fileBytes(dir/"chunks/2_0.glb")));
  const auto o = compareSet("a GLB of another length",dir,{});
  std::printf("a GLB of another length at load: island search; log: %s\n",o.log.c_str());
  require(o.path==WorldCloth::Path::Search && tagged(o.log,"[world-cloth] ")==1 &&
          o.log.find("bytes at world load, the index lists the cell with a GLB of")!=std::string::npos,
          "a GLB of another length must keep the bundle out with its reason, got: "+o.log);
  require(o.log.find(std::to_string(cells.size()-1)+" of "+std::to_string(cells.size())+" cells read from their .wcb")!=
          std::string::npos,"the index must stay in use for the other cells, got: "+o.log);
  }

// ---- the cook ----

void cooks(const fs::path& root, const ClothWorld& world) {
  // A second cook of an unchanged world writes nothing; an old time on its files shows any rewrite.
  const auto dir = root/"recook";
  writeWorldChunks(world.mesh,clothSource,dir);
  auto stats = cookWorldChunks(dir);
  require(stats.written==stats.cells && stats.clothWritten && stats.indexWritten,"a first cook writes everything");
  const fs::file_time_type old = std::chrono::floor<std::chrono::seconds>(fs::file_time_type::clock::now()-std::chrono::hours(24));
  for(const auto* name:{wcl::FileName,wci::FileName})
    fs::last_write_time(dir/name,old);
  stats = cookWorldChunks(dir);
  require(stats.written==0 && !stats.clothWritten && !stats.indexWritten &&
          fs::last_write_time(dir/wcl::FileName)==old && fs::last_write_time(dir/wci::FileName)==old,
          "a second cook of an unchanged world must write nothing and keep the bundle's time");
  // An override of cell 0,0: its cell, its bundle and its index are written, the base cells are left.
  const auto over = root/"recook-override";
  writeCell(over/"chunks/0_0.glb",world.mesh,{0,0},[](size_t) { return true; },[&](GlbCorner& c, uint32_t v) {
    if(v==world.bannerVertex+2)
      c.position.x += 10.f;
    });
  stats = cookWorldChunks(dir,over);
  require(stats.written==1 && stats.left==stats.cells-1 && stats.clothWritten && stats.indexWritten &&
          fs::exists(over/wcl::FileName) && fs::last_write_time(dir/wcl::FileName)==old,
          "an override must write its replaced cell, its bundle and its index, and leave the base's bundle");
  std::printf("cook: a second cook writes nothing (bundle time kept); an override writes 1 of %zu cells, the bundle "
              "and the index\n",stats.cells);

  // Two cooks into the same directory at once.
  const auto both = root/"two-cooks";
  writeWorldChunks(world.mesh,clothSource,both);
  std::string errors[2];
  auto cook = [&](int i) {
    try {
      cookWorldChunks(both);
      }
    catch(const std::exception& error) {
      errors[i] = error.what();
      }
    };
  std::thread first(cook,0), second(cook,1);
  first.join();
  second.join();
  for(const auto& error:errors)
    require(error.empty(),"one of two cooks at once failed: "+error);
  const auto difference = verifyCookedWorld(both,{},{},true);
  require(difference.empty(),"two cooks at once must leave a set that verifies: "+difference);
  expectBundle("two cooks into one directory at once",both);

  // verify-cooked-world names the surface of a changed pin.
  const auto pinned = root/"pin";
  fs::copy(both,pinned,fs::copy_options::recursive);
  auto data = ClothBundle::read(pinned/wcl::FileName).data();
  auto& surface = data.cloth.surfaces[1];
  surface.pins[0] = (surface.pins[0]+1)%uint32_t(surface.mesh.vertices.size());
  writeBytes(pinned/wcl::FileName,encodeClothBundle(data));
  const auto pin = verifyCookedWorld(pinned,{},{},true);
  require(pin.find("surface 1 ("+surface.visual+"): pins differ")!=std::string::npos,
          "verify-cooked-world must name the surface whose pin changed, got: "+pin);
  std::printf("verify-cooked-world with one pin changed: %s\n",pin.c_str());
  }

// ---- one cook per directory (#1284) ----

// Stops a cook in its hook until released.
struct Gate {
  std::mutex              sync;
  std::condition_variable changed;
  bool                    stopped = false, released = false;
  void stop() {
    std::unique_lock lock(sync);
    stopped = true;
    changed.notify_all();
    changed.wait(lock,[&] { return released; });
    }
  bool waitStopped() {
    std::unique_lock lock(sync);
    return changed.wait_for(lock,std::chrono::seconds(120),[&] { return stopped; });
    }
  void release() {
    std::lock_guard lock(sync);
    released = true;
    changed.notify_all();
    }
  };

struct Cook {
  std::thread       thread;
  std::atomic<bool> finished {false};
  std::string       error;
  void start(std::function<void()> body) {
    thread = std::thread([this,body=std::move(body)] {
      try {
        body();
        }
      catch(const std::exception& e) {
        error = e.what();
        }
      finished = true;
      });
    }
  bool waitFinished(std::chrono::seconds limit) {
    const auto until = Clock::now()+limit;
    while(!finished && Clock::now()<until)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    return finished;
    }
  };

// Every cell read from its .wcb equals its GLB read.
void cellsFromWcb(const std::string& label, const fs::path& dir, const fs::path& over) {
  setCookedPath(false);
  ChunkTable plain;
  const auto mesh = readWorldChunks(dir,over,&plain);
  setCookedPath(true);
  ChunkTable cooked;
  (void)readWorldChunks(dir,over,&cooked);
  const auto lists = chunkTriangleLists(mesh,plain);
  for(size_t i=0;i<lists.size();++i) {
    std::vector<uint32_t> sources;
    for(const auto t:lists[i])
      sources.push_back(plain.triangleSources[t]);
    const auto a = plain.source->read(i,sources), b = cooked.source->read(i,sources);
    const auto difference = compareWorldMeshes(a.mesh,b.mesh);
    require(difference.empty() && a.vertexIds==b.vertexIds && a.featureIds==b.featureIds,
            label+": cell "+plain.entries[i].file+" from its .wcb differs from its GLB: "+difference);
    }
  require(cooked.source->reads().cooked==lists.size() && cooked.source->reads().glb==0,
          label+": every cell must be read from its .wcb");
  }

void lock(const fs::path& root) {
  const auto base = root/"lock", over = root/"lock-override";
  writeWorldChunks(stripMesh(),stripSource,base);
  writeStripOverride(over,stripMesh(),true);
  cookWorldChunks(base,over);
  // 1_0 of the border override with its shift moved from border 4 to border 8: a GLB of the same length.
  auto moved = stripMesh();
  moved.vertices[stripVertex(8,0)].y += 30.f;
  const auto edited = writeStripOverride(root/"lock-other",moved,false)/"chunks/1_0.glb";
  require(fs::file_size(edited)==fs::file_size(over/"chunks/1_0.glb"),"the moved edit must keep its GLB's length");

  {
  Gate gate;
  Cook a, b;
  a.start([&] { cookWorldChunks(base,over,{},{},[&] { gate.stop(); }); });
  const bool stopped = gate.waitStopped();
  bool waited = false;
  if(stopped) {
    // While B waits, its GLB is unreadable (broken magic at the same length), then becomes the moved edit: a B that
    // read a GLB before taking the lock would fail on it.
    auto broken = fileBytes(edited);
    broken[0] = 'X';
    writeBytes(over/"chunks/1_0.glb",broken);
    b.start([&] { cookWorldChunks(base,over); });
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    waited = !b.finished;
    fs::copy_file(edited,over/"chunks/1_0.glb",fs::copy_options::overwrite_existing);
    }
  gate.release();
  a.thread.join();
  if(b.thread.joinable())
    b.thread.join();
  require(stopped,"cook A must reach its stop after the GLB check");
  require(waited,"cook B of the same directory must wait while A holds it; B ended with: "+
          (b.error.empty() ? std::string("success") : b.error));
  require(a.error.empty() && b.error.empty(),"both cooks must finish: A: "+a.error+"; B: "+b.error);
  const auto difference = verifyCookedWorld(base,over,{},true);
  require(difference.empty(),"after both cooks the set must verify with hashes: "+difference);
  cellsFromWcb("after two cooks of one directory",base,over);
  expectBundle("after two cooks of one directory",base,over);
  std::puts("lock: cook B waited 1.5 s while cook A was stopped before its index, reading no GLB (a broken one was "
            "replaced before A went on); both finished, the set verifies with hashes and every cell reads its .wcb "
            "equal to its GLB");
  }

  // Cooks of two directories do not wait for each other.
  {
  const auto x = root/"lock-x", y = root/"lock-y";
  writeWorldChunks(stripMesh(),stripSource,x);
  writeWorldChunks(stripMesh(),stripSource,y);
  Gate gate;
  Cook a, b;
  a.start([&] { cookWorldChunks(x,{},{},{},[&] { gate.stop(); }); });
  const bool stopped = gate.waitStopped();
  bool ran = false;
  if(stopped) {
    b.start([&] { cookWorldChunks(y); });
    ran = b.waitFinished(std::chrono::seconds(60));
    }
  gate.release();
  a.thread.join();
  if(b.thread.joinable())
    b.thread.join();
  require(stopped && ran,"a cook of another directory must finish while cook A is stopped");
  require(a.error.empty() && b.error.empty(),"both cooks must finish: A: "+a.error+"; B: "+b.error);
  require(verifyCookedWorld(x,{},{},true).empty() && verifyCookedWorld(y,{},{},true).empty(),"both sets must verify");
  std::puts("lock: a cook of another directory finished while cook A was stopped");
  }
  }

// ---- carried over from the review of task 3: a .wcb of another source world ----

void otherWorldWcb(const fs::path& root) {
  const auto dir = root/"other-world";
  writeWorldChunks(stripMesh(),stripSource,dir);
  cookWorldChunks(dir);
  auto chunk = fileBytes(dir/"chunks/1_0.wcb");
  put32(chunk,120,get32(chunk,120)^1u); // the .wcb header's CRC32 of the source world
  writeBytes(dir/"chunks/1_0.wcb",chunk);
  LogCapture log;
  setCookedPath(false);
  ChunkTable plain;
  const auto mesh = readWorldChunks(dir,{},&plain);
  setCookedPath(true);
  ChunkTable cooked;
  (void)readWorldChunks(dir,{},&cooked);
  const auto lists = chunkTriangleLists(mesh,plain);
  std::vector<std::string> glbCells;
  for(size_t i=0;i<lists.size();++i) {
    std::vector<uint32_t> sources;
    for(const auto t:lists[i])
      sources.push_back(plain.triangleSources[t]);
    const auto before = cooked.source->reads().glb;
    const auto a = plain.source->read(i,sources), b = cooked.source->read(i,sources);
    const auto difference = compareWorldMeshes(a.mesh,b.mesh);
    require(difference.empty() && a.vertexIds==b.vertexIds && a.featureIds==b.featureIds,
            "a .wcb of another source world: cell "+plain.entries[i].file+" differs from its GLB: "+difference);
    if(cooked.source->reads().glb!=before)
      glbCells.push_back(plain.entries[i].file);
    }
  const auto text = log.take();
  require(glbCells==std::vector<std::string>{"chunks/1_0.glb"},"only 1_0 must read its GLB");
  require(tagged(text,"[world-chunks] ")==2 && text.find("1_0.wcb was cooked for another source world than CONTRACT.ZEN")!=
          std::string::npos,"the cell must log that its .wcb was cooked for another source world, got: "+text);
  std::printf("a .wcb of another source world: only 1_0 reads its GLB, equal; log: %s\n",text.c_str());
  }

// ---- the base's NEWWORLD in a scratch copy ----

size_t cookedFiles(const fs::path& dir) {
  size_t count=0;
  for(const auto& entry:fs::recursive_directory_iterator(dir)) {
    const auto name = entry.path().filename();
    count += entry.path().extension()==".wcb" || name==wci::FileName || name==wcl::FileName;
    }
  return count;
  }

// Hard links (or copies across file systems) of every file of a world, without cooked files.
void linkWorld(const fs::path& from, const fs::path& to) {
  fs::create_directories(to);
  for(const auto& entry:fs::recursive_directory_iterator(from)) {
    const auto target = to/fs::relative(entry.path(),from);
    if(entry.is_directory()) {
      fs::create_directories(target);
      continue;
      }
    const auto name = entry.path().filename();
    if(!entry.is_regular_file() || entry.path().extension()==".wcb" || name==wci::FileName || name==wcl::FileName)
      continue;
    std::error_code error;
    fs::create_hard_link(entry.path(),target,error);
    if(error)
      fs::copy_file(entry.path(),target);
    }
  }

void newWorld(const fs::path& root, const fs::path& world) {
  const auto scratch = root/"NEWWORLD";
  const auto inBase = cookedFiles(world);
  linkWorld(world,scratch);
  const auto started = Clock::now();
  const auto stats = cookWorldChunks(scratch);
  std::printf("NEWWORLD cooked beside its GLBs in %s: %zu cells, bundle %llu bytes with %zu surfaces and %llu "
              "triangles, %.1f s\n",scratch.string().c_str(),stats.cells,static_cast<unsigned long long>(stats.clothBytes),
              stats.clothSurfaces,static_cast<unsigned long long>(stats.clothTriangles),
              std::chrono::duration<double>(Clock::now()-started).count());
  require(cookedFiles(world)==inBase,"the base must not be written");
  require(stats.clothSurfaces>0,"NEWWORLD has world cloth");
  expectBundle("base NEWWORLD",scratch);
  }

// ---- the guard of WorldCloth::Algorithm ----

struct Digest {
  Sha256 hash;
  void u32(uint32_t v) {
    uint8_t b[4];
    for(unsigned i=0;i<4;++i)
      b[i] = uint8_t(v>>(8*i));
    hash.update(b,4);
    }
  void f32(float v) { u32(std::bit_cast<uint32_t>(v)); }
  void text(const std::string& s) {
    u32(uint32_t(s.size()));
    hash.update(s.data(),s.size());
    }
  void polygons(const asset::PolygonList& p) {
    u32(uint32_t(p.material_indices.size()));
    for(size_t t=0;t<p.material_indices.size();++t) {
      u32(p.material_indices[t]);
      u32(uint32_t(p.lightmap_indices[t]));
      u32(wcb::packFlags(p.flags[t]));
      u32(uint32_t(uint16_t(p.flags[t].sector_index)));
      for(unsigned c=0;c<3;++c) {
        u32(p.vertex_indices[t*3+c]);
        u32(p.feature_indices[t*3+c]);
        }
      }
    }
  void mesh(const asset::WorldMesh& m) {
    text(m.name);
    for(const auto* v:{&m.bbox.min,&m.bbox.max}) {
      f32(v->x); f32(v->y); f32(v->z);
      }
    u32(uint32_t(m.materials.size()));
    for(const auto& material:m.materials)
      text(gothicglb::materialJson(material,0));
    u32(uint32_t(m.vertices.size()));
    for(const auto& v:m.vertices) {
      f32(v.x); f32(v.y); f32(v.z);
      }
    u32(uint32_t(m.features.size()));
    for(const auto& f:m.features) {
      f32(f.texture.x); f32(f.texture.y); u32(f.light); f32(f.normal.x); f32(f.normal.y); f32(f.normal.z);
      }
    polygons(m.polygons);
    }
  };

void guardAlgorithm(const ClothWorld& world) {
  auto landscape = world.mesh;
  landscape.name = "CLOTH_CONTRACT_MESH";
  const auto cloth = WorldCloth::searchIslands(landscape);
  Digest d;
  d.u32(uint32_t(cloth.surfaces.size()));
  for(const auto& s:cloth.surfaces) {
    d.u32(s.id);
    d.text(s.visual);
    d.f32(s.stiffness);
    d.f32(s.linearDamping);
    d.u32(uint32_t(s.pins.size()));
    for(const auto pin:s.pins)
      d.u32(pin);
    d.mesh(s.mesh);
    }
  d.mesh(cloth.original);
  d.polygons(landscape.polygons);
  const auto value = d.hash.finish();
  std::printf("searchIslands on the fixture: %zu surfaces, digest %s\n",cloth.surfaces.size(),value.c_str());
  require(value==resultDigest,"extract's result changed: raise WorldCloth::Algorithm and update this digest (now "+
          value+")");
  }

}

int main() {
  TemporaryDirectory temporary;
  try {
    fs::create_directories(temporary.path);
    const auto world = clothWorld();
    fixture(temporary.path,world);
    fallbacks(temporary.path,world);
    cooks(temporary.path,world);
    lock(temporary.path);
    otherWorldWcb(temporary.path);

    const auto* gothic = std::getenv("GOTHIC_DIR");
    require(gothic!=nullptr && *gothic!='\0',"GOTHIC_DIR must point to a Gothic II installation");
    const char* args[] = {"gothic-worldcloth-cooked-contract","-g",gothic};
    CommandLine commandLine(3,args);
    Resources resources;
    Resources::loadBase();
    const auto base = Resources::worldBaseDir("NEWWORLD.ZEN");
    require(!base.empty() && fs::exists(base/"world.json"),"the base has no NEWWORLD");
    newWorld(temporary.path,base);

    guardAlgorithm(world);
    std::puts("World cloth bundle contract passed");
    return EXIT_SUCCESS;
    }
  catch(const std::exception& error) {
    std::fprintf(stderr,"FAIL: %s\n",error.what());
    return EXIT_FAILURE;
    }
  }
