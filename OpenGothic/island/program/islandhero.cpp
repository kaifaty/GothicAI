#include "islandhero.h"

#include "islandbody.h"

#include "physics/dynamicworld.h"

#include <algorithm>
#include <cmath>

using namespace Tempest;

namespace island {

namespace {
constexpr float Pi = 3.14159265358979f;

float wrap(float degrees) {
  degrees = std::fmod(degrees+180.f, 360.f);
  return (degrees<0 ? degrees+360.f : degrees)-180.f;
  }
}

Hero::Hero(DynamicWorld& physics, const Vec3& at, float yaw)
  : physics(physics), feet(at), angle(wrap(yaw)) {
  body = physics.collision().createVirtualCharacter(BodyRadius*100.f, BodyHeight*100.f, feet);
  // a zero step settles the capsule's contacts where it was put
  body->move({}, 0, StepHeight*100.f, WalkSlope*Pi/180.f, BodyMass, nullptr);
  }

Hero::~Hero() = default;

float Hero::speed(bool run) {
  return run ? RunSpeed : WalkSpeed;
  }

void Hero::teleport(const Vec3& at, float yaw) {
  feet   = at;
  angle  = wrap(yaw);
  vy     = 0;
  inJump = false;
  body->setPosition(feet);
  body->move({}, 0, StepHeight*100.f, WalkSlope*Pi/180.f, BodyMass, nullptr);
  grounded = false;
  }

float Hero::waterAt(float x, float z, float& ground) const {
  const auto land = physics.landRay(Vec3(x, feet.y+StepHeight*100.f, z), 0);
  if(!land.hasCol) {
    ground = feet.y;
    return 0;
    }
  ground = land.v.y;
  // From the sky down: the island's water faces up only, and DynamicWorld::waterRay looks up at it from below.
  const auto down = physics.navigationRay(Vec3(x, ground+1000.f, z), Vec3(x, ground-1.f, z), nullptr);
  if(!down.hasCol || down.kind!=DynamicWorld::NavigationHit::Water)
    return 0;
  return std::max(0.f, (down.point.y-ground)*0.01f);
  }

void Hero::tick(const Control& control, uint64_t dt) {
  if(dt==0)
    return;
  const float seconds = float(dt)*0.001f;
  // turning right turns the yaw down: the right of (cos a, sin a) is (sin a, -cos a), the direction of a - 90
  angle = wrap(angle-control.turn);
  const float a = angle*Pi/180.f;
  const Vec3  ahead = {std::cos(a), 0, std::sin(a)};
  const Vec3  right = {std::sin(a), 0, -std::cos(a)};
  Vec3        wish  = ahead*std::clamp(control.forward, -1.f, 1.f) + right*std::clamp(control.strafe, -1.f, 1.f);
  if(const float l=wish.length(); l>1.f)
    wish = wish/l;
  Vec3 step = wish*(speed(control.run)*seconds*100.f);

  // No swimming: a step that would take him deeper than his hips is not taken.
  if(step.x!=0.f || step.z!=0.f) {
    float ground = 0;
    const float next = waterAt(feet.x+step.x, feet.z+step.z, ground);
    if(next>WadeDepth && next>depth)
      step = Vec3();
    }

  if(control.jump && grounded) {
    vy       = std::sqrt(2.f*Gravity*JumpHeight);
    grounded = false;
    inJump   = true;
    ++jumpCount;
    }

  // In the air the vertical speed is the island's: the mean of this step's two speeds (exact under constant gravity).
  // VirtualCharacter::move adds a step of the platform's gravity to every request; that one is taken back out. On
  // the ground the request is level and the platform's gravity keeps the feet on it, as in the game.
  const bool  airborne = !grounded;
  const float next     = vy-Gravity*seconds;
  const float mean     = 0.5f*(vy+next);
  if(airborne) {
    step.y = (mean+DynamicWorld::gravityMS*seconds)*seconds*100.f;
    vy     = next;
    }

  const auto moved = body->move(step, dt, StepHeight*100.f, WalkSlope*Pi/180.f, BodyMass, nullptr);
  feet = moved.position;

  if(airborne) {
    // Ground under him once the rise is over: he lands. Running uphill the contacts leave an upward speed along
    // the slope; it is the ground's, not a jump's, and does not keep him in the air.
    const float solved = moved.velocity.y*0.01f;
    if(moved.grounded && mean<=0.f) {
      grounded = true;
      inJump   = false;
      vy       = 0;
      ++landCount;
      }
    else if(solved>mean+1e-3f) {
      // a slide down ground too steep to stand on: the ground takes some of the fall, but never lifts him
      vy = std::max(vy, std::min(solved, 0.f));
      }
    else if(mean>0.f && solved<mean-1e-3f) {
      // a ceiling over the rise
      vy = std::min(vy, solved);
      }
    } else if(!moved.grounded) {
    // off a ledge or onto ground too steep to stand on: he falls from rest
    grounded = false;
    vy       = 0;
    }

  float ground = 0;
  depth = waterAt(feet.x, feet.z, ground);
  }

Vec3 Hero::cameraTargetBone() const {
  return feet+Vec3(0, PelvisHeight*100.f, 0);
  }

float Hero::cameraTargetRotationRad() const {
  return angle*Pi/180.f;
  }

Matrix4x4 Hero::cameraTargetMatrix(bool /*firstPerson*/) const {
  auto m = bodyMatrix();
  m.translate(0, PelvisHeight*100.f, 0);
  return m;
  }

Matrix4x4 Hero::bodyMatrix() const {
  auto m = Matrix4x4::mkIdentity();
  m.translate(feet);
  m.rotateOY(-angle);
  return m;
  }

}
