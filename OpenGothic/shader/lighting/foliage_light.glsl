#ifndef FOLIAGE_LIGHT_GLSL
#define FOLIAGE_LIGHT_GLSL

// Living foliage (reports/engine/2026-11-14-living-foliage): the sun through a leaf. A pixel with the foliage bit
// (common.glsl, GBufFoliageBit) keeps in gbufOrm.b the share of the sun the leaf lets through. A card of leaves
// has one normal for both of its sides, so the side is told by the eye: the light comes through where the eye
// looks toward the sun, and fades where the normal already faces the sun (no light counted twice).

// the light that has passed a leaf: greener than the sun, a leaf keeps most of the red and the blue. Under the low
// warm sun the tuft reads as the same grass lit from behind, a warm yellow-green
const vec3 FoliageThroughTint = vec3(0.75, 1.15, 0.32);

// The share of the sun that comes through, on the scale of the cosine of a lit surface. view: from the pixel to
// the eye; sunDir: to the sun
float foliageThrough(float translucency, vec3 normal, vec3 view, vec3 sunDir) {
  const float toSun = clamp(dot(view, -sunDir), 0.0, 1.0);
  const float lobe  = toSun*toSun;
  return translucency * lobe*lobe * clamp(1.0 - 1.5*dot(normal, sunDir), 0.0, 1.0);
  }

#endif
