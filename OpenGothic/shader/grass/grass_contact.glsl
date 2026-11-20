#ifndef GRASS_CONTACT_GLSL
#define GRASS_CONTACT_GLSL

#include "grass/grass_light.glsl"

// The sun's screen ray over the grass (reports/render-features/2026-11-16-grass-sun-contact): a blade is in no
// shadow map, so it shades neither the ground at its root nor the blade behind it. A short ray from a sunlit point
// towards the sun through the depth buffer finds the blades in its way. Only a blade's pixel casts: the ground, a
// stone, the hero are in the shadow map and would be counted twice. Near the camera only: further a blade is
// thinner than a pixel, and the ground's shade under the grass (grass_ground.glsl) stands for the shadow.
// Needs scene.glsl and common.glsl. [INTERNAL] grassContact=0 or OPENGOTHIC_GRASS_CONTACT=0 turn it off
// (graphics/grasscontact.h, VF_GRASS_CONTACT: set in a frame after one that drew blades).
// An occupancy mask of the G-buffer rejects empty ray rectangles; OPENGOTHIC_GRASS_CONTACT_MASK=0 disables it.
// OPENGOTHIC_GRASS_CONTACT_STUDY=count|compare captures exact rectangle emptiness or checks skips against the full ray.

const float GrassContactLength = 30.0;  // cm towards the sun: a blade of the main grass at the sun's mid height
const int   GrassContactSteps  = 12;    // at most; a ray that is short on the screen takes fewer, one per StepPx
const float GrassContactStepPx = 3.0;
const float GrassContactMinPx  = 6.0;   // a ray shorter than this on the screen crosses no blade it could tell
// cm: a blade this much nearer to the eye than the ray is in its way; nearer than that it stands before both
const float GrassContactThick  = 4.0;
const float GrassContactFrom   = 600.0; // cm from the eye: the shadow fades from here
const float GrassContactTo     = 900.0; // and is gone here
const float GrassContactDark   = 0.85;  // a blade lets some of the sun through
const float GrassContactLift   = 0.3;   // cm off the surface, and a share of the distance to the eye
const float GrassContactLiftK  = 0.0015;
const float GrassContactOwn    = 1.5;   // cm: the receiver's own surface within this of its plane casts nothing on it
const float GrassContactOwnK   = 0.6;   // and within this share of the ray's depth under the plane: a blade leans and bends
const float GrassContactSkip   = 3.0;   // cm: what a blade's ray meets nearer than this is the blade itself

// Include a one-pixel margin for projection and truncation, and inspect only on-screen texels.
void grassContactBounds(vec2 px0, vec2 px1, vec2 size, out ivec2 lo, out ivec2 hi) {
  lo = ivec2(clamp(floor(min(px0, px1)) - 1.0, vec2(0), size));
  hi = ivec2(clamp(floor(max(px0, px1)) + 1.0, vec2(-1), size - 1.0));
  }

bool grassContactRectEmpty(sampler2D diffTex, ivec2 lo, ivec2 hi) {
  for(int y=lo.y; y<=hi.y; ++y)
    for(int x=lo.x; x<=hi.x; ++x)
      if(isGBufGrass(texelFetch(diffTex, ivec2(x,y), 0).a))
        return false;
  return true;
  }

// Each word from grass_contact_mask.comp covers 64x64 pixels, with one bit per 16x16 cell in row order.
bool grassContactMaskEmpty(usampler2D mask, ivec2 lo, ivec2 hi) {
  if(any(greaterThan(lo, hi)))
    return true;
  const ivec2 cellLo = lo >> 4, cellHi = hi >> 4;
  const ivec2 wordLo = cellLo >> 2, wordHi = cellHi >> 2;
  const ivec2 words  = wordHi - wordLo + 1;
  if(words.x * words.y > 4)
    return false;
  for(int y=wordLo.y; y<=wordHi.y; ++y) {
    const int firstRow = max(cellLo.y - 4*y, 0);
    const int lastRow  = min(cellHi.y - 4*y, 3);
    const uint rows    = (0xFFFFu << (4*firstRow)) & (0xFFFFu >> (4*(3-lastRow)));
    for(int x=wordLo.x; x<=wordHi.x; ++x) {
      const int firstCol = max(cellLo.x - 4*x, 0);
      const int lastCol  = min(cellHi.x - 4*x, 3);
      const uint cols    = (0xFu << firstCol) & (0xFu >> (3-lastCol));
      const uint covered = rows & (cols * 0x1111u); // repeat the column bits in every row
      if((texelFetch(mask, ivec2(x,y), 0).r & covered)!=0u)
        return false;
      }
    }
  return true;
  }

// frag: gl_FragCoord.xy; d: the pixel's depth; surfN: the surface's normal, vec3(0) for a blade's pixel (its card
// turns its width to the eye, its normal is three quarters the ground's). Returns the share of the sun left, 0..1
float grassContact(in sampler2D depthTex, in sampler2D diffTex, in usampler2D mask, const vec2 frag, const float d,
                   const vec3 surfN, const bool useMask, const uint study, inout uint grassClass) {
  const vec4  scr  = vec4(frag*scene.screenResInv*2.0 - vec2(1.0), d, 1.0);
  const vec4  pos4 = scene.viewProjectInv*scr;
  const vec4  clip = scr/pos4.w;
  const float fade = 1.0 - smoothstep(GrassContactFrom, GrassContactTo, clip.w);
  if(fade<=0.0)
    return 1.0;

  const vec3  toEye  = scene.camPos - pos4.xyz/pos4.w;
  const vec3  view   = normalize(toEye);
  const bool  blade  = surfN==vec3(0);
  const vec3  planeN = blade ? normalize(vec3(toEye.x, 0.0, toEye.z) + vec3(1e-4, 0.0, 0.0)) : surfN;
  const vec3  dir    = scene.sunDir;

  const float lift = GrassContactLift + GrassContactLiftK*clip.w;
  const vec4  c0   = clip + scene.viewProject*vec4(planeN*lift, 0.0);
  const vec4  dc   = scene.viewProject*vec4(dir*GrassContactLength, 0.0);
  const vec4  c1   = c0 + dc;
  if(c0.w<=0.0 || c1.w<=0.0)
    return 1.0;

  const vec2  size  = vec2(textureSize(depthTex, 0));
  const vec2  px0   = (c0.xy/c0.w*0.5 + 0.5)*size;
  const vec2  px1   = (c1.xy/c1.w*0.5 + 0.5)*size;
  const float pxLen = length(px1-px0);
  if(pxLen < GrassContactMinPx)
    return 1.0;

  if(study==1u) {
    ivec2 lo, hi;
    grassContactBounds(px0, px1, size, lo, hi);
    grassClass = grassContactRectEmpty(diffTex, lo, hi) ? 2u : 1u;
    return 1.0;
    }

  bool skipped = false;
  if(useMask) {
    // With positive endpoint w, projecting c0 + dc*t for 0 <= t <= 1 gives a convex combination
    // of px0 and px1. Bound the viewport-scaled clip magnitude / w by 2^18. Even 64 binary32
    // rounding errors (unit roundoff 2^-24) then fit within the one-pixel padding.
    // Leave the existing sampling at the high screen edge alone, where an index could round to size.
    const vec3 magnitude = max(abs(c0.xyw), abs(dc.xyw));
    const float limit = min(c0.w,c1.w) * (262144.0/max(size.x,size.y));
    if(all(lessThan(magnitude, vec3(limit))) && all(lessThan(max(px0,px1) + 1.0, size))) {
      ivec2 lo, hi;
      grassContactBounds(px0, px1, size, lo, hi);
      skipped = grassContactMaskEmpty(mask, lo, hi);
      }
    // The mask ORs the same grass predicate over every covered cell. Its zero proves that no texel
    // the loop can fetch passes isGBufGrass, so the full loop would also return 1.0.
    if(skipped && study==0u)
      return 1.0;
    }
  if(study==2u)
    grassClass = skipped ? 2u : 1u;

  // the ray's depth under the receiver's own plane at its end, cm along the view: over 0 when the sun stands behind
  // the plane (a blade lit from behind). What lies at that depth is the receiver itself, not a thing in the way
  const float sink  = -dot(dir, planeN)*GrassContactLength/max(dot(view, planeN), 0.15);
  const int   steps = clamp(int(pxLen/GrassContactStepPx), 4, GrassContactSteps);
  const ivec2 self  = ivec2(px0);
  const float noise = interleavedGradientNoise(frag + 5.588238*float(scene.taaFrame & 63u));
  for(int i=0; i<steps; ++i) {
    // denser near the point: the blade that touches it is the one that ties it to the ground
    const float s = (float(i) + noise)/float(steps);
    const float t = s*sqrt(s);
    const vec4  c = c0 + dc*t;
    const vec3  n = c.xyz/c.w;
    if(any(greaterThanEqual(abs(n.xy), vec2(1.0))))
      break;
    const ivec2 at = ivec2((n.xy*0.5 + 0.5)*size);
    if(at==self || (blade && t*GrassContactLength<GrassContactSkip))
      continue;
    const float ray   = linearDepth(n.z, scene.clipInfo);
    const float there = linearDepth(texelFetch(depthTex, at, 0).x, scene.clipInfo);
    const float under = ray - there;
    if(under <= 0.2 + 0.002*ray || under >= GrassContactThick)
      continue;
    const float own = sink*t;
    if(own>0.0 && abs(under-own) < GrassContactOwn + GrassContactOwnK*own)
      continue;
    if(!isGBufGrass(texelFetch(diffTex, at, 0).a))
      continue;
    if(study==2u && skipped)
      grassClass = 3u;
    // towards its end the ray lets go: the shadow of a blade's tip is thinner than the blade
    return 1.0 - fade*GrassContactDark*(1.0 - smoothstep(0.75, 1.0, t));
    }
  return 1.0;
  }

#endif
