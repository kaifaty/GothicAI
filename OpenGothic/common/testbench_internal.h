#pragma once

#include <Tempest/Matrix4x4>
#include <Tempest/Vec>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <deque>
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

#include "resources.h"
#include "game/constants.h"
#include "graphics/objvisual.h"
#include "game/inventory.h"
#include "physics/charactervirtualdiagnostics.h"
#include "physics/dynamicworld.h"
#include "physics/physics.h"
#include "physics/physicmesh.h"
#include "physics/physicmeshshape.h"
#include "physics/ragdoll.h"
#include "utils/keycodec.h"
#include "world/waypoint.h"

class Interactive;
class MoveTrigger;
class Npc;
class PlayerControl;
class Skeleton;
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

struct QuestSite { bool ready=false; Tempest::Vec3 dry={}, direction={}; float waterY=0; std::string waypoint; };
struct SpellActor {Npc* npc=nullptr;std::string role;};
struct MovableFixture { ObjVisual visual; DynamicWorld::Item body; };

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
};

struct LivingWalker { Npc* npc=nullptr; WayPoint ends[2]; bool next=false,queued=false; int meleePhase=-1;uint64_t meleeAttackAt=0; };

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

extern std::string outDir;

void say(const std::string& s);
Tempest::Vec3 facing(const Npc& pl);
Npc* findNpc(World& w, Npc& pl, const std::string& name);
bool hidden(World& w,const Tempest::Vec3& from,const Tempest::Vec3& to);
float num(const std::vector<std::string>& a,size_t i,float def);
void prepareSpellFixture(World& world,Npc& player,std::string_view fixture);
void destructionCheckpoint(World& world);

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
bool fracturePiecesCommand(const std::vector<uintptr_t>& ids,Npc* actor,const Tempest::Vec3& stand,const Tempest::Vec3& direction,CamMode& cam,bool& cutPending,const std::vector<std::string>& a,World& w);
bool waterCommand(Water& waterState,const WaterContext& context,const std::vector<std::string>& a,World& w,Npc& pl,PlayerControl& ctrl);
bool furniturePartsCommand(FurnitureParts& state,const std::vector<std::string>& a,World& w,Npc& pl);
bool npcSimCommand(NpcDuel& duelState,NpcTierTest& tierState,NpcSpeedTest& speedState,size_t& pc,const std::vector<std::string>& prog,const std::vector<std::string>& a,World& w,Npc& pl);

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
void fracturePiecesTick(FracturePieces& state,World& world);
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

}
