#pragma once

#include <Tempest/Vec>

#include <algorithm>
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

  // The submitted blades by the bands of their own distance from the camera (grass.vert's): 0-10, 10-24, 24-48 m
  // and beyond (reports/engine/2026-11-18-grass-volume: what a layer of volume over the far ground would take over).
  // Their triangles under each set of the broad blades' link bounds of step 1b of docs/grass-plan.md
  static constexpr size_t Bands    = 4;
  static constexpr float  BandCm[Bands-1] = {1000.f, 2400.f, 4800.f};
  static constexpr size_t LinkSets = 3;
  static constexpr float  LinkCm[LinkSets][3] = {{1000.f, 2500.f, 4500.f}, {800.f, 1200.f, 2400.f}, {600.f, 900.f, 1800.f}};
  size_t bandBlades[Bands]                       = {};
  size_t bandFine[Bands]                         = {}; // of them the short sward's fine blades
  size_t bandShown[Bands]                        = {}; // of them neither gone into the ground nor wholly out of view
  size_t bandTriangles[LinkSets][Bands]          = {};
  size_t bandShownTriangles[LinkSets][Bands]     = {};
  };

size_t  wasteBand(float dist);
uint8_t wasteLinks(size_t set, float cellDist); // a broad blade's under Waste::LinkCm[set]

// What decides the count and the links of a batch at a distance (GrassField::draw and grass.vert)
struct WasteRule final {
  float          full      = 0.f; // cm: the ring of the batch's kind
  float          fadeFrom  = 0.f;
  float          radius    = 0.f;
  float          cellRadius= 0.f; // a cell further than this is not drawn at all
  float          rankShare = 0.f;
  float          rootsBeyond = 0.f;
  bool           fine      = false;
  float          heightStep= 0.25f; // cm of the height byte (Grass::heightStep)

  size_t  ranks(float dist) const;
  uint8_t links(float dist) const;

  // The CPU counterpart of grassBladeHeight in grass_blade.glsl.
  float height(uint32_t rankByte, float dist, float baseHeight) const {
    const float rank = (float(rankByte)+0.5f)/256.f;
    const float grow = std::clamp((full/std::max(dist,1.f)*rankShare - rank)/0.12f, 0.f, 1.f);
    const float t    = std::clamp((dist-fadeFrom)/(radius-fadeFrom), 0.f, 1.f);
    return baseHeight*(grow*(1.f - t*t*(3.f-2.f*t)));
    }
  };

struct WasteCell final {
  Tempest::Vec3 origin;        // cm: the corner of the cell less its margin, its lowest root in y (the draw's push)
  float         x0 = 0, z0 = 0; // cm: the cell's corner
  float         size = 0;      // cm
  float         minY = 0, maxY = 0; // cm: of the cell's box in the view's test
  float         pad  = 0;      // cm: of that box in x and z
  float         dist = 0;      // cm: across to the cell's nearest point, of its links (Grass::broadLinks)
  };

// drawn: the blades of a batch the field has just submitted, by rank; links: of each
void countWaste(Waste& out, const Frustrum& fr, const Tempest::Vec3& cam, const WasteRule& rule, const WasteCell& cell,
                std::span<const Blade> drawn, uint8_t links);

}
