#include "npcactivityhost.h"

#include "world/objects/npc.h"
#include "world/world.h"

#include <cmath>
#include <limits>

void NpcEngineCalls::setWalkMode(Npc& npc, int32_t modeBits) {
  const int32_t weaponBit = 0x80;
  const int32_t mode = modeBits & (~weaponBit);
  if(mode>=0 && mode<=3) //TODO: weapon flags
    npc.aiPush(AiQueue::aiSetWalkMode(WalkBit(mode)));
  }

std::string NpcEngineCalls::nearestWaypoint(World& world, const Npc& npc) {
  auto wp = world.findWayPoint(npc.position());
  if(wp)
    return wp->name;
  return "";
  }

int32_t NpcEngineCalls::distanceToWaypoint(World& world, const Npc& npc, std::string_view name) {
  //NOTE: in CoM, some way-point and free-points share same name - need to be precise
  auto* wp = world.findWayPoint(name);
  if(wp==nullptr)
    return std::numeric_limits<int32_t>::max();
  float ret = std::sqrt(npc.qDistTo(wp));
  if(ret<float(std::numeric_limits<int32_t>::max()))
    return int32_t(ret);
  return std::numeric_limits<int32_t>::max();
  }

void NpcEngineCalls::gotoWaypoint(World& world, Npc& npc, std::string_view name) {
  auto to = world.findWayPoint(npc.position(), name);
  if(to!=nullptr) {
    npc.aiPush(AiQueue::aiGoToPoint(*to));
    return;
    }
  // in vanilla 'ai_gotowp' sometimes is used incorrectly, so we need to check all other points
  to = world.findPoint(name, false);
  if(to!=nullptr)
    npc.aiPush(AiQueue::aiGoToPoint(*to));
  }

NpcActivityHost::NpcActivityHost(World& world, Npc& npc, Helper helper)
  :world(world), npc(npc), helper(std::move(helper)) {
  }

void NpcActivityHost::deferred(std::string_view name) {
  helper(name);
  }

const void* NpcActivityHost::lookAtTarget() {
  return npc.lookAtTarget();
  }

bool NpcActivityHost::isValidNpc(const void* other) {
  return other!=nullptr;
  }

void NpcActivityHost::stopLookAt() {
  npc.aiPush(AiQueue::aiStopLookAt());
  }

void NpcActivityHost::removeWeapon() {
  npc.aiPush(AiQueue::aiRemoveWeapon());
  }

void NpcActivityHost::setWalkMode(int32_t mode) {
  NpcEngineCalls::setWalkMode(npc,mode);
  }

std::string NpcActivityHost::waypoint() {
  return npc.handle().wp;
  }

std::string NpcActivityHost::nearestWaypoint() {
  return NpcEngineCalls::nearestWaypoint(world,npc);
  }

bool NpcActivityHost::stringsEqual(std::string_view a, std::string_view b) {
  return a==b; // Hlp_StrCmp is an exact comparison
  }

int32_t NpcActivityHost::distanceToWaypoint(std::string_view name) {
  return NpcEngineCalls::distanceToWaypoint(world,npc,name);
  }

void NpcActivityHost::gotoWaypoint(std::string_view name) {
  NpcEngineCalls::gotoWaypoint(world,npc,name);
  }

bool NpcActivityHost::isMobAvailable(std::string_view scheme) {
  return world.availableMob(npc,scheme)!=nullptr;
  }

int32_t NpcActivityHost::bodyState() {
  return int32_t(npc.bodyState());
  }

void NpcActivityHost::useMob(std::string_view scheme, int32_t state) {
  npc.aiPush(AiQueue::aiUseMob(scheme,state));
  }
