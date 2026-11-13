#include "worldcook.h"

#include "gothicglb.h"
#include "worldchunkcook.h"
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
              "matches its chunk's length and SHA-256 (%.1f s)\n",o.world.string().c_str(),manifest.cells.size(),
              ms(started,Clock::now())/1000.0);
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

  enum { ReadGlb, Today, Parse, Sort, Stitch, Rest, TodayNear, ReadWcb, Validate, Index, Assemble, CookedNear };
  std::vector<Series> series = {
    {"today: read GLB files (separate pass)",{}},
    {"today: readWorldChunks total",{}},
    {"  read + cgltf parse + attribute extraction",{}},
    {"  source-order sort",{}},
    {"  ID resolution, dedup maps, stitching, table",{}},
    {"  rest: trace copy, teardown of reader and sets",{}},
    {"today: ChunkMeshSource::read of the Near ring",{}},
    {"cooked: read .wcb files and the index",{}},
    {"cooked: validate (open + index + world.json + GLB stat)",{}},
    {"cooked: read + validate the index alone",{}},
    {"cooked: assemble whole mesh and table",{}},
    {"cooked: read + validate the index and the Near ring",{}},
    };
  ChunkTable table;
  size_t triangles = 0;
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
    const auto mesh = readWorldChunks(o.world,o.content,&table,&trace);
    const auto t1 = Clock::now();
    triangles = mesh.polygons.material_indices.size();
    series[Today].values.push_back(ms(t0,t1));
    series[Parse].values.push_back(ms(t0,trace.parsed));
    series[Sort].values.push_back(ms(trace.parsed,trace.ordered));
    series[Stitch].values.push_back(ms(trace.ordered,trace.stitched));
    series[Rest].values.push_back(ms(trace.stitched,t1));
    // The game's streaming reader today: one GLB per chunk, IDs from the stitched world's tables.
    const auto lists = chunkTriangleLists(mesh,table);
    const auto t2 = Clock::now();
    for(const auto i:nearCells) {
      std::vector<uint32_t> sources;
      for(const auto t:lists[i])
        sources.push_back(table.triangleSources[t]);
      if(table.source->read(i,sources).mesh.polygons.material_indices.size()!=sources.size())
        throw std::runtime_error("a Near chunk reads back another triangle count");
      }
    series[TodayNear].values.push_back(ms(t2,Clock::now()));
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
  return 0;
  }

}

void roundTrip(const fs::path& dir, const fs::path& overrideDir, const fs::path& outDir) {
  cookWorldChunks(dir,overrideDir,outDir);
  const auto difference = verifyCookedWorld(dir,overrideDir,outDir,true);
  if(!difference.empty())
    throw std::runtime_error("cooked round trip of "+dir.string()+": "+difference);
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
