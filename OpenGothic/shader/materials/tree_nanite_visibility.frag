#version 460
#extension GL_EXT_mesh_shader : require
#extension GL_GOOGLE_include_directive : require

#include "tree_nanite_geometry.glsl"

layout(early_fragment_tests) in;
layout(location=1) perprimitiveEXT in flat uvec2 rasterOwner;
layout(location=0) out uvec2 outVisibility;

void main() {
  outVisibility = treeVisibility(rasterOwner.x, rasterOwner.y, gl_FrontFacing);
  }
