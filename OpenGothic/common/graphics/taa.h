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
  // the sub-pixel offset of frame `frame`: Halton (2, 3) of 8 phases, pixels of the internal resolution, within ±0.5
  Tempest::Vec2      jitterPx(uint32_t frame);
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
  // the frame moved farther than JumpDistance or turned more than JumpAngle
  bool               isCameraJump(const View& cur, const View& prev);
  // degrees between the two views' rotations
  float              rotationAngle(const Tempest::Matrix4x4& viewLwcA, const Tempest::Matrix4x4& viewLwcB);
  }
