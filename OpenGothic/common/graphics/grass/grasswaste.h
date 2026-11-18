#pragma once

#include <Tempest/Vec>

#include <cstddef>
#include <cstdint>
#include <span>

#include "graphics/grass/grassplace.h"

class Frustrum;

// The blades the field submits for nothing (reports/engine/2026-11-17-grass-cost, "Сколько травинок подаётся зря").
// A cell of 8 m is drawn whole when its box is in view, with the ranks that stand at its nearest point; a blade that
// by its own distance has gone into the ground, or stands out of the view, still passes the pipeline. With
// OPENGOTHIC_GRASS_WASTE=1 the field keeps the blades on the CPU too and counts them every frame, and what cells
// cut into squares of 4, 2 and 1 m would submit by the same rules. A measure: nothing of a frame depends on it.
namespace Grass {

bool wasteOn();

struct Waste final {
  static constexpr size_t Subs = 3;                       // squares of 4, 2 and 1 m
  static constexpr float  SubCm[Subs] = {400.f, 200.f, 100.f};

  size_t gone          = 0; // submitted blades the vertex shader drops: of a rank or a distance that has no height
  size_t goneTriangles = 0;
  size_t out           = 0; // of the rest those wholly out of the view
  size_t outTriangles  = 0;
  size_t subDrawn[Subs]     = {};
  size_t subTriangles[Subs] = {};
  size_t subDraws[Subs]     = {}; // draw calls: a batch of a square with a blade to draw
  };

// What decides the count and the links of a batch at a distance (GrassField::draw and grass.vert)
struct WasteRule final {
  float          full      = 0.f; // cm: the ring of the batch's kind
  float          fadeFrom  = 0.f;
  float          radius    = 0.f;
  float          cellRadius= 0.f; // a cell further than this is not drawn at all
  float          rankShare = 0.f;
  float          rootsBeyond = 0.f;
  bool           fine      = false;

  size_t  ranks(float dist) const;
  uint8_t links(float dist) const;
  };

struct WasteCell final {
  Tempest::Vec3 origin;        // cm: the corner of the cell less its margin, its lowest root in y (the draw's push)
  float         x0 = 0, z0 = 0; // cm: the cell's corner
  float         size = 0;      // cm
  float         minY = 0, maxY = 0; // cm: of the cell's box in the view's test
  float         pad  = 0;      // cm: of that box in x and z
  };

// drawn: the blades of a batch the field has just submitted, by rank; links: of each
void countWaste(Waste& out, const Frustrum& fr, const Tempest::Vec3& cam, const WasteRule& rule, const WasteCell& cell,
                std::span<const Blade> drawn, uint8_t links);

}
