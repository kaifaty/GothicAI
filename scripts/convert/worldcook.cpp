#include "worldcook.h"

#include "gothicglb.h"
#include "physics/worldclothbundle.h"
#include "worldchunkcook.h"
#include "worldchunkcookedsource.h"
#include "worldchunks.h"
#include "worlddata.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>

namespace fs = std::filesystem;

namespace WorldCook {

const char* const usage =
  "       content-convert cook-world|verify-cooked-world <world dir> [--content <override world dir>] [--out <dir>]\n"
  "       content-convert time-world <world dir> [--content <dir>] [--out <dir>]\n";

namespace {

using Clock = std::chrono::steady_clock;
constexpr int runs = 5;

double ms(Clock::time_point a, Clock::time_point b) {
  return std::chrono::duration<double,std::milli>(b-a).count();
  }

struct Options {
  fs::path world, content, out;
  };

std::optional<Options> parse(const std::vector<std::string>& args) {
  if(args.size()<2)
    return std::nullopt;
  Options o;
  o.world = args[1];
  for(size_t i=2;i<args.size();i+=2) {
    if(i+1>=args.size())
      return std::nullopt;
    if(args[i]=="--content")
      o.content = args[i+1];
    else if(args[i]=="--out")
      o.out = args[i+1];
    else
      return std::nullopt;
    }
  return o;
  }

int cook(const Options& o) {
  const auto started = Clock::now();
  const auto stats = cookWorldChunks(o.world,o.content,o.out);
  std::printf("%s: %zu cells, %llu triangles cooked; %zu .wcb written, %zu left unchanged; index %s %s; "
              "GLB %llu bytes, .wcb %llu bytes, index %llu bytes (%.1f s)\n",
              o.world.string().c_str(),stats.cells,static_cast<unsigned long long>(stats.triangles),stats.written,
              stats.left,cookedIndexPath(o.world,o.content,o.out).string().c_str(),
              stats.indexWritten ? "written" : "left unchanged",static_cast<unsigned long long>(stats.glbBytes),
              static_cast<unsigned long long>(stats.wcbBytes),static_cast<unsigned long long>(stats.indexBytes),
              ms(started,Clock::now())/1000.0);
  std::printf("world cloth bundle %s %s: %zu surfaces, %llu triangles taken out of the cells, %llu bytes\n",
              cookedClothPath(o.world,o.content,o.out).string().c_str(),stats.clothWritten ? "written" : "left unchanged",
              stats.clothSurfaces,static_cast<unsigned long long>(stats.clothTriangles),
              static_cast<unsigned long long>(stats.clothBytes));
  return 0;
  }

int verify(const Options& o) {
  const auto started = Clock::now();
  const auto difference = verifyCookedWorld(o.world,o.content,o.out,true);
  if(!difference.empty()) {
    std::cerr << "content-convert: " << o.world.string() << ": the cooked world differs from readWorldChunks: "
              << difference << '\n';
    return 1;
    }
  const auto manifest = readWorldChunkManifest(o.world);
  std::printf("%s: %zu cooked cells and the index assemble into readWorldChunks' mesh and chunk table; every GLB "
              "matches its chunk's length and SHA-256; the world cloth bundle equals a fresh island search (%.1f s)\n",
              o.world.string().c_str(),manifest.cells.size(),ms(started,Clock::now())/1000.0);
  return 0;
  }

// The first zCVobStartpoint in vob order: the game's WayMatrix::startPoint.
const asset::VirtualObject* startPoint(const std::vector<std::shared_ptr<asset::VirtualObject>>& vobs) {
  for(const auto& vob:vobs) {
    if(vob==nullptr)
      continue;
    if(vob->type==asset::VirtualObjectType::zCVobStartpoint)
      return vob.get();
    if(const auto* child=startPoint(vob->children))
      return child;
    }
  return nullptr;
  }

struct Series {
  std::string         name;
  std::vector<double> values;
  };

void printSeries(const Series& s) {
  auto v = s.values;
  std::sort(v.begin(),v.end());
  std::printf("| %-48s | %10.1f | %10.1f | %10.1f |\n",s.name.c_str(),v[v.size()/2],v.front(),v.back());
  }

int timeWorld(const Options& o) {
  const auto manifest = readWorldChunkManifest(o.world);
  std::vector<fs::path> glbs, wcbs;
  const auto indexPath = cookedIndexPath(o.world,o.content,o.out);
  uint64_t glbBytes = 0, wcbBytes = 0;
  bool cooked = fs::exists(indexPath);
  const uint64_t indexBytes = cooked ? fs::file_size(indexPath) : 0;
  for(const auto& cell:manifest.cells) {
    glbs.push_back(chunkGlbPath(o.world,o.content,cell.drawFile()));
    wcbs.push_back(cookedChunkPath(o.world,o.content,cell.drawFile(),o.out));
    glbBytes += fs::file_size(glbs.back());
    if(fs::exists(wcbs.back()))
      wcbBytes += fs::file_size(wcbs.back());
    else
      cooked = false;
    }

  // The Near ring: the 5 x 5 cells around the start point.
  const auto data = WorldData::read(o.world);
  const auto* start = startPoint(data.vobs);
  std::vector<size_t> nearCells;
  ChunkId startCell;
  if(start!=nullptr) {
    startCell = ChunkId::fromPosition(start->position.x,start->position.z);
    for(size_t i=0;i<manifest.cells.size();++i)
      if(std::abs(manifest.cells[i].id.x-startCell.x)<=2 && std::abs(manifest.cells[i].id.z-startCell.z)<=2)
        nearCells.push_back(i);
    }

  // Warm the page cache once: every run below reads the same files from memory.
  for(const auto* list:{&glbs,&wcbs})
    for(const auto& path:*list)
      if(fs::exists(path))
        (void)gothicglb::readFile(path);
  if(cooked)
    (void)gothicglb::readFile(indexPath);
  // The world cloth bundle beside the index, checked against it as the game checks it at load.
  const auto clothPath = cookedClothPath(o.world,o.content,o.out);
  const bool bundled = cooked && fs::exists(clothPath);
  CookedSet set;
  if(bundled) {
    const auto bytes = gothicglb::readFile(indexPath);
    set = {indexPath,sha256Bytes(bytes),manifest.source,manifest.cells.size(),{}};
    (void)gothicglb::readFile(clothPath);
    }

  enum { ReadGlb, Today, Parse, Sort, Stitch, Rest, TodayNear, SourceNear, ReadWcb, Validate, Index, Assemble, CookedNear,
         ClothSearch, ClothRead, ClothApply };
  std::vector<Series> series = {
    {"today: read GLB files (separate pass)",{}},
    {"today: readWorldChunks total",{}},
    {"  read + cgltf parse + attribute extraction",{}},
    {"  source-order sort",{}},
    {"  ID resolution, dedup maps, stitching, table",{}},
    {"  rest: trace copy, teardown of reader and sets",{}},
    {"today: ChunkMeshSource::read of the Near ring",{}},
    {"cooked: ChunkMeshSource::read of the Near ring",{}},
    {"cooked: read .wcb files and the index",{}},
    {"cooked: validate (open + index + world.json + GLB stat)",{}},
    {"cooked: read + validate the index alone",{}},
    {"cooked: assemble whole mesh and table",{}},
    {"cooked: read + validate the index and the Near ring",{}},
    {"cloth: WorldCloth::extract by island search",{}},
    {"cloth: read + validate the bundle",{}},
    {"cloth: apply the bundle (exclusions, retain)",{}},
    };
  ChunkTable table;
  size_t triangles = 0;
  uint64_t nearCooked = 0;
  size_t clothSurfaces = 0, clothTriangles = 0;
  for(int run=0;run<runs;++run) {
    auto t0 = Clock::now();
    uint64_t read = 0;
    for(const auto& path:glbs)
      read += gothicglb::readFile(path).size();
    series[ReadGlb].values.push_back(ms(t0,Clock::now()));
    if(read!=glbBytes)
      throw std::runtime_error("GLB files changed while timing");

    {
    WorldChunkTrace trace;
    table = {};
    t0 = Clock::now();
    auto mesh = readWorldChunks(o.world,o.content,&table,&trace);
    const auto t1 = Clock::now();
    triangles = mesh.polygons.material_indices.size();
    series[Today].values.push_back(ms(t0,t1));
    series[Parse].values.push_back(ms(t0,trace.parsed));
    series[Sort].values.push_back(ms(trace.parsed,trace.ordered));
    series[Stitch].values.push_back(ms(trace.ordered,trace.stitched));
    series[Rest].values.push_back(ms(trace.stitched,t1));
    // The game's streaming reader on the GLB path: one GLB per chunk, IDs from the stitched world's tables.
    // currentFile always takes that path; the files are unchanged, so it reads what OPENGOTHIC_WCB=0 reads.
    const auto lists = chunkTriangleLists(mesh,table);
    std::vector<std::vector<uint32_t>> sources;
    for(const auto i:nearCells) {
      sources.emplace_back();
      for(const auto t:lists[i])
        sources.back().push_back(table.triangleSources[t]);
      }
    auto nearRing = [&](bool glb) {
      for(size_t k=0;k<nearCells.size();++k)
        if(table.source->read(nearCells[k],sources[k],glb).mesh.polygons.material_indices.size()!=sources[k].size())
          throw std::runtime_error("a Near chunk reads back another triangle count");
      };
    const auto t2 = Clock::now();
    nearRing(true);
    series[TodayNear].values.push_back(ms(t2,Clock::now()));
    // The same reads on the cooked path, when the world is cooked beside its GLBs (the game ignores --out).
    const auto before = table.source->reads().cooked;
    const auto t3 = Clock::now();
    nearRing(false);
    const auto t4 = Clock::now();
    nearCooked = table.source->reads().cooked-before;
    if(nearCooked==nearCells.size())
      series[SourceNear].values.push_back(ms(t3,t4));

    // World cloth: the island search on a copy of the stitched mesh and table, then the bundle on the loaded ones.
    {
    auto searchedMesh = mesh;
    auto searchedTable = table;
    const auto c0 = Clock::now();
    const auto searched = WorldCloth::searchIslands(searchedMesh,&searchedTable);
    series[ClothSearch].values.push_back(ms(c0,Clock::now()));
    clothSurfaces  = searched.surfaces.size();
    clothTriangles = searched.original.polygons.material_indices.size();
    }
    if(bundled) {
      const auto c1 = Clock::now();
      const auto bundle = ClothBundle::read(clothPath);
      checkClothBundle(bundle,set);
      const auto c2 = Clock::now();
      const auto applied = applyClothBundle(bundle,mesh,table);
      const auto c3 = Clock::now();
      if(applied.surfaces.size()!=clothSurfaces || applied.original.polygons.material_indices.size()!=clothTriangles)
        throw std::runtime_error("the world cloth bundle holds other surfaces than the island search");
      series[ClothRead].values.push_back(ms(c1,c2));
      series[ClothApply].values.push_back(ms(c2,c3));
      }
    }
    if(!cooked)
      continue;

    auto share = [](std::vector<uint8_t>&& bytes) {
      return std::make_shared<const std::vector<uint8_t>>(std::move(bytes));
      };
    auto span = [](const std::shared_ptr<const std::vector<uint8_t>>& bytes) {
      return std::span<const uint8_t>(bytes->data(),bytes->size());
      };
    t0 = Clock::now();
    const auto indexBytesRead = share(gothicglb::readFile(indexPath));
    std::vector<std::shared_ptr<const std::vector<uint8_t>>> bytes;
    for(const auto& path:wcbs)
      bytes.push_back(share(gothicglb::readFile(path)));
    const auto t1 = Clock::now();
    CookedWorld world;
    world.dir = o.world;
    world.manifest = readWorldChunkManifest(o.world);
    world.index = CookedIndex::open(span(indexBytesRead),indexPath.string(),indexBytesRead);
    checkCookedIndex(world.index,world.manifest);
    for(size_t i=0;i<bytes.size();++i) {
      world.cells.push_back(CookedChunk::open(span(bytes[i]),wcbs[i].string(),bytes[i]));
      checkCookedCell(world.cells.back(),i,world.manifest,world.index,glbs[i]);
      }
    const auto t2 = Clock::now();
    ChunkTable assembled;
    const auto mesh = assembleCookedWorld(world,&assembled);
    const auto t3 = Clock::now();
    if(mesh.polygons.material_indices.size()!=triangles)
      throw std::runtime_error("the cooked world has another triangle count");
    series[ReadWcb].values.push_back(ms(t0,t1));
    series[Validate].values.push_back(ms(t1,t2));
    series[Assemble].values.push_back(ms(t2,t3));

    // The index alone, and a Near reader's view: the index, then only its cells.
    const auto t4 = Clock::now();
    const auto index = CookedIndex::read(indexPath);
    checkCookedIndex(index,manifest);
    const auto t5 = Clock::now();
    series[Index].values.push_back(ms(t4,t5));
    const auto nearIndex = CookedIndex::read(indexPath);
    checkCookedIndex(nearIndex,manifest);
    for(const auto i:nearCells) {
      const auto chunk = CookedChunk::read(wcbs[i]);
      checkCookedCell(chunk,i,manifest,nearIndex,glbs[i]);
      }
    series[CookedNear].values.push_back(ms(t5,Clock::now()));
    }

  std::printf("world %s: %zu cells, %zu triangles; GLB %llu bytes, .wcb %llu bytes, index %llu bytes%s\n",
              o.world.string().c_str(),manifest.cells.size(),triangles,static_cast<unsigned long long>(glbBytes),
              static_cast<unsigned long long>(wcbBytes),static_cast<unsigned long long>(indexBytes),
              cooked ? "" : " (not cooked: cooked path skipped)");
  if(cooked) {
    const auto index = CookedIndex::read(indexPath);
    size_t vertexExceptions = 0, featureExceptions = 0, materialExceptions = 0;
    for(size_t i=0;i<index.cells().size();++i) {
      vertexExceptions   += index.vertexExceptions(i).size();
      featureExceptions  += index.featureExceptions(i).size();
      materialExceptions += index.materialExceptions(i).size();
      }
    std::printf("index %s: %zu cells, %zu missing orders, exceptions: %zu vertex, %zu feature, %zu material; "
                "%zu appended materials\n",indexPath.string().c_str(),index.cells().size(),index.missing().size(),
                vertexExceptions,featureExceptions,materialExceptions,index.appendedMaterials());
    }
  std::printf("world cloth: %zu surfaces, %zu triangles by island search; bundle %s%s\n",clothSurfaces,clothTriangles,
              clothPath.string().c_str(),bundled ? (": "+std::to_string(fs::file_size(clothPath))+" bytes").c_str() :
              " missing (bundle rows skipped)");
  if(start!=nullptr) {
    uint64_t nearTriangles = 0, nearGlb = 0, nearWcb = 0;
    for(const auto i:nearCells) {
      nearTriangles += table.entries[i].triangleCount;
      nearGlb += fs::file_size(glbs[i]);
      if(cooked)
        nearWcb += fs::file_size(wcbs[i]);
      }
    std::printf("start point %s at (%.0f, %.0f, %.0f), cell %d,%d; Near ring 5 x 5: %zu cells, %llu triangles, "
                "GLB %llu bytes, .wcb %llu bytes\n",start->vob_name.c_str(),double(start->position.x),
                double(start->position.y),double(start->position.z),startCell.x,startCell.z,nearCells.size(),
                static_cast<unsigned long long>(nearTriangles),static_cast<unsigned long long>(nearGlb),
                static_cast<unsigned long long>(nearWcb));
    }
  else
    std::printf("no start point in %s\n",o.world.string().c_str());
  std::printf("wall ms over %d runs, warm page cache:\n| %-48s | %10s | %10s | %10s |\n|---|---:|---:|---:|\n",runs,
              "stage","median","min","max");
  for(const auto& s:series)
    if(!s.values.empty())
      printSeries(s);
    else if(&s==&series[SourceNear])
      std::printf("| %-48s | not cooked: %llu of %zu Near cells read their .wcb beside the GLB |\n",s.name.c_str(),
                  static_cast<unsigned long long>(nearCooked),nearCells.size());
  return 0;
  }

}

void roundTrip(const fs::path& dir, const fs::path& overrideDir, const fs::path& outDir) {
  cookWorldChunks(dir,overrideDir,outDir);
  const auto difference = verifyCookedWorld(dir,overrideDir,outDir,true);
  if(!difference.empty())
    throw std::runtime_error("cooked round trip of "+dir.string()+": "+difference);
  }

void clothRoundTrip(const fs::path& dir) {
  auto fail = [&](const std::string& reason) {
    throw std::runtime_error("world cloth round trip in "+dir.string()+": "+reason);
    };
  // A city banner across the border of cells 0,0 and 1,0 (four positions, four triangles, both sides) and a
  // ground triangle in cell 2,0.
  asset::WorldMesh mesh;
  mesh.materials.resize(2);
  mesh.materials[0].name = "SELF_TEST_GROUND";
  mesh.materials[1].name = "NW_CITY_FLAG_01";
  mesh.vertices = {{6300,600,100},{6500,600,100},{6500,300,100},{6300,300,100},{13000,0,0},{13100,0,0},{13000,0,100}};
  for(uint32_t i=0;i<7;++i)
    mesh.features.push_back({{float(i)*0.25f,0.5f},0xFF102030u+i,{0.f,0.f,1.f}});
  const uint32_t triangles[5][3] = {{0,1,2},{0,2,3},{0,2,1},{0,3,2},{4,5,6}};
  for(uint32_t t=0;t<5;++t) {
    for(const auto v:triangles[t]) {
      mesh.polygons.vertex_indices.push_back(v);
      mesh.polygons.feature_indices.push_back(v);
      }
    mesh.polygons.material_indices.push_back(t<4 ? 1 : 0);
    mesh.polygons.lightmap_indices.push_back(-1);
    mesh.polygons.flags.emplace_back();
    }
  writeWorldChunks(mesh,{"CLOTH.ZEN",5,0xC10751u},dir);
  roundTrip(dir,{},{});
  const auto bundle = ClothBundle::read(cookedClothPath(dir,{})).data();
  size_t excluded = 0, cells = 0;
  for(const auto& cell:bundle.exclusions) {
    excluded += cell.size();
    cells += cell.empty() ? 0 : 1;
    }
  if(bundle.cloth.surfaces.size()!=1 || excluded!=4 || cells!=2)
    fail(std::to_string(bundle.cloth.surfaces.size())+" surfaces, "+std::to_string(excluded)+" triangles out of "+
         std::to_string(cells)+" cells; expected one banner of 4 triangles across two cells");
  // The game's extract takes the bundle and gives the island search's result.
  ChunkTable cookedTable, searchedTable;
  auto cookedMesh = readWorldChunks(dir,{},&cookedTable);
  auto searchedMesh = cookedMesh;
  searchedTable = cookedTable;
  const auto taken = WorldCloth::extract(cookedMesh,&cookedTable);
  if(WorldCloth::lastPath()!=WorldCloth::Path::Bundle)
    fail("extract did not take the bundle");
  const auto searched = WorldCloth::searchIslands(searchedMesh,&searchedTable);
  auto difference = compareWorldCloth(searched,taken);
  if(difference.empty())
    difference = compareWorldMeshes(searchedMesh,cookedMesh);
  if(difference.empty())
    difference = compareChunkTables(searchedTable,cookedTable);
  if(!difference.empty())
    fail("extract with the bundle differs from the island search: "+difference);
  }

std::optional<int> run(const std::vector<std::string>& args) {
  if(args.empty() || (args[0]!="cook-world" && args[0]!="verify-cooked-world" && args[0]!="time-world"))
    return std::nullopt;
  const auto options = parse(args);
  if(!options) {
    std::cerr << "usage:\n" << usage;
    return 2;
    }
  if(args[0]=="cook-world")
    return cook(*options);
  if(args[0]=="verify-cooked-world")
    return verify(*options);
  return timeWorld(*options);
  }

}
