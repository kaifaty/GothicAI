#pragma once

#include <Tempest/Matrix4x4>
#include <Tempest/Vec>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "physics/dynamicworld.h"
#include "utils/keycodec.h"

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

extern std::string outDir;

void event(std::string_view kind, const std::string& fields = {});
std::string jsonStr(std::string_view s);
std::string ragdollNumber(float value);
std::string ragdollVector(const Tempest::Vec3& value);
std::string ragdollMatrix(const Tempest::Matrix4x4& value);
void arenaAbort(std::string_view reason);
Tempest::Vec3 movablePosition(const Tempest::Matrix4x4& matrix);
std::optional<DynamicWorld::MovableVobSnapshot> furnitureBody(World& world,uint32_t zenId);
void release(PlayerControl& ctrl, KeyCodec::Action a);

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
