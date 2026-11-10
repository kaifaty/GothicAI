#pragma once

#include <Tempest/Matrix4x4>
#include <Tempest/Vec>

#include <cstddef>
#include <cstdint>

#include "resources.h"

// The UV density of a texture owner (reports/engine/2026-11-10-texture-mips/README.md): the lowest number of UV units
// per metre over its triangles - the most stretched triangle decides, it shows the largest texels - and the bounds of
// the triangles counted. Triangles with no world area (under a square centimetre) or no UV area are left out.
struct TexelDensity {
  float         uvPerMetre = 0;  // 0: no usable triangle; such an owner keeps the whole chain, unless it is flat
  Tempest::Vec3 min, max;        // bounds of the counted triangles, in the units of the vertices
  bool          flat = false;    // it has triangles, none with any world area: nothing to draw

  bool valid() const { return uvPerMetre>0; }

  // Triangles of indices [first, first+count), positions in units of metresPerUnit (the game's centimetres).
  static TexelDensity measure(const Resources::Vertex* vertices, size_t vertexCount, const uint32_t* indices,
                              size_t first, size_t count, float metresPerUnit = 0.01f);

  // The same owner placed by `transform`: bounds through it, the density divided by its largest axis scale.
  TexelDensity placed(const Tempest::Matrix4x4& transform) const;
  };
