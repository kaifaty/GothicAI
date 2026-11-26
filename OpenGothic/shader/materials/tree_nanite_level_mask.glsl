#ifndef TREE_NANITE_LEVEL_MASK_GLSL
#define TREE_NANITE_LEVEL_MASK_GLSL

#include "common.glsl"

float treeCoarserShare(uint boundary) {
  return frame.lodCoverage[boundary/4u][boundary%4u];
  }

bool treeLevelCovers(uint level, vec2 samplePosition) {
  // The authored levels and billboard share one rank in the plane at the tree pivot.
  // Geometry, UVs and depth are allowed to differ on the two sides of each level band.
  if(frame.lodCoverage[0].x == 0.0)
    return level == 0u;
  const vec4 pivot = frame.viewProject * vec4(0, 0, 0, 1);
  const float depth = max(pivot.w, 50.0);
  const vec2 screen = (pivot.xy/depth*0.5 + 0.5)*frame.viewport.xy;
  const float cmPerPixel = 2.0*depth*frame.viewport.w/frame.up.w;
  const uvec2 cell = uvec2(ivec2(floor((samplePosition - screen)*cmPerPixel)));
  const float rank = float(pcg3d(uvec3(cell, 0)).x >> 8u)*(1.0/16777216.0);
  const float lower = level == 5u ? 0.0 : treeCoarserShare(level);
  const float upper = level == 0u ? 1.0 : treeCoarserShare(level - 1u);
  return rank >= lower && rank < upper;
  }

#endif
