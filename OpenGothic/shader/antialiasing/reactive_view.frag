#version 450
#extension GL_ARB_separate_shader_objects : enable

// Debug view of the TAA reactive mask (OPENGOTHIC_TAA_REACTIVE_VIEW=1, Renderer::drawReactiveView): the frame shows
// taa.reactive as it was written, before the resolve's ceiling. Black is exactly 0; any nonzero value starts at a
// dark red floor and goes up to white at 1, so a faint mask is not lost in the dark

layout(binding = 0) uniform sampler2D reactive;

layout(location = 0) in  vec2 UV;
layout(location = 0) out vec4 outColor;

void main() {
  const float m = textureLod(reactive, UV, 0).r;
  if(m==0.0) {
    outColor = vec4(0,0,0,1);
    return;
    }
  // red from 0.25 to 1 over the first half, then green and blue join it towards white
  const float r = 0.25 + 0.75*clamp(m*2.0, 0.0, 1.0);
  const float g = clamp(m*2.0 - 1.0, 0.0, 1.0);
  outColor = vec4(r, g, g, 1);
  }
