#pragma once

#include <Tempest/Vec>
#include <cstdint>

// Motion of own particles (particles.d "curl", "drag") and the flame's flicker that drives an emitter and its lamp
// (vobs.d "flicker", "flicker_from"): reports/content/remaster/wall-torch/README.md, items 1 and 4 of "Способ".
// Engine units: cm and ms.
namespace PfxMotion {

struct Params {
  float curlStrength = 0;  // rms acceleration of the curl field, cm/ms^2 (particles.d m/s^2 * 1e-4)
  float curlWave     = 0;  // wave number, rad/cm (2*pi * periods per metre / 100)
  float curlDrift    = 0;  // the field's drift, cm/ms (m/s * 0.1)
  float drag         = 0;  // 1/ms (particles.d 1/s * 1e-3): velocity decays towards zero, the world has no wind

  bool active() const { return curlStrength>0 || drag>0; }
  };

// The longest step of the motion: a longer frame is split (a frame at 50..144 fps takes one substep), so the path
// does not depend on the frame rate
inline constexpr float MaxSubstepMs = 20.f;

// A divergence-free field: the curl of a sum of plane waves of a vector potential over six fixed directions
// (icosahedron axes), each at its own multiple of the wave number; rms magnitude curlStrength
Tempest::Vec3 curl(const Params& p, const Tempest::Vec3& at, float timeMs);

// Advances a particle by dt: gravity and the curl at the start of each substep act as a constant acceleration
// over it, drag is integrated exactly (stable at any drag and step). origin turns pos into world space for
// the field; timeMs is the time at the start of the step.
void step(const Params& p, const Tempest::Vec3& gravity, Tempest::Vec3& pos, Tempest::Vec3& vel,
          const Tempest::Vec3& origin, float timeMs, float dtMs);

struct Flicker {
  float    pulseHz  = 0;  // the flame's puffing
  float    jitterHz = 0;  // the tips' trembling
  float    amount   = 0;  // the largest deviation of the brightness from 1
  uint32_t seed     = 0;

  bool active() const { return amount>0; }
  };

// The brightness multiplier at a world time, 1 +- amount: smooth noise at the pulse (0.7) plus the trembling (0.3).
// The emitter and the lamp that names it call this with the same world tick and get the same value.
float         flicker(const Flicker& f, uint64_t timeMs);
// The lamp's trembling position: three noise channels at jitter_hz (pulse_hz without it), up to cm on each axis
Tempest::Vec3 jitter (const Flicker& f, uint64_t timeMs, float cm);
// OPENGOTHIC_FLICKER_LOG=1: the emitter and the lamp log their signal each frame, for a bench
bool          logEnabled();

} // namespace PfxMotion
