#pragma once

#include <Tempest/Vec>

#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>

// A protective fall (step 1 of docs/physical-character-plan.md): a knocked-off body's
// motors lead its arms, neck and trunk into an authored pose, the arms give way once
// they touch, the neck once the trunk lies. The pose is a one-frame clip of the model.
namespace RagdollBrace {

enum class Kind : uint8_t { None, Front, Back, Left, Right };

// S_BRACE_F, S_BRACE_B, S_BRACE_L, S_BRACE_R; "none" for Kind::None.
std::string_view clipName(Kind kind);
std::string_view landClipName(Kind kind);

// The throw's horizontal direction in the body's frame: within 60 degrees of forward
// it is F, within 60 degrees of back B, otherwise the side the body falls to (L: its left).
Kind select(float forward, float left);

// Bones by role, not by Gothic name; the one place that knows the names.
enum class Role : uint8_t { Other, Pelvis, Spine, Neck, Head, Clavicle, UpperArm, Forearm, Hand, Finger,
                            Thigh, Calf, Foot, Toe };
enum class Side : uint8_t { None, Left, Right };
Role role(std::string_view boneName);
Side side(std::string_view boneName);

// The one table of the motor phases (seconds; torque in N*m per kilogram of the part).
// Without a landing pose every new number is ignored, retaining the single-pose fall.
struct Profile final {
  float armsEnter = .28f;  // the target goes from the birth angles to the pose
  float armsHold  = .70f;  // held until the arm's hand, fingers or forearm touches, no longer than this from birth
  float armsDrop  = .10f;  // after the touch the limit falls to armsLevel in this time
  float armsLevel = .35f;
  float armsRest  = .20f;  // with a landing pose, hold armsLevel before yielding
  float armsYield = .40f;  // then to zero in this time
  float armsTorque = 18.f;
  float neckEnter = .25f;
  float neckHold  = 1.f;   // held until the trunk (pelvis or spine) touches, no longer than this
  float neckYield = .40f;
  float neckTorque = 12.f;
  float spineEnter = 0.f;
  float legsEnter  = .12f;
  float legsFront = 0.f, legsBack = 1.f, legsSide = 1.f; // first-pose switches, 0 or 1
  float landEnter = .18f;
  float landHold = .25f;
  float landYield = .45f;
  float landTorque = 14.f;
  };

// The game's profile; the bench command `brace-profile <group> <key> <value>` edits it.
// A changed number applies to the next fall.
Profile& profile();
// Also land enter|hold|yield|torque, arms rest, legs front|back|side (0 or 1).
// Rejects unknown names and negative or nonfinite numbers without changing the profile.
bool setNumber(Profile& profile, std::string_view group, std::string_view key, float value);
std::string describe(const Profile& profile);

// Measures of one braced fall, from the body's birth. Negative: did not happen.
struct Report final {
  Kind  kind = Kind::None;
  Kind  landKind = Kind::None;
  float landMs = -1;              // landing-phase onset from birth; absent without its pose
  float handMs[2] = {-1,-1};      // first touch of the left and right hand (hand or fingers)
  float armMs[2]  = {-1,-1};      // first touch of the arm's hand, fingers or forearm: the arm gives way
  float torsoMs   = -1;           // first touch of the pelvis or the spine
  float headImpact = -1;          // m/s, normal speed of the head's first contact
  float elbowDeg[2] = {-1,-1};    // elbow flexion when the hand first touches, 0 = straight
  float handDepthCm = 0;          // deepest penetration of a hand into its support
  float upperArmKg = 0, forearmKg = 0, handKg = 0;
  float clampMaxDeg = 0;          // largest angle a pose target was moved to its joint limits
  std::string clampBone;
  };

// One motor of a ragdoll joint, for contracts and benches.
struct MotorProbe final {
  uint32_t      bone = 0;
  float         massKg = 0;
  float         torqueLimit = 0;   // N*m, the larger of the swing and twist motors; zero while both are off
  float         errorDeg = 0;      // the joint's rotation from its motor target
  float         requestDeg = 0;    // how far the limits moved the brace's latest target (0 without a brace)
  float         clampDeg = 0;      // how far the limits moved the pose target when it was accepted
  Tempest::Vec4 target;            // constraint-space target quaternion, xyzw
  bool          on = false;        // the swing or the twist motor drives
  };

// Touches written by the contact listener of the physics workers: no allocation, no lock.
// A part is a native body index of the rig (its collision subgroup); contacts between
// parts of the same rig are ignored.
struct Contacts final {
  uint64_t watched = 0;            // parts whose touches matter
  uint64_t deep = 0;               // parts whose penetration depth is kept (hands)
  uint64_t fast = 0;               // parts whose approach speed is kept (head)
  std::atomic<uint64_t> touched {0};
  std::atomic<uint32_t> depthUm {0};   // deepest penetration of a deep part, micrometres
  std::atomic<uint32_t> speedMmS {0};  // fastest approach of a fast part in the current step, mm/s
  void mark(uint32_t part, float penetrationM, float approachMs);
  };

}
