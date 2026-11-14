#version 450

#extension GL_ARB_separate_shader_objects : enable
#extension GL_EXT_control_flow_attributes : enable

// FP16, Metal only (taau_resolve_h, Shaders), as taa_resolve.frag: colours in half, the blend in float
#if defined(FP16)
#extension GL_EXT_shader_explicit_arithmetic_types_float16 : require
#define hfloat float16_t
#define hvec3  f16vec3
const float BoxEps = 1e-4;
#else
#define hfloat float
#define hvec3  vec3
const float BoxEps = 1e-5;
#endif

// Our own temporal upscale (graphics plan 1g; reports/render-features/2026-11-05-upscale/step-1g-1-taau.md): the
// resolve of taa_resolve.frag with the history at the output resolution. The scene is drawn smaller, with a longer
// jitter sequence (Taa::upscalePhases); every frame an output pixel takes the input sample nearest to its centre
// with the weight `conf` that falls with the distance to that sample, in output pixels. The history's alpha is the
// weight it has gathered, up to `wMax`: the new frame's share is conf/(conf + gathered), so a pixel with a fresh
// history takes the first sample near its centre whole and one with a full history a small share. Over the
// sequence each output pixel gets samples near its own centre, so the history converges to the full resolution in
// about one sequence.
// The colour box and the motion come from taau_prepare.frag, once per input texel; the history is read by
// Catmull-Rom and written at the output resolution.
// The nine input samples of the box do not span what the history has gathered between them (a joint narrower than
// an input pixel), so a box of their own size wears the detail down every frame: where the point stands still and
// the history has gathered a sample's weight the box is `boxStatic` times wider. A history that has gathered
// nothing (after a cut, where a flame has just been) has no such detail to keep and is clipped to the box as it is.
// Where the history is not trusted (reset, off the screen) the frame alone is enlarged by Catmull-Rom at the point
// the output pixel looks at: sharp, and free of the nearest sample's blocks.
// Under the reactive mask (a flame, smoke: they change every frame and have no motion vector) the resolve is the
// TAA's of that enlarged frame: the share by the distance to the sample would differ between the output pixels of
// one input texel, and what changes every frame would show it as a grid of dots two pixels apart.

layout(push_constant, std430) uniform Push {
  vec2  jitter;      // this frame's offset, input pixels: the picture is moved by +jitter (Taa::jitterMatrix)
  float wMax;        // the most weight a history keeps: a sample at the centre then takes 1/(1 + wMax)
  uint  reset;       // no history: the frame as it is
  float reactiveMax; // the mask's ceiling
  uint  reactive;    // 0: the mask is off and not read
  float kernel;      // exp(-kernel*d^2), d in output pixels
  float clipTrust;   // how fast a history that the colour box has moved loses its weight
  float boxStatic;   // the colour box's extent where nothing moves, times its own; 1 in motion
  float boxMotion;   // output pixels a frame from which the box is its own size
  } push;

layout(binding = 0) uniform sampler2D current;  // linear HDR, pre-exposed, input resolution, bilinear
layout(binding = 1) uniform sampler2D boxMid;   // taau_prepare.frag: the box's centre, YCoCg; a: motion x
layout(binding = 2) uniform sampler2D boxExt;   // the box's half extent, YCoCg; a: motion y
layout(binding = 3) uniform sampler2D history;  // bilinear, output resolution
layout(binding = 4) uniform sampler2D reactive; // the reactive mask of this frame, R8, input resolution

layout(location = 0) out vec4 outHistory; // the tonemapping sharpens the scene from it (tonemapping.frag RCAS)

const float WeightInit = 0.05; // the weight of a frame taken as it is: an enlargement, soon replaced
const float MaskSmooth = 8.0;  // the mask from 1/MaskSmooth takes the enlarged frame by one share for all pixels

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

// 5 bilinear taps of the 4x4 Catmull-Rom, as taa_resolve.frag; `pos` in texels of `tex`, `centerA` is the alpha of
// the centre tap
hvec3 sampleCatmullRom(sampler2D tex, vec2 pos, vec2 size, out float centerA) {
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
  const float  kn = 1.0/ks;
#else
  const float  kn = 1.0;
#endif
  const hfloat k0 = hfloat(w12.x*w0.y *kn);
  const hfloat k1 = hfloat(w0.x *w12.y*kn);
  const hfloat k2 = hfloat(w12.x*w12.y*kn);
  const hfloat k3 = hfloat(w3.x *w12.y*kn);
  const hfloat k4 = hfloat(w12.x*w3.y *kn);

  const vec4 h12 = textureLod(tex, tc12, 0);
  centerA = h12.a;
  hvec3 c = hvec3(textureLod(tex, vec2(tc12.x, tc0.y ), 0).rgb)*k0
          + hvec3(textureLod(tex, vec2(tc0.x,  tc12.y), 0).rgb)*k1
          + hvec3(h12.rgb)*k2
          + hvec3(textureLod(tex, vec2(tc3.x,  tc12.y), 0).rgb)*k3
          + hvec3(textureLod(tex, vec2(tc12.x, tc3.y ), 0).rgb)*k4;
#if defined(FP16)
  return max(c, hvec3(0));
#else
  return max(c/ks, hvec3(0));
#endif
  }

// from the box centre towards `p` until the box edge (Karis 2014)
hvec3 clipToBox(hvec3 c, hvec3 ext, hvec3 p) {
  const hvec3  e = ext + hfloat(BoxEps);
  const hvec3  v = p - c;
  const hvec3  a = abs(v/e);
  const hfloat m = max(a.x, max(a.y, a.z));
  return m>hfloat(1.0) ? c + v/m : p;
  }

void main() {
  const ivec2 inSizeI = textureSize(current, 0);
  const vec2  inSize  = vec2(inSizeI);
  const vec2  outSize = vec2(textureSize(history, 0));
  const vec2  uv      = gl_FragCoord.xy/outSize;
  const vec2  scale   = outSize/inSize;

  // The picture is moved by +jitter, so the point at this pixel's centre lies at uv*inSize + jitter of the input,
  // and the input texel that holds it sampled the scene `d0` input pixels away from that centre (Taa::upscaleTap)
  const vec2  pos = uv*inSize + push.jitter;
  const ivec2 c   = clamp(ivec2(floor(pos)), ivec2(0), inSizeI-1);
  const vec2  d0  = (vec2(c) + 0.5) - pos;
  const vec2  dC  = d0*scale;

  // the motion of the nearest point of the texel's neighbourhood: the camera's and the object's own
  const vec4  bm     = texelFetch(boxMid, c, 0);
  const vec4  be     = texelFetch(boxExt, c, 0);
  const vec2  mv     = vec2(bm.a, be.a);
  const vec2  histUv = uv + mv;
  // this frame's mask under the ceiling
  const float k      = push.reactive!=0 ? min(texelFetch(reactive, c, 0).r, push.reactiveMax) : 0.0;

  const bool  alone  = push.reset!=0 || any(lessThan(histUv, vec2(0))) || any(greaterThan(histUv, vec2(1)));
  float unused = 0.0;
  if(alone) {
    outHistory = vec4(sampleCatmullRom(current, pos, inSize, unused), WeightInit);
    return;
    }

  const float conf = exp(-push.kernel*dot(dC,dC)); // 1: a sample at this pixel's centre
  const float kS   = clamp(k*MaskSmooth, 0.0, 1.0);
  vec3        cur  = texelFetch(current, c, 0).rgb;
  if(kS>0.0)
    cur = mix(cur, vec3(sampleCatmullRom(current, pos, inSize, unused)), kS);

  float histA = 0.0;
  hvec3 hist  = sampleCatmullRom(history, histUv*outSize, outSize, histA);
  if(any(isnan(hist)) || any(isinf(hist)))
    hist = hvec3(cur);

  const float still  = 1.0 - clamp(length(mv*outSize)/push.boxMotion, 0.0, 1.0);
  const float held   = clamp((histA - WeightInit)/(1.0 - WeightInit), 0.0, 1.0);
  const hvec3 bMid   = hvec3(bm.rgb);
  const hvec3 bExt   = hvec3(be.rgb)*hfloat(mix(1.0, push.boxStatic, still*held));
  const hvec3 histY0 = rgb2ycocg(hist);
  const hvec3 histY  = clipToBox(bMid, bExt, histY0);
  hist = max(ycocg2rgb(histY), hvec3(0));

  // The weight the history has gathered; one that the box has moved is not this point's any more and loses it
  const float moved = length(vec3(histY - histY0))/(length(vec3(bExt)) + 1e-3);
  const float wH    = min(max(histA, 0.0), push.wMax)/(1.0 + push.clipTrust*moved);

  // The new frame's share by the weights. Under the mask it is a full history's 1/(1 + wMax) for every pixel and
  // grows to the whole with the mask, the weights by luminance fade and the frame is the enlarged one. The mask
  // also empties the gathered weight: a pixel the flame has left takes the next frames at once (Taa::upscaleShare)
  const float aAcc  = conf/(conf + wH + 1e-4);
  const float a     = mix(mix(aAcc, 1.0/(1.0 + push.wMax), kS), 1.0, k);
  const float wCur  = a      /(1.0 + (1.0-k)*float(rgb2ycocg(hvec3(cur)).x));
  const float wHist = (1.0-a)/(1.0 + (1.0-k)*float(histY.x));
  const vec3  res   = (cur*wCur + vec3(hist)*wHist)/max(wCur + wHist, 1e-6);

  outHistory = vec4(res, max(min(wH + conf, push.wMax)*(1.0-kS), WeightInit));
  }
