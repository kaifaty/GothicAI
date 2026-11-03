#version 460
#extension GL_ARB_separate_shader_objects : enable
#extension GL_GOOGLE_include_directive    : enable
#if defined(VIRTUAL_SHADOW)
#extension GL_EXT_control_flow_attributes : enable
#endif

#if defined(VIRTUAL_SHADOW)
#include "virtual_shadow/vsm_common.glsl"
#endif

#include "lighting/rt/rt_common.glsl"
#include "lighting/tonemapping.glsl"
#include "scene.glsl"
#include "common.glsl"
#if defined(MATERIAL_MAPS)
#include "lighting/material_brdf.glsl"
#endif

layout(early_fragment_tests) in;

layout(location = 0) out vec4 outColor;

layout(push_constant, std140) uniform Pbo {
  vec3  origin; //lwc
  } push;
layout(binding = 0, std140) uniform UboScene {
  SceneDesc scene;
  };
layout(binding  = 1) uniform texture2D  gbufDiffuse;
layout(binding  = 2) uniform utexture2D gbufNormal;
layout(binding  = 3) uniform texture2D  depth;
#if defined(MATERIAL_MAPS)
layout(binding = 12) uniform texture2D gbufOrm;
#endif

#if defined(VIRTUAL_SHADOW)
layout(binding  = 4, std430) readonly buffer SsboLighting { LightSource lights[]; };
layout(binding  = 5, std430) readonly buffer Omni  { uint pageTblOmni[]; };
layout(binding  = 6)         uniform sampler2D pageData;
#endif

layout(location = 0) in vec4      cenPosition;
layout(location = 1) in vec3      color;
layout(location = 2) in flat uint lightId;
layout(location = 3) in flat float cookie;

// light masks (LightGroup::cookie): a lantern's cage as a latitude-longitude map round the light in its own
// axes, glTF handed (x mirrored against the engine's), u the azimuth atan(z,x), v the angle from straight up
layout(binding = 13) uniform sampler2D cookie1;
layout(binding = 14) uniform sampler2D cookie2;
layout(binding = 15) uniform sampler2D cookie3;
layout(binding = 16) uniform sampler2D cookie4;

float cookieMask(vec3 dir) {
  const float a  = -fract(cookie)*2.0*M_PI; // into the light's own axes
  const vec3  l  = vec3(cos(a)*dir.x - sin(a)*dir.z, dir.y, sin(a)*dir.x + cos(a)*dir.z);
  const vec3  g  = vec3(-l.x, l.y, l.z);
  const vec2  uv = vec2(atan(g.z, g.x)/(2.0*M_PI) + 0.5, acos(clamp(g.y, -1.0, 1.0))/M_PI);
  switch(int(cookie)) {
    case 1: return textureLod(cookie1, uv, 0).r;
    case 2: return textureLod(cookie2, uv, 0).r;
    case 3: return textureLod(cookie3, uv, 0).r;
    case 4: return textureLod(cookie4, uv, 0).r;
    }
  return 1.0;
  }

bool isShadow(vec3 rayOrigin, vec3 direction, float R) {
#if defined(RAY_QUERY)
  {
    const float dirLength    = length(direction);
    const vec3  rayDirection = -direction/dirLength;
    const float rayDistance  = dirLength - cenPosition.w*0.0375; //NOTE: padding of ~3%, in case if light inside wall
    if(rayDistance<=0)
      return false;

    uint flags = RayFlagsShadow;
#if !defined(RAY_QUERY_AT)
    flags |= gl_RayFlagsOpaqueEXT;
#endif

    rayQueryEXT rayQuery;
    rayQueryInitializeEXT(rayQuery, topLevelAS, flags, CM_ShadowCaster,
                          rayOrigin, 0.0, rayDirection, rayDistance);
    rayQueryProceedShadow(rayQuery);
    if(rayQueryGetIntersectionTypeEXT(rayQuery, true) == gl_RayQueryCommittedIntersectionNoneEXT)
      return false;
    return true;
  }
#else
  return false;
#endif
  }

#if defined(VIRTUAL_SHADOW)
// Lamp shadows from the light's cube faces in the virtual shadow map, as a PCSS: 8 blocker-search and 12 filter
// taps, each a bilinear gather. The penumbra is w = S*(d2-d1)/d1 at the receiver: S the flame's size
// (LightSource.sourceSize), d1 the blocker's and d2 the receiver's distance from the light
// (reports/render-features/2026-11-03-lamp-shadows/README.md). Taps are directions round the light, so a tap past
// the face's edge reads the neighbour face and the cube's seams do not show; a neighbour without pages falls back
// to the home face, clamped inside its block. Every gathered texel is compared with the receiver plane along that
// texel's own ray, so wide kernels do not shadow a sloped surface by itself. The tap pattern turns per pixel
// (interleaved gradient noise) and stays still from frame to frame.
const vec2 omniSearch[8] = {
  vec2(0.2500,0.0000), vec2(-0.3193,0.2925), vec2(0.0489,-0.5569), vec2(0.4024,0.5249),
  vec2(-0.7385,-0.1306), vec2(0.6996,-0.4450), vec2(-0.2340,0.8705), vec2(-0.4463,-0.8593)
  };
const vec2 omniFilter[12] = {
  vec2(0.2041,0.0000), vec2(-0.2607,0.2388), vec2(0.0399,-0.4547), vec2(0.3286,0.4286),
  vec2(-0.6030,-0.1067), vec2(0.5712,-0.3634), vec2(-0.1911,0.7107), vec2(-0.3644,-0.7016),
  vec2(0.7906,0.2887), vec2(-0.8224,0.3395), vec2(0.3965,-0.8472), vec2(0.2930,0.9341)
  };
const float OmniZk        = 0.0309278; // vsmApplyProjective: z = k*(1/fz - 1), k = zNear/(zFar - zNear)
const float OmniMaxRadius = 24.0;      // texels: the widest search and filter

vec3 lwcAt(ivec2 p, float z) {
  const vec2 scr = ((vec2(p) + 0.5)/vec2(textureSize(depth,0)))*2.0 - 1.0;
  const vec4 w   = scene.viewProjectLwcInv*vec4(scr, z, 1.0);
  return w.xyz/w.w;
  }

// normal of the visible surface from the depth buffer: on each axis the neighbour nearer in depth, so a crease
// or a silhouette does not tilt it (screen-space derivatives would mix the two surfaces there)
vec3 depthNormal(ivec2 c, float d) {
  const ivec2 sz = textureSize(depth, 0) - ivec2(1);
  const ivec2 r  = min(c + ivec2(1,0), sz), l = max(c - ivec2(1,0), ivec2(0));
  const ivec2 u  = min(c + ivec2(0,1), sz), b = max(c - ivec2(0,1), ivec2(0));
  const float dr = texelFetch(depth, r, 0).x, dl = texelFetch(depth, l, 0).x;
  const float du = texelFetch(depth, u, 0).x, db = texelFetch(depth, b, 0).x;
  const vec3  p0 = lwcAt(c, d);
  const vec3  dx = abs(dr-d)<abs(dl-d) ? lwcAt(r, dr) - p0 : p0 - lwcAt(l, dl);
  const vec3  dy = abs(du-d)<abs(db-d) ? lwcAt(u, du) - p0 : p0 - lwcAt(b, db);
  const vec3  n  = cross(dx, dy);
  return dot(n,n)>1e-12 ? normalize(n) : vec3(0);
  }

float omniGather(const vec3 d, const uint home, const vec3 P, const vec3 n, const float R, const vec2 atlasInv,
                 inout float blockRatio, inout float blockCount) {
  uint face  = vsmLightDirToFace(d);
  uint pageD = pageTblOmni[lightId*6 + face];
  vec3 Pf    = vsmMapDirToFace(P, face);
  if(face!=home && ((pageD & 0xF)==0 || Pf.z<=0)) {
    face  = home;
    pageD = pageTblOmni[lightId*6 + face];
    Pf    = vsmMapDirToFace(P, face);
    }

  const float sz  = float((pageD & 0xF)*VSM_PAGE_SIZE);
  const vec2  org = vec2(unpackVsmPageId(pageD >> 16u)*VSM_PAGE_SIZE);
  const vec3  df  = vsmMapDirToFace(d, face);
  const vec2  t   = clamp(((df.xy/df.z)*0.5 + 0.5)*sz, vec2(1.0), vec2(sz-1.0));
  const vec4  z4  = textureGather(pageData, (org + t)*atlasInv);

  // the four texels' rays in face coordinates; gather order: x (i0,j1), y (i1,j1), z (i1,j0), w (i0,j0)
  const vec2 i0  = floor(t - 0.5);
  const vec2 f   = t - 0.5 - i0;
  const vec2 x01 = (vec2(i0.x + 0.5, i0.x + 1.5)/sz)*2.0 - 1.0;
  const vec2 y01 = (vec2(i0.y + 0.5, i0.y + 1.5)/sz)*2.0 - 1.0;
  const vec4 X   = vec4(x01.x, x01.y, x01.y, x01.x);
  const vec4 Y   = vec4(y01.y, y01.y, y01.x, y01.x);

  // receiver depth along the face's axis on each ray: the plane through P with normal n
  const vec3 nf  = vsmMapDirToFace(n, face);
  const float c  = dot(nf, Pf);
  const vec4 den = nf.x*X + nf.y*Y + vec4(nf.z);
  const vec4 drc = clamp(c/min(den, vec4(-1e-4)), vec4(0.5*Pf.z), vec4(2.0*Pf.z));

  // occluder depth along the axis; 0 is a cleared texel: nothing there
  const vec4 occ  = (OmniZk/(z4 + OmniZk))*R;
  // a third of a texel, plus the 16-bit depth step at that distance (dz = k*R/d^2 per unit of distance)
  const vec4 bias = 0.3 + (0.6/sz)*drc + (2.0/(65535.0*OmniZk*R))*drc*drc;
  const vec4 blk  = vec4(lessThan(occ, drc - bias)) * vec4(greaterThan(z4, vec4(0)));

  blockRatio += dot(blk, occ/drc);
  blockCount += dot(blk, vec4(1));

  const vec4 lit = vec4(1) - blk;
  const vec2 xx  = mix(lit.wz, lit.xy, f.y);
  return mix(xx.x, xx.y, f.x);
  }

// share of the light that reaches the surface at P (relative to the light) with the plane normal n
float omniShadow(const vec3 P, const vec3 n, const float R, const float S) {
  const uint home  = vsmLightDirToFace(P);
  const uint pageD = pageTblOmni[lightId*6 + home];
  if((pageD & 0xF)==0)
    return 1.0; // a face without pages: the light reaches it unshadowed, as with no shadow at all

  const vec2  atlasInv = vec2(1.0)/vec2(textureSize(pageData, 0));
  const float dist  = length(P);
  const vec3  u     = P/dist;
  const vec3  a     = abs(u.y)<0.99 ? vec3(0,1,0) : vec3(1,0,0);
  const vec3  T     = normalize(cross(a, u));
  const vec3  B     = cross(u, T);
  const float texel = 2.0/float((pageD & 0xF)*VSM_PAGE_SIZE); // angle of a texel at the face's centre

  const float ign = fract(52.9829189*fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
  const float rot = ign*2.0*M_PI;
  const mat2  rm  = mat2(cos(rot), sin(rot), -sin(rot), cos(rot));

  // blockers: within the cone that one at a fifth of the distance could shadow, S/2*(1/0.2 - 1)/d
  const float searchR = clamp(2.0*S/dist, 1.5*texel, OmniMaxRadius*texel);
  float ratio = 0, count = 0;
  [[unroll]]
  for(int i=0; i<omniSearch.length(); ++i) {
    const vec2 o = (rm*omniSearch[i])*searchR;
    omniGather(u + o.x*T + o.y*B, home, P, n, R, atlasInv, ratio, count);
    }
  if(count==0)
    return 1.0;
  if(count>=4.0*omniSearch.length())
    return 0.0;

  // the penumbra's half-width as an angle from the light: S/2*(d2/d1 - 1)/d2
  const float d1d2    = max(ratio/count, 0.02);
  const float filterR = clamp(0.5*S/dist*(1.0/d1d2 - 1.0), texel, OmniMaxRadius*texel);
  float lit = 0;
  [[unroll]]
  for(int i=0; i<omniFilter.length(); ++i) {
    const vec2 o = (rm*omniFilter[i])*filterR;
    lit += omniGather(u + o.x*T + o.y*B, home, P, n, R, atlasInv, ratio, count);
    }
  return lit/float(omniFilter.length());
  }
#endif

void main() {
  vec2  scr = (gl_FragCoord.xy/vec2(textureSize(depth,0)))*2.0-1.0;
  float z   = texelFetch(depth, ivec2(gl_FragCoord.xy), 0).x;

  vec4 pos = scene.viewProjectLwcInv*vec4(scr.x,scr.y,z,1.0);
  pos.xyz/=pos.w;
  pos.xyz += push.origin;

  vec3 ldir = (pos.xyz-cenPosition.xyz);

  const float distanceSquare = dot(ldir,ldir);
  const float factor         = distanceSquare / (cenPosition.w*cenPosition.w);
  const float smoothFactor   = max(1.0 - factor * factor, 0.0);

  if(factor>1.0)
    discard;

  const vec3 normal = normalFetch(gbufNormal, ivec2(gl_FragCoord.xy));

  float lambert = max(0.0,-dot(normalize(ldir),normal));
  float light   = (lambert/max(factor, 0.005)) * (smoothFactor*smoothFactor);
  if(cookie>0.0)
    light *= cookieMask(normalize(ldir));
  if(light<=0.0)
    discard;

#if defined(MATERIAL_MAPS)
  const vec3 surfacePos = pos.xyz;
#endif
#if defined(VIRTUAL_SHADOW)
  {
    // the plane for the shadow's depth test: the geometric one, unless it disagrees much (a thin edge, a normal map)
    vec3 geomNormal = depthNormal(ivec2(gl_FragCoord.xy), z);
    geomNormal *= sign(dot(geomNormal, normal));
    const vec3 planeN = dot(geomNormal, normal)>=0.3 ? geomNormal : normal;
    light *= omniShadow(pos.xyz - cenPosition.xyz, planeN, cenPosition.w, lights[lightId].sourceSize);
    if(light<=0.0)
      discard;
  }
#else
  const float NormalBias = 2.0;
  pos.xyz = pos.xyz + NormalBias*normal; //bias
  if(isShadow(pos.xyz, pos.xyz-cenPosition.xyz, cenPosition.w))
    discard;
#endif

  //outColor     = vec4(0.5,0.5,0.5,1);
  //outColor     = vec4(light,light,light,0.0);
  //outColor     = vec4(d.rgb*color*vec3(light),0.0);

  const vec3 d      = texelFetch(gbufDiffuse, ivec2(gl_FragCoord.xy), 0).xyz;
  const vec3 linear = textureAlbedo(d.rgb);

  vec3 ret = linear * (color * light) * max(1.0, scene.exposure) * Fd_Lambert * 0.25;
#if defined(MATERIAL_MAPS)
  const vec4 orm = texelFetch(gbufOrm, ivec2(gl_FragCoord.xy), 0);
  if(orm.a>0.5) {
    const vec3 view = normalize(scene.camPos - surfacePos);
    ret = materialBrdf(linear, normal, view, -normalize(ldir), orm.rgb) *
          (color * light) * max(1.0, scene.exposure) * 0.25;
    }
#endif

  outColor = vec4(ret, 0.0);
  }
