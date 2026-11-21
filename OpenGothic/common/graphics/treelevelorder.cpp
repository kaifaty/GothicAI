#include "treelevelorder.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <numeric>
#include <vector>

#include "asset/staticmesh.h"

void TreeLevelOrder::sortInSpace(asset::StaticMesh& mesh) {
  const size_t count = mesh.positions.size();
  if(count==0)
    return;
  // pieces: positions joined by a triangle
  std::vector<uint32_t> piece(count);
  std::iota(piece.begin(), piece.end(), 0u);
  const auto root = [&](uint32_t i) {
    while(piece[i]!=i) {
      piece[i] = piece[piece[i]];
      i = piece[i];
      }
    return i;
    };
  for(const auto& sub:mesh.sub_meshes)
    for(const auto& triangle:sub.triangles) {
      const uint32_t first = root(sub.wedges[triangle.wedges[0]].index);
      piece[root(sub.wedges[triangle.wedges[1]].index)] = first;
      piece[root(sub.wedges[triangle.wedges[2]].index)] = first;
      }

  struct Middle { std::array<double,3> sum {}; uint32_t count = 0; };
  std::vector<Middle> middles(count);
  std::array<float,3> low = {mesh.positions[0].x, mesh.positions[0].y, mesh.positions[0].z}, high = low;
  for(size_t i=0; i<count; ++i) {
    auto& middle = middles[root(uint32_t(i))];
    const float at[3] = {mesh.positions[i].x, mesh.positions[i].y, mesh.positions[i].z};
    for(size_t axis=0; axis<3; ++axis) {
      middle.sum[axis] += double(at[axis]);
      low[axis]  = std::min(low[axis],  at[axis]);
      high[axis] = std::max(high[axis], at[axis]);
      }
    ++middle.count;
    }
  // the Z-order curve of a 1024^3 grid over the mesh's box, a cube; pieces of one cell keep apart by their roots
  const double size = std::max({double(high[0]-low[0]), double(high[1]-low[1]), double(high[2]-low[2]), 1e-6});
  std::vector<uint64_t> key(count);
  for(size_t i=0; i<count; ++i) {
    const uint32_t id = root(uint32_t(i));
    const auto& middle = middles[id];
    uint64_t code = 0;
    for(size_t axis=0; axis<3; ++axis) {
      const double at = (middle.sum[axis]/double(middle.count) - double(low[axis]))/size;
      const auto cell = uint32_t(std::clamp(at*1023.0, 0.0, 1023.0));
      for(uint32_t bit=0; bit<10; ++bit)
        code |= uint64_t((cell >> bit) & 1u) << (3u*bit + axis);
      }
    key[i] = (code << 32) | id;
    }
  std::vector<uint32_t> order(count);
  std::iota(order.begin(), order.end(), 0u);
  std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return key[a]<key[b]; });

  std::vector<asset::Vec3> positions(count);
  std::vector<uint16_t>    moved(count); // wedges index positions with 16 bits
  for(size_t i=0; i<count; ++i) {
    positions[i]    = mesh.positions[order[i]];
    moved[order[i]] = uint16_t(i);
    }
  mesh.positions = std::move(positions);
  for(auto& sub:mesh.sub_meshes)
    for(auto& wedge:sub.wedges)
      wedge.index = moved[wedge.index];
  }
