#pragma once

#include <Tempest/Matrix4x4>

#include "asset/modeltypes.h"

#include "graphics/mesh/animationsolver.h"
#include "graphics/mesh/hitzones.h"
#include "graphics/mesh/flinch.h"
#include "graphics/mesh/lipsync.h"
#include "graphics/mesh/talkgestures.h"
#include "graphics/mesh/eyeblink.h"
#include "game/constants.h"
#include "meshobjects.h"
#include "effect.h"

class Serialize;
class ObjVisual;
class Npc;
class Item;
namespace TestBench { bool ragdollBodyDrawable(const Npc& npc); }

class MdlVisual final {
  public:
    MdlVisual();
    MdlVisual(const MdlVisual&)=delete;
    MdlVisual(MdlVisual&&) = default;
    MdlVisual& operator = (MdlVisual&&) = default;
    ~MdlVisual();

    void                           save(Serialize& fout, const Npc& npc) const;
    void                           save(Serialize& fout, const Interactive& mob) const;
    void                           load(Serialize& fin,  Npc& npc);
    void                           load(Serialize& fin,  Interactive &mob);

    void                           setVisual(const Skeleton *visual);

    void                           setObjMatrix(const Tempest::Matrix4x4 &m, bool syncAttach = false);
    bool                           minSkinnedVertex(float fatness, Tempest::Vec3& point) const;

    void                           setHeadRotation(float dx, float dz);
    Tempest::Vec2                  headRotation() const;
    void                           setYTranslationEnable(bool e);
    void                           setVisualBody(World& owner, MeshObjects::Mesh&& body);
    void                           setVisualBody(Npc& npc, MeshObjects::Mesh&& h, MeshObjects::Mesh&& body, int32_t version);
    void                           syncAttaches();
    const Skeleton*                visualSkeleton() const;
    std::string_view               visualSkeletonScheme() const;

    bool                           hasOverlay(const Skeleton*  sk) const;
    void                           addOverlay(const Skeleton*  sk, uint64_t time);
    void                           delOverlay(std::string_view sk);
    void                           delOverlay(const Skeleton*  sk);
    void                           clearOverlays();

    void                           setArmor      (Npc& npc, MeshObjects::Mesh&& armor);
    void                           setBody       (Npc& npc, MeshObjects::Mesh&& body, const int32_t version);
    void                           setSword      (MeshObjects::Mesh&& sword);
    void                           setRangedWeapon(MeshObjects::Mesh&& bow);
    void                           setShield     (MeshObjects::Mesh&& shield);
    void                           setAmmoItem   (MeshObjects::Mesh&& ammo, std::string_view bone);
    // the arrow (the bolt) has left: not shown in the hand till the next one is placed (the slot is kept:
    // the shot leaves from there)
    void                           dropAmmo();
    bool                           hasAmmoShown() const { return !ammunition.view.isEmpty(); }
    void                           setSlotItem   (MeshObjects::Mesh&& itm,  std::string_view bone);
    void                           setStateItem  (MeshObjects::Mesh&& itm,  std::string_view bone);
    void                           clearSlotItem (std::string_view bone);
    bool                           setFightMode  (asset::MdsFightMode mode);
    void                           dropWeapon    (Npc& owner);
    void                           dropShield    (Npc& owner);

    void                           setMagicWeapon(Effect&& spell, World& owner);
    void                           setMagicWeaponKey(World& owner, SpellFxKey key, int32_t keyLvl=0);

    void                           startEffect (World& owner, Effect&& pfx, int32_t slot, bool noSlot);
    void                           stopEffect  (const VisualFx& vfx);
    void                           stopEffect  (int32_t slot);
    // an effect of the animations' events that does not start (the native PEE while an aimed one plays)
    void                           setMutedPfx(std::string_view name) { muted = name; }
    std::string_view               mutedPfx() const { return muted; }
    void                           setNpcEffect(World& owner, Npc& npc, std::string_view s, zenkit::NpcFlag flags);
    void                           setFatness  (float f);
    void                           emitBlockEffect(Npc& dst, Npc& src);

    bool                           setToFightMode(const WeaponState ws);
    void                           updateWeaponSkeleton(const Item *sword, const Item *bow);

    void                           setTorch(bool t, World& owner);
    bool                           isUsingTorch() const;

    Pose&                          pose() { return *skInst; }
    const Pose&                    pose() const { return *skInst; }
    // A render-only physical layer: gameplay and aiming continue to read the ordinary Pose.
    // Both setters return whether attached visuals need synchronization.
    bool                           setPhysicalPose(const std::vector<Tempest::Matrix4x4>& physical,
                                                   const std::vector<size_t>& bones, float weight);
    bool                           clearPhysicalPose();
    const Tempest::Matrix4x4&       renderedBone(size_t bone) const;
    struct ArrowPinProbe {
      size_t             bone=size_t(-1);
      Tempest::Matrix4x4 local, world;
      };
    bool                           stickArrow(MeshObjects::Mesh&& arrow, size_t bone,
                                              const Tempest::Matrix4x4& worldMatrix, std::string_view visualName={});
    void                           saveStuckArrows(Serialize& fout) const;
    bool                           loadStuckArrows(Serialize& fin, World& world);
    void                           clearStuckArrows();
    size_t                         stuckArrowCount() const { return stuckArrows.size(); }
    bool                           stuckArrowProbe(size_t index, ArrowPinProbe& out) const;
    const HitZones&                hitZoneShape() const { return hitZones; }
    // the animation of that name plays k times as fast from its next start (1: as it is)
    void                           setAnimSpeed(std::string_view name, float k);
    void                           setLayerSpeed(const Animation::Sequence* seq, float k, uint64_t now);
    void                           hitStop(uint64_t until, float k);
    bool                           updateAnimation(Npc* npc, Interactive* mobsi, World& world, uint64_t dt, bool force,
                                                    bool pausePose=false);
    bool                           updateFootIk(Npc& npc, World& world, uint64_t dt, bool alignedToGround);
    bool                           hasQuadrupedIk() const;
    bool                           hasGaze() const;
    bool                           updateLean(Npc& npc, uint64_t dt);
    bool                           updateAim(Npc& npc, uint64_t dt);
    bool                           updateFlinch(Npc& npc, uint64_t dt);
    void                           flinch(const Npc& npc, const Flinch::Hit& hit);
    bool                           updateGaze(Npc& npc, uint64_t dt);
    void                           processLayers  (World& world);
    bool                           processEvents(World& world, uint64_t &barrier, Animation::EvCount &ev);
    auto                           mapBone(const size_t boneId) const -> Tempest::Vec3;
    auto                           mapWeaponBone() const -> Tempest::Vec3;
    auto                           mapHeadBone() const -> Tempest::Vec3;
    // the arrow on the string as posed: its nock, where it points
    bool                           arrowRay(Tempest::Vec3& pos, Tempest::Vec3& dir) const;
    // Rendered bow/arrow attachment bounds for the camera's test-bench visibility diagnostic.
    bool                           aimAttachmentBounds(bool ammo, Tempest::Matrix4x4& matrix, Tempest::Vec3 (&bounds)[2]) const;
    // where an arrow passing from-to hits the body (its bones), t: how far from 'from'
    auto                           rayHitZone(const Tempest::Vec3& from, const Tempest::Vec3& to, float& t,
                                              size_t* bone=nullptr) const -> HitZones::Zone;
    bool                           hitZoneCenter(HitZones::Zone z, Tempest::Vec3& at) const;
    std::string                    hitZonesDump() const;
    void                           hitZonesPosed(std::vector<HitZones::Posed>& out) const;
    void                           fitHitZones();
    // the bow in the hand, in the arrow's frame (the test bench)
    std::string                    bowDump() const;
    // an animation of the model (the test bench): its frames, the nodes it moves, how far it goes, which
    // way the hips face (the thighs) at a few points of it
    std::string                    animInfo(std::string_view name) const;
    // ms the animation of that name takes (the overlays chosen), 0: none
    float                          animTime(std::string_view name) const;
    // cm the bow's string goes from rest to drawn as held in the aim (its morph), 0: not known
    float                          bowSpan() const;
    // the bow holds still in the aim: an arrow on the string
    bool                           isAimSteady() const;
    // the upper body can turn to aim (Aim on, the bones there)
    bool                           isAimValid() const;
    // degrees the arrow (before the sway) points off where the shot has to go, -1 not aiming
    float                          aimTrackError() const;

    bool                           isStanding() const;

    bool                           hasAnim(AnimationSolver::Anim a, WeaponState st, WalkBit wlk) const;
    bool                           hasAnim(std::string_view scheme) const;

    const Animation::Sequence*     startAnimAndGet(std::string_view name, uint64_t tickCount, bool forceAnim = false);
    const Animation::Sequence*     startAnimAndGet(Npc& npc, std::string_view      name, uint8_t comb, BodyState bs);
    const Animation::Sequence*     startAnimAndGet(Npc& npc, AnimationSolver::Anim    a, uint8_t comb, WeaponState st, WalkBit wlk);

    bool                           startAnim      (Npc& npc, WeaponState st);
    const Animation::Sequence*     startAnimItem  (Npc& npc, std::string_view scheme, int state);
    const Animation::Sequence*     startAnimSpell (Npc& npc, std::string_view scheme, bool invest);
    bool                           startAnimDialog(Npc& npc);
    void                           startMMAnim    (Npc& npc, std::string_view anim, std::string_view node);
    void                           startFaceAnim  (Npc& npc, std::string_view anim, float intensity, uint64_t duration);
    float                          faceWeight     (std::string_view anim) { return head.view.morphWeight(anim); }
    void                           stopDlgAnim    (Npc& npc);
    // a voiced line: the mouth follows the voice instead of the talk loop
    void                           startSpeech    (Npc& npc, std::shared_ptr<LipSync::Track> voice);
    void                           stopSpeech     () { mouth.stop(); }
    void                           updateMouth    (Npc& npc, uint64_t dt);
    // the hands and the head while it speaks (TalkGestures)
    void                           setTalkMood    (std::string_view mood) { gestures.setMood(TalkGestures::moodOf(mood)); }
    void                           updateGestures (Npc& npc, uint64_t dt);
    void                           updateBlink    (Npc& npc, uint64_t dt);
    // the other one speaks to it, it thinks of an answer (TalkGestures)
    void                           listen         (uint64_t until) { gestures.listen(until); }
    void                           stopListening  (uint64_t now)   { gestures.stopListening(now); }
    void                           heard          (uint64_t now)   { gestures.heard(now); }
    void                           think          (uint64_t now, uint64_t until) { gestures.think(now,until); }
    void                           stopAnim       (Npc& npc, std::string_view anim);
    bool                           stopItemStateAnim(Npc &npc);
    void                           stopWalkAnim   (Npc &npc);
    void                           setAnimRotate  (Npc &npc, int dir);
    void                           setAnimWhirl   (Npc &npc, int dir);

    void                           interrupt();
    WeaponState                    fightMode() const { return fgtMode; }
    Tempest::Vec3                  displayPosition() const;
    const Tempest::Matrix4x4&      transform() const { return pos; }
    float                          viewDirection() const;

    const Animation::Sequence*     continueCombo(Npc& npc, AnimationSolver::Anim a, BodyState bs, WeaponState st, WalkBit wlk);
    uint16_t                       comboLength() const;

    Bounds                         bounds() const;

  private:
    // the bow's morph that holds it drawn as the aim animation does (G2)
    static constexpr const char* BowHeld = "S_READY";
    static constexpr const char* BowLoose = "S_RELAX";
    static constexpr float       CrouchSlow = 0.4f;  // crouched the upper body follows the aim that much slower
    static constexpr float       CantQuick  = 0.5f;  // the bow canted (a quick shot) that much faster
    static constexpr float       PeekClear  = 15.f;  // cm the head keeps from a wall leaning out round a corner
    static constexpr float       HeadOverShoulder = 20.f; // cm
    static constexpr uint64_t    LooseLate = 1000; // ms: the animation's release after the hand let go is the same

    template<class View>
    struct Attach {
      size_t           boneId=size_t(-1);
      View             view;
      std::string_view bone;
      };
    using MeshAttach = Attach<MeshObjects::Mesh>;
    using PfxAttach  = Attach<Effect>;

    struct PfxSlot {
      Effect      view;
      uint64_t    timeUntil=0;
      int         id=0;
      bool        noSlot = false;
      };

    struct TorchSlot {
      std::unique_ptr<ObjVisual> view;
      size_t                     boneId=size_t(-1);
      };

    struct ArrowPin {
      MeshObjects::Mesh  view;
      std::string        visual;
      size_t             bone=size_t(-1);
      Tempest::Matrix4x4 local;
      };

    static constexpr size_t MaxStuckArrows=64;

    void implSetBody(Npc* npc, World& world, MeshObjects::Mesh&& body, const int32_t version);
    void setSlotAttachment(MeshObjects::Mesh&& itm, std::string_view bone);

    void bind(MeshAttach& slot, MeshObjects::Mesh&&   itm, std::string_view bone);
    void bind(PfxAttach&  slot, Effect&& itm, std::string_view bone);

    template<class View>
    void bind(Attach<View>& slot, std::string_view bone);
    template<class View>
    void syncAttaches(Attach<View>& mesh);

    template<class View>
    void rebindAttaches(Attach<View>& mesh, const Skeleton& to);
    void rebindAttaches(const Skeleton& to);
    void setupGestures();

    Tempest::Matrix4x4             pos;
    MeshObjects::Mesh              view;

    MeshAttach                     head, sword, bow, shield;
    LipSync                        mouth;
    TalkGestures                   gestures;
    const Animation::Sequence*     gestureSeq = nullptr;  // the gesture started last
    uint32_t                       gestureSyl = 0;        // syllables of the voice seen so far
    EyeBlink                       blink;
    uint32_t                       blinkShift = 0;        // gaze shifts seen so far
    MeshAttach                     ammunition, stateItm;
    std::vector<MeshAttach>        item;
    std::vector<MeshAttach>        attach;
    std::vector<ArrowPin>          stuckArrows;

    std::vector<PfxSlot>           effects;
    std::string_view               muted;
    PfxSlot                        pfx;

    TorchSlot                      torch;

    std::string                    hnpcVisualName;
    bool                           hnpcFlagGhost = false;
    PfxSlot                        hnpcVisual;

    const Skeleton*                skeleton=nullptr;
    HitZones                       hitZones;
    bool                           stringByHand = false; // the bow's string was set by the hand last frame
    uint64_t                       looseAt      = 0;     // when the hand let the string go (a shot)

    WeaponState                    fgtMode=WeaponState::NoWeapon;
    AnimationSolver                solver;
    std::unique_ptr<Pose>          skInst;
    std::vector<Tempest::Matrix4x4> renderedPose;
    std::vector<uint8_t>           renderedMask;
    friend bool TestBench::ragdollBodyDrawable(const Npc& npc);
  };

