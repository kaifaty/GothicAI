#pragma once

#include <Tempest/Matrix4x4>
#include <Tempest/Vec>

#include <cstdint>

// Temporal antialiasing by the camera (graphics plan, step 1a; reports/render-features/2026-11-03-taa):
// the jitter sequence, the reprojection of the history and the camera jump rule. CPU only, no device.
namespace Taa {
  constexpr uint32_t Phases       = 8;
  constexpr float    JumpDistance = 500.f; // cm a frame: farther is a cut, the history starts over
  constexpr float    JumpAngle    = 30.f;  // degrees a frame

  // radical inverse of `index` in `base` (Halton), in [0,1)
  float              halton(uint32_t index, uint32_t base);
  // the sub-pixel offset of frame `frame`: Halton (2, 3) of `phases` phases (8 for TAA), pixels of the internal
  // resolution, within ±0.5
  Tempest::Vec2      jitterPx(uint32_t frame, uint32_t phases = Phases);
  // a temporal upscale from `in` to `out` pixels (graphics plan 1г): Apple's ⌈8·(out/in)²⌉ phases, by the area
  uint32_t           upscalePhases(uint32_t inW, uint32_t inH, uint32_t outW, uint32_t outH);
  // and its mip bias of the scene's textures: log2(in/out) − 1, by the width
  float              upscaleMipBias(uint32_t inW, uint32_t outW);
  // Our own temporal upscale (taau_resolve.frag): the input texel that holds the centre of output pixel (outX, outY)
  // in a frame moved by `jitter` input pixels, and how far that texel's sample of the scene lies from the centre,
  // in input pixels (within ±0.5 away from the frame's edge)
  struct UpscaleTap {
    int32_t       x = 0, y = 0;
    Tempest::Vec2 offset;
    };
  UpscaleTap         upscaleTap(uint32_t outX, uint32_t outY, uint32_t outW, uint32_t outH,
                                uint32_t inW, uint32_t inH, const Tempest::Vec2& jitter);
  // the share of the new frame's alpha such a sample gets: exp(-kernel*d²), d its offset in output pixels
  float              upscaleConfidence(const Tempest::Vec2& offset, float scaleX, float scaleY, float kernel);
  // The resolve's accumulation (taau_resolve.frag): `conf` this frame's sample weight, `gathered` the weight in the
  // history's alpha, `wMax` its cap, `moved` how far the colour box moved the history in box extents, `mask` this
  // frame's reactive mask under the ceiling. The new frame's share is conf/(conf + gathered). A mask from
  // 1/UpscaleMaskSmooth makes it a full history's 1/(1 + wMax) whatever the sample's distance, so that the output
  // pixels of one input texel take what changes every frame alike, and it grows to the whole with the mask; the
  // history then keeps min(gathered + conf, wMax)*(1 - mask*UpscaleMaskSmooth), never less than UpscaleWeightInit
  constexpr float    UpscaleWeightInit = 0.05f;
  constexpr float    UpscaleMaskSmooth = 8.f;
  struct UpscaleShare {
    float share  = 0; // of the new frame
    float weight = 0; // the history's alpha after this frame
    };
  UpscaleShare       upscaleShare(float conf, float gathered, float wMax, float moved, float clipTrust, float mask);
  // In motion the history is read between its pixels every frame, and each reading softens it; a pixel far from
  // this frame's sample would keep that softened history for many frames. So from `moveSpeed` output pixels a frame
  // the new frame is its enlargement at every pixel, with one weight for all of them, `upscaleMoveConf`: the one that
  // gives it `share` against a history of `wMax`. Below that speed the two are mixed by `move` = speed/moveSpeed
  // A history in motion is an average of enlarged frames, which the samples at the pixels' centres should replace
  // soon after a stop: it keeps no more than the resolve's `moveWeight`, and `upscaleMoveConf` is taken against that
  float              upscaleMoveConf(float share, float wMax);
  float              upscaleConfInMotion(float conf, float moveConf, float move);
  // The history's cubic in full motion (Keys' -a) by the enlargement: `sharp` from 2 times by the width, Catmull-Rom's
  // 0.5 up to 1.5 times, where the enlarged frame brings more of the detail itself and deeper lobes overdo it
  // (cobbles under a walking camera at 67 %: 1.13 of the full resolution's sharpness with 0.75, 1.01 with 0.5)
  float              upscaleHistSharp(float sharp, uint32_t inW, uint32_t outW);
  // The share of the frame the scene is drawn at by [INTERNAL] vidResIndex: 0 the whole, 2 and more a half, 1 the
  // step between: 3/4 for the spatial upscale and 2/3 for our own temporal one, whose look was accepted at 2/3
  float              internalScale(float vidResIndex, bool taau);
  // The depth test of our own upscale (taau_prepare.frag). A perspective's depth is a + b/w of the view depth w, so
  // 1/w is in proportion to (1 - depth) + (a - 1): `upscaleDepthOffset` is that a - 1, n/(f - n); 0 with no
  // perspective. `upscaleHidden`: how much the previous frame hid a point whose 1/w was `invWExpected` then, where
  // that frame kept `invWStored`: 0 up to `tol` nearer (in shares of the depth), 1 from twice that
  float              upscaleDepthOffset(const Tempest::Matrix4x4& proj);
  float              upscaleHidden(float invWExpected, float invWStored, float tol);
  // the clip-space translation that moves the picture by `px` pixels of a w×h target: J of J*proj
  Tempest::Matrix4x4 jitterMatrix(const Tempest::Vec2& px, uint32_t w, uint32_t h);

  // a camera without jitter: proj, view from the camera (large-world coordinates: no translation) and its position
  struct View {
    Tempest::Matrix4x4 proj;
    Tempest::Matrix4x4 viewLwc;
    Tempest::Vec3      origin;
    };

  // This frame's jittered NDC (x, y, depth) -> where the same static point lies in the history, in the same
  // jittered NDC: J*prev.proj*prev.viewLwc * T(cur.origin-prev.origin) * (J*cur.proj*cur.viewLwc)^-1.
  // Identity for a camera that stands still, wherever it is in the world.
  Tempest::Matrix4x4 reprojection(const View& cur, const View& prev, const Tempest::Matrix4x4& jitter);
  // Motion vectors of objects (graphics plan 1b, reports/render-features/2026-11-10-motion-vectors): the previous
  // camera with this frame's jitter, J*prev.proj*prev.viewLwc. Only its linear part is meant to be used, on a
  // world-space offset vec4(d,0): the scene's viewProjectPrev
  Tempest::Matrix4x4 viewProjectPrev(const View& prev, const Tempest::Matrix4x4& jitter);
  // The scene's pair: reprojectClip = reprojection(cur, prev, jitter) and viewProjectPrev(prev, jitter); on a cut or
  // the first frame (`continuous` false) identity and this frame's camera, so that only an object's own move counts
  struct MotionCamera {
    Tempest::Matrix4x4 reprojectClip;
    Tempest::Matrix4x4 viewProjectPrev;
    };
  MotionCamera       motionCamera(const View& cur, const View& prev, bool continuous, const Tempest::Matrix4x4& jitter);
  // The residual the G-buffer stores for a point at clip position `clipCur` of this frame that moved by `d` (world,
  // previous - current) since the last one: where it was, in texture coordinates, minus where it would have been
  // had it stood still. The shaders' formula (main.vert, main.frag), 0 for d=0
  Tempest::Vec2      motionResidual(const Tempest::Matrix4x4& reprojectClip, const Tempest::Matrix4x4& viewProjectPrev,
                                    const Tempest::Vec4& clipCur, const Tempest::Vec3& d);
  // The resolve's blend (taa_resolve.frag) with the reactive mask (graphics plan 1v,
  // reports/render-features/2026-11-10-taa-reactive): `alpha` the new frame's share, `lumCur` and `lumHist` the
  // luminance (YCoCg Y) of this frame and of the clipped history, `mask` this frame's mask under the ceiling and
  // `maskPrev` the last frame's from the history's alpha. k = max(mask, maskPrev) lifts the share to mix(alpha, 1, k)
  // and fades the weights by inverse luminance by (1 - k); with k = 0 they are the weights without a mask
  struct ResolveWeights {
    float cur  = 0;
    float hist = 0;
    };
  ResolveWeights     resolveWeights(float alpha, float lumCur, float lumHist, float mask, float maskPrev);
  // the resolved value of one channel by those weights
  float              resolveBlend(const ResolveWeights& w, float cur, float hist);
  // the frame moved farther than JumpDistance or turned more than JumpAngle
  bool               isCameraJump(const View& cur, const View& prev);
  // degrees between the two views' rotations
  float              rotationAngle(const Tempest::Matrix4x4& viewLwcA, const Tempest::Matrix4x4& viewLwcB);
  }
