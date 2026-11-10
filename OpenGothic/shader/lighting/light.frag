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
#if defined(VSM_LAMP_PROBE)
  uint  probeMode;  // OPENGOTHIC_VSM_LAMP: 0 the PCSS, 1 'hard' (one gather), 2 'flat' (no receiver plane), 3 truth
  uint  probeStamp; // not 0: count this frame (OPENGOTHIC_VSM_STATS), the stamp marks a pixel already lit
  float probeNarrow; // OPENGOTHIC_VSM_LAMP_NARROW: OmniNarrow instead, texels; 0 always twelve taps; <0 the default
  // under TAA, OPENGOTHIC_VSM_LAMP_TAPS: OmniTaaSearch1, OmniTaaSearch2, OmniTaaNarrow, OmniTaaWide instead
  uint  probeSearch1, probeSearch2, probeNarrowTaps, probeWideTaps;
  uint  probeFlags; // 1 OPENGOTHIC_VSM_LAMP_FLOOR, 2 OPENGOTHIC_VSM_LAMP_CENTRE
  // OPENGOTHIC_VSM_LAMP_EXP (step-2d-why.md): 1 still, the pattern of main under TAA too; 2 inner, the first search
  // takes the inner taps; 4 litall, 'all lit' only after the whole search; 8 radial, the taps' radii move per frame;
  // 16 expect, the mean of 16 frames' patterns in one frame; 32 paths, the exit path as a colour instead of light
  uint  probeExp;
#endif
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
#if defined(VSM_LAMP_PROBE)
// counters of the lamp filter for one frame (Renderer::logVsmStats, LampProbe in renderer.cpp), then a stamp per
// pixel to count the pixels lit by any lamp
layout(binding  = 7, std430) buffer LampProbe { uint probe[16]; uint probePixel[]; };
uint probeTaps = 0, probeSlow = 0, probeNeighbour = 0, probeEarly = 0, probeBin = 0;
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
// (interleaved gradient noise) and stays still from frame to frame without TAA. Under TAA the noise moves by
// 5.588238 pixels a frame and fewer taps are taken; TAA gathers the frames (omniShadowTaa).
// The hard variant (-DVSM_LAMP_HARD, light_vsm_hard; [INTERNAL] vsmLampSoft=0, the default on Metal) takes one gather
// of the home face for every pixel instead, as 'faint' does.
const vec2 omniSearch[8] = {
  vec2(0.2500,0.0000), vec2(-0.3193,0.2925), vec2(0.0489,-0.5569), vec2(0.4024,0.5249),
  vec2(-0.7385,-0.1306), vec2(0.6996,-0.4450), vec2(-0.2340,0.8705), vec2(-0.4463,-0.8593)
  };
const vec2 omniFilter[12] = {
  vec2(0.2041,0.0000), vec2(-0.2607,0.2388), vec2(0.0399,-0.4547), vec2(0.3286,0.4286),
  vec2(-0.6030,-0.1067), vec2(0.5712,-0.3634), vec2(-0.1911,0.7107), vec2(-0.3644,-0.7016),
  vec2(0.7906,0.2887), vec2(-0.8224,0.3395), vec2(0.3965,-0.8472), vec2(0.2930,0.9341)
  };
// a narrow penumbra: four taps on a square at the twelve's mean radius (their second moment, r^2/2)
const vec2 omniNarrow[4] = { vec2(0.7071,0.0), vec2(0.0,0.7071), vec2(-0.7071,0.0), vec2(0.0,-0.7071) };
const float OmniZk        = 0.0309278; // vsmApplyProjective: z = k*(1/fz - 1), k = zNear/(zFar - zNear)
const float OmniMaxRadius = 24.0;      // texels: the widest search and filter
// texels: a penumbra up to this half-width is filtered by four taps, not twelve; the gathers' bilinear weights
// smooth it, and the two filters differ less than TAA's noise (reports/render-features/2026-11-04-lamp-pcss/step-2c.md)
const float OmniNarrow    = 1.5;
// under TAA (reports/render-features/2026-11-04-lamp-pcss/step-2d.md): taps of the first and the second blocker
// search, of a narrow and of a wide penumbra, on Vogel discs turned by the frame's noise
const int   OmniTaaSearch1 = 2;
const int   OmniTaaSearch2 = 2;
const int   OmniTaaNarrow  = 4;
const int   OmniTaaWide    = 6;
const bool  OmniTaaFloor   = false; // the search stops after its first tap when that puts an edge at the floor
const bool  OmniTaaCentre  = false; // the narrow penumbra's threshold in texels of the face's centre

// a cube face's block in the atlas and the receiver in that face's axes
struct OmniFace {
  vec2  org;  // the block's first texel in the atlas
  float sz;   // its size, texels
  vec3  Pf;   // the receiver, in face axes (z along the face's axis)
  vec3  nf;   // the receiver plane's normal, in face axes
  uint  face;
  };

OmniFace omniFace(const uint face, const uint pageD, const vec3 P, const vec3 n) {
  OmniFace f;
  f.org  = vec2(unpackVsmPageId(pageD >> 16u)*VSM_PAGE_SIZE);
  f.sz   = float((pageD & 0xF)*VSM_PAGE_SIZE);
  f.Pf   = vsmMapDirToFace(P, face);
  f.nf   = vsmMapDirToFace(n, face);
  f.face = face;
  return f;
  }

float omniGatherFace(const vec3 d, const OmniFace f, const float R, const vec2 atlasInv,
                     inout float blockRatio, inout float blockCount) {
  const vec3 df = vsmMapDirToFace(d, f.face);
  const vec2 t  = clamp(((df.xy/df.z)*0.5 + 0.5)*f.sz, vec2(1.0), vec2(f.sz-1.0));
  const vec4 z4 = textureGather(pageData, (f.org + t)*atlasInv);

  // the four texels' rays in face coordinates; gather order: x (i0,j1), y (i1,j1), z (i1,j0), w (i0,j0)
  const vec2 i0  = floor(t - 0.5);
  const vec2 fr  = t - 0.5 - i0;
  const vec2 x01 = (vec2(i0.x + 0.5, i0.x + 1.5)/f.sz)*2.0 - 1.0;
  const vec2 y01 = (vec2(i0.y + 0.5, i0.y + 1.5)/f.sz)*2.0 - 1.0;
  const vec4 X   = vec4(x01.x, x01.y, x01.y, x01.x);
  const vec4 Y   = vec4(y01.y, y01.y, y01.x, y01.x);

  // receiver depth along the face's axis on each ray: the plane through P with normal n
  const float c   = dot(f.nf, f.Pf);
  const vec4  den = f.nf.x*X + f.nf.y*Y + vec4(f.nf.z);
#if defined(VSM_LAMP_PROBE)
  // 'flat': every texel against the receiver's own depth, without the plane along the rays
  const vec4  drc = push.probeMode==2 ? vec4(f.Pf.z)
                                      : clamp(c/min(den, vec4(-1e-4)), vec4(0.5*f.Pf.z), vec4(2.0*f.Pf.z));
  ++probeTaps;
#else
  const vec4  drc = clamp(c/min(den, vec4(-1e-4)), vec4(0.5*f.Pf.z), vec4(2.0*f.Pf.z));
#endif

  // occluder depth along the axis; 0 is a cleared texel: nothing there
  const vec4 occ  = (OmniZk/(z4 + OmniZk))*R;
  // a third of a texel, plus the 16-bit depth step at that distance (dz = k*R/d^2 per unit of distance)
  const vec4 bias = 0.3 + (0.6/f.sz)*drc + (2.0/(65535.0*OmniZk*R))*drc*drc;
  // the lamp's own fixture (a brazier's basket, a lantern's frame round the flame) is not in the map: the faces are
  // drawn without it (main.vert, processVertexVsm), so whatever stands behind it still shadows
  const vec4 blk  = vec4(lessThan(occ, drc - bias)) * vec4(greaterThan(z4, vec4(0)));

  blockRatio += dot(blk, occ/drc);
  blockCount += dot(blk, vec4(1));

  const vec4 lit = vec4(1) - blk;
  const vec2 xx  = mix(lit.wz, lit.xy, fr.y);
  return mix(xx.x, xx.y, fr.x);
  }

// a tap: on the home face, or on the neighbour its direction falls on when that one has pages
float omniGather(const vec3 d, const OmniFace home, const bool homeOnly, const vec3 P, const vec3 n, const float R,
                 const vec2 atlasInv, inout float blockRatio, inout float blockCount) {
  if(!homeOnly) {
#if defined(VSM_LAMP_PROBE)
    ++probeSlow;
#endif
    const uint face = vsmLightDirToFace(d);
    if(face!=home.face) {
      const uint pageD = pageTblOmni[lightId*6 + face];
#if defined(VSM_LAMP_PROBE)
      if((pageD & 0xF)!=0 && vsmMapDirToFace(P, face).z>0)
        ++probeNeighbour;
#endif
      if((pageD & 0xF)!=0 && vsmMapDirToFace(P, face).z>0)
        return omniGatherFace(d, omniFace(face, pageD, P, n), R, atlasInv, blockRatio, blockCount);
      }
    }
  return omniGatherFace(d, home, R, atlasInv, blockRatio, blockCount);
  }

// tap i of n on a Vogel disc of radius 1: the same second moment (r^2/2) as the twelve, spread evenly for any n
vec2 omniVogel(const int i, const int n) {
  const float a = float(i)*2.39996323;
  return sqrt((float(i) + 0.5)/float(n))*vec2(cos(a), sin(a));
  }

// the same with the radius's place inside its ring: xi 0.5 is omniVogel, a noise in [0,1) spreads it over the ring
vec2 omniVogelR(const int i, const int n, const float xi) {
  const float a = float(i)*2.39996323;
  return sqrt((float(i) + xi)/float(n))*vec2(cos(a), sin(a));
  }

// omniShadow under TAA: the same search and penumbra with fewer taps, turned by a noise that moves every frame, so
// that TAA's history averages a different set of taps in each pixel
float omniShadowTaa(const vec3 P, const vec3 n, const float R, const float S, const OmniFace hf, const float dist,
                    const vec3 u, const vec3 T, const vec3 B, const float texel, const float fcMax, const float fcEdge,
                    const vec2 atlasInv, const uint frame) {
#if defined(VSM_LAMP_PROBE)
  const bool inner  = (push.probeExp & 2u)!=0u;
  const bool litAll = (push.probeExp & 4u)!=0u;
  const bool radial = (push.probeExp & 8u)!=0u;
  const int  nS1    = int(push.probeSearch1);
  const int  nS2    = int(push.probeSearch2);
  const int  nN     = int(push.probeNarrowTaps);
  const int  nW     = int(push.probeWideTaps);
  const bool floorX = (push.probeFlags & 1u)!=0u;
  const bool centre = (push.probeFlags & 2u)!=0u;
#else
  const bool inner  = false;
  const bool litAll = false;
  const bool radial = false;
  const int  nS1    = OmniTaaSearch1;
  const int  nS2    = OmniTaaSearch2;
  const int  nN     = OmniTaaNarrow;
  const int  nW     = OmniTaaWide;
  const bool floorX = OmniTaaFloor;
  const bool centre = OmniTaaCentre;
#endif
  const vec2  px  = gl_FragCoord.xy + 5.588238*float(frame & 63u);
  const float ign = fract(52.9829189*fract(dot(px, vec2(0.06711056, 0.00583715))));
  // the study's radial shift: a second noise, its own every frame
  const float xi  = radial ? fract(52.9829189*fract(dot(px, vec2(0.00583715, 0.06711056)))) : 0.5;
  const float rot = ign*2.0*M_PI;
  const mat2  rm  = mat2(cos(rot), sin(rot), -sin(rot), cos(rot));
  float ratio = 0, count = 0;

  // the search: one Vogel disc of nS1 + nS2 taps, its outer nS1 first; where they agree the answer is clear
  const int   nS         = nS1 + nS2;
  const float searchR    = clamp(2.0*S/dist, 1.5*texel, OmniMaxRadius*texel);
  const bool  searchHome = fcMax + 2.5*searchR < fcEdge;
  bool atFloor = false;
  for(int i=0; i<nS1; ++i) {
    const vec2 o = (rm*omniVogelR(inner ? i : nS - 1 - i, nS, xi))*searchR;
    omniGather(u + o.x*T + o.y*B, hf, searchHome, P, n, R, atlasInv, ratio, count);
    // an edge (some of the four texels blocked) whose blockers put the penumbra at the floor: no more search
    if(i==0 && floorX && count>0 && count<4.0 && 0.5*S/dist*(1.0/max(ratio/count, 0.02) - 1.0) <= texel) {
      atFloor = true;
      break;
      }
    }
#if defined(VSM_LAMP_PROBE)
  probeEarly = count==0 ? 1u : (count>=4.0*float(nS1) ? 2u : 0u);
  probeBin   = searchR<=1.5*texel ? 16u : 0u;
#endif
  if(!atFloor) {
    if(count==0 && !litAll)
      return 1.0;
    if(count>=4.0*float(nS1))
      return 0.0;
    for(int i=nS1; i<nS; ++i) {
      const vec2 o = (rm*omniVogelR(inner ? i : nS - 1 - i, nS, xi))*searchR;
      omniGather(u + o.x*T + o.y*B, hf, searchHome, P, n, R, atlasInv, ratio, count);
      }
    if(count==0)
      return 1.0;
    }

  const float d1d2    = max(ratio/count, 0.02);
  const float filterR = clamp(0.5*S/dist*(1.0/d1d2 - 1.0), texel, OmniMaxRadius*texel);
#if defined(VSM_LAMP_PROBE)
  {
    const float w = filterR/texel;
    probeBin |= w<=1.001 ? 0u : (w<=1.5 ? 1u : (w<=2.0 ? 2u : (w<=3.0 ? 3u : (w<=6.0 ? 4u : 5u))));
    probeBin |= 8u;
  }
  const float narrowT = push.probeNarrow>=0.0 ? push.probeNarrow : OmniNarrow;
#else
  const float narrowT = OmniNarrow;
#endif
  const bool  filterHome = fcMax + 2.5*filterR < fcEdge;
  const bool  narrow     = filterR <= narrowT*texel/(centre ? 1.0 : 1.0 + fcMax*fcMax);
#if defined(VSM_LAMP_PROBE)
  if(narrow)
    probeBin |= 32u;
#endif
  // two loops, so that the game's shader has each disc as constants
  float lit = 0;
  if(narrow) {
    for(int i=0; i<nN; ++i) {
      const vec2 o = (rm*omniVogelR(i, nN, xi))*filterR;
      lit += omniGather(u + o.x*T + o.y*B, hf, filterHome, P, n, R, atlasInv, ratio, count);
      }
    return lit/float(nN);
    }
  for(int i=0; i<nW; ++i) {
    const vec2 o = (rm*omniVogelR(i, nW, xi))*filterR;
    lit += omniGather(u + o.x*T + o.y*B, hf, filterHome, P, n, R, atlasInv, ratio, count);
    }
  return lit/float(nW);
  }

// share of the light that reaches the surface at P (relative to the light) with the plane normal n;
// 'faint': the light is too weak here to show a soft edge, one gather decides
float omniShadow(const vec3 P, const vec3 n, const float R, const float S, const bool faint) {
  const uint home  = vsmLightDirToFace(P);
  const uint pageD = pageTblOmni[lightId*6 + home];
  if((pageD & 0xF)==0)
    return 1.0; // a face without pages (the pool overflowed): the light reaches it unshadowed

  const vec2     atlasInv = vec2(1.0)/vec2(textureSize(pageData, 0));
  const OmniFace hf       = omniFace(home, pageD, P, n);
  const float    dist     = length(P);
  const vec3     u        = P/dist;
  float ratio = 0, count = 0;
#if defined(VSM_LAMP_HARD)
  // hard lamp shadows ([INTERNAL] vsmLampSoft=0, the default on Metal): every pixel as 'faint', one gather of the
  // home face with its bilinear weight, no penumbra from the flame's size
  // (reports/render-features/2026-11-05-vsm-mac-hard/README.md)
  return omniGatherFace(u, hf, R, atlasInv, ratio, count);
#else
#if defined(VSM_LAMP_PROBE)
  if(faint || push.probeMode==1)
    return omniGatherFace(u, hf, R, atlasInv, ratio, count);
#else
  if(faint)
    return omniGatherFace(u, hf, R, atlasInv, ratio, count);
#endif

  const vec3  a     = abs(u.y)<0.99 ? vec3(0,1,0) : vec3(1,0,0);
  const vec3  T     = normalize(cross(a, u));
  const vec3  B     = cross(u, T);
  const float texel = 2.0/hf.sz; // angle of a texel at the face's centre
  // the whole kernel on the home face, so no tap needs the neighbour: by the search's and the filter's own radius.
  // A turn by a small angle moves a face coordinate x by up to sqrt(1+x^2)*sqrt(1+x^2+y^2) times it, 2.45 at a corner
  const vec2  fc     = abs(hf.Pf.xy/hf.Pf.z);
  const float fcMax  = max(fc.x, fc.y);
  const float fcEdge = 1.0 - 2.0/hf.sz;

#if defined(VSM_LAMP_PROBE)
  if(push.probeMode==3u) {
    // truth: no early exits, 32 search and 64 filter taps, the still pattern
    const float ig0 = fract(52.9829189*fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
    const float r0  = ig0*2.0*M_PI;
    const mat2  rm0 = mat2(cos(r0), sin(r0), -sin(r0), cos(r0));
    const float sR  = clamp(2.0*S/dist, 1.5*texel, OmniMaxRadius*texel);
    const bool  sH  = fcMax + 2.5*sR < fcEdge;
    for(int i=0; i<32; ++i) {
      const vec2 o = (rm0*omniVogel(i, 32))*sR;
      omniGather(u + o.x*T + o.y*B, hf, sH, P, n, R, atlasInv, ratio, count);
      }
    if(count==0)
      return 1.0;
    const float fR = clamp(0.5*S/dist*(1.0/max(ratio/count, 0.02) - 1.0), texel, OmniMaxRadius*texel);
    const bool  fH = fcMax + 2.5*fR < fcEdge;
    float lt = 0;
    for(int i=0; i<64; ++i) {
      const vec2 o = (rm0*omniVogel(i, 64))*fR;
      lt += omniGather(u + o.x*T + o.y*B, hf, fH, P, n, R, atlasInv, ratio, count);
      }
    return lt/64.0;
    }
  if(scene.taaActive!=0u && (push.probeExp & 1u)==0u) {
    if((push.probeExp & 16u)!=0u) {
      // the expectation: 16 frames' patterns at once
      float e = 0;
      for(uint k=0u; k<16u; ++k)
        e += omniShadowTaa(P, n, R, S, hf, dist, u, T, B, texel, fcMax, fcEdge, atlasInv, scene.taaFrame + k);
      return e/16.0;
      }
    return omniShadowTaa(P, n, R, S, hf, dist, u, T, B, texel, fcMax, fcEdge, atlasInv, scene.taaFrame);
    }
#else
  if(scene.taaActive!=0u)
    return omniShadowTaa(P, n, R, S, hf, dist, u, T, B, texel, fcMax, fcEdge, atlasInv, scene.taaFrame);
#endif

  const float ign = fract(52.9829189*fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
  const float rot = ign*2.0*M_PI;
  const mat2  rm  = mat2(cos(rot), sin(rot), -sin(rot), cos(rot));

  // blockers: within the cone that one at a fifth of the distance could shadow, S/2*(1/0.2 - 1)/d.
  // Four spread taps first: where they agree (all lit or all blocked, 16 texels) the answer is clear
  const float searchR    = clamp(2.0*S/dist, 1.5*texel, OmniMaxRadius*texel);
  const bool  searchHome = fcMax + 2.5*searchR < fcEdge;
  [[unroll]]
  for(int i=1; i<omniSearch.length(); i+=2) {
    const vec2 o = (rm*omniSearch[i])*searchR;
    omniGather(u + o.x*T + o.y*B, hf, searchHome, P, n, R, atlasInv, ratio, count);
    }
#if defined(VSM_LAMP_PROBE)
  probeEarly = count==0 ? 1u : (count>=2.0*omniSearch.length() ? 2u : 0u);
  probeBin   = searchR<=1.5*texel ? 16u : 0u;
#endif
  if(count==0)
    return 1.0;
  if(count>=2.0*omniSearch.length())
    return 0.0;
  [[unroll]]
  for(int i=0; i<omniSearch.length(); i+=2) {
    const vec2 o = (rm*omniSearch[i])*searchR;
    omniGather(u + o.x*T + o.y*B, hf, searchHome, P, n, R, atlasInv, ratio, count);
    }

  // the penumbra's half-width as an angle from the light: S/2*(d2/d1 - 1)/d2
  const float d1d2    = max(ratio/count, 0.02);
  const float filterR = clamp(0.5*S/dist*(1.0/d1d2 - 1.0), texel, OmniMaxRadius*texel);
#if defined(VSM_LAMP_PROBE)
  {
    // the penumbra's half-width in texels: at the floor (1), up to 1.5, 2, 3, 6, wider
    const float w = filterR/texel;
    probeBin |= w<=1.001 ? 0u : (w<=1.5 ? 1u : (w<=2.0 ? 2u : (w<=3.0 ? 3u : (w<=6.0 ? 4u : 5u))));
    probeBin |= 8u; // reached the filter
  }
#endif
  const bool filterHome = fcMax + 2.5*filterR < fcEdge;

  // a narrow penumbra, in texels where they are smallest (a face's texel spans 1/(1+x^2) of the centre's angle)
#if defined(VSM_LAMP_PROBE)
  const float narrowT = push.probeNarrow>=0.0 ? push.probeNarrow : OmniNarrow;
#else
  const float narrowT = OmniNarrow;
#endif
  float lit = 0;
  if(filterR <= narrowT*texel/(1.0 + fcMax*fcMax)) {
#if defined(VSM_LAMP_PROBE)
    probeBin |= 32u;
#endif
    [[unroll]]
    for(int i=0; i<omniNarrow.length(); ++i) {
      const vec2 o = (rm*omniNarrow[i])*filterR;
      lit += omniGather(u + o.x*T + o.y*B, hf, filterHome, P, n, R, atlasInv, ratio, count);
      }
    return lit/float(omniNarrow.length());
    }
  [[unroll]]
  for(int i=0; i<omniFilter.length(); ++i) {
    const vec2 o = (rm*omniFilter[i])*filterR;
    lit += omniGather(u + o.x*T + o.y*B, hf, filterHome, P, n, R, atlasInv, ratio, count);
    }
  return lit/float(omniFilter.length());
#endif
  }
#endif

void main() {
  vec2  scr = (gl_FragCoord.xy/vec2(textureSize(depth,0)))*2.0-1.0;
  float z   = texelFetch(depth, ivec2(gl_FragCoord.xy), 0).x;

  vec4 pos = scene.viewProjectLwcInv*vec4(scr.x,scr.y,z,1.0);
  pos.xyz/=pos.w;
#if defined(VIRTUAL_SHADOW)
  // geometric normal of the visible surface, before any discard (derivatives)
  vec3 geomNormal = cross(dFdx(pos.xyz), dFdy(pos.xyz));
#endif
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
    // the plane for the shadow's depth test: the geometric one, unless it disagrees much (a silhouette, a normal map)
    geomNormal = dot(geomNormal,geomNormal)>1e-12 ? normalize(geomNormal) : vec3(0);
    geomNormal *= sign(dot(geomNormal, normal));
    const vec3 planeN = dot(geomNormal, normal)>=0.3 ? geomNormal : normal;
    // too faint to show a soft edge: under ~1/500 of white before the tonemapper
    const bool faint  = max(color.r, max(color.g, color.b))*light*max(1.0, scene.exposure)*0.25 < 0.002;
    light *= omniShadow(pos.xyz - cenPosition.xyz, planeN, cenPosition.w, lights[lightId].sourceSize, faint);
#if defined(VSM_LAMP_PROBE)
    if(push.probeStamp!=0) {
      // 0 lamp-pixel pairs at the filter, 1 pixels lit by any lamp, 2 pairs past 'faint', 3 out after the first
      // 4 taps (lit), 4 out blocked, 5 gathers past 'faint', 6 taps checking the neighbour face, 7 taps read from it,
      // 8..13 pairs at the filter by the penumbra's half-width (1, 1.5, 2, 3, 6, more texels at the face's centre),
      // 14 the search at its floor of 1.5 texels, 15 pairs filtered by four taps
      const uint px = uint(gl_FragCoord.y)*uint(textureSize(depth,0).x) + uint(gl_FragCoord.x);
      atomicAdd(probe[0], 1u);
      if(atomicExchange(probePixel[px], push.probeStamp)!=push.probeStamp)
        atomicAdd(probe[1], 1u);
      if(!faint) {
        atomicAdd(probe[2], 1u);
        if(probeEarly==1u)
          atomicAdd(probe[3], 1u);
        if(probeEarly==2u)
          atomicAdd(probe[4], 1u);
        atomicAdd(probe[5], probeTaps);
        atomicAdd(probe[6], probeSlow);
        atomicAdd(probe[7], probeNeighbour);
        if((probeBin & 8u)!=0)
          atomicAdd(probe[8 + (probeBin & 7u)], 1u);
        if((probeBin & 16u)!=0)
          atomicAdd(probe[14], 1u);
        if((probeBin & 32u)!=0)
          atomicAdd(probe[15], 1u);
        }
      }
    if(light<=0.0 && (push.probeExp & 32u)==0u)
      discard;
#else
    if(light<=0.0)
      discard;
#endif
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

#if defined(VSM_LAMP_PROBE)
  if((push.probeExp & 32u)!=0u) {
    // the exit path: green lit after the first search, red blocked after it, blue a narrow filter, yellow a wide one,
    // grey faint
    vec3 c = vec3(0.15);
    if(probeEarly==1u)
      c = vec3(0,1,0); else
    if(probeEarly==2u)
      c = vec3(1,0,0); else
    if((probeBin & 8u)!=0u)
      c = (probeBin & 32u)!=0u ? vec3(0,0.3,1) : vec3(1,0.8,0);
    ret = c*0.5*max(1.0, scene.exposure);
    }
#endif
  outColor = vec4(ret, 0.0);
  }
