#pragma once

#include <Tempest/Matrix4x4>
#include <Tempest/Vec>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "ikmath.h"

class Skeleton;

// The free hands to an obstacle (reports/physics/2026-11-15-reach-wall): a body about to run into a wall puts
// them out, the palms come to stand on it, the elbows fold as the body arrives; pressed into it, the body stays
// propped on them. Over the animation, after every other pass: the arm bent by two bones, the palm turned flat to
// the surface, the fingers opened. Where the surface is, the caller's rays say; here the point is held in the world
// once the palm is on it, so that the palm stands and does not slide.
class Reach final {
  public:
    static constexpr float Palm      = 8.f;   // cm from the wrist along the fingers to the palm's middle
    static constexpr float PalmThick = 2.5f;  // cm from the surface to the wrist's bone
    static constexpr float MaxReach  = 0.98f; // of the arm's length at most

    // What the look is tuned by (the bench's reach-form)
    struct Form final {
      float guard   = 0.90f; // of the arm's length: how far the hand goes out before the palm is on the surface
      float in      = 180.f; // ms the hands come in
      float out     = 220.f; // ms they go back to the animation
      float follow  = 60.f;  // ms: a surface that moves is followed this softly
      float wrist   = 75.f;  // degrees the hand bends back against the forearm at most
      float lean    = 20.f;  // degrees the fingers lean inwards off straight up
      float elbow   = 0.6f;  // how far the elbow goes out beside down (0: straight down)
      float side    = 12.f;  // cm the palm's point is out of the shoulder, sideways
      float rise    = 4.f;   // cm it is above the shoulder
      float palmFlip = 0.f;  // 1: the palm is the other side of the hand (the bone's frame is not what the bind says)
      };
    static Form& form();

    struct Target final {
      bool          found = false;
      bool          fixed = true;   // a surface that stands: the point is kept in the world
      Tempest::Vec3 point, normal;
      };

    struct Input final {
      uint64_t              dt     = 0;
      bool                  active = false;  // the hands are wanted
      Tempest::Vec3         toward;          // along the ground, into the obstacle
      std::array<bool,2>    free   = {};     // left, right
      std::array<Target,2>  target = {};
      };

    // State for the test bench
    struct Probe final {
      bool                 valid = false;
      std::array<float,2>  weight  = {};        // 0..1
      std::array<bool,2>   planted = {};        // the palm stands on its point
      std::array<float,2>  gap     = {};        // cm from the surface to the palm's skin: 0 on it, < 0 in it
      std::array<float,2>  slip    = {};        // cm the palm's middle is off the point it was planted on
      std::array<float,2>  elbow   = {};        // degrees between the upper arm and the forearm (180: straight)
      std::array<float,2>  wrist   = {};        // degrees the fingers are off the forearm's line
      float                armLength = 0;       // cm, shoulder to the palm's middle
      std::array<float,2>  curl    = {};        // degrees the finger bone is bent off the bind pose: before, after (left)
      };
    static Probe        playerProbe;
    static const void*  probeNpc;
    static Probe        subjectProbe;

    void   setSkeleton(const Skeleton* sk);
    bool   isValid() const { return valid; }
    // the shoulder joint of an arm (0 left, 1 right) in a pose
    size_t upperBone(size_t arm) const { return arms[arm].upper; }
    // the arm is on its way to a surface or on it
    bool   isOut(size_t arm) const { return arms[arm].has; }
    // cm, shoulder to the palm's middle (the bind pose)
    float  armLength() const;

    // false: nothing changed (no new skeleton needed)
    bool   update(const Input& in);
    void   apply(Tempest::Matrix4x4* tr, size_t numBones);

    // the stronger arm's, eased: the render rig's wrists go onto Gothic's by it
    float  weight() const;
    // one arm's (0 left, 1 right): the render rig opens that hand by it
    float  weight(size_t arm) const;
    void   probe(Probe& p) const;

  private:
    struct Arm final {
      size_t              upper = size_t(-1), fore = size_t(-1), hand = size_t(-1), finger = size_t(-1);
      std::vector<size_t> upperTree, foreTree, handTree, fingerTree;
      Tempest::Vec3       fingersLocal, palmLocal;   // in the hand bone's frame
      IkMath::Mat3        fingerBind = {};           // R_hand_bind^T * R_finger_bind
      float               length = 0;                // shoulder to wrist, the bind pose

      float               w       = 0;
      bool                has     = false;
      bool                fixed   = true;
      bool                planted = false;
      Tempest::Vec3       point, normal, plantedAt;

      float               gap = 0, slip = 0, elbow = 0, wrist = 0, curlWas = 0, curlNow = 0;
      };

    void applyArm(Arm& a, const Arm& other, Tempest::Matrix4x4* tr, size_t numBones);

    bool                valid = false;
    std::array<Arm,2>   arms;
    Tempest::Vec3       toward;
  };
