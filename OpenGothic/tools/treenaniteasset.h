#pragma once

#include <filesystem>

#include "graphics/mesh/clusterlod.h"

namespace TreeNanite {

using ClusterLod::NoGroup;
using ClusterLod::Point;
using ClusterLod::Vertex;
using ClusterLod::Group;

inline constexpr uint32_t FormatVersion = 1;
inline constexpr uint32_t WindFormatVersion = 2;
inline constexpr uint32_t MaxLogicalTriangles = ClusterLod::MaxFoliageTriangles;
inline constexpr uint32_t MaxMeshletVertices = 64;
inline constexpr uint32_t MaxMeshletPrimitives = 64;

struct Cluster {
  uint32_t refinedGroup, parentGroup;
  uint32_t material;
  uint32_t firstMeshlet, meshletCount;
  };

struct Meshlet {
  Point boxMin, boxMax; // model-space centimetres, including the wind envelope
  uint32_t firstVertex, firstPrimitive;
  uint32_t vertexCount, primitiveCount;
  };

using ClusterLod::Skin;

enum BoneKind : uint32_t { RootBone, TrunkBone, WhorlBone, ShootBone };

struct Bone {
  Point pivot, axis;
  uint32_t parent, kind;
  };

// Upper rotation angles in radians, mirrored by tree_nanite_pose.comp.
inline constexpr std::array<float, 4> WindAngleLimit{0.f, 0.022f, 0.025f, 0.10f};

struct Asset {
  std::array<uint8_t, 32> sourceHash, materialHash;
  std::vector<Vertex> vertices;
  std::vector<Group> groups;
  std::vector<Cluster> clusters;
  std::vector<Meshlet> meshlets;
  std::vector<uint32_t> vertexIds;
  std::vector<uint32_t> primitives; // corner indices in bits 0..7, 8..15, 16..23
  std::vector<Bone> bones;
  std::vector<Skin> skin; // one entry per vertex, empty in format 1
  };

void write(const std::filesystem::path& path, const Asset& asset);
Asset load(const std::filesystem::path& path);
float windTravel(const Asset& asset, uint32_t vertexId);

struct Plane {
  Point normal;
  float offset;
  };

struct View {
  Point camera, forward; // world centimetres, unit view direction
  float viewportHeight, verticalProjection;
  float nearDepth, farDepth;
  std::array<Plane, 6> frustum; // inward world-space half-spaces
  };

bool clusterSelected(const Asset& asset, const Cluster& cluster, const View& view,
                     const Point& translation, float scale, float thresholdPx = 1.f);
bool meshletVisible(const Meshlet& meshlet, const View& view, const Point& translation, float scale);

}
