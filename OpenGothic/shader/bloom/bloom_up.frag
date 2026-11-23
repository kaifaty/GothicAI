#version 450
#extension GL_ARB_separate_shader_objects : enable

// Bloom, one step up the chain: a 3x3 tent over the smaller level, added onto the larger one (blend One, One).
layout(binding = 0) uniform sampler2D src;

layout(location = 0) in  vec2 UV;
layout(location = 0) out vec4 outColor;

vec3 tap(vec2 o) {
  return textureLod(src, UV + o/vec2(textureSize(src,0)), 0).rgb;
  }

void main() {
  vec3 r = tap(vec2(0,0))*4.0;
  r += (tap(vec2(-1,0)) + tap(vec2(1,0)) + tap(vec2(0,-1)) + tap(vec2(0,1)))*2.0;
  r +=  tap(vec2(-1,-1)) + tap(vec2(1,-1)) + tap(vec2(-1,1)) + tap(vec2(1,1));
  outColor = vec4(r/16.0, 1.0);
  }
