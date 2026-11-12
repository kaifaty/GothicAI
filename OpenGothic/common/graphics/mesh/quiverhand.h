#pragma once

#include <Tempest/Matrix4x4>
#include <Tempest/Vec>

#include <cstddef>
#include <vector>

class Skeleton;

// The right hand on the quiver while it slides along the belt (reports/archery/2026-11-12-quiver-to-hand): drawing the
// bow, the left hand takes it off the back and the right one, waiting by the hip in G2's animation, takes the quiver
// by its tube under the mouth and leads it to the right side; putting the bow away, back. Over the animation: the arm
// bent by two bones so the palm is at the grip, the hand turned as the animation has it; `weight` eases it in and out
class QuiverHand final {
  public:
    static constexpr float Palm  = 8.f;   // cm from the wrist (BIP01 R HAND) along the fingers to the palm's middle
    static constexpr float Reach = 0.98f; // of the arm's length at most

    struct Input {
      float              weight = 0;  // 0: the animation's own hand
      Tempest::Matrix4x4 grip;        // where the palm takes the quiver, relative to the pelvis bone: its position
      };

    void  setSkeleton(const Skeleton* sk);
    // false: nothing changed (no new skeleton needed)
    bool  update(const Input& in);
    void  apply(Tempest::Matrix4x4* tr, size_t numBones);

    float weight() const { return in.weight; }
    // cm the palm is from the grip as shown (the bench), -1: not led
    float miss()   const { return missNow; }

  private:
    bool                valid  = false;
    size_t              pelvis = size_t(-1), upper = size_t(-1), fore = size_t(-1), hand = size_t(-1), finger = size_t(-1);
    std::vector<size_t> upperTree, foreTree, handTree;
    Input               in;
    float               missNow = -1;
  };
