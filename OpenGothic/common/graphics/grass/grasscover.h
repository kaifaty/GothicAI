#pragma once

#include <cstdint>

// Where the sward lies on a meadow's own ground (reports/engine/2026-11-14-meadow-ground): one rule for the blades
// (grassplace.cpp) and for the ground's two layers (shader/grass/grass_cover.glsl). Whole numbers of the place in
// 1/16 cm, the same hashes as the patches', so the two agree without being told. Keep the numbers here and in the
// shader in step.
//
// The sward is whole but for two things. A spot of bare earth in the short sward: an outline bent by two noises, no
// circle. And the border with the ground beside the meadow (a road with `verge`): the line where the sward ends
// wanders about the mesh's edge, from a bay of earth in the meadow to a tongue of sward over the road, by a slow
// noise and a quick one, so no stretch of it is straight. On both a third noise, of the size of a tuft, breaks the
// last decimetres into tufts.
namespace Grass {

constexpr int32_t CoverNotch  = 35;  // cm: a bay of earth goes this far into the meadow
constexpr int32_t CoverSpan   = 125; // cm: from the deepest bay to the longest tongue, at a reach of 90
constexpr int32_t CoverRipple = 15;  // cm: the quick noise on the line, to either side
constexpr int32_t CoverSoft   = 40;  // cm: the sward thins over this across the line
constexpr int32_t CoverInside = 80;  // cm: deeper in the meadow the border is not felt
constexpr int32_t CoverReach  = 90;  // cm: the reach the span is given for
constexpr int32_t CoverSward  = 128; // of 256: the ground is sward where the level is over it
constexpr int32_t CoverBlades = 96;  // blades stand from this level: single ones on the earth of the rim

// cm past the border where no sward comes, at this reach of the record
constexpr int32_t coverBeyond(int32_t reach) {
  return reach + CoverRipple + CoverSoft;
  }
constexpr int32_t MaxVergeCm = coverBeyond(120); // at the longest reach a record may ask for (grassground.h)

// A spot of bare earth, 0..256, at a place in units
int32_t coverBare(int32_t x, int32_t z);
// The sward at a border, 0..256. s: units to the border, under 0 in the meadow, over 0 on the ground beside it;
// reach: cm, the record's
int32_t coverBorder(int32_t x, int32_t z, int32_t s, int32_t reach);
// The noise of the tufts on a level between none and whole, about 0: -CoverTuft..CoverTuft
constexpr int32_t CoverTuft = 72;
int32_t coverTuft(int32_t x, int32_t z);

// The level of the sward, 0..256 with the tufts' noise on what is neither none nor whole. border: 256 where no
// border is near
inline int32_t coverLevel(int32_t x, int32_t z, int32_t border) {
  const int32_t bare  = coverBare(x,z);
  const int32_t level = border<256-bare ? border : 256-bare;
  if(level<=0 || level>=256)
    return level;
  return level + coverTuft(x,z);
  }

// The share of the blades that stand at a level, 0..256
inline int32_t coverBlades(int32_t level) {
  const int32_t q = (level-CoverBlades)*256/(224-CoverBlades);
  return q<0 ? 0 : (q>256 ? 256 : q);
  }

}
