#version 460
#extension GL_EXT_mesh_shader : require
#extension GL_GOOGLE_include_directive : require

#define TREE_MATERIAL_FRAGMENT
#include "tree_nanite_geometry.glsl"
#include "tree_nanite_material.glsl"

layout(early_fragment_tests) in;
layout(location=0) perprimitiveEXT in flat uint materialId;
layout(location=2) in vec2 rasterUv;
layout(location=3) in vec3 rasterNormal;
layout(location=4) in vec3 rasterPosition;
layout(location=5) in vec3 rasterPreviousStatic;
layout(location=6) in vec3 rasterPreviousDelta;
#if defined(VERIFY)
layout(location=1) perprimitiveEXT in flat uvec2 rasterOwner;
layout(location=4) out uvec2 outOwner;
#endif

void main() {
  const TreeMaterial material = materials[materialId];
  const vec2 uv = rasterUv * material.uvScaleFlags.xy;
  const TreeGradients grad = TreeGradients(dFdxFine(uv), dFdyFine(uv),
                                          dFdxFine(rasterPosition), dFdyFine(rasterPosition));
  treeWriteMaterial(materialId, material, uv, rasterNormal, gl_FrontFacing, grad);
  outMotion = treeMotion(rasterPreviousStatic, rasterPreviousDelta);
#if defined(VERIFY)
  outOwner = treeVisibility(rasterOwner.x, rasterOwner.y, gl_FrontFacing);
#endif
  }
