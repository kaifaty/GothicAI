#version 460
#extension GL_EXT_mesh_shader : require
#extension GL_GOOGLE_include_directive : require

#include "tree_nanite_geometry.glsl"

#if defined(TREE_GAME)
#include "tree_bough_dissolve.glsl"
layout(location=2) perprimitiveEXT in flat vec3 rasterPivot;
layout(location=3) perprimitiveEXT in flat uint rasterJoint;
#elif defined(TREE_WIND)
#if !defined(TREE_SHADOW)
#include "tree_nanite_level_mask.glsl"
#endif
layout(location=0) perprimitiveEXT in flat uint treeLayer;
layout(location=2) in vec2 treeUv;
layout(set=0, binding=5) uniform sampler2DArray treeAlbedo;
#else
layout(early_fragment_tests) in;
#endif
#if !defined(TREE_SHADOW)
layout(location=1) perprimitiveEXT in flat uvec2 rasterOwner;
layout(location=0) out uvec2 outVisibility;
#endif

void main() {
#if defined(TREE_GAME)
  const uint fade = boughJointFade(rasterJoint);
  if(fade != 0u && !boughJointCovers(fade ^ BoughFadeChild, rasterPivot, gl_FragCoord.xy, scene))
    discard;
#endif
#if defined(TREE_WIND) && !defined(TREE_GAME)
  const uint layer = treeLayer & 1u;
  if(frame.control.w != 0u && layer == 1u && texture(treeAlbedo, vec3(treeUv, float(layer))).a < 0.5)
    discard;
#if !defined(TREE_SHADOW)
  if(!treeLevelCovers(treeLayer >> 1u, gl_FragCoord.xy))
    discard;
#endif
#endif
#if !defined(TREE_SHADOW)
  outVisibility = treeVisibility(rasterOwner.x, rasterOwner.y, gl_FrontFacing);
#endif
  }
