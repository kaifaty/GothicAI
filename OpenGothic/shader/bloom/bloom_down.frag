#version 450
#extension GL_ARB_separate_shader_objects : enable

// Bloom, one step down the chain: the 13-tap filter of Jimenez (Next Generation Post Processing in Call of Duty:
// Advanced Warfare, SIGGRAPH 2014). KARIS: the first step from the scene weighs each 2x2 group by 1/(1+luma),
// so a single hot pixel (a glint on a wet stone) does not pulse as a blob.
// The glare (KARIS, the first step): what is brighter than the tone curve can show, a flame's core, gives the chain
// more than its share, as the glow an eye or a lens puts around a small bright source. The weight goes from 1 at
// glareFrom to 1 + glareBoost at glareTo, by the brightest channel (a flame is red, its luminance is low).
layout(binding = 0) uniform sampler2D src;

#if defined(KARIS)
layout(push_constant, std430) uniform Push {
  float glareFrom;
  float glareInvRange; // 1/(to - from)
  float glareBoost;    // 0: off
  } push;
#endif

layout(location = 0) in  vec2 UV;
layout(location = 0) out vec4 outColor;

vec3 tap(vec2 o) {
  return textureLod(src, UV + o/vec2(textureSize(src,0)), 0).rgb;
  }

#if defined(KARIS)
vec3 group(vec3 a, vec3 b, vec3 c, vec3 d, float w, inout float sum) {
  const vec3  g = (a+b+c+d)*0.25;
  const float k = w/(1.0 + dot(g, vec3(0.2126, 0.7152, 0.0722)));
  sum += k;
  return g*k;
  }
#endif

void main() {
  const vec3 a = tap(vec2(-2,-2)), b = tap(vec2(0,-2)), c = tap(vec2(2,-2));
  const vec3 d = tap(vec2(-1,-1)), e = tap(vec2(1,-1));
  const vec3 f = tap(vec2(-2, 0)), g = tap(vec2(0, 0)), h = tap(vec2(2, 0));
  const vec3 i = tap(vec2(-1, 1)), j = tap(vec2(1, 1));
  const vec3 k = tap(vec2(-2, 2)), l = tap(vec2(0, 2)), m = tap(vec2(2, 2));
#if defined(KARIS)
  float sum = 0.0;
  vec3  r   = group(d,e,i,j,0.5,sum) + group(a,b,f,g,0.125,sum) + group(b,c,g,h,0.125,sum) +
              group(f,g,k,l,0.125,sum) + group(g,h,l,m,0.125,sum);
  r /= max(sum, 1e-6);
  const float peak = max(r.r, max(r.g, r.b));
  const float w    = clamp((peak-push.glareFrom)*push.glareInvRange, 0.0, 1.0);
  r *= 1.0 + push.glareBoost*w*w*(3.0-2.0*w);
#else
  const vec3 r = (d+e+i+j)*0.125 + (a+b+f+g + b+c+g+h + f+g+k+l + g+h+l+m)*0.03125;
#endif
  outColor = vec4(r, 1.0);
  }
