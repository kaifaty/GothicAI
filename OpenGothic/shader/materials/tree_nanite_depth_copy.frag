#version 460

layout(set=0, binding=0) uniform sampler2D sourceDepth;

void main() {
  gl_FragDepth = texelFetch(sourceDepth, ivec2(gl_FragCoord.xy), 0).r;
  }
