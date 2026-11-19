#ifndef PROBE_PIXEL_CAGE_GLSL
#define PROBE_PIXEL_CAGE_GLSL

#if defined(GRASS_CAGE)
#include "grass/grass_blade_hint.glsl"

layout(push_constant) uniform GrassCagePush {
  int grassCageSteps;
  };

// Allocation and the ambient gather must request the same cell for each pixel.
int probePixelLod(vec3 basePos, vec3 camPos, float lodScale, bool blade) {
  const int lod = probeGridLod(basePos, camPos, lodScale);
  return blade ? min(lod + grassCageSteps, 12) : lod;
  }
#else
// Keep the original shader path, including its texture reads, at zero steps.
#define probePixelLod(basePos, camPos, lodScale, blade) probeGridLod(basePos, camPos, lodScale)
#endif

#endif
