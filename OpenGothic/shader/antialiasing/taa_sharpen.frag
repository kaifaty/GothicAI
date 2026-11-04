#version 450

#extension GL_ARB_separate_shader_objects : enable

// Sharpening after the TAA resolve: RCAS ("robust contrast adaptive sharpening") of AMD FidelityFX FSR 1,
// MIT licence, Copyright (c) 2021 Advanced Micro Devices, Inc. (FsrRcasF in ffx_fsr1.h), without its noise
// removal. The lobe is limited by the min and max of the cross, so a contrast edge (a lamp post against the sky)
// gets no halo. RCAS works in 0..1: the HDR frame goes through an invertible Reinhard of its max channel.

layout(push_constant, std430) uniform Push {
  float sharpness; // 0..1: 0 copies the frame
  } push;

layout(binding = 0) uniform sampler2D src; // the resolved frame, linear HDR, pre-exposed

layout(location = 0) out vec4 outColor;

// FSR_RCAS_LIMIT: the strongest negative lobe that cannot clip
const float RcasLimit = 0.25 - 1.0/16.0;

float max3(vec3 c) { return max(c.r, max(c.g, c.b)); }
float min3(vec3 c) { return min(c.r, min(c.g, c.b)); }

vec3 toUnit(vec3 c)   { return c/(1.0 + max3(c)); }
vec3 fromUnit(vec3 c) { return c/max(1.0 - max3(c), 1e-4); }

vec3 fetch(ivec2 at, ivec2 size) {
  return toUnit(max(texelFetch(src, clamp(at, ivec2(0), size-1), 0).rgb, vec3(0)));
  }

void main() {
  const ivec2 size = textureSize(src, 0);
  const ivec2 pix  = ivec2(gl_FragCoord.xy);
  const vec3  raw  = max(texelFetch(src, pix, 0).rgb, vec3(0));
  if(push.sharpness<=0.0) {
    outColor = vec4(raw, 1);
    return;
    }

  //    b
  //  d e f
  //    h
  const vec3 b = fetch(pix + ivec2( 0,-1), size);
  const vec3 d = fetch(pix + ivec2(-1, 0), size);
  const vec3 e = toUnit(raw);
  const vec3 f = fetch(pix + ivec2( 1, 0), size);
  const vec3 h = fetch(pix + ivec2( 0, 1), size);

  const vec3 mn4 = min(min(b, d), min(f, h));
  const vec3 mx4 = max(max(b, d), max(f, h));

  // the lobe that keeps the result inside the cross' range, per channel; the most limiting one wins
  const vec3 hitMin = min(mn4, e)/(4.0*mx4 + 1e-5);
  const vec3 hitMax = (1.0 - max(mx4, e))/(4.0*mn4 - 4.0 - 1e-5);
  const vec3 lobeC  = max(-hitMin, hitMax);
  const float lobe  = max(-RcasLimit, min(max3(lobeC), 0.0))*push.sharpness;

  const vec3 c = (lobe*(b + d + f + h) + e)/(4.0*lobe + 1.0);
  outColor = vec4(fromUnit(clamp(c, vec3(0), vec3(0.9999))), 1);
  }
