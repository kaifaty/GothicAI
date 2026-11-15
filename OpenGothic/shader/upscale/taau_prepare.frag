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
//    neighbourhood, YCoCg. The resolve widens it where the point stands still;
//  - whether the history of this texel is another surface's (FSR 2's depth clip, by the previous frame's depth
//    kept here instead of one rebuilt from this frame's): `hidden`, the share of the history's footprint where the
//    previous frame showed something nearer than this point was then, so the point was behind it (the ground a
//    walking leg has just left: its history is the boot); and the edge: a texel beside such a footprint, or one
//    that carries the motion of a nearer neighbour which moves otherwise than itself (the ground at a moving
//    figure's edge), whose history is its own surface from another place or with the other's edge in it. The
//    resolve keeps the box of both at its own size and takes the weight of a hidden one's history away. The
//    depths and the motion of this frame are kept for the next one's test.

layout(push_constant, std430) uniform Push {
  mat4  reproject; // this frame's NDC and depth -> the previous frame's NDC, both with this frame's jitter
  float gamma;     // the variance box in sigmas
  float depthOff;  // 1/w of the view depth is in proportion to (1 - z) + depthOff (Taa::upscaleDepthOffset)
  float depthTol;  // a history nearer than the point by more than this share of its depth hid it; 0: no test
  float pad0;
  } push;

layout(binding = 0) uniform sampler2D depth;
layout(binding = 1) uniform sampler2D motion;   // the objects' own motion (main.frag), same size as the depth
layout(binding = 2) uniform sampler2D current;  // linear HDR, pre-exposed, same size
layout(binding = 3) uniform sampler2D depthPrev; // outAux of the previous frame

layout(location = 0) out vec4 outBoxMid; // the box's centre, YCoCg; a: motion x
layout(location = 1) out vec4 outBoxExt; // the box's half extent, YCoCg; a: motion y
layout(location = 2) out vec4 outAux;    // for the next frame: r: 1 - the nearest depth of the 3x3; g: 1 - this texel's; ba: the motion
layout(location = 3) out vec2 outFlags;  // r: hidden; g: the edge

const float DynTol   = 0.05; // the tolerance of a point that moves by itself: its depth a frame ago is not known
const float BorrowPx = 0.5;  // input pixels between the borrowed motion and the texel's own from which it is another's

vec3 rgb2ycocg(vec3 c) {
  return vec3( 0.25*c.r + 0.5*c.g + 0.25*c.b,
               0.5 *c.r           - 0.5 *c.b,
              -0.25*c.r + 0.5*c.g - 0.25*c.b);
  }

void main() {
  const ivec2 size = textureSize(depth, 0);
  const ivec2 pix  = ivec2(gl_FragCoord.xy);

  float z    = 2.0, zFar = 0.0;
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
      zFar = max(zFar, zp);
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

  // the texel's own motion, where it took a neighbour's
  float borrowed = 0.0;
  if(at!=pix && prev.w>0.0) {
    const vec2 ndcOwn  = ((vec2(pix) + 0.5)/vec2(size))*2.0 - 1.0;
    const vec4 prevOwn = push.reproject*vec4(ndcOwn, texelFetch(depth, pix, 0).r, 1.0);
    if(prevOwn.w>0.0) {
      const vec2 mvOwn = (prevOwn.xy/prevOwn.w - ndcOwn)*0.5 + texelFetch(motion, pix, 0).xy;
      borrowed = clamp(length((mv - mvOwn)*vec2(size))/BorrowPx, 0.0, 1.0);
      }
    }

  // The previous frame's depth where the history is read, against the depth this point had then: the reprojection's
  // w is the previous view depth over this one. The neighbourhood's own spread of depths (a slope) adds to the
  // tolerance, as the previous frame was sampled up to a texel away.
  // First the nearest depth of the 3x3 around the history's texel, in one read: a texel beside what hid the point
  // had the hiding surface's smoothed edge in its history, thinner than a texel of the depth, so with something
  // nearer anywhere there the box is not widened. Only then, which is rare, the texels' own depths over the
  // history's footprint: `hidden`.
  // Only a surface that moved otherwise than this point has uncovered it: one that keeps its place beside the
  // point (a post against a wall, the camera still) is nearer in one frame's texel and not in the next's by the
  // jitter alone, so the previous frame's motion there has to differ from this texel's
  float hidden = 0.0, beside = 0.0;
  if(push.depthTol>0.0 && prev.w>0.0) {
    const float iwNear = (1.0 - z)    + push.depthOff;
    const float iwFar  = (1.0 - zFar) + push.depthOff;
    const float iwExp  = max(iwNear/prev.w, 1e-12);
    const bool  dyn    = any(notEqual(texelFetch(motion, at, 0).xy, vec2(0)));
    const float slope  = iwNear/max(iwFar, 1e-12) - 1.0;
    const float tol    = (dyn ? max(push.depthTol, DynTol) : push.depthTol) + slope;
    const float tolW   = tol + slope;
    const vec2  hp     = ((vec2(pix) + 0.5)/vec2(size) + mv)*vec2(size) - 0.5;
    const vec4  sW     = texelFetch(depthPrev, clamp(ivec2(floor(hp + 0.5)), ivec2(0), size-1), 0);
    beside = clamp(length((sW.ba - mv)*vec2(size))/BorrowPx, 0.0, 1.0)
            *clamp(((sW.r + push.depthOff)/iwExp - 1.0 - tolW)/max(tolW, 1e-6), 0.0, 1.0);
    if(beside>0.0) {
      const ivec2 h0 = ivec2(floor(hp));
      const vec2  hf = hp - vec2(h0);
      [[unroll]]
      for(int i=0; i<4; ++i) {
        const ivec2 o      = ivec2(i&1, i>>1);
        const vec4  s      = texelFetch(depthPrev, clamp(h0 + o, ivec2(0), size-1), 0);
        const float nearer = (s.g + push.depthOff)/iwExp - 1.0;
        const float other  = clamp(length((s.ba - mv)*vec2(size))/BorrowPx, 0.0, 1.0);
        const float w      = (o.x==0 ? 1.0 - hf.x : hf.x)*(o.y==0 ? 1.0 - hf.y : hf.y);
        hidden += w*other*clamp((nearer - tol)/max(tol, 1e-6), 0.0, 1.0);
        }
      }
    }

  const vec3  mean  = m1/9.0;
  const vec3  sigma = sqrt(max(m2/9.0 - mean*mean, vec3(0)));
  const vec3  vMin  = max(mean - push.gamma*sigma, nMin);
  const vec3  vMax  = min(mean + push.gamma*sigma, nMax);

  outBoxMid = vec4(0.5*(vMax + vMin), mv.x);
  outBoxExt = vec4(0.5*(vMax - vMin), mv.y);
  outAux    = vec4(1.0 - z, 1.0 - texelFetch(depth, pix, 0).r, mv);
  outFlags  = vec2(hidden, max(borrowed, beside));
  }
