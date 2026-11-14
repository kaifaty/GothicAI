#ifndef GRASS_COMMON_GLSL
#define GRASS_COMMON_GLSL

// The grass system (reports/engine/2026-11-14-grass-system): what the blades (grass.vert) and the ground under
// them (grass_ground.glsl) share. The patches are the processor's (common/graphics/grass/grassplace.cpp): the same
// whole numbers, so the ground knows where the grass stands without being told. Keep the two in step.

// SceneDesc::grassRing: x full density to here, cm; y the blades go into the ground from here; z to here;
// w 1 with blades, 0.5 the ground's mark alone (OPENGOTHIC_GRASS=ground), 0 off

// Bucket::foliage of a ground under BK_GRASS (Grass::pack)
float grassPackedDensity(uint bits) { return float( bits        & 0x3Fu)*(2.0/63.0); }
float grassPackedHeight (uint bits) { return float((bits >> 6)  & 0x3Fu); }
float grassPackedSlope  (uint bits) { return float((bits >> 12) & 0x3Fu); }
float grassPackedWind   (uint bits) { return float((bits >> 18) & 0x3Fu)*(4.0/63.0); }
float grassPackedThrough(uint bits) { return float( bits >> 24)/255.0; }

// The look
const float GrassRoughness   = 0.92;
const float GrassRootShade   = 0.55;                     // of the ground's colour at the root
const vec3  GrassTipGreen    = vec3(0.95, 1.15, 0.70);   // of it at the tip, by the tuft's dryness
const vec3  GrassTipStraw    = vec3(1.25, 1.15, 0.75);
const float GrassRootAo      = 0.35;
// The ground: in the shade of the blades that stand on it; and from afar what the blades look like together
const float GrassGroundShade = 0.65;
const vec3  GrassFarTint     = vec3(0.88, 0.93, 0.66);
const float GrassFarAo       = 0.74;

// The wind on an upright blade (change A of the note): it bends as a rod, by its length. Radians at pressure 1:
// 7.5 degrees at 12 cm, 16 at 20, 33 at 45
const float GrassLeanTop     = 0.6283;   // 36 degrees
const float GrassLeanLength  = 27.0;     // cm
const float GrassLeanPower   = 1.8;
const float GrassLeanMaxTan  = 1.678;    // twice tan(40 degrees): the lean and the swing together, at the tip
const float GrassSwingK      = 1.5;      // of the leaf's swing share
const float GrassNormalLean  = 0.6;      // of the lean's tangent tilts the normal against the wind
const float GrassLeanSheen   = 0.16;     // the grass that lies over shows more of its blades' faces: lighter
const float GrassStillFrom   = 2500.0;   // cm: the blade's own swing and the flutter fade from here
const float GrassStillTo     = 3000.0;

float grassLeanAngle(float heightCm) {
  return GrassLeanTop*(1.0 - exp(-pow(max(heightCm,0.0)/GrassLeanLength, GrassLeanPower)));
  }

uint grassHash(int ix, int iz, uint salt) {
  uint h = uint(ix)*0x9E3779B1u ^ uint(iz)*0x85EBCA77u ^ salt;
  h ^= h >> 15;
  h *= 0x2C1B3C6Du;
  h ^= h >> 12;
  h *= 0x297A2D39u;
  h ^= h >> 15;
  return h;
  }

int grassFloorDiv(int a, int b) {
  const int q = a/b;
  return (a%b!=0 && ((a<0)!=(b<0))) ? q-1 : q;
  }

int grassValueNoise(ivec2 p, int step, uint salt) {
  const int ix = grassFloorDiv(p.x,step), iz = grassFloorDiv(p.y,step);
  const int tx = ((p.x-ix*step)*256)/step, tz = ((p.y-iz*step)*256)/step;
  const int sx = (tx*tx*(768-2*tx)) >> 16, sz = (tz*tz*(768-2*tz)) >> 16;
  const int a = int((grassHash(ix,  iz,  salt) >> 8) & 0xFFu), b = int((grassHash(ix+1,iz,  salt) >> 8) & 0xFFu);
  const int c = int((grassHash(ix,  iz+1,salt) >> 8) & 0xFFu), d = int((grassHash(ix+1,iz+1,salt) >> 8) & 0xFFu);
  const int top = a*(256-sx) + b*sx;
  const int bot = c*(256-sx) + d*sx;
  return (top*(256-sz) + bot*sz) >> 8;
  }

// p: the place in 1/16 cm. Grass::patchNoise
int grassPatch(ivec2 p) {
  return (3*grassValueNoise(p,9600,0x51u) + grassValueNoise(p+ivec2(1777,911),3520,0xA7u)) >> 2;
  }

// Grass::patchDensity and patchHeight, as shares
const float GrassPatchBare  = 15962.0;
const float GrassPatchShort = 18361.0;
const float GrassPatchMain0 = 26192.0;
const float GrassPatchMain1 = 28857.0;

// How much grass stands at a place of the patches: 0 bare ground, about 0.3 the short grass, 1 the main and the
// clumps
float grassStand(int noise) {
  const float n       = float(noise);
  const float density = n<GrassPatchShort ? 0.7*clamp((n-GrassPatchBare)/(GrassPatchShort-GrassPatchBare), 0.0, 1.0)
                                          : mix(0.7, 1.0, clamp((n-GrassPatchMain0)/(GrassPatchMain1-GrassPatchMain0), 0.0, 1.0));
  const float height  = n<GrassPatchMain0 ? mix(0.3, 0.55, clamp((n-GrassPatchShort)/(GrassPatchMain0-GrassPatchShort), 0.0, 1.0))
                                          : mix(0.55, 0.9, clamp((n-GrassPatchMain0)/(GrassPatchMain1-GrassPatchMain0), 0.0, 1.0));
  return density*height/0.9;
  }

#endif
