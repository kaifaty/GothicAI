#pragma once

#include "asset/activitydefinitions.h"

#include <functional>
#include <string>
#include <string_view>

class Npc;
class WayPoint;
class World;

// Engine calls of a routine activity, shared by the script externals of the same name and by
// the record path (D4.1), so both paths give the engine the same calls.
namespace NpcEngineCalls {
void        setWalkMode(Npc& npc, int32_t modeBits);                                   // AI_SetWalkMode
std::string nearestWaypoint(World& world, const Npc& npc);                             // Npc_GetNearestWP
int32_t     distanceToWaypoint(World& world, const Npc& npc, std::string_view name);   // Npc_GetDistToWP
int32_t     distanceToWaypoint(const Npc& npc, const WayPoint* point);
int32_t     heightDifference(const Npc& npc, const Npc& other);                     // Npc_GetHeightToNpc
const WayPoint* resolveWaypoint(World& world, const Npc& npc, std::string_view name);  // AI_GotoWP target
void        gotoWaypoint(World& world, Npc& npc, std::string_view name);               // AI_GotoWP
void        equipItem(Npc& npc, uint32_t item);                                        // EquipItem
}

// The engine of a routine activity executed from its record: SELF is the NPC. The perception
// helper stays a script function; the Gothic layer runs it through `helper`.
class NpcActivityHost final : public ActivityDefinitions::Host {
  public:
    using Helper = std::function<void(std::string_view)>;
    using ItemCreator = std::function<void(int32_t)>;
    NpcActivityHost(World& world, Npc& npc, Helper helper, ItemCreator itemCreator = {});

    void        deferred(std::string_view helper) override;
    const void* lookAtTarget() override;
    bool        isValidNpc(const void* npc) override;
    void        stopLookAt() override;
    void        removeWeapon() override;
    void        setWalkMode(int32_t mode) override;
    std::string waypoint() override;
    std::string nearestWaypoint() override;
    bool        stringsEqual(std::string_view a, std::string_view b) override;
    int32_t     distanceToWaypoint(std::string_view waypoint) override;
    void        gotoWaypoint(std::string_view waypoint) override;
    bool        isMobAvailable(std::string_view scheme) override;
    int32_t     bodyState() override;
    void        useMob(std::string_view scheme, int32_t state) override;
    int32_t     itemCount(int32_t item) override;
    void        createItem(int32_t item) override;
    void        equipItem(int32_t item) override;
    void        useItem(int32_t item) override;
    void        removeItem(int32_t item) override;

  private:
    World& world;
    Npc&   npc;
    Helper helper;
    ItemCreator itemCreator;
  };
