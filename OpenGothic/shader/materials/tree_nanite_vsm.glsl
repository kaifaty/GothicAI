#ifndef TREE_NANITE_VSM_GLSL
#define TREE_NANITE_VSM_GLSL

#include "tree_nanite_geometry.glsl"
#include "virtual_shadow/vsm_common.glsl"

const uint TreeShadowLanes = 32u;
const uint TreeShadowBatch = 1024u;
const uint TreeShadowRasterWidth = 128u;
const uint TreeShadowRasterBatch = 65536u;

struct TreeShadowSource {
  vec4 lower, upper;
  uvec4 draw; // physical meshlet, material, contour flags
  };

layout(set=0, binding=20, std430) readonly buffer TreeShadowSources { TreeShadowSource casters[]; };
layout(set=0, binding=21, std430) readonly buffer TreeShadowPages { VsmHeader header; uint pageList[]; } treePages;
layout(set=0, binding=22, std430) readonly buffer TreeShadowLights { LightSource treeLights[]; };
#if defined(TREE_SHADOW_RASTER)
layout(set=0, binding=23, std430) readonly buffer TreeShadowPairs { uvec2 treePairs[]; };
layout(set=0, binding=24, std430) readonly buffer TreeShadowStats { uvec4 treeShadowStats; };
#else
layout(set=0, binding=23, std430) writeonly buffer TreeShadowPairs { uvec2 treePairs[]; };
layout(set=0, binding=24, std430) buffer TreeShadowStats { uvec4 treeShadowStats; };
#endif

#endif
