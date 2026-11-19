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

#endif
