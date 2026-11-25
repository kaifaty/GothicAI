#version 460
#extension GL_EXT_mesh_shader : require
#extension GL_GOOGLE_include_directive : require

#include "tree_nanite_geometry.glsl"

#if defined(TREE_WIND)
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
#if defined(TREE_WIND)
  if(frame.control.w != 0u && treeLayer == 1u && texture(treeAlbedo, vec3(treeUv, float(treeLayer))).a < 0.5)
    discard;
#endif
#if !defined(TREE_SHADOW)
  outVisibility = treeVisibility(rasterOwner.x, rasterOwner.y, gl_FrontFacing);
#endif
  }
