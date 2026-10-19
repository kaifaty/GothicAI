#ifndef MATERIAL_BRDF_GLSL
#define MATERIAL_BRDF_GLSL

#include "common.glsl"

vec3 materialF0(vec3 albedo, float metallic) {
  return mix(vec3(0.04), clamp(albedo, 0.0, 1.0), metallic);
  }

vec3 materialSpecular(vec3 normal, vec3 view, vec3 light, vec3 f0, float roughness) {
  const float noL = max(dot(normal, light), 0.0);
  const float noV = max(dot(normal, view), 0.0);
  if(noL<=0.0 || noV<=0.0)
    return vec3(0.0);

  const vec3 sum = view + light;
  const vec3 h   = sum * inversesqrt(max(dot(sum,sum), 1e-8));
  const float noH = max(dot(normal, h), 0.0);
  const float voH = clamp(dot(view, h), 0.0, 1.0);

  // Perceptual roughness -> GGX slope variance. Keep the delta limit finite.
  const float alpha = max(roughness*roughness, 0.002025);
  const float a2    = alpha*alpha;
  const float denom = noH*noH*(a2 - 1.0) + 1.0;
  const float d     = a2 / max(M_PI*denom*denom, 1e-12);

  // Height-correlated Smith visibility: G2 / (4 * NoL * NoV).
  const float lambdaV = noL * sqrt(noV*noV*(1.0 - a2) + a2);
  const float lambdaL = noV * sqrt(noL*noL*(1.0 - a2) + a2);
  const float visibility = 0.5 / max(lambdaV + lambdaL, 1e-8);
  const float f = 1.0 - voH;
  const float f2 = f*f;
  const float schlick = f2*f2*f;
  const vec3 fresnel = f0 + (vec3(1.0) - f0)*schlick;
  return d * visibility * fresnel;
  }

vec3 materialBrdf(vec3 albedo, vec3 normal, vec3 view, vec3 light, vec3 orm) {
  const float metallic = orm.b;
  const vec3 diffuse = albedo * Fd_Lambert * (1.0 - metallic);
  return diffuse + materialSpecular(normal, view, light, materialF0(albedo, metallic), orm.g);
  }

vec3 materialEnvBrdf(vec3 f0, float roughness, float noV) {
  // Karis' analytical split-sum approximation (no integration LUT).
  const vec4 c0 = vec4(-1.0, -0.0275, -0.572, 0.022);
  const vec4 c1 = vec4( 1.0,  0.0425,  1.04, -0.04);
  const vec4 r  = roughness*c0 + c1;
  const float a004 = min(r.x*r.x, exp2(-9.28*noV))*r.x + r.y;
  const vec2 ab = vec2(-1.04, 1.04)*a004 + r.zw;
  return max(f0*ab.x + ab.y, vec3(0.0));
  }

#endif
