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
#include "virtual_shadow/vsm_common.glsl"
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
// Sun from the virtual shadow map: the page and mip as vsm_mark_pages.comp picks them (only those pages exist),
// then a 4x4 texel tent (a 3-texel box over bilinear weights, as the cascades' four gathers) in virtual texels,
// each texel through the page table. The receiver's depth at every texel centre comes from its plane, so the
// filter does not shadow the surface by itself on slopes; a texel whose page is missing takes the centre's answer.
int vsmShadowLod(vec2 dx, vec2 dy) {
  float px     = dot(dx, dx);
  float py     = dot(dy, dy);
  float minLod = 0.5 * log2(min(px, py));
  return max(0, int(minLod + push.vsmMipBias));
  }

vec3 vsmShadowPos(vec4 wpos, vec4 offset) {
  vec4 shPos = scene.viewVirtualShadowLwc * wpos + offset;
  shPos.xyz /= shPos.w;
  return shPos.xyz;
  }

// texel of the virtual clipmap 'mip' (0..VSM_CLIPMAP_SIZE): stored depth, or -1 where no page holds it
float vsmSunTexel(ivec2 tx, int mip) {
  if(any(lessThan(tx, ivec2(0))) || any(greaterThanEqual(tx, ivec2(VSM_CLIPMAP_SIZE))))
    return -1;
  const uint pageD = texelFetch(pageTbl, ivec3(tx/VSM_PAGE_SIZE, mip), 0).x;
  if(pageD==0)
    return -1;
  const ivec2 at = unpackVsmPageId(pageD >> 16u)*VSM_PAGE_SIZE + (tx%VSM_PAGE_SIZE);
  return texelFetch(pageData, at, 0).x;
  }

float vsmSunShadow(const float depth, const vec3 normal) {
  const vec2  frag = gl_FragCoord.xy;
  const vec4  wpos = worldPosLwc(frag, depth) + vec4(normal*0.002, 0);

  const vec3 shPos0 = vsmShadowPos(wpos, vec4(0));
  const vec2 shPos1 = vsmShadowPos(wpos, scene.vsmDdx).xy;
  const vec2 shPos2 = vsmShadowPos(wpos, scene.vsmDdy).xy;

  int mip = vsmShadowLod((shPos1 - shPos0.xy)*VSM_CLIPMAP_SIZE, (shPos2 - shPos0.xy)*VSM_CLIPMAP_SIZE);
  mip = vsmCalcMipIndex(shPos0.xy, mip);
  if(mip>=VSM_PAGE_MIPS)
    return 1.0;

  // receiver plane in shadow space: depth change per texel of this mip
  const mat4  m  = scene.viewVirtualShadowLwc;
  const vec3  r0 = vec3(m[0][0], m[1][0], m[2][0]);
  const vec3  r1 = vec3(m[0][1], m[1][1], m[2][1]);
  const vec3  r2 = vec3(m[0][2], m[1][2], m[2][2]);
  const vec3  pn = (geomNormal!=vec3(0)) ? geomNormal : normal;
  const vec3  pm = vec3(dot(r0,pn)/dot(r0,r0), dot(r1,pn)/dot(r1,r1), dot(r2,pn)/dot(r2,r2));
  const float mz = max(pm.z, 0.1*length(pm)); // steeper than ~84 degrees counts as 84
  const vec2  dz = -(pm.xy/mz) * (2.0*float(1 << mip)/float(VSM_CLIPMAP_SIZE));
  const float bias = 2.0/65535.0;

  const vec2  t    = (shPos0.xy/float(1 << mip)*0.5 + 0.5)*float(VSM_CLIPMAP_SIZE) - 0.5;
  const ivec2 base = ivec2(floor(t));
  const vec2  f    = t - vec2(base);
  const float refZ = shPos0.z + bias;

  // centre texel: the answer for the taps without a page
  const ivec2 near = ivec2(floor(t + 0.5));
  const float zc   = vsmSunTexel(near, mip);
  const float lc   = (zc<=0 || zc < refZ + dot(dz, vec2(near) - t)) ? 1.0 : 0.0;

  const vec4 wx = vec4(1.0-f.x, 1.0, 1.0, f.x);
  const vec4 wy = vec4(1.0-f.y, 1.0, 1.0, f.y);

  float sum = 0;
  [[unroll]]
  for(int y=0; y<4; ++y) {
    [[unroll]]
    for(int x=0; x<4; ++x) {
      const ivec2 tx = base + ivec2(x-1, y-1);
      const float z  = vsmSunTexel(tx, mip);
      float lit = lc;
      if(z>=0)
        lit = (z==0 || z < refZ + dot(dz, vec2(tx) - t)) ? 1.0 : 0.0;
      sum += lit*wx[x]*wy[y];
      }
    }
  return sum/9.0;
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
  light *= vsmSunShadow(depth, normal);
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
#if defined(VIRTUAL_SHADOW)
  {
    // derivatives before any early return; across a silhouette the cross product is useless and is dropped below
    const vec4 p4 = worldPosLwc(gl_FragCoord.xy, d);
    const vec3 p  = p4.xyz/p4.w;
    const vec3 gn = cross(dFdx(p), dFdy(p));
    if(dot(gn,gn)>1e-12)
      geomNormal = normalize(gn);
  }
#endif
  if(d==1.0) {
    outColor = vec4(0);
    return;
    }

  const vec4  diff   = texelFetch(gbufDiffuse, fragCoord, 0);
  const vec3  normal = normalFetch(gbufNormal, fragCoord);
#if defined(VIRTUAL_SHADOW)
  // face the geometric normal like the shading one; a disagreeing one is a silhouette or a deep normal map
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
