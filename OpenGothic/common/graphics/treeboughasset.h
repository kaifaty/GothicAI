#pragma once

#include <Tempest/StorageBuffer>
#include <Tempest/Vec>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

#include "graphics/material.h"

class StaticMesh;

namespace TreeBough {

constexpr uint32_t NoNode = UINT32_MAX;
constexpr uint32_t MaxIndices = 48;
constexpr uint32_t MaxVertices = 32;
constexpr uint32_t DescendantSide = 0x8000u;

// Preorder forest and matching compact draw range (tree_bough.glsl).
struct alignas(16) Node {
  Tempest::Vec4 sphere;
  Tempest::Vec3 boxMin;
  float diameter = 0;
  Tempest::Vec3 boxMax;
  float thresholdPx = 0;
  float lowPx = 0;
  float highPx = 0;
  uint32_t parent = NoNode;
  uint32_t subtreeEnd = 0;
  };
static_assert(sizeof(Node)==64);

struct Range {
  uint32_t firstIndex = 0;
  uint32_t indexCount = 0;
  uint32_t primitive = 0;
  uint32_t cardCount = 0;
  };
static_assert(sizeof(Range)==16);

struct Page {
  Material material;
  Tempest::StorageBuffer ibo, vbo;
  };

struct Asset {
  std::vector<Node> nodes;
  std::vector<Range> ranges;
  std::vector<uint32_t> roots;
  std::vector<Page> pages; // GLB primitive order, with no compact geometry for wood
  uint32_t sourceSub = 0; // corresponding original canopy submesh
  uint32_t sourceCards = 0;
  uint32_t sourceTriangles = 0;
  };

struct Projection {
  Tempest::Vec3 forwardLocal; // one view-depth coefficient per local-space axis
  float originDepth = 0;
  float focalScale = 0;
  float nearZ = 0;
  };

struct Selected {
  uint32_t node;
  uint32_t transition; // index + 1 of the band owner, with DescendantSide set for its children
  float weight; // parent share in the active band
  };

bool enabled();
bool cpuEnabled();

// Attach only on success. Malformed or unsupported sidecars keep the original mesh.
void attach(StaticMesh& mesh, const std::filesystem::path& file);

float screenSize(const Node& node, const Projection& view);
float parentWeight(const Node& node, float px);
float impostorJointWeight(float threshold, float px);
uint32_t impostorJointFade(float weight);
void select(const Asset& asset, const Projection& view, std::vector<Selected>& out);

}
