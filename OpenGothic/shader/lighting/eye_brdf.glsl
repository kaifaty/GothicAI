#ifndef EYE_BRDF_GLSL
#define EYE_BRDF_GLSL

#include "common.glsl"
#include "lighting/material_brdf.glsl"

// An eyeball lit as a wet surface in the shade of its lids (reports/content/2026-11-hero-head/eye-shader.md, "Способ"
// 1-3). The G-buffer of an eye pixel: gbufOrm.g the roughness, gbufOrm.b the lid's shade (common.glsl, GBufEyeBit).

// The tear film over the eye: n = 1.337, F0 = ((n-1)/(n+1))^2 = 0.021, not the 0.04 of other dielectrics
const float EyeF0 = 0.021;

// The lid's soft shade is lost by the shadow maps (a texel of the VSM is ~0.5 cm, the lid's edge a few mm), so the
// shade takes this much of the direct light of the sun and the lamps at its darkest; the ambient light and the sky's
// reflection it takes in full (ambient_light.frag)
const float EyeLidDirect = 0.6;

float eyeLidDirect(float lid) {
  return 1.0 - EyeLidDirect*(1.0 - lid);
  }

// Lambert of the eye's colour and GGX of the tear film. No fade of the lobe by roughness as other own materials have
// (materialSpecularWeight): the eye's roughness is low anyway, the highlight is the point of it
vec3 eyeBrdf(vec3 albedo, vec3 normal, vec3 view, vec3 light, float roughness) {
  return albedo*Fd_Lambert + materialSpecular(normal, view, light, vec3(EyeF0), roughness);
  }

#endif
