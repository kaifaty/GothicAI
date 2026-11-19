#ifndef TREE_BOUGH_DISSOLVE_GLSL
#define TREE_BOUGH_DISSOLVE_GLSL

#include "common.glsl"

// Both versions hash the same raster sample relative to the projected tree pivot. Their atlas UVs
// and depths are unrelated. Subtracting the jittered projection also removes the camera's TAA jitter.
const uint BoughChildSide = 0x8000u;
const uint BoughWeightMask = 0xffffffu;
const uint BoughFadeChild = 0x1000000u;
const uint BoughFadeActive = 0x2000000u;

bool boughCovers(uint fade, uint seed, vec2 sampleOffset) {
  if((fade & BoughFadeActive)==0u)
    return true;
  const uvec2 cell = uvec2(ivec2(floor(sampleOffset)));
  const uint rank = pcg3d(uvec3(cell, seed)).x >> 8u;
  const uint weight = fade & BoughWeightMask;
  const bool parentCovers = rank < weight || weight==BoughWeightMask;
  return parentCovers != ((fade & BoughFadeChild)!=0u);
  }

#endif
