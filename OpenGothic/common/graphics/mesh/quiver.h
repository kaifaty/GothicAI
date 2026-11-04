#pragma once

#include <Tempest/Matrix4x4>
#include <Tempest/Vec>

#include <cstdint>
#include <vector>

#include "graphics/meshobjects.h"

class Skeleton;
class ProtoMesh;

// The quiver on the right hip of a bowman (reports/archery/2026-11-04-reload-from-quiver, variant A;
// reports/archery/2026-11-04-quiver-mount): it hangs from a ring at the mouth, rigid on BIP01 PELVIS, at the
// belt behind the right thigh, pushed out by the worn armour's mesh standing. The body swings round the ring:
// a damped pendulum of two degrees of freedom (back-forth, in-out), the tilt strap a spring to the rest (the
// bottom back and out), driven by how the pelvis moves; the right thigh, calf, forearm and hand (capsules)
// and the armour's flaps (its points) push it aside. The arrows in it are the mesh of the arrow the owner
// carries, tip down, the fletching above the mouth; how many show goes by steps of their count. The body is
// the own-content mesh ITRW_QUIVER (content/meshes/meshes.json): its frame has the mouth's centre at the
// origin and the quiver standing along -Y. OPENGOTHIC_QUIVER=0: none.
class Quiver final {
  public:
    static constexpr const char* Visual = "ITRW_QUIVER.3DS";

    static constexpr float Length       = 67.f;  // cm, mouth to bottom (the model, reports/content/2026-11-quiver)
    static constexpr float MouthRadius  = 5.75f; // cm, outer: the mouth 11.5 cm across
    static constexpr float BottomRadius = 4.75f;
    static constexpr float MinOut       = 22.f;  // cm the mouth's centre is out of the body's axis at least
    static constexpr float MouthHeight  = 105.f; // cm above the soles in the bind pose
    static constexpr float MouthBack    = 4.f;   // cm behind the body's axis
    static constexpr float TiltBack     = 22.5f; // deg: the bottom back from the vertical
    static constexpr float TiltOut      = 7.5f;  // deg: the bottom out
    static constexpr float Clearance    = 2.f;   // cm from the armour or the body to the quiver's side
    static constexpr float Fletching    = 27.f;  // cm of an arrow above the mouth (the fletching and the nock)
    static constexpr float FletchingMax = 30.f;

    // the swing (the research in the report): the strap and the weight hold it at ~1.8 Hz, the leather on
    // the leg damps it; the centre of the mass that far down the axis
    static constexpr float SwingFreq    = 1.8f;  // Hz
    static constexpr float SwingDamp    = 0.3f;  // of the critical
    static constexpr float SwingMax     = 35.f;  // deg from the rest
    static constexpr float MassAt       = 30.f;  // cm from the ring
    static constexpr float PushMargin   = 0.5f;  // cm the obstacles keep from the tube
    static constexpr float SimDistance  = 2500.f;// cm from the camera: farther it hangs at rest, not computed

    static bool enabled();
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
      size_t             pelvis = size_t(-1);
      Tempest::Matrix4x4 local;
      float              out = 0;
      };
    // only the ring is pushed out, by the armour standing (the bind pose); the swing gets out of the legs' way
    static Fit fit(const Skeleton& sk, const std::vector<SkinPoint>& bind);
    // which of the armour's points can reach the quiver as it swings (the flaps, the legs): by their place
    // in the bind pose round the quiver at rest
    static std::vector<size_t> nearPoints(const Fit& f, const Skeleton& sk, const std::vector<SkinPoint>& bind);

    // what pushes it: a capsule a-b of radius r (a point: a==b, r 0), in the world
    struct Obstacle {
      Tempest::Vec3 a, b;
      float         r = 0;
      };
    // the swing on by dt seconds: the pelvis bone in the world now; obstacles in the world
    void         step(const Tempest::Matrix4x4& pelvis, const std::vector<Obstacle>& obstacles, float dt);
    // at rest, the swing forgotten (far, unseen, after a jump)
    void         rest();
    const std::vector<size_t>& near() const { return nearIds; }
    void         setNear(std::vector<size_t>&& ids) { nearIds = std::move(ids); }

    // the arrows' matrices in the quiver's frame for that arrow mesh (its tip at -X, as a shot flies)
    static std::vector<Tempest::Matrix4x4> arrowLayout(const Tempest::Vec3* arrowBox, int count);

    void set(const State& st, const Fit& f, MeshObjects::Mesh&& body, std::vector<MeshObjects::Mesh>&& arrows,
             std::vector<Tempest::Matrix4x4>&& arrowLocal);
    void clear();
    const State& state() const { return st; }
    bool         isShown() const { return st.on; }
    size_t       bone() const { return fitted.pelvis; }
    const Fit&   fit() const { return fitted; }
    void         rebind(const Skeleton& sk);
    void         sync(const Tempest::Matrix4x4& pelvis);
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
      float         swing = 0, back = 0, outward = 0; // deg from the rest: all, the bottom backward, outward
      };
    // posed: the pelvis bone in the world, the points of the armour in the world; legBone: a bone of a leg
    Probe probe(const Tempest::Matrix4x4& pelvis, const std::vector<SkinPoint>& posed,
                const std::vector<bool>& legBone, const std::vector<bool>& armBone) const;

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
    // the swing: the bottom's direction in the quiver's rest frame ((0,-1,0) at rest); the mass in the world,
    // its speed, and where the ring and the rest were the last step
    Tempest::Vec3                   axis = {0,-1,0};
    Tempest::Vec3                   mass, massV, lastPivot, lastRest;
    bool                            moving = false;

    Tempest::Matrix4x4              swingMatrix() const;
  };
