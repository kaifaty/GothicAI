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
