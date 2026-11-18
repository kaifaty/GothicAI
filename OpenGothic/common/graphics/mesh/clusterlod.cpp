// Hierarchy construction adapted from meshoptimizer v1.3 demo/clusterlod.h.
// Copyright (C) 2016-2026, by Arseny Kapoulkine (arseny.kapoulkine@gmail.com).
// Distributed under the MIT license in OpenGothic/lib/meshoptimizer/LICENSE.md.
#include "clusterlod.h"

#include <meshoptimizer.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <map>
#include <stdexcept>
#include <tuple>

namespace ClusterLod {
namespace {

constexpr size_t GroupSize = 16;
constexpr float Infinity = std::numeric_limits<float>::infinity();
constexpr std::array<float, 9> AttributeWeights = {0.5f, 0.5f, 0.5f, 1.f, 1.f, 0.25f, 0.25f, 0.25f, 0.25f};

using Indices = std::vector<uint32_t>;
using Edge = std::array<uint32_t, 2>;
using Boundary = std::map<Edge, int>;
using MaterialContours = std::map<uint32_t, std::vector<std::tuple<uint32_t, uint32_t, int>>>;

struct Wedge {
  Point                position;
  std::array<float, 9> attributes;
  uint32_t             vertex;
  uint32_t             material;
  };

uint32_t offset(size_t size) {
  if(size >= NoGroup)
    throw std::length_error("Cluster LOD graph exceeds 32-bit indices");
  return uint32_t(size);
  }

double distance(const Point& a, const Point& b) {
  const double x = double(a[0]) - b[0];
  const double y = double(a[1]) - b[1];
  const double z = double(a[2]) - b[2];
  return std::sqrt(x*x + y*y + z*z);
  }

float enclosingRadius(double radius) {
  const float rounded = float(radius);
  return double(rounded) < radius ? std::nextafter(rounded, Infinity) : rounded;
  }

void addEdge(Boundary& edges, uint32_t from, uint32_t to) {
  if(from != to)
    edges[{std::min(from, to), std::max(from, to)}] += (from < to ? 1 : -1);
  }

bool collinearBetween(const Point& point, const Point& from, const Point& to) {
  std::array<double, 3> a, b;
  for(size_t k = 0; k < a.size(); ++k) {
    a[k] = double(from[k]) - point[k];
    b[k] = double(to[k]) - point[k];
    }
  return a[0]*b[0] + a[1]*b[1] + a[2]*b[2] < 0 &&
         a[0]*b[1] == a[1]*b[0] && a[1]*b[2] == a[2]*b[1] && a[2]*b[0] == a[0]*b[2];
  }

Sphere mergeSpheres(std::span<const Sphere> spheres) {
  const auto bounds = meshopt_computeSphereBounds(spheres.front().center.data(), spheres.size(), sizeof(Sphere),
                                                  &spheres.front().radius, sizeof(Sphere));
  const Point center = {bounds.center[0], bounds.center[1], bounds.center[2]};
  double radius = bounds.radius;
  for(const auto& child : spheres)
    radius = std::max(radius, distance(center, child.center) + child.radius);
  // Leave room for roundoff in the shader's float length and subtraction.
  return {center, enclosingRadius(radius + 8 * std::numeric_limits<float>::epsilon() * std::max(1.0, radius))};
  }

class Builder {
  public:
    Builder(std::span<const Vertex> vertices, std::span<const Triangle> triangles, std::span<const uint8_t> boundary);
    Graph finish();

  private:
    Graph result;
    std::vector<Wedge> wedges;
    Indices positions; // wedge to the canonical representative at its position
    Indices meshletWedges; // parallel to result.vertexIndices, for the build only
    Indices live;
    std::vector<uint8_t> fixed; // position locks from the chunk and finished branches

    void clusterize(const Indices& indices, uint32_t level, uint32_t refinedGroup, Indices& output);
    void appendIndices(Indices& target, const Cluster& cluster, bool remapPositions) const;
    std::vector<Indices> partition() const;
    std::vector<uint8_t> lockBoundary(const std::vector<Indices>& partitions) const;
    Group groupBounds(const Indices& clusters) const;
    void finishGroup(const Indices& clusters, const std::vector<uint8_t>& locks,
                     Indices& pending, std::vector<uint8_t>& nextFixed);
    bool sameBoundary(const Indices& original, const Indices& simplified) const;
    MaterialContours materialContours(const Indices& indices) const;
  };

Builder::Builder(std::span<const Vertex> vertices, std::span<const Triangle> triangles, std::span<const uint8_t> boundary) {
  if(boundary.size() != vertices.size())
    throw std::invalid_argument("Cluster LOD requires one boundary flag per vertex");
  offset(vertices.size());
  result.vertices.assign(vertices.begin(), vertices.end());
  if(triangles.empty())
    return;

  Indices indices;
  indices.reserve(triangles.size() * 3);
  std::map<std::array<uint32_t, 10>, uint32_t> unique;
  for(const auto& triangle : triangles) {
    for(const auto id : triangle.vertices) {
      if(id >= vertices.size())
        throw std::out_of_range("Cluster LOD vertex index");
      const auto& vertex = vertices[id];
      const auto& p = vertex.position;
      const auto& n = vertex.normal;
      const auto& uv = vertex.uv;
      for(float value : {p[0], p[1], p[2], n[0], n[1], n[2], uv[0], uv[1]}) {
        if(!std::isfinite(value))
          throw std::invalid_argument("Cluster LOD requires finite positions and attributes");
        }
      const std::array<uint32_t, 10> key = {
        std::bit_cast<uint32_t>(p[0]), std::bit_cast<uint32_t>(p[1]), std::bit_cast<uint32_t>(p[2]),
        std::bit_cast<uint32_t>(n[0]), std::bit_cast<uint32_t>(n[1]), std::bit_cast<uint32_t>(n[2]),
        std::bit_cast<uint32_t>(uv[0]), std::bit_cast<uint32_t>(uv[1]), vertex.color, triangle.material,
        };
      const auto [it, inserted] = unique.try_emplace(key, offset(wedges.size()));
      if(inserted) {
        Wedge wedge = {p, {n[0], n[1], n[2], uv[0], uv[1]}, id, triangle.material};
        for(size_t channel = 0; channel < 4; ++channel)
          wedge.attributes[5 + channel] = float((vertex.color >> (channel * 8)) & 255u) / 255.f;
        wedges.push_back(wedge);
        fixed.push_back(0);
        }
      if(boundary[id])
        fixed[it->second] = meshopt_SimplifyVertex_Lock;
      indices.push_back(it->second);
      }
    }

  positions.resize(wedges.size());
  meshopt_generatePositionRemap(positions.data(), wedges.front().position.data(), wedges.size(), sizeof(Wedge));
  for(size_t i = 0; i < wedges.size(); ++i)
    fixed[positions[i]] |= fixed[i];
  clusterize(indices, 0, NoGroup, live);
  }

void Builder::clusterize(const Indices& indices, uint32_t level, uint32_t refinedGroup, Indices& output) {
  std::map<uint32_t, Indices> byMaterial;
  for(size_t i = 0; i < indices.size(); i += 3) {
    auto& materialIndices = byMaterial[wedges[indices[i]].material];
    materialIndices.insert(materialIndices.end(), indices.data() + i, indices.data() + i + 3);
    }

  for(const auto& [material, materialIndices] : byMaterial) {
    constexpr size_t MinTriangles = MaxTriangles / 3;
    const size_t capacity = meshopt_buildMeshletsBound(materialIndices.size(), MaxVertices, MinTriangles);
    std::vector<meshopt_Meshlet> meshlets(capacity);
    Indices vertexIndices(materialIndices.size());
    std::vector<uint8_t> localIndices(materialIndices.size());
    const auto count = meshopt_buildMeshletsFlex(meshlets.data(), vertexIndices.data(), localIndices.data(),
                                               materialIndices.data(), materialIndices.size(), wedges.front().position.data(),
                                               wedges.size(), sizeof(Wedge), MaxVertices, MinTriangles, MaxTriangles, 0.f, 2.f);
    for(size_t i = 0; i < count; ++i) {
      const auto& m = meshlets[i];
      const auto ids = std::span(vertexIndices).subspan(m.vertex_offset, m.vertex_count);
      std::array<Point, MaxVertices> points;
      for(size_t j = 0; j < ids.size(); ++j)
        points[j] = wedges[ids[j]].position;
      const auto b = meshopt_computeSphereBounds(points.front().data(), ids.size(), sizeof(Point), nullptr, 0);
      const Point center = {b.center[0], b.center[1], b.center[2]};
      double radius = b.radius;
      for(size_t j = 0; j < ids.size(); ++j)
        radius = std::max(radius, distance(center, points[j]));

      output.push_back(offset(result.clusters.size()));
      result.clusters.push_back({{center, enclosingRadius(radius)}, level, material, refinedGroup, NoGroup,
                                offset(result.vertexIndices.size()), offset(result.indices.size()),
                                m.vertex_count, m.triangle_count});
      for(uint32_t id : ids)
        result.vertexIndices.push_back(wedges[id].vertex);
      meshletWedges.insert(meshletWedges.end(), ids.begin(), ids.end());
      const auto* first = localIndices.data() + m.triangle_offset;
      result.indices.insert(result.indices.end(), first, first + size_t(m.triangle_count) * 3);
      }
    }
  }

void Builder::appendIndices(Indices& target, const Cluster& cluster, bool remapPositions) const {
  const auto indices = std::span(result.indices).subspan(cluster.indexOffset, size_t(cluster.triangleCount) * 3);
  for(uint8_t index : indices) {
    const uint32_t wedge = meshletWedges[cluster.vertexOffset + index];
    target.push_back(remapPositions ? positions[wedge] : wedge);
    }
  }

std::vector<Indices> Builder::partition() const {
  if(live.size() <= GroupSize)
    return {live};

  Indices indices, counts, groups(live.size());
  for(uint32_t id : live) {
    const auto& cluster = result.clusters[id];
    appendIndices(indices, cluster, true);
    counts.push_back(cluster.triangleCount * 3);
    }
  const size_t count = meshopt_partitionClusters(groups.data(), indices.data(), indices.size(), counts.data(),
                                                counts.size(), wedges.front().position.data(), wedges.size(),
                                                sizeof(Wedge), GroupSize);
  std::vector<Indices> partitions(count);
  for(size_t i = 0; i < live.size(); ++i)
    partitions[groups[i]].push_back(live[i]);
  return partitions;
  }

std::vector<uint8_t> Builder::lockBoundary(const std::vector<Indices>& partitions) const {
  Indices owner(wedges.size(), NoGroup);
  auto positionLocks = fixed;
  for(uint32_t group = 0; group < partitions.size(); ++group) {
    for(uint32_t id : partitions[group]) {
      const auto& cluster = result.clusters[id];
      for(uint32_t wedge : std::span(meshletWedges).subspan(cluster.vertexOffset, cluster.vertexCount)) {
        const uint32_t position = positions[wedge];
        if(owner[position] != NoGroup && owner[position] != group)
          positionLocks[position] = meshopt_SimplifyVertex_Lock;
        owner[position] = group;
        }
      }
    }
  std::vector<uint8_t> locks(wedges.size());
  for(size_t i = 0; i < locks.size(); ++i)
    locks[i] = positionLocks[positions[i]];
  return locks;
  }

Group Builder::groupBounds(const Indices& clusters) const {
  std::vector<Sphere> spheres;
  Group group = {};
  for(uint32_t id : clusters) {
    const auto& cluster = result.clusters[id];
    spheres.push_back(cluster.bounds);
    if(cluster.refinedGroup != NoGroup) {
      const auto& refined = result.groups[cluster.refinedGroup];
      spheres.push_back(refined.bounds);
      group.errorCm = std::max(group.errorCm, refined.errorCm);
      }
    }
  group.bounds = mergeSpheres(spheres);
  return group;
  }

bool Builder::sameBoundary(const Indices& original, const Indices& simplified) const {
  // Compare directed open edges by position across wedges. Also preserve the exact
  // wedge vertices and edges of marked boundaries, even inside a closed surface.
  auto signature = [&](const Indices& indices) {
    Boundary open;
    std::vector<Edge> fixedEdges;
    Indices fixedVertices;
    for(size_t i = 0; i < indices.size(); i += 3) {
      for(size_t j = 0; j < 3; ++j) {
        const uint32_t a = indices[i + j], b = indices[i + (j + 1) % 3];
        const uint32_t p = positions[a], q = positions[b];
        addEdge(open, p, q);
        if(fixed[p]) {
          fixedVertices.push_back(a);
          if(fixed[q])
            fixedEdges.push_back({a, b});
          }
        }
      }
    std::erase_if(open, [](const auto& edge) { return edge.second == 0; });
    std::sort(fixedEdges.begin(), fixedEdges.end());
    std::sort(fixedVertices.begin(), fixedVertices.end());
    fixedVertices.erase(std::unique(fixedVertices.begin(), fixedVertices.end()), fixedVertices.end());
    return std::tuple(std::move(open), std::move(fixedEdges), std::move(fixedVertices));
    };
  return signature(original) == signature(simplified);
  }

MaterialContours Builder::materialContours(const Indices& indices) const {
  std::map<uint32_t, Boundary> materials;
  for(size_t i = 0; i < indices.size(); i += 3) {
    const uint32_t material = wedges[indices[i]].material;
    auto& edges = materials[material];
    for(size_t j = 0; j < 3; ++j)
      addEdge(edges, positions[indices[i+j]], positions[indices[i+(j+1)%3]]);
    }

  // Canonicalize each material's oriented contour. A straight run can lose
  // vertices, but its endpoints, bends and junctions must stay.
  MaterialContours contours;
  for(const auto& [material, edges] : materials) {
    struct Junction {
      uint32_t prev = NoGroup, next = NoGroup;
      bool     multiple = false, straight = false;
      };
    std::map<uint32_t, Junction> points;
    auto& contour = contours[material];
    for(const auto& [edge, count] : edges) {
      if(count == 0)
        continue;
      const uint32_t from = edge[count < 0], to = edge[count > 0];
      auto& a = points[from];
      auto& b = points[to];
      a.multiple |= a.next != NoGroup || std::abs(count) != 1;
      b.multiple |= b.prev != NoGroup || std::abs(count) != 1;
      a.next = to;
      b.prev = from;
      }
    for(auto& [id, point] : points)
      point.straight = !point.multiple && point.prev != NoGroup && point.next != NoGroup &&
                       collinearBetween(wedges[id].position, wedges[point.prev].position, wedges[point.next].position);
    for(const auto& [edge, count] : edges) {
      if(count == 0)
        continue;
      const uint32_t from = edge[count < 0];
      uint32_t to = edge[count > 0];
      if(points.at(from).straight)
        continue;
      while(points.at(to).straight)
        to = points.at(to).next;
      contour.emplace_back(from, to, std::abs(count));
      }
    std::sort(contour.begin(), contour.end());
    }
  return contours;
  }

void Builder::finishGroup(const Indices& clusters, const std::vector<uint8_t>& locks,
                          Indices& pending, std::vector<uint8_t>& nextFixed) {
  Indices original;
  for(uint32_t id : clusters)
    appendIndices(original, result.clusters[id], false);

  const auto material = wedges[original.front()].material;
  const bool mixed = std::ranges::any_of(original, [&](uint32_t id) { return wedges[id].material != material; });
  MaterialContours contours;
  std::vector<uint8_t> materialLocks;
  if(mixed) {
    contours = materialContours(original);
    materialLocks = locks;
    auto positionLocks = fixed;
    for(const auto& [id, contour] : contours) {
      for(const auto& [from, to, count] : contour) {
        positionLocks[from] = meshopt_SimplifyVertex_Lock;
        positionLocks[to] = meshopt_SimplifyVertex_Lock;
        }
      }
    for(uint32_t wedge : original)
      materialLocks[wedge] |= positionLocks[positions[wedge]];
    }

  Indices simplified(original.size());
  float error = 0.f;
  const unsigned options = meshopt_SimplifyLockBorder | meshopt_SimplifySparse | meshopt_SimplifyErrorAbsolute |
                           meshopt_SimplifyErrorClamped | meshopt_SimplifyPreserveFolds;
  simplified.resize(meshopt_simplifyWithAttributes(simplified.data(), original.data(), original.size(),
                                                   wedges.front().position.data(), wedges.size(), sizeof(Wedge),
                                                   wedges.front().attributes.data(), sizeof(Wedge), AttributeWeights.data(),
                                                   AttributeWeights.size(), (mixed ? materialLocks : locks).data(), (original.size() / 6) * 3,
                                                   std::numeric_limits<float>::max(), options, &error));

  // Keep at most 85% of the triangles to accept a replacement.
  const bool reduced = !simplified.empty() && simplified.size() * 100 <= original.size() * 85;
  bool materialsMatch = true;
  if(reduced && mixed) {
    for(size_t i = 0; i < simplified.size(); i += 3) {
      const auto id = wedges[simplified[i]].material;
      materialsMatch &= id == wedges[simplified[i+1]].material && id == wedges[simplified[i+2]].material;
      }
    materialsMatch = materialsMatch && contours == materialContours(simplified);
    }
  auto group = groupBounds(clusters);
  const uint32_t groupId = offset(result.groups.size());
  for(uint32_t id : clusters)
    result.clusters[id].parentGroup = groupId;

  if(reduced && materialsMatch && sameBoundary(original, simplified)) {
    // Cover float projection roundoff when the errors of two transitions tie.
    group.errorCm = std::max(group.errorCm, error) * (1.f + 8 * std::numeric_limits<float>::epsilon());
    clusterize(simplified, result.clusters[clusters.front()].level + 1, groupId, pending);
    }
  else {
    group.errorCm = Infinity;
    for(uint32_t wedge : original)
      nextFixed[positions[wedge]] = meshopt_SimplifyVertex_Lock;
    }
  result.groups.push_back(group);
  }

Graph Builder::finish() {
  std::vector<uint8_t> nextFixed;
  while(!live.empty()) {
    const auto partitions = partition();
    const auto locks = lockBoundary(partitions);
    Indices pending;
    // Keep both simplification and boundary validation on the same round's locks.
    nextFixed = fixed;
    for(const auto& group : partitions)
      finishGroup(group, locks, pending, nextFixed);
    fixed.swap(nextFixed);
    live.swap(pending);
    }
  return std::move(result);
  }

}

size_t Graph::byteSize() const {
  static_assert(sizeof(Vertex) == 36 && sizeof(Cluster) == 48 && sizeof(Group) == 20);
  return vertices.size() * sizeof(Vertex) + vertexIndices.size() * sizeof(uint32_t) + indices.size() +
         clusters.size() * sizeof(Cluster) + groups.size() * sizeof(Group);
  }

Graph build(std::span<const Vertex> vertices, std::span<const Triangle> triangles, std::span<const uint8_t> boundary) {
  return Builder(vertices, triangles, boundary).finish();
  }

float projectError(const Group& group, const View& view) {
  const float x = group.bounds.center[0] - view.camera[0];
  const float y = group.bounds.center[1] - view.camera[1];
  const float z = group.bounds.center[2] - view.camera[2];
  const float nearDistance = std::max(std::sqrt(x*x + y*y + z*z) - group.bounds.radius, 1.f);
  return group.errorCm / nearDistance * (0.5f * view.verticalProjection * view.viewportHeight);
  }

std::vector<uint32_t> selectCut(const Graph& graph, const View& view, float thresholdPx) {
  Indices selected;
  for(uint32_t i = 0; i < graph.clusters.size(); ++i) {
    const auto& cluster = graph.clusters[i];
    const auto& parent = graph.groups[cluster.parentGroup];
    if(thresholdPx <= 0) {
      if(cluster.refinedGroup == NoGroup)
        selected.push_back(i);
      continue;
      }
    if(thresholdPx == Infinity) {
      if(parent.errorCm == Infinity)
        selected.push_back(i);
      continue;
      }

    const float ownError = cluster.refinedGroup == NoGroup ? 0.f : projectError(graph.groups[cluster.refinedGroup], view);
    if(ownError <= thresholdPx && (parent.errorCm == Infinity || projectError(parent, view) > thresholdPx))
      selected.push_back(i);
    }
  return selected;
  }

}
