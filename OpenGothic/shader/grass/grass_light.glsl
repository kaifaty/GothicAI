#ifndef GRASS_LIGHT_GLSL
#define GRASS_LIGHT_GLSL

#include "common.glsl"

// The grass system (reports/engine/2026-11-14-grass-system): a blade under the sun's shadow map.

// A blade's pixel (grass.frag) and the far ground of a meadow (grass_ground.glsl) carry the leaf's mark without
// the mark of the alpha test; a card of leaves is cut by alpha and carries both
bool isGBufGrass(float hint) {
  const int x = int(hint*255.0 + 0.5);
  return (x & (GBufFoliageBit | (1 << 2)))==GBufFoliageBit;
  }

// A blade is thinner than the shadow's filter and turns its width to the eye, so its own plane (geomNormal, from
// the depth buffer) stands upright while its normal is three quarters the ground's. The filter carries the
// receiver's plane over its taps: an upright one went under the ground behind the blade and read the ground as
// the blade's shadow - the tall grass stood dark in the open sun. A blade whose plane is not its normal's takes
// the plane of its normal, as a facet edge-on to a lamp does (light.frag); the ground keeps its own
bool isGrassBlade(float hint, vec3 geomNormal, vec3 normal) {
  return isGBufGrass(hint) && dot(geomNormal,normal)<0.8;
  }

#endif
