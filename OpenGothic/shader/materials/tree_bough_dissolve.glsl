#ifndef TREE_BOUGH_DISSOLVE_GLSL
#define TREE_BOUGH_DISSOLVE_GLSL

#include "common.glsl"
#include "scene.glsl"

// Both versions hash the same raster sample relative to the projected tree pivot. Their atlas UVs
// and depths are unrelated. Subtracting the jittered projection also removes the camera's TAA jitter.
const uint BoughChildSide = 0x8000u;
const uint BoughWeightMask = 0xffffffu;
const uint BoughFadeChild = 0x1000000u;
const uint BoughFadeActive = 0x2000000u;
const uint BoughJointTag = 0x80000000u; // static instance animation slot and tagged G-buffer payload reference

bool boughCovers(uint fade, uint seed, vec2 sampleOffset) {
  if((fade & BoughFadeActive)==0u)
    return true;
  const uvec2 cell = uvec2(ivec2(floor(sampleOffset)));
  const uint rank = pcg3d(uvec3(cell, seed)).x >> 8u;
  const uint weight = fade & BoughWeightMask;
  const bool parentCovers = rank < weight || weight==BoughWeightMask;
  return parentCovers != ((fade & BoughFadeChild)!=0u);
  }

vec3 boughMaskCoordinates(vec3 origin, SceneDesc scene) {
  precise vec4 pivot = scene.view * vec4(origin, 1.0);
  pivot.z = max(pivot.z, scene.clipInfo.x / scene.clipInfo.z);
  precise vec4 clip = scene.project * pivot;
  precise vec2 screen = (clip.xy / clip.w * 0.5 + 0.5) * vec2(scene.screenRes);
  // Map offset pixels to centimetres in the plane at the pivot's depth.
  precise float scale = 2.0 * clip.w * scene.screenResInv.y / abs(scene.project[1][1]);
  return vec3(screen, scale);
  }

uint boughJointFade(uint joint) {
  if((joint & BoughJointTag)==0u)
    return 0u;
  const float weight = uintBitsToFloat(joint & ~BoughJointTag);
  return BoughFadeActive | clamp(uint(round(weight*float(BoughWeightMask))), 1u, BoughWeightMask-1u);
  }

uint boughJointSeed(vec3 origin) {
  // Wood, foliage and the billboard each have their own draw instance but share the tree pivot.
  return pcg3d(floatBitsToUint(origin)).y;
  }

vec2 boughSampleOffset(vec2 fragCoord, vec3 mask) {
  precise vec2 sampleOffset = (fragCoord - mask.xy) * mask.z;
  return sampleOffset;
  }

bool boughJointCovers(uint fade, vec3 origin, vec2 fragCoord, SceneDesc scene) {
  if((fade & BoughFadeActive)==0u)
    return true;
  const vec2 sampleOffset = boughSampleOffset(fragCoord, boughMaskCoordinates(origin, scene));
  return boughCovers(fade, boughJointSeed(origin), sampleOffset);
  }

// The level band of a hierarchy tree's own GLBs (graphics/treelevelband.h): its word in the deform slot of a static
// instance, the shares of the coarser GLB in the main view and in the shadows, and the side the instance draws.
const uint BoughBandTag = 0x80000000u;
const uint BoughBandCoarse = 0x40000000u;
const uint BoughBandBits = 15u;
const uint BoughBandFull = (1u << BoughBandBits) - 1u;

uint boughBandWord(Instance obj) {
  return obj.deformCount==0u ? obj.deformPtr : 0u;
  }

bool boughBandActive(uint word) {
  return (word & BoughBandTag)!=0u;
  }

// The coarser side holds the ranks below its share. The rank is independent of the joint's at the same cell.
bool boughBandCovers(uint word, bool shadow, uvec2 cell, uint seed) {
  if((word & BoughBandTag)==0u)
    return true;
  const uint share = (word >> (shadow ? BoughBandBits : 0u)) & BoughBandFull;
  const uint rank = pcg3d(uvec3(cell, seed)).y >> (32u - BoughBandBits);
  return (rank < share) == ((word & BoughBandCoarse)!=0u);
  }

// A shadow's cell: the sample's offset from the pivot in the plane across the light, in steps of 1/128 of the tree's
// height. Both GLBs of a band take the same cells, so one light ray sees one of them; the cells stay on the tree when
// the camera moves.
vec2 boughBandShadowCell(vec3 pos, vec3 pivot, vec3 toLight, float height) {
  const vec3 axis = abs(toLight.y)<0.999 ? vec3(0,1,0) : vec3(1,0,0);
  const vec3 u = normalize(cross(toLight, axis));
  const vec3 v = cross(toLight, u);
  const vec3 at = pos - pivot;
  return vec2(dot(at, u), dot(at, v)) * (128.0/max(height, 1.0));
  }

#endif
