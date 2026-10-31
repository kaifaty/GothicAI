#pragma once

#include <Tempest/Event>
#include <Tempest/Matrix4x4>
#include <Tempest/Point>

#include "game/definitions/definitiontypes.h"

class World;
class Npc;
class DbgPainter;
class Serialize;

class Camera final {
  public:
    Camera();

    constexpr static const float minShadowY = -0.025f;

    enum Mode {
      Dialog,
      Normal,
      Inventory,
      Melee,
      Ranged,
      Magic,
      Mobsi,
      Death,
      Swim,
      Dive,
      Fall,
      Cutscene,
      FirstPerson,
      };

    enum MarvinMode {
      M_Normal,
      M_Freeze,
      M_Free,
      M_Pinned,
      };

    struct ListenerPos {
      Tempest::Vec3 up;
      Tempest::Vec3 front;
      Tempest::Vec3 pos;
      };

    void reset();
    void reset(const Npc* pl);
    // a blow along `dir` (world) nods the view by about `deg` and lets it back within a tenth of a second
    void impact(const Tempest::Vec3& dir, float deg);

    void save(Serialize &s);
    void load(Serialize &s, Npc* pl);

    void changeZoom(int delta);
    void setViewport(uint32_t w, uint32_t h);

    void rotateLeft(uint64_t dt);
    void rotateRight(uint64_t dt);

    void moveForward(uint64_t dt);
    void moveBack(uint64_t dt);
    void moveLeft(uint64_t dt);
    void moveRight(uint64_t dt);

    void setMode(const Mode m);
    void setMarvinMode(MarvinMode m);
    bool isMarvin() const;
    bool isFree() const;
    bool isInWater() const;
    bool isCutscene() const;

    void setToggleEnable(bool e);
    bool isToggleEnabled() const;

    void setInertiaTargetEnable(bool e);
    bool isInertiaTargetEnabled() const;

    void setFirstPerson(bool fp);
    bool isFirstPerson() const;

    void setLookBack(bool lb);

    // Over the shoulder while a bow is aimed by hand (game/archery): the center of the screen is where it
    // aims; zoom 0..1 (the draw) brings the camera closer, narrows the view
    void setAim(bool on, float zoom);
    void setAimAnchor(const Tempest::Vec3& at); // the hero's feet: the shoulder is above
    void setAimPitch(float deg);                // the aim starts looking that far down (+) or up
    // the stance aiming: the shoulder that much lower (crouched), that much to the right (leaning out), the
    // camera over the right shoulder (1) .. the left one (-1)
    void setAimStance(float drop, float side, float shoulder);
    // leaning over an edge (graphics/mesh/aim): the shoulder that much ahead and down, as the head goes;
    // kept while the camera goes back from the aim
    void setAimLean(float reach, float drop);
    bool isAim() const { return aimOn; }
    // The point over the shoulder the center of the screen passes while aiming: from the hero's feet and
    // the way the camera looks (horizontal)
    static Tempest::Vec3 aimShoulder(const Tempest::Vec3& feet, const Tempest::Vec3& forward);
    // The center of the screen as a ray: from the shoulder (aiming), else from the camera
    void aimRay(Tempest::Vec3& from, Tempest::Vec3& dir) const;

    void toggleDebug();

    void tick(uint64_t dt);
    void debugDraw(DbgPainter& p);

    Tempest::PointF    spin() const;
    Tempest::Vec3      destTarget() const;
    float              azimuth() const;

    void               setSpin(const Tempest::PointF& p);
    void               setTarget(const Tempest::Vec3& pos);

    void               setAngles(const Tempest::PointF& pos);
    void               setPosition(const Tempest::Vec3& pos);
    void               setDialogDistance(float d);

    void               onRotateMouse(const Tempest::PointF& dpos);

    Tempest::Matrix4x4 projective() const;
    Tempest::Matrix4x4 view() const;
    Tempest::Matrix4x4 viewProj() const;
    Tempest::Matrix4x4 viewShadow(const Tempest::Vec3& ldir, size_t layer) const;

    Tempest::Matrix4x4 viewLwc() const;
    Tempest::Matrix4x4 viewProjLwc() const;
    Tempest::Matrix4x4 viewShadowLwc(const Tempest::Vec3& ldir, size_t layer) const;

    Tempest::Matrix4x4 viewShadowVsm(const Tempest::Vec3& ldir) const;
    Tempest::Matrix4x4 viewShadowVsmLwc(const Tempest::Vec3& ldir) const;

    ListenerPos        listenerPosition() const;
    Tempest::Vec3      originLwc() const { return origin+shake; };

    float              zNear() const;
    float              fovY() const; // degrees, the vertical field of view (the aim narrows it)
    void               setNearClip(float zNear); // debug (test bench): clips away what is closer to the camera
    float              zFar()  const;

  private:
    struct Pin {
      Tempest::Vec3       origin = {};
      Tempest::Vec3       spin   = {};
      };

    struct State {
      float               range  = 3.f;
      Tempest::Vec3       spin   = {};
      Tempest::Vec3       target = {};
      };

    struct Interpolated {
      Tempest::Vec3       target    = {};
      Tempest::Vec3       rotOffset = {};
      };

    State                 state;
    Interpolated          inter;
    Pin                   pin;

    Tempest::Vec3         origin    = {};
    Tempest::Vec3         angles    = {};
    Tempest::Vec3         veloTrans = {};
    float                 userRange  = 0;
    Tempest::Vec3         shake      = {};
    Tempest::Vec3         kickAmp    = {};  // impact: degrees of pitch and yaw
    Tempest::Vec3         kickOff    = {};
    float                 kickT      = 1;   // seconds since the impact

    float                 targetVelo = 0;

    float                 dlgRange   = 0;

    Tempest::Matrix4x4    proj;
    float                 nearClip = 10.f;
    uint32_t              vpWidth=0;
    uint32_t              vpHeight=0;
    float                 depthNear = 0;

    bool                  dbg           = false;
    bool                  tgEnable      = true;
    bool                  fpEnable      = false;
    bool                  lbEnable      = false;
    bool                  inertiaTarget = true;
    Mode                  camMod        = Normal;
    MarvinMode            camMarvinMod  = M_Normal;
    bool                  inWater       = false;

    mutable int           raysCasted = 0;

    bool                  aimOn      = false;
    float                 aimBlend   = 0;   // 0..1: from the usual camera to over the shoulder
    float                 aimZoom    = 0;   // wanted...
    float                 aimZoomS   = 0;   // ...smoothed
    float                 aimSaved   = 0;   // the elevation before the aim: back to it after...
    float                 aimPitchBack = 0; // ...what is left to go back, as the camera blends out
    Tempest::Vec3         aimAnchor  = {};
    float                 aimDrop    = 0;
    float                 aimSide    = 0;
    float                 aimShoulderK = 1;
    float                 aimReach   = 0;   // leaning over an edge: ahead...
    float                 aimLeanDrop= 0;   // ...and down
    float                 aimLeanRoom= 1;   // 0..1 of it there is room for (a roof, a wall in the way)
    float                 aimRangeRoom = 1; // 0..1 of the range behind the shoulder there is room for
    float                 aimSteepAdvance = 0; // ahead of the hands when looking steeply down
    float                 aimSteepRoom = 1; // room for that extra displacement
    Tempest::Vec3         aimFrom    = {};  // the shoulder point the center of the screen passes
    float                 fovK       = 1;   // of the field of view, the aim narrows it

    void                  updateProj();
    void                  tickAim(float dtF);

    static const float    minLength;

    void                  tickFirstPerson(float dtF);
    void                  tickThirdPerson(float dtF);

    Tempest::Vec3         clampRotation(Tempest::Vec3 spin);
    float                 calcCameraColision(const Tempest::Vec3& target, const Tempest::Vec3& dir, const Tempest::Vec3& rotSpin, float dist) const;
    Tempest::Vec3         calcCameraColision(const Tempest::Vec3& from, const Tempest::Vec3& dir) const;

    Tempest::Vec3         calcLookAtAngles(const Tempest::Vec3& origin, const Tempest::Vec3& target,
                                           const Tempest::Vec3& rotOffset, const Tempest::Vec3& defSpin) const;

    void                  implMove(Tempest::KeyEvent::KeyType t, uint64_t dt);

    Tempest::Matrix4x4    mkView    (const Tempest::Vec3& pos, const Tempest::Vec3& spin) const;
    Tempest::Matrix4x4    mkRotation(const Tempest::Vec3& spin) const;
    Tempest::Matrix4x4    mkViewShadow(const Tempest::Vec3& cameraPos, float rotation,
                                       const Tempest::Matrix4x4& viewProj, const Tempest::Vec3& lightDir, size_t layer) const;
    Tempest::Matrix4x4    mkViewShadowVsm(const Tempest::Vec3& cameraPos, const Tempest::Vec3& ldir) const;

    Tempest::Vec3         followTarget(Tempest::Vec3 pos,  Tempest::Vec3 dest, float dtF);
    Tempest::Vec3         followTrans (Tempest::Vec3 pos,  Tempest::Vec3 dest, float dtF, float velo);
    Tempest::Vec3         followRot   (Tempest::Vec3 spin, Tempest::Vec3 dest, float dtF, float velo);

    void                  followAng   (Tempest::Vec3& spin, Tempest::Vec3 dest, float dtF);
    static void           followAng   (float& ang, float dest, float speed, float dtF);

    const Def::Camera& cameraDef() const;
  };
