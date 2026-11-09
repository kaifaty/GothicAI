#pragma once

#include <Tempest/Matrix4x4>
#include <Tempest/Vec>
#include <Tempest/Point>

#include <condition_variable>
#include <mutex>
#include <thread>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <functional>
#include <map>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "ui/crosshair.h"
#include "resources.h"
#include "game/constants.h"
#include "game/meleedodge.h"
#include "game/ragdollsave.h"
#include "graphics/objvisual.h"
#include "graphics/mesh/hitzones.h"
#include "graphics/mesh/aim.h"
#include "graphics/mesh/gaze.h"
#include "world/objects/archer.h"
#include "game/inventory.h"
#include "physics/charactervirtualdiagnostics.h"
#include "physics/dynamicworld.h"
#include "physics/physics.h"
#include "physics/physicmesh.h"
#include "physics/physicmeshshape.h"
#include "physics/ragdoll.h"
#include "utils/keycodec.h"
#include "world/waypoint.h"

class Camera;
namespace Tempest { class Pixmap; }

class Interactive;
class MoveTrigger;
class Marvin;
class Npc;
class PlayerControl;
class Skeleton;
class Sound;
class World;
struct HeroDeed;
enum PercType : uint8_t;

class ClothBulletBenchDriver final {
  public:
    static void step(DynamicWorld& world,DynamicWorld::BulletBody& bullet,const Tempest::Vec3& direction,uint64_t dt) {
      world.moveBullet(bullet,direction,dt);
      }
  };

namespace testbench_detail {

struct Hold {
  KeyCodec::Action a;
  uint64_t         until;
  };

struct Spot {
  std::string name;
  float       angle = 0;
  float       rough = 0;
  float       dist  = 0;
  Tempest::Vec3 pos;
  };

enum SpotKind { K_Slope, K_Stairs, K_Name };

enum CamMode : uint8_t {
  C_Game,
  C_Follow, // around the player, relative to its facing
  C_World,  // around the player, fixed world direction
  C_Fixed,  // fixed position, looking at the player
  C_Hold,   // the camera stays as it is: the position and where it looks
  C_Turn,   // yaw at the held camera position, retaining its pitch
  C_Water,  // retain the water overview after deferred player teleports
  C_RagdollArena, // benchmark-only fixed view derived from the complete twenty-model group
  C_Face,   // close to the face, relative to the facing of the body
  C_Shoulder, // over the hero's shoulder at an npc (camNpc)
  C_NpcFace,  // the npc's face, from the hero's side
  C_Two,      // the hero and the npc from the side
  C_Arrow,    // from the side of the newest arrow's way, going along with it (overlay trail records it)
  };

inline constexpr int Fps = 30;

struct Rgba { uint8_t r,g,b,a; };
extern const Rgba Text;

struct Canvas {
  uint8_t* px = nullptr;
  int      w  = 0;
  int      h  = 0;

  void blend(int x, int y, Rgba c);
  void rect(int x, int y, int rw, int rh, Rgba c);
  void dot(float x, float y, int r, Rgba c);
  void line(float x0, float y0, float x1, float y1, int r, Rgba c);
  // a polyline stroked as the game strokes the crosshair: white over a dark edge, smooth; each pixel once
  void stroke(const std::vector<Tempest::Vec2>& pt, float alpha, const Crosshair::Pen& pen);
  int text(int x, int y, std::string_view s, int sc, Rgba c);
};

// frames go to ffmpeg from a thread of their own: the game goes on while a frame is written
class VideoPipe final {
  public:
    explicit VideoPipe(std::FILE* f);
    ~VideoPipe();
    void push(std::vector<uint8_t>&& frame);

  private:
    static constexpr size_t MaxQueued = 4;
    void loop();

    std::FILE*                       f;
    std::mutex                       sync;
    std::condition_variable          cv;
    std::deque<std::vector<uint8_t>> queue;
    bool                             done = false;
    std::thread                      th;
};

struct QuestSite { bool ready=false; Tempest::Vec3 dry={}, direction={}; float waterY=0; std::string waypoint; };

struct LivingWalker;

struct Melee {
  // `dodge ... before <attacker> <ms>`: the melee dodge waits for the attacker's next blow to come that near
  struct DodgeArm {
    Npc*                  npc      = nullptr;
    Npc*                  attacker = nullptr;
    MeleeDodge::Direction dir      = MeleeDodge::Direction::Back;
    uint64_t              leadMs   = 0;
    uint64_t              until    = 0;
    };
  std::optional<DodgeArm> dodgeArm;

  // Respond to an actual native AI charge, leaving its decision and RNG untouched.
  struct MeleeAiResponse {
    Npc* npc = nullptr;
    Npc* attacker = nullptr;
    bool parry = false;
    uint64_t until = 0;
    };
  std::optional<MeleeAiResponse> meleeAiResponse;

  bool meleeFixture=false,meleeMeasuring=false,meleeSteady=false,meleeBrawl=false;
  uint32_t meleeAttackStarts=0;
  std::string meleeMode,meleePelvis;
  uint64_t meleeMotionFrom=0;
  std::vector<Npc*> meleeShadowActors;
  std::vector<Tempest::Vec3> meleeShadowPlaces;
  Tempest::Vec3 meleeShadowWall,meleeShadowForward;
  std::string meleeShadowWeapon;
};

struct MeleeContext {
  Melee& melee;
  std::vector<Npc*>& crew;
  std::deque<LivingWalker>& livingWalkers;
  std::vector<Tempest::Vec3>& ragdollArenaPlaces;
  Tempest::Vec3& ragdollArenaTarget;
};

struct Routine {
  QuestSite   questCliff,questWater;
  Npc*        questNpc = nullptr;
  std::string questCase;
  Tempest::Vec3        questStable = {};
  bool        questTrace=false,questWaterCase=false,questSawDrop=false,questSawWater=false,questWait=false,questLaunched=false,questStandingWait=false;
  uint64_t    questDeadline=0,questWallNs=0;
  uint32_t    questLastReturns=0;
  struct RoutineReturnCase {
    std::string label, symbol, type;
    Tempest::Vec3 start;
    bool failed=false;
    Npc* npc=nullptr;
    uint32_t npcId=uint32_t(-1);
    int32_t hp=0;
    std::vector<uint8_t> fixture;
    };
  std::vector<RoutineReturnCase> routineCases;
  std::vector<size_t> routineRetries;
  size_t routineIndex=0, routineRetryIndex=0;
  bool routineRetry=false;
  Npc* routineNpc=nullptr;
  int routinePhase=0;
  uint64_t routineSince=0, routineUp=0, routineFrame=0, routineDeadline=90000;
  uint32_t routineReturns=0, routineGuardBase=0, routineJumps=0, routineMobJumps=0, routineClamps=0;
  RagdollSave::QuestReturn routinePreviousGuard;
  Tempest::Vec3 routinePrevious, routineUpPosition;
  Interactive* routinePreviousMob=nullptr;
  float routineTravel=0, routineMaxStep=0, routineMinHero=0, routineRouteLength=0;
  bool routineBelow=false, routineHasRoute=false;
  std::string routinePathFirst, routinePathLast, routinePathSample;
};

struct RoutineContext {
  Routine& routine;
  Npc*& pinned;
  Tempest::Vec3& pinAt;
  std::FILE*& csv;
  Npc*& subject;
  uint64_t& time;
  Npc& (*subjectOf)(Npc&);
};

struct OwnMesh {
  uint32_t ownMeshZen=uint32_t(-1);
  Tempest::Vec3 ownMeshAxis,ownMeshApproach,ownMeshHeroStart,ownMeshBodyStart;
  float ownMeshMaxCrossTrack=0;
  std::deque<std::pair<uint64_t,Tempest::Vec3>> ownMeshHeroHistory;
  bool ownMeshWalking=false;
  unsigned ownMeshArrowHits=0;
  struct OwnMeshChest {
    Interactive* chest=nullptr;
    size_t lid=size_t(-1);
    bool opened=false,sawTransition=false,taken=false;
    std::string lastAnimation;
    std::map<size_t,size_t> contents,heroBefore;
  } ownMeshChest;
};

struct OwnMeshContext {
  OwnMesh& ownMesh;
  CamMode& cam;
  bool& camAuto;
};

struct RagdollBench {
  Npc*        waitRagdollNpc = nullptr;
  std::string waitRagdollState;
  uint64_t    waitRagdollEnd = 0;
  bool        ragdollChurn = false;
  uint64_t    ragdollPulseFrame = 0;
  std::vector<Tempest::Vec3> ragdollArenaPlaces;
  std::vector<float> ragdollArenaRadii;
  Tempest::Vec3        ragdollArenaTarget;
  std::string ragdollArenaWaypoint;
  bool        ragdollArenaCamera = false;
  std::string ragdollCoverageMode;
  size_t      ragdollCoverageFrames = 0;
  bool        ragdollCoverageActive = false;
  bool        ragdollCoverageFailed = false;
  struct BirthLive { Tempest::Vec3 position; uint64_t tick; bool inAir; };
  Npc*        birthActor = nullptr;
  std::string birthLabel;
  std::deque<BirthLive> birthHistory;
  Npc*        birthContinuation = nullptr;
  std::string birthContinuationLabel;
  uint64_t    birthTick = 0;
  const Npc*  expectedRagdollLoot = nullptr;
  bool        ragdollLootDidOpen = false;
  struct PerfSample {
    std::string segment;
    uint64_t frame = 0;
    float wallMs = 0, stepMs = 0;
    size_t worldBodies = 0, actors = 0, corpses = 0, physical = 0, sleeping = 0, parts = 0, centersInView = 0, visibleMeshes = 0;
    size_t physicalBeforeImpulse = 0, sleepingBeforeImpulse = 0;
    uint64_t rays = 0;
    };
  std::vector<PerfSample> ragdollPerformance;
  std::vector<PerfSample> ragdollCoverage;
  std::chrono::steady_clock::time_point lastPerfFrame;
  bool        hasPerfFrame = false;
  float       perfWallMs = 0;
  size_t      perfPhysicalBeforeImpulse = 0, perfSleepingBeforeImpulse = 0;
  struct SavedNpc {
    Npc* npc = nullptr;
    std::vector<uint8_t> bytes;
    std::optional<Ragdoll::State> body;
    std::vector<std::pair<std::string,Tempest::Vec3>> bones;
    Tempest::Vec3 position;
    int hp = 0;
    bool dead = false, unconscious = false, enabled = false;
    };
  std::unordered_map<std::string,SavedNpc> savedRagdollNpcs;
  };

struct RagdollContext {
  RagdollBench& ragdoll;
  std::vector<Npc*>& crew;
  Tempest::Vec3& camPos;
  std::FILE*& csv;
  Npc*& subject;
  std::unordered_map<const Npc*,uint64_t>& bodyIds;
  uint64_t& nextBodyId;
  uint64_t& step;
  Npc*& questNpc;
  uint64_t& recFrame;
  std::string& recName;
  const bool& ragdollTrace;
  const bool& ragdollPerf;
  Npc& (*subjectOf)(Npc&);
};

struct WorldBench {
  std::string perfSegment;
  uint64_t    perfFrame = 0;
  bool        perfArmed = false;
  bool        waitStreaming = false;
  std::chrono::steady_clock::time_point streamingDeadline;
  // `deterministic on`: a `shot` does not depend on how many frames the loading took
  // (reports/engine/2026-11-03-streaming-optimisation/deterministic-shots.md)
  bool        deterministic = false;
  uint64_t streamingNearBefore = 0, streamingFarBefore = 0;
  uint64_t streamingPublishedBefore = 0, streamingReleasedBefore = 0;
  Resources::TextureStats streamingTexturesBefore;
  uint64_t streamingVobCreatedBefore = 0, streamingVobReleasedBefore = 0;
  double streamingVobMissingBefore = 0;
  uint64_t streamingNpcTransitionsBefore = 0;
#ifdef OPENGOTHIC_ENABLE_APP_CADENCE
  bool        cadenceVerified=false,cadenceWait=false;
  uint32_t    cadenceDown=0;
#endif
  };

struct WorldContext {
  WorldBench& world;
  RagdollBench& ragdoll;
  std::vector<Npc*>& crew;
  Npc*& subject;
  uint64_t& time;
  uint64_t& waitUntil;
  const Tempest::Vec3& camPos;
  Npc& (*subjectOf)(Npc&);
  };

struct SpellActor {Npc* npc=nullptr;std::string role;};
struct MovableFixture { ObjVisual visual; DynamicWorld::Item body; };

struct LivingPhysics { uint64_t time=0;uint64_t dt=0;float milliseconds=0;size_t bodies=0; };

struct ClothBench {
  uint32_t clothZen=uint32_t(-1);
  std::string clothCase,clothObserverSupport="native-floor";
  Tempest::Vec3 clothCenter,clothDirection=Tempest::Vec3(1,0,0);
  bool clothTrace=false,clothMeasuring=false,clothOverflow=false,clothHold=false;
  Tempest::Vec3 clothHeldPosition;
  uint64_t clothSample=uint64_t(-1);
  std::vector<LivingPhysics> clothPhysics;
  std::unordered_map<uintptr_t,std::vector<Tempest::Vec3>> clothPrevious;
  std::unordered_map<uintptr_t,bool> clothPreviousActive;
  const void* clothArrow=nullptr;
  Tempest::Vec3 clothShotTarget,clothShotNormal;
  float clothArrowDownstream=0;
  uint64_t clothShotAt=0,clothShotHitsBefore=0;
  std::string clothShotKind;
  Npc* clothArrowNpc=nullptr;
};

struct ClothContext {
  ClothBench& cloth;
  CamMode& cam;
  Tempest::Vec3& camPos;
  Tempest::PointF& camHeldAngles;
  Tempest::PointF& camTurnAngles;
  float& camTurnDegrees;
  uint64_t& camTurnStart;
  uint64_t& camTurnDuration;
  Npc*& subject;
  std::vector<Npc*>& crew;
  uint64_t& time;
  uint64_t& waitUntil;
  void (*updateCamera)(World&,Npc&);
};

struct WaterFilm {
  std::string waterFilm;
  Tempest::Vec3 waterFilmLip;
  bool waterFilmKill=false;
  uint64_t waterFilmStart=0;
};

struct Movables {
  // Test-only props use the body API directly; the map allowlist contains only
  // visuals actually authored in the stock worlds, not these Item mesh copies.
  std::vector<std::unique_ptr<MovableFixture>> movableFixtures;
  std::unordered_set<uintptr_t> movableMapIds;
  std::vector<uintptr_t> movableIds;
  std::unordered_map<uintptr_t,Tempest::Vec3> movableReference;
  std::unordered_map<uintptr_t,float> movableTravel;
  std::unordered_set<uintptr_t> movableNpcTouched;
  uintptr_t movableTable=0,movableDish=0;
  Tempest::Vec3 movableCenter,movableWalkStart,movableWalkEnd;
  Npc* movableWalker=nullptr;
  WayPoint movableDestination;
  bool movableReady=false,movableTrace=false,movableMeasuring=false,movableOverflow=false;
  std::string movablePhase;
  uint64_t movablePhaseStart=0,movableSampleTick=0;
  std::vector<LivingPhysics> movablePhysics;
  uint64_t movableWakeStart=0;
  bool movableWakeTrace=false;
  std::unordered_map<uintptr_t,float> movableWakeTravel;
  struct CandleAttachment {
    uint32_t zenId=uint32_t(-1);
    std::string visual;
    bool light=false;
    Tempest::Vec3 local,initial;
    float maximumError=0,maximumTravel=0;
  };
  std::vector<CandleAttachment> candleAttachments;
  uintptr_t candleBody=0;
  uint32_t candleZenId=uint32_t(-1),candleTableZenId=uint32_t(-1);
  Tempest::Matrix4x4 candleTableInitial;
  Tempest::Vec3 candleInitial,candleSupport;
  Tempest::Vec3 candleTableBounds[2];
  float candleMaximumTilt=0,candleMaximumTravel=0;
  bool candleTrace=false,candleLeftTable=false,candleSnapshotMissing=false,candleCaptured=false;
  uint64_t candleStart=0,candleSamples=0;
  struct NoiseListener { Npc* npc=nullptr; std::string role; Tempest::Vec3 initial; unsigned heard=0; };
  std::vector<NoiseListener> noiseListeners;
  bool noiseTrace=false,noiseHasPushed=false,noiseTipNegative=false;
  std::string noisePhase;
  uint64_t noiseStart=0;
  Tempest::Vec3 noiseSource;
  Tempest::Matrix4x4 noiseTableInitial;
  float noiseTableMaximumTilt=0;
  std::unordered_map<uint32_t,unsigned> noisePerceptions;
  std::unordered_map<const Npc*,uint32_t> noiseNpcIds;
  uint32_t noiseNextNpcId=0;
  Npc* noiseHost=nullptr;
  uint64_t noiseMeasuredImpacts=0,noiseMeasuredHeard=0,noiseMeasuredRays=0;
  bool noiseBridgeDebug=false;
  uint64_t noiseBridgeImpacts=0,noiseBridgeLoudImpacts=0,noiseBridgeCulpritImpacts=0;
  uint64_t noiseBridgeUnseenCulpritImpacts=0,noiseBridgeCupImpacts=0;
  uint64_t noiseBridgeDialogueImpacts=0,noiseBridgeCombatImpacts=0;
  float noiseBridgeCupMaxRadius=0;
  std::vector<uint64_t> noiseBridgeHeard;
  std::vector<uint64_t> noiseBridgeEvents;
  std::vector<SpellActor> spellActors;
  std::vector<Item*> spellItems;
  std::map<int32_t,size_t> spellRunes;
  bool spellArenaReady=false,spellTrace=false,spellWasFrozen=false;
  std::string spellCase;
  uint64_t spellSampleTick=0,spellReleaseAt=0,spellWaitUntil=0;
  int32_t spellCastingId=0;
  int32_t spellEquipPending=0;
  uint64_t spellEquipUntil=0;
  int spellWaitFrozen=-1;
  bool spellGuardTrace=false;
  std::string spellGuardCase;
  uint32_t spellGuardObservedReturns=0;
};

struct MovablesContext {
  Movables& movables;
  CamMode& cam;
  std::vector<Npc*>& crew;
  QuestSite& questCliff;
  QuestSite& questWater;
  Tempest::Vec3& ragdollArenaTarget;
  std::string& shot;
  Npc*& subject;
  const char* const& scenarioPath;
};

struct BodyAnimationEntry {
  uint32_t seed = 0;
  unsigned side = 0;
  uint64_t time = 0;
  std::string animation;
  int variant = 0;
  uint64_t sourceSeq = 0;
  unsigned consumed = 0;
  };

struct BodyAnimationActor {
  const Npc* npc = nullptr;
  uint32_t rng = 0;
  uint64_t solverCalls = 0, controlledCalls = 0, fallbackCalls = 0, accepted = 0;
  uint64_t lastTime = uint64_t(-1);
  int lastVariant = -1;
  };

struct BodyAnimationControl {
  enum Mode { Default, Isolated, Replay };
  Mode mode = Default;
  bool initialized = false;
  bool valid = true;
  bool active = false;
  bool totalsWritten = false;
  uint32_t seed = 0;
  uint64_t errors = 0, errorsAtStart = 0;
  BodyAnimationActor actors[4];
  unsigned actorCount = 2;
  std::vector<BodyAnimationEntry> fixture;
  std::string sourceEventsHash;
  };

struct BodyImpact {
  Npc* npc=nullptr;
  Npc* animated=nullptr;
  Npc* witness=nullptr;
  Npc* player=nullptr;
  Npc* culprit=nullptr;
  Tempest::Vec3 anchor;
  std::unique_ptr<PhysicMeshShape> floorShape,wallShape,ledgeShape;
  DynamicWorld::Item floor,wall,ledge;
  std::string mode;
  float value=0,maximumSpeed=0,launchSpeed=0;
  int hpBefore=0,animatedBefore=0,damage=0,capsuleDamage=0,questGuard=0;
  uint64_t started=0;
  size_t impacts=0,deeds=0,murders=0,defeats=0,weapon=size_t(-1);
  bool ready=false,tracing=false,observing=false,duel=false,sawAir=false,guardSaved=false;
  struct DuelBody {Npc* npc=nullptr;bool knocked=false;int damage=0;float speed=0;uint64_t since=0;};
  std::vector<DuelBody> duelBodies;
  void deed(const HeroDeed& value);
  void perception(Npc& observer,Npc& other,Npc* victim,PercType value);
};

// References to shared bench fields used by P30 commands and their hooks.
struct BodyContext {
  bool& bodyLive;
  std::string& bodyLiveWait;
  std::chrono::steady_clock::time_point& bodyLiveDeadline;
  BodyAnimationControl& bodyAnimation;
  Npc*& duelA;
  Npc*& duelB;
  uint64_t& duelFrom;
  uint64_t& duelEnd;
  bool& duelWaiting;
  std::vector<Npc*>& observeActors;
  uint64_t& observeFrom;
  uint64_t& observeEnd;
  bool& observeActive;
  Tempest::Vec3& observeVisibleA;
  Tempest::Vec3& observeVisibleB;
  bool& observeVisibleSaved;
  std::string& observeProbeCase;
  std::unique_ptr<::Sound>& probeSound;
  uint64_t& probeSoundStart;
  uint64_t& probeSoundDuration;
  bool& probeOccluded;
  Npc*& subject;
  uint32_t& lcg;
  size_t& pc;
  const std::vector<std::string>& prog;
  uint64_t& time;
  uint64_t& waitUntil;
};

struct PhysicsTracesContext {
  Npc*& traceHost;
  uint32_t& traceTable;
  Tempest::Matrix4x4& traceOrigin;
  Tempest::Vec3& traceCenter;
  Tempest::Vec3& traceVisible;
  Tempest::Vec3& traceBlocked;
  Tempest::Vec3& traceFar;
  Tempest::Vec3& traceWall;
  Tempest::Vec3& traceShotDirection;
  bool& traceReady;
};

// References to shared bench fields used by the body-impact fixtures.
struct BodyImpactContext {
  BodyImpact& bodyImpact;
  Npc*& subject;
  std::vector<SpellActor>& spellActors;
  std::vector<Npc*>& crew;
  bool& spellTrace;
  bool& bodyTrace;
  uint64_t& time;
  uint64_t& waitUntil;
};

struct Destruction {
  std::vector<std::unique_ptr<MovableFixture>> destructionFixtures;
  std::vector<uintptr_t> destructionIds;
  std::map<std::pair<uintptr_t,size_t>,Tempest::Vec3> destructionFragmentOrigins;
  std::map<std::pair<uintptr_t,size_t>,float> destructionFragmentTravel;
  std::string destructionCase;
  Tempest::Vec3 destructionCaster,destructionTarget,destructionDirection,destructionActorStart;
  Npc* destructionActor=nullptr;
  WayPoint destructionDestination;
  uint64_t destructionStart=0,destructionNextArrow=0;
  uint64_t destructionLastBreak=0;
  uint64_t destructionFallSample=0;
  unsigned destructionArrows=0;
  bool destructionReady=false,destructionTrace=false,destructionShooting=false;
  bool destructionSleepSampled=false;
  bool destructionSupportTrace=false;
  uint64_t destructionSupportNext=0;
  const Npc* destructionSkinRequest=nullptr;
  std::unordered_map<const ProtoMesh*,std::vector<Resources::VertexA>> destructionSkinGeometry;
  std::string destructionSkinSample="null";
  Npc* destructionMeleeActor=nullptr;
  Npc* destructionMeleeNpc=nullptr;
  Tempest::Vec3 destructionMeleeFacing,destructionMeleeStand;
  bool destructionMeleeRestance=false;
  uint64_t destructionMeleeNext=0,destructionMeleeDeadline=0;
  unsigned destructionMeleeAttempts=0,destructionMeleeHits=0,destructionMeleeBlows=0;
  bool destructionMeleeRunning=false,destructionMeleeArena=false;
  Npc* destructionFarmNpc=nullptr;
  Tempest::Vec3 destructionFarmNpcStart,destructionFarmStart,destructionFarmEnd;
  const WayPoint* destructionFarmNpcGoal=nullptr;
  uint32_t destructionFarmZen=uint32_t(-1);
  float destructionFarmNpcTravel=0;
  float destructionFarmNpcBest=std::numeric_limits<float>::infinity();
  Npc* destructionTraceHost=nullptr;
  World* destructionTraceWorld=nullptr;
  uint32_t destructionTraceZen=uint32_t(-1);
  Tempest::Vec3 destructionTraceSource;
  bool destructionTraceDebug=false;
};

// References to the shared bench fields used by the destruction fixtures.
struct DestructionContext {
  CamMode& cam;
  Tempest::Vec3& movableCenter;
  std::vector<uintptr_t>& movableIds;
  bool& movableMeasuring;
  bool& movableReady;
  Tempest::Vec3& movableWalkStart;
  Npc*& noiseHost;
  Tempest::Vec3& noiseSource;
  bool& noiseTrace;
  Tempest::Vec3& ragdollArenaTarget;
  std::string& ragdollArenaWaypoint;
  std::vector<SpellActor>& spellActors;
  uint64_t& time;
  uint64_t& waitUntil;
  std::unordered_set<std::string>& brokenSignals;
  const char* const& scenarioPath;
};

struct LivingWalker { Npc* npc=nullptr; WayPoint ends[2]; bool next=false,queued=false; int meleePhase=-1;uint64_t meleeAttackAt=0; };

struct ItemWake {
  struct ItemWakeSample {
    Item* item=nullptr;
    std::string name;
    Tempest::Vec3 initial,collisionBounds[2],boundsOrigin;
    float floorY=0,travel=0,penetration=0;
    bool bodyCreated=false,hadBody=false,fell=false,below=false,preparationDive=false;
    };
  std::vector<ItemWakeSample> itemWakeItems;
  std::vector<Npc*> itemWakeActors;
  Item* itemWakeMover=nullptr;
  std::unique_ptr<PhysicMesh> itemWakeTable;
  std::unique_ptr<ObjVisual> itemWakeTableVisual;
  Tempest::Vec3 itemWakeBladePoints[3],itemWakePreviousTip,itemWakePreviousGrip,itemWakeSwordRoot;
  std::vector<Tempest::Vec3> itemWakeBladePath;
  size_t itemWakeBladePathEnd=0;
  bool itemWakeBladeCrossed[3]{};
  float itemWakeBladeSpeed=0,itemWakeSwordLength=0;
  const Animation::Sequence* itemWakeSwordSequence=nullptr;
  uint32_t itemWakeSwordOptimalFrame=0;
  uint64_t itemWakeCalibrationEnd=0;
  bool itemWakeCalibrationSample=false;
  Tempest::Vec3 itemWakeCenter;
  std::string itemWakeCase;
  bool itemWakeSite=false,itemWakeRunning=false,itemWakePreparing=false;
  uint64_t itemWakeSample=0,itemWakePulse=0,itemWakeStart=0,itemWakeWindow=0;
  uint64_t itemWakeLastNativeStep=0,itemWakeZeroFrames=0,itemWakeFirstPhysicsTick=0,itemWakeLastPhysicsTick=0;
  float itemWakeMinimumGap=std::numeric_limits<float>::infinity();
  size_t itemWakeQueryOverlaps=0,itemWakeInitiallyBodyless=0;
  std::vector<float> itemWakePhysics;
  };

struct Living {
  std::deque<LivingWalker> livingWalkers;
  bool livingFixture=false,livingDebug=false,livingMotion=false,livingMeasuring=false,livingMode=false;
  std::string livingPhase;
  Tempest::Vec3 livingHero;
  std::vector<LivingPhysics> livingPhysics;
  bool livingOverflow=false;
  Npc* seatNpc=nullptr;
  Npc* seatCorpse=nullptr;
  Interactive* seatMob=nullptr;
  std::vector<Item*> seatItems;
  std::vector<Tempest::Vec3> seatPreviousParts,seatPreviousItems;
  uint64_t seatSampleTick=0;
  std::string seatPhase;
  };

struct ArcheryBench {
  Npc* arrowNpc=nullptr;
  std::string arrowCase,arrowAim;
  float arrowDraw=1.f;
  uint64_t arrowSampleTick=0;
  bool arrowTrace=false,arrowKoOnHit=false;
  uint32_t arrowSeenHits=0;
  bool stickingTrace=false;
  std::string stickingCase;
  uint64_t stickingSampleTick=0;
  Tempest::Vec3 stickingWorldPoint;
  std::vector<std::pair<Npc*,Tempest::Vec3>> archers; // archer: they stand there and shoot at archerAt...
  Npc*        archerAt     = nullptr;
  bool        archerDice   = false;       // ...by the dice
  // aimat: the manual aim aims there instead of the mouse, every frame
  Npc*           aimNpc   = nullptr;
  HitZones::Zone aimZone  = HitZones::Body;
  bool        aimLeg    = false;  // aimat ... leg: the middle of the legs' capsules (limb takes the arms too)
  bool           aimFixed = false;
  bool           aimRaw   = false; // no holding over for the drop of the arrow
  bool           aimMouse = false; // the game's camera turns onto it, as the mouse would: the shot from the
                                   // center of the screen
  Tempest::Vec3           aimAt    = {};
  };

struct Trail { const void* key = nullptr; std::vector<Tempest::Vec3> pts; uint64_t last = 0; };

struct ArcheryContext {
  ArcheryBench& archery;
  Living& living;
  std::vector<Npc*>& crew;
  Npc*& subject;
  Tempest::Vec3& ragdollArenaTarget;
  CamMode& cam;
  std::vector<Trail>& trails;
  float& camAngle;
  float& camDist;
  float& camHeight;
  float& camLook;
  };

struct LivingCounts {
  size_t models=0,alive=0,kinematic=0,parts=0,dynamic=0,dynamicParts=0,duplicates=0;
  };

struct ItemWakeContext {
  ItemWake& itemWake;
  Destruction& destruction;
  CamMode& cam;
  Tempest::Vec3& ragdollArenaTarget;
  };

struct LivingContext {
  Living& living;
  std::vector<Npc*>& crew;
  Npc*& subject;
  std::FILE*& csv;
  std::vector<Tempest::Vec3>& ragdollArenaPlaces;
  bool (*frameRagdollArena)(size_t);
  };

struct CharacterMoveFrame {
  Tempest::Vec3 requested,acceptedDelta,steepNormal;
  uint64_t sequence=0,dt=0;
  float advancementRatio=0;
  bool valid=false,steepContact=false,blocked=false;
  };

struct Character {
  struct CharacterRoute {
    std::string name,waypoint;
    Tempest::Vec3 start,goal,direction;
    float tolerance=80;
    bool prepared=false,active=false,reached=false,sawSwim=false,sawSlide=false,falling=false;
    unsigned fallCount=0;
    Tempest::Vec3 lastSupported;
    uint64_t began=0,arrived=0;
  } characterRoute;
  struct CharacterIdleSlope {
    bool prepared=false,npc=false;
    float angle=0;
    uint64_t began=0,previousTick=0;
    Tempest::Vec3 start,previous;
  } characterIdleSlope;
  struct CharacterFrame {
    std::string route;
    uint64_t frame=0,time=0,dt=0;
    Tempest::Vec3 root,groundVelocity,visualRoot;
    bool moving=false,air=false,fall=false,swim=false,enabled=false,active=false,captured=false,normal=false;
    float physics=0,wall=0;
  };
  std::vector<CharacterFrame> characterFrames;
  struct NpcCharacterRoute {
    Npc* npc=nullptr;
    WayPoint destination;
    Interactive* seat=nullptr;
    std::string name;
    Tempest::Vec3 start,goal,lastPosition,lastVisual,progressPosition;
    uint64_t began=0,arrived=0,progressTime=0;
    unsigned stalls=0;
    float maxVisualJump=0,maxPhysicalJump=0,maxAboveFloor=0,mugMassKg=0;
    bool mugContact=false;
    uint64_t supportSince=0;Tempest::Vec3 preparationSample;
    bool sawPreparationSample=false;
    std::deque<std::string> supportHistory;
    bool active=false,reached=false,stalled=false,sat=false,standing=false;
  } npcCharacterRoute;
  struct NpcCharacterFrame {
    std::string phase;
    uint64_t frame=0,time=0,dt=0,npc=0;
    Tempest::Vec3 root,visual;
    bool enabled=false,active=false,moving=false,air=false,interactive=false,captured=false,normal=false;
    float physics=0,wall=0;
    unsigned nearby=0,virtualNearby=0,movingNearby=0,movingVirtualNearby=0;
    CharacterMoveFrame move;
  };
  std::deque<LivingWalker> npcCharacterCrowd;
  std::vector<NpcCharacterFrame> npcCharacterFrames;
  struct NpcCharacterTraceActor { Npc* npc=nullptr;Tempest::Vec3 lastRoot,lastVisual;bool hasPrevious=false; };
  std::vector<NpcCharacterTraceActor> npcCharacterTraceActors;
  struct CharacterIdle {
    Npc* actor=nullptr;
    Npc* fixtureNpc=nullptr;
    std::string site,role;
    Tempest::Vec3 initial;
    uint64_t began=0;
  } characterIdle;
  bool npcCharacterMeasuring=false,npcCharacterHasWall=false,npcCharacterObserving=false;
  std::chrono::steady_clock::time_point npcCharacterLastWall;
  std::string npcCharacterWindow;
  struct NpcCharacterGoal { Tempest::Vec3 target;const Npc* npc=nullptr;std::string name,kind;bool active=false; };
  std::unordered_map<const Npc*,NpcCharacterGoal> npcCharacterGoals;
  std::unordered_set<const Npc*> npcCharacterWalkers;
  uint64_t npcCharacterObservedFrames=0,npcCharacterCvFrames=0,npcCharacterArrivals=0,npcCharacterObserveStart=0;
  unsigned npcCharacterMaxNearby=0,npcCharacterCrowdMinNearby=20,npcCharacterCrowdMaxNearby=0;
  float npcCharacterCrowdMaximumDistance=0;
  std::chrono::steady_clock::time_point characterLastWall;
  bool characterHasWall=false,characterMeasuring=false;
  uint64_t characterFrame=0;
  Item* characterItem=nullptr;
  std::unique_ptr<MovableFixture> characterProp;
  uintptr_t characterPropId=0;
  std::string characterPropRole;
  Tempest::Vec3 characterPropInitial,characterPropHeroInitial;
  float characterPropTravel=0;
  uint64_t characterPropBegan=0;
  Tempest::Vec3 characterPropDirection;
  uint64_t characterMeasureStart=0;
  std::unique_ptr<ObjVisual> characterPlatformVisual;
  std::unique_ptr<PhysicMesh> characterPlatform;
  Tempest::Matrix4x4 characterPlatformMatrix;
  MoveTrigger* characterMover=nullptr;
  Npc* characterRideNpc=nullptr;
  Tempest::Vec3 characterRideStart,characterRideHeroStart,characterRideLocalStart,characterRideDirection,characterRidePivot;
  Tempest::Matrix4x4 characterRideMatrixStart;
  float characterRideAngle=0,characterRideTravel=0,characterRideMaximumDrift=0;
  float characterRideMaximumHorizontalDrift=0,characterRideMaximumLocalDrift=0;
  uint64_t characterRideBegan=0;
  bool characterRiding=false,characterRideMoving=false,characterRideRotating=false;
};

// References to the shared bench fields used by the human CharacterVirtual fixtures.
struct CharacterContext {
  CamMode& cam;
  float& camAngle;
  bool& camAuto;
  float& camDist;
  float& camHeight;
  uint64_t& frameMs;
  bool& hasPrev;
  std::vector<Hold>& holds;
  std::vector<std::unique_ptr<MovableFixture>>& movableFixtures;
  bool& movableReady;
  Tempest::Vec3& movableWalkEnd;
  Tempest::Vec3& movableWalkStart;
  Npc*& noiseHost;
  bool& rec;
  bool& recVideo;
  uint64_t& step;
  Npc*& subject;
  std::vector<KeyCodec::Action>& taps;
};

struct MonsterCharacter {
  struct MonsterCharacterRoute {
    Npc* npc=nullptr;
    WayPoint destination,heroDestination;
    bool heroRun=false,isolatedLedge=false;
    std::string name,animation;
    Tempest::Vec3 start,goal,lastPosition,lastVisual,progressPosition,lastSupported,attackStart;
    uint64_t began=0,arrived=0,progressTime=0,attackEnded=0;
    unsigned stalls=0,falls=0,cvFrames=0,swimFrames=0,callbacks=0;
    float maxPhysicalJump=0,maxVisualJump=0,attackDistance=0,attackRise=0,tolerance=60;
    bool active=false,reached=false,stalled=false,falling=false,wasAirborne=false,sawSwim=false,leftWater=false,chase=false,attack=false;
    Tempest::Vec3 savedPosition;
    unsigned restoreSteps=0;
  } monsterCharacterRoute;
  // Source shape must outlive the collision handle that borrows its mesh.
  std::unique_ptr<PhysicMeshShape> monsterCharacterLedgeShape;
  DynamicWorld::Item monsterCharacterLedgeBody;
  struct MonsterCharacterFrame {
    std::string route,animation;
    uint64_t time=0,dt=0,npc=0;
    Tempest::Vec3 root,visual;
    bool enabled=false,active=false,moving=false,air=false,fall=false,swim=false;
    float bodyYaw=0;
    CharacterMoveFrame move;
  };
  std::vector<MonsterCharacterFrame> monsterCharacterFrames;
  struct MonsterCharacterExtra {
    Npc* npc=nullptr;
    std::string role;
    std::vector<WayPoint> points;
    size_t next=0;
    bool loop=false,queued=false,stalled=false,falling=false,wasAirborne=false;
    Tempest::Vec3 start,lastPosition,lastVisual,progressPosition,lastSupported;
    uint64_t progressTime=0,arrived=0;
    unsigned arrivals=0,stalls=0,falls=0,cvFrames=0;
    float maxPhysicalJump=0,maxVisualJump=0,travel=0;
  };
  std::deque<MonsterCharacterExtra> monsterCharacterExtras;
  struct MonsterCharacterProp {
    std::unique_ptr<MovableFixture> fixture;
    std::string role;
    Tempest::Vec3 start,lastIntact;
    uintptr_t id=0;
    float travel=0;
    unsigned fragments=0;
    bool broken=false;
  };
  std::vector<MonsterCharacterProp> monsterCharacterProps;
  MonsterCharacterProp monsterCharacterBarrel;
  struct MonsterCharacterPerformance {
    uint64_t time=0,dt=0;
    float physics=0,wall=0,maximumDistance=0;
    unsigned nearby=0,virtualNearby=0,moving=0;
    bool captured=false,normal=false;
  };
  std::vector<MonsterCharacterPerformance> monsterCharacterPerformance;
  bool monsterCharacterMeasuring=false,monsterCharacterHasWall=false,monsterCharacterCrowd=false,monsterCharacterPack=false;
  std::chrono::steady_clock::time_point monsterCharacterLastWall;
  const Npc* monsterCharacterRenderRequest=nullptr;
  Tempest::Vec3 monsterCharacterHeroStart;
  uint32_t monsterCharacterSeed=1;
  unsigned monsterCharacterMinNearby=20,monsterCharacterMaxNearby=0;
  float monsterCharacterMaximumDistance=0;
};

// References to shared bench fields used by the monster CharacterVirtual fixtures.
struct MonsterCharacterContext {
  Character& character;
  CharacterContext characterContext;
  CamMode& cam;
  bool& camAuto;
  Npc*& subject;
  Tempest::Vec3& ragdollArenaTarget;
  std::vector<Tempest::Vec3>& ragdollArenaPlaces;
  std::string& ragdollArenaWaypoint;
  bool& rec;
  bool& recVideo;
};

struct Water {
  std::unordered_set<uintptr_t> ids;
  std::unordered_map<uintptr_t,std::string> labels;
  std::unordered_map<uintptr_t,uint64_t> sleepAt;
  std::unordered_map<uintptr_t,Tempest::Vec3> reference;
  std::unordered_map<uintptr_t,float> travel;
  std::vector<Item*> items;
  std::string caseName,waypoint;
  Tempest::Vec3 dry,center,direction,heroStart,heroLast,observerStart,cameraTarget;
  uint32_t barrelZen=uint32_t(-1),crateZen=uint32_t(-1);
  uint64_t start=0,sample=0,observerSample=0;
  float heroMaxStep=0,rayTop=0;
  bool ready=false,trace=false,swim=false,sawSwim=false,observer=false;
  struct WaterActor { Npc* npc=nullptr; std::string role; uint64_t sleepAt=uint64_t(-1); };
  std::vector<WaterActor> actors;
  bool bodiesReady=false,lootReady=false,killOnThrow=false;
  std::string lootMode;
};

// References to the shared bench state used by the water fixtures.
struct WaterContext {
  CamMode& cam;
  Tempest::Vec3& camPos;
  bool& movableMeasuring;
  std::vector<uintptr_t>& movableIds;
  std::unordered_set<uintptr_t>& movableMapIds;
  bool& movableReady;
  Tempest::Vec3& movableCenter;
  QuestSite& questWater;
  std::vector<std::unique_ptr<MovableFixture>>& destructionFixtures;
  std::vector<Hold>& holds;
  std::vector<KeyCodec::Action>& taps;
  PlayerControl*& ctrl;
  std::vector<Npc*>& crew;
  std::vector<SpellActor>& spellActors;
  bool& spellGuardTrace;
  std::map<int32_t,size_t>& spellRunes;
  int32_t& spellCastingId;
  int32_t& spellEquipPending;
  uint64_t& spellEquipUntil;
  const Npc*& expectedRagdollLoot;
  bool& ragdollLootDidOpen;
};

struct WaterFilmContext {
  WaterFilm& film;
  Water& water;
  Movables& movables;
  Destruction& destruction;
  WaterContext waterContext;
  DestructionContext destructionContext;
  CamMode& cam;
  Tempest::Vec3& camPos;
  Npc*& subject;
  std::vector<Npc*>& crew;
  bool& rec;
  uint64_t& time;
  uint64_t& recStart;
};

struct PlacedItems {
  std::vector<std::vector<size_t>> batches;
  std::vector<float> maximumTravel;
  std::vector<bool> npcTouched;
  std::vector<size_t> probeNeighbours;
  size_t target=size_t(-1);
  uint32_t tableZen=uint32_t(-1);
  std::vector<size_t> observed;
  std::unordered_map<size_t,Tempest::Vec3> reference;
  std::unordered_map<size_t,float> travel;
  Tempest::Vec3 walkStart,walkGoal,walkDirection,walkPrevious;
  Tempest::Matrix4x4 tableOrigin;
  uint64_t walkSince=0,walkArrived=0;
  float walkMaximumStep=0,walkMaximumCrossTrack=0;
  bool walking=false,tracing=false,walkBodyBefore=false;
  size_t pickupSymbol=0,pickupCount=0,pickupBefore=0;
  bool pickupAccepted=false,watchingTheft=false;
  std::vector<std::string> theftWitnesses;
  std::vector<size_t> theftRecipients;
  std::string theftOwner,theftItem,theftRoom,theftRoomOwner;
  int32_t theftOwnerGuild=0,theftRoomGuild=-1;
  size_t theftDeeds=0;
  void deed(const HeroDeed& value);
  void perception(Npc& self,Npc&,Npc*,PercType value);
  size_t batch=0,probed=0,under5=0;
  float maximumShift=0;
  uint64_t started=0,startedStep=0,lastNative=0;
  Tempest::Vec3 observer;
  bool probing=false,performance=false;
  struct Sample {uint64_t tick=0,nativeSteps=0;float physicsMs=0,frameMs=0;size_t bodies=0,placedBodies=0,created=0;};
  std::vector<Sample> samples;
  std::chrono::steady_clock::time_point frameWall;
  bool hasFrame=false;
};

struct FurnitureDrop {
  Item* item=nullptr;
  std::string role;
  std::vector<Tempest::Vec3> vertices,collisionVertices;
  float supportY=0,supportLocalY=0;
  };

struct FurnitureParts {
  uint32_t table=uint32_t(-1),cupboard=uint32_t(-1);
  std::string tableVisual;
  Tempest::Matrix4x4 tableOrigin;
  Tempest::Vec3 tableCenter,walkStart,walkGoal,walkDirection;
  Npc* walker=nullptr;
  WayPoint walkDestination;
  std::vector<FurnitureDrop> drops;
  Npc* corpse=nullptr;
  bool walking=false;
  float walkMaxStep=0;
  Tempest::Vec3 walkPrevious;
  uint64_t walkStartTick=0,walkArrived=0;
  float walkMaxCrossTrack=0;
  struct PhysicsSample {
    uint64_t time=0,dt=0,nativeSteps=0;
    float milliseconds=0;
    size_t bodies=0,active=0;
    };
  std::vector<DynamicWorld::Item> loadBodies;
  std::vector<PhysicsSample> loadSamples;
  uint64_t loadWarmUntil=0,loadEnd=0,loadLastNative=0,loadZeroFrames=0,loadNextWake=0;
  size_t loadWakeResets=0;
  Tempest::Vec3 loadBounds[2];
  float loadResetPoseError=0,loadResetVelocityError=0,loadResetAngularError=0;
  size_t loadMinimumActive=10,loadDecomposed=0;
  bool loadRunning=false;
  bool traceApple=false;
  };

struct ItemHulls {
  struct ItemHullDrop {
    Item* item=nullptr;
    std::string label,symbol;
    std::vector<Tempest::Vec3> vertices;
    JPH::ShapeRefC shape;
    Tempest::Vec3 axis,sleepPosition;
    Tempest::Matrix4x4 sleepTransform=Tempest::Matrix4x4::mkIdentity();
    float mass=0,maxJitter=0,extraPaddingCm=0;
    uint64_t sleepAt=uint64_t(-1);
    bool belowWorld=false;
    };
  std::vector<ItemHullDrop> itemHullDrops;
  std::unique_ptr<PhysicMeshShape> itemHullSupportShape;
  DynamicWorld::Item itemHullSupportBody;
  JPH::ShapeRefC itemHullSupportQuery;
  Tempest::Vec3 itemHullAnchor,itemHullHero,itemHullNormal{0,1,0};
  std::string itemHullSurface;
  uint64_t itemHullStart=0,itemHullSample=uint64_t(-1);
  std::vector<float> itemHullCosts;
  bool itemHullTracing=false,itemHullLoad=false;
};

// The pieces of each prop broken during a bench, sampled for the destruction guards.
struct FracturePieces {
  struct Piece {
    size_t index=0;
    double volume=0;
    std::string group;
    asset::MaterialGroup material=asset::MaterialGroup::UNDEFINED;
    Tempest::Vec3 center,thin,longAxis,start,at13;
    float extent[3]={};
    bool flat=false,stave=false;
    // Frame-by-frame track until the piece first goes below the ground (destruction2_fell).
    bool fellReported=false;
    uint64_t lastTime=0;
    Tempest::Vec3 lastCenter,lastVelocity,lastOmega;
    float lastHeight=0,lastNative=0;
    std::vector<Tempest::Vec3> vertices;
    JPH::ShapeRefC hull;
    uint32_t bondGroup=0; // bond group at the break: pieces of one group share one body
    };
  struct Break {
    uintptr_t parent=0;
    std::string label,visual;
    size_t version=0,samples=0,groups=0;
    uint64_t breakTime=0;
    std::vector<Piece> pieces;
    };
  std::vector<Break> breaks;
  // `destruction2 remnant <n>`: the fixture's actor strikes the largest group of its broken prop,
  // re-aimed before each swing, until the prop has more bodies or n swings are spent.
  struct RemnantAttack {
    bool running=false;
    unsigned limit=0,swings=0,ordinal=0;
    size_t bodies=0,pieces=0;
    double largest=0;
    uint64_t next=0,deadline=0;
    };
  RemnantAttack remnant;
  };

struct Healing {
  struct {
    std::string label;
    size_t item = size_t(-1), lastItems = 0;
    uint64_t start = 0, firstHeal = 0, lastHeal = 0, lastConsume = 0, nextSecond = 0, strikeAt = 0, hotkeyAt = 0;
    uint64_t paused = 0, savedAt = 0, savedPaused = 0, savedLastHeal = 0, savedNextSecond = 0;
    size_t portions = 0, savedSamples = 0;
    int hp = 0, healed = 0, damage = 0, savedHealed = 0, savedDamage = 0;
    bool sawDrink = false;
    bool armed = false, sawHand = false, heldWeapon = true, handLayer = false, leftInsert = false, sound = false;
    bool leftHeld = true, sawLeftItem = false;
    bool walking = false, baseline = false, walkOnly = true, actionExecuted = false;
    bool hotkey = false, checkWalk = false;
    WalkBit expectedWalk = WalkBit::WM_Run;
    Npc* npc = nullptr;
    uint64_t queueRunAt = 0;
    WeaponState weapon = WeaponState::NoWeapon;
    std::string clip, nativeClip, action, motion;
    Tempest::Vec3 previous;
    float distance = 0, previousYaw = 0, yawTravel = 0;
    uint64_t observedAt = 0, handStart = 0, handEnd = 0, insertAt = 0, destroyAt = 0, movingMs = 0, strikeDelay = 0;
    unsigned direction = 0;
    size_t secondItem = size_t(-1);
    std::vector<std::pair<uint64_t,int>> samples;
    std::vector<uint8_t> saved;
    } test;
  float walkSpeed[2] = {};
};

struct NpcCommands {
  int64_t     npcDayStart = 0;
  uint64_t    npcDayMinutes = 0;
  // volley: shots by hand in a row - press, draw to volleyDraw, hold volleyHold, let go, wait for the
  // next arrow
  int         volleyLeft  = 0;
  int         volleyPhase = 0;
  uint64_t    volleyHold  = 0;
  float       volleyDraw  = 1;
  uint64_t    volleyJitter = 0;  // ms more to hold, at random up to that (the sway is a function of time)
  uint64_t    volleyHoldNow = 0;
  // wander: the crew walks the waynet from waypoint to waypoint near the hero (docs/navigation.md, stage 0)
  std::vector<Npc*>            wanderers;
  std::vector<const WayPoint*> wanderPoints;
  size_t                       wanderLegs = 0;
  Npc*        downNpc      = nullptr; // waitdown: till either participant falls
  Npc*        downOther    = nullptr;
  uint64_t    downFrom = 0, downEnd = 0;
  uint64_t    volleyT     = 0;
  struct StunDrift {
    Npc* target=nullptr;
    Npc* attacker=nullptr;
    Tempest::Vec3 origin,axis;
    uint64_t start=0;
    unsigned next=0;
  } stunDrift;
};

struct CameraBench {
  CamMode     cam       = C_Game;
  float       camDist   = 220;
  float       camHeight = 40;
  float       camAngle  = 90;   // C_Follow: degrees from the facing (0 in front, 90 on the left); C_World: world yaw
  float       camLook   = 45;   // look at this height above the player origin
  bool        camLift   = true; // C_Follow, C_World: kept above the ground under it (off: where it is put,
                                // a floor above it is no ground: under a gallery, in a hall)
  bool        camAuto   = false;
  int         occlTime  = 0;
  Tempest::Vec3        camPos;
  Tempest::PointF      camHeldAngles;
  Tempest::PointF      camTurnAngles;
  float       camTurnDegrees = 0;
  uint64_t    camTurnStart = 0, camTurnDuration = 0;

  bool        rec       = false;
  bool        recVideo  = false;
  std::string recName;
  uint64_t    recFrame  = 0;
  uint64_t    recStart  = 0;
  std::string shot;
  float       clip      = 0;     // > 0: everything closer to the camera than the subject minus this is clipped away
  uint8_t     occl      = 0;    // bit per leg: the ankle is hidden from the camera
  // the hero placed around the subject: distance, angle from its facing (0 in front), orbiting
  float       placeDist  = 0;
  float       placeAngle = 0;
  float       orbitSpeed = 0;     // degrees per second
  uint64_t    orbitUntil = 0;

  // talking (gothic-shorts)
  std::string camNpc;           // C_Shoulder, C_NpcFace, C_Two
  // slowmo: the game runs `slow` times slower than the video (its sound is not recorded meanwhile)
  float       slow      = 1;
  uint64_t    slowAcc   = 0;      // microseconds of game time owed to the next frame
  uint64_t    frameMs   = 0;      // the video's time of this frame, ms
  // track: where an npc's head (or trunk) is in each recorded frame, for labels that follow it
  std::vector<std::string> trackNpcs;
  bool        trackHead = true;
  // sight: the hero's line of sight to an npc in each recorded frame and shot (the focus' rays).
  std::string sightNpc;
  float       sightFan  = 700;
  float       camSmooth = 0.12f; // share of the way to the wanted camera per frame; 1: no smoothing
  bool        smValid   = false;
  bool        cutPending = false; // `cam cut`: the next placement of the camera is a cut (Camera::markCut)
  Tempest::Vec3        smOrg, smTgt;
  // sound: the mix of the game (Tempest loopback), recorded with the video
  std::FILE*  audio     = nullptr;
  uint64_t    audioFrames = 0;
  uint64_t    audioAcc  = 0;
  std::vector<int16_t> mix;
  struct VoiceStem {
    std::mutex           sync;
    std::string          path;
    uint32_t             rate = 0;
    uint64_t             sample = 0;
    std::vector<int16_t> pcm;
    bool                 stopped = false;
    };
  std::vector<std::shared_ptr<VoiceStem>> voiceStems;
  std::chrono::steady_clock::time_point recWall;  // when the recording started
};

struct OverlayBench {
  std::unique_ptr<VideoPipe> video;
  uint32_t    videoW    = 0;
  uint32_t    videoH    = 0;
  bool        overlay   = true;
  bool        ui        = false;  // the interface is in the captured frame
  bool        zoom      = true;
  bool        bright    = true;  // auto exposure: forests and interiors are too dark to see the legs
  float          aspect     = 4.f/3.f; // of the captured frame
  float          uiScale    = 1;       // of the game's interface
  std::string zonesNpc;         // showzones: the npc whose hit zones the overlay draws
  bool        zonesOnly = false; // the overlay draws the hit zones only
  // overlay trail: the way each arrow flew, kept trailKeep ms after its last move; nothing else drawn
  using Trail = testbench_detail::Trail;
  bool        trailOn   = false;
  bool        trailLine = false;  // ...and a dashed straight line along the way it left: the bow's line
  uint64_t    trailKeep = 1500;
  std::vector<Trail> trails;
};

struct OverlayContext {
  CameraBench& camera;
  OverlayBench& overlay;
  Living& living;
  std::vector<Npc*>& crew;
  PlayerControl*& ctrl;
  Npc*& subject;
  uint64_t& time;
  std::string& label;
};

struct CameraContext {
  CameraBench& camera;
  OverlayBench& overlay;
  OverlayContext overlayContext;
  Water& water;
  RagdollBench& ragdoll;
  MonsterCharacter& monster;
  Character& character;
  Living& living;
  ArcheryBench& archery;
  Movables& movables;
  Routine& routine;
  std::vector<Npc*>& crew;
  Npc*& subject;
  PlayerControl*& ctrl;
  uint64_t& time;
  std::vector<std::string>& prog;
  size_t& pc;
  bool& hasPrev;
  std::vector<Tempest::Vec3>& anim1;
  std::vector<Tempest::Vec3>& anim2;
  bool& ordered;
  bool& bodyLive;
  const bool& ragdollPerf;
  const bool& realtime;
  Npc& (*subjectOf)(Npc&);
  void (*waterFilmFraming)(World&,uint32_t,uint32_t);
};

void updateCamera(World& w,Npc& pl);
void placeHero(World& w,Npc& pl);
bool frameRagdollArena(size_t expected=20);
void lookFrom(const CameraContext& ctx,const Tempest::Vec3& org,const Tempest::Vec3& tgt);
void updateCamera(const CameraContext& ctx,World& w,Npc& pl);
void placeHero(const CameraContext& ctx,World& w,Npc& pl);
bool frameRagdollArena(const CameraContext& ctx,size_t expected);
void capture(const CameraContext& ctx,Tempest::Pixmap& frame,float uiScale);
void stopRecording(const CameraContext& ctx);
void writeWavHeader(std::FILE* f,uint32_t rate,uint64_t frames,uint16_t channels=2);
bool cameraCommand(const CameraContext& ctx,const std::string& line,const std::vector<std::string>& a,
                   World& w,Npc& pl,PlayerControl& ctrl);
bool overlayCommand(const OverlayContext& ctx,const std::vector<std::string>& a);
void drawOverlay(const OverlayContext& ctx,Tempest::Pixmap& pm);
void drawZones(const OverlayContext& ctx,Canvas& cv,Camera* cam);
void drawArenaMark(Tempest::Pixmap& pm);
void writeVideo(const OverlayContext& ctx,const Tempest::Pixmap& pm);

struct ControlContext {
  uint64_t& time;
  uint64_t& waitUntil;
  std::chrono::steady_clock::time_point& realWaitUntil;
  bool& waitArrive;
  uint64_t& arriveEnd;
  std::string& label;
  bool& hasPrev;
  std::vector<Hold>& holds;
  std::vector<KeyCodec::Action>& taps;
  Healing& healing;
  uint64_t& inputTraceUntil;
  std::vector<Tempest::Vec3>& routePoints;
  size_t& routeNext;
  Tempest::Vec3& routePosition;
  float& routeSpeed;
  double& routeLength;
  uint64_t& routeStarted;
  bool& routeActive;
  bool& routeWasExternal;
  Npc& (*subjectOf)(Npc&);
};

struct DialogContext {
  uint64_t& time;
  Npc*& hearNpc;
  uint64_t& hearUntil;
  uint64_t& hearEvery;
  uint64_t& hearNext;
  Npc*& thinkNpc;
  uint64_t& thinkUntil;
  bool& speaking;
  uint64_t& speakUntil;
  uint64_t& heroEndAt;
  bool& waitReply;
  bool& replyNeed;
  uint64_t& replyFrom;
  uint64_t& replyEnd;
  uint64_t& replyQuiet;
  std::string& waitAiKey;
  std::string& waitAiStage;
  uint64_t& waitAiEnd;
  Npc& (*subjectOf)(Npc&);
};

struct NpcCommandsContext {
  NpcCommands& npcCommands;
  Healing& healing;
  std::vector<Hold>& holds;
  std::vector<KeyCodec::Action>& taps;
  Npc*& pinned;
  Tempest::Vec3& pinAt;
  uint32_t& scenarioSeed;
  uint32_t& lcg;
  uint32_t& monsterCharacterSeed;
  std::vector<Npc*>& crew;
  uint64_t& time;
  Melee& melee;
  ArcheryBench& archery;
  Npc*& subject;
  void (*stunDriftSample)(World&,uint64_t,uint64_t);
};

struct NpcTierTest {
  Npc* npc = nullptr;
  const WayPoint* ends[2] = {};
  unsigned legs = 0, turns = 0, phase = 0, switches = 0;
  unsigned toFull[2] = {}, toSimplified[2] = {};
  uint64_t deadline = 0, switchedAt = 0;
  Tempest::Vec3 before, started, lastPosition;
  float beforeAngle = 0, lastAngle = 0, maximumJump = 0, maximumHeight = 0;
  float travelPerMs = 0;
  const WayPoint* destination = nullptr;
  std::vector<const WayPoint*> path;
  NpcSimTier previous = NpcSimTier::Full;
  bool pending = false;
};

struct NpcSpeedTest {
  Npc* npc = nullptr;
  const WayPoint* start = nullptr;
  const WayPoint* end = nullptr;
  Tempest::Vec3 previous;
  uint64_t began = 0, previousTick = 0, measuredMs = 0;
  unsigned leg = 0, samples = 0;
  float length = 0, distance = 0, expectedDistance = 0;
  std::string firstAnimation, lastAnimation;
};

struct NpcDuel {
  Npc* actors[2] = {};
  std::string before[2];
  size_t weapons[2] = {size_t(-1),size_t(-1)};
  int hits[2] = {}, damage[2] = {};
  int swings[2] = {}, blocked[2] = {}, misses[2] = {};
  int64_t firstResponseMs = -1;
  std::string hitLog = "[";
  int loser = -1;
  uint32_t seed = 0;
  uint64_t from = 0, end = 0;
  bool active = false;

  void hit(Npc& victim, Npc& by, int32_t loss, std::string_view);
  };

std::string movableRows(World& world,bool allMap=false);
void movableBegin(World& world,std::string phase);
void movableFields(World& world);
void movableCheckpoint(World& world);
float noiseFacingError(const Npc& npc,const Tempest::Vec3& point);
uint32_t noiseNpcId(const Npc* npc);
void prepareNoiseFixture(World& world,Npc& player,bool tipping=false);
void noisePushTable(World& world,Npc* culprit=nullptr);
void movableCandleSetup(World& world,Npc& player);
void movableCandleFields(World& world);
void movableCandlePush(World& world);
void movableCandleCheckpoint(World& world);
void noiseTableCase(World& world,Npc& player,bool push=true);
void noiseDropItems(World& world,unsigned count);
void noiseItemCase(World& world,Npc& player,std::string_view phase);
void noiseCheckpoint(World& world);
void noiseFields(World& world);
void movableSaveSnapshot(World& world,std::string_view label);
void movablePin(World& world,Npc& player,std::string_view target);
void pushMovablePinnedTable(World& world);
bool noiseCommand(const MovablesContext& context,const std::vector<std::string>& a,World& w,Npc& pl,PlayerControl& ctrl);
bool movableCommand(const MovablesContext& context,const std::vector<std::string>& a,World& w,Npc& pl,PlayerControl& ctrl);
bool spellCommand(const MovablesContext& context,const std::vector<std::string>& a,World& w,Npc& pl,PlayerControl& ctrl);
bool physicsNoiseDebug(const MovablesContext& context,World& world,Npc& player,std::string_view command,std::string& value,std::string& error);
void physicsNoise(const MovablesContext& context,const PhysicsNoise& noise,std::string_view sound,bool played);
void physicsNoiseHeard(const MovablesContext& context,const Npc& npc,const PhysicsNoise& noise,bool callbackDispatched);
MovablesContext movablesContext();
bool chooseQuestSite(World& world,bool water,QuestSite& site,const std::function<bool(QuestSite&)>& accept={});

extern std::string outDir;

std::string rest(const std::string& line);
Npc* npcBySymbol(World& w,std::string_view symbol);
Marvin& marvin();
void press(PlayerControl& ctrl,KeyCodec::Action a);
bool aiCommand(const std::string& cmd,std::string& reply);
std::string nameUtf8(const Npc& n);
const char* gazeSource(Gaze::Source s);
bool controlCommand(const ControlContext& ctx,const std::string& line,const std::vector<std::string>& a,World& w,Npc& pl,PlayerControl& ctrl);
bool dialogCommand(const DialogContext& ctx,const std::string& line,const std::vector<std::string>& a,World& w,Npc& pl);

void say(const std::string& s);
Tempest::Vec3 facing(const Npc& pl);
void face(Npc& pl,const Tempest::Vec3& dir);
std::vector<Spot> findSpots(World& w,const Npc& pl,SpotKind kind,float a0,float a1,std::string_view part,size_t count);
bool aimSpotsCommand(bool& hasPrev,const std::string& line,const std::vector<std::string>& a,World& w,Npc& pl);
Npc* findNpc(World& w, Npc& pl, const std::string& name);
bool hidden(World& w,const Tempest::Vec3& from,const Tempest::Vec3& to);
float num(const std::vector<std::string>& a,size_t i,float def);
bool isNumber(const std::string& s);
void prepareSpellFixture(World& world,Npc& player,std::string_view fixture);
void destructionCheckpoint(World& world);

bool destructionTracesDebug(Destruction& state,const DestructionContext& context,World& world,Npc& player,std::string_view command,std::string& value,std::string& error);
bool destructionOwns(Destruction& state,uintptr_t id);
std::string destructionRows(Destruction& state,World& world);
std::string destructionFragments(Destruction& state,World& world);
void destructionFields(Destruction& state,const DestructionContext& context,World& world);
uintptr_t destructionAdd(Destruction& state,const DestructionContext& context,World& world,std::string_view name,const Tempest::Vec3& support);
bool destructionDropFloor(const DestructionContext& context,World& world,Tempest::Vec3& result);
void destructionSetup(Destruction& state,const DestructionContext& context,World& world,Npc& player,std::string_view name);
void destructionBegin(Destruction& state,const DestructionContext& context,World& world);
void destructionShoot(Destruction& state,World& world,Npc& player);
void destructionCheckpoint(Destruction& state,const DestructionContext& context,World& world);
std::string destructionFloorProbe(const Npc& npc,std::string_view label,const Tempest::Vec3& point);
void destructionSupportSample(Destruction& state,const DestructionContext& context,World& world);
bool destructionMeleeEquip(World& world,Npc& actor,std::string_view weapon);
bool destructionMeleeProbe(Destruction& state,World& world,Npc& actor,const DynamicWorld::MovableVobSnapshot& body,std::string_view stage);
void destructionMeleeSetup(Destruction& state,const DestructionContext& context,World& world,Npc& player,std::string_view kind,std::string_view weapon,bool npc);
void destructionMeleeTick(Destruction& state,const DestructionContext& context,World& world);
std::string destructionRoutineRows(const Npc& npc);
void destructionFarmSetup(Destruction& state,const DestructionContext& context,World& world,Npc& player,uint32_t zenId,bool npc=false);
void destructionFarmSetupNext(Destruction& state,const DestructionContext& context,World& world,Npc& player);
void destructionFarmNpcWalk(Destruction& state,World& world,std::string_view phase);
void destructionFarmCheckpoint(Destruction& state,World& world,Npc& player,std::string_view phase);
void destructionSessionSnapshot(World& world,std::string_view label);
void destructionAge(World& world,int64_t minutes,bool youngest);

bool destructionCommand(Destruction& state,const DestructionContext& context,const std::vector<std::string>& a,World& w,Npc& pl,PlayerControl& ctrl);

void destructionCamera(World& world,const Tempest::Vec3& source,const Tempest::Vec3& direction);
size_t arrowBone(const Skeleton& skeleton,std::string_view token);
bool noiseFloor(World& world,const Tempest::Vec3& source,float distance,bool wall,Tempest::Vec3& result,bool distant=false);
void prepareMovableCast(World& world,Npc& player);
void saveMovableSession(World& world,const std::string& label);

bool monsterCharacterCommand(MonsterCharacter& state,const MonsterCharacterContext& context,const std::vector<std::string>& a,World& w,Npc& pl);
bool characterCommand(Character& state,const CharacterContext& context,const std::vector<std::string>& a,World& w,Npc& pl,PlayerControl& ctrl);
bool placedItemsCommand(PlacedItems& state,const std::vector<std::string>& a,World& w,Npc& pl,PlayerControl& ctrl);
bool itemHullsCommand(ItemHulls& state,const std::vector<std::string>& a,World& w,Npc& pl);
bool fracturePiecesCommand(FracturePieces& state,Destruction& destruction,const DestructionContext& context,bool& cutPending,const std::vector<std::string>& a,World& w);
bool waterCommand(Water& waterState,const WaterContext& context,const std::vector<std::string>& a,World& w,Npc& pl,PlayerControl& ctrl);
bool furniturePartsCommand(FurnitureParts& state,const std::vector<std::string>& a,World& w,Npc& pl);
bool npcSimCommand(NpcDuel& duelState,NpcTierTest& tierState,NpcSpeedTest& speedState,size_t& pc,const std::vector<std::string>& prog,const std::vector<std::string>& a,World& w,Npc& pl);

const char* envStr(const char* name);
float groundY(World& w,float x,float z,float y);
const char* policyName(NpcProcessPolicy p);

BodyContext bodyContext();
void stopBodyLive();
void setupArena(World& w,Npc& pl);
bool waitingForBodyLive();
const char* animationModeName();
void animationControlError(const std::string& reason);
bool beginAnimationControl(uint32_t seed,const std::vector<Npc*>& actors);
bool beginAnimationControl(uint32_t seed,Npc& first,Npc& second);
unsigned animationActorIndex(const Npc& npc);
std::string animationActorFields(unsigned index);
BodyAnimationEntry* animationFixtureEntry(unsigned side,uint64_t time);
void observeAnimationAttempt(const Npc& npc,const std::string& fields,uint64_t gameTime);
void endAnimationControl();
void animationControlTotals();
bool bodyCommand(const BodyContext& context,const std::vector<std::string>& a,World& w,Npc& pl);

PhysicsTracesContext physicsTracesContext();
bool physicsTracesDebug(const PhysicsTracesContext& context,World& world,Npc& player,std::string_view command,std::string& value,std::string& error);

BodyImpactContext bodyImpactContext();
void bodyImpactTick(World& world);
bool bodyImpactCommand(BodyImpact& state,const BodyImpactContext& context,const std::vector<std::string>& a,World& w,Npc& pl);

void exec(const std::string& line, World& w, Npc& pl, PlayerControl& ctrl);

void lookFrom(const Tempest::Vec3& org,const Tempest::Vec3& tgt);
std::string ragdollNativePrecision(const Ragdoll::PartState& part);
std::string ragdollInventory(const Npc& npc,World& world,Inventory::IteratorType type=Inventory::T_Inventory);
std::string noiseListenerRows(World& world);
void noiseBegin(World& world,std::string_view phase,const Tempest::Vec3& source);
Npc* noiseAddListener(World& world,std::string role,const Tempest::Vec3& point,Npc* npc=nullptr);
void noiseRetireListeners();
uintptr_t destructionAdd(World& world,std::string_view name,const Tempest::Vec3& support);

extern const bool npcCharacterTrace;
extern const char* characterMoveFrameColumns;

Tempest::Vec3 rotY(const Tempest::Vec3& v,float deg);
bool chooseRagdollArena(World& world,Npc& player);
bool slopeAt(World& w, const Tempest::Vec3& p, float& angle, Tempest::Vec3& down, float& rough);
void prepareMovableFixture(World& world,Npc& player);

void monsterCharacterError(std::string_view reason);
Npc* monsterCharacterActor(World& world,std::string_view instance,const Tempest::Vec3& at);
void monsterCharacterClear(MonsterCharacter& state,const MonsterCharacterContext& context,World& world);
void monsterCharacterLedge(MonsterCharacter& state,const MonsterCharacterContext& context,World& world,Npc& player,std::string_view species,float height,float bottom=0.f);
void monsterCharacterPack(MonsterCharacter& state,const MonsterCharacterContext& context,World& world,Npc& player);
void monsterCharacterCliff(MonsterCharacter& state,const MonsterCharacterContext& context,World& world,Npc& player,bool higher=false);
void monsterCharacterCrowd(MonsterCharacter& state,const MonsterCharacterContext& context,World& world,Npc& player);
void monsterCharacterPrepare(MonsterCharacter& state,const MonsterCharacterContext& context,World& world,Npc& player,const std::string& name);
void monsterCharacterBegin(MonsterCharacter& state,World& world);
void monsterCharacterEnd(MonsterCharacter& state,const MonsterCharacterContext& context,World& world);
void monsterCharacterAttack(MonsterCharacter& state,const MonsterCharacterContext& context,World& world,Npc& player,const std::string& species);
void monsterCharacterSnapshot(World& world,Npc& player,std::string_view label);
void monsterCharacterSave(MonsterCharacter& state,World& world,Npc& player);
void monsterCharacterFields(MonsterCharacter& state,const MonsterCharacterContext& context,World& world,uint64_t dt);

void npcCharacterStopMotionTrace(Character& state,World* world);
bool characterFloor(World& world,const Tempest::Vec3& at,Tempest::Vec3& floor,float depth=600.f);
bool characterLane(World& world,const Tempest::Vec3& start,const Tempest::Vec3& end,float maximumStep=35.f);
bool characterStairsProfile(World& world,const Tempest::Vec3& low,const Tempest::Vec3& high,unsigned& risers,float& maximumRiser,
                            float minimumRiser=6.f,float minimumRise=45.f);
void characterFreezeNear(World& world,Npc& player,const Tempest::Vec3& start,const Tempest::Vec3& goal);
void characterIdleGuards(World& world,const Npc& player);
bool prepareCharacterRoute(Character& state,const CharacterContext& context,World& world,Npc& player,const std::string& name,bool abortMissing=true,std::string_view onlyWaypoint={});
void characterSnapshot(World& world,Npc& player,std::string_view operation);
void characterRouteEnd(Character& state,World& world,Npc& player);
Tempest::Matrix4x4 characterSupportMatrix(Character& state,World& world);
void characterRideSample(Character& state,World& world,const Npc& rider);
void characterFields(Character& state,const CharacterContext& context,World& world,Npc& player,uint64_t dt,PlayerControl& ctrl);
void characterPropCheckpoint(Character& state,World& world,Npc& player);
void prepareCharacterProp(Character& state,const CharacterContext& context,World& world,Npc& player,const std::string& role);
void characterRideCheckpoint(Character& state,World& world,Npc& player,bool setup=false);
std::string npcCharacterBodyJson(const CharacterVirtualDiagnostics::Body& body,
                                const std::vector<DynamicWorld::MovableVobSnapshot>& movable);
std::string npcCharacterPoseJson(const CharacterVirtualDiagnostics::Pose& pose,
                                const std::vector<DynamicWorld::MovableVobSnapshot>& movable);
std::string npcCharacterCollisionJson(World& world,const CharacterVirtualDiagnostics::NpcCollision& collision);
std::string npcCharacterMoveJson(World& world,const CharacterVirtualDiagnostics::NpcMove& move);
CharacterMoveFrame characterMoveFrame(const Npc& npc);
bool npcCharacterNeedsMovableLookup(const CharacterVirtualDiagnostics::Pose& pose);
std::string npcCharacterSample(World& world,const Npc& npc,float physicalJump,float visualJump,uint64_t frame,uint64_t dt);
void npcCharacterSupportTrace(Character& state,const CharacterContext& context,World& world,const Npc& npc,float physicalJump,float visualJump);
void npcCharacterMotionTrace(Character& state,const CharacterContext& context,World& world,uint64_t dt);
void npcCharacterSnapshot(World& world,Npc& player,std::string_view label);
Npc* npcCharacterActor(World& world,const Tempest::Vec3& position);
void npcCharacterPlaceHero(World& world,Npc& player,const Tempest::Vec3& start,const Tempest::Vec3& goal);
void characterIdleSlopeSample(Character& state,const CharacterContext& context,World& world,Npc& player,std::string_view label);
void characterIdleSlopePrepare(Character& state,const CharacterContext& context,World& world,Npc& player,float requestedAngle,bool npc);
void characterIdlePrepare(Character& state,const CharacterContext& context,World& world,Npc& player,PlayerControl& ctrl,std::string_view role,std::string_view site);
void characterIdleWeapon(Character& state,World& world,std::string_view kind);
void characterIdleSnapshot(Character& state,const CharacterContext& context,World& world,std::string_view label,bool begin=false);
void npcCharacterPrepare(Character& state,const CharacterContext& context,World& world,Npc& player,const std::string& name);
void npcCharacterBegin(Character& state,World& world,std::string_view phase={});
void npcCharacterEnd(Character& state,World& world);
void characterStairPerformance(World& world,Npc& player);
void npcCharacterMarket(Character& state,const CharacterContext& context,World& world,Npc& player);
void npcCharacterFields(Character& state,const CharacterContext& context,World& world,Npc& player,uint64_t dt);
bool prepareCharacterRideSupport(Character& state,World& world,Npc& player,bool kinematic,bool rotating,Tempest::Vec3& top);
void writeCharacterMoveFrame(std::FILE* file,const CharacterMoveFrame& move);

void clothFields(ClothContext ctx,World& world);
void clothInitial(ClothContext ctx,World& world,std::string_view label);
bool clothCommand(ClothContext ctx,const std::vector<std::string>& a,World& w,Npc& pl);
void waterFilmTick(WaterFilmContext ctx,World& world);
void waterFilmFraming(WaterFilmContext ctx,World& world,uint32_t width,uint32_t height);
bool waterFilmCommand(WaterFilmContext ctx,const std::vector<std::string>& a,World& w,Npc& pl);

bool waterOwns(Water& state,const DynamicWorld::WaterBodySnapshot& body);
void waterFields(Water& state,const WaterContext& context,World& world);
std::string waterRows(Water& state,World& world);
void waterCheckpoint(Water& state,const WaterContext& context,World& world,std::string_view label);
bool waterProbe(World& world,const Tempest::Vec3& at,float minDepth,float maxDepth,Tempest::Vec3& surface);
bool waterFireballCaster(World& world,const Tempest::Vec3& target,float targetRadius,Tempest::Vec3& caster);
bool waterFindWindSite(Water& state,World& world,std::vector<Tempest::Vec3>& places);
void waterTorch(Water& state,World& world,Npc& player);
void prepareWaterFixture(Water& state,const WaterContext& context,World& world,Npc& player,std::string_view mode);
void waterInitial(Water& state,const WaterContext& context,World& world,Npc& player,PlayerControl& ctrl,std::string_view label);
std::string waterActorRows(Water& state,World& world);
void waterBodiesCheckpoint(Water& state,const WaterContext& context,World& world,std::string_view label);
bool waterBodiesAdd(Water& state,const WaterContext& context,World& world,Npc& player,std::string_view symbol,std::string role,const Tempest::Vec3& at,bool dead=true);
void prepareWaterBodiesFixture(Water& state,const WaterContext& context,World& world,Npc& player,std::string_view mode);
void waterBodiesInitial(Water& state,const WaterContext& context,World& world,Npc& player,std::string_view label);
void waterBodiesLootReady(Water& state,World& world,Npc& player,std::string_view mode);
void waterBodiesLoot(Water& state,const WaterContext& context,World& world,Npc& player,PlayerControl& ctrl);
void prepareWaterSpell(Water& state,const WaterContext& context,World& world,Npc& player,bool wind);

void healingObserve(Healing& state,World& world,Npc& player);
void healingCommand(Healing& state,const std::vector<std::string>& a,World& world,Npc& player,PlayerControl& ctrl,
                    const std::vector<Npc*>& meleeShadowActors);

bool dodgeDirection(std::string_view name,MeleeDodge::Direction& dir);
void meleeDodgeFields(World& world);
void meleeAiResponseFields(World& world);
void meleePhysicsCamera();
bool meleeCommand(const MeleeContext& context,const std::vector<std::string>& a,World& w,Npc& pl,PlayerControl& ctrl);
MeleeContext meleeContext();

const char* questReason(uint8_t hazards);
void questFields(World& world,Npc& npc,bool csv);
void routineReturnTick(World& world,Npc& player,PlayerControl& ctrl);
bool routineCommand(const RoutineContext& context,const std::vector<std::string>& a,World& w,Npc& pl,PlayerControl& ctrl);
RoutineContext routineContext();

void ragdollSessionArchive(const std::vector<std::string>& args, World& world, Npc& player, uint64_t initialDt=0);
void writeRagdoll(Npc& player);
bool arenaProjected(const Tempest::Vec3& point);
bool arenaMeshVisible(World& world, const Npc& npc);
RagdollBench::PerfSample ragdollCoverageSample(World& world);
void sampleRagdollPerformance(World& world);
bool arenaModeMatches(const RagdollBench::PerfSample& sample, std::string_view mode);
void arenaModelProof(World& world, const Npc& npc, size_t frame);
bool arenaFullSchema(World& world, std::string_view mode);
bool ragdollCommand(const RagdollContext& context,const std::string& line,const std::vector<std::string>& a,World& w,Npc& pl,PlayerControl& ctrl);
RagdollContext ragdollContext();
void ownMeshFields(World& world,Npc& player);
void ownMeshChestFields(World& world,const Npc& player);
bool ownMeshCommand(const OwnMeshContext& context,const std::vector<std::string>& a,World& w,Npc& pl,PlayerControl& ctrl);
OwnMeshContext ownMeshContext();

void event(std::string_view kind, const std::string& fields = {});
std::string jsonStr(std::string_view s);
std::string symName(World& w, const Npc* n);
std::string stateName(World& w, const Npc& n);
std::string ragdollNumber(float value);
std::string ragdollVector(const Tempest::Vec3& value);
std::string ragdollMatrix(const Tempest::Matrix4x4& value);
void arenaAbort(std::string_view reason);
Tempest::Vec3 movablePosition(const Tempest::Matrix4x4& matrix);
std::optional<DynamicWorld::MovableVobSnapshot> furnitureBody(World& world,uint32_t zenId);
void release(PlayerControl& ctrl, KeyCodec::Action a);
bool destructionSupported(World& world,std::string_view visual,const Tempest::Vec3& at,Tempest::Vec3& support,Tempest::Vec3& failed);

std::string npcDuelSnapshot(World& world, Npc& npc);
bool tickNpcDuel(NpcDuel& state,World& world);
bool npcTierTestTick(NpcTierTest& state,World& world,Npc& player,uint64_t frameMs);
bool npcSpeedPrepare(NpcSpeedTest& state,World& world);
bool npcSpeedTick(NpcSpeedTest& state,World& world);

uint64_t furniturePeakMemory();
void furnitureClearDrops(FurnitureParts& state,World& world);
void furnitureTraceBegin(FurnitureParts& state,World& world);
void furnitureTraceReport(FurnitureParts& state,World& world);
void furnitureRemoveTable(FurnitureParts& state,World& world);
void furnitureSetup(FurnitureParts& state,World& world,Npc& player);
void furnitureTableItems(FurnitureParts& state,World& world,Npc& player);
void furnitureCupboardItem(FurnitureParts& state,World& world,Npc& player);
void furnitureItemReport(FurnitureParts& state,World& world);
void furnitureRagdoll(FurnitureParts& state,World& world,Npc& player);
void furnitureRagdollReport(FurnitureParts& state,World& world);
void furnitureWalk(FurnitureParts& state,World& world,Npc& player,bool npc=false);
void furnitureWalkReport(FurnitureParts& state,World& world,Npc& player);
void furniturePerformance(FurnitureParts& state,World& world,Npc& player);
void furniturePerformanceSample(FurnitureParts& state,World& world,uint64_t dt);
void furniturePerformanceReport(FurnitureParts& state,World& world);

void itemHullCensus(World& world);
void itemHullClear(ItemHulls& state,World& world);
void itemHullSetup(ItemHulls& state,World& world,Npc& player,std::string_view surface);
void itemHullSample(ItemHulls& state,World& world);
void itemHullReport(ItemHulls& state,World& world);

void fracturePiecesBreak(FracturePieces& state,World& world,uintptr_t parent,std::string_view label);
void fracturePiecesTick(FracturePieces& state,Destruction& destruction,const DestructionContext& context,World& world);
std::optional<std::pair<Tempest::Vec3,Tempest::Vec3>> fracturePiecesView(World& world,const std::vector<uintptr_t>& ids,
                                                                         const Tempest::Vec3& camera,const Tempest::Vec3& target);
// `destruction2 stand`: the actor back where it stood before the blow, facing the same way
// (the close "pieces" frame after the take: the lunge leaves the hero inside the prop).
void fracturePiecesStand(Npc* actor,const Tempest::Vec3& stand,const Tempest::Vec3& facing);

void placedItemsCensus(World& world);
void placedItemsProbe(PlacedItems& state,World& world,Npc& player);
void placedItemsDiagnose(PlacedItems& state,World& world,Npc& player,uint32_t zenId,bool control);
void placedItemsFields(PlacedItems& state,World& world,Npc& player,PlayerControl& ctrl,std::vector<Hold>& holds);
void placedItemsPerformance(PlacedItems& state,World& world,Npc& player);
void placedItemsPerformanceReport(PlacedItems& state,World& world);
void placedItemsFloor(PlacedItems& state,World& world,Npc& player);
void placedItemsWalk(PlacedItems& state,World& world,Npc& player);
void placedItemsWalkReport(PlacedItems& state,World& world,Npc& player);
void placedItemsArrowReady(PlacedItems& state,World& world,Npc& player);
void placedItemsArrow(PlacedItems& state,World& world,Npc& player);
void placedItemsArrowReport(PlacedItems& state,World& world);
void placedItemsTable(PlacedItems& state,World& world,Npc& player,bool owned=false);
void placedItemsTablePush(PlacedItems& state,World& world,Npc& player);
void placedItemsTableReport(PlacedItems& state,World& world);
void placedItemsSnapshot(World& world,std::string_view label,uint64_t initialDt=0);
void placedItemsSave(World& world);
void placedItemsPickupReady(PlacedItems& state,World& world,Npc& player);
void placedItemsPickup(PlacedItems& state,World& world,Npc& player,PlayerControl& ctrl);
void placedItemsPickupReport(PlacedItems& state,World& world,Npc& player);

bool itemWakeTableItems(World& world,Npc& player,bool sword);
void itemWakeFields(World& world);
void itemWakeResetWindow(World& world,uint64_t window);
void itemWakeSetup(World& world,Npc& player,std::string_view name);
bool itemWakeBreakCrate(World& world,Npc& player);
void itemWakeBegin(World& world,Npc& player);
void itemWakeEventDebris(World& world,Npc& player);
void itemWakeCheckpoint(World& world);
bool itemWakeCommand(const std::vector<std::string>& a,World& w,Npc& pl);
ItemWakeContext itemWakeContext();

const char* leanLineName(Aim::LeanLine line);
bool aimPoint(Npc& n, HitZones::Zone z, bool leg, Tempest::Vec3& out);
void arrowShot(World& w, Npc& pl, Tempest::Vec3& org, Tempest::Vec3& tgt);
float manualAimOff(Npc& pl);
const char* archerStage(Archer::Stage s);
bool arrowAimPoint(Npc& npc, Tempest::Vec3& point);
size_t npcArrowPins(World& world);
void stickingFields(World& world);
void arrowFields(World& world);
float aimSegmentDistance(const Tempest::Vec3& p, const Tempest::Vec3& q, const Tempest::Vec3& a, const Tempest::Vec3& b, float& along);
float aimBoxDistance(const Tempest::Vec3& from, const Tempest::Vec3& to, const Tempest::Vec3 (&box)[2]);
std::vector<std::string> split(std::string_view s);
WorldContext worldContext();
bool worldCommand(const WorldContext& context,const std::string& line,const std::vector<std::string>& a,
                  World& w,Npc& pl,PlayerControl& ctrl);
NpcCommandsContext npcCommandsContext();
bool npcCommandsCommand(const NpcCommandsContext& context,const std::string& line,const std::vector<std::string>& a,
                        World& w,Npc& pl,PlayerControl& ctrl);

ArcheryContext archeryContext();
bool archeryCommand(const ArcheryContext& context,const std::string& line,const std::vector<std::string>& a,
                    World& w,Npc& pl,PlayerControl& ctrl);

LivingCounts livingCounts();
std::string livingCountFields(const LivingCounts& s, World& world);
void livingFields(World& world);
void livingSeatFields(World& world);
bool livingCommand(const LivingContext& context,const std::vector<std::string>& a,World& w,Npc& pl,PlayerControl& ctrl);
LivingContext livingContext();

}
