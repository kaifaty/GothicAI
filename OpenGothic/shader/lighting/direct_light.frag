#version 460
#extension GL_ARB_separate_shader_objects : enable
#extension GL_GOOGLE_include_directive : enable
#if defined(VIRTUAL_SHADOW)
#extension GL_EXT_control_flow_attributes : enable
#endif

#define LWC 1

#include "lighting/rt/rt_common.glsl"
#include "lighting/tonemapping.glsl"
#include "lighting/shadow_sampling.glsl"
#include "lighting/purkinje_shift.glsl"
#include "scene.glsl"
#include "common.glsl"
#if defined(MATERIAL_MAPS)
#include "lighting/material_brdf.glsl"
#endif
#if defined(VIRTUAL_SHADOW)
#include "virtual_shadow/vsm_sun_shadow.glsl"
#endif

layout(location = 0) out vec4 outColor;

layout(push_constant, std430) uniform UboPush {
  vec3  originLwc;
#if defined(VIRTUAL_SHADOW)
  float vsmMipBias;
#endif
  } push;

layout(binding = 0, std140) uniform UboScene {
  SceneDesc scene;
  };

layout(binding = 1) uniform sampler2D  gbufDiffuse;
layout(binding = 2) uniform usampler2D gbufNormal;
layout(binding = 3) uniform sampler2D  depth;
#if defined(MATERIAL_MAPS)
layout(binding = 12) uniform sampler2D gbufOrm;
#endif

#if defined(SHADOW_MAP)
layout(binding = 4) uniform sampler2D textureSm0;
layout(binding = 5) uniform sampler2D textureSm1;
#endif

#if defined(VIRTUAL_SHADOW)
layout(binding = 4) uniform utexture3D pageTbl;
layout(binding = 6) uniform texture2D  pageData;
#endif

// geometric normal of the visible surface (LWC), from the depth buffer; vec3(0) where it cannot be told
vec3 geomNormal = vec3(0);

bool planetOcclusion(float viewPos, vec3 sunDir) {
  const float y = RPlanet + max(viewPos*0.1, 0);
  if(rayIntersect(vec3(0,y,0), sunDir, RPlanet)>=0)
    return true;
  return false;
  }

vec4 worldPos(vec2 frag, float depth) {
  const vec2 fragCoord = (frag.xy*scene.screenResInv)*2.0 - vec2(1.0);
  const vec4 scr       = vec4(fragCoord.x, fragCoord.y, depth, 1.0);
  return scene.viewProjectInv * scr;
  }

vec4 worldPosLwc(vec2 frag, float depth) {
  const vec2 fragCoord = (frag.xy*scene.screenResInv)*2.0 - vec2(1.0);
  const vec4 scr       = vec4(fragCoord.x, fragCoord.y, depth, 1.0);
  return scene.viewProjectLwcInv * scr;
  }

vec4 worldPos(float depth) {
  return worldPos(gl_FragCoord.xy,depth);
  }

#if defined(SHADOW_MAP)
float calcShadow(vec3 pos, const vec3 normal) {
  return calcShadow(pos, normal, scene, textureSm0, textureSm1);
  }
#else
float calcShadow(vec3 pos, const vec3 normal) { return 1.0; }
#endif

#if defined(VIRTUAL_SHADOW)
vec3 vsmShadowPos(vec4 wpos, vec4 offset) {
  vec4 shPos = scene.viewVirtualShadowLwc * wpos + offset;
  shPos.xyz /= shPos.w;
  return shPos.xyz;
  }

vec3 lwcAt(ivec2 p, float z) {
  const vec4 w = worldPosLwc(vec2(p) + 0.5, z);
  return w.xyz/w.w;
  }

// normal of the visible surface from the depth buffer: on each axis the neighbour nearer in depth, so a crease
// or a silhouette does not tilt it (screen-space derivatives would mix the two surfaces there)
vec3 depthNormal(ivec2 c, float d) {
  const ivec2 sz = textureSize(depth, 0) - ivec2(1);
  const ivec2 r  = min(c + ivec2(1,0), sz), l = max(c - ivec2(1,0), ivec2(0));
  const ivec2 u  = min(c + ivec2(0,1), sz), b = max(c - ivec2(0,1), ivec2(0));
  const float dr = texelFetch(depth, r, 0).r, dl = texelFetch(depth, l, 0).r;
  const float du = texelFetch(depth, u, 0).r, db = texelFetch(depth, b, 0).r;
  const vec3  p0 = lwcAt(c, d);
  const vec3  dx = abs(dr-d)<abs(dl-d) ? lwcAt(r, dr) - p0 : p0 - lwcAt(l, dl);
  const vec3  dy = abs(du-d)<abs(db-d) ? lwcAt(u, du) - p0 : p0 - lwcAt(b, db);
  const vec3  n  = cross(dx, dy);
  return dot(n,n)>1e-12 ? normalize(n) : vec3(0);
  }

// the same over the footprint of a texel of the sun's shadow map on screen: where the camera is so close that a texel
// spans many pixels (0.5 cm at the finest level), a mesh's facets are smaller than the shadow's 4x4 filter, and the
// plane of one facet, carried over its neighbours, dives under them. Both neighbours on an axis when the surface
// goes on smoothly through the pixel, else the nearer in depth as above. n1: the pixel's own normal
vec3 depthNormalWide(ivec2 c, float d, vec3 n1) {
  const vec4 wpos   = worldPosLwc(gl_FragCoord.xy, d);
  const vec3 shPos0 = vsmShadowPos(wpos, vec4(0));
  const vec2 shPos1 = vsmShadowPos(wpos, scene.vsmDdx).xy;
  const vec2 shPos2 = vsmShadowPos(wpos, scene.vsmDdy).xy;
  int mip = vsmSunLod((shPos1 - shPos0.xy)*VSM_CLIPMAP_SIZE, (shPos2 - shPos0.xy)*VSM_CLIPMAP_SIZE, push.vsmMipBias);
  mip = vsmCalcMipIndex(shPos0.xy, mip);
  const mat4  m     = scene.viewVirtualShadowLwc;
  const float texel = (2.0*float(1 << mip)/float(VSM_CLIPMAP_SIZE)) / length(vec3(m[0][0], m[1][0], m[2][0])); // cm
  const vec3  p0    = lwcAt(c, d);
  const ivec2 sz    = textureSize(depth, 0) - ivec2(1);
  // a pixel's step over the surface, by the facet's plane
  const vec3  view  = normalize(p0 - lwcAt(c, 0.5*d));
  const float cosV  = max(abs(dot(view, n1)), 0.2);
  const float px    = length(lwcAt(c + ivec2(1,0), d) - p0);
  const int   k     = clamp(int(texel*cosV/max(px, 1e-6) + 0.5), 1, 24);
  if(k<=1)
    return n1;
  const ivec2 r  = min(c + ivec2(k,0), sz), l = max(c - ivec2(k,0), ivec2(0));
  const ivec2 u  = min(c + ivec2(0,k), sz), b = max(c - ivec2(0,k), ivec2(0));
  const float dr = texelFetch(depth, r, 0).r, dl = texelFetch(depth, l, 0).r;
  const float du = texelFetch(depth, u, 0).r, db = texelFetch(depth, b, 0).r;
  const vec3  pr = lwcAt(r, dr), pl = lwcAt(l, dl), pu = lwcAt(u, du), pb = lwcAt(b, db);
  vec3 dx = abs(dr-d)<abs(dl-d) ? pr - p0 : p0 - pl;
  vec3 dy = abs(du-d)<abs(db-d) ? pu - p0 : p0 - pb;
  if(length(pr + pl - 2.0*p0) < 0.5*length(pr - pl))
    dx = pr - pl;
  if(length(pu + pb - 2.0*p0) < 0.5*length(pu - pb))
    dy = pu - pb;
  vec3 n = cross(dx, dy);
  if(dot(n,n)<=1e-12)
    return n1;
  n = normalize(n);
  // an edge inside the footprint (a thin thing, a silhouette): the pixel's own plane
  return dot(n, n1)>=0.5 ? n : n1;
  }

// the sun's virtual shadow map (virtual_shadow/vsm_sun_shadow.glsl) at this pixel; the normal offset is the
// mark pass's, so the same page is found
// bench `vsm study <bits> [k]` (reports/render-features/2026-11-11-facet-shards): variants of the test on facets
// that stand edge-on or with the back to the sun while the shading normal still takes its light
const uint VS_SUN_SHADING_PLANE = 1u;  // the receiver plane from the shading normal everywhere
const uint VS_SUN_STEEP         = 2u;  // the plane's slope limit at ~89 degrees, not 84
const uint VS_SUN_LIFT          = 4u;  // a facet turned from the sun: the shading normal's plane, the point lifted off it
const uint VS_SUN_TWO_PLANES    = 8u;  // a texel is lit when it clears the facet's plane or the shading normal's
const uint VS_SUN_CLASS         = 16u; // classes as colours instead of the light
const uint VS_SUN_WIDE_NORMAL   = 32u; // the facet normal over a shadow texel's footprint on screen, not over a pixel

float sunShadowVsm(const float depth, const vec3 normal) {
  const uint  study  = scene.vsmStudy;
  vec4        wpos   = worldPosLwc(gl_FragCoord.xy, depth) + vec4(normal*0.002, 0);
  vec3        planeN = (geomNormal!=vec3(0)) ? geomNormal : normal;
  float       minCos = 0.1;
  const float facetL = dot(planeN, scene.sunDir);
  if((study & VS_SUN_SHADING_PLANE)!=0u)
    planeN = normal;
  if((study & VS_SUN_STEEP)!=0u)
    minCos = 0.02;
  if((study & VS_SUN_LIFT)!=0u && facetL<minCos) {
    // cm, in world units (wpos is homogeneous): what a ray to the sun needs to clear a convex mesh of radius ~12 cm
    // from a facet turned by asin(-facetL) past the edge-on, R*sin^2/2
    const float a    = minCos - facetL;
    const float lift = scene.vsmStudyK*min(0.1 + 6.0*a*a, 0.6);
    planeN = normal;
    wpos  += vec4(normal*(lift*wpos.w), 0);
    }
  const vec3 shPos0  = vsmShadowPos(wpos, vec4(0));
  const vec2 shPos1  = vsmShadowPos(wpos, scene.vsmDdx).xy;
  const vec2 shPos2  = vsmShadowPos(wpos, scene.vsmDdy).xy;
  const vec3 planeN2 = (study & VS_SUN_TWO_PLANES)!=0u ? normal : planeN;
  return vsmSunShadow(shPos0, shPos1, shPos2, scene.viewVirtualShadowLwc, planeN, planeN2, push.vsmMipBias, minCos, pageTbl, pageData);
  }
#endif

#if defined(RAY_QUERY)
bool rayTest(vec3 pos, vec3  rayDirection, float tMin, float tMax, out float rayT) {
  uint flags = RayFlagsShadow;
#if !defined(RAY_QUERY_AT)
  flags |= gl_RayFlagsOpaqueEXT;
#endif

  rayQueryEXT rayQuery;
  rayQueryInitializeEXT(rayQuery, topLevelAS, flags, CM_ShadowCaster,
                        pos, tMin, rayDirection, tMax);
  rayQueryProceedShadow(rayQuery);
  if(rayQueryGetIntersectionTypeEXT(rayQuery, true)==gl_RayQueryCommittedIntersectionNoneEXT)
    return false;
  rayT = rayQueryGetIntersectionTEXT(rayQuery, true);
  return true;
  }

bool rayTest(vec3 pos, float tMin, float tMax) {
  float t = 0;
  return rayTest(pos, scene.sunDir, tMin, tMax, t);
  }

float calcRayShadow(vec3 pos, vec3 normal, float depth) {
  vec4 sp = scene.viewShadowLwc[1]*vec4(pos,1);
  if(all(lessThan(abs(sp.xy/sp.w), vec2(0.999))))
    return 1.0;

  const float tMin = 5;
  if(!rayTest(pos + push.originLwc, tMin, 5000*100))
    return 1.0;
  return 0.0;
  }
#endif

float shadowFactor(const float depth, const vec3 normal, bool isFlat) {
  float light  = (isFlat ? 0 : dot(scene.sunDir,normal));
  if(light<=0)
    return 0;

  const float NormalBias = 0.0015;
  const vec4  wpos4 = worldPosLwc(gl_FragCoord.xy, depth) + vec4(normal*NormalBias, 0);
  const vec3  wpos  = wpos4.xyz/wpos4.w;

  if(planetOcclusion(wpos.y, scene.sunDir))
    return 0;

#if defined(VIRTUAL_SHADOW)
  light *= sunShadowVsm(depth, normal);
#else
  light *= calcShadow(wpos, normal);
#endif

#if defined(RAY_QUERY)
    if(light>0.001) {
      light *= calcRayShadow(wpos, normal, depth);
      }
#endif

  return light;
  }

void main(void) {
  const ivec2 fragCoord = ivec2(gl_FragCoord.xy);
  const float d         = texelFetch(depth, fragCoord, 0).r;
  if(d==1.0) {
    outColor = vec4(0);
    return;
    }

  const vec4  diff   = texelFetch(gbufDiffuse, fragCoord, 0);
  const vec3  normal = normalFetch(gbufNormal, fragCoord);
#if defined(VIRTUAL_SHADOW)
  // face the geometric normal like the shading one; a disagreeing one is a thin edge or a deep normal map
  geomNormal = depthNormal(fragCoord, d);
  if((scene.vsmStudy & VS_SUN_WIDE_NORMAL)!=0u && geomNormal!=vec3(0))
    geomNormal = depthNormalWide(fragCoord, d, geomNormal);
  geomNormal *= sign(dot(geomNormal, normal));
  if(dot(geomNormal, normal)<0.3)
    geomNormal = vec3(0);
#endif

  const bool  isFlat = isGBufFlat(diff.a);
  const float light  = shadowFactor(d, normal, isFlat);

#if defined(SHADOW_MAP)
  /*
  {
    // debug code
    const vec4 pos4  = worldPos(d);
    const vec4 shPos = scene.viewShadow[1]*vec4(pos4);
    // vec4 s = shadowSample(textureSm1, shPos.xy/shPos.w);
    // shadow = s.x;
    outColor = vec4(0, (isFlat ? 1 : 0), (isATest ? 1 : 0), 1.0);
    return;
  }
  */
#endif

  // const vec3 linear      = vec3(1);
  const vec3 linear      = textureAlbedo(diff.rgb);

  const vec3 illuminance = scene.sunColor * light;
  vec3 luminance = linear * illuminance * Fd_Lambert;
#if defined(VIRTUAL_SHADOW)
  if((scene.vsmStudy & VS_SUN_CLASS)!=0u) {
    // where the shading normal takes the sun: by the facet's own cosine to it (red turned away, yellow steeper than
    // 84 degrees, blue-grey the rest; white no facet normal) and by the shadow's answer (dark shadowed, bright lit)
    const float nl = dot(scene.sunDir, normal);
    vec3 c = vec3(0);
    if(!isFlat && nl>0) {
      const float fl = dot(geomNormal, scene.sunDir);
      c = geomNormal==vec3(0) ? vec3(1) : (fl<=0 ? vec3(1,0,0) : (fl<0.1 ? vec3(1,1,0) : vec3(0.35,0.45,0.7)));
      c *= (light/nl>0.5) ? 1.0 : 0.25;
      }
    outColor = vec4(c*scene.sunColor*Fd_Lambert*0.5*scene.exposure, 0.0);
    return;
    }
#endif
#if defined(MATERIAL_MAPS)
  const vec4 orm = texelFetch(gbufOrm, fragCoord, 0);
  if(orm.a>0.5 && light>0.0) {
    const vec4 pos = worldPos(d);
    const vec3 view = normalize(scene.camPos - pos.xyz/pos.w);
    luminance = materialBrdf(linear, normal, view, scene.sunDir, orm.rgb) * illuminance;
    }
#endif

  outColor = vec4(luminance * scene.exposure, 0.0);

  // outColor = vec4(vec3(lcolor), diff.a); // debug
  // if(diff.a>0)
  //   outColor.r = 0;
  }
