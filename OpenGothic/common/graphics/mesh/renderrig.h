#pragma once

#include <Tempest/Matrix4x4>
#include <Tempest/Vec>

#include "asset/modeltypes.h"
#include "ikmath.h"

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

class Skeleton;

// A render skeleton of our own under a Gothic one (reports/engine/2026-11-modern-skeleton, step 1): the game keeps
// its skeleton (HUMANS.MDH: animations, events, IK, hit zones), our meshes sit on the rig, and every frame the rig's
// pose is made from Gothic's final world matrices. The rig's skeleton is an ordinary hierarchy record
// (models/models.json), in the same space and scale as the source one. The algorithm is solve() of
// scripts/content/hero_body/retarget_pilot.py, without its metres and Blender frame.
//
// models/render-rigs.json of our content:
//   {"format": "gothic-render-rigs", "version": 1,
//    "rigs": {"<rig>": {
//       "skeleton": "HUM_RIG_MPFB.MDH",          the rig's hierarchy record
//       "source":   "HUMANS.MDH",                the Gothic hierarchy it follows
//       "bones":  [{"name": "thigh_l", "source": "BIP01 L THIGH", "align": [w, x, y, z]}, ...],
//       "root":   {"bone": "pelvis", "source": "BIP01 PELVIS", "knees": true},
//       "legs":   [{"thigh": "thigh_l", "calf": "calf_l", "foot": "foot_l", "ball": "ball_l",
//                   "source_foot": "BIP01 L FOOT", "source_knee": "BIP01 L CALF", "ankle": [x, y, z], "soft": 0.5}],
//       "fingers":[{"source_hand": "BIP01 L HAND", "source_finger": "BIP01 L FINGER0",
//                   "chains": [["index_01_l", "index_02_l", "index_03_l"], ...], "shares": [1.0, 0.9, 0.7]}],
//       "twists": [{"bone": "lowerarm_twist_01_l", "of": "hand_l", "against": "lowerarm_l", "axis": "hand_l",
//                   "share": 1.0}],
//       "shoulders": [{"clavicle": "clavicle_l", "arm": "upperarm_l", "elbow": "lowerarm_l", "chest": "spine_03",
//                      "share": 0.33, "from": 45, "max": 30}],
//       "arms":   [{"upper": "upperarm_r", "lower": "lowerarm_r", "hand": "hand_r", "source_hand": "BIP01 R HAND"},
//                  {"upper": "upperarm_l", "lower": "lowerarm_l", "hand": "hand_l", "source_hand": "BIP01 L HAND",
//                   "holds": "hand_r", "soft": 1.0}],
//       "sockets":{"ZS_RIGHTHAND": {"bone": "hand_r", "offset": [16 floats]}}}},
//    "meshes": {"<mesh>": "<rig>"}}
// - bones: nodes of the rig's skeleton. source - the Gothic node whose turn the bone takes; align - a turn of the
//   bone's bind pose onto the Gothic one, a quaternion (w, x, y, z), column vectors, world (model) frame. A node not
//   listed, or listed without source, follows its parent rigidly as in the bind pose. align without source is unused.
// - root: the bone that is placed by the source node (the pelvis); the other bones are placed by their parent.
//   lift (0..0.5, default 0) raises it by that share of its height over the lower of the legs' IK targets: Gothic's
//   clips are made for legs shorter than its bind skeleton (reports/engine/2026-11-modern-skeleton, «Ноги»), so legs
//   fitted to the bind bend their knees without it. The lift is world up (+y), none when the targets are not below.
//   knees (default false, not with lift): the knees bend as Gothic's whatever its clip's leg lengths («Аудит»). The
//   root goes up or down (world +y) so that the legs that carry the body - a foot on the ground under the hip - reach
//   their feet with our bone lengths at Gothic's knee angle; a foot on the ground keeps its ball where Gothic's is and
//   turns about it (the heel up, or back down to Gothic's flat foot) for the rest; a foot in the air takes Gothic's
//   turns as they are. Needs the root and each leg's thigh, calf and foot with a source, the thighs right under the
//   root.
//   heel (default: as knees; false brings the ball-on-ball feet back): our foot is not Gothic's length (the hero's
//   ankle-to-ball is 14 cm, Gothic's 23), so with the balls together our ankle stood 9 cm ahead of Gothic's, the leg
//   vaulted over another point and the pelvis that kept Gothic's knee rose through every step and dropped at the
//   next one (reports/engine/2026-11-modern-skeleton, «Пятка»). A leg that stands on its foot - it points down, its
//   thigh and shin do - stands heel on heel instead: the pivot is Gothic's ball moved back by the bind difference,
//   along the ground while the heel is up (the foot rolls over its own ball and turns further than Gothic's for the
//   same rise of the ankle, until it leaves the ground with a high heel), along the foot while the toes are up
//   (the foot turns about Gothic's ankle). A foot is on the ground by the lower of its ball and the point under
//   Gothic's ankle. A leg on the ground is never straighter than Gothic's knee: a foot that cannot reach stops
//   short. The root's lift and each leg's share of the heel are kept between solves (Gait): the lift follows its
//   goal over a moment, holds while no foot is on the ground and a leg points down (a jump, the flight of a run),
//   and is never under what a carrying leg needs with its foot flat. Without a Gait every solve stands alone.
// - legs: the chain thigh -> calf -> foot -> ball (each the parent of the next); ankle - the IK target, cm in the
//   frame of source_foot. The knee bends towards the thigh's front: the model's front (+z) of the bind pose carried
//   by the thigh's turn; source_knee only when that front lies along the leg. soft (cm, 0..20, default 0) - soft IK:
//   the last soft cm of reach come slowly, the knee does not snap straight.
// - fingers: the turn of source_finger against source_hand is spread over each chain, share i on phalanx i.
// - twists: a twist bone (no source, no children, its parent B) follows B and turns about B's bind axis (B -> axis)
//   by share x the twist of `of` against `against` from the bind pose (swing-twist about that axis), share -2..2.
//   The hand's twist put on along the forearm (share > 0), the upper arm's own twist taken off near the shoulder
//   (of = upperarm, against = clavicle, share < 0); the skin spreads over B and its twist bones along B.
// - shoulders: the scapulohumeral rhythm - the clavicle turns further towards its arm by share x (the arm's rise
//   from the chest's down above `from` degrees), at most `max` degrees. arm needs a source; chest is an ancestor of
//   the clavicle; the arm's axis is arm -> elbow in the bind pose.
// - arms: the chain upper -> lower -> hand (each the parent of the next, each with a source) by two-bone IK, the elbow
//   to the side Gothic's arm bends it (its back when straight), soft as for the legs. The wrist goes where solve()'s
//   Hands say: by `world` onto Gothic's wrist (source_hand: a ladder, a workbench - our arms are not Gothic's length),
//   by `hold` - an arm that holds another hand (holds) - where Gothic holds it against that hand (a 2H hilt, a bow);
//   with both 0 the arm takes Gothic's turns as the other bones.
// - sockets: a Gothic node (ZS_*) carried by a rig bone: M_bone(t) * offset; offset column-major as in the records.
// - meshes: mesh name -> rig, the name upper case, without folder and extension.
// legs, fingers, twists, shoulders, arms, sockets and meshes may be left out. Unknown fields are errors. A broken file loads no rig, a broken
// rig is left out alone, each with a message "render rigs: ..." or "render rig <name>: ...".
struct RenderRigSpec final {
  struct Bone final {
    std::string  name, source;
    IkMath::Quat align;
    };
  struct Leg final {
    std::string   thigh, calf, foot, ball, sourceFoot, sourceKnee;
    Tempest::Vec3 ankle;
    float         soft = 0;
    };
  struct Fingers final {
    std::string                           sourceHand, sourceFinger;
    std::vector<std::vector<std::string>> chains;
    std::vector<float>                    shares;
    };
  struct Socket final {
    std::string        source, bone;
    Tempest::Matrix4x4 offset;
    };
  struct Twist final {
    std::string bone, of, against, axis;
    float       share = 0;
    };
  struct Shoulder final {
    std::string clavicle, arm, elbow, chest;
    float       share = 0, from = 0, max = 0;  // degrees
    };
  struct Arm final {
    std::string upper, lower, hand, sourceHand, holds;
    float       soft = 0;
    };

  std::string          name, skeleton, source;
  std::vector<Bone>    bones;
  std::string          rootBone, rootSource;
  float                rootLift = 0;
  bool                 rootKnees = false;
  bool                 rootHeel = false;
  std::vector<Leg>     legs;
  std::vector<Fingers> fingers;
  std::vector<Twist>   twists;
  std::vector<Shoulder> shoulders;
  std::vector<Arm>     arms;
  std::vector<Socket>  sockets;
  };

class RenderRig final {
  public:
    ~RenderRig();

    // where the arms put the wrists (render-rigs.json, arms), 0..1 each: the game's state, eased by the caller
    struct Hands final {
      float hold  = 0;  // two hands on one thing: a 2H weapon, a bow, a crossbow, a spell
      float world = 0;  // the hands on the world: a ladder, a workbench, a chest
      };

    // what the legs keep between solves (render-rigs.json, root: heel): one per body, reset (valid = false) when the
    // body is put somewhere else
    struct Gait final {
      static constexpr size_t MaxLegs = 4;
      float lift = 0;             // the root's lift, cm
      float plant[MaxLegs] = {};  // each leg's share of standing heel on heel
      bool  valid = false;        // false: the next solve starts anew
      };

    // Throws std::runtime_error("render rig <name>: ...") when a name is missing from its skeleton, the rig has more
    // than Resources::MAX_NUM_SKELETAL_NODES bones, or a part does not fit (a leg out of chain, a bone in two parts).
    static std::unique_ptr<RenderRig> build(const RenderRigSpec& spec, const asset::ModelHierarchy& rig,
                                            const asset::ModelHierarchy& source);

    std::string_view name()        const { return rigName; }
    // the rig's skeleton: the palette is in the order of its nodes, as a mesh bound to it expects
    const Skeleton&  skeleton()    const { return *own; }
    size_t           bones()       const { return bone.size(); }
    // the nodes solve() reads; fits() - the source skeleton is the one the rig was made for (names in order)
    size_t           sourceNodes() const { return sourceNames.size(); }
    bool             fits(const Skeleton& source) const;

    // The rig's world matrices out[bones()] from the source's final world matrices source[sourceCount] (Pose::tr,
    // after IK and ragdoll). No allocation. False: too few source matrices, out is not written.
    // gait, dt: the legs' memory and the seconds since the last solve of this body; nullptr - the solve stands alone
    bool             solve(const Tempest::Matrix4x4* source, size_t sourceCount, Tempest::Matrix4x4* out,
                           const Hands& hands, Gait* gait = nullptr, float dt = 0) const;
    bool             solve(const Tempest::Matrix4x4* source, size_t sourceCount, Tempest::Matrix4x4* out) const {
      return solve(source,sourceCount,out,Hands());
      }

    // the socket of a source node (ZS_RIGHTHAND) over the solved palette: M_bone(t) * offset; false - none
    bool             socket(std::string_view sourceNode, const Tempest::Matrix4x4* palette, Tempest::Matrix4x4& out) const;
    bool             socket(size_t sourceNode, const Tempest::Matrix4x4* palette, Tempest::Matrix4x4& out) const;

    // the source node a rig bone belongs to (the skin's points for the hit zones): its source, else the nearest
    // ancestor's, else the source's root node; size_t(-1) - no such bone
    size_t           sourceOf(size_t bone) const;

  private:
    RenderRig() = default;

    enum class Role : uint8_t { Plain, Thigh, Calf, Foot, Phalanx, Twist, ArmUpper, ArmLower, ArmHand };
    static constexpr size_t None    = size_t(-1);
    static constexpr size_t MaxLegs = Gait::MaxLegs;   // knees: per-leg state on the stack

    struct Bone final {
      size_t             parent = None;  // rig node; None - under the source's root node
      size_t             source = None;  // source node of the turn; None - rigid behind the parent
      Tempest::Matrix4x4 local;          // bind pose under the parent's
      IkMath::Mat3       turn = {};      // R_source_bind^T * align * R_bind: R = R_source(t) * turn
      Role               role = Role::Plain;
      size_t             part = 0;       // the leg, the fingers or the twist
      float              share = 0;      // of the finger's bend
      size_t             shoulder = None; // the clavicle's rhythm
      bool               afterArms = false; // below an arm's forearm: placed once the arms are solved
      };
    struct Leg final {
      size_t        thigh = None, calf = None, foot = None, ball = None;
      size_t        sourceFoot = None, sourceKnee = None;
      Tempest::Vec3 ankle;               // in the frame of sourceFoot
      Tempest::Vec3 front;               // the knee's way: the model's front of the bind pose in the thigh's frame
      float         soft = 0;            // cm of soft reach at the bind size
      float         length = 0;          // thigh + calf in the bind pose
      Tempest::Vec3 ballFoot;            // knees: our ball in the frame of sourceFoot, bind pose
      IkMath::Mat3  footBind = {};       // knees: R_sourceFoot_bind^T
      float         heelBack = 0;        // heel: our ankle ahead of the source foot node in the bind pose, cm
      float         under = 0;           // heel: our ball's height against the source foot node's, bind pose, cm
      };
    struct Fingers final {
      size_t        hand = None, finger = None;
      IkMath::Mat3  bind = {};           // R_finger_bind^T * R_hand_bind
      };
    struct Socket final {
      std::string        name;
      size_t             source = None, bone = None;
      Tempest::Matrix4x4 offset;
      };
    struct Twist final {
      size_t        bone = None, parent = None, of = None, against = None;
      IkMath::Mat3  bindRel = {};        // R_against_bind^T * R_of_bind
      Tempest::Vec3 axisAgainst;         // the axis in the frame of `against`, bind pose
      Tempest::Vec3 axisParent;          // the same axis in the parent's frame
      float         share = 0;
      };
    struct Shoulder final {
      size_t        clavicle = None, arm = None, chest = None;
      Tempest::Vec3 clavAxis, armAxis, chestDown;  // in the bones' own frames, bind pose
      float         share = 0, from = 0, max = 0;  // radians for from and max
      };
    struct Arm final {
      size_t        upper = None, lower = None, hand = None, sourceHand = None;
      size_t        holds = None;        // the arm whose hand this one holds on to
      Tempest::Vec3 back;                // the elbow's way on a straight arm: the model's back in the upper's frame
      float         soft = 0, length = 0;
      };
    // a foot on the ground (knees): Gothic's ball, the ankle's offset from it, the axis (+ lifts the heel), the turn
    // range about it (radians) and how much the foot is on the ground
    struct Pivot final {
      Tempest::Vec3 ball, r, k;
      float         lo = 0, hi = 0, contact = 0;
      float         match = 0;           // heel: the turn that lifts our ankle as high as Gothic's heel lifts its own
      };

    Tempest::Matrix4x4 place(size_t i, const Tempest::Matrix4x4* source, const Tempest::Matrix4x4* out,
                             float lift = 0) const;
    void               solveLeg(const Leg& leg, const Tempest::Matrix4x4* source, Tempest::Matrix4x4* out,
                                float carry, float lowBall, float scale, float plant) const;
    Pivot              pivot(const Leg& leg, const Tempest::Matrix4x4* source, float lowBall, float scale,
                             float plant) const;
    float              footLow(const Leg& leg, const Tempest::Matrix4x4* source, float scale) const;
    float              kneeLift(const Tempest::Matrix4x4* source, float* carry, float& lowBall, float& scale) const;
    float              heelLift(const Tempest::Matrix4x4* source, float* carry, float* plant, float& lowFoot,
                                float& scale, Gait* gait, float dt) const;
    void               solveArm(const Arm& arm, const Tempest::Matrix4x4* source, Tempest::Matrix4x4* out,
                                const Hands& hands) const;
    IkMath::Mat3       rhythm(const Shoulder& s, const Tempest::Matrix4x4& clavicle, const Tempest::Matrix4x4* source,
                              const Tempest::Matrix4x4* out) const;
    void               solveTwist(const Twist& t, Tempest::Matrix4x4* out) const;

    std::string               rigName;
    std::unique_ptr<Skeleton> own;
    std::vector<std::string>  sourceNames;
    std::vector<Bone>         bone;
    std::vector<size_t>       order;       // parents first
    size_t                    sourceRoot   = None;
    size_t                    sourceNeeded = 0;
    size_t                    rootBone     = None, rootSource = None;
    Tempest::Vec3             rootLocal;   // the root bone's bind place in the source node's bind frame
    float                     lift = 0;    // share of the root's height over the lower leg target it is raised by
    bool                      knees = false;
    bool                      heel = false;
    std::vector<Leg>          legs;
    std::vector<Fingers>      fingers;
    std::vector<Twist>        twists;
    std::vector<Shoulder>     shoulders;
    std::vector<Arm>          arms;        // the held ones after the ones they hold
    std::vector<size_t>       afterArms;   // in order
    std::vector<Socket>       sockets;
  };

// models/render-rigs.json: read at once, each rig built on its first ask and kept by name.
class RenderRigIndex final {
  public:
    using Lookup = std::function<bool(std::string_view record, asset::ModelHierarchy& out)>;
    using Report = std::function<void(const std::string& message)>;

    RenderRigIndex(Lookup lookup, Report report);
    ~RenderRigIndex();

    void             read(const std::filesystem::path& file);  // no file: no rigs, as without the feature
    void             parse(std::string_view json);
    size_t           size() const { return specs.size(); }
    // the rigs read (built or not yet), by name; meshTable - mesh (upper case, without folder and extension) -> rig
    std::vector<std::string> rigNames() const;
    const std::map<std::string,std::string,std::less<>>& meshTable() const { return meshes; }

    // nullptr: no such rig or a broken one (reported once)
    const RenderRig* rig(std::string_view name);
    const RenderRig* rigOfMesh(std::string_view mesh);

  private:
    Lookup                                                      lookup;
    Report                                                      report;
    std::map<std::string,RenderRigSpec,std::less<>>             specs;
    std::map<std::string,std::string,std::less<>>               meshes;
    std::unordered_map<std::string,std::unique_ptr<RenderRig>>  built;  // nullptr: tried and broken
  };
