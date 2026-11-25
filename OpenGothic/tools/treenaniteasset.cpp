#include "treenaniteasset.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <fstream>
#include <stdexcept>

namespace TreeNanite {
namespace {

constexpr std::array<char, 4> Magic = {'T', 'N', 'D', 'G'};
constexpr float Infinity = std::numeric_limits<float>::infinity();
constexpr std::array<size_t, 6> Strides = {36, 20, 20, 40, 4, 4};
static_assert(std::endian::native == std::endian::little && std::numeric_limits<float>::is_iec559);
static_assert(sizeof(Vertex) == Strides[0] && sizeof(Group) == Strides[1] &&
              sizeof(Cluster) == Strides[2] && sizeof(Meshlet) == Strides[3]);

[[noreturn]] void reject(const char* reason) {
  throw std::runtime_error(std::string("Tree DAG: ") + reason);
  }

template<class T>
void read(std::istream& stream, T* data, size_t count) {
  const auto bytes = std::streamsize(sizeof(T) * count);
  if(count && !stream.read(reinterpret_cast<char*>(data), bytes))
    reject("truncated input");
  }

template<class T>
void read(std::istream& stream, std::vector<T>& array, uint32_t count) {
  array.resize(count);
  read(stream, array.data(), array.size());
  }

template<class T>
void write(std::ostream& stream, const T* data, size_t count) {
  const auto bytes = std::streamsize(sizeof(T) * count);
  if(count && !stream.write(reinterpret_cast<const char*>(data), bytes))
    reject("could not write output");
  }

template<class T>
void write(std::ostream& stream, const std::vector<T>& array) {
  write(stream, array.data(), array.size());
  }

bool finite(const auto& array) {
  return std::ranges::all_of(array, [](float x) { return std::isfinite(x); });
  }

void validate(const Asset& asset) {
  for(const auto& vertex : asset.vertices)
    if(!finite(vertex.position) || !finite(vertex.normal) || !finite(vertex.uv))
      reject("invalid vertex");
  for(const auto& group : asset.groups)
    if(!finite(group.bounds.center) || !std::isfinite(group.bounds.radius) ||
       group.bounds.radius < 0.f || !(group.errorCm >= 0.f))
      reject("invalid transition");

  if(asset.clusters.empty())
    reject("no clusters");
  std::vector<bool> fine(asset.groups.size()), coarse(asset.groups.size());
  size_t meshletEnd = 0, vertexEnd = 0, primitiveEnd = 0;
  for(const auto& cluster : asset.clusters) {
    if(cluster.parentGroup >= asset.groups.size() || cluster.firstMeshlet != meshletEnd ||
       cluster.meshletCount == 0 || cluster.meshletCount > asset.meshlets.size() - meshletEnd)
      reject("invalid cluster");
    fine[cluster.parentGroup] = true;
    if(cluster.refinedGroup != NoGroup) {
      if(cluster.refinedGroup >= cluster.parentGroup ||
         !(asset.groups[cluster.refinedGroup].errorCm < Infinity) ||
         asset.groups[cluster.refinedGroup].errorCm > asset.groups[cluster.parentGroup].errorCm)
        reject("invalid cluster transition");
      coarse[cluster.refinedGroup] = true;
      }

    size_t logicalTriangles = 0;
    meshletEnd += cluster.meshletCount;
    for(size_t i = cluster.firstMeshlet; i < meshletEnd; ++i) {
      const auto& meshlet = asset.meshlets[i];
      if(meshlet.firstVertex != vertexEnd || meshlet.firstPrimitive != primitiveEnd ||
         meshlet.vertexCount == 0 || meshlet.vertexCount > MaxMeshletVertices ||
         meshlet.vertexCount > asset.vertexIds.size() - vertexEnd ||
         meshlet.primitiveCount == 0 || meshlet.primitiveCount > MaxMeshletPrimitives ||
         meshlet.primitiveCount > asset.primitives.size() - primitiveEnd ||
         !finite(meshlet.boxMin) || !finite(meshlet.boxMax))
        reject("invalid meshlet");

      logicalTriangles += meshlet.primitiveCount;
      vertexEnd += meshlet.vertexCount;
      primitiveEnd += meshlet.primitiveCount;
      for(size_t j = meshlet.firstVertex; j < vertexEnd; ++j) {
        const uint32_t id = asset.vertexIds[j];
        if(id >= asset.vertices.size())
          reject("invalid global vertex index");
        for(size_t axis = 0; axis < 3; ++axis)
          if(asset.vertices[id].position[axis] < meshlet.boxMin[axis] ||
             asset.vertices[id].position[axis] > meshlet.boxMax[axis])
            reject("vertex outside meshlet bounds");
        }
      for(size_t j = meshlet.firstPrimitive; j < primitiveEnd; ++j) {
        const uint32_t primitive = asset.primitives[j];
        if((primitive >> 24) != 0 || (primitive & 255u) >= meshlet.vertexCount ||
           ((primitive >> 8) & 255u) >= meshlet.vertexCount || ((primitive >> 16) & 255u) >= meshlet.vertexCount)
          reject("invalid local primitive indices");
        }
      }
    if(logicalTriangles > MaxLogicalTriangles)
      reject("too many triangles in a logical cluster");
    }
  if(meshletEnd != asset.meshlets.size() || vertexEnd != asset.vertexIds.size() || primitiveEnd != asset.primitives.size())
    reject("unreferenced meshlet data");
  for(size_t group = 0; group < asset.groups.size(); ++group)
    if(!fine[group] || coarse[group] != std::isfinite(asset.groups[group].errorCm))
      reject("incomplete transition");
  }

double projectError(const Group& group, const View& view, const Point& translation, float scale) {
  const auto& bounds = group.bounds;
  const double radius = double(bounds.radius) * scale;
  double depth = 0, distanceSquared = 0;
  for(size_t axis = 0; axis < 3; ++axis) {
    const double delta = double(bounds.center[axis]) * scale + translation[axis] - view.camera[axis];
    depth += delta * view.forward[axis];
    distanceSquared += delta * delta;
    }
  const double guard = 8. * std::numeric_limits<float>::epsilon() * std::max(1., std::abs(depth) + radius);
  const auto crosses = [&](float plane) { return std::abs(depth - plane) <= radius + guard; };
  if(crosses(view.nearDepth) || crosses(view.farDepth))
    return Infinity;
  const double distance = std::max(std::sqrt(distanceSquared) - radius, double(view.nearDepth));
  return double(group.errorCm) * scale * (0.5 * view.verticalProjection * view.viewportHeight) / distance;
  }

}

void write(const std::filesystem::path& path, const Asset& asset) {
  validate(asset);
  const std::array sizes = {asset.vertices.size(), asset.groups.size(), asset.clusters.size(),
                            asset.meshlets.size(), asset.vertexIds.size(), asset.primitives.size()};
  std::array<uint32_t, sizes.size()> counts;
  for(size_t i = 0; i < counts.size(); ++i) {
    if(sizes[i] >= NoGroup)
      reject("section exceeds 32-bit indices");
    counts[i] = uint32_t(sizes[i]);
    }
  std::ofstream stream(path, std::ios::binary);
  if(!stream)
    reject("cannot open output");
  write(stream, Magic.data(), Magic.size());
  write(stream, &FormatVersion, 1);
  write(stream, asset.sourceHash.data(), asset.sourceHash.size());
  write(stream, asset.materialHash.data(), asset.materialHash.size());
  write(stream, counts.data(), counts.size());
  write(stream, asset.vertices);
  write(stream, asset.groups);
  write(stream, asset.clusters);
  write(stream, asset.meshlets);
  write(stream, asset.vertexIds);
  write(stream, asset.primitives);
  if(!stream.flush())
    reject("could not flush output");
  }

Asset load(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary | std::ios::ate);
  if(!stream)
    reject("cannot open input");
  const auto fileSize = stream.tellg();
  stream.seekg(0);
  auto magic = Magic;
  uint32_t version;
  Asset asset;
  std::array<uint32_t, Strides.size()> counts;
  read(stream, magic.data(), magic.size());
  read(stream, &version, 1);
  if(magic != Magic || version != FormatVersion)
    reject("unsupported file format");
  read(stream, asset.sourceHash.data(), asset.sourceHash.size());
  read(stream, asset.materialHash.data(), asset.materialHash.size());
  read(stream, counts.data(), counts.size());
  auto declaredSize = stream.tellg();
  for(size_t i = 0; i < counts.size(); ++i)
    declaredSize += std::streamoff(counts[i]) * std::streamoff(Strides[i]);
  if(fileSize != declaredSize)
    reject("invalid section sizes");
  read(stream, asset.vertices, counts[0]);
  read(stream, asset.groups, counts[1]);
  read(stream, asset.clusters, counts[2]);
  read(stream, asset.meshlets, counts[3]);
  read(stream, asset.vertexIds, counts[4]);
  read(stream, asset.primitives, counts[5]);
  validate(asset);
  return asset;
  }

bool clusterSelected(const Asset& asset, const Cluster& cluster, const View& view,
                     const Point& translation, float scale, float thresholdPx) {
  if(thresholdPx <= 0.f)
    return cluster.refinedGroup == NoGroup;
  const auto error = [&](uint32_t group) { return projectError(asset.groups[group], view, translation, scale); };
  return (cluster.refinedGroup == NoGroup || error(cluster.refinedGroup) <= thresholdPx) &&
         (asset.groups[cluster.parentGroup].errorCm == Infinity || error(cluster.parentGroup) > thresholdPx);
  }

bool meshletVisible(const Meshlet& meshlet, const View& view, const Point& translation, float scale) {
  for(const auto& plane : view.frustum) {
    double support = plane.offset, magnitude = std::abs(plane.offset);
    for(size_t axis = 0; axis < 3; ++axis) {
      const double n = plane.normal[axis];
      const double model = n < 0 ? meshlet.boxMin[axis] : meshlet.boxMax[axis];
      const double scaled = model * scale;
      support += n * (scaled + translation[axis]);
      magnitude += std::abs(n) * (std::abs(scaled) + std::abs(translation[axis]));
      }
    if(support < -8. * std::numeric_limits<float>::epsilon() * magnitude)
      return false;
    }
  return true;
  }

}
