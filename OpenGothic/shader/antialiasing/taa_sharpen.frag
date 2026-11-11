#version 450

#extension GL_ARB_separate_shader_objects : enable
#extension GL_GOOGLE_include_directive    : enable

#include "antialiasing/rcas.glsl"

layout(push_constant, std430) uniform Push {
  float sharpness; // 0..1: 0 copies the frame
  } push;

layout(binding = 0) uniform sampler2D src; // the resolved frame, linear HDR, pre-exposed

layout(location = 0) out vec4 outColor;

void main() {
  outColor = vec4(rcas(src, ivec2(gl_FragCoord.xy), push.sharpness), 1);
  }
