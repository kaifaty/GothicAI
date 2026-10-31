#pragma once

#include <Tempest/Matrix4x4>
#include <Tempest/Vec>

#include "asset/modeltypes.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

#include "animation.h"

class Skeleton;

// AimWalk: steps with the bow drawn (the manual aim, game/archery). G2 cannot: its aim (S_BOWAIM,
// S_CBOWAIM) moves the upper body only, from BIP01 SPINE1 up, the legs stay as the raise left them, and
// a step plays a whole-body animation that lowers the bow. Here the legs walk an animation of their own
// (the bow carried sneaking, slow and careful) under the aim: the root, the pelvis and the legs are its,
// the upper body stays as the aim holds it and only goes with the hips - their bob, and their turn spread
// over the spine (the hips face the way they step, the chest the target). The hero moves as the legs
// step (their root motion, Pose::animMoveSpeed): the feet do not slide. Runs right after the animation's
// skeleton, before the other post-passes (lean, foot IK, the aim, the bow drawn)
class AimWalk final {
  public:
    static constexpr float Forward = 100.f; // cm/s stepping forward...
    static constexpr float Back    = 70.f;  // ...back

    struct Input final {
      const Animation::Sequence* legs = nullptr; // the steps (a loop with root motion)
      float                      walk = 0;       // -1 back..1 forward, 0 standing
      uint64_t                   dt   = 0;
      bool                       active = false; // manual aiming still owns the legs
      bool                       blocked = false; // the body has stopped at an edge; settle the legs in place
      };

    // State for the test bench
    struct Probe final {
      bool  valid  = false;
      float weight = 0;   // 0..1 the legs step
      float walk   = 0;   // -1..1 as asked
      float phase  = 0;   // 0..1 of the steps' cycle
      float speed  = 0;   // cm/s the steps go (root motion), + forward
      float twist  = 0;   // degrees the hips are turned off the upper body (the stance's), spread over the spine
      float chest  = 0;   // degrees the chest is off the stance after it (0: the aim keeps its turn)
      float drop   = 0;   // cm the pelvis is lower than in the stance
      };

    static bool    isEnabled();
    static void    setEnabled(bool e);

    void           setSkeleton(const Skeleton* sk);
    bool           isValid() const { return valid; }
    bool           isActive() const { return (weight>0.f || stopping) && seq!=nullptr; }
    bool           isStopping() const { return stopping; }
    // cm the steps lower the pelvis from the stance (the last pose)
    float          lowered() const { return std::max(drop,0.f); }

    // Moves the steps on; true if the pose has to be rebuilt
    bool           update(const Input& in);
    // the root motion of the next dt ms, in the model's space (as Animation::Sequence::speed)
    Tempest::Vec3  move(uint64_t dt) const;
    // after the animation's skeleton: m is the model's matrix the root hangs from, base the samples it was
    // built of
    void           apply(Tempest::Matrix4x4* tr, size_t numBones, const asset::AnimationSample* base,
                         const Tempest::Matrix4x4& m);
    void           probe(Probe& p) const;
    // Final contacts, after FootIk. Rebuilds within one update share the same previous frame.
    void           captureFeet(const Tempest::Matrix4x4* tr, size_t numBones);
    void           constrainFoot(size_t leg, Tempest::Vec3& target) const;
    void           captureFootTarget(size_t leg, const Tempest::Vec3& normal, const Tempest::Vec3& shown);
    bool           stanceFoot(size_t leg, Tempest::Vec3& point) const;

    static Probe   playerProbe;   // filled every frame for the hero

  private:
    float          cycle() const;                 // ms
    float          rate() const;                  // the cycle's time per real time
    bool           sample(float at, size_t node, asset::AnimationSample& out) const;
    float          hipsYaw(const Tempest::Matrix4x4* tr) const;
    float          chestYaw(const Tempest::Matrix4x4* tr) const;

    const Skeleton*            skeleton = nullptr;
    bool                       valid    = false;
    size_t                     root = size_t(-1), pelvis = size_t(-1), spine = size_t(-1), spine1 = size_t(-1), spine2 = size_t(-1);
    size_t                     lThigh = size_t(-1), rThigh = size_t(-1), lArm = size_t(-1), rArm = size_t(-1);
    size_t                     lFoot = size_t(-1), rFoot = size_t(-1);
    std::vector<size_t>        legsTree;              // the thighs and down, parents first
    std::vector<size_t>        spineTree, spine1Tree, spine2Tree;
    std::vector<int>           seqNode;               // node -> its index in the steps' animation, -1

    const Animation::Sequence* seq    = nullptr;
    float                      weight = 0;
    float                      walk   = 0;
    float                      dir    = 1;  // the way the steps go (kept while they fade out)
    float                      phase  = 0;  // ms into the cycle
    float                      twist  = 0, chest = 0, drop = 0;
    bool                       blocked = false, stopping = false, contacts = false, haveFeet = false;
    bool                       finishPose = false, resumeFade = false;
    float                      stopTime = 0, stopWeight = 0;
    float                      resumeWeight = 0, resumePhase = 0;
    size_t                     support = 0;
    struct Leg final {
      size_t thigh = size_t(-1), calf = size_t(-1), foot = size_t(-1), toe = size_t(-1);
      std::vector<size_t> thighTree, calfTree, footTree;
      Tempest::Matrix4x4 raw, final, previousRaw, previousFinal, from, anchor;
      Tempest::Vec3 goal;
      Tempest::Vec3 correction;
      float speed = 0;
      };
    std::array<Leg,2>           feet;
    std::vector<Tempest::Matrix4x4> stance;           // the pose as the aim holds it (apply)
    std::vector<Tempest::Matrix4x4> standing;         // settled leg locals; follow the model, never world anchors
    Tempest::Matrix4x4          model;
    std::array<Tempest::Vec3,2>  stancePoints;
  };
