#pragma once

#include <Tempest/Matrix4x4>
#include <Tempest/Vec>

#include <cstdint>
#include <vector>

#include "leanaccel.h"

class Skeleton;

// Lean: a runner leans into a turn, the trunk leans forward up a slope and a bit back down one, and with
// the body's own acceleration: forward as it speeds up, back as it brakes (LeanAccel).
// Runs as a post-pass over the world-space bone matrices that Pose builds, before FootIk (which then
// puts the feet on the ground). Speed, turn and slope come from how the npc moves, smoothed.
class Lean final {
  public:
    struct Input final {
      bool          active   = false;   // walking, running on the ground
      bool          grounded = false;   // or standing on it: a stop still brakes after the run has ended
      Tempest::Vec3 position = {};      // of the npc, on the ground
      Tempest::Vec3 forward  = {1,0,0}; // horizontal facing of the body
      uint64_t      dt       = 0;
      };

    // State for the test bench, degrees, cm/s
    struct Probe final {
      bool  valid  = false;
      bool  active = false;
      float roll   = 0;  // into the turn, + to the left of the facing
      float pitch  = 0;  // of the trunk, + forward
      float speed  = 0;
      float turn   = 0;  // degrees per second, + to the left
      float slope  = 0;  // along the facing, + uphill
      float accel  = 0;  // cm/s^2 along the facing, smoothed
      };

    static bool    isEnabled();
    static void    setEnabled(bool e);

    void           setSkeleton(const Skeleton* sk);
    bool           isValid() const { return valid; }

    // Moves the smoothed state toward the input; true if the pose has to be rebuilt
    bool           update(const Input& in);
    void           apply(Tempest::Matrix4x4* tr, size_t numBones, const Tempest::Vec3& ground);
    void           probe(Probe& p) const;

    static Probe       playerProbe;   // filled every frame for the hero
    static Probe       subjectProbe;  // and for the npc the test bench watches
    static const void* probeNpc;

  private:
    size_t              spine = size_t(-1), lfoot = size_t(-1), rfoot = size_t(-1);
    std::vector<size_t> spineTree, lfootTree, rfootTree;
    bool                valid  = false;

    bool                moving = false; // the last update had a position
    Tempest::Vec3       prevPos = {};
    Tempest::Vec3       forward = {1,0,0};
    float               speed  = 0;     // cm/s, smoothed
    float               turn   = 0;     // rad/s, + to the left, smoothed
    float               slope  = 0;     // smoothed
    LeanAccel::State    accel;
    float               roll   = 0;     // radians
    float               pitch  = 0;
  };
