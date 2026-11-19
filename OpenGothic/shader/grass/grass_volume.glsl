#ifndef GRASS_VOLUME_GLSL
#define GRASS_VOLUME_GLSL

#include "grass/grass_common.glsl"

// The far meadow as a volume (reports/engine/2026-11-19-grass-far-volume). Past where the blades go into the ground,
// the ground of the grass is a shell of grass under its surface: a ray from the eye goes down through it in a few
// steps (Habel, Wimmer, Jeschke 2007, «Instant Animated Grass»), and the grass in it is the meadow's own: the
// patches set its height and density (Grass::patchHeight, patchDensity), the tufts of a 75 cm lattice gather it
// into bodies of their own height and dryness, as the blades' tufts do (grassplace.h). Each step meets the blades of
// its height, the lit tips at the top and the shaded roots low, in the shade of the tuft above them from the sun.
// The layer keeps the tone the far ground has (grass_ground.glsl), as a meadow of the mean patch would have it, and
// adds what the eye sees of the grass that is there: the tufts, the gaps between them, the drier and the greener,
// the side of the tuft the sun does not reach, the parallax of tops over roots. It comes in where the blades thin
// out and goes to the plain far look where a tuft is a few pixels. A ground pixel nearer than that pays nothing
const float GrassVolumeFrom       = 0.9;    // of grassRing.y: the layer comes in from here to grassRing.z
const int   GrassVolumeSteps      = 8;
const float GrassVolumeSigma      = 0.064;  // 1/cm at a density of 1: 320 blades a m², 2 cm wide
const float GrassVolumeShell      = 3.2;    // of the record's height: the tallest tuft of the tallest patch
const float GrassVolumeTuftStep   = 75.0;   // cm: TuftStep of grassplace.h
const float GrassVolumeDetailFrom = 12.0;   // cm a pixel: a tuft is eight pixels across and more
const float GrassVolumeDetailTo   = 36.0;   // two and less: the plain far look
const float GrassVolumeMeanPatch  = 36000.0; // the patches' noise of the mean meadow the layer keeps the tone of
const float GrassVolumeAmbient    = 0.4;    // the light a blade in the shade still has, of the sun's
const float GrassVolumeMin        = 0.55;
const float GrassVolumeMax        = 1.6;

float grassVolumeRamp(float n, float a, float b, float x, float y) {
  return mix(x, y, clamp((n-a)/(b-a), 0.0, 1.0));
  }

// Grass::patchHeight
float grassPatchHeight(float n) {
  if(n<18361.0)
    return 0.55;
  if(n<GrassPatchMain0)
    return grassVolumeRamp(n, 18361.0, GrassPatchMain0, 0.55, 0.65);
  if(n<GrassPatchMain1)
    return grassVolumeRamp(n, GrassPatchMain0, GrassPatchMain1, 0.65, 0.9);
  if(n<46436.0)
    return grassVolumeRamp(n, GrassPatchMain1, 46436.0, 0.9, 1.15);
  return grassVolumeRamp(n, 46436.0, 56000.0, 1.5, 2.4);
  }

// Grass::patchDensity, of BladesPerM2
float grassPatchDensity(float n) {
  if(n<GrassPatchMain0)
    return 1.25;
  if(n<GrassPatchMain1)
    return grassVolumeRamp(n, GrassPatchMain0, GrassPatchMain1, 1.25, 1.0);
  if(n<46436.0)
    return 1.0;
  return grassVolumeRamp(n, 46436.0, 56000.0, 1.0, 2.0);
  }

// The tuft at a place: x how much the place is in it (0 between tufts), y its height and z its dryness, of 1
vec3 grassVolumeTuft(vec2 xz) {
  const vec2  c    = xz/GrassVolumeTuftStep;
  const ivec2 base = ivec2(floor(c - 0.5));
  vec3 best = vec3(0.0, 1.0, 0.5);
  for(int j=0; j<2; ++j)
    for(int i=0; i<2; ++i) {
      const ivec2 cell = base + ivec2(i,j);
      const uint  h    = grassHash(cell.x, cell.y, 0xC1u);
      const vec2  at   = (vec2(cell) + vec2(float(h & 0xFFu), float((h >> 8) & 0xFFu))/255.0)*GrassVolumeTuftStep;
      const float r    = 22.0 + 18.0*float((h >> 16) & 0xFu)/15.0;
      const float in_  = 1.0 - smoothstep(0.45*r, r, distance(xz, at));
      if(in_>best.x)
        best = vec3(in_, 0.7 + 0.65*float((h >> 20) & 0xFu)/15.0, float((h >> 24) & 0xFFu)/255.0);
      }
  return best;
  }

// pos: the ground's point, cm; gn: its normal; toEye: to the eye, of 1; d: cm to it; heightCm: the record's height;
// lean: the tangent of the gust's lean (grass_ground.glsl); k: the layer's share here.
// x..z: what the colour of the far ground is multiplied by, w: its ambient occlusion
vec4 grassVolume(vec3 pos, vec3 gn, vec3 toEye, float d, float heightCm, float lean, float k) {
  const float pixel  = 2.0*d/(abs(scene.project[1][1])*float(scene.screenRes.y));
  k *= 1.0 - smoothstep(GrassVolumeDetailFrom, GrassVolumeDetailTo, pixel);
  if(k<=0.001)
    return vec4(1.0);

  const float shell  = heightCm*GrassVolumeShell;
  const float c      = max(dot(toEye, gn), 0.03);              // the ray's descent a cm of its length
  const vec3  ray    = -toEye;
  const vec2  across = (ray - gn*dot(ray, gn)).xz/c;            // cm aside a cm down
  const vec2  wind   = scene.foliageWind.xy*(0.5*lean);         // the tops lie over downwind
  const float sunC   = max(dot(scene.sunDir, gn), 0.12);
  const float slice  = shell/float(GrassVolumeSteps);
  const float path   = slice/c;

  // the meadow here and the mean meadow it keeps the tone of
  const float noise  = float(grassPatch(ivec2(floor(pos.xz*16.0))));
  const float hPatch = heightCm*grassPatchHeight(noise);
  const float rPatch = grassPatchDensity(noise);
  const float hMean  = heightCm*grassPatchHeight(GrassVolumeMeanPatch)*0.85;
  const float rMean  = grassPatchDensity(GrassVolumeMeanPatch)*0.8;
  const vec3  tipMean= mix(GrassTipGreen, GrassTipStraw, 0.5);

  vec3  col = vec3(0.0), colRef = vec3(0.0);
  float ao  = 0.0,       aoRef  = 0.0;
  float t   = 1.0,       tRef   = 1.0;
  for(int i=0; i<GrassVolumeSteps; ++i) {
    const float down = (float(i)+0.5)*slice;
    const float y    = shell - down;                            // cm over the shell's bottom, where the roots are
    const vec2  xz   = pos.xz + across*down - wind*y;
    const vec3  tuft = grassVolumeTuft(xz);
    const float h    = hPatch*mix(0.6, tuft.y, tuft.x);
    const float rho  = rPatch*mix(0.55, 1.3, tuft.x);
    const float u    = y/max(h, 1.0);
    if(u<1.0) {
      const float a     = 1.0 - exp(-GrassVolumeSigma*rho*(1.0 - u*u)*path);
      const float sun   = exp(-GrassVolumeSigma*rho*0.5*(h - y)/sunC);
      const vec3  tip   = mix(GrassTipGreen, GrassTipStraw, tuft.z);
      const vec3  f     = mix(vec3(GrassRootShade), tip, grassTone(u))*mix(GrassVolumeAmbient, 1.0, sun);
      col += t*a*f;
      ao  += t*a*mix(GrassRootAo, 1.0, u);
      t   *= 1.0 - a;
      }
    const float uRef = y/hMean;
    if(uRef<1.0) {
      const float a     = 1.0 - exp(-GrassVolumeSigma*rMean*(1.0 - uRef*uRef)*path);
      const float sun   = exp(-GrassVolumeSigma*rMean*0.5*(hMean - y)/sunC);
      const vec3  f     = mix(vec3(GrassRootShade), tipMean, grassTone(uRef))*mix(GrassVolumeAmbient, 1.0, sun);
      colRef += tRef*a*f;
      aoRef  += tRef*a*mix(GrassRootAo, 1.0, uRef);
      tRef   *= 1.0 - a;
      }
    }
  // what the ray meets under the grass: the earth in the shade of all of it
  const vec3 earth = vec3(GrassRootShade*GrassVolumeAmbient);
  col    += t*earth;
  colRef += tRef*earth;
  ao     += t*GrassRootAo;
  aoRef  += tRef*GrassRootAo;

  const vec3  m  = clamp(col/max(colRef, vec3(1e-3)), vec3(GrassVolumeMin), vec3(GrassVolumeMax));
  const float mo = clamp(ao/max(aoRef, 1e-3), GrassVolumeMin, 1.0/GrassVolumeMin);
  return vec4(mix(vec3(1.0), m, k), mix(1.0, mo, k));
  }

#endif
