#version 450
#extension GL_ARB_separate_shader_objects : enable

// Debug view of the motion vectors of objects (OPENGOTHIC_MOTION_VIEW=1, Renderer::drawMotionView): the frame shows
// gbufMotion. Hue is the direction (the residual points to where the point was a frame ago), brightness the length;
// full brightness at push.fullPx pixels a frame, a dark grey floor marks any nonzero value, black is exactly 0

layout(binding = 0) uniform sampler2D motion;

layout(push_constant, std430) uniform Push {
  vec2  pxPerUnit; // gbufMotion's size: texture coordinates to its pixels
  float fullPx;
  } push;

layout(location = 0) in  vec2 UV;
layout(location = 0) out vec4 outColor;

vec3 hue(float h) {
  const vec3 k = vec3(1.0, 2.0/3.0, 1.0/3.0);
  return clamp(abs(fract(h + k)*6.0 - 3.0) - 1.0, 0.0, 1.0);
  }

void main() {
  const vec2  m   = textureLod(motion, UV, 0).xy;
  const vec2  px  = m*push.pxPerUnit;
  const float len = length(px);
  if(len==0.0) {
    outColor = vec4(0,0,0,1);
    return;
    }
  const float a = atan(px.y, px.x)/(2.0*3.14159265) + 0.5;
  // a quarter of the way up for any nonzero value, then linear to full at fullPx: 1 px/frame is half brightness at 2
  const float v = 0.25 + 0.75*clamp(len/push.fullPx, 0.0, 1.0);
  outColor = vec4(hue(a)*v, 1);
  }
