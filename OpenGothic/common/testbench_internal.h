#pragma once

#include <Tempest/Matrix4x4>
#include <Tempest/Vec>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "game/constants.h"
#include "physics/dynamicworld.h"
#include "physics/physics.h"
#include "utils/keycodec.h"
#include "world/waypoint.h"

class Npc;
class PlayerControl;
class World;
struct HeroDeed;
enum PercType : uint8_t;

namespace testbench_detail {

struct Hold {
  KeyCodec::Action a;
  uint64_t         until;
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
