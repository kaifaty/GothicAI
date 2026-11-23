#version 450

#extension GL_ARB_separate_shader_objects : enable

// Motion vectors for MetalFX Temporal (graphics plan 1г; reports/render-features/2026-11-05-upscale): where this
// pixel's point lay a frame ago, from this frame's depth by the camera, plus the object's own move from the G-buffer
// (graphics plan 1b, Renderer::gbufMotion; 0 for the static world and with -mv 0). Apple's convention: from the
// current position to the previous one, in texture coordinates of the input, which is also the residual's; the
// scaler's motionVectorScale (the input size) makes pixels of them. The reprojection carries this frame's jitter on
// both sides (Taa::reprojection), so the vector has none.

layout(push_constant, std430) uniform Push {
  mat4 reproject; // this frame's NDC and depth -> the previous frame's NDC, both with this frame's jitter
  } push;

layout(binding = 0) uniform sampler2D depth;
layout(binding = 1) uniform sampler2D motion; // the objects' own motion (main.frag), same size as the depth

layout(location = 0) out vec2 outMotion;

void main() {
  const ivec2 size = textureSize(depth, 0);
  const ivec2 pix  = ivec2(gl_FragCoord.xy);
  const float z    = texelFetch(depth, pix, 0).r;
  const vec2  ndc  = ((vec2(pix) + 0.5)/vec2(size))*2.0 - 1.0;
  const vec4  prev = push.reproject*vec4(ndc, z, 1.0);
  if(prev.w<=0.0) {
    // behind the previous camera: no history to follow
    outMotion = vec2(0);
    return;
    }
  outMotion = (prev.xy/prev.w - ndc)*0.5 + texelFetch(motion, pix, 0).xy;
  }
