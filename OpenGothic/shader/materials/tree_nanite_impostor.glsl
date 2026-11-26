#ifndef TREE_NANITE_IMPOSTOR_GLSL
#define TREE_NANITE_IMPOSTOR_GLSL

// The content pilot samples the stock 12 × 12 octahedral atlas in its existing paint pass.
// The three captures, tangent frames and parallax match materials/impostor.frag.
layout(set=0, binding=19) uniform sampler2D routeImpostorAlbedo;
layout(set=0, binding=20) uniform sampler2D routeImpostorSurface;
layout(set=0, binding=21) uniform sampler2D routeImpostorOrm;

const vec2 ImpostorGrid = vec2(11);
const float ImpostorInner = 96.0;

vec2 impostorSign(vec2 value) {
  return mix(vec2(-1), vec2(1), greaterThanEqual(value, vec2(0)));
  }

vec2 routeOctEncode(vec3 point) {
  point /= dot(abs(point), vec3(1));
  return point.y < 0.0 ? (1.0 - abs(point.zx))*impostorSign(point.xz) : point.xz;
  }

vec3 routeOctDecode(vec2 point) {
  vec3 v = vec3(point.x, 1.0 - dot(abs(point), vec2(1)), point.y);
  if(v.y < 0.0)
    v.xz = (1.0 - abs(v.zx))*impostorSign(v.xz);
  return normalize(v);
  }

mat3 routeImpostorBasis(vec3 direction) {
  const vec3 side = cross(vec3(0, 1, 0), direction);
  const vec3 right = dot(side, side) > 1e-12 ? normalize(side) : vec3(1, 0, 0);
  return mat3(right, cross(direction, right), direction);
  }

vec2 routeFrameAxes(mat3 basis, vec3 point) {
  return vec2(dot(basis[0], point), -dot(basis[1], point));
  }

vec2 routeAtlasUv(ivec2 cell, vec2 uv) {
  return (vec2(cell)*128.0 + 16.0 + uv*ImpostorInner)/1536.0;
  }

struct RouteImpostorSample {
  vec4 albedo;
  vec3 normal, orm;
  float depth;
  int materialClass;
  };

RouteImpostorSample routeSampleCapture(ivec2 cell, vec3 ray, vec3 center, vec3 dx, vec3 dy) {
  RouteImpostorSample result = RouteImpostorSample(vec4(0), vec3(0), vec3(0), 0.0, 0);
  const vec3 direction = routeOctDecode(vec2(cell)*(2.0/ImpostorGrid) - 1.0);
  const float rayDot = dot(ray, direction);
  if(rayDot >= -0.1)
    return result;
  const mat3 basis = routeImpostorBasis(direction);
  const vec2 uvDx = routeFrameAxes(basis, dx)*(0.5*ImpostorInner);
  const vec2 uvDy = routeFrameAxes(basis, dy)*(0.5*ImpostorInner);
  const float lod = clamp(0.5*log2(max(1.0, max(dot(uvDx, uvDx), dot(uvDy, uvDy)))), 0.0, 4.0);
  const float planeDot = dot(center, direction);
  const vec3 plane = center - ray*(planeDot/rayDot);
  const float pad = (16.0 - 0.5*exp2(ceil(lod)))/ImpostorInner;
  const vec2 initial = clamp(routeFrameAxes(basis, plane)*0.5 + 0.5, vec2(-pad), vec2(1.0 + pad));
  const float depth = textureLod(routeImpostorSurface, routeAtlasUv(cell, initial), lod).a*2.0 - 1.0;
  const vec2 corrected = routeFrameAxes(basis, center + ray*((depth-planeDot)/rayDot))*0.5 + 0.5;
  if(any(lessThan(corrected, vec2(0))) || any(greaterThan(corrected, vec2(1))))
    return result;
  const vec2 uv = routeAtlasUv(cell, corrected);
  result.albedo = textureLod(routeImpostorAlbedo, uv, lod);
  if(result.albedo.a <= 0.0)
    return result;
  const vec4 surface = textureLod(routeImpostorSurface, uv, lod);
  result.normal = routeOctDecode(surface.rg*2.0 - 1.0);
  result.depth = (surface.a*2.0 - 1.0 - planeDot)/rayDot;
  result.orm = textureLod(routeImpostorOrm, uv, lod).rgb;
  const int mip = int(floor(lod+0.5));
  const ivec2 texel = ivec2(uv*vec2(textureSize(routeImpostorOrm, mip)));
  result.materialClass = int(round(texelFetch(routeImpostorOrm, texel, mip).a*4.0));
  return result;
  }

RouteImpostorSample routeSampleImpostor(vec3 ray) {
  const vec4 sphere = frame.impostorSphere;
  const vec3 eye = (frame.eye.xyz - sphere.xyz)/sphere.w;
  const vec3 direction = normalize(eye);
  const vec3 rayUnit = normalize(ray);
  const float rayDot = dot(ray, direction);
  const float centerDepth = -dot(eye, direction)/min(rayDot, -1e-5);
  const vec3 center = eye + ray*centerDepth;
  const vec3 dx = dFdx(center), dy = dFdy(center);
  RouteImpostorSample result = RouteImpostorSample(vec4(0), vec3(0), vec3(0), 0.0, 0);
  if(rayDot >= 0.0 || !treeLevelCovers(5u, gl_FragCoord.xy) ||
     any(greaterThan(abs(routeFrameAxes(routeImpostorBasis(direction), center)), vec2(1))))
    return result;

  const vec2 grid = (routeOctEncode(direction)*0.5 + 0.5)*ImpostorGrid;
  const ivec2 cell = clamp(ivec2(grid), ivec2(0), ivec2(ImpostorGrid)-1);
  const vec2 v = grid - vec2(cell);
  const ivec2 middle = v.x >= v.y ? ivec2(1, 0) : ivec2(0, 1);
  const vec3 angular = vec3(1.0-max(v.x, v.y), abs(v.x-v.y), min(v.x, v.y));
  const RouteImpostorSample a = routeSampleCapture(cell, rayUnit, center, dx, dy);
  const RouteImpostorSample b = routeSampleCapture(cell+middle, rayUnit, center, dx, dy);
  const RouteImpostorSample c = routeSampleCapture(cell+1, rayUnit, center, dx, dy);
  vec3 weights = angular*vec3(a.albedo.a, b.albedo.a, c.albedo.a);
  const float coverage = dot(weights, vec3(1));
  if(coverage < 0.5)
    return result;
  weights /= coverage;
  const RouteImpostorSample dominant = weights.x >= max(weights.y, weights.z) ? a : (weights.y >= weights.z ? b : c);
  const vec3 n = a.normal*weights.x + b.normal*weights.y + c.normal*weights.z;
  const vec3 materialWeights = weights*vec3(a.materialClass == dominant.materialClass,
                                            b.materialClass == dominant.materialClass,
                                            c.materialClass == dominant.materialClass);
  result.albedo = vec4(mat3(a.albedo.rgb, b.albedo.rgb, c.albedo.rgb)*weights, coverage);
  result.normal = normalize(dot(n,n) > 1e-8 ? n : dominant.normal);
  result.orm = (mat3(a.orm, b.orm, c.orm)*materialWeights)/dot(materialWeights, vec3(1));
  result.depth = (centerDepth + dot(weights, vec3(a.depth, b.depth, c.depth))/length(ray))*sphere.w;
  return result;
  }

#endif
