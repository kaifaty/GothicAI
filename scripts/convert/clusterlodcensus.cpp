#include "gothicglb.h"
#include "graphics/mesh/clusterlod.h"
#include "worldchunksruntime.h"

#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <limits>
#include <locale>
#include <map>
#include <sstream>
#include <stdexcept>

namespace {

using PositionKey = std::array<uint32_t, 3>;

PositionKey positionKey(const asset::Vec3& position) {
  const auto bits = [](float value) { return value==0.f ? 0u : std::bit_cast<uint32_t>(value); };
  return {bits(position.x),bits(position.y),bits(position.z)};
  }

std::vector<uint8_t> sharedPositions(const asset::WorldMesh& mesh, const ChunkTable& table) {
  constexpr uint32_t Shared = std::numeric_limits<uint32_t>::max();
  const auto& vertices = mesh.polygons.vertex_indices;
  std::map<PositionKey,uint32_t> owners;
  // The chunk writer assigns whole triangles by centroid. A geometric seam can be off the grid line.
  for(size_t t=0; t<table.triangleChunks.size(); ++t) {
    const auto owner = table.triangleChunks[t];
    for(size_t corner=0; corner<3; ++corner) {
      const auto position = positionKey(mesh.vertices[vertices[t*3+corner]]);
      const auto [it,inserted] = owners.try_emplace(position,owner);
      if(!inserted && it->second!=owner)
        it->second = Shared;
      }
    }

  std::vector<uint8_t> shared(mesh.vertices.size());
  for(size_t i=0; i<mesh.vertices.size(); ++i) {
    const auto owner = owners.find(positionKey(mesh.vertices[i]));
    shared[i] = owner!=owners.end() && owner->second==Shared;
    }
  return shared;
  }

struct LevelCounts {
  size_t triangles = 0;
  size_t clusters = 0;
  };

struct Counts {
  std::vector<LevelCounts> levels;
  size_t                  top = 0;
  size_t                  topBoundary = 0;
  double                  buildMs = 0;
  size_t                  bytes = 0;

  void add(const Counts& cell) {
    if(levels.size()<cell.levels.size())
      levels.resize(cell.levels.size());
    for(size_t i=0; i<cell.levels.size(); ++i) {
      levels[i].triangles += cell.levels[i].triangles;
      levels[i].clusters  += cell.levels[i].clusters;
      }
    top         += cell.top;
    topBoundary += cell.topBoundary;
    buildMs     += cell.buildMs;
    bytes       += cell.bytes;
    }
  };

Counts countCell(const asset::WorldMesh& mesh, std::span<const uint32_t> sourceTriangles, std::span<const uint8_t> shared) {
  std::map<std::pair<uint32_t,uint32_t>,uint32_t> local;
  std::vector<ClusterLod::Vertex> vertices;
  std::vector<ClusterLod::Triangle> triangles;
  std::vector<uint8_t> boundary;
  triangles.reserve(sourceTriangles.size());
  for(const auto source : sourceTriangles) {
    ClusterLod::Triangle tri {{},mesh.polygons.material_indices[source]};
    for(size_t corner=0; corner<3; ++corner) {
      const size_t index = size_t(source)*3+corner;
      const auto v = mesh.polygons.vertex_indices[index];
      const auto f = mesh.polygons.feature_indices[index];
      const auto [it,inserted] = local.try_emplace({v,f},uint32_t(vertices.size()));
      if(inserted) {
        const auto& p = mesh.vertices[v];
        const auto& feature = mesh.features[f];
        const auto c = feature.light; // AARRGGBB in the stitched mesh
        const auto rgba = (c & 0xff00ff00u) | ((c & 0x00ff0000u) >> 16) | ((c & 0x000000ffu) << 16);
        vertices.push_back({{p.x,p.y,p.z},{feature.normal.x,feature.normal.y,feature.normal.z},
                            {feature.texture.x,feature.texture.y},rgba});
        boundary.push_back(shared[v]);
        }
      tri.vertices[corner] = it->second;
      }
    triangles.push_back(tri);
    }

  const auto started = std::chrono::steady_clock::now();
  const auto graph = ClusterLod::build(vertices,triangles,boundary);
  Counts counts;
  counts.buildMs = std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count();
  counts.bytes = graph.byteSize();
  for(const auto& cluster : graph.clusters) {
    if(counts.levels.size()<=cluster.level)
      counts.levels.resize(size_t(cluster.level)+1);
    auto& level = counts.levels[cluster.level];
    level.triangles += cluster.triangleCount;
    ++level.clusters;

    if(!std::isinf(graph.groups[cluster.parentGroup].errorCm))
      continue;
    counts.top += cluster.triangleCount;
    for(size_t t=0; t<cluster.triangleCount; ++t) {
      bool touches = false;
      for(size_t corner=0; corner<3; ++corner) {
        const auto index = graph.indices[cluster.indexOffset+t*3+corner];
        touches |= boundary[graph.vertexIndices[cluster.vertexOffset+index]]!=0;
        }
      counts.topBoundary += touches;
      }
    }
  return counts;
  }

void writeRow(std::ostream& out, const std::string& cell, const Counts& counts, size_t levelCount) {
  const double fraction = counts.top==0 ? 0.0 : double(counts.topBoundary)/double(counts.top);
  out << cell << '\t' << counts.levels.size() << '\t' << counts.top << '\t' << counts.topBoundary << '\t'
      << std::fixed << std::setprecision(6) << fraction << '\t' << std::setprecision(3) << counts.buildMs << '\t' << counts.bytes;
  for(size_t i=0; i<levelCount; ++i) {
    const auto level = i<counts.levels.size() ? counts.levels[i] : LevelCounts{};
    out << '\t' << level.triangles << '\t' << level.clusters;
    }
  out << '\n';
  }

}

void runClusterLodCensus(const std::filesystem::path& world, const std::filesystem::path& output) {
  ChunkTable table;
  const auto mesh = readWorldChunksGlb(world,{},table);
  const auto lists = chunkTriangleLists(mesh,table);
  const auto shared = sharedPositions(mesh,table);
  std::vector<Counts> cells;
  cells.reserve(lists.size());
  Counts total;
  for(size_t i=0; i<lists.size(); ++i) {
    try {
      cells.push_back(countCell(mesh,lists[i],shared));
      }
    catch(const std::exception& error) {
      throw std::runtime_error(table.entries[i].file + ": " + error.what());
      }
    total.add(cells.back());
    }

  std::ostringstream out;
  out.imbue(std::locale::classic());
  out << "chunk\tlevels\ttop_triangles\ttop_boundary_triangles\ttop_boundary_fraction\tbuild_ms\tgraph_bytes";
  for(size_t i=0; i<total.levels.size(); ++i)
    out << "\tL" << i << "_triangles\tL" << i << "_clusters";
  out << '\n';
  for(size_t i=0; i<cells.size(); ++i) {
    const auto id = table.entries[i].id;
    writeRow(out,std::to_string(id.x)+"_"+std::to_string(id.z),cells[i],total.levels.size());
    }
  writeRow(out,"TOTAL",total,total.levels.size());

  const auto report = out.str();
  if(output.empty()) {
    std::cout << report;
    if(!std::cout)
      throw std::runtime_error("cannot write cluster LOD census to stdout");
    }
  else {
    gothicglb::writeFile(output,report.data(),report.size());
    }
  }
