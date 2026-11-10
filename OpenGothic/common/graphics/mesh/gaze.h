#pragma once

#include <Tempest/Matrix4x4>
#include <Tempest/Vec>

#include <cstdint>
#include <vector>

class Skeleton;

// Gaze: a character looks at whom it talks to. Runs as a post-pass over the world-space bone
// matrices that Pose builds (after FootIk): the head turns first, the chest and the waist join for
// larger angles; the turn is smooth and limited, a partner behind the back is let go.
class Gaze final {
  public:
    // why it looks there
    enum class Source : uint8_t {
      None,
      Talk,      // the one it talks to
      Script,    // AI_LookAtNpc, AI_LookAt
      Focus,     // the hero: the npc in focus
      Attention, // an npc: the hero coming close, a weapon drawn, a fight (world/objects/attention)
      Input,     // explicit head input relative to the body, for this actor tick
      };

    struct Input final {
      bool          active  = false;
      Tempest::Vec3 target  = {};      // where to look: the eyes of the other one
      Tempest::Vec3 forward = {1,0,0}; // horizontal facing of the body
      bool          headOnly = false;  // the hero (the body is the player's), one walking
      Source        source  = Source::None;
      uint64_t      dt      = 0;
      Tempest::Vec3 talk    = {};      // the head moving with the voice: pitch (+ up), yaw, roll, radians
      bool          eyes    = false;   // an own head with eyes: the face aims at the target from them...
      Tempest::Vec3 eyesAt  = {};      // ...here in the frame of the head bone (else from the bone itself)
      };

    // State for the test bench, degrees
    struct Probe final {
      bool          valid    = false;
      bool          active   = false;
      Source        source   = Source::None;
      float         weight   = 0;
      float         yaw      = 0;  // smoothed turn
      float         pitch    = 0;
      float         yawGoal  = 0;  // where the target is, from the body facing
      float         pitchGoal= 0;
      float         error    = -1; // angle between the facing of the head and the target, -1 if unknown
      float         headYaw  = 0;  // parts of the turn: head, chest, waist
      float         chestYaw = 0;
      float         waistYaw = 0;
      float         animYaw  = 0;  // the head as animated, from the body facing
      float         animPitch= 0;
      float         hipsYaw  = 0;  // the hips as animated, from where the npc faces
      Tempest::Vec3 head     = {};
      Tempest::Vec3 facing   = {};
      Tempest::Vec3 target   = {};
      };

    static bool    isEnabled();
    static void    setEnabled(bool e);

    void           setSkeleton(const Skeleton* sk);
    bool           isValid() const { return valid; }
    bool           isActive() const { return weight>0.f; }
    uint32_t       shiftCount() const { return shifts; }  // the eyes went elsewhere: to a partner, away, a jump (a blink comes then)

    // Moves the smoothed state toward the input; true if the pose has to be rebuilt
    bool           update(const Input& in);
    void           apply(Tempest::Matrix4x4* tr, size_t numBones);
    void           probe(Probe& p) const;

    static Probe       playerProbe;   // filled every frame for the hero
    static Probe       subjectProbe;  // and for the npc the test bench watches
    static const void* probeNpc;

  private:
    size_t              waist = size_t(-1), chest = size_t(-1), head = size_t(-1);
    size_t              lthigh = size_t(-1), rthigh = size_t(-1);
    std::vector<size_t> waistTree, chestTree, headTree;
    bool                valid    = false;
    bool                headOnly = false;
    Source              source   = Source::None;
    bool                tracking = false; // looked at the target the last update
    uint32_t            shifts   = 0;
    float               lastGoal = 0;     // yawGoal of the last update

    float               weight  = 0;
    float               yaw     = 0;  // radians, smoothed
    float               pitch   = 0;
    float               yawGoal = 0;
    float               pitchGoal = 0;
    Tempest::Vec3       target  = {};
    Tempest::Vec3       forward = {1,0,0};  // of the body, the turns are counted from it
    Tempest::Vec3       npcForward = {};    // where the npc faces

    // from the last pose
    Tempest::Vec3       hips    = {};       // facing of the hips as animated (at a mob it may differ from the npc)
    Tempest::Vec3       headPos = {};
    bool                eyes     = false;   // aims from the eyes (Input::eyes)...
    Tempest::Vec3       eyesAt   = {};      // ...in the frame of the head bone
    Tempest::Vec3       eyesPos  = {};      // where they were drawn by the last pose, world
    bool                eyesKnown = false;
    int                 faceAxis = -1;  // local axis of the head bone that points where the face does
    float               faceSign = 1;
    bool                faceFixed = false; // known from the bind pose
    float               animYaw  = 0;   // the head as animated, from the body facing
    float               animPitch = 0;
    float               animPitchLp = 0; // its slow part
    float               parts[3] = {};  // debug: head, chest, waist turn, radians
    float               error    = -1;
    Tempest::Vec3       facing   = {};
    Tempest::Vec3       talk     = {};
  };
