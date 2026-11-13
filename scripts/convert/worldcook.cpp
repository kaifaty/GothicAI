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
  "       content-convert time-world <world dir> [--content <dir>] [--out <dir>] [--runs N]\n";

namespace {

using Clock = std::chrono::steady_clock;

double ms(Clock::time_point a, Clock::time_point b) {
  return std::chrono::duration<double,std::milli>(b-a).count();
  }

struct Options {
  fs::path world, content, out;
  int      runs = 5;
  };

std::optional<Options> parse(const std::vector<std::string>& args, bool timing) {
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
    else if(timing && args[i]=="--runs" && args[i+1].size()==1 && args[i+1][0]>='1' && args[i+1][0]<='9')
      o.runs = args[i+1][0]-'0';
    else
      return std::nullopt;
    }
  return o;
  }

int cook(const Options& o) {
  const auto started = Clock::now();
  const auto stats = cookWorldChunks(o.world,o.content,o.out);
  std::printf("%s: %zu cells, %llu triangles cooked; GLB %llu bytes, .wcb %llu bytes (%.1f s)\n",
              o.world.string().c_str(),stats.cells,static_cast<unsigned long long>(stats.triangles),
              static_cast<unsigned long long>(stats.glbBytes),static_cast<unsigned long long>(stats.wcbBytes),
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
  std::printf("%s: %zu cooked cells assemble into readWorldChunks' mesh and chunk table; every GLB matches its "
              "chunk's length and SHA-256 (%.1f s)\n",o.world.string().c_str(),manifest.cells.size(),
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
  std::printf("| %-44s | %10.1f | %10.1f | %10.1f |\n",s.name.c_str(),v[v.size()/2],v.front(),v.back());
  }

int timeWorld(const Options& o) {
  const auto manifest = readWorldChunkManifest(o.world);
  std::vector<fs::path> glbs, wcbs;
  uint64_t glbBytes = 0, wcbBytes = 0;
  bool cooked = true;
  for(const auto& cell:manifest.cells) {
    glbs.push_back(chunkGlbPath(o.world,o.content,cell.drawFile()));
    wcbs.push_back(cookedChunkPath(o.world,o.content,cell.drawFile(),o.out));
    glbBytes += fs::file_size(glbs.back());
    if(fs::exists(wcbs.back()))
      wcbBytes += fs::file_size(wcbs.back());
    else
      cooked = false;
    }

  // Warm the page cache once: every run below reads the same files from memory.
  for(const auto* list:{&glbs,&wcbs})
    for(const auto& path:*list)
      if(fs::exists(path))
        (void)gothicglb::readFile(path);

  std::vector<Series> series = {
    {"today: read GLB files (separate pass)",{}},
    {"today: readWorldChunks total",{}},
    {"  read + cgltf parse + attribute extraction",{}},
    {"  source-order sort",{}},
    {"  ID resolution, dedup maps, stitching, table",{}},
    {"cooked: read .wcb files",{}},
    {"cooked: validate (open + world.json + GLB stat)",{}},
    {"cooked: assemble whole mesh and table",{}},
    };
  ChunkTable table;
  size_t triangles = 0;
  for(int run=0;run<o.runs;++run) {
    auto t0 = Clock::now();
    uint64_t read = 0;
    for(const auto& path:glbs)
      read += gothicglb::readFile(path).size();
    series[0].values.push_back(ms(t0,Clock::now()));
    if(read!=glbBytes)
      throw std::runtime_error("GLB files changed while timing");

    {
    WorldChunkTrace trace;
    table = {};
    t0 = Clock::now();
    const auto mesh = readWorldChunks(o.world,o.content,&table,&trace);
    const auto t1 = Clock::now();
    triangles = mesh.polygons.material_indices.size();
    series[1].values.push_back(ms(t0,t1));
    series[2].values.push_back(ms(t0,trace.parsed));
    series[3].values.push_back(ms(trace.parsed,trace.ordered));
    series[4].values.push_back(ms(trace.ordered,trace.stitched));
    }
    if(!cooked)
      continue;

    t0 = Clock::now();
    std::vector<std::shared_ptr<const std::vector<uint8_t>>> bytes;
    for(const auto& path:wcbs)
      bytes.push_back(std::make_shared<const std::vector<uint8_t>>(gothicglb::readFile(path)));
    const auto t1 = Clock::now();
    CookedWorld world;
    world.dir = o.world;
    world.manifest = readWorldChunkManifest(o.world);
    for(size_t i=0;i<bytes.size();++i) {
      const std::span<const uint8_t> view(bytes[i]->data(),bytes[i]->size());
      world.cells.push_back(CookedChunk::open(view,wcbs[i].string(),bytes[i]));
      checkCookedCell(world.cells.back(),world.manifest.cells[i],world.manifest,glbs[i]);
      }
    const auto t2 = Clock::now();
    ChunkTable assembled;
    const auto mesh = assembleCookedWorld(world,&assembled);
    const auto t3 = Clock::now();
    if(mesh.polygons.material_indices.size()!=triangles)
      throw std::runtime_error("the cooked world has another triangle count");
    series[5].values.push_back(ms(t0,t1));
    series[6].values.push_back(ms(t1,t2));
    series[7].values.push_back(ms(t2,t3));
    }

  std::printf("world %s: %zu cells, %zu triangles; GLB %llu bytes, .wcb %llu bytes%s\n",o.world.string().c_str(),
              manifest.cells.size(),triangles,static_cast<unsigned long long>(glbBytes),
              static_cast<unsigned long long>(wcbBytes),cooked ? "" : " (not cooked: cooked path skipped)");
  const auto data = WorldData::read(o.world);
  if(const auto* start=startPoint(data.vobs)) {
    const auto cell = ChunkId::fromPosition(start->position.x,start->position.z);
    uint64_t nearTriangles = 0, nearGlb = 0, nearWcb = 0;
    size_t nearCells = 0;
    for(size_t i=0;i<table.entries.size();++i) {
      const auto& e = table.entries[i];
      if(std::abs(e.id.x-cell.x)>2 || std::abs(e.id.z-cell.z)>2)
        continue;
      ++nearCells;
      nearTriangles += e.triangleCount;
      nearGlb += fs::file_size(glbs[i]);
      if(cooked)
        nearWcb += fs::file_size(wcbs[i]);
      }
    std::printf("start point %s at (%.0f, %.0f, %.0f), cell %d,%d; Near ring 5 x 5: %zu cells, %llu triangles, "
                "GLB %llu bytes, .wcb %llu bytes\n",start->vob_name.c_str(),double(start->position.x),
                double(start->position.y),double(start->position.z),cell.x,cell.z,nearCells,
                static_cast<unsigned long long>(nearTriangles),static_cast<unsigned long long>(nearGlb),
                static_cast<unsigned long long>(nearWcb));
    }
  else
    std::printf("no start point in %s\n",o.world.string().c_str());
  std::printf("wall ms over %d runs, warm page cache:\n| %-44s | %10s | %10s | %10s |\n|---|---:|---:|---:|\n",o.runs,
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
  const auto options = parse(args,args[0]=="time-world");
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
