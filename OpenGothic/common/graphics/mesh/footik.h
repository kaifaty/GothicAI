#pragma once

#include <Tempest/Matrix4x4>
#include <Tempest/Vec>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

class Skeleton;
class Npc;
class World;
class DynamicWorld;

// Foot IK: keeps the feet of humanoids and four-legged creatures on uneven ground.
// Runs as a post-pass over the world-space bone matrices that Pose builds from animation:
// the body drops to the lower planted foot, each leg is bent by a two-bone solver and the
// planted feet are tilted to the ground normal. Four-legged creatures (front paws near the
// ground in the bind pose) also pitch the body to the height of the front paws; they are not
// tilted to the ground normal as a whole (surface_align) while IK is on.
class FootIk final {
  public:
    // legs: hind (or the only) pair first, then the front pair; left before right
    enum Leg : uint8_t {
      Left       = 0,
      Right      = 1,
      FrontLeft  = 2,
      FrontRight = 3,
      MaxLegs    = 4,
      };

    struct Ground final {
      bool          hasCol = false;
      float         y      = 0;
      Tempest::Vec3 n      = {0,1,0};
      bool          toeCol = false; // ground under the toes: a swinging foot clears a step edge early
      float         toeY   = 0;
      };

    struct Input final {
      bool     enable  = false;
      bool     hardOff = false; // jump, fall, swim: switch off at once instead of fading out
      float    floorY  = 0;     // height that the animation treats as ground (object origin)
      float    crouch  = 0;     // cm the body is lowered on purpose (crouched to aim a bow), the legs bend
      bool     hasStanceFloor = false;
      float    stanceFloor = 0; // retain the ordinary aim stance's body height while its feet settle
      Ground   ground[MaxLegs];
      uint64_t dt      = 0;
      };

    // State of the legs for the test bench: positions before and after IK, the ground, metrics
    struct Probe final {
      struct Leg final {
        Tempest::Vec3 anim [4] = {};  // thigh, calf, foot, toe from the animation
        Tempest::Vec3 final[4] = {};  // the same after IK
        bool          hasCol   = false;
        float         groundY  = 0;   // ground under the ankle (ray)
        Tempest::Vec3 n        = {0,1,0};
        bool          toeCol   = false;
        float         toeY     = 0;   // ground under the toes (ray)
        float         smoothY  = 0;   // smoothed ground height that IK aims at
        float         plant    = 0;
        float         slack    = 0;
        float         err      = 0;   // contact error, see debugLog
        float         kneeDev  = 0;
        float         tilt     = 0;
        float         roll     = 0;
        float         yaw      = 0;
        float         outward  = 0;
        };
      const Skeleton* skeleton = nullptr;
      uint8_t numLegs = 0;
      uint32_t badBones = 0; // bones with NaN or infinite numbers after IK
      bool  valid   = false;
      bool  enable  = false;
      bool  hardOff = false;
      float weight  = 0;
      float pelvis  = 0;
      float chest   = 0;   // four legs: how far the shoulders are lowered
      float target  = 0;   // where the pelvis goes
      float crouch  = 0;   // cm of it on purpose (crouched)
      float limit   = 0;   // how high it may be for the planted feet to reach the ground
      float floorY  = 0;
      Leg   leg[MaxLegs];
      };

    static bool    isEnabled();
    static void    setEnabled(bool e);
    static bool    setParam(std::string_view name, float value);
    // cm the body goes down crouched to aim (a human)
    static float   aimCrouchDepth();
    static auto    paramList() -> std::string;
    static void    setProbing(bool p);
    static bool    isProbing();
    static Probe   playerProbe; // filled every frame while probing

    void           setSkeleton(const Skeleton* sk);
    bool           isValid() const { return valid; }
    bool           isQuadruped() const { return valid && quadruped; }

    // Ground under the animated feet of an npc; disabled input if the npc state does not allow IK
    auto           mkInput(const Npc& npc, World& world, const Tempest::Matrix4x4& obj,
                           bool alignedToGround, uint64_t dt, const class AimWalk* walk = nullptr) const -> Input;
    // Moves the smoothed state toward the input; true if the pose has to be rebuilt
    bool           update(const Input& in);
    // Applies IK to world-space bone matrices and records the animated ankle positions
    void           apply(Tempest::Matrix4x4* tr, size_t numBones, float floorY,
                         const Tempest::Vec3& bodyOffset = {}, class AimWalk* walk = nullptr);

    void           debugLog(std::string_view name, const Input& in, bool verbose);
    void           probe(Probe& p, const Input& in) const;

  private:
    auto mkGroundInput(Input in, const DynamicWorld& physics, const Tempest::Matrix4x4& obj,
                       bool alignedToGround, const class AimWalk* walk) const -> Input;

    struct Chain final {
      size_t              thigh = size_t(-1);
      size_t              calf  = size_t(-1);
      size_t              foot  = size_t(-1);
      size_t              toe   = size_t(-1);
      std::vector<size_t> thighTree, calfTree, footTree;
      bool                front    = false; // a front leg: upper arm, forearm, hand, finger
      float               bendSign = 1;     // a nearly straight leg bends forward (knee) or backward (elbow)

      Tempest::Vec3       animAnkle  = {};
      Tempest::Vec3       animHip    = {};
      Tempest::Vec3       animToe    = {};
      Tempest::Vec3       finalAnkle = {};
      float               groundY    = 0;  // world height of the ground under the foot, smoothed
      float               plant      = 1;  // 1: the foot stands on the ground, 0: it swings
      float               slack      = 0;  // how much lower the animated leg could still reach
      float               behind     = 0;  // 0..1: how far the foot is behind the hip (toes stay on the ground)
      float               reach      = 0;  // four legs: extra height range of a paw far from the middle of the body
      Tempest::Vec3       normal     = {0,1,0};  // the foot is tilted to it, smoothed
      Tempest::Vec3       heading    = {1,0,0};  // horizontal direction of the toes in the animation
      Tempest::Vec3       outDir     = {0,0,0};  // horizontal direction away from the other leg
      float               outward    = 0;        // radians the leg turns outward on a slope, smoothed
      float               kneeDev    = 0;  // debug: degrees between the animated and the solved knee bend
      float               tilt       = 0;  // debug: degrees the foot is tilted to the ground
      float               roll       = 0;  // debug: degrees the foot is rolled sideways
      float               yaw        = 0;  // debug: degrees the toes turned outward (+) from the animation
      Tempest::Vec3       dbgAnim [4] = {};  // debug: thigh, calf, foot, toe before IK
      Tempest::Vec3       dbgFinal[4] = {};  // debug: the same after IK
      };

    float offset(const Chain& c, float floorY) const;
    float swingOffset(const Chain& c, float floorY, float lowAnkle, float body) const;
    void  solve(Tempest::Matrix4x4* tr, const Chain& c, const Tempest::Vec3& target,
                const Tempest::Vec3& normal, float outward, float toeOff) const;

    const Skeleton* skeleton = nullptr;
    Chain    legs[MaxLegs];
    size_t   numLegs    = 0;
    bool     quadruped  = false;
    float    scale      = 1;    // leg length relative to a human: step heights scale with it
    Tempest::Vec3 forward = {};  // four legs: horizontal direction from the hips to the shoulders
    float    spread     = 0;    // a foot further than this from the origin: the pose is not valid yet
    float    pelvis     = 0;  // how far the body is lowered, relative to the floor, smoothed
    float    crouch     = 0;  // cm of the target on purpose (crouched to aim)
    float    chest      = 0;  // four legs: how far the shoulders are lowered
    float    roll       = 0;  // four legs: radians the body leans to its left side (+) across a slope, smoothed
    float    pairWidth[2] = {}; // four legs: distance between the hips and between the shoulders
    float    lastFloor  = 0;
    float    lastDFloor = 0;  // how much the floor moved in the last frame
    float    dbgTarget  = 0;
    uint32_t dbgBad     = 0;
    float    dbgLimit   = 0;
    float    weight     = 0;
    bool     valid      = false;
    uint64_t logTimer   = 0;
    uint64_t logTime    = 0;
  };
