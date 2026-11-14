#pragma once

// The island's hero (engine layers, step 8.6): a capsule on the platform's walking controller
// (CollisionWorld::VirtualCharacter) with the vertical motion of its own. In the game MoveAlgo of the RPG layer owns
// falling and jumps; here gravity, the jump, landing and wading are the island's, by the numbers of islandbody.h.

#include "cameratarget.h"
#include "physics/collisionworld.h"

#include <Tempest/Vec>

#include <cstdint>
#include <memory>

class DynamicWorld;

namespace island {

// What the hero is asked to do for one step: the same from the keys, the built-in demo or a recorded track.
struct Control {
  float forward = 0;   // -1..1: back .. ahead
  float strafe  = 0;   // -1..1: left .. right
  bool  run     = false;
  bool  jump    = false; // a press: jumps once, if he stands
  float turn    = 0;   // degrees of yaw this step, positive to the right
  float pitch   = 0;   // degrees the camera tilts this step, positive looks down
  };

class Hero final : public CameraTarget {
  public:
    // feet: centimetres, on the ground or above it; yaw: degrees, the direction (cos, sin) of x and z
    Hero(DynamicWorld& physics, const Tempest::Vec3& feet, float yaw);
    ~Hero() override;

    // One step of dt milliseconds.
    void tick(const Control& control, uint64_t dt);
    // Puts the hero there, standing (no fall, no jump).
    void teleport(const Tempest::Vec3& feet, float yaw);

    const Tempest::Vec3& position() const { return feet; } // the feet, centimetres
    float                yaw()      const { return angle; }
    float                verticalSpeed() const { return vy; } // metres per second, up
    bool                 onGround() const { return grounded; }
    bool                 jumping()  const { return inJump; }
    // Water over the ground under the feet, metres (0 on dry land), as of the last step.
    float                waterDepth() const { return depth; }
    // How many jumps began and how many landings (ground touched after being in the air) so far.
    uint32_t             jumps()    const { return jumpCount; }
    uint32_t             landings() const { return landCount; }
    // The speed of the step asked for, metres per second (walk or run).
    static float         speed(bool run);

    // CameraTarget: the camera follows the pelvis and turns with the hero.
    Tempest::Vec3      cameraTargetBone() const override;
    float              cameraTargetRotation() const override { return angle; }
    float              cameraTargetRotationRad() const override;
    Tempest::Matrix4x4 cameraTargetMatrix(bool firstPerson) const override;
    bool               cameraTargetSwim() const override { return false; }
    bool               cameraTargetInAir() const override { return !grounded; }
    bool               cameraTargetJump() const override { return inJump; }
    bool               cameraTargetDead() const override { return false; }
    std::string_view   cameraTargetScheme() const override { return {}; }
    std::string_view   cameraTargetPosScheme() const override { return {}; }

    // The capsule's own matrix: its centre on the feet, turned with the hero.
    Tempest::Matrix4x4 bodyMatrix() const;

  private:
    // The water over the ground at (x, z) near the feet' height, metres; 0 where there is none.
    float waterAt(float x, float z, float& ground) const;

    DynamicWorld&                                     physics;
    std::unique_ptr<CollisionWorld::VirtualCharacter> body;
    Tempest::Vec3                                     feet;
    float                                             angle     = 0;
    float                                             vy        = 0;
    float                                             depth     = 0;
    bool                                              grounded  = false;
    bool                                              inJump    = false;
    uint32_t                                          jumpCount = 0;
    uint32_t                                          landCount = 0;
  };

}
