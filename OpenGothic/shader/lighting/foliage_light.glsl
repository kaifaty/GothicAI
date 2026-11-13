#ifndef FOLIAGE_LIGHT_GLSL
#define FOLIAGE_LIGHT_GLSL

// Living foliage (reports/engine/2026-11-14-living-foliage): the sun through a leaf. A pixel with the foliage bit
// (common.glsl, GBufFoliageBit) keeps in gbufOrm.b the share of the sun the leaf lets through. A card of leaves
// has one normal for both of its sides, so the side is told by the eye: the light comes through where the eye
// looks toward the sun, and fades where the normal already faces the sun (no light counted twice).

// the light that has passed a leaf: yellower and greener than the sun
const vec3 FoliageThroughTint = vec3(1.1, 1.0, 0.5);

// The share of the sun that comes through, on the scale of the cosine of a lit surface. view: from the pixel to
// the eye; sunDir: to the sun
float foliageThrough(float translucency, vec3 normal, vec3 view, vec3 sunDir) {
  const float toSun = clamp(dot(view, -sunDir), 0.0, 1.0);
  const float lobe  = toSun*toSun;
  return translucency * lobe*lobe * clamp(1.0 - 1.5*dot(normal, sunDir), 0.0, 1.0);
  }

#endif
