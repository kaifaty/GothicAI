#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

// Graphics-independent cloth tessellation, in Gothic centimetres. Vertex has
// the same fields as Resources::Vertex, without its graphics dependencies.
namespace ClothGeometry {
  struct Vertex {
    float pos[3] = {};
    float norm[3] = {};
    float uv[2] = {};
    uint32_t color = 0xffffffffu;
    };

  // Apply these barycentric weights to the original vertex's position or morph
  // sample to reproduce the native fallback on the subdivided visible mesh.
  struct SourceWeights {
    std::array<uint32_t,3> vertices = {};
    std::array<float,3> weights = {};
    };

  struct Options {
    float targetEdgeCm = 25.f;
    size_t minParticles = 100;
    size_t maxParticles = 600;
    uint32_t maxDepth = 6;
    };

  struct Mesh {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<uint32_t> pins;
    std::vector<SourceWeights> sourceWeights;
    size_t uniquePositions = 0;
    float maxEdgeCm = 0.f;
    uint32_t subdivisionDepth = 0;
    bool budgetLimited = false;
    };

  // Uniform midpoint subdivision is conforming across every original edge.
  // UV seams, split normals and separately wound reverse faces retain separate
  // visible vertices; the particle budget welds positions to the same 0.01 cm
  // precision as Cloth::add. Original vertices stay first, unchanged.
  // New pins lie only on original edges with two pinned endpoints. A lone pin
  // never pins an adjacent edge or triangle interior. Invalid input throws
  // std::invalid_argument; an entirely empty input returns an empty mesh.
  Mesh subdivide(std::span<const Vertex> vertices,
                 std::span<const uint32_t> indices,
                 std::span<const uint32_t> pins,
                 const Options& options = {});
  }
