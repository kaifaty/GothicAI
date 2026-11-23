#version 450
#extension GL_ARB_separate_shader_objects : enable

// Bloom into the scene, before tonemapping: scene*(1-k) + bloom*k (blend One, SrcAlpha), no threshold, the way
// a lens scatters a little of all light. The chain sums its levels; dividing by their count keeps it an average.
layout(binding = 0) uniform sampler2D src;

layout(push_constant, std430) uniform Push {
  float strength;
  float levels;
  } push;

layout(location = 0) in  vec2 UV;
layout(location = 0) out vec4 outColor;

void main() {
  const vec3 bloom = textureLod(src, UV, 0).rgb/push.levels;
  outColor = vec4(bloom*push.strength, 1.0-push.strength);
  }
