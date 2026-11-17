#ifndef CROWN_LIGHT_GLSL
#define CROWN_LIGHT_GLSL

// The light of a tree's crown (reports/engine/2026-11-16-modern-trees/crown-light.md; BK_CROWN of scene.glsl,
// common/graphics/crownlight.h). The vertices of the own GLB carry the openness of the crown where they stand
// (COLOR_0 A: 1 outside, 0 in the depth; 1 without the colour); vertex_process.glsl sends it in the normal's length.

// the openness out of the length of the interpolated normal
float crownOpenness(vec3 normal) {
  return clamp(length(normal)*2.0 - 1.0, 0.0, 1.0);
  }

// OPENGOTHIC_CROWN_LIGHT=mask (VF_CROWN_MASK): the crown by thirds of its openness, the depth red, the outside
// blue; what lets no light through (the bark) magenta. Pure colours: the light changes how bright one is, not which
vec3 crownMask(float openness, float through) {
  if(through<=0.0)
    return vec3(0.6, 0.0, 0.6);
  if(openness<1.0/3.0)
    return vec3(0.6, 0.0, 0.0);
  if(openness<2.0/3.0)
    return vec3(0.0, 0.6, 0.0);
  return vec3(0.0, 0.0, 0.6);
  }

#endif
