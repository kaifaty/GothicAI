#pragma once

#include <Tempest/Matrix4x4>
#include <Tempest/Vec>

#include <cstdint>
#include <vector>

#include "graphics/meshobjects.h"

class Skeleton;
class ProtoMesh;

// The quiver behind the right hip of a bowman (reports/archery/2026-11-04-quiver-mount, research.md): it hangs from a
// ring at the mouth, rigid on BIP01 PELVIS, 50 deg from the right side toward the back, the bottom back and out;
// only the ring is pushed out, by the worn armour's mesh standing. Rigid, no spring (the pelvis's speeding up of
// the G2 keys is noise): its turn about the vertical follows half the gait's turn of the pelvis against
// BIP01 SPINE1, and it gives way to what is in it by one turn round the ring (two axes, up to GiveAngle) that
// leaves the least of the deepest of all - the right leg, forearm and hand (capsules), the armour's flaps and
// legs (its points), the bow and a two-hander on the back - followed critically damped, no overshoot. The wide
// sleeves of robes (cloth) do not push it. The arrows in it are the mesh of the arrow the owner carries, set by
// its shaft, their tips meeting at the axis at the bottom; how many show goes by steps of their count. The body
// is the own-content mesh ITRW_QUIVER (content/meshes/meshes.json): its frame has the mouth's centre at the
// origin and the quiver standing along -Y. OPENGOTHIC_QUIVER=0: none.
class Quiver final {
  public:
    static constexpr const char* Visual = "ITRW_QUIVER.3DS";

    static constexpr float Length       = 67.f;  // cm, mouth to bottom (the model, reports/content/2026-11-quiver)
    static constexpr float MouthRadius  = 5.75f; // cm, outer: the mouth 11.5 cm across
    static constexpr float BottomRadius = 4.75f;
    static constexpr float Around       = 50.f;  // deg from the right side toward the back (research/sweep_armour.json)
    static constexpr float MinOut       = 20.5f; // cm the mouth's centre is out of the body's axis at least
    static constexpr float MouthHeight  = 105.f; // cm above the soles in the bind pose
    static constexpr float MouthBack    = 0.f;   // cm behind the body's axis before the push
    static constexpr float TiltBack     = 20.f;  // deg: the bottom back from the vertical
    static constexpr float TiltOut      = 8.f;   // deg: the bottom out, away from the body
    static constexpr float Clearance    = 2.f;   // cm from the armour or the body to the quiver's side
    static constexpr float Fletching    = 27.f;  // cm of an arrow above the mouth (the fletching and the nock)
    static constexpr float FletchingMax = 30.f;

    // the give (research.md): no spring; one turn round the ring that leaves the least of what is in it
    static constexpr float GiveAngle    = 12.f;  // deg at most
    static constexpr float RingContact  = 7.f;   // cm down the axis: nearer the ring a contact counts less, none at 7
    static constexpr float GiveTime     = 0.15f; // s: the follower is 90% there
    static constexpr float GiveStep     = 1.f;   // deg in 1/60 s at most
    static constexpr float TwistSlow    = 0.6f;  // s: the pelvis's turn against SPINE1 slower than that is a stance,
                                                 // not a gait: it is not followed
    static constexpr float MassAt       = 30.f;  // cm from the ring
    static constexpr float PushMargin   = 0.5f;  // cm the obstacles keep from the tube
    static constexpr float SimDistance  = 2500.f;// cm from the camera: farther it hangs at rest, not computed

    static bool enabled();

    // the test bench's probe (`quiver place`, `quiver still`): another place to look at, the quiver held at rest.
    // Off: the place and the swing above
    struct Place {
      bool  on = false;
      float around = Around; // deg the push goes behind the right side
      float side = 0, back = MouthBack, height = MouthHeight, minPush = MinOut;
      float tiltBack = TiltBack, tiltOut = TiltOut, tiltRight = 0;
      };
    static Place place;
    static bool  still;
    // how many arrows show for so many carried: 0, 1, 3, 6, 9, 12 (steps 1, 2-4, 5-9, 10-19, 20+)
    static int  visibleArrows(size_t count);

    // what it shows now; a change rebuilds it (Npc::updateQuiver)
    struct State {
      bool             on      = false;
      size_t           ammo    = size_t(-1); // the arrow's item class
      int              visible = 0;
      const ProtoMesh* body    = nullptr;    // the armour (or the naked body) it was fitted to
      const Skeleton*  skeleton= nullptr;
      bool operator==(const State& o) const {
        return on==o.on && ammo==o.ammo && visible==o.visible && body==o.body && skeleton==o.skeleton;
        }
      };

    // a point of the armour, the body or a rigid part of the armour, posed, with what it follows the most
    struct SkinPoint {
      Tempest::Vec3 pos;
      size_t        bone = size_t(-1);
      size_t        src  = size_t(-1); // the skin's vertex (ProtoMesh::skinVertexData); -1: a rigid part
      };

    // where the quiver hangs for that skeleton and the armour's points in the bind pose: in the pelvis bone's
    // frame; out - cm the mouth's centre is from the body's axis to the right
    struct Fit {
      bool               valid = false;
      size_t             pelvis = size_t(-1), spine = size_t(-1);
      Tempest::Matrix4x4 local;
      Tempest::Matrix4x4 pelvisBind, spineBind; // inverted: the bones' turns from the bind pose
      Tempest::Vec3      fwd = {0,0,1};          // the body's forward in the bind pose
      float              out = 0;
      };
    // only the ring is pushed out, by the armour standing (the bind pose); the swing gets out of the legs' way
    static Fit fit(const Skeleton& sk, const std::vector<SkinPoint>& bind);
    // which of the armour's points can reach the quiver as it swings (the flaps, the legs): by their place
    // in the bind pose round the quiver at rest
    // (not the arms: the capsules; a robe's sleeves do not push it); leg: the point follows the right leg
    static void nearPoints(const Fit& f, const Skeleton& sk, const std::vector<SkinPoint>& bind,
                           std::vector<size_t>& ids, std::vector<uint8_t>& leg);

    // what pushes it: a capsule a-b of radius r (a point: a==b, r 0), in the world
    enum Push : uint8_t { Carried, Leg, Arm, Flap };
    struct Obstacle {
      Tempest::Vec3 a, b;
      float         r    = 0;
      Push          kind = Carried; // the leg gives way farther; the bow and a two-hander on the back: Carried
      };
    // the give on by dt seconds: the pelvis and BIP01 SPINE1 in the world now; obstacles in the world
    void         step(const Tempest::Matrix4x4& pelvis, const Tempest::Matrix4x4& spine,
                      const std::vector<Obstacle>& obstacles, float dt);
    // the quiver at rest in the world: on the pelvis, turned about the vertical halfway to SPINE1
    Tempest::Matrix4x4 restFrame(const Tempest::Matrix4x4& pelvis, const Tempest::Matrix4x4& spine) const;
    size_t       spineBone() const { return fitted.spine; }
    // at rest, the swing forgotten (far, unseen, after a jump)
    void         rest();
    const std::vector<size_t>&  near() const { return nearIds; }
    const std::vector<uint8_t>& nearLeg() const { return nearLegs; }
    void         setNear(std::vector<size_t>&& ids, std::vector<uint8_t>&& leg) { nearIds = std::move(ids); nearLegs = std::move(leg); }

    // the arrows' matrices in the quiver's frame for that arrow mesh (its tip at -X, as a shot flies): by the
    // shaft (the middle of the head), the nocks spread over the mouth, the tips meeting at the axis at the bottom
    static std::vector<Tempest::Matrix4x4> arrowLayout(const ProtoMesh* arrow, int count);

    void set(const State& st, const Fit& f, MeshObjects::Mesh&& body, std::vector<MeshObjects::Mesh>&& arrows,
             std::vector<Tempest::Matrix4x4>&& arrowLocal);
    void clear();
    const State& state() const { return st; }
    bool         isShown() const { return st.on; }
    size_t       bone() const { return fitted.pelvis; }
    const Fit&   fit() const { return fitted; }
    void         rebind(const Skeleton& sk);
    void         sync(const Tempest::Matrix4x4& pelvis, const Tempest::Matrix4x4& spine);
    void         setTextureOwner(const Tempest::Vec3& position, bool spatial);

    // the arrows the reload has taken out and holds (shown one less each): the next task's hook
    int          taken() const { return takenOut; }
    void         setTaken(int n) { takenOut = n; }

    struct Probe {
      bool          on = false;
      int           arrows = 0;
      float         armor = 0, leg = 0;   // cm the armour (the body but its legs and arms) and the legs go into the tube
      float         arm = 0;              // cm an arm or the head goes in (a hand swinging by the hip): not fitted
      Tempest::Vec3 mouth;                // the mouth's centre in the pelvis bone's frame
      float         out = 0;
      float         top = 0, bottom = 0;  // cm the highest arrow point over the mouth, the lowest one over the bottom
      float         swing = 0, back = 0, outward = 0; // deg from the rest (the give): all, the bottom backward, outward
      float         twist = 0;            // deg the rest turns against BIP01 SPINE1 about the vertical
      float         bow = 0, longsword = 0; // cm the bow and the two-hander on the back go into the tube
      float         blade = 0;            // cm an arrow comes out of the tube's wall
      float         yield = 0;            // deg the target gives way to the obstacles
      float         legAt = 0;            // cm down the axis where the leg goes in the deepest
      float         legLeft = 0;          // cm the left leg goes in
      };
    // posed: the pelvis and SPINE1 in the world, the points of the armour in the world; legBone: a bone of a leg;
    // bow, longsword: the points of those on the back in the world (empty: not there)
    Probe probe(const Tempest::Matrix4x4& pelvis, const Tempest::Matrix4x4& spine, const std::vector<SkinPoint>& posed,
                const std::vector<bool>& legBone, const std::vector<bool>& armBone,
                const std::vector<Tempest::Vec3>& bow, const std::vector<Tempest::Vec3>& longsword,
                const std::vector<bool>& leftBone = {}) const;

    // which bones are a leg's (THIGH, CALF, FOOT, TOE) and an arm's or the head's (left out of the fit)
    static std::vector<bool> legBones(const Skeleton& sk);
    static std::vector<bool> armBones(const Skeleton& sk);

  private:
    State                           st;
    Fit                             fitted;
    int                             takenOut = 0;
    MeshObjects::Mesh               body;
    std::vector<MeshObjects::Mesh>  arrows;
    std::vector<Tempest::Matrix4x4> arrowLocal;
    std::vector<size_t>             nearIds;
    std::vector<uint8_t>            nearLegs;
    // the give: the bottom's direction in the quiver's rest frame ((0,-1,0) at rest); the mass's offset from the
    // rest across the axis in that frame (cm) and its speed; the ring's last place and speed, its speeding up
    // (in the rest frame, smoothed)
    Tempest::Vec3                   axis = {0,-1,0};
    Tempest::Vec3                   dev, devV, lastPivot, devTarget;
    bool                            moving = false;
    // the pelvis's turn against SPINE1 (rad): now, slowly followed; the rest turns by half their difference
    float                           twistNow = 0, twistSlow = 0, yawOffset = 0;
    float                           twistOf(const Tempest::Matrix4x4& pelvis, const Tempest::Matrix4x4& spine) const;

    Tempest::Matrix4x4              swingMatrix() const;
  };
