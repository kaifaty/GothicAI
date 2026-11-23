#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "graphics/grass/grassplace.h"

// The border of a meadow with the ground beside it (reports/engine/2026-11-14-meadow-ground): the open edges of
// the grass's triangles, found from the chunks' meshes as they are read - no field of the world's format. One
// finder serves the blades (grassplace.cpp: the band at an open edge, the roots past it) and the ground's shader
// (the pieces along a ground with `verge`, packed into one buffer: shader/grass/grass_border.glsl), so the two
// see the same line.
namespace Grass {

// An open edge of the grass, or a part of it: grass on its left only
struct OpenEdge final {
  int32_t  ax = 0, az = 0, bx = 0, bz = 0; // cm
  float    outX = 0.f, outZ = 0.f;
  uint32_t tri = 0;                        // of `tris`: the triangle it is an edge of
  bool     verge = false;                  // the ground past it has `verge` of this grass
  uint8_t  reach = 0, margin = 0;          // cm, that record's
  };

constexpr int32_t VergeStep = 40*Unit;     // the verge lies no further off the edge's height

// The order of the triangles that does not depend on the chunks'
bool triLess(const Tri& l, const Tri& r);
bool insideXZ(const int32_t x[3], const int32_t z[3], int32_t px, int32_t pz);

void addVerge(Summary& out, const float pos[3][3], const float normal[3][3], uint16_t slot, float reach, float margin);

// tris: sorted by triLess, all the grass near the box lo..hi (units) and `band` round it. An edge one triangle has,
// whose pieces of 50 cm have no grass 6 cm outside (so an edge that meets two shorter ones of its neighbours is not
// open). Neighbouring open pieces are joined; a piece with a verge of its grass under that point is `verge`
void openEdges(std::span<const Tri*const> tris, std::span<const VergeTri*const> verge,
               const int32_t lo[2], const int32_t hi[2], int32_t band, std::vector<OpenEdge>& out);

// A piece of the border with a verge, as the ground's shader takes it
struct BorderSeg final {
  float    ax = 0.f, az = 0.f, bx = 0.f, bz = 0.f; // cm, whole numbers: the ends the blades count from
  uint32_t reach = 0, margin = 0;
  };

// The verge pieces of the open edges of `own`'s grass. around: the chunk and those round it that are in
std::vector<BorderSeg> chunkBorder(const Summary& own, std::span<const Summary*const> around);

// The buffer of the ground's shader: 4 words of a head (the side of the table, the bin's size in cm, 0, 0), a
// table of side*side words (offset << 6 | count) wrapped round the world by the bin's index, then 5 words a piece
// (four floats, reach | margin << 8). A piece is in every bin it comes within BorderPad of
constexpr uint32_t BorderSide = 256;
constexpr float    BorderBin  = 400.f;
constexpr float    BorderPad  = 180.f;

std::vector<uint32_t> packBorder(std::span<const BorderSeg> segs);

struct BorderHit final {
  bool  found = false;
  float dist  = 0.f; // cm
  float reach = 0.f, margin = 0.f;
  };
// The nearest piece within `limit` (not over BorderPad) of a place, cm: what grassBorder of the shader finds
BorderHit nearestBorder(std::span<const uint32_t> grid, float x, float z, float limit);

}
