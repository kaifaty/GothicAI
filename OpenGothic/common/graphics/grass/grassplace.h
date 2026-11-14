#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "graphics/grass/grassparams.h"

// Where the grass grows (reports/engine/2026-11-14-grass-system, "Где растёт трава"). No device, no engine state:
// the summary of a chunk's ground goes in, the blades of one cell of the world come out.
//
// Everything that decides whether a blade exists, where its root is and what its rank is, is counted in whole
// numbers: positions in 1/16 cm, the point in a triangle and its height in 64 bits, the masks by hashes of the
// world lattice's node. So the set of blades is the same on every machine and does not depend on the order the
// chunks came in, on the chunk a triangle belongs to or on the cell that asks. Fractions (the lean, the tint, the
// height) only dress a blade.
namespace Grass {

constexpr int32_t Unit       = 16;               // units per cm
constexpr int32_t NodeStep   = 4*Unit;           // the lattice of candidates: 625 nodes per m2
constexpr int32_t CellSize   = 800*Unit;         // a cell of the world: 8 m
constexpr int32_t CellNodes  = CellSize/NodeStep;
constexpr int32_t CellMargin = 64*Unit;          // a root lies no further out of the cell of its node
constexpr int32_t TuftStep   = 75*Unit;          // the lattice of the tufts' centres
constexpr int32_t NodesPerM2 = 625;
constexpr int32_t MaxExtent  = 1<<18;            // units: a longer triangle is left out (164 m)
constexpr int32_t PastEdge   = 35;               // cm: sparse low blades root this far past an open edge
constexpr int32_t PastStep   = 12*Unit;          // of the ground there against the grass's plane: more is a ledge
constexpr float   PastHeight = 12.f;             // cm: no blade past an edge is taller

// A triangle of a ground material with `grass`, as the chunk's mesh has it
struct Tri final {
  int32_t  x[3] = {}, y[3] = {}, z[3] = {};      // units; counter-clockwise seen from above
  float    u[3] = {}, v[3] = {};
  float    nx[3] = {}, ny[3] = {}, nz[3] = {};
  uint16_t slot = 0;                             // of the field's grass materials
  };

struct WaterTri final {
  int32_t x[3] = {}, y[3] = {}, z[3] = {};
  };

struct Summary final {
  std::vector<Tri>      grass;
  std::vector<WaterTri> water;
  std::vector<WaterTri> beside; // ground of other materials that touches the grass
  bool empty() const { return grass.empty(); }
  };

// pos, uv, normal: per vertex, the world's cm. Left out: a triangle on edge seen from above, one that faces down,
// one longer than MaxExtent
void addGrass(Summary& out, const float pos[3][3], const float uv[3][2], const float normal[3][3], uint16_t slot);
void addWater(Summary& out, const float pos[3][3]);
// Ground without grass that shares a corner with the grass: a root past an open edge stands on it. Left out: one
// that faces down or stands upright
void addBeside(Summary& out, const float pos[3][3], const float normal[3][3]);

// 16 bytes of a blade (shader/grass/grass_common.glsl):
//   w0: x 14 | z 14 | lean 4          root in units from the cell's corner less CellMargin
//   w1: y 16 | height 8 | rank 8      y in 1/8 cm over CellBlades::minY; height in 1/4 cm
//   w2: facing 8 | phase 8 | nx 8 | nz 8
//   w3: u 12 | v 12 | width 4 | tint 4
struct Blade final {
  uint32_t w[4] = {};
  };

struct Batch final {
  uint16_t slot  = 0;
  uint32_t first = 0;
  uint32_t count = 0;
  };

struct CellBlades final {
  std::vector<Blade> blades;   // by slot, then by rank: a start of a batch is an even sample of it
  std::vector<Batch> batches;
  int32_t            minY = 0; // units
  int32_t            maxY = 0;
  };

struct Disc final {            // a thing to pick up: the grass round it is low
  int32_t x = 0, z = 0;        // units
  };

struct CellInput final {
  int32_t                        cx = 0, cz = 0; // the cell [cx*CellSize, (cx+1)*CellSize) and the same in z
  std::span<const Summary*const> summaries;      // of the chunks round the cell; the order does not matter
  std::span<const Params>        params;         // by slot
  std::span<const Disc>          items;
  uint32_t                       rankLimit = 256; // blades of a rank below it only (a cell far from the camera)
  float                          density   = 1.f; // of the records' (the quality)
  };

CellBlades buildCell(const CellInput& in);

// A blade read back, for the tests and the counts
struct BladeInfo final {
  int32_t x = 0, y = 0, z = 0;  // units; y to 2 units
  float   height = 0.f;         // cm
  uint8_t rank   = 0;
  };
BladeInfo decode(const Blade& b, int32_t cx, int32_t cz, int32_t minY);

// The patches of the meadow: 0..65535 at a place (units), the same numbers as grassPatch of grass_common.glsl
int32_t patchNoise(int32_t x, int32_t z);
// The share of BladesPerM2 a patch keeps, in 1/256, and the height against the record's
int32_t patchDensity(int32_t noise);
float   patchHeight(int32_t noise);

}
