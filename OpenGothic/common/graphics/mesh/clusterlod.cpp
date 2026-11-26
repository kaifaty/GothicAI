// Hierarchy construction adapted from meshoptimizer v1.3 demo/clusterlod.h.
// Copyright (C) 2016-2026, by Arseny Kapoulkine (arseny.kapoulkine@gmail.com).
// Distributed under the MIT license in OpenGothic/lib/meshoptimizer/LICENSE.md.
#include "clusterlod.h"

#include <meshoptimizer.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <map>
#include <numeric>
#include <set>
#include <stdexcept>

namespace ClusterLod {
namespace {

constexpr size_t GroupSize = 16;
constexpr float Infinity = std::numeric_limits<float>::infinity();
constexpr std::array<float, 9> AttributeWeightsCm = {50.f, 50.f, 50.f, 25.f, 25.f, 20.f, 20.f, 20.f, 20.f};

enum class Profile { World, Foliage64, Foliage128 };

using Indices = std::vector<uint32_t>;
using Edge = std::array<uint32_t, 2>;
using Boundary = std::map<Edge, int>;
using MaterialContours = std::map<uint32_t, Boundary>;
using BorderMoves = std::map<uint32_t, Point>;

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

using Vector = std::array<double, 3>;

Vector delta(const Point& a, const Point& b) {
  return {double(a[0]) - b[0], double(a[1]) - b[1], double(a[2]) - b[2]};
  }

double dot(const Vector& a, const Vector& b) {
  return std::inner_product(a.begin(), a.end(), b.begin(), 0.);
  }

Vector normal(const Point& a, const Point& b, const Point& c) {
  const auto u = delta(b, a), v = delta(c, a);
  return {u[1]*v[2] - u[2]*v[1], u[2]*v[0] - u[0]*v[2], u[0]*v[1] - u[1]*v[0]};
  }

float enclosingRadius(double radius) {
  const float rounded = float(radius);
  return double(rounded) < radius ? std::nextafter(rounded, Infinity) : rounded;
  }

void addEdge(Boundary& edges, uint32_t from, uint32_t to) {
  if(from != to)
    edges[{std::min(from, to), std::max(from, to)}] += (from < to ? 1 : -1);
  }

void discardInteriorEdges(Boundary& edges) {
  std::erase_if(edges, [](const auto& edge) { return edge.second == 0; });
  }

double distanceToSegment(const Point& point, const Point& from, const Point& to) {
  std::array<double, 3> a, b;
  double dot = 0, lengthSquared = 0;
  for(size_t k = 0; k < a.size(); ++k) {
    a[k] = double(point[k]) - from[k];
    b[k] = double(to[k]) - from[k];
    dot += a[k] * b[k];
    lengthSquared += b[k] * b[k];
    }
  const double t = lengthSquared > 0 ? std::clamp(dot / lengthSquared, 0.0, 1.0) : 0;
  double squared = 0;
  for(size_t k = 0; k < a.size(); ++k) {
    const double delta = a[k] - b[k] * t;
    squared += delta * delta;
    }
  return std::sqrt(squared);
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
    Builder(std::span<const Vertex> vertices, std::span<const Triangle> triangles,
            std::span<const uint8_t> boundary, BuildStats* stats, CensusVariant variant,
            Profile profile = Profile::World, std::span<const Skin> skin = {});
    Graph finish();

  private:
    Graph result;
    std::vector<Wedge> wedges;
    Indices positions; // wedge to the canonical representative at its position
    Indices meshletWedges; // parallel to result.vertexIndices, for the build only
    Indices live;
    std::vector<uint8_t> fixed; // position locks from the chunk and finished branches
    Indices materials;
    std::vector<float> coverageErrors;
    BuildStats* stats;
    CensusVariant variant;
    Profile profile;

    void clusterize(const Indices& indices, uint32_t level, uint32_t refinedGroup, Indices& output);
    void appendIndices(Indices& target, const Cluster& cluster, bool remapPositions) const;
    std::vector<Indices> partition() const;
    std::vector<uint8_t> lockBoundary(const std::vector<Indices>& partitions) const;
    Group groupBounds(const Indices& clusters) const;
    void finishGroup(const Indices& clusters, const std::vector<uint8_t>& locks,
                     Indices& pending, std::vector<uint8_t>& nextFixed);
    Boundary openBoundary(const Indices& indices) const;
    bool retainsLocks(const Indices& original, const Indices& simplified, const std::vector<uint8_t>& locks) const;
    MaterialContours materialContours(const Indices& indices, const Boundary& open) const;
    float materialError(const MaterialContours& original, const Indices& simplified, const Boundary& open) const;
    float foldError(const Indices& original, const Indices& simplified) const;
    bool retainsFoliageBoundary(const Indices& original, const Indices& simplified,
                                const Boundary& open, const Boundary& replacement,
                                const std::vector<uint8_t>& locks) const;
    bool dilateBorders(const Indices& original, const Indices& simplified, const Boundary& open,
                       const Boundary& replacement, const std::vector<uint8_t>& locks,
                       float budget, BorderMoves& moves, float& dilation) const;
    void applyBorderMoves(Indices& simplified, const BorderMoves& moves, std::vector<uint8_t>& nextFixed);
  };

Builder::Builder(std::span<const Vertex> vertices, std::span<const Triangle> triangles,
                 std::span<const uint8_t> boundary, BuildStats* stats, CensusVariant variant, Profile profile,
                 std::span<const Skin> skin)
  : stats(stats), variant(variant), profile(profile) {
  if(boundary.size() != vertices.size())
    throw std::invalid_argument("Cluster LOD requires one boundary flag per vertex");
  if(!skin.empty() && skin.size() != vertices.size())
    throw std::invalid_argument("Tree DAG requires one set of joint weights per vertex");
  offset(vertices.size());
  result.vertices.assign(vertices.begin(), vertices.end());
  result.skin.assign(skin.begin(), skin.end());
  if(triangles.empty())
    return;

  Indices indices, locked;
  uint32_t foliageMaterial = NoGroup;
  indices.reserve(triangles.size() * 3);
  std::map<std::array<uint32_t, 10>, uint32_t> unique;
  for(const auto& triangle : triangles) {
    const uint32_t material = variant == CensusVariant::NoMaterials && !triangle.locked ? 0 : triangle.material;
    if(profile != Profile::World && !triangle.locked) {
      if(foliageMaterial != NoGroup && material != foliageMaterial)
        throw std::invalid_argument("Tree DAG expects one foliage material");
      foliageMaterial = material;
      }
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
        std::bit_cast<uint32_t>(uv[0]), std::bit_cast<uint32_t>(uv[1]), vertex.color, material,
        };
      const auto [it, inserted] = unique.try_emplace(key, offset(wedges.size()));
      if(inserted) {
        Wedge wedge = {p, {n[0], n[1], n[2], uv[0], uv[1]}, id, material};
        for(size_t channel = 0; channel < 4; ++channel)
          wedge.attributes[5 + channel] = float((vertex.color >> (channel * 8)) & 255u) / 255.f;
        wedges.push_back(wedge);
        materials.push_back(material);
        fixed.push_back(0);
        }
      else if(!skin.empty() && skin[id] != skin[wedges[it->second].vertex]) {
        throw std::invalid_argument("Tree DAG coincident vertices have different joint weights");
        }
      if(boundary[id] || triangle.locked)
        fixed[it->second] = meshopt_SimplifyVertex_Lock;
      (triangle.locked ? locked : indices).push_back(it->second);
      }
    }

  positions.resize(wedges.size());
  meshopt_generatePositionRemap(positions.data(), wedges.front().position.data(), wedges.size(), sizeof(Wedge));
  for(size_t i = 0; i < wedges.size(); ++i) {
    if(!skin.empty() && skin[wedges[i].vertex] != skin[wedges[positions[i]].vertex])
      throw std::invalid_argument("Tree DAG coincident vertices have different joint weights");
    fixed[positions[i]] |= fixed[i];
    }
  clusterize(indices, 0, NoGroup, live);
  if(!locked.empty()) {
    // Remove these faces from the partitioner. Pin their coincident positions
    // on adjoining pieces as well, so a mixed cut cannot open the contact.
    Indices terminal;
    clusterize(locked, 0, NoGroup, terminal);
    const auto id = offset(result.groups.size());
    auto group = groupBounds(terminal);
    group.errorCm = Infinity;
    for(const auto cluster : terminal)
      result.clusters[cluster].parentGroup = id;
    result.groups.push_back(group);
    coverageErrors.push_back(0);
    if(stats) {
      stats->positionErrorCm.push_back(Infinity);
      if(profile != Profile::World)
        stats->dilationCm.push_back(0);
      ++stats->stoppedGroups[size_t(StopReason::Locked)];
      stats->stoppedTriangles[size_t(StopReason::Locked)] += locked.size()/3;
      }
    }
  }

void Builder::clusterize(const Indices& indices, uint32_t level, uint32_t refinedGroup, Indices& output) {
  std::map<uint32_t, Indices> byMaterial;
  for(size_t i = 0; i < indices.size(); i += 3) {
    auto& materialIndices = byMaterial[wedges[indices[i]].material];
    materialIndices.insert(materialIndices.end(), indices.data() + i, indices.data() + i + 3);
    }

  for(const auto& [material, materialIndices] : byMaterial) {
    const size_t maxVertices = profile == Profile::Foliage128 ? MaxFoliageVertices : MaxVertices;
    const size_t maxTriangles = profile == Profile::Foliage128 ? MaxFoliageTriangles : MaxTriangles;
    const size_t minTriangles = maxTriangles / 3;
    const size_t capacity = meshopt_buildMeshletsBound(materialIndices.size(), maxVertices, minTriangles);
    std::vector<meshopt_Meshlet> meshlets(capacity);
    Indices vertexIndices(materialIndices.size());
    std::vector<uint8_t> localIndices(materialIndices.size());
    const auto count = meshopt_buildMeshletsFlex(meshlets.data(), vertexIndices.data(), localIndices.data(),
                                               materialIndices.data(), materialIndices.size(), wedges.front().position.data(),
                                               wedges.size(), sizeof(Wedge), maxVertices, minTriangles, maxTriangles, 0.f, 2.f);
    for(size_t i = 0; i < count; ++i) {
      const auto& m = meshlets[i];
      const auto ids = std::span(vertexIndices).subspan(m.vertex_offset, m.vertex_count);
      std::array<Point, MaxFoliageVertices> points;
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
  if(variant == CensusVariant::NoBoundaries)
    return std::vector<uint8_t>(wedges.size());
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

Boundary Builder::openBoundary(const Indices& indices) const {
  Boundary edges;
  for(size_t i = 0; i < indices.size(); i += 3)
    for(size_t j = 0; j < 3; ++j)
      addEdge(edges, positions[indices[i+j]], positions[indices[i+(j+1)%3]]);
  discardInteriorEdges(edges);
  return edges;
  }

bool Builder::retainsLocks(const Indices& original, const Indices& simplified, const std::vector<uint8_t>& locks) const {
  std::set<uint32_t> required;
  for(uint32_t wedge : original)
    if(locks[wedge] & meshopt_SimplifyVertex_Lock)
      required.insert(positions[wedge]);
  for(uint32_t wedge : simplified)
    required.erase(positions[wedge]);
  return required.empty();
  }

MaterialContours Builder::materialContours(const Indices& indices, const Boundary& open) const {
  MaterialContours materials;
  for(size_t i = 0; i < indices.size(); i += 3) {
    auto& edges = materials[wedges[indices[i]].material];
    for(size_t j = 0; j < 3; ++j)
      addEdge(edges, positions[indices[i+j]], positions[indices[i+(j+1)%3]]);
    }
  for(auto& [id, edges] : materials) {
    discardInteriorEdges(edges);
    for(const auto& [edge, count] : open)
      edges.erase(edge);
    }
  return materials;
  }

struct ContourPoint {
  uint32_t next = NoGroup;
  size_t incoming = 0, outgoing = 0;
  };

std::map<uint32_t, ContourPoint> contourPoints(const Boundary& edges) {
  std::map<uint32_t, ContourPoint> points;
  for(const auto& [edge, count] : edges) {
    const uint32_t from = edge[count < 0], to = edge[count > 0];
    const size_t multiplicity = size_t(std::abs(count));
    points[from].next = to;
    points[from].outgoing += multiplicity;
    points[to].incoming += multiplicity;
    }
  return points;
  }

float Builder::materialError(const MaterialContours& original, const Indices& simplified, const Boundary& open) const {
  for(size_t i = 0; i < simplified.size(); i += 3) {
    const auto material = materials[simplified[i]];
    if(material != materials[simplified[i+1]] || material != materials[simplified[i+2]])
      return Infinity;
    }

  const auto replacement = materialContours(simplified, open);
  std::map<uint32_t, std::set<uint32_t>> surviving;
  for(uint32_t wedge : simplified)
    surviving[wedges[wedge].material].insert(positions[wedge]);

  double error = 0;
  for(const auto& [material, edges] : original) {
    const auto target = replacement.find(material);
    if(edges.empty() && target == replacement.end())
      return Infinity;

    const auto points = contourPoints(edges);
    const auto& kept = surviving[material];
    std::set<uint32_t> removed;
    for(const auto& [point, links] : points) {
      if(kept.contains(point))
        continue;
      if(links.incoming != 1 || links.outgoing != 1)
        return Infinity;
      removed.insert(point);
      }

    // A replacement edge is a chord of its old contour run. The farthest old
    // vertex bounds the motion of the entire run under projection onto the chord.
    Boundary contracted;
    Indices run;
    for(const auto& [edge, count] : edges) {
      const uint32_t from = edge[count < 0];
      if(!kept.contains(from))
        continue;
      uint32_t to = edge[count > 0];
      run.clear();
      while(!kept.contains(to)) {
        if(!removed.erase(to))
          return Infinity;
        run.push_back(to);
        to = points.at(to).next;
        }
      if(from != to)
        contracted[{std::min(from, to), std::max(from, to)}] += from < to ? std::abs(count) : -std::abs(count);
      for(uint32_t point : run)
        error = std::max(error, distanceToSegment(wedges[point].position, wedges[from].position, wedges[to].position));
      }

    // A circuit that loses all of its vertices can disappear only at an error
    // covering its size. Half its perimeter bounds its diameter.
    while(!removed.empty()) {
      const uint32_t first = *removed.begin();
      uint32_t point = first;
      double perimeter = 0;
      do {
        if(!removed.erase(point))
          return Infinity;
        const uint32_t next = points.at(point).next;
        perimeter += distance(wedges[point].position, wedges[next].position);
        point = next;
        } while(point != first);
      error = std::max(error, perimeter * 0.5);
      }

    discardInteriorEdges(contracted);
    if(target == replacement.end() ? !contracted.empty() : contracted != target->second)
      return Infinity;
    }
  return enclosingRadius(error);
  }

float Builder::foldError(const Indices& original, const Indices& simplified) const {
  using Face = std::array<uint32_t, 3>;
  const auto face = [&](const Indices& indices, size_t at) {
    Face triangle = {positions[indices[at]], positions[indices[at+1]], positions[indices[at+2]]};
    std::ranges::rotate(triangle, std::ranges::min_element(triangle));
    return triangle;
    };

  // Opposing faces can lose visible area with no movement of a directed open
  // edge. Find their positions before comparing oriented faces with the cut.
  const size_t count = original.size()/3;
  std::vector<Face> faces;
  faces.reserve(count);
  std::vector<std::array<double, 3>> normals;
  normals.reserve(count);
  std::map<Edge, std::vector<size_t>> incident;
  for(size_t i = 0; i < count; ++i) {
    const auto triangle = face(original, i*3);
    const auto& a = wedges[triangle[0]].position;
    const auto& b = wedges[triangle[1]].position;
    const auto& c = wedges[triangle[2]].position;
    const double ux = double(b[0])-a[0], uy = double(b[1])-a[1], uz = double(b[2])-a[2];
    const double vx = double(c[0])-a[0], vy = double(c[1])-a[1], vz = double(c[2])-a[2];
    faces.push_back(triangle);
    normals.push_back({uy*vz-uz*vy, uz*vx-ux*vz, ux*vy-uy*vx});
    for(size_t k = 0; k < 3; ++k) {
      const auto [from, to] = std::minmax(triangle[k], triangle[(k+1)%3]);
      incident[{from, to}].push_back(i);
      }
    }

  std::vector<bool> folded(count);
  for(const auto& [edge, adjacent] : incident) {
    for(size_t a = 0; a < adjacent.size(); ++a) {
      const auto& x = normals[adjacent[a]];
      for(size_t b = a+1; b < adjacent.size(); ++b) {
        const auto& y = normals[adjacent[b]];
        if(x[0]*y[0] + x[1]*y[1] + x[2]*y[2] < 0)
          folded[adjacent[a]] = folded[adjacent[b]] = true;
        }
      }
    }
  if(std::ranges::none_of(folded, [](bool b) { return b; }))
    return 0;

  std::set<Face> kept;
  for(size_t i = 0; i < simplified.size(); i += 3)
    kept.insert(face(simplified, i));

  // Join adjacent changed faces and measure each lost patch. The box
  // diagonal also covers a sheet whose two windings cancel its perimeter.
  std::vector<size_t> parent(count);
  std::iota(parent.begin(), parent.end(), size_t(0));
  const auto root = [&](size_t i) {
    while(parent[i] != i) {
      parent[i] = parent[parent[i]];
      i = parent[i];
      }
    return i;
    };
  for(size_t i = 0; i < count; ++i)
    folded[i] = folded[i] && !kept.contains(faces[i]);
  for(const auto& [edge, adjacent] : incident) {
    const auto first = std::ranges::find_if(adjacent, [&](size_t i) { return folded[i]; });
    if(first != adjacent.end())
      for(auto next = first+1; next != adjacent.end(); ++next)
        if(folded[*next])
          parent[root(*next)] = root(*first);
    }

  struct Bounds {
    Point min = { Infinity,  Infinity,  Infinity};
    Point max = {-Infinity, -Infinity, -Infinity};
    double perimeter = 0;
    };
  std::vector<Bounds> patches(count);
  for(size_t i = 0; i < count; ++i) {
    if(!folded[i])
      continue;
    auto& patch = patches[root(i)];
    for(const auto id : faces[i]) {
      const auto& point = wedges[id].position;
      for(size_t k = 0; k < 3; ++k) {
        patch.min[k] = std::min(patch.min[k], point[k]);
        patch.max[k] = std::max(patch.max[k], point[k]);
        }
      }
    }
  for(const auto& [edge, adjacent] : incident) {
    const auto changed = [&](size_t i) { return folded[i]; };
    if(std::ranges::count_if(adjacent, changed)==1) {
      const auto i = *std::ranges::find_if(adjacent, changed);
      patches[root(i)].perimeter += distance(wedges[edge[0]].position, wedges[edge[1]].position);
      }
    }
  double error = 0;
  for(const auto& patch : patches)
    if(patch.min[0] != Infinity)
      error = std::max({error, distance(patch.min, patch.max), patch.perimeter*0.5});
  return enclosingRadius(error);
  }

bool Builder::retainsFoliageBoundary(const Indices& original, const Indices& simplified,
                                     const Boundary& open, const Boundary& replacement,
                                     const std::vector<uint8_t>& locks) const {
  const auto locked = [&](uint32_t id) { return (locks[id] & meshopt_SimplifyVertex_Lock) != 0; };
  std::set<uint32_t> required;
  for(uint32_t wedge : original)
    if(locked(wedge))
      required.insert(wedge);
  for(uint32_t wedge : simplified)
    required.erase(wedge);
  if(!required.empty())
    return false;
  for(const auto& [edge, winding] : open) {
    if(!locked(edge[0]) || !locked(edge[1]))
      continue;
    const auto found = replacement.find(edge);
    if(found == replacement.end() || found->second != winding)
      return false;
    }
  return true;
  }

// PreserveArea's free-border expansion follows meshoptimizer v1.3 dilateBorders.
// The replacement gets its own positions, and a group with unsafe expansion stays fine.
bool Builder::dilateBorders(const Indices& original, const Indices& simplified, const Boundary& open,
                            const Boundary& replacement, const std::vector<uint8_t>& locks,
                            float budget, BorderMoves& moves, float& dilation) const {
  struct Offset { Vector direction{}; double length = 0; };
  std::map<uint32_t, Offset> offsets;
  double perimeter = 0;
  // A triangulated opaque shoot can gain border-incident faces while
  // shrinking overall. Use the foliage surface itself to measure that loss.
  const bool preserveSurfaceArea = !result.skin.empty();
  const auto locked = [&](uint32_t id) { return (locks[id] & meshopt_SimplifyVertex_Lock) != 0; };
  const auto measure = [&](const Indices& indices, const Boundary& border, bool accumulate) {
    double area = 0;
    for(size_t i = 0; i < indices.size(); i += 3) {
      bool touchesBorder = false;
      for(size_t corner = 0; corner < 3; ++corner) {
        const uint32_t a = positions[indices[i + corner]];
        const uint32_t b = positions[indices[i + (corner + 1) % 3]];
        const uint32_t c = positions[indices[i + (corner + 2) % 3]];
        const auto edge = border.find({std::min(a,b), std::max(a,b)});
        if(edge == border.end() || edge->second != (a < b ? 1 : -1) || (locked(a) && locked(b)))
          continue;
        touchesBorder = true;
        if(!accumulate)
          continue;
        const auto ab = delta(wedges[b].position, wedges[a].position);
        auto ca = delta(wedges[a].position, wedges[c].position);
        const double edgeSquared = dot(ab, ab);
        if(edgeSquared == 0)
          continue;
        const double projection = dot(ca, ab) / edgeSquared;
        for(size_t axis = 0; axis < 3; ++axis)
          ca[axis] -= ab[axis] * projection;
        const double outwardLength = std::sqrt(dot(ca, ca));
        if(outwardLength == 0)
          continue;
        const double length = std::sqrt(edgeSquared);
        perimeter += preserveSurfaceArea && (locked(a) || locked(b)) ? length * 0.5 : length;
        for(uint32_t point : {a, b}) {
          if(locked(point))
            continue;
          auto& offset = offsets[point];
          offset.length += length;
          for(size_t axis = 0; axis < 3; ++axis)
            offset.direction[axis] += ca[axis] * (length / outwardLength);
          }
        }
      if(touchesBorder || preserveSurfaceArea) {
        const auto n = normal(wedges[indices[i]].position, wedges[indices[i+1]].position, wedges[indices[i+2]].position);
        area += 0.5 * std::sqrt(dot(n,n));
        }
      }
    return area;
    };
  const double oldArea = measure(original, open, false);
  const double newArea = measure(simplified, replacement, true);
  if(newArea >= oldArea)
    return true;
  if(newArea < oldArea * 0.25 || perimeter == 0)
    return false;

  const double expansion = (oldArea - newArea) / perimeter;
  for(auto& [id, offset] : offsets) {
    auto& n = offset.direction;
    for(double& axis : n)
      axis /= offset.length;
    const double miter = expansion / std::max(0.15, dot(n,n));
    const auto& before = wedges[id].position;
    Point after;
    for(size_t axis = 0; axis < 3; ++axis)
      after[axis] = float(double(before[axis]) + n[axis] * miter);
    const double shift = distance(before, after);
    if(shift > budget)
      return false;
    if(shift > 0)
      moves.emplace(id, after);
    dilation = std::max(dilation, enclosingRadius(shift));
    }
  const auto after = [&](uint32_t wedge) -> const Point& {
    const auto moved = moves.find(positions[wedge]);
    return moved == moves.end() ? wedges[wedge].position : moved->second;
    };
  for(size_t i = 0; i < simplified.size(); i += 3) {
    const auto a = simplified[i], b = simplified[i+1], c = simplified[i+2];
    const auto before = normal(wedges[a].position, wedges[b].position, wedges[c].position);
    if(dot(before,before) > 0 && dot(before, normal(after(a), after(b), after(c))) <= 0)
      return false;
    }
  return true;
  }

void Builder::applyBorderMoves(Indices& simplified, const BorderMoves& moves, std::vector<uint8_t>& nextFixed) {
  std::map<uint32_t,uint32_t> newWedge, newPosition;
  for(uint32_t& id : simplified) {
    const uint32_t p = positions[id];
    const auto moved = moves.find(p);
    if(moved == moves.end())
      continue;
    const uint32_t next = offset(wedges.size());
    const auto [it, inserted] = newWedge.try_emplace(id, next);
    if(inserted) {
      auto wedge = wedges[id];
      auto vertex = result.vertices[wedge.vertex];
      if(!result.skin.empty()) {
        const auto binding = result.skin[wedge.vertex];
        result.skin.push_back(binding);
        }
      vertex.position = wedge.position = moved->second;
      wedge.vertex = offset(result.vertices.size());
      result.vertices.push_back(vertex);
      wedges.push_back(wedge);
      materials.push_back(wedge.material);
      positions.push_back(newPosition.try_emplace(p, next).first->second);
      nextFixed.push_back(0);
      }
    id = it->second;
    }
  }

void Builder::finishGroup(const Indices& clusters, const std::vector<uint8_t>& locks,
                          Indices& pending, std::vector<uint8_t>& nextFixed) {
  Indices original;
  for(uint32_t id : clusters)
    appendIndices(original, result.clusters[id], false);

  const bool foliage = profile != Profile::World;
  const bool keepOpen = !foliage && variant != CensusVariant::NoOpenBorder && variant != CensusVariant::NoBoundaries;
  const auto open = openBoundary(original);
  const auto material = wedges[original.front()].material;
  const bool mixed = std::ranges::any_of(original, [&](uint32_t id) { return wedges[id].material != material; });
  const auto contours = mixed ? materialContours(original, open) : MaterialContours{};
  auto groupLocks = locks;
  groupLocks.resize(wedges.size());
  auto positionLocks = std::vector<uint8_t>(wedges.size());
  if(keepOpen)
    for(const auto& [edge, count] : open)
      for(uint32_t position : edge)
        positionLocks[position] = meshopt_SimplifyVertex_Lock;

  // Keep contour junctions. Degree-two bends can move along their own seam.
  for(const auto& [id, edges] : contours)
    for(const auto& [position, links] : contourPoints(edges))
      if(links.incoming != 1 || links.outgoing != 1)
        positionLocks[position] = meshopt_SimplifyVertex_Lock;
  if(foliage)
    for(const auto& [position, links] : contourPoints(open))
      if(links.incoming != 1 || links.outgoing != 1)
        positionLocks[position] = meshopt_SimplifyVertex_Lock;

  // Preserve attribute splits within each material. Across materials, the
  // tag check pairs wedges without requiring the two charts to share attributes.
  std::map<std::pair<uint32_t, uint32_t>, uint32_t> charts;
  if(variant != CensusVariant::NoAttributeSeams) {
    for(uint32_t wedge : original) {
      const auto [it, inserted] = charts.try_emplace({positions[wedge], materials[wedge]}, wedge);
      if(!inserted && wedges[wedge].attributes != wedges[it->second].attributes)
        positionLocks[positions[wedge]] |= foliage ? meshopt_SimplifyVertex_Lock : meshopt_SimplifyVertex_Protect;
      }
    }
  for(uint32_t wedge : original)
    groupLocks[wedge] |= positionLocks[positions[wedge]];
  if(foliage)
    for(uint32_t wedge : original)
      groupLocks[positions[wedge]] |= groupLocks[wedge];

  Indices simplified(original.size());
  float error = 0, positionError = 0;
  constexpr std::array<float, AttributeWeightsCm.size()> ZeroWeights{};
  const auto& weights = variant == CensusVariant::PositionsOnly ? ZeroWeights : AttributeWeightsCm;
  const unsigned options = meshopt_SimplifySparse | meshopt_SimplifyPreserveFolds |
                           meshopt_SimplifyPermissive;
  simplified.resize(meshopt_simplifyWithAbsoluteAttributes(simplified.data(), original.data(), original.size(),
                      wedges.front().position.data(), wedges.size(), sizeof(Wedge),
                      wedges.front().attributes.data(), sizeof(Wedge), weights.data(), weights.size(),
                      groupLocks.data(), mixed ? materials.data() : nullptr, (original.size() / 6) * 3,
                      options, &error, &positionError));

  StopReason stop = StopReason::Count;
  float seamError = 0, dilation = 0;
  BorderMoves moves;
  if(simplified.empty() || simplified.size() >= original.size() ||
     (variant != CensusVariant::AnyReduction && simplified.size() * 100 > original.size() * 85)) {
    stop = StopReason::Reduction;
    }
  else {
    const auto replacementOpen = openBoundary(simplified);
    if(foliage)
      seamError = materialError({{material, open}}, simplified, {});
    else if(mixed)
      seamError = materialError(contours, simplified, replacementOpen);
    if(!std::isfinite(seamError))
      stop = StopReason::Material;
    else if((keepOpen && open != replacementOpen) || !retainsLocks(original, simplified, locks))
      stop = StopReason::Boundary;
    else if(foliage && !retainsFoliageBoundary(original, simplified, open, replacementOpen, groupLocks))
      stop = StopReason::Boundary;
    else if(foliage && !dilateBorders(original, simplified, open, replacementOpen, groupLocks,
                                     std::max({error, positionError, seamError}), moves, dilation))
      stop = StopReason::Area;
    }

  auto group = groupBounds(clusters);
  const uint32_t groupId = offset(result.groups.size());
  const float localPositionError = positionError;
  float coverageError = 0, childCoverageError = 0;
  for(uint32_t id : clusters) {
    const uint32_t child = result.clusters[id].refinedGroup;
    if(child != NoGroup) {
      childCoverageError = std::max(childCoverageError, coverageErrors[child]);
      if(stats)
        positionError = std::max(positionError, stats->positionErrorCm[child]);
      }
    result.clusters[id].parentGroup = groupId;
    }

  if(stop == StopReason::Count) {
    const float shapeError = std::max(seamError, foldError(original, simplified));
    constexpr float RoundingGuard = 1.f + 8 * std::numeric_limits<float>::epsilon();
    if(foliage) {
      const float localError = std::max({error, result.skin.empty() ? positionError : localPositionError, shapeError});
      group.errorCm = enclosingRadius((double(group.errorCm) + localError +
                                      dilation) * RoundingGuard);
      applyBorderMoves(simplified, moves, nextFixed);
      }
    else {
      coverageError = enclosingRadius(double(childCoverageError) + shapeError);
      group.errorCm = std::max({group.errorCm, error, positionError, coverageError}) * RoundingGuard;
      }
    const size_t first = pending.size();
    clusterize(simplified, result.clusters[clusters.front()].level + 1, groupId, pending);
    if(foliage) {
      std::vector<Sphere> bothSides = {group.bounds};
      for(size_t i = first; i < pending.size(); ++i)
        bothSides.push_back(result.clusters[pending[i]].bounds);
      group.bounds = mergeSpheres(bothSides);
      }
    }
  else {
    group.errorCm = Infinity;
    positionError = Infinity;
    if(variant != CensusVariant::NoBoundaries)
      for(uint32_t wedge : original)
        nextFixed[positions[wedge]] = meshopt_SimplifyVertex_Lock;
    if(stats) {
      ++stats->stoppedGroups[size_t(stop)];
      stats->stoppedTriangles[size_t(stop)] += original.size() / 3;
      }
    }
  result.groups.push_back(group);
  coverageErrors.push_back(coverageError);
  if(stats) {
    stats->positionErrorCm.push_back(positionError);
    if(foliage)
      stats->dilationCm.push_back(stop == StopReason::Count ? dilation : 0.f);
    }
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
  static_assert(sizeof(Vertex) == 36 && sizeof(Cluster) == 48 && sizeof(Group) == 20 && sizeof(Skin) == 8);
  return vertices.size() * sizeof(Vertex) + vertexIndices.size() * sizeof(uint32_t) + indices.size() +
         clusters.size() * sizeof(Cluster) + groups.size() * sizeof(Group) + skin.size() * sizeof(Skin);
  }

Graph build(std::span<const Vertex> vertices, std::span<const Triangle> triangles, std::span<const uint8_t> boundary) {
  return Builder(vertices, triangles, boundary, nullptr, CensusVariant::Normal).finish();
  }

Graph buildWithStats(std::span<const Vertex> vertices, std::span<const Triangle> triangles,
                     std::span<const uint8_t> boundary, BuildStats& stats, CensusVariant variant) {
  stats = {};
  return Builder(vertices, triangles, boundary, &stats, variant).finish();
  }

Graph buildFoliage(std::span<const Vertex> vertices, std::span<const Triangle> triangles,
                   std::span<const uint8_t> boundary, BuildStats& stats, bool logical128, std::span<const Skin> skin) {
  stats = {};
  const auto profile = logical128 ? Profile::Foliage128 : Profile::Foliage64;
  return Builder(vertices, triangles, boundary, &stats, CensusVariant::Normal, profile, skin).finish();
  }

float projectError(const Group& group, const View& view) {
  const float x = group.bounds.center[0] - view.camera[0];
  const float y = group.bounds.center[1] - view.camera[1];
  const float z = group.bounds.center[2] - view.camera[2];
  if(view.farDepth > 0.f) {
    // Projection error only bounds surfaces that survive clipping on both sides of the transition.
    // An ancestor sphere encloses its transition spheres, so intersections refine the same cut.
    const float depth = x*view.forward[0] + y*view.forward[1] + z*view.forward[2];
    if(depth + group.bounds.radius >= view.farDepth - view.farGuard &&
       depth - group.bounds.radius <= view.farDepth + view.farGuard)
      return Infinity;
    }
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
