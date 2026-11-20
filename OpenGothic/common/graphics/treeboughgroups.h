#pragma once

#include <algorithm>
#include <bit>

#include "graphics/mesh/submesh/packedmesh.h"
#include "graphics/treeboughasset.h"

// Whole mesh workgroups of the compact foliage cut (tree_bough_mesh.glsl): TreeBoughRender lays its commands out
// with these, and stage 2 of tree_bough_expand.comp pads the last group of each one.
namespace TreeBough {

// Nodes of one class share a mesh workgroup: as many as fit its vertices and primitives.
constexpr uint32_t groupSize(const Range& range) {
  const uint32_t size = std::max({range.vertexCount, range.indexCount/3u, 8u});
  return std::bit_floor(uint32_t(PackedMesh::MaxVert)/size);
  }

// Payload slots of a mesh command: its last workgroup is padded to whole `group` nodes.
constexpr uint32_t groupSlots(uint32_t nodes, uint32_t group) {
  return (nodes+group-1u)/group*group;
  }

}
