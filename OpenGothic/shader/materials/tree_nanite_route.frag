#version 460
#extension GL_GOOGLE_include_directive : require

#define TREE_WIND
#define TREE_FRAME_ONLY
#include "tree_nanite_geometry.glsl"
#include "common.glsl"
#include "tree_nanite_level_mask.glsl"
#include "tree_nanite_impostor.glsl"

layout(set=0, binding=14) uniform sampler2D routeAlbedo;
layout(set=0, binding=15) uniform usampler2D routeNormal;
layout(set=0, binding=16) uniform sampler2D routeDepth;
layout(set=0, binding=17) uniform sampler2D routeShadow;
layout(set=0, binding=18) uniform sampler2D routeOrm;
layout(location=0) out vec4 outColor;

float sunVisibility(vec3 point, float bias) {
  const vec3 p = (frame.shadowProject*vec4(point, 1.0)).xyz;
  const ivec2 size = textureSize(routeShadow, 0);
  const ivec2 center = ivec2(floor((p.xy*0.5 + 0.5)*vec2(size)));
  if(p.z < 0.0 || p.z > 1.0 || any(lessThan(center, ivec2(1))) ||
     any(greaterThanEqual(center, size - 1)))
    return 1.0;
  float lit = 0.0;
  for(int y = -1; y <= 1; ++y)
    for(int x = -1; x <= 1; ++x)
      lit += p.z - bias <= texelFetch(routeShadow, center + ivec2(x, y), 0).r ? 1.0 : 0.0;
  return lit/9.0;
  }

vec3 linearToSrgb(vec3 color) {
  return mix(12.92*color, 1.055*pow(max(color, vec3(0)), vec3(1.0/2.4)) - 0.055,
             greaterThan(color, vec3(0.0031308)));
  }

void main() {
  const ivec2 pixel = ivec2(gl_FragCoord.xy);
  const vec2 ndc = (gl_FragCoord.xy*frame.viewport.zw)*2.0 - 1.0;
  const vec3 ray = frame.forward.xyz + frame.right.xyz*(ndc.x/frame.right.w) - frame.up.xyz*(ndc.y/frame.up.w);
  const vec3 sky = mix(vec3(0.58, 0.67, 0.74), vec3(0.24, 0.43, 0.65), clamp(ray.y*0.8, 0.0, 1.0));
  const float depth = texelFetch(routeDepth, pixel, 0).r;
  const float groundDistance = -frame.eye.y/min(ray.y, -1e-5);
  // Extract camera depth from the current perspective matrix representation.
  const float nearPlane = 50.0;
  const float zScale = 24000.0/(24000.0-nearPlane);
  float treeDistance = zScale*nearPlane/(zScale-depth);
  bool hitTree = depth < 1.0;
  RouteImpostorSample impostor = RouteImpostorSample(vec4(0), vec3(0), vec3(0), 0.0, 0);
  if(frame.impostorSphere.w > 0.0) {
    impostor = routeSampleImpostor(ray);
    if(impostor.albedo.a >= 0.5 && impostor.depth >= nearPlane && impostor.depth < 24000.0) {
      hitTree = true;
      treeDistance = impostor.depth;
      } else {
      impostor.albedo.a = 0.0;
      }
    }
  const vec3 sun = frame.sun.xyz;
  const vec3 light = mix(vec3(0.83, 0.62, 0.40), vec3(0.91, 0.90, 0.84), smoothstep(0.07, 0.6, sun.y));
  vec3 color = sky;
  if(hitTree && (ray.y >= 0.0 || treeDistance < groundDistance)) {
    const bool isImpostor = impostor.albedo.a > 0.0;
    const vec3 albedo = isImpostor ? impostor.albedo.rgb : texelFetch(routeAlbedo, pixel, 0).rgb;
    const vec3 orm = isImpostor ? impostor.orm : texelFetch(routeOrm, pixel, 0).rgb;
    const vec3 n = isImpostor ? impostor.normal : decodeNormal(texelFetch(routeNormal, pixel, 0).r);
    const vec3 point = frame.eye.xyz + ray*treeDistance;
    const float lambert = dot(n, sun);
    const float direct = max(lambert, 0.0) + max(-lambert, 0.0)*orm.b*0.5;
    const float lit = sunVisibility(point, 0.00025);
    const float ambient = mix(0.32, 0.56, 0.55 + 0.45*n.y)*mix(0.62, 1.0, orm.r);
    color = albedo*(ambient*vec3(0.86, 0.95, 1.10) + light*direct*lit);
    color = mix(color, sky, 0.12*smoothstep(2000.0, 16000.0, treeDistance));
    } else if(ray.y < 0.0 && groundDistance < 24000.0) {
    const vec3 point = frame.eye.xyz + ray*groundDistance;
    const float shading = 0.92 + 0.08*cos(point.x/114.0)*cos(point.z/91.0);
    const vec3 albedo = vec3(0.175, 0.166, 0.111)*shading;
    color = albedo*(vec3(0.63, 0.72, 0.83) + light*sun.y*sunVisibility(point, 0.00012));
    color = mix(color, sky, smoothstep(3800.0, 22000.0, groundDistance));
    }
  outColor = vec4(linearToSrgb(color), 1.0);
  }
