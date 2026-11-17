#ifndef CROWN_LIGHT_GLSL
#define CROWN_LIGHT_GLSL

#include "lighting/foliage_light.glsl"

// The light of a tree's crown (reports/engine/2026-11-16-modern-trees/crown-light.md; BK_CROWN of scene.glsl,
// common/graphics/crownlight.h). The vertices of the own GLB carry the openness of the crown where they stand
// (COLOR_0 A: 1 outside, 0 in the depth; 1 without the colour); vertex_process.glsl sends it in the normal's length.

// The openness out of the length of the interpolated normal. Normals of different directions come out shorter
// between the vertices (a trunk of ten sides: by 5 %), so the top tenth of the openness reads as 1 - a round mesh
// without the colour is lit as before
const float CrownOpenTop = 0.9;

float crownOpenness(vec3 normal) {
  return clamp((length(normal)*2.0 - 1.0)/CrownOpenTop, 0.0, 1.0);
  }

// What the crown leaves of the light of a pixel at this openness - of the sun, the lamps, the sky, the probes and
// of what comes through the leaf alike. Needles by the trunk of a spruce get 10-30 % of the diffuse light of an
// outer bough, and the sun reaches them as rare flecks (Beer-Lambert, the note). The shadows the engine has - of
// the cards on each other - leave the depth as much sun as the outside (the note, the first run), so the shade
// takes the sun as well. 1 at the outside: a mesh without the colour is lit as before. The G-buffer keeps it in
// the ORM alpha (gbufCrownShade of common.glsl)
const float CrownShadeFloor = 0.25;

float crownShade(float openness) {
  return mix(CrownShadeFloor, 1.0, openness);
  }

// The sky through a leaf, on the scale of the irradiance of a surface that faces away from the eye. The sun's lobe
// (foliageThrough: the fourth power of the cosine between the eye's ray and the light's) over a hemisphere of even
// light keeps 2/5 of that irradiance; it fades where the normal already faces that light, as the sun's does.
// view: from the pixel to the eye
const float CrownSkyLobe = 0.4;

float crownSkyThrough(float translucency, vec3 normal, vec3 view) {
  return CrownSkyLobe * foliageThrough(translucency, normal, view, -view);
  }

// OPENGOTHIC_CROWN_LIGHT=mask (VF_CROWN_MASK): the crown by thirds of the alpha of its vertices, the depth red, the outside
// blue; what lets no light through (the bark) magenta. Pure colours: the light changes how bright one is, not which
vec3 crownMask(float openness, float through) {
  if(through<=0.0)
    return vec3(0.6, 0.0, 0.6);
  if(openness*CrownOpenTop<1.0/3.0)
    return vec3(0.6, 0.0, 0.0);
  if(openness*CrownOpenTop<2.0/3.0)
    return vec3(0.0, 0.6, 0.0);
  return vec3(0.0, 0.0, 0.6);
  }

#endif
