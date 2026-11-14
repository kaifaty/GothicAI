#pragma once

#include "ragdoll_brace.h"

#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Constraints/SwingTwistConstraint.h>
#include <Jolt/Physics/Ragdoll/Ragdoll.h>

#include <memory>
#include <string_view>
#include <vector>

// Internal to the physics module: the brace state Ragdoll::Impl owns. It overrides the
// ordinary fade motor of the braced parts and records the fall's measures.
namespace RagdollBrace {

class Rig final {
  public:
    struct Joint final { std::string_view name; int parent = -1; };

    // pose: world rotation of every native part in the brace pose (empty for Kind::None,
    // which only measures). age: the body's fade clock now. Pose targets outside their
    // joint limits are moved onto them here, once, as is the optional landPose.
    // An empty landPose retains the single-pose behavior. Returns null for a rig without the
    // human parts the profile names.
    static std::unique_ptr<Rig> create(Kind kind, const Profile& profile, JPH::Ragdoll& native,
                                       const std::vector<Joint>& joints, const std::vector<JPH::Quat>& pose, float age,
                                       const std::vector<JPH::Quat>& landPose);

    Contacts& contacts() { return touch; }
    Kind      kind() const { return braceKind; }
    void      advance(float deltaSeconds);
    // One part's fade motor: target is the parent-relative body-space orientation and
    // torque the N*m limit of the ordinary fade; both change only for a braced part.
    void      drive(size_t part, float mass, JPH::Quat& target, float& torque);
    // After a native Update: first touches, the head's first impact and the elbows.
    void      afterStep(const JPH::Ragdoll& native, const JPH::BodyInterface& bodies);
    Report    report() const;
    float     requestDeg(size_t part, const JPH::SwingTwistConstraint& joint) const;
    float     clampDeg(size_t part) const { return part<parts.size() ? parts[part].clampDeg : 0.f; }

  private:
    enum class Group : uint8_t { None, Arms, Neck, Spine, Legs };
    struct Part final {
      Role      role = Role::Other;
      Side      side = Side::None;
      Group     group = Group::None;
      bool      posed = false;            // the motor leads this part to the pose
      JPH::Quat pose = JPH::Quat::sIdentity();
      JPH::Quat landPose = JPH::Quat::sIdentity();
      JPH::Quat request = JPH::Quat::sIdentity();
      bool      requested = false;
      float     clampDeg = 0;
      };
    float     armWeight(size_t side) const;
    float     neckWeight() const;
    float     elbow(const JPH::Ragdoll& native, const JPH::BodyInterface& bodies, size_t side) const;
    uint64_t  mask(Role a, Role b, Role c, Side s) const;

    Kind              braceKind = Kind::None;
    Profile           numbers;
    std::vector<Part> parts;
    float             age = 0;
    bool              hasLandPose = false;
    float             landAt = -1;
    float             armAt[2] = {-1,-1}, handAt[2] = {-1,-1}, torsoAt = -1, headAt = -1;
    Contacts          touch;
    uint64_t          armMask[2] = {}, handMask[2] = {}, torsoMask = 0, headMask = 0;
    int               upperArm[2] = {-1,-1}, forearm[2] = {-1,-1}, hand[2] = {-1,-1};
    Report            measures;
  };

// Every joint motor of a limp rig, in native part order; bones by part.
std::vector<MotorProbe> probes(const JPH::Ragdoll& native, const std::vector<size_t>& bones, const Rig* rig);

}
