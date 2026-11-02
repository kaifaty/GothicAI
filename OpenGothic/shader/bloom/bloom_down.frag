#version 450
#extension GL_ARB_separate_shader_objects : enable

// Bloom, one step down the chain: the 13-tap filter of Jimenez (Next Generation Post Processing in Call of Duty:
// Advanced Warfare, SIGGRAPH 2014). KARIS: the first step from the scene weighs each 2x2 group by 1/(1+luma),
// so a single hot pixel (a glint on a wet stone) does not pulse as a blob.
layout(binding = 0) uniform sampler2D src;

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
#else
  const vec3 r = (d+e+i+j)*0.125 + (a+b+f+g + b+c+g+h + f+g+k+l + g+h+l+m)*0.03125;
#endif
  outColor = vec4(r, 1.0);
  }
