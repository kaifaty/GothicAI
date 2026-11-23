#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace ClusterLod {

constexpr uint32_t MaxVertices = 64;
constexpr uint32_t MaxTriangles = 64;
constexpr uint32_t NoGroup = std::numeric_limits<uint32_t>::max();

using Point = std::array<float, 3>;

struct Vertex {
  Point                position; // world centimetres
  Point                normal;
  std::array<float, 2> uv;
  uint32_t             color;    // RGBA8, red in the low byte
  };

struct Triangle {
  std::array<uint32_t, 3> vertices;
  uint32_t               material;
  bool                   locked = false; // stays in the cut at level 0
  };

struct Sphere {
  Point center;
  float radius;
  };

// Both sides of a replacement use this exact sphere and error when selecting a cut.
struct Group {
  Sphere bounds;
  float  errorCm; // positive infinity for a terminal group
  };

struct Cluster {
  Sphere   bounds;      // tight bounds of this meshlet, for culling
  uint32_t level;
  uint32_t material;
  uint32_t refinedGroup; // transition that produced this meshlet, or NoGroup at level 0 (error 0)
  uint32_t parentGroup;  // transition that replaces this meshlet, including terminal groups
  uint32_t vertexOffset;
  uint32_t indexOffset;
  uint32_t vertexCount;
  uint32_t triangleCount;
  };

struct Graph {
  std::vector<Vertex>   vertices;      // input vertex IDs and bit patterns are retained
  std::vector<uint32_t> vertexIndices; // meshlet-local vertex to graph vertex
  std::vector<uint8_t>  indices;       // three meshlet-local indices per triangle
  std::vector<Cluster>  clusters;
  std::vector<Group>    groups;

  size_t byteSize() const; // payload bytes, excluding vector bookkeeping and capacity
  };

// The open edge chain is derived from the triangles. A nonzero flag also freezes its position.
Graph build(std::span<const Vertex> vertices, std::span<const Triangle> triangles,
            std::span<const uint8_t> boundary);

enum class StopReason { Reduction, Material, Boundary, Locked, Count };
enum class CensusVariant { Normal, NoMaterials, NoOpenBorder, NoBoundaries, NoAttributeSeams, AnyReduction, PositionsOnly };

struct BuildStats {
  std::vector<float> positionErrorCm; // parallel to Graph::groups
  std::array<size_t, size_t(StopReason::Count)> stoppedGroups{}, stoppedTriangles{};
  };

// Counterfactual variants are for the census and can discard material or boundary guarantees.
Graph buildWithStats(std::span<const Vertex> vertices, std::span<const Triangle> triangles,
                     std::span<const uint8_t> boundary, BuildStats& stats,
                     CensusVariant variant = CensusVariant::Normal);

struct View {
  Point camera;
  float viewportHeight;     // positive, in pixels
  float verticalProjection; // positive magnitude of projection[1][1] for a perspective camera
  Point forward = {};       // unit direction into the main view
  float farDepth = 0;       // positive to protect transitions that cross the far clip plane
  float farGuard = 0;       // half width of the precision band around the plane, in centimetres
  };

float projectError(const Group& group, const View& view);
std::vector<uint32_t> selectCut(const Graph& graph, const View& view, float thresholdPx);

}
