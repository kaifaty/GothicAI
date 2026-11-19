#ifndef GRASS_BLADE_HINT_GLSL
#define GRASS_BLADE_HINT_GLSL

#include "common.glsl"

// DrawBuckets keeps the window, foliage and grass-ground flags mutually exclusive.
// Reserve their otherwise unused hint combination for blades. Blade ORM alpha is
// 1, so the window's glow stays zero. The sun's grass test still sees both blades
// and the far grass ground, which keeps the foliage bit alone.
const int GBufGrassBladeHint = GBufFoliageBit | GBufWindowBit;

bool isGBufGrassBlade(float hint) {
  return int(hint*255.0 + 0.5)==GBufGrassBladeHint;
  }

#endif
