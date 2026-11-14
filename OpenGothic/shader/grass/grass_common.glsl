#ifndef GRASS_COMMON_GLSL
#define GRASS_COMMON_GLSL

// The grass system (reports/engine/2026-11-14-grass-system): what the blades (grass.vert, grass.frag) and the
// ground under them (grass_ground.glsl) share. The patches and the spots of bare earth are the processor's
// (common/graphics/grass/grasspatch.cpp): the same whole numbers, so the ground knows where the grass stands
// without being told. Keep the two in step.

// SceneDesc::grassRing: x full density to here, cm; y the blades go into the ground from here; z to here;
// w 1 with blades, 0.5 the ground's mark alone (OPENGOTHIC_GRASS=ground), 0 off

// Bucket::foliage of a ground under BK_GRASS (Grass::pack)
float grassPackedDensity(uint bits) { return float( bits        & 0x3Fu)*(2.0/63.0); }
float grassPackedHeight (uint bits) { return float((bits >> 6)  & 0x3Fu); }
float grassPackedSlope  (uint bits) { return float((bits >> 12) & 0x3Fu); }
float grassPackedWind   (uint bits) { return float((bits >> 18) & 0x3Fu)*(4.0/63.0); }
float grassPackedThrough(uint bits) { return float( bits >> 24)/255.0; }

// Parts of the look a bench switches off to weigh them, and the paint that measures the cover (Grass::Part of
// common/graphics/grass/grassdiag.h; `grass parts <mask>` of the bench): grassRing.w = 1 + 2*mask
const uint GrassPartGroundShade = 1u;
const uint GrassPartFar         = 2u;
const uint GrassPartTip         = 4u;
const uint GrassPartRoot        = 8u;
const uint GrassPartRootAo      = 16u;
const uint GrassPartNormal      = 32u;
const uint GrassPartPaint       = 64u;
const uint GrassPartHide        = 128u;
const uint GrassPartCover       = 256u; // the paint of the meadow's own ground (grass_layers.glsl): sward green, earth red

uint grassPartsOff(float ringW) { return uint(max(ringW - 1.0, 0.0)*0.5 + 0.25); }

// The paint of the ground: bands of the distance from the camera along the ground, 2, 3.5, 5 and 10 m, each an
// eighth of it wide to either side; black between them
vec3 grassPaintBand(float d) {
  if(abs(d- 200.0)< 25.0) return vec3(0,0,1);
  if(abs(d- 350.0)< 44.0) return vec3(0,1,1);
  if(abs(d- 500.0)< 62.0) return vec3(0,1,0);
  if(abs(d-1000.0)<125.0) return vec3(1,1,0);
  return vec3(0);
  }

// The look. A blade takes the colour of the ground under its root (a coarse level of its texture) and is darker
// at the root and lighter at the tip: the tone grows as the root of the height along it, so the shade sits low
// in the grass and what the eye meets from the side is the lit upper half
const float GrassRoughness   = 0.92;
const float GrassRootShade   = 0.55;                     // of the ground's colour at the root
const vec3  GrassTipGreen    = vec3(1.10, 1.40, 0.90);   // of it at the tip, by the tuft's dryness
const vec3  GrassTipStraw    = vec3(1.46, 1.38, 0.96);
// of a ground with its own layers (grass_layers.glsl): the colour under the root is the sward's own
const float GrassOwnRootShade= 0.62;
const vec3  GrassOwnTipGreen = vec3(1.16, 1.22, 1.04);
const vec3  GrassOwnTipStraw = vec3(1.30, 1.24, 1.02);
const float GrassRootAo      = 0.35;
const float GrassWidth       = 1.3;                      // cm at the root, and per step of the blade's four bits
const float GrassWidthStep   = 0.11;
const float GrassTaper       = 0.7;                      // of the width is gone at the tip
// The ground. Seen from above it lies between the blades in their shade. Seen along the meadow it stands for the
// blades the eye would meet behind the ones that are drawn, and from afar for all of them: it takes what the
// grass looks like together. The share of that look: by the eye's direction against the ground, from GrassGrazeTo
// (none) to GrassGrazeFrom (whole), and by the distance: the shade between the blades is a thing of the close view,
// gone from where the blades start to thin to half of the distance where they start to go (grass_ground.glsl)
const float GrassGroundShade = 0.72;
const vec3  GrassFarTint     = vec3(1.00, 1.10, 0.74);
const float GrassFarAo       = 0.95;
const float GrassGrazeFrom   = 0.25;
const float GrassGrazeTo     = 0.70;
const vec3  GrassEarthTint   = vec3(0.92, 0.70, 0.80);   // the ground of a bare spot: earth, not painted grass

float grassTone(float t) { return sqrt(clamp(t, 0.0, 1.0)); }

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

// Grass::patchNoise at a place in 1/16 cm; under GrassPatchMain0 the short sward, over GrassPatchMain1 the main
// grass (common/graphics/grass/grasspatch.cpp)
const float GrassPatchMain0 = 26192.0;
const float GrassPatchMain1 = 28857.0;

int grassPatch(ivec2 p) {
  return (3*grassValueNoise(p,9600,0x51u) + grassValueNoise(p+ivec2(1777,911),3520,0xA7u)) >> 2;
  }

// Grass::patchBare: a spot of bare earth in the short sward, 0..1
float grassBare(ivec2 p) {
  const int step = 6400, radius = 880, soft = 480, rim = 320;
  const int cx = grassFloorDiv(p.x-step/2,step), cz = grassFloorDiv(p.y-step/2,step);
  float bare = 0.0;
  for(int j=cz; j<=cz+1; ++j)
    for(int i=cx; i<=cx+1; ++i) {
      const uint h = grassHash(i,j,0xB3u);
      if((h & 0xFFu)>=140u)
        continue;
      const ivec2 c = ivec2(i,j)*step + ivec2((((h >> 8) & 0xFFFu)*uint(step)) >> 12, (((h >> 20) & 0xFFFu)*uint(step)) >> 12);
      const int   r = radius + int(((grassHash(i,j,0xB4u) & 0xFFu)*uint(radius)) >> 8);
      const vec2  d = vec2(p-c);
      if(dot(d,d)>=float(r+rim)*float(r+rim) || grassPatch(c)>=int(GrassPatchMain0))
        continue;
      const float l = length(d) + float(grassValueNoise(p,960,0xB5u)-32640)*(float(rim)/32640.0);
      bare = max(bare, clamp((float(r)-l)/float(soft), 0.0, 1.0));
      }
  return bare;
  }

// How much grass stands at a place of the patches, of the main grass: the short sward is as dense to the eye
float grassStand(int noise) {
  return mix(0.85, 1.0, clamp((float(noise)-GrassPatchMain0)/(GrassPatchMain1-GrassPatchMain0), 0.0, 1.0));
  }

#endif
