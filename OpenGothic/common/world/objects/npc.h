#pragma once

#include <functional>

#include "graphics/mesh/pose.h"
#include "graphics/mesh/animation.h"
#include "graphics/meshobjects.h"
#include "graphics/mdlvisual.h"
#include "game/gametime.h"
#include "game/movealgo.h"
#include "game/inventory.h"
#include "game/fightalgo.h"
#include "game/archerai.h"
#include "game/gamescript.h"
#include "game/ragdollsave.h"
#include "game/meleeimpact.h"
#include "physics/dynamicworld.h"
#include "physics/ragdoll.h"
#include "world/aiqueue.h"
#include "world/objects/attention.h"
#include "world/objects/archer.h"
#include "world/fplock.h"
#include "world/waypath.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <zenkit/addon/daedalus.hh>

class Interactive;
class WayPoint;
struct PhysicsNoise;

struct HeroDeed;

class Npc final {
  public:
    using JumpStatus = MoveAlgo::JumpStatus;

    enum GoToHint : uint8_t {
      GT_No,
      GT_Way,
      GT_NextFp,
      GT_Enemy,
      GT_Item,
      GT_Point,
      GT_EnemyG, //deprecated
      GT_Flee,
      };

    enum HitSound : uint8_t {
      HS_NoSound = 0,
      HS_Dead    = 1
      };

    enum CastState : uint8_t {
      CS_NoCast      = 0,
      CS_Finalize    = 1,

      CS_Invest_0    = 16,
      CS_Invest_1    = 17,
      CS_Invest_2    = 18,
      CS_Invest_3    = 19,
      CS_Invest_4    = 20,
      CS_Invest_5    = 21,
      CS_Invest_6    = 22,
      CS_Invest_Last = 31,

      CS_Cast_0      = 32,
      CS_Cast_1      = 33,
      CS_Cast_2      = 34,
      CS_Cast_3      = 35,
      CS_Cast_Last   = 47,

      CS_Emit_0      = 48,
      CS_Emit_Last   = 63,
      };

    enum BeginCastResult : uint8_t {
      BC_No,
      BC_NoMana,
      BC_Cast,
      BC_Invest,
      };

    using Anim = AnimationSolver::Anim;

    Npc(World &owner, size_t instance, std::string_view waypoint, NpcProcessPolicy aiPolicy = NpcProcessPolicy::AiNormal);
    Npc(const Npc&)=delete;
    ~Npc();

    void       save(Serialize& fout, size_t id, std::string_view directory);
    void       load(Serialize& fout, size_t id, std::string_view directory);
    void       postValidate();

    void       drawVobBox(DbgPainter& p) const;
    void       drawVobRay(DbgPainter& p, const Npc& npc) const;
    // Debug overlay of the shadow melee blade: its pose during a swing and the latest contacts.
    void       drawMeleeShadow(DbgPainter& p, uint64_t now) const;
    void       updateMeleeShadow();
    void       resolveMeleeContacts();
    static bool skipMeleeAlly(bool player, bool selectedTarget, bool sameGuild, bool sameParty, bool enemy) {
      return !player && !selectedTarget && (sameGuild || sameParty || !enemy);
      }

    bool       setPosition (float x,float y,float z);
    bool       setPosition (const Tempest::Vec3& pos);
    void       setDirection(const Tempest::Vec3& pos);
    void       setDirection(float rotation);
    void       setDirectionY(float rotation);
    void       setRunAngle  (float angle);

    auto       transform()  const -> Tempest::Matrix4x4;
    auto       position()   const -> Tempest::Vec3;
    auto       cameraBone(bool isFirstPerson = false) const -> Tempest::Vec3;
    auto       cameraMatrix(bool isFirstPerson = false) const -> Tempest::Matrix4x4;
    float      rotation() const;
    float      rotationRad() const;
    float      rotationY() const;
    float      rotationYRad() const;
    float      runAngle() const { return runAng; }
    float      fatness() const { return bdFatness; }
    Bounds     bounds() const;
    auto       bBox() const -> const Tempest::Vec3*;
    auto       bBoxCol() const -> const Tempest::Vec3*;

    void       stopDlgAnim();
    void       clearSpeed();
    bool       resetPositionToTA();
    // Stand-only reuse after death or an airborne/climbing episode timeout.
    void       resetNavigationEpisode();

    void       setProcessPolicy(NpcProcessPolicy t);
    auto       processPolicy() const -> NpcProcessPolicy { return aiPolicy; }

    bool       isPlayer() const;
    void       setWalkMode(WalkBit m);
    WalkBit    walkMode() const;
    void       tick(uint64_t dt);
    void       tickAnimationTags();
    bool       startClimb(JumpStatus jump);

    auto       world() -> World&;
    auto       world() const -> const World&;

    auto       centerPosition() const -> Tempest::Vec3;
    auto       collosionCenter() const -> Tempest::Vec3;
    Npc*       lookAtTarget() const;
    auto       portalName() -> std::string_view;
    auto       formerPortalName() -> std::string_view;

    float      qDistTo(const Tempest::Vec3 pos) const;
    float      qDistTo(const WayPoint* p) const;
    float      qDistTo(const Npc& p) const;
    float      qDistTo(const Interactive& p) const;
    float      qDistTo(const Item& p) const;
    auto       fightDistanceTo(const Npc& npc) const -> Tempest::Vec3;

    void       updateAnimation(uint64_t dt, bool force = false);
    void       updateTransform();

    std::string_view displayName() const;
    auto       displayPosition() const -> Tempest::Vec3;
    void       setVisual    (std::string_view visual);
    void       setVisual    (const Skeleton*  visual);
    bool       hasOverlay   (std::string_view sk) const;
    bool       hasOverlay   (const Skeleton*  sk) const;
    void       addOverlay   (std::string_view sk, uint64_t time);
    void       addOverlay   (const Skeleton*  sk, uint64_t time);
    void       delOverlay   (std::string_view sk);
    void       delOverlay   (const Skeleton*  sk);

    bool       toggleTorch();
    void       setTorch(bool use);
    bool       isUsingTorch() const;

    void       setVisualBody (int32_t headTexNr, int32_t teethTexNr,
                              int32_t bodyVer, int32_t bodyColor,
                              std::string_view body, std::string_view head);
    void       updateArmor   ();
    void       setSword      (MeshObjects::Mesh&& sword);
    void       setRangedWeapon(MeshObjects::Mesh&& bow);
    void       setShield     (MeshObjects::Mesh&& shield);
    void       setMagicWeapon(Effect&& spell);
    void       setSlotItem   (MeshObjects::Mesh&& itm, std::string_view slot);
    void       setStateItem  (MeshObjects::Mesh&& itm, std::string_view slot);
    void       setAmmoItem   (MeshObjects::Mesh&& itm, std::string_view slot);
    void       clearSlotItem (std::string_view slot);
    void       setPhysic     (DynamicWorld::NpcItem&& item);
    void       setFatness    (float f);
    void       setScale      (float x,float y,float z);

    bool       setAnim(Anim a);
    auto       setAnimAngGet(Anim a) -> const Animation::Sequence*;
    auto       setAnimAngGet(Anim a, uint8_t comb) -> const Animation::Sequence*;
    void       setAnimRotate(int rot);
    bool       setAnimItem(std::string_view scheme, int state);
    void       stopAnim(std::string_view ani);
    void       startFaceAnim(std::string_view anim, float intensity, uint64_t duration);
    float      faceWeight(std::string_view anim);  // how much of the face anim shows now, 0..1 (test bench)
    // a voiced line starts: the mouth follows the voice (LipSync)
    void       startSpeech(const Tempest::Sound& voice);
    void       startSpeech(std::shared_ptr<LipSync::Track> voice);
    void       setTalkMood(std::string_view mood);  // of the line it starts to say: its gestures follow it
    // the hero speaks to it (his voice) for the next `ms`; words of him came; it thinks of an answer for
    // the next `ms` (0: it answers now)
    void       listenTo(uint64_t ms);
    void       stopListening();
    void       heardWords();
    void       thinkOfAnswer(uint64_t ms);
    void       stopSpeech();
    bool       stopItemStateAnim();
    bool       hasAnim(std::string_view scheme) const;
    bool       hasAnim(Anim a) const;
    bool       hasSwimAnimations() const;
    bool       isFinishingMove() const;

    auto       animMoveSpeed(uint64_t dt) const -> Tempest::Vec3;

    bool       isJumpAnim() const;
    bool       isFlyAnim() const;
    bool       isFalling() const;
    bool       isFallingDeep() const;
    bool       isSlide() const;
    bool       isInAir() const;
    bool       isJump() const;
    bool       isJumpUp() const;
    bool       isStanding() const;
    bool       isSwim() const;
    bool       isInWater() const;
    bool       isDive() const;
    // Passive navigation-dataset readout; no movement or gaze state is changed.
    Tempest::Vec3 navigationGroundNormal() const { return mvAlgo.groundNormal(); }
    bool navigationClimbing() const { return mvAlgo.isClimb(); }
    bool       isCasting() const;

    void       setTalentSkill(Talent t,int32_t lvl);
    int32_t    talentSkill(Talent t) const;

    void       invalidateTalentOverlays(Talent t);
    void       invalidateTalentOverlays();

    void       setTalentValue(Talent t,int32_t lvl);
    int32_t    talentValue(Talent t) const;
    int32_t    hitChance(Talent t) const;

    void       setRefuseTalk(uint64_t milis);
    bool       isRefuseTalk() const;

    int32_t    mageCycle() const;
    bool       canSneak() const;
    int32_t    attribute (Attribute a) const;
    void       changeAttribute(Attribute a, int32_t val, bool allowUnconscious);
    int32_t    protection(Protection p) const;
    void       changeProtection(Protection p, int32_t val);

    uint32_t   instanceSymbol() const;
    uint32_t   guild() const;
    bool       isMonster() const;
    bool       isHuman() const;
    void       setTrueGuild(int32_t g);
    int32_t    trueGuild() const;
    int32_t    magicCyrcle() const;
    int32_t    level() const;
    int32_t    experience() const;
    int32_t    experienceNext() const;
    int32_t    learningPoints() const;
    int32_t    diveTime() const;

    void      setAttitude(Attitude att);
    Attitude  attitude() const { return permAttitude; }

    bool      isFriend() const;

    void      setTempAttitude(Attitude att);
    Attitude  tempAttitude() const { return tmpAttitude; }

    void      startDialog(Npc& other);
    bool      startState(ScriptFn id, std::string_view wp);
    bool      startState(ScriptFn id, std::string_view wp, gtime endTime, bool noFinalize);
    void      clearState(bool noFinalize);
    BodyState bodyState() const;
    BodyState bodyStateMasked() const;
    bool      hasState(BodyState s) const;
    bool      hasStateFlag(BodyState flg) const;
    bool      spellFrozen() const;

    void      setToFightMode(const size_t item);
    void      setToFistMode();

    void      aiPush(AiQueue::AiAction&& a);
    void      resumeAiRoutine();
    // external AI control (ai_bridge): routines and script perceptions are paused
    void      setExternalControl(bool e) { externalCtrl = e; }
    bool      isExternalControl() const  { return externalCtrl; }

    bool      canSwitchWeapon() const;
    bool      closeWeapon(bool noAnim);
    bool      drawWeaponFist();
    bool      drawWeaponMelee();
    bool      drawWeaponBow();
    bool      drawMage(uint8_t slot);
    bool      drawSpell(int32_t spell);
    void      emitDrawWeapon();
    auto      weaponState() const -> WeaponState;

    bool      canFinish(Npc &oth);
    void      fistShoot();
    bool      blockFist();
    bool      finishingMove();
    bool      isMeleeDownStrike(Anim anim, WeaponState ws) const;
    // Frames of the finishing chop's stroke down: from the top the blade's tip falls from to where it rests at its lowest.
    static std::optional<std::pair<uint32_t,uint32_t>> meleeFinishStroke(const std::vector<float>& tipHeight);
    static bool isFinishSequence(const Animation::Sequence& seq);
    void      swingSword();
    bool      swingSwordL();
    bool      swingSwordR();
    bool      blockSword();
    auto      beginCastSpell() -> BeginCastResult;
    void      endCastSpell(bool playerCtrl = false);
    void      setActiveSpellInfo(int32_t info);
    int32_t   activeSpellLevel() const;
    bool      aimBow();
    bool      shootBow(Interactive* focOverride = nullptr);
    bool      hasAmmunition() const;
    // an arrow (a bolt) is put in, ready to shoot
    bool      isBowLoaded() const;

    // the bow aimed by hand (game/archery): the hero's input drives it (PlayerControl)
    Archer&   archer() { return arch; }
    void      setManualAim(bool on, const Tempest::Vec3& point = {}, const Tempest::Vec2& sway = {},
                           const Tempest::Vec2& tremor = {});
    bool      hasManualAim() const { return manualAim; }
    // the stance aiming by hand: crouched 0..1, leaning out sideways -1 (left)..1 (right), the bow canted
    // for a quick shot 0..1, as smooth as whoever drives it makes it (PlayerControl); stepping -1 (back)..1
    // (forward)
    void      setAimStance(float crouch, float peek, float cant = 0, float walk = 0, bool stopWalk = false) {
      stanceCrouch = crouch; stancePeek = peek; stanceCant = cant; stanceWalk = walk;
      stanceWalkStopped = stopWalk;
      }
    float     aimCrouch() const { return stanceCrouch; }
    float     aimPeek()   const { return stancePeek; }
    float     aimCant()   const { return stanceCant; }
    float     aimWalk()   const { return stanceWalk; }
    bool      isAimWalkStopped() const { return stanceWalkStopped; }
    bool      isEdgeMoveBlocked(const Tempest::Vec3& modelDirection) const {
      return mvAlgo.isEdgeMoveBlocked(modelDirection);
      }
    // The manual stance looks from the hero's shoulder toward the crosshair, independently of the camera.
    auto      aimLookAt(const Tempest::Vec3& target) const -> Tempest::Vec3;
    void      setAimLook(const Tempest::Vec3& dir) { stanceLook = dir; }
    auto      aimLook()   const -> Tempest::Vec3 { return stanceLook; }
    // the animation of that name plays k times as fast from its next start (the reload by dexterity)
    void      setAnimSpeed(std::string_view name, float k) { visual.setAnimSpeed(name,k); }
    // how far the animation of that name has played, 0..1; -1: it does not play
    float     animProgress(std::string_view name) const;
    auto      manualAimPoint() const -> Tempest::Vec3 { return manualPoint; }
    auto      manualAimSway() const -> Tempest::Vec2 { return manualSway; }
    auto      manualAimTremor() const -> Tempest::Vec2 { return manualTremor; }
    bool      isAimSteady() const;
    float     aimTrackError() const { return visual.aimTrackError(); }
    // the arrow on the string as posed: its nock, where it points (the crosshair, the shot)
    bool      arrowRay(Tempest::Vec3& pos, Tempest::Vec3& dir) const;
    bool      aimAttachmentBounds(bool ammo, Tempest::Matrix4x4& matrix, Tempest::Vec3 (&bounds)[2]) const {
      return visual.aimAttachmentBounds(ammo,matrix,bounds);
      }
    // lets the arrow go along the arrow: drawn that far, thrown off by the release
    bool      releaseArrow(float draw, const Archery::Stats& st);
    // one frame of the bow aimed by hand at `at`, the hero's (PlayerControl) or an npc's (ArcherAi): the
    // caller fills draw, breath, the stance and dt; the turn, ready, reload and loaded are measured here,
    // the reload plays as fast as the stats say; true: an arrow left
    bool      tickManualAim(const Tempest::Vec3& at, Archer::Input in, const Archery::Stats& st);
    // the upper body can aim by hand (Aim on, the bones there): else an npc shoots by the dice
    bool      canAimByHand() const { return visual.isAimValid(); }
    ArcherAi& archerAi() { return archAi; }
    // the bow aimed by hand no more: the reload's tempo back, the string let down, no aim point; idle: back
    // to standing with the bow (the hero; an npc's fight plays its own animation)
    void      endManualAim(bool idle);
    // where an arrow passing from-to hits this one, t: how far from 'from'
    auto      rayHitZone(const Tempest::Vec3& from, const Tempest::Vec3& to, float& t, size_t* bone = nullptr) const -> HitZones::Zone;
    bool      hitZoneCenter(HitZones::Zone z, Tempest::Vec3& at) const;
    std::string hitZonesDump() const { return visual.hitZonesDump(); }
    void        hitZonesPosed(std::vector<HitZones::Posed>& out) const { visual.hitZonesPosed(out); }
    void        fitHitZones() { visual.fitHitZones(); }
    std::string bowDump() const { return visual.bowDump(); }
    std::string animInfo(std::string_view name) const { return visual.animInfo(name); }
    // ms the animation of that name takes as this one plays it (its overlays: the trained archers' reload)
    float       animTime(std::string_view name) const { return visual.animTime(name); }
    // the test bench: hit, it neither stumbles nor flinches (a target that keeps its pose)
    void        setBenchStill(bool s) { benchStill = s; }
    bool        isBenchStill() const { return benchStill; }

    bool      isEnemy(const Npc& other) const;
    bool      isDead() const;
    bool      isLie() const;
    bool      isUnconscious() const;
    bool      isDown() const;
    bool      isMeleeKnockedDown() const { return meleeKnockdown; }
    static bool canAccumulateMeleePosture(bool enabled, bool blocked, bool knockedDown, bool down) {
      return enabled && !blocked && !knockedDown && !down;
      }
    bool      isAttack() const;
    bool      isTalk() const;
    // whom this npc talks or listens to, for a while: it looks at them (gaze)
    void      setTalkPartner(Npc* other, uint64_t ms);
    Npc*      talkPartner() const;
    // where the gaze looks: the partner, where a script says, the hero's focus, what caught the eye
    auto      gazeTarget(Tempest::Vec3& at) const -> Gaze::Source;
    Npc*      attentionTarget() const { return attention.target(); }

    bool      isAttackAnim() const;
    auto      animNames() const -> std::string; // the playing animations, for debugging
    void      flinch(const Flinch::Hit& hit); // the upper body gives way to a blow (also the test bench)
    bool      isPrehit() const;
    bool      isImmortal() const;

    // Pure native state for the passive body observer. No queue materialization, RNG, animation
    // start or perception callback is performed by these probes.
    struct BodyObservationState final {
      uint32_t bodyState = 0;
      uint8_t weaponState = 0;
      uint32_t weaponSymbol = 0;
      int32_t weaponLength = 0;
      int32_t skill = 0;
      uint8_t skillTalent = 0;
      int32_t fightTactic = 0;
      int32_t primitive = 0;
      uint64_t choiceId = 0;
      uint64_t faiWaitMs = 0, aniWaitMs = 0, aiWaitMs = 0, outputWaitMs = 0, segmentRemainingMs = 0;
      bool faiGate = false, aniGate = false, aiGate = false, outputGate = false;
      bool attackAnim = false, prehit = false, defence = false, jumpBack = false;
      bool defWindow = false, parWindow = false, interruptible = false;
      bool nativeEligible = false;
      bool hasMove = false, hasLeft = false, hasRight = false, hasBack = false;
      bool hasAttack = false, hasAttackLeft = false, hasAttackRight = false, hasBlock = false;
      };
    struct BodyObservationTarget final {
      float fightDistanceCm = 0, attackRangeCm = 0, gRangeCm = 0, closeupRangeCm = 0;
      bool inWRange = false, inGRange = false, inCloseup = false, focus30 = false, focus5 = false;
      bool meleeLineClear = false;
      };
    auto      bodyObservationState() const -> BodyObservationState;
    auto      bodyObservationTarget(const Npc& target) const -> BodyObservationTarget;
    // Read-only debugging evidence; these privileged ray/pose fields are never actor inputs.
    void      traceVisibilityDiagnostic(std::string_view sourcePhase, bool force=false) noexcept;

    void      setPerceptionTime   (uint64_t time);
    void      setPerceptionEnable (PercType t, size_t fn);
    void      setPerceptionDisable(PercType t);

    bool      perceptionProcess(Npc& pl);
    bool      perceptionProcess(Npc& pl, Npc *victim, float quadDist, PercType perc, const std::shared_ptr<HeroDeed>& deed = {});
    // a perception held back by Gothic::interceptPerception, given to the scripts after all
    bool      passPerception(Npc& other, Npc* victim, PercType perc);
    // A physical impact has a location, independent of a known actor.
    bool      hearPhysicsNoise(const PhysicsNoise& noise);
    // A free NPC's brief investigation; normal perceptions and dialogue retain priority.
    bool      startPhysicsInvestigation(const Tempest::Vec3& at, uint64_t scanMs);
    bool      isPhysicsInvestigating() const { return physicsInvestigationUntil!=0; }
    std::string_view physicsInvestigationResult() const { return physicsInvestigationReason; }
    void      stopPhysicsInvestigation(bool resume, std::string_view reason="interrupted");
    bool      canHearPhysicsNoise() const;
    bool      hasPerc(PercType perc) const;
    float     perceptionRange(PercType perc) const;
    bool      canWitnessDeed(const Npc& culprit, const Npc* victim, PercType perc) const;
    // Transient command scope, deliberately absent from saves. Native NPC fights
    // and ordinary item transfers do not acquire a crime mark.
    void      setServerDeed(std::string_view kind, Npc* victim = nullptr, size_t count = 0,
                           std::string_view owner = {});
    void      clearServerDeed();
    bool      isServerDeed(std::string_view kind, const Npc* victim = nullptr) const;
    const char* serverDeedSimulationError();
    void      consumeServerAttack();
    void      setNonlethalFight(Npc& target);
    bool      isNonlethalFight(const Npc& other) const;
    uint64_t  percNextTime() const;

    auto      interactive() const -> Interactive* { return currentInteract; }
    bool      setInteraction(Interactive* id, bool quick=false);
    void      quitInteraction();
    void      processDefInvTorch();

    auto      detectedMob() const -> Interactive*;
    void      setDetectedMob(Interactive* id);

    bool      isInState  (ScriptFn stateFn) const;
    bool      isInRoutine(ScriptFn stateFn) const;
    bool      wasInState (ScriptFn stateFn) const;
    uint64_t  stateTime() const;
    void      setStateTime(int64_t time);

    void      addRoutine(gtime s, gtime e, uint32_t callback, std::string_view point);
    // a day the scripts did not give (ai_bridge, the director's npc's): entries added after this replace it
    void      clearRoutine();
    void      excRoutine(size_t callback);

    // the daily routine, read-only: for tools outside the scripts (the AI bridge)
    struct RoutineInfo final {
      gtime            start;
      gtime            end;
      ScriptFn         state;
      std::string_view point;
      };
    void      forEachRoutine(const std::function<void(const RoutineInfo&)>& f) const;
    ScriptFn  currentState() const { return aiState.funcIni; }
    // The initializer has run; installing funcIni alone does not start the state.
    bool      isStateStarted() const { return aiState.started; }
    Npc*      stateOther() const { return currentOther; }  // ai_bridge: whom the script state is about
    void      multSpeed(float s);

    bool      testMove(const Tempest::Vec3& pos);
    bool      tryMove(const Tempest::Vec3& dp);
    bool      tryMove     (const Tempest::Vec3& dp, DynamicWorld::CollisionTest& out);
    bool      tryTranslate(const Tempest::Vec3& to);
    bool      tryTranslate(const Tempest::Vec3&         to,
                           DynamicWorld::CollisionTest& out);

    JumpStatus tryJump();
    bool      hasCollision() const { return physic.hasCollision(); }
    bool      hasRagdoll() const { return ragdoll!=nullptr; }
    bool      isRagdollBlending() const { return visual.pose().isRagdollBlending(); }
    const Skeleton* visualSkeleton() const { return visual.visualSkeleton(); }
    const Ragdoll* ragdollBody() const { return ragdoll.get(); }
    const Ragdoll* livingBody() const { return living.get(); }
    enum class MeleeBenchMode : uint8_t { Off, Base, Full, Chain, Blade };
    void configureMeleeBench(MeleeBenchMode mode, bool dynamicPelvis);
    struct ArrowPhysicsProbe {
      size_t bone = size_t(-1);
      Tempest::Vec3 point, impulseNs;
      float strength = 0;
      uint32_t hits = 0, applied = 0, corpseImpulses = 0;
      };
    const ArrowPhysicsProbe& arrowPhysicsProbe() const { return arrowPhysics; }
    // the last blade or fist blow handed to the living rig
    const ArrowPhysicsProbe& meleeReactionProbe() const { return meleeReaction; }
    const Tempest::Matrix4x4& animationBone(size_t bone) const { return visual.pose().bone(bone); }
    const Tempest::Matrix4x4& renderedBone(size_t bone) const { return visual.renderedBone(bone); }
    using ArrowPinProbe = MdlVisual::ArrowPinProbe;
    size_t stuckArrowCount() const { return visual.stuckArrowCount(); }
    bool stuckArrowProbe(size_t index, ArrowPinProbe& out) const { return visual.stuckArrowProbe(index,out); }
    bool stickArrow(MeshObjects::Mesh&& arrow, size_t bone, const Tempest::Matrix4x4& worldMatrix,
                    std::string_view visualName={});
    void addCorpseArrowImpulse(const Bullet& bullet);
    const RagdollSave::QuestReturn& ragdollQuestReturnState() const { return ragdollQuestReturn; }
    uint32_t ragdollQuestReturnCount() const { return ragdollQuestReturns; }
    const Tempest::Vec3& ragdollQuestReturnFrom() const { return ragdollQuestFrom; }
    void      prepareRagdoll();
    void      prepareLivingBody();
    void      syncRagdoll();
    void      suspendRagdoll();
    void      finishMeleeShadowSwing(bool remove=true, bool discard=false);
    bool      setRagdollDown(bool death);
    void      addRagdollImpulse(const Tempest::Vec3& impulse);
    float     physicsMass() const;
    bool      characterVirtualActive() const { return mvAlgo.characterVirtualActive(); }
    Tempest::Vec3 characterVirtualGroundVelocity() const { return mvAlgo.characterVirtualGroundVelocity(); }
    void      finishCharacterVirtualSupport(uint64_t dt) { mvAlgo.carryCharacterVirtual(dt); }
    void      setCharacterVirtualDiagnosticsEnabled(bool enabled) { physic.setVirtualDiagnosticsEnabled(enabled); }
    bool      characterVirtualDiagnostics(CharacterVirtualDiagnostics& out) const { return physic.virtualDiagnostics(out); }
    Tempest::Vec3 characterVisualPosition() const;
    bool      addSpellImpulse(const Tempest::Vec3& impulseNs, bool reaction=true, Npc* culprit=nullptr);
    Tempest::Vec3 spellVelocity() const { return mvAlgo.spellVelocity(); }
    bool      spellWhirling() const;

    void      startDive();
    bool      transformInto(size_t instance); // shape change spell: another creature until transformBack
    void      transformBack();

    auto      dialogChoices(Npc &player, const std::vector<uint32_t> &except, bool includeImp) -> std::vector<GameScript::DlgChoice>;

    zenkit::INpc&                        handle() { return *hnpc; }
    const std::shared_ptr<zenkit::INpc>& handlePtr() const { return hnpc; }

    auto      inventory() const -> const Inventory& { return invent; }
    size_t    itemCount  (size_t id) const;
    Item*     activeWeapon();
    Item*     getItem    (size_t id);
    Item*     addItem    (size_t id, size_t amount);
    Item*     addItem    (std::unique_ptr<Item>&& i);
    Item*     takeItem   (Item& i);
    void      onWldItemRemoved(const Item& itm);
    void      delItem    (size_t id, uint32_t amount);
    void      useItem    (size_t item);
    void      useItem    (size_t item, uint8_t slotHint, bool force);
    void      setCurrentItem(size_t item);
    void      unequipItem(size_t item);
    void      addItem    (size_t id, Interactive& chest, size_t count=1);
    void      addItem    (size_t id, Npc& from,          size_t count=1);
    void      moveItem   (size_t id, Interactive& to,    size_t count=1);
    void      sellItem   (size_t id, Npc& to,            size_t count=1);
    void      buyItem    (size_t id, Npc& from,          size_t count=1);
    void      dropItem   (size_t id,                     size_t count=1);
    void      clearInventory();
    Item*     currentArmor();
    Item*     currentMeleeWeapon();
    Item*     currentRangedWeapon();
    Item*     currentShield();
    auto      mapWeaponBone() const -> Tempest::Vec3;
    auto      mapHeadBone() const -> Tempest::Vec3;
    struct EyePose {
      Tempest::Vec3 origin = {}, forward = {1,0,0}, right = {0,0,1}, up = {0,1,0};
      bool animatedHead = false;
      };
    // Reads the applied head bone, including animation, gaze and physical pose corrections.
    EyePose   navigationEyePose() const;
    auto      aimDirection(const Tempest::Vec3& target) const -> Tempest::Vec3;
    auto      aimLeanDirection(const Tempest::Vec3& target) const -> Tempest::Vec3;
    auto      aimHead(const Tempest::Vec3& target, float angle) const -> Tempest::Vec3;
    auto      aimCurrentHead() const -> Tempest::Vec3;
    auto      aimLeanShift() const -> Tempest::Vec3;
    auto      aimArrow(const Tempest::Vec3& target, float angle) const -> Tempest::Vec3;
    auto      aimEdge(const Tempest::Vec3& direction, float range=100.f) const -> Aim::LeanEdge;
    auto      aimLean(const Tempest::Vec3& target, bool manual=false) const -> Aim::LeanResult;
    auto      mapBone(std::string_view bone) const -> Tempest::Vec3;

    bool      turnTo  (float dx, float dz, bool noAnim, uint64_t dt);
    bool      rotateTo(float dx, float dz, float speed, AnimationSolver::TurnType anim, uint64_t dt);
    bool      whirlTo(float dx, float dz, float step, uint64_t dt);
    bool      isRotationAllowed() const;
    auto      playAnimByName(std::string_view name, BodyState bs) -> const Animation::Sequence*;
    bool      setPlayerPose(bool kneel);
    void      tickPeeStream();
    bool      tickPlayerPose();
    bool      isPlayerKneeling() const { return playerKneeling; }

    bool      checkGoToNpcdistance(const Npc& other);

    bool      isAiQueueEmpty() const;
    bool      isAiBusy() const;
    size_t    aiQueueSize() const { return aiQueue.size(); }
    void      clearAiQueue();

    auto      currentWayPoint() const -> const WayPoint* { return currentFp; }
    auto      currentGoToPoint() const -> const WayPoint* { return go2.wp; }
    auto      currentTaPoint() const -> const WayPoint*;
    void      attachToPoint(const WayPoint* p);
    GoToHint  moveHint() const { return go2.flag; }
    // Passive navigation telemetry: reads the active destination or a ready go-to queue head.
    struct NavigationGoal final {
      bool active = false;
      const WayPoint* point = nullptr;
      const Npc* npc = nullptr;
      Tempest::Vec3 position;
      std::string_view kind;
      std::string_view name;
      };
    auto      navigationGoal() const -> NavigationGoal;
    // Radius and full height of the landscape-query capsule, in game centimeters.
    auto      navigationCapsule() const -> Tempest::Vec2 {
      return {physic.capsuleRadius(),physic.capsuleHeight()};
      }
    float     navigationClearance() const {
      return physic.groundOffset()+physic.capsuleHeight()*0.5f;
      }
    // Direct, owned ground-position target for bench-controlled walks; no waypoint graph lookup.
    // Uses a 20 cm arrival radius even after a collision, to preserve navmesh corners.
    // Returns false while airborne or while leaving an interaction, so the caller can retry.
    bool      goToPosition(const Tempest::Vec3& position);
    void      clearGoTo();
    void      stopWalking();

    bool      canSeeNpc(const Npc& oth, bool freeLos) const;
    // with a bow, a crossbow, a spell drawn: one steeply below seen when leaning over an edge (the aim
    // leans the body that far), for the focus
    bool      canSeeNpcOverEdge(const Npc& oth) const;
    bool      canSeeItem(const Item& it, bool freeLos) const;
    bool      canSeeSource() const;
    bool      canRayHitPoint(const Tempest::Vec3 pos, bool freeLos = true, float extRange=0.f) const;
    bool      canRayHitPoint(const Tempest::Vec3 pos, float angOverride, float extRange=0.f) const;
    bool      canRayHitPoint(const Tempest::Vec3 self, const Tempest::Vec3 pos, float angOverride, float extRange=0.f) const;

    auto      canSenseNpc(const Npc& oth, bool freeLos, float extRange=0.f) const -> SensesBit;
    auto      canSenseNpc(const Tempest::Vec3 pos, bool freeLos, bool isNoisy, float extRange=0.f) const -> SensesBit;

    void      setTarget(Npc* t);
    Npc*      target() const;

    void      clearNearestEnemy();
    int32_t   lastHitSpellId() const { return lastHitSpell; }
    Npc*      lastHitBy() const { return lastHit; }  // who hit this npc last (ai_bridge: who killed it)
    void      setLastHitBy(Npc* by) { lastHit = by; }  // ai_bridge's debug kill: the killer it names

    void      setOther(Npc* ot);
    void      setVictim(Npc* ot);

    bool      haveOutput() const;
    uint64_t  speechLeft() const;      // ms of the line it says now (0: none)
    bool      hasOutputQueued() const; // a line waits in its ai queue
    bool      isPlayingQueuedAnim() const; // AI_PlayAni: a nod, a shrug...
    void      setAiOutputBarrier(uint64_t dt, bool overlay);

    bool      doAttack(Anim anim, BodyState bs);
    void      commitSpell();
    // ignoreDefence is reserved for a confirmed contact in the devmode npc_hit fixture.
    void      takeDamage(Npc& other, const Bullet* b, bool ignoreDefence = false, float meleeScale = 1.f, const MeleeBladeContact* contact = nullptr);
    void      takeDamage(Npc& other, const Bullet* b, const VisualFx* vfx, int32_t splId);
    bool      isTargetableBySpell(TargetType t) const;

    void      emitSoundEffect(std::string_view sound, float range, bool freeSlot);
    void      emitSoundGround(std::string_view sound, float range, bool freeSlot);
    void      emitSoundSVM   (std::string_view sound);

    void      startEffect(Npc& to, const VisualFx& vfx);
    void      stopEffect (const VisualFx& vfx);
    void      runEffect  (Effect&& e);

  private:
    struct Routine final {
      gtime            start;
      gtime            end;
      ScriptFn         callback;
      const WayPoint*  point = nullptr;
      std::string      fallbackName;

      std::string_view wayPointName() const;
      };

    enum TransformBit : uint8_t {
      TR_Pos  =1,
      TR_Rot  =1<<1,
      TR_Scale=1<<2,
      };

    struct AiState final {
      ScriptFn funcIni;
      ScriptFn funcLoop;
      ScriptFn funcEnd;
      uint64_t sTime   =0;
      gtime    eTime   =gtime::endOfTime();
      bool     started =false;
      uint64_t loopNextTime=0;
      const char* hint="";
      };

    struct Perc final {
      ScriptFn func;
      };

    struct GoTo final {
      GoToHint         flag = GoToHint::GT_No;
      Npc*             npc  = nullptr;
      const WayPoint*  wp   = nullptr;
      Tempest::Vec3    pos  = {};
      float            pointDistance = 0.f; // transient bench precision; zero keeps normal tolerance
      Npc*             nativeCombatTarget = nullptr; // transient provenance; not a script goto

      void             save(Serialize& fout) const;
      void             load(Serialize&  fin);
      Tempest::Vec3    target() const;
      bool             isClose(const Npc& self, float dist) const;

      bool             empty() const;
      void             clear();
      void             set(Npc* to, GoToHint hnt = GoToHint::GT_Way);
      void             set(const WayPoint* to, GoToHint hnt = GoToHint::GT_Way);
      void             set(const Item* to);
      void             set(const Tempest::Vec3& to);
      void             setFlee();
      };

    void      updateWeaponSkeleton();
    void      tickTimedEvt(Animation::EvCount &ev);
    void      tickRegen(int32_t& v,const int32_t max,const int32_t chg, const uint64_t dt);
    void      setViewPosition(const Tempest::Vec3& pos);
    void      syncCharacterVirtualSupport(const Tempest::Vec3& pos);
    bool      tickCast(uint64_t dt);
    bool      tickPhysicsInvestigation(uint64_t dt);

    int       aiOutputOrderId() const;
    bool      performOutput(const AiQueue::AiAction &ai);

    auto      currentRoutine(bool assertWp = false) const -> const Routine&;
    gtime     endTime(const Routine& r) const;

    bool      implPointAt(const Tempest::Vec3& to);
    bool      implLookAtWp(uint64_t dt);
    bool      implLookAtNpc(uint64_t dt);
    void      implHeadBack(uint64_t dt);
    bool      implLookAt (float dx, float dy, float dz, uint64_t dt);
    bool      implTurnAway(const Npc& oth, uint64_t dt);
    bool      implTurnToFai(const Npc& oth, uint64_t dt);
    bool      implTurnTo (const Npc& oth, uint64_t dt);
    bool      implTurnTo (const Npc& oth, AnimationSolver::TurnType anim, uint64_t dt);
    bool      implTurnTo (const WayPoint* wp, AnimationSolver::TurnType anim, uint64_t dt);
    bool      implTurnTo (float dx, float dz, AnimationSolver::TurnType anim, uint64_t dt);
    bool      implWhirlTo(const Npc& oth, uint64_t dt);
    bool      implGoTo   (uint64_t dt);
    bool      implGoTo   (uint64_t dt, float destDist);
    bool      implAttack (uint64_t dt, bool bodyOverride=false, FightAlgo::Action bodyAction=FightAlgo::MV_NULL);
    void      traceCombatSnapshot();
    struct BodyMovementTrace;
    void      traceCombatGate(std::string_view reason);
    void      traceCombatAttempt(Anim requested, bool accepted, const std::string& before, std::string_view hook);
    std::string combatPoseIdentity() const;
    std::string combatPoseLayers() const;
    bool      implAiTick (uint64_t dt);
    void      implAiWait (uint64_t dt);
    void      implAniWait(uint64_t dt);
    void      implFaiWait(uint64_t dt);
    void      implSetFightMode(const Animation::EvCount& ev);
    bool      implAiFlee(uint64_t dt);

    bool      setGoToLadder();

    bool      isPlayerEnabledState(const ::AiState& st) const;
    void      tickRoutine();
    int       invokeRoutine(ScriptFn function);
    void      nextAiAction(AiQueue& queue, uint64_t dt);
    bool      startQueuedState(const AiQueue::AiAction& act);
    void      commitDamage();
    Npc*      updateNearestEnemy();
    Npc*      updateNearestBody();
    bool      checkHealth(bool onChange, bool forceKill);
    void      onNoHealth(bool death, HitSound sndMask);
    bool      hasAutoroll() const;
    bool      prepareTurn();
    void      stopWalkAnimation();
    void      takeDamage(Npc& other, const Bullet* b, const CollideMask bMask, int32_t splId, bool isSpell, float meleeScale = 1.f, const MeleeBladeContact* contact = nullptr);
    void      hitReaction(const Npc& other, const Bullet* b, int32_t damage, bool stumble, const MeleeBladeContact* contact = nullptr);
    void      takeFallDamage(const Tempest::Vec3& fallSpeed);
    void      takeDrownDamage();

    void      dropTorch(bool burnout = false);

    void      saveAiState(Serialize& fout) const;
    void      loadAiState(Serialize& fin);
    void      saveTrState(Serialize& fout) const;
    void      loadTrState(Serialize& fin);

    static float angleDir(float x,float z);

    uint8_t   calcAniComb() const;
    uint64_t  perceptionTimeClampt() const;

    bool               isAlignedToGround() const;
    Tempest::Vec3      groundNormal() const;
    Tempest::Matrix4x4 mkPositionMatrix() const;
    void               resetCharacterVisualHeight();
    void               updateCharacterVisualHeight();
    bool               updateLyingVisualHeight();
    void               releaseRagdoll(bool blend);
    bool               recoverRagdoll(bool force = false);
    bool               createRagdoll();
    void               updateRagdollQuestAnchor();
    void               checkRagdollQuestHazard();
    bool               needsRagdollQuestReturn() const;
    bool               recoverSpellThrow();
    void               queueArrowPhysics(const Bullet& bullet);
    void               dispatchArrowPhysics(Ragdoll& body);
    bool               queueMeleeReaction(const Npc& other, const Tempest::Vec3& at, const MeleeBladeContact* contact);
    Tempest::Vec3      meleeImpulse(const Npc& attacker, const MeleeBladeContact* contact) const;
    bool               hasQueuedPhysicalSpellState(const AiQueue& queue) const;

    World&                         owner;
    // Debug trace cache only: neither serialized nor used by native AI/animation decisions.
    std::string                    bodyTraceState, bodyTraceGate;
    uint64_t                       bodyTraceSampleTime = 0;
    bool                           bodyTraceMovementActive = false;
    uint64_t                       visibilityDiagnosticGeneration=0, visibilityDiagnosticCount=0;
    std::array<uint64_t,2>          visibilityDiagnosticTime={};
    std::array<std::string,2>       visibilityDiagnosticKey;
    // main props
    std::shared_ptr<zenkit::INpc>  hnpc={};
    float                          x=0.f;
    float                          y=0.f;
    float                          z=0.f;
    float                          angle    = 0.f;
    float                          sz[3]={1.f,1.f,1.f};

    // visual props
    std::string                    body,head;
    int32_t                        vHead = 0, vTeeth = 0, vColor = 0;
    int32_t                        bdColor = 0;
    float                          bdFatness = 0;
    MdlVisual                      visual;

    // visual props (cache)
    uint8_t                        durtyTranform=0;
    Tempest::Vec3                  lastGroundNormal;
    // Transient presentation only: unexplained grounded height changes fade in 100 ms.
    struct CharacterVisualStep {
      uint64_t tick = 0;
      float    offset = 0;
      };
    std::vector<CharacterVisualStep> characterVisualSteps;
    float                          characterVisualYOffset = 0;
    // Transient placement of an animated lying mesh; never changes the walking root.
    float                          lieVisualYOffset = 0;
    Tempest::Vec3                  characterVisualSamplePosition;
    Tempest::Vec3                  characterVisualSampleNormal;
    uint64_t                       characterVisualSampleTick = 0;
    bool                           characterVisualHasSample = false;
    bool                           characterVisualGroundSample = false;

    DynamicWorld::NpcItem          physic;
    std::unique_ptr<Ragdoll>       living;
    MeleeBenchMode                meleeBench = MeleeBenchMode::Off;
    bool                          meleeBenchDynamicPelvis = false;
    uint64_t                      livingRetryTick = 0;
    bool                          livingNeedsRebuild = false;
    bool                          livingWasFrozen = false;
    bool                          livingWasInteracting = false;
    MeleeBenchMode meleeBenchMode() const;
    void updateMeleeBenchBlade();
    struct MeleeShadow;
    struct MeleeReach;
    mutable std::unique_ptr<MeleeShadow> meleeShadow;
    bool                          meleeShadowBody = false;
    bool                          meleeShadowSuspended = false;
    struct MeleeShadowMark final {
      Tempest::Vec3 point;
      uint64_t      time  = 0;
      bool          world = false;
      std::string   label;
      };
    std::vector<MeleeBladeSegment> meleeShadowDrawPose;
    bool                          meleeShadowDrawActive = false;
    std::vector<MeleeShadowMark>  meleeShadowMarks;
    void          noteMeleeShadowMarks(const std::vector<MeleeBladeContact>& contacts);
    void syncMeleeShadowSwing();
    void cacheMeleeShadowWindows(const Animation::Sequence* sequence, float length, bool normalY) const;
    MeleeReach sampleMeleeReach(const Npc& target, Pose& sampled, const Pose::CombatLayer& attack, float yawDelta) const;
    void consumeMeleeContacts();
    void meleeHitStop(Npc& target, const MeleeBladeContact& hit, float multiplier, bool blocked);
    void stopMeleeSwing();
    void noteMeleeStep(const Tempest::Vec3& root, const Tempest::Vec3& offered, const Tempest::Vec3& moved, uint64_t dt);
    auto meleeStep(uint64_t dt) -> Tempest::Vec3;
    void clearMeleeShadow();
    ArrowPhysicsProbe             arrowPhysics;
    ArrowPhysicsProbe             meleeReaction;
    struct ArrowImpact {
      size_t bone;
      Tempest::Vec3 point, impulseNs;
      float strength;
      std::weak_ptr<zenkit::INpc> culprit;
      bool melee = false;
      float stiffness = 1.f;
      };
    std::vector<ArrowImpact>       arrowImpacts;
    Tempest::Vec3                  pendingSpellImpulse = {};
    std::weak_ptr<zenkit::INpc>     spellImpulseCulprit;
    std::vector<Tempest::Matrix4x4> arrowPhysicalPose;
    std::unique_ptr<Ragdoll>       ragdoll;
    std::optional<Ragdoll::State>  ragdollDormant;
    std::vector<Tempest::Matrix4x4> ragdollPose;
    Tempest::Matrix4x4             ragdollObjectMatrix;
    Tempest::Vec3                  ragdollLastPosition;
    Tempest::Vec3                  ragdollLastVelocity;
    uint64_t                      ragdollLastTick = 0;
    bool                          ragdollHasLastPosition = false;
    uint64_t                      ragdollStartTick = 0;
    bool                          ragdollLegacyDown = false;
    bool                          ragdollRecovering = false;
    void                          applyMeleePosture(Npc& attacker, const MeleeBladeContact* contact, bool blocked, float before);
    bool                          tickMeleePosture(uint64_t dt);
    MeleeImpact::Posture           meleePosture;
    bool                          meleeKnockdown = false;
    bool                          meleeDownStrike = false; // the chop from above at one lying is playing
    bool                          meleeGetupForced = false;
    uint64_t                      meleeFallTick = 0, meleeRiseTick = 0;
    uint64_t                      meleeSlowSince = 0;
    uint32_t                      meleeReturnCount = 0;
    Tempest::Vec3                 meleeSavePosition;
    RagdollSave::QuestReturn       ragdollQuestReturn;
    bool                          ragdollQuestSpellThrow = false;
    uint64_t                      ragdollQuestSampleTick = 0;
    uint32_t                      ragdollQuestReturns = 0;
    Tempest::Vec3                 ragdollQuestFrom;

    WalkBit                        wlkMode                 =WalkBit::WM_Run;
    int32_t                        trGuild                 =GIL_NONE;
    int32_t                        talentsSk[TALENT_MAX_G2]={};
    int32_t                        talentsVl[TALENT_MAX_G2]={};
    uint64_t                       refuseTalkMilis         =0;

    // attitude
    Attitude                       permAttitude=ATT_NULL;
    Attitude                       tmpAttitude =ATT_NULL;

    // perception
    uint64_t                       perceptionTime    =0;
    uint64_t                       perceptionNextTime=0;
    Perc                           perception[PERC_Count];
    // Transient: hearing neither changes save format nor needs to survive a load.
    Tempest::Vec3                   physicsNoisePosition;
    uint64_t                       physicsNoiseUntil = 0;
    uint64_t                       physicsNoiseNextTime = 0;
    bool                           physicsNoiseCallback = false;
    Tempest::Vec3                  physicsInvestigationPosition;
    uint64_t                       physicsInvestigationUntil = 0;
    uint64_t                       physicsInvestigationScanUntil = 0;
    uint64_t                       physicsInvestigationScanMs = 0;
    float                          physicsInvestigationFacing = 0;
    bool                           physicsInvestigationWalking = false;
    std::string_view               physicsInvestigationReason;

    // inventory
    Inventory                      invent;
    bool                           invTorch = false;

    // last hit
    Npc*                           lastHit          = nullptr;
    char                           lastHitType      = 'A';
    int32_t                        lastHitSpell     = 0;

    // spell cast
    CastState                      castLevel        = CS_NoCast;
    int32_t                        manaInvested     = 0;
    int32_t                        aiExpectedInvest = 1;
    size_t                         currentSpellCast = size_t(-1);
    uint64_t                       castNextTime     = 0;
    int32_t                        spellInfo        = 0;

    // transform-backshape
    struct TransformBack;
    std::unique_ptr<TransformBack> transformSpl;

    // ai state
    uint64_t                       aniWaitTime=0;
    uint64_t                       waitTime=0;
    uint64_t                       faiWaitTime=0;
    uint64_t                       outWaitTime=0;

    uint64_t                       aiOutputBarrier=0;
    NpcProcessPolicy               aiPolicy=NpcProcessPolicy::AiNormal;
    AiState                        aiState;
    ScriptFn                       aiPrevState;
    AiQueue                        aiQueue;
    AiQueue                        aiQueueOverlay;
    Npc*                           nativeCombatQueueTarget=nullptr; // only during this actor's ZS_ATTACK callback
    bool                           playerKneeling=false;
    std::unique_ptr<Effect>        peeStream;
    bool                           playerPoseExiting=false;
    bool                           externalCtrl=false; // transient, not serialized
    bool                           percPassThrough=false; // passPerception: no interception
    std::vector<Routine>           routines;

    Interactive*                   currentInteract=nullptr;
    Npc*                           currentOther   =nullptr;
    Npc*                           currentVictim  =nullptr;

    const WayPoint*                currentLookAt=nullptr;
    Npc*                           currentLookAtNpc=nullptr;
    Npc*                           talkWith        =nullptr;
    uint64_t                       talkUntil       =0;
    Attention                      attention;
    std::optional<Tempest::Vec2>    headInput; // transient yaw/pitch for this tick, no AI ownership
    Archer                         arch;
    ArcherAi                       archAi;
    Tempest::Vec3                  aimDirPrev  = {};  // the manual aim's last direction (a turn shakes it)
    float                          reloadTempo = 1;   // how fast the reload plays (set on the model)
    bool                           manualAim   = false;
    Tempest::Vec3                  manualPoint = {};
    Tempest::Vec2                  manualSway  = {};
    Tempest::Vec2                  manualTremor = {};
    float                          stanceCrouch = 0;
    float                          stancePeek   = 0;
    float                          stanceCant   = 0;
    float                          stanceWalk   = 0;
    bool                           stanceWalkStopped = false;
    Tempest::Vec3                  stanceLook   = {};
    bool                           benchStill   = false;
    Npc*                           currentTarget  =nullptr;
    std::string                    serverDeedKind;
    const Npc*                     serverDeedVictim = nullptr;
    bool                           serverAttackRecorded = false;
    bool                           serverDeedSimulationLost = false;
    size_t                         serverDeedCount = 0;
    std::string                    serverDeedOwner;
    size_t                         nonlethalTarget = size_t(-1);
    uint64_t                       nonlethalUntil = 0;
    Npc*                           nearestEnemy   =nullptr;
    AiOuputPipe*                   outputPipe     =nullptr;

    Tempest::Vec3                  moveMobCacheKey={};
    Interactive*                   moveMob        =nullptr;

    GoTo                           go2;
    const WayPoint*                currentFp      =nullptr;
    FpLock                         currentFpLock;
    WayPath                        wayPath;

    MoveAlgo                       mvAlgo;
    FightAlgo                      fghAlgo;
    uint64_t                       lastEventTime=0;

    float                          angleY   = 0.f;
    float                          runAng   = 0.f;

    Sound                          sfxWeapon;

  friend class MoveAlgo;
  friend class BodyController;
  friend class BodyArena;
  friend class BodyTrajectory;
  friend class NpcKeys;
  friend bool TestBench::ragdollBodyDrawable(const Npc& npc);
  };
