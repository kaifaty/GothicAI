#pragma once

#include <Tempest/Matrix4x4>
#include <Tempest/Vec>

#include <cstdint>
#include <vector>

#include "graphics/meshobjects.h"

class Skeleton;
class ProtoMesh;

// The quiver on the right hip of a bowman (reports/archery/2026-11-04-reload-from-quiver, variant A): rigid on
// BIP01 PELVIS, the mouth at the belt behind the right thigh, the bottom back and out; pushed out by the worn
// armour's mesh. The arrows in it are the mesh of the arrow the owner carries, tip down, the fletching above
// the mouth; how many show goes by steps of their count. The body of the quiver is the own-content mesh
// ITRW_QUIVER (content/meshes/meshes.json): its frame has the mouth's centre at the origin and the quiver
// standing along -Y. OPENGOTHIC_QUIVER=0: none.
class Quiver final {
  public:
    static constexpr const char* Visual = "ITRW_QUIVER.3DS";

    static constexpr float Length       = 68.f;  // cm, mouth to bottom
    static constexpr float MouthRadius  = 4.75f; // cm, outer: the mouth 9.5 cm across
    static constexpr float BottomRadius = 4.f;
    static constexpr float MouthHeight  = 105.f; // cm above the soles in the bind pose
    static constexpr float MouthBack    = 4.f;   // cm behind the body's axis
    static constexpr float TiltBack     = 22.5f; // deg: the bottom back from the vertical
    static constexpr float TiltOut      = 7.5f;  // deg: the bottom out
    static constexpr float Clearance    = 2.f;   // cm from the armour or the body to the quiver's side
    static constexpr float Fletching    = 27.f;  // cm of an arrow above the mouth (the fletching and the nock)
    static constexpr float FletchingMax = 30.f;

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
      };

    // where the quiver hangs for that skeleton and the armour's points in the bind pose: in the pelvis bone's
    // frame; out - cm the mouth's centre is from the body's axis to the right
    struct Fit {
      bool               valid = false;
      size_t             pelvis = size_t(-1);
      Tempest::Matrix4x4 local;
      float              out = 0;
      };
    // moving: the same points in the frames of the walks, runs and the aim (motionFrames), carried into the
    // bind pose by the pelvis: the legs and the flaps swing back and out of the pelvis as they go
    static Fit fit(const Skeleton& sk, const std::vector<SkinPoint>& bind, const std::vector<SkinPoint>& moving);

    // the bones (model space) of the frames the fit looks at: of each of the moves the bowman has, evenly
    static std::vector<std::vector<Tempest::Matrix4x4>> motionFrames(const Skeleton& sk);
    // points of a frame into the bind pose: that frame's pelvis put where the bind pose has it
    static void toBind(const Skeleton& sk, const std::vector<Tempest::Matrix4x4>& frame, std::vector<SkinPoint>& pts);

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
    Tempest::Vec3                   arrowSpan[2]; // the arrows' lowest and highest point along the quiver (cm)
  };
