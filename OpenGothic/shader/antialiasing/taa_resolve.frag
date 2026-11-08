#version 450

#extension GL_ARB_separate_shader_objects : enable
#extension GL_EXT_control_flow_attributes : enable

// FP16, Metal only (taa_resolve_h, Shaders): colours in half, MSL `half`; Vulkan has no shaderFloat16 here and takes
// the float build. Depth, uv, reprojection, the moments and the final blend stay float: E[c^2]-mean^2 cancels, and in
// half it coarsens sigma on flat areas (reports/engine/2026-11-05-shader-compile).
#if defined(FP16)
#extension GL_EXT_shader_explicit_arithmetic_types_float16 : require
#define hfloat float16_t
#define hvec3  f16vec3
const float BoxEps = 1e-4;  // 1e-5 is below half's normal range
const float BoxFar = 60000; // 1e30 is past it
#else
#define hfloat float
#define hvec3  vec3
const float BoxEps = 1e-5;
const float BoxFar = 1e30;
#endif

// Temporal AA by the camera (docs/graphics-plan.md, step 1a; reports/render-features/2026-11-03-taa):
// the history moves with the camera over the depth of this frame, read by Catmull-Rom, clipped to the
// variance box of the 3x3 neighbourhood in YCoCg and blended with weights by inverse luminance.
// Objects that move by themselves have no vectors yet (1b): the clip holds back their trails.

layout(push_constant, std430) uniform Push {
  mat4  reproject; // this frame's NDC and depth -> the history's NDC, both with this frame's jitter
  float alpha;     // the share of the new frame
  uint  reset;     // no history: the frame as it is
  } push;

layout(binding = 0) uniform sampler2D current; // linear HDR, pre-exposed
layout(binding = 1) uniform sampler2D depth;
layout(binding = 2) uniform sampler2D history; // bilinear

layout(location = 0) out vec4 outHistory; // the sharpening pass makes the scene from it (taa_sharpen.frag)

const float VarianceGamma = 1.0;

const hfloat H025 = hfloat(0.25);
const hfloat H05  = hfloat(0.5);

hvec3 rgb2ycocg(hvec3 c) {
  return hvec3( H025*c.r + H05*c.g + H025*c.b,
                H05 *c.r           - H05 *c.b,
               -H025*c.r + H05*c.g - H025*c.b);
  }

hvec3 ycocg2rgb(hvec3 c) {
  return hvec3(c.x + c.y - c.z,
               c.x       + c.z,
               c.x - c.y - c.z);
  }

// 5 bilinear taps of the 4x4 Catmull-Rom (the corner taps dropped, the weights renormalized)
hvec3 sampleCatmullRom(vec2 uv, vec2 size) {
  const vec2 pos  = uv*size;
  const vec2 pos1 = floor(pos - 0.5) + 0.5;
  const vec2 f    = pos - pos1;

  const vec2 w0  = f*(-0.5 + f*(1.0 - 0.5*f));
  const vec2 w1  = 1.0 + f*f*(-2.5 + 1.5*f);
  const vec2 w2  = f*(0.5 + f*(2.0 - 1.5*f));
  const vec2 w3  = f*f*(-0.5 + 0.5*f);
  const vec2 w12 = w1 + w2;

  const vec2 tc0  = (pos1 - 1.0)/size;
  const vec2 tc3  = (pos1 + 2.0)/size;
  const vec2 tc12 = (pos1 + w2/w12)/size;

  const float  ks = w12.x*w0.y + w0.x*w12.y + w12.x*w12.y + w3.x*w12.y + w12.x*w3.y;
#if defined(FP16)
  const float  kn = 1.0/ks; // normalized before the sum: the half sum stays near the colour
#else
  const float  kn = 1.0;
#endif
  const hfloat k0 = hfloat(w12.x*w0.y *kn);
  const hfloat k1 = hfloat(w0.x *w12.y*kn);
  const hfloat k2 = hfloat(w12.x*w12.y*kn);
  const hfloat k3 = hfloat(w3.x *w12.y*kn);
  const hfloat k4 = hfloat(w12.x*w3.y *kn);

  hvec3 c = hvec3(textureLod(history, vec2(tc12.x, tc0.y ), 0).rgb)*k0
          + hvec3(textureLod(history, vec2(tc0.x,  tc12.y), 0).rgb)*k1
          + hvec3(textureLod(history, tc12,                 0).rgb)*k2
          + hvec3(textureLod(history, vec2(tc3.x,  tc12.y), 0).rgb)*k3
          + hvec3(textureLod(history, vec2(tc12.x, tc3.y ), 0).rgb)*k4;
#if defined(FP16)
  return max(c, hvec3(0));
#else
  return max(c/ks, hvec3(0));
#endif
  }

// from the box centre towards `p` until the box edge (Karis 2014)
hvec3 clipToBox(hvec3 boxMin, hvec3 boxMax, hvec3 p) {
  const hvec3  c = H05*(boxMax + boxMin);
  const hvec3  e = H05*(boxMax - boxMin) + hfloat(BoxEps);
  const hvec3  v = p - c;
  const hvec3  a = abs(v/e);
  const hfloat m = max(a.x, max(a.y, a.z));
  return m>hfloat(1.0) ? c + v/m : p;
  }

void main() {
  const ivec2 size = textureSize(current, 0);
  const ivec2 pix  = ivec2(gl_FragCoord.xy);
  const vec3  cur  = texelFetch(current, pix, 0).rgb;

  if(push.reset!=0) {
    outHistory = vec4(cur, 1);
    return;
    }

  // neighbourhood of this frame: the colour box and the nearest depth (its vector carries the edges)
  vec3  m1 = vec3(0), m2 = vec3(0);
  hvec3 nMin = hvec3( BoxFar), nMax = hvec3(-BoxFar);
  float zNear = 2.0;
  ivec2 zPix  = pix;
  [[unroll]]
  for(int y=-1; y<=1; ++y) {
    [[unroll]]
    for(int x=-1; x<=1; ++x) {
      const ivec2 at = clamp(pix + ivec2(x,y), ivec2(0), size-1);
      const hvec3 c  = rgb2ycocg(hvec3(texelFetch(current, at, 0).rgb));
      const vec3  cf = vec3(c);
      m1  += cf;
      m2  += cf*cf;
      nMin = min(nMin, c);
      nMax = max(nMax, c);
      const float z = texelFetch(depth, at, 0).r;
      if(z<zNear) {
        zNear = z;
        zPix  = at;
        }
      }
    }

  // static world: the motion of the nearest point by the camera alone
  const vec2  sizeF  = vec2(size);
  const vec2  ndcZ   = ((vec2(zPix) + 0.5)/sizeF)*2.0 - 1.0;
  const vec4  prev4  = push.reproject*vec4(ndcZ, zNear, 1.0);
  const vec2  uv     = (vec2(pix) + 0.5)/sizeF;
  vec2        histUv = vec2(-1);
  if(prev4.w>0.0)
    histUv = uv + (prev4.xy/prev4.w - ndcZ)*0.5;

  // the history from beyond the screen: this frame only
  if(any(lessThan(histUv, vec2(0))) || any(greaterThan(histUv, vec2(1)))) {
    outHistory = vec4(cur, 1);
    return;
    }

  hvec3 hist = sampleCatmullRom(histUv, sizeF);
  if(any(isnan(hist)) || any(isinf(hist)))
    hist = hvec3(cur);

  // variance clipping (Salvi 2016), inside the min-max box of the neighbourhood
  const vec3  mean  = m1/9.0;
  const vec3  sigma = sqrt(max(m2/9.0 - mean*mean, vec3(0)));
  const hvec3 bMin  = max(hvec3(mean - VarianceGamma*sigma), nMin);
  const hvec3 bMax  = min(hvec3(mean + VarianceGamma*sigma), nMax);
  const hvec3 histY = clipToBox(bMin, bMax, rgb2ycocg(hist));
  hist = max(ycocg2rgb(histY), hvec3(0));

  // weights by inverse luminance: one bright spark does not burn into the history
  const float wCur  = push.alpha      /(1.0 + float(rgb2ycocg(hvec3(cur)).x));
  const float wHist = (1.0-push.alpha)/(1.0 + float(histY.x));
  const vec3  res   = (cur*wCur + vec3(hist)*wHist)/(wCur + wHist);

  outHistory = vec4(res, 1);
  }
