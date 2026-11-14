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

// A blade is thinner than the shadow's filter and turns its width to the eye. Seen against the sun, its own plane
// (geomNormal, from the depth buffer) has its back to the sun, while its normal, three quarters the ground's,
// takes the light: the filter carried that plane over the ground behind the blade and read half of it as the
// blade's shadow - the meadow against the light stood dark. Such a pixel takes the plane of its normal, as a facet
// edge-on to a lamp does (light.frag). The ground and a blade whose plane faces the sun keep their own
bool isGrassThin(float hint, vec3 geomNormal, vec3 normal, vec3 sunDir) {
  return isGBufGrass(hint) && geomNormal!=vec3(0) && dot(geomNormal,sunDir)<0.2 && dot(normal,sunDir)>0.0;
  }

#endif
