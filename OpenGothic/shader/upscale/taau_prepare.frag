#version 450

#extension GL_ARB_separate_shader_objects : enable
#extension GL_EXT_control_flow_attributes : enable

// Our own temporal upscale (graphics plan 1g; reports/render-features/2026-11-05-upscale/step-1g-1-taau.md): what
// every output pixel of one input texel shares, once per input texel instead of once per output pixel
// (taau_resolve.frag reads it by the texel that holds the output pixel's centre):
//  - the motion of the nearest point of the 3x3 neighbourhood, as the TAA resolve takes it, so that an edge carries
//    its object's history with it: the camera's from the depth plus the object's own from the G-buffer
//    (motion_camera.frag), from the current position to the previous one, in texture coordinates;
//  - the colour box the history is clipped to: the variance box (Salvi 2016) inside the min-max of the 3x3
//    neighbourhood, YCoCg. The resolve widens it where the point stands still.

layout(push_constant, std430) uniform Push {
  mat4  reproject; // this frame's NDC and depth -> the previous frame's NDC, both with this frame's jitter
  float gamma;     // the variance box in sigmas
  } push;

layout(binding = 0) uniform sampler2D depth;
layout(binding = 1) uniform sampler2D motion;   // the objects' own motion (main.frag), same size as the depth
layout(binding = 2) uniform sampler2D current;  // linear HDR, pre-exposed, same size

layout(location = 0) out vec4 outBoxMid; // the box's centre, YCoCg; a: motion x
layout(location = 1) out vec4 outBoxExt; // the box's half extent, YCoCg; a: motion y

vec3 rgb2ycocg(vec3 c) {
  return vec3( 0.25*c.r + 0.5*c.g + 0.25*c.b,
               0.5 *c.r           - 0.5 *c.b,
              -0.25*c.r + 0.5*c.g - 0.25*c.b);
  }

void main() {
  const ivec2 size = textureSize(depth, 0);
  const ivec2 pix  = ivec2(gl_FragCoord.xy);

  float z    = 2.0;
  ivec2 at   = pix;
  vec3  m1   = vec3(0), m2 = vec3(0);
  vec3  nMin = vec3(1e30), nMax = vec3(-1e30);
  [[unroll]]
  for(int y=-1; y<=1; ++y) {
    [[unroll]]
    for(int x=-1; x<=1; ++x) {
      const ivec2 p  = clamp(pix + ivec2(x,y), ivec2(0), size-1);
      const float zp = texelFetch(depth, p, 0).r;
      if(zp<z) {
        z  = zp;
        at = p;
        }
      const vec3 yc = rgb2ycocg(texelFetch(current, p, 0).rgb);
      m1  += yc;
      m2  += yc*yc;
      nMin = min(nMin, yc);
      nMax = max(nMax, yc);
      }
    }

  const vec2 ndc  = ((vec2(at) + 0.5)/vec2(size))*2.0 - 1.0;
  const vec4 prev = push.reproject*vec4(ndc, z, 1.0);
  // behind the previous camera: off the screen from any pixel, the resolve takes this frame alone
  vec2 mv = vec2(4);
  if(prev.w>0.0)
    mv = (prev.xy/prev.w - ndc)*0.5 + texelFetch(motion, at, 0).xy;

  const vec3  mean  = m1/9.0;
  const vec3  sigma = sqrt(max(m2/9.0 - mean*mean, vec3(0)));
  const vec3  vMin  = max(mean - push.gamma*sigma, nMin);
  const vec3  vMax  = min(mean + push.gamma*sigma, nMax);

  outBoxMid = vec4(0.5*(vMax + vMin), mv.x);
  outBoxExt = vec4(0.5*(vMax - vMin), mv.y);
  }
