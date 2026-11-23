#pragma once

#include <Tempest/Vec>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <utility>

// Step 4 of the texture levels, part C (docs/world-streaming-textures.md): an owner wants the sharper of its level now
// and its level at the closest point of the view's path over the next T seconds - the segment from the hero (and from
// the camera) along its velocity, so nothing behind the view is pulled in. Pure: no device, no Resources.
namespace TextureLookAhead {
  // The horizon T = HorizonFactor x p90 of the measured load latency (demand rose -> published), within
  // [0, MaxHorizon]. A load must start before the level is needed by at least its latency, and the owners are
  // evaluated once a frame, so the anticipated border is seen up to a frame late; the latency itself is about one frame
  // (the main thread sees a finished load in the next frame; the worker's own work is well under a millisecond). Twice
  // the p90 covers both. Measured on Ryzen with four loads in flight: p90 10-11 ms at 90 frames a second.
  constexpr double   HorizonFactor  = 2.0;
  // Seconds: the cap, against a latency that grows with a backlog (at most 10 m of path at 20 m/s)...
  constexpr double   MaxHorizon     = 0.5;
  // ...and T before the first window of latencies: about two frames at 30 frames a second.
  constexpr double   DefaultHorizon = 0.05;
  // The p90 is of the last WindowSamples loads, not of the session: a world load's backlog (seconds) must not hold T
  // at the cap for the rest of the game.
  constexpr uint64_t WindowSamples  = 32;
  // Seconds: the velocity is the move over the last Window, so a single uneven frame does not swing the path.
  constexpr double   Window         = 0.25;

  // T in seconds from the p90 of a window of load latencies in milliseconds and how many it holds.
  double horizon(double latencyP90Ms, uint64_t samples);

  // The velocity of a point from its positions over the last Window; a jump of the view (ChunkResidency::JumpDistance)
  // resets it, so a teleport does not count as velocity.
  class Velocity final {
    public:
      // `position` in centimetres at `seconds` of a monotonic clock
      void          add(const Tempest::Vec3& position, double seconds);
      void          reset() { history.clear(); }
      // centimetres a second; zero until two positions span a millisecond
      Tempest::Vec3 velocity() const;

    private:
      std::deque<std::pair<double,Tempest::Vec3>> history;
  };

  // Distance in metres from the segment [from, from+ahead] (centimetres) to the box [min, max] or to a sphere: the
  // closest point of the segment.
  double distance(const Tempest::Vec3& from, const Tempest::Vec3& ahead, const Tempest::Vec3& min, const Tempest::Vec3& max);
  double distance(const Tempest::Vec3& from, const Tempest::Vec3& ahead, const Tempest::Vec3& centre, float radius);
  }
