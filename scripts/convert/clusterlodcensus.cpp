#include "gothicglb.h"
#include "graphics/mesh/clusterlod.h"
#include "gltfmesh.h"
#include "meshclusterlod.h"
#include "stressmeshes.h"
#include "utils/fileext.h"
#include "worldchunkcook.h"
#include "worldchunksruntime.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cctype>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <limits>
#include <locale>
#include <map>
#include <numbers>
#include <numeric>
#include <set>
#include <sstream>
#include <stdexcept>

#include <miniz.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

namespace {

using PositionKey = std::array<uint32_t, 3>;
using Edge = std::array<uint32_t, 2>;

PositionKey positionKey(const asset::Vec3& position) {
  const auto bits = [](float value) { return value==0.f ? 0u : std::bit_cast<uint32_t>(value); };
  return {bits(position.x),bits(position.y),bits(position.z)};
  }

struct PositionUse {
  size_t chunk;
  bool   open;
  bool   sharedSourceVertex;
  };

struct EdgeUse {
  size_t chunk;
  bool   open;
  };

struct Topology {
  explicit Topology(const asset::WorldMesh& mesh, const ChunkTable& table);

  std::pair<std::set<uint32_t>, size_t> inspectCell(const asset::WorldMesh& mesh, std::span<const uint32_t> triangles,
                                                   size_t chunk);
  void writeSharedInterior(std::ostream& out, const ChunkTable& table) const;

  std::vector<uint32_t> vertexPositions;

  private:
  static constexpr uint32_t NoOwner = std::numeric_limits<uint32_t>::max();
  static constexpr uint32_t Shared = NoOwner-1;
  std::vector<PositionKey> positions;
  std::vector<uint32_t> owners;
  std::vector<uint32_t> sourceVertexOwners;
  std::map<uint32_t,std::vector<PositionUse>> sharedPositions;
  std::map<Edge,std::vector<EdgeUse>> sharedEdges;
  };

Topology::Topology(const asset::WorldMesh& mesh, const ChunkTable& table) {
  std::map<PositionKey,uint32_t> unique;
  vertexPositions.reserve(mesh.vertices.size());
  for(const auto& vertex : mesh.vertices) {
    const auto position = positionKey(vertex);
    const auto [it,inserted] = unique.try_emplace(position,uint32_t(positions.size()));
    if(inserted)
      positions.push_back(position);
    vertexPositions.push_back(it->second);
    }

  owners.resize(positions.size(),NoOwner);
  sourceVertexOwners.resize(mesh.vertices.size(),NoOwner);
  const auto addOwner = [](uint32_t& owner, uint32_t chunk) {
    if(owner==NoOwner)
      owner = chunk;
    else if(owner!=chunk)
      owner = Shared;
    };
  // A cell owns whole triangles by centroid. Its open edge chain can be off the grid line.
  for(size_t t=0; t<table.triangleChunks.size(); ++t) {
    const auto chunk = table.triangleChunks[t];
    for(size_t corner=0; corner<3; ++corner) {
      const auto vertex = mesh.polygons.vertex_indices[t*3+corner];
      addOwner(owners[vertexPositions[vertex]],chunk);
      addOwner(sourceVertexOwners[vertex],chunk);
      }
    }
  }

std::pair<std::set<uint32_t>, size_t> Topology::inspectCell(const asset::WorldMesh& mesh,
                                                           std::span<const uint32_t> triangles, size_t chunk) {
  std::map<Edge,int> edges;
  std::map<uint32_t,bool> shared;
  for(const auto triangle : triangles) {
    std::array<uint32_t,3> p;
    for(size_t corner=0; corner<3; ++corner) {
      const auto vertex = mesh.polygons.vertex_indices[size_t(triangle)*3+corner];
      p[corner] = vertexPositions[vertex];
      if(owners[p[corner]]==Shared)
        shared[p[corner]] |= sourceVertexOwners[vertex]==Shared;
      }
    for(size_t corner=0; corner<3; ++corner) {
      const auto a = p[corner], b = p[(corner+1)%3];
      if(a!=b)
        edges[{std::min(a,b),std::max(a,b)}] += a<b ? 1 : -1;
      }
    }

  std::set<uint32_t> open;
  for(const auto& [edge,winding] : edges) {
    if(winding!=0)
      open.insert(edge.begin(),edge.end());
    if(owners[edge[0]]==Shared && owners[edge[1]]==Shared)
      sharedEdges[edge].push_back({chunk,winding!=0});
    }
  size_t interior = 0;
  for(const auto& [p,sharedSourceVertex] : shared) {
    const auto isOpen = open.contains(p);
    sharedPositions[p].push_back({chunk,isOpen,sharedSourceVertex});
    interior += !isOpen;
    }
  return {std::move(open),interior};
  }

void Topology::writeSharedInterior(std::ostream& out, const ChunkTable& table) const {
  std::set<std::pair<uint32_t,size_t>> meetsOpenEdge;
  for(const auto& [edge,uses] : sharedEdges) {
    if(std::ranges::none_of(uses,[](const auto& use) { return use.open; }))
      continue;
    for(const auto& use : uses) {
      if(!use.open) {
        for(const auto p : edge)
          meetsOpenEdge.emplace(p,use.chunk);
        }
      }
    }

  size_t unique = 0, occurrences = 0, sameSourceVertex = 0, meetsOpenPosition = 0, onOpenEdge = 0;
  std::vector<uint32_t> examples;
  for(const auto& [p,uses] : sharedPositions) {
    const bool anyOpen = std::ranges::any_of(uses,[](const auto& use) { return use.open; });
    bool anyInterior = false;
    for(const auto& use : uses) {
      if(use.open)
        continue;
      anyInterior = true;
      ++occurrences;
      sameSourceVertex += use.sharedSourceVertex;
      meetsOpenPosition += anyOpen;
      onOpenEdge += meetsOpenEdge.contains({p,use.chunk});
      }
    if(anyInterior) {
      ++unique;
      if(examples.size()<5)
        examples.push_back(p);
      }
    }

  out << "\nshared_interior_metric\tcount\n"
      << "unique_positions\t" << unique << '\n'
      << "chunk_occurrences\t" << occurrences << '\n'
      << "occurrences_sharing_source_vertex_index\t" << sameSourceVertex << '\n'
      << "occurrences_on_another_chunk_open_position\t" << meetsOpenPosition << '\n'
      << "occurrences_on_same_incident_edge_open_in_another_chunk\t" << onOpenEdge << '\n'
      << "\nshared_interior_example\tx_cm\ty_cm\tz_cm\tchunks\n"
      << std::defaultfloat << std::setprecision(std::numeric_limits<float>::max_digits10);
  for(size_t i=0; i<examples.size(); ++i) {
    const auto p = examples[i];
    out << i+1;
    for(const auto bits : positions[p])
      out << '\t' << std::bit_cast<float>(bits);
    out << '\t';
    const auto& uses = sharedPositions.at(p);
    for(size_t j=0; j<uses.size(); ++j) {
      const auto id = table.entries[uses[j].chunk].id;
      if(j!=0)
        out << ',';
      out << id.x << '_' << id.z << (uses[j].open ? "(open)" : "(interior)");
      }
    out << '\n';
    }
  }

struct LevelCounts {
  size_t triangles = 0;
  size_t clusters = 0;
  };

struct LevelErrors {
  std::vector<float> position, full;
  std::vector<double> positionDistance, fullDistance;

  void add(float positionCm, const ClusterLod::Group& group);
  };

constexpr double VerticalFov = 67.5; // Settings::loadSystemPackOptions default, without aim zoom.
constexpr double ViewportHeight = 1440;

double verticalProjection() {
  return 1./std::tan(VerticalFov*std::numbers::pi_v<double>/360.);
  }

void LevelErrors::add(float positionCm, const ClusterLod::Group& group) {
  const double pixels = 0.5*ViewportHeight*verticalProjection();
  const auto distance = [&](float errorCm) {
    const double nearCm = pixels*errorCm;
    // The projected error uses a 1 cm lower bound on the near distance.
    return nearCm<=1. ? 0. : (group.bounds.radius+nearCm)/100.;
    };
  position.push_back(positionCm);
  full.push_back(group.errorCm);
  positionDistance.push_back(distance(positionCm));
  fullDistance.push_back(distance(group.errorCm));
  }

struct Counts {
  std::vector<LevelCounts> levels;
  size_t                  top = 0;
  size_t                  topBoundary = 0;
  size_t                  sharedInterior = 0;
  double                  buildMs = 0;
  size_t                  bytes = 0;

  void add(const Counts& cell) {
    if(levels.size()<cell.levels.size())
      levels.resize(cell.levels.size());
    for(size_t i=0; i<cell.levels.size(); ++i) {
      levels[i].triangles += cell.levels[i].triangles;
      levels[i].clusters  += cell.levels[i].clusters;
      }
    top            += cell.top;
    topBoundary    += cell.topBoundary;
    sharedInterior += cell.sharedInterior;
    buildMs        += cell.buildMs;
    bytes          += cell.bytes;
    }
  };

struct CellInput {
  std::vector<ClusterLod::Vertex> vertices;
  std::vector<ClusterLod::Triangle> triangles;
  std::vector<uint32_t> positions;
  };

CellInput cellInput(const asset::WorldMesh& mesh, std::span<const uint32_t> sourceTriangles, const Topology& topology,
                    std::span<const uint8_t> lockedMaterials) {
  std::map<std::pair<uint32_t,uint32_t>,uint32_t> local;
  CellInput input;
  input.triangles.reserve(sourceTriangles.size());
  for(const auto source : sourceTriangles) {
    const auto material = mesh.polygons.material_indices[source];
    ClusterLod::Triangle tri {{},material,lockedMaterials[material]!=0};
    for(size_t corner=0; corner<3; ++corner) {
      const size_t index = size_t(source)*3+corner;
      const auto v = mesh.polygons.vertex_indices[index];
      const auto f = mesh.polygons.feature_indices[index];
      const auto [it,inserted] = local.try_emplace({v,f},uint32_t(input.vertices.size()));
      if(inserted) {
        const auto& p = mesh.vertices[v];
        const auto& feature = mesh.features[f];
        const auto c = feature.light; // AARRGGBB in the stitched mesh
        const auto rgba = (c & 0xff00ff00u) | ((c & 0x00ff0000u) >> 16) | ((c & 0x000000ffu) << 16);
        input.vertices.push_back({{p.x,p.y,p.z},{feature.normal.x,feature.normal.y,feature.normal.z},
                                  {feature.texture.x,feature.texture.y},rgba});
        input.positions.push_back(topology.vertexPositions[v]);
        }
      tri.vertices[corner] = it->second;
      }
    input.triangles.push_back(tri);
    }
  return input;
  }

Counts countCell(const CellInput& input, const std::set<uint32_t>& open, size_t sharedInterior,
                 std::vector<LevelErrors>& errors, ClusterLod::BuildStats& totals) {
  const std::vector<uint8_t> boundary(input.vertices.size());
  ClusterLod::BuildStats stats;
  const auto started = std::chrono::steady_clock::now();
  const auto graph = ClusterLod::buildWithStats(input.vertices,input.triangles,boundary,stats);
  Counts counts;
  counts.buildMs = std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count();
  counts.bytes = graph.byteSize();
  counts.sharedInterior = sharedInterior;
  for(size_t i=0; i<stats.stoppedGroups.size(); ++i) {
    totals.stoppedGroups[i] += stats.stoppedGroups[i];
    totals.stoppedTriangles[i] += stats.stoppedTriangles[i];
    }
  std::vector<bool> seen(graph.groups.size());
  for(const auto& cluster : graph.clusters) {
    if(counts.levels.size()<=cluster.level)
      counts.levels.resize(size_t(cluster.level)+1);
    auto& level = counts.levels[cluster.level];
    level.triangles += cluster.triangleCount;
    ++level.clusters;

    if(cluster.refinedGroup!=ClusterLod::NoGroup && !seen[cluster.refinedGroup]) {
      seen[cluster.refinedGroup] = true;
      if(errors.size()<=cluster.level)
        errors.resize(size_t(cluster.level)+1);
      errors[cluster.level].add(stats.positionErrorCm[cluster.refinedGroup],graph.groups[cluster.refinedGroup]);
      }

    if(!std::isinf(graph.groups[cluster.parentGroup].errorCm))
      continue;
    counts.top += cluster.triangleCount;
    for(size_t t=0; t<cluster.triangleCount; ++t) {
      bool touches = false;
      for(size_t corner=0; corner<3; ++corner) {
        const auto index = graph.indices[cluster.indexOffset+t*3+corner];
        touches |= open.contains(input.positions[graph.vertexIndices[cluster.vertexOffset+index]]);
        }
      counts.topBoundary += touches;
      }
    }
  return counts;
  }

void writeRow(std::ostream& out, const std::string& cell, const Counts& counts, size_t levelCount) {
  const double fraction = counts.top==0 ? 0.0 : double(counts.topBoundary)/double(counts.top);
  out << cell << '\t' << counts.levels.size() << '\t' << counts.top << '\t' << counts.topBoundary << '\t'
      << std::fixed << std::setprecision(6) << fraction << '\t' << counts.sharedInterior << '\t'
      << std::setprecision(3) << counts.buildMs << '\t' << counts.bytes;
  for(size_t i=0; i<levelCount; ++i) {
    const auto level = i<counts.levels.size() ? counts.levels[i] : LevelCounts{};
    out << '\t' << level.triangles << '\t' << level.clusters;
    }
  out << '\n';
  }

template<class T>
void writePercentiles(std::ostream& out, std::vector<T>& values) {
  if(values.empty()) {
    out << "\t0\t0\t0";
    return;
    }
  std::sort(values.begin(),values.end());
  const double median = (double(values[(values.size()-1)/2])+values[values.size()/2])*0.5;
  const size_t p90 = values.size()-values.size()/10-1;
  out << '\t' << median << '\t' << values[p90] << '\t' << values.back();
  }

void writeErrors(std::ostream& out, std::vector<LevelErrors>& errors, size_t levelCount) {
  out << "\nviewport_width\tviewport_height\tvertical_fov_deg\tvertical_projection\tthreshold_px\n"
      << "2560\t" << ViewportHeight << '\t' << VerticalFov << '\t' << std::setprecision(6) << verticalProjection() << "\t1\n";
  out << "\nlevel\taccepted_groups"
         "\tposition_error_cm_p50\tposition_error_cm_p90\tposition_error_cm_max"
         "\tfull_error_cm_p50\tfull_error_cm_p90\tfull_error_cm_max"
         "\tposition_enable_distance_m_p50\tposition_enable_distance_m_p90\tposition_enable_distance_m_max"
         "\tfull_enable_distance_m_p50\tfull_enable_distance_m_p90\tfull_enable_distance_m_max\n"
      << std::setprecision(3);
  errors.resize(levelCount);
  for(size_t i=0; i<errors.size(); ++i) {
    auto& level = errors[i];
    out << i << '\t' << level.full.size();
    writePercentiles(out,level.position);
    writePercentiles(out,level.full);
    writePercentiles(out,level.positionDistance);
    writePercentiles(out,level.fullDistance);
    out << '\n';
    }
  }

void writeRelaxations(std::ostream& out, std::span<const CellInput> cells, const Counts& total) {
  if(total.levels.empty() || total.top*10<=total.levels[0].triangles*3)
    return;
  using ClusterLod::CensusVariant;
  constexpr std::pair<const char*,CensusVariant> variants[] = {
    {"without_materials",CensusVariant::NoMaterials},
    {"without_open_border",CensusVariant::NoOpenBorder},
    {"without_boundaries",CensusVariant::NoBoundaries},
    {"without_attribute_seams",CensusVariant::NoAttributeSeams},
    {"accept_any_reduction",CensusVariant::AnyReduction},
    {"positions_only",CensusVariant::PositionsOnly},
    };
  const auto fraction = [&](size_t top) { return double(top)/double(total.levels[0].triangles); };
  out << "\nrelaxation\ttop_triangles\tsaved_top_triangles\ttop_fraction_of_level_0\n"
      << std::fixed << std::setprecision(6)
      << "nominal\t" << total.top << "\t0\t" << fraction(total.top) << '\n';
  for(const auto& [name,variant] : variants) {
    size_t top = 0;
    for(const auto& cell : cells) {
      const std::vector<uint8_t> boundary(cell.vertices.size());
      ClusterLod::BuildStats stats;
      ClusterLod::buildWithStats(cell.vertices,cell.triangles,boundary,stats,variant);
      top += std::accumulate(stats.stoppedTriangles.begin(),stats.stoppedTriangles.end(),size_t(0));
      }
    out << name << '\t' << top << '\t' << int64_t(total.top)-int64_t(top) << '\t' << fraction(top) << '\n';
    }
  }

}

void runClusterLodCensus(const std::filesystem::path& world, const std::filesystem::path& output) {
  ChunkTable table;
  const auto mesh = readWorldChunksGlb(world,{},table);
  const auto lockedMaterials = clusterLodLockedMaterials(world,mesh.materials);
  const auto lists = chunkTriangleLists(mesh,table);
  Topology topology(mesh,table);
  std::vector<CellInput> inputs;
  inputs.reserve(lists.size());
  std::vector<Counts> cells;
  cells.reserve(lists.size());
  Counts total;
  std::vector<LevelErrors> errors;
  ClusterLod::BuildStats stats;
  for(size_t i=0; i<lists.size(); ++i) {
    try {
      const auto [open,sharedInterior] = topology.inspectCell(mesh,lists[i],i);
      inputs.push_back(cellInput(mesh,lists[i],topology,lockedMaterials));
      cells.push_back(countCell(inputs.back(),open,sharedInterior,errors,stats));
      }
    catch(const std::exception& error) {
      throw std::runtime_error(table.entries[i].file + ": " + error.what());
      }
    total.add(cells.back());
    }

  std::ostringstream out;
  out.imbue(std::locale::classic());
  out << "chunk\tlevels\ttop_triangles\ttop_boundary_triangles\ttop_boundary_fraction\tshared_interior_positions\tbuild_ms\tgraph_bytes";
  for(size_t i=0; i<total.levels.size(); ++i)
    out << "\tL" << i << "_triangles\tL" << i << "_clusters";
  out << '\n';
  for(size_t i=0; i<cells.size(); ++i) {
    const auto id = table.entries[i].id;
    writeRow(out,std::to_string(id.x)+"_"+std::to_string(id.z),cells[i],total.levels.size());
    }
  writeRow(out,"TOTAL",total,total.levels.size());
  writeErrors(out,errors,total.levels.size());
  out << "\nstop_reason\tgroups\ttop_triangles\n";
  constexpr std::array reasonNames = {"reduction","material","boundary","locked"};
  static_assert(reasonNames.size()==size_t(ClusterLod::StopReason::Count));
  for(size_t i=0; i<reasonNames.size(); ++i)
    out << reasonNames[i] << '\t' << stats.stoppedGroups[i] << '\t' << stats.stoppedTriangles[i] << '\n';
  topology.writeSharedInterior(out,table);
  writeRelaxations(out,inputs,total);

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

namespace {

namespace fs = std::filesystem;

struct OwnMesh {
  std::string name;
  fs::path relative;
  bool discrete = false;
  };

rapidjson::Document readIndex(const fs::path& path) {
  const auto bytes = gothicglb::readFile(path);
  rapidjson::Document doc;
  doc.Parse<rapidjson::kParseValidateEncodingFlag>(reinterpret_cast<const char*>(bytes.data()),bytes.size());
  if(doc.HasParseError() || !doc.IsObject())
    throw std::runtime_error("invalid index "+path.string());
  return doc;
  }

std::vector<OwnMesh> ownMeshes(const fs::path& root) {
  const auto index = root/"meshes"/"meshes.json";
  const auto doc = readIndex(index);
  std::vector<OwnMesh> meshes;
  for(const auto& member:doc.GetObject()) {
    const auto& value = member.value;
    if(!value.IsObject() || !value.HasMember("glb") || !value["glb"].IsString())
      throw std::runtime_error("invalid GLB record "+std::string(member.name.GetString())+" in "+index.string());
    const std::string name(value["glb"].GetString(),value["glb"].GetStringLength());
    const fs::path path(name);
    if(path.is_absolute() || path.has_root_name() || name.find('\0')!=std::string::npos || !FileExt::hasExt(name,"GLB") ||
       std::find(path.begin(),path.end(),"..")!=path.end())
      throw std::runtime_error("invalid GLB path "+name+" in "+index.string());
    meshes.push_back({member.name.GetString(),path,value.HasMember("levels") || value.HasMember("impostor")});
    }
  return meshes;
  }

enum MaterialLock : uint8_t { TwoSided = 1, Deforming = 2 };

std::string materialKey(std::string name) {
  for(char& c:name)
    c = char(std::toupper(static_cast<unsigned char>(c)));
  return name;
  }

std::map<std::string,uint8_t> ownMaterialLocks(const fs::path& root) {
  const auto index = root/"materials"/"materials.json";
  if(!fs::is_regular_file(index))
    return {};
  const auto doc = readIndex(index);
  std::map<std::string,uint8_t> flags;
  for(const auto& member:doc.GetObject()) {
    const auto& value = member.value;
    if(!value.IsObject())
      continue;
    uint8_t mask = 0;
    if(value.HasMember("two_sided") && value["two_sided"].IsString()) {
      const std::string_view twoSided(value["two_sided"].GetString());
      if(twoSided=="same" || twoSided=="flip")
        mask |= TwoSided;
      }
    if((value.HasMember("foliage") && value["foliage"].IsObject()) ||
       (value.HasMember("strand") && value["strand"].IsTrue()))
      mask |= Deforming;
    flags[materialKey(member.name.GetString())] = mask;
    }
  return flags;
  }

struct OwnMeshInput {
  explicit OwnMeshInput(const fs::path& path, const std::map<std::string,uint8_t>& materialLocks) {
    mesh = loadGltfMesh(path,&bytes);
    colors = loadGltfMeshColors(bytes,mesh);
    for(const auto& sub:mesh.sub_meshes) {
      const auto it = materialLocks.find(materialKey(sub.mat.texture));
      lockFlags.push_back(it==materialLocks.end() ? 0 : it->second);
      }
    }

  asset::StaticMesh    mesh;
  GltfMeshColors       colors;
  std::vector<uint8_t> bytes, lockFlags;
  };

struct MeshCounts {
  Counts                             graph;
  size_t                             locked = 0, alphaTest = 0, twoSided = 0, deforming = 0, discrete = 0;
  size_t                             assets = 0;
  std::array<std::vector<double>,2>   distances;

  void add(const MeshCounts& other) {
    graph.add(other.graph);
    for(size_t i=0; i<distances.size(); ++i)
      distances[i].insert(distances[i].end(),other.distances[i].begin(),other.distances[i].end());
    locked += other.locked;
    alphaTest += other.alphaTest;
    twoSided += other.twoSided;
    deforming += other.deforming;
    discrete += other.discrete;
    assets += other.assets;
    }
  };

MeshCounts meshCounts(const OwnMeshInput& input, const ClusterLod::Graph& graph, double buildMs, bool discrete) {
  MeshCounts out;
  out.assets = 1;
  out.discrete = discrete;
  out.graph.buildMs = buildMs;
  out.graph.bytes = graph.byteSize();
  for(size_t i=0; i<input.mesh.sub_meshes.size(); ++i) {
    const auto& sub = input.mesh.sub_meshes[i];
    const auto flags = input.lockFlags[i];
    out.alphaTest += MeshClusterLod::alphaTest(sub.mat) ? sub.triangles.size() : 0;
    out.twoSided  += (flags & TwoSided)!=0 ? sub.triangles.size() : 0;
    out.deforming += (flags & Deforming)!=0 ? sub.triangles.size() : 0;
    out.locked    += MeshClusterLod::locked(sub.mat,flags!=0) ? sub.triangles.size() : 0;
    }
  const double pixels = 0.5*ViewportHeight*verticalProjection();
  std::vector<bool> seen(graph.groups.size());
  for(const auto& cluster:graph.clusters) {
    if(cluster.level>=out.graph.levels.size())
      out.graph.levels.resize(cluster.level+1);
    auto& level = out.graph.levels[cluster.level];
    level.triangles += cluster.triangleCount;
    ++level.clusters;
    if(std::isinf(graph.groups[cluster.parentGroup].errorCm))
      out.graph.top += cluster.triangleCount;
    if(cluster.level>=1 && cluster.level<=2 && !seen[cluster.refinedGroup]) {
      seen[cluster.refinedGroup] = true;
      const auto& group = graph.groups[cluster.refinedGroup];
      const double nearCm = pixels*group.errorCm;
      out.distances[cluster.level-1].push_back(nearCm<=1. ? 0. : (group.bounds.radius+nearCm)/100.);
      }
    }
  return out;
  }

void meshRow(std::ostream& out, std::string_view root, std::string_view name, MeshCounts& counts, size_t levels) {
  const auto& g = counts.graph;
  const double top = g.levels.empty() ? 0. : double(g.top)/double(g.levels.front().triangles);
  out << root << '\t' << name << '\t' << counts.assets << '\t' << counts.discrete << '\t'
      << g.levels.size() << '\t' << g.top << '\t' << std::fixed << std::setprecision(6) << top << '\t'
      << counts.locked << '\t' << counts.alphaTest << '\t' << counts.twoSided << '\t' << counts.deforming << '\t'
      << std::setprecision(3) << g.buildMs << '\t' << g.bytes;
  for(auto& values:counts.distances) {
    out << '\t' << values.size();
    if(values.empty()) {
      out << "\tNA\tNA";
      continue;
      }
    std::sort(values.begin(),values.end());
    const double median = (values[(values.size()-1)/2]+values[values.size()/2])*0.5;
    out << '\t' << median << '\t' << values[values.size()-values.size()/10-1];
    }
  for(size_t i=0; i<levels; ++i)
    out << '\t' << (i<g.levels.size() ? g.levels[i].triangles : 0);
  out << '\n';
  }

std::string utf8Json(std::string_view source) {
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  writer.String(source.data(),rapidjson::SizeType(source.size()));
  return buffer.GetString();
  }

std::string writeDenseMesh(const OwnMesh& entry, const OwnMeshInput& input, const fs::path& target, int level) {
  if(!input.colors.empty())
    return "stress split does not interpolate COLOR_0";
  std::optional<asset::StaticMesh> dense = input.mesh;
  for(int i=0; i<level; ++i) {
    std::string reason;
    dense = StressBase::Meshes::subdivide(*dense,&reason);
    if(!dense)
      return reason;
    }
  size_t wedges = 0;
  for(const auto& sub:dense->sub_meshes)
    wedges += sub.wedges.size();
  if(wedges>65535) // The static glTF reader loads one position per wedge of each material.
    return std::to_string(wedges)+" wedges exceed 65535 glTF positions";

  // The converter has already flattened node transforms and mirrored X. Write in glTF
  // coordinates again and keep the material convention consumed by the game's reader.
  gothicglb::GlbBuilder glb;
  std::string materials = "[", primitives = "[";
  for(size_t i=0; i<dense->sub_meshes.size(); ++i) {
    const auto& sub = dense->sub_meshes[i];
    std::vector<float> pos, normal, uv;
    for(const auto& wedge:sub.wedges) {
      const auto& p = dense->positions[wedge.index];
      const auto& n = wedge.normal;
      if(n.x==0 && n.y==0 && n.z==0)
        return "the split produced a zero normal";
      pos.insert(pos.end(),{-p.x*0.01f,p.y*0.01f,p.z*0.01f});
      normal.insert(normal.end(),{-n.x,n.y,n.z});
      uv.insert(uv.end(),{wedge.texture.x,wedge.texture.y});
      }
    std::vector<uint16_t> indices;
    for(const auto& t:sub.triangles)
      indices.insert(indices.end(),std::begin(t.wedges),std::end(t.wedges));
    const auto aPos = glb.accessor(pos,5126,"VEC3",3,false,34962);
    const auto aNrm = glb.accessor(normal,5126,"VEC3",3,false,34962);
    const auto aUv = glb.accessor(uv,5126,"VEC2",2,false,34962);
    const auto aIdx = glb.accessor(indices,5123,"SCALAR",1,false,34963);
    if(i>0) {
      materials += ",";
      primitives += ",";
      }
    const auto& mat = sub.mat;
    const char* mode = MeshClusterLod::alphaTest(mat) ? "MASK" :
                       mat.alpha_func==asset::AlphaFunction::BLEND ? "BLEND" : "OPAQUE";
    constexpr const char* groups[] = {"UNDEF","METAL","STONE","WOOD","EARTH","WATER","SNOW"};
    materials += "{\"name\":"+utf8Json(mat.name)+",\"alphaMode\":\""+mode+"\",\"extras\":{\"group\":\""+
                 groups[uint8_t(mat.group)]+"\"}}";
    primitives += "{\"attributes\":{\"POSITION\":"+std::to_string(aPos)+",\"NORMAL\":"+std::to_string(aNrm)+
                  ",\"TEXCOORD_0\":"+std::to_string(aUv)+"},\"indices\":"+std::to_string(aIdx)+
                  ",\"material\":"+std::to_string(i)+"}";
    }
  const std::string json =
    "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
    "\"nodes\":[{\"name\":"+utf8Json(entry.name)+",\"mesh\":0}],\"meshes\":[{\"primitives\":"+primitives+
    "]}],\"materials\":"+materials+"],\"accessors\":"+glb.accessorsJson()+",\"bufferViews\":"+glb.viewsJson()+
    ",\"buffers\":[{\"byteLength\":"+std::to_string(glb.bin.size())+"}]}";
  const auto bytes = glb.finish(json);
  gothicglb::writeFile(target,bytes.data(),bytes.size());
  return {};
  }

}

void runOwnMeshClusterLod(const std::vector<std::filesystem::path>& roots, const std::filesystem::path& output, bool cook) {
  struct Set {
    fs::path root;
    std::vector<std::pair<std::string,MeshCounts>> rows;
    MeshCounts total;
    };
  std::vector<Set> sets;
  MeshCounts all;
  for(const auto& root:roots) {
    Set set;
    set.root = root;
    const auto materialLocks = ownMaterialLocks(root);
    size_t written = 0, skipped = 0;
    for(const auto& entry:ownMeshes(root)) {
      if(cook && entry.discrete) {
        ++skipped;
        continue;
        }
      const auto path = root/"meshes"/entry.relative;
      try {
        const OwnMeshInput input(path,materialLocks);
        if(cook && std::ranges::any_of(input.lockFlags,[](uint8_t flags) { return (flags & Deforming)!=0; })) {
          ++skipped;
          continue;
          }
        const auto started = std::chrono::steady_clock::now();
        const auto graph = MeshClusterLod::build(input.mesh,input.colors,input.lockFlags);
        const double ms = std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count();
        if(cook) {
          const auto crc = uint32_t(mz_crc32(0,input.bytes.data(),input.bytes.size()));
          MeshClusterLod::write(path,input.bytes.size(),crc,graph);
          ++written;
          } else {
          auto counts = meshCounts(input,graph,ms,entry.discrete);
          set.total.add(counts);
          set.rows.emplace_back(entry.name,std::move(counts));
          }
        }
      catch(const std::exception& error) {
        throw std::runtime_error(path.string()+": "+error.what());
        }
      }
    if(cook) {
      std::cout << root.string() << ": " << written << " mesh LOD graphs cooked, " << skipped
                << " records with discrete levels, impostors or deforming materials skipped\n";
      } else {
      all.add(set.total);
      sets.push_back(std::move(set));
      }
    }
  if(cook)
    return;

  std::ostringstream out;
  out.imbue(std::locale::classic());
  out << "content\tmesh\tasset_count\tdiscrete_count\tlevels\ttop_triangles\ttop_fraction\t"
         "locked_triangles\talpha_test_triangles\ttwo_sided_triangles\tdeforming_triangles\tbuild_ms\tgraph_bytes\t"
         "L1_transitions\tL1_p50_m\tL1_p90_m\tL2_transitions\tL2_p50_m\tL2_p90_m";
  const auto levels = all.graph.levels.size();
  for(size_t i=0; i<levels; ++i)
    out << "\tL" << i << "_triangles";
  out << '\n';
  for(auto& set:sets) {
    const auto root = set.root.string();
    for(auto& [name,counts]:set.rows)
      meshRow(out,root,name,counts,levels);
    meshRow(out,root,"TOTAL",set.total,levels);
    }
  meshRow(out,"*","TOTAL",all,levels);
  const auto report = out.str();
  if(output.empty()) {
    std::cout << report;
    if(!std::cout)
      throw std::runtime_error("cannot write mesh LOD census to stdout");
    } else {
    gothicglb::writeFile(output,report.data(),report.size());
    }
  }

void stressOwnMeshContent(const std::filesystem::path& root, const std::filesystem::path& output, int level) {
  const auto meshes = ownMeshes(root);
  const auto source = fs::canonical(root);
  const auto destination = fs::weakly_canonical(output);
  const auto relative = destination.lexically_relative(source);
  if((!relative.empty() && *relative.begin()!="..") || (fs::exists(destination) && !fs::is_empty(destination)))
    throw std::runtime_error("stress output must be an empty directory outside the content set: "+destination.string());
  fs::create_directories(destination);
  fs::copy(source,destination,fs::copy_options::recursive);
  const auto locks = ownMaterialLocks(source);
  size_t split = 0;
  std::set<fs::path> processed;
  for(const auto& entry:meshes) {
    if(level==0 || entry.discrete || !processed.insert(entry.relative.lexically_normal()).second)
      continue;
    const auto path = source/"meshes"/entry.relative;
    const OwnMeshInput input(path,locks);
    const auto reason = writeDenseMesh(entry,input,destination/"meshes"/entry.relative,level);
    if(reason.empty())
      ++split;
    else
      std::cout << "mesh left at source density: "<<entry.name<<": "<<reason<<'\n';
    }
  std::cout << destination.string() << ": " << split << " of " << meshes.size() << " meshes subdivided ("
            << (1u<<(2*level)) << " triangles per source triangle)\n";
  }
