#version 450

#extension GL_ARB_separate_shader_objects : enable
#extension GL_EXT_control_flow_attributes : enable

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

layout(location = 0) out vec4 outHistory;
layout(location = 1) out vec4 outScene;

const float VarianceGamma = 1.0;

vec3 rgb2ycocg(vec3 c) {
  return vec3( 0.25*c.r + 0.5*c.g + 0.25*c.b,
               0.5 *c.r            - 0.5 *c.b,
              -0.25*c.r + 0.5*c.g - 0.25*c.b);
  }

vec3 ycocg2rgb(vec3 c) {
  return vec3(c.x + c.y - c.z,
              c.x       + c.z,
              c.x - c.y - c.z);
  }

// 5 bilinear taps of the 4x4 Catmull-Rom (the corner taps dropped, the weights renormalized)
vec3 sampleCatmullRom(vec2 uv, vec2 size) {
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

  const float k0 = w12.x*w0.y;
  const float k1 = w0.x *w12.y;
  const float k2 = w12.x*w12.y;
  const float k3 = w3.x *w12.y;
  const float k4 = w12.x*w3.y;

  vec3 c = textureLod(history, vec2(tc12.x, tc0.y ), 0).rgb*k0
         + textureLod(history, vec2(tc0.x,  tc12.y), 0).rgb*k1
         + textureLod(history, tc12,                 0).rgb*k2
         + textureLod(history, vec2(tc3.x,  tc12.y), 0).rgb*k3
         + textureLod(history, vec2(tc12.x, tc3.y ), 0).rgb*k4;
  return max(c/(k0+k1+k2+k3+k4), vec3(0));
  }

// from the box centre towards `p` until the box edge (Karis 2014)
vec3 clipToBox(vec3 boxMin, vec3 boxMax, vec3 p) {
  const vec3  c = 0.5*(boxMax + boxMin);
  const vec3  e = 0.5*(boxMax - boxMin) + 1e-5;
  const vec3  v = p - c;
  const vec3  a = abs(v/e);
  const float m = max(a.x, max(a.y, a.z));
  return m>1.0 ? c + v/m : p;
  }

void main() {
  const ivec2 size = textureSize(current, 0);
  const ivec2 pix  = ivec2(gl_FragCoord.xy);
  const vec3  cur  = texelFetch(current, pix, 0).rgb;

  if(push.reset!=0) {
    outHistory = vec4(cur, 1);
    outScene   = vec4(cur, 1);
    return;
    }

  // neighbourhood of this frame: the colour box and the nearest depth (its vector carries the edges)
  vec3  m1 = vec3(0), m2 = vec3(0);
  vec3  nMin = vec3( 1e30), nMax = vec3(-1e30);
  float zNear = 2.0;
  ivec2 zPix  = pix;
  [[unroll]]
  for(int y=-1; y<=1; ++y) {
    [[unroll]]
    for(int x=-1; x<=1; ++x) {
      const ivec2 at = clamp(pix + ivec2(x,y), ivec2(0), size-1);
      const vec3  c  = rgb2ycocg(texelFetch(current, at, 0).rgb);
      m1  += c;
      m2  += c*c;
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
    outScene   = vec4(cur, 1);
    return;
    }

  vec3 hist = sampleCatmullRom(histUv, sizeF);
  if(any(isnan(hist)) || any(isinf(hist)))
    hist = cur;

  // variance clipping (Salvi 2016), inside the min-max box of the neighbourhood
  const vec3 mean  = m1/9.0;
  const vec3 sigma = sqrt(max(m2/9.0 - mean*mean, vec3(0)));
  const vec3 bMin  = max(mean - VarianceGamma*sigma, nMin);
  const vec3 bMax  = min(mean + VarianceGamma*sigma, nMax);
  const vec3 histY = clipToBox(bMin, bMax, rgb2ycocg(hist));
  hist = max(ycocg2rgb(histY), vec3(0));

  // weights by inverse luminance: one bright spark does not burn into the history
  const float wCur  = push.alpha      /(1.0 + rgb2ycocg(cur).x);
  const float wHist = (1.0-push.alpha)/(1.0 + histY.x);
  const vec3  res   = (cur*wCur + hist*wHist)/(wCur + wHist);

  outHistory = vec4(res, 1);
  outScene   = vec4(res, 1);
  }
