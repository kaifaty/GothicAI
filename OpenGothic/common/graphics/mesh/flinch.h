#pragma once

#include <Tempest/Matrix4x4>
#include <Tempest/Vec>

#include <cstdint>
#include <vector>

class Skeleton;

// Flinch: a blow throws the upper body away from it and the head lags behind, then they come back.
// The game's stumble animations know only a blow from the front and one from behind, and play only
// when the body may be interrupted (not aiming, casting, stumbling already, not big monsters); this
// comes on top of whatever plays, from where the blow came, as strong as it hurt. Runs as the last
// post-pass over the world-space bone matrices that Pose builds: the waist, the chest and the head turn.
class Flinch final {
  public:
    struct Hit final {
      Tempest::Vec3 dir      = {};  // where the blow pushes, world
      float         height   = 0.7f;// where it lands: 0 the hips, 1 the head
      float         twist    = 0;   // -1..1: off the middle it turns the trunk about up, + to the right
      float         strength = 0;   // 0..1
      };

    struct Input final {
      Tempest::Vec3 forward  = {1,0,0}; // horizontal facing of the body
      bool          headOnly = false;   // four legs: the trunk carries the front paws
      uint64_t      dt       = 0;
      };

    // A run into a wall, a table, a person (game/bumpsignal): the trunk is thrown off the obstacle or folds
    // over it, the head lags behind. Its own springs beside the blow's, shorter and stiffer.
    struct Bump final {
      enum Kind : uint8_t { Wall, Waist, Person, Pushed, Braced, KindCount };
      Kind          kind     = Wall;
      Tempest::Vec3 normal   = {};  // world, horizontal: off the obstacle; Pushed: the way the runner goes
      float         strength = 0;   // 0..1
      };

    // The answer of a kind at full strength: degrees, rad/s. The test bench tunes it (bump-form)
    struct BumpForm final {
      float trunk    = 0;  // peak of the trunk's throw: off the wall, over the table, the runner's way
      float twist    = 0;  // its turn about up towards an obstacle at the side
      float head     = 0;  // peak of the head's nod against the throw (below zero: with it)
      float freq     = 14, damp     = 0.75f;
      float headFreq = 18, headDamp = 0.55f;
      float floor    = 0.35f; // share of the answer the weakest bump gets
      };
    static BumpForm& bumpForm(Bump::Kind kind);

    // State for the test bench, degrees
    struct Probe final {
      bool     valid    = false;
      bool     active   = false;
      float    bend     = 0;  // of the trunk away from the blow
      float    bendYaw  = 0;  // where the top of the trunk goes, from the facing, + to the right
      float    twist    = 0;  // about up, + to the right
      float    head     = 0;  // of the head against the trunk
      uint32_t hits     = 0;
      float    strength = 0;  // of the last blow
      float    height   = 0;
      float    bumpPitch = 0; // the bump's throw of the trunk: forward +
      float    bumpRoll  = 0; // about the facing
      float    bumpTwist = 0;
      float    bumpHead  = 0; // the head against the trunk, forward +
      uint32_t bumps     = 0;
      };

    static bool    isEnabled();
    static void    setEnabled(bool e);

    void           setSkeleton(const Skeleton* sk);
    bool           isValid() const { return valid; }

    void           hit(const Hit& h, const Tempest::Vec3& forward);
    void           bump(const Bump& b, const Tempest::Vec3& forward);
    // Moves the springs on; true if the pose has to be rebuilt
    bool           update(const Input& in);
    void           apply(Tempest::Matrix4x4* tr, size_t numBones);
    void           probe(Probe& p) const;

    static Probe       playerProbe;   // filled every frame for the hero
    static Probe       subjectProbe;  // and for the npc the test bench watches
    static const void* probeNpc;

  private:
    bool                active() const;
    bool                bumpActive() const;

    size_t              waist = size_t(-1), chest = size_t(-1), head = size_t(-1);
    std::vector<size_t> waistTree, chestTree, headTree;
    bool                valid    = false;
    bool                headOnly = false;

    // rotations as axis*angle in the body's frame (forward, up, side), and how fast they change
    Tempest::Vec3       trunk  = {}, trunkV = {};
    Tempest::Vec3       neck   = {}, neckV  = {}; // the head against the trunk
    Tempest::Vec3       forward = {1,0,0};

    // the bump's own springs, the same frame; their stiffness is the last bump's
    Tempest::Vec3       bTrunk = {}, bTrunkV = {};
    Tempest::Vec3       bNeck  = {}, bNeckV  = {};
    float               bFreq = 14, bDamp = 0.75f, bHeadFreq = 18, bHeadDamp = 0.55f;
    uint32_t            bumps    = 0;

    uint32_t            hits     = 0;
    float               lastStrength = 0;
    float               lastHeight   = 0;
  };
