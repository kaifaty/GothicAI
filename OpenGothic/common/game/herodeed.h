#pragma once

#include <Tempest/Vec>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

class Npc;
class Item;
class Interactive;

// A snapshot at the source of an action. Hero actions retain their native
// perception delivery; NPC actions are admitted only by an explicit server mark.
struct HeroDeed final {
  struct Person final {
    std::string npc;
    int32_t guild = -1;
    int32_t npctype = -1;
    };
  struct InventoryItem final {
    std::string item;
    size_t count = 0;
    };

  static std::shared_ptr<HeroDeed> capture(Npc& hero, std::string_view kind, Npc* victim = nullptr,
                                         const Item* item = nullptr, Interactive* chest = nullptr,
                                         size_t count = 0);
  // Read-only preflight uses precisely the same ownership snapshot as capture.
  // ownershipPosition selects a remote target's room before the actor walks there;
  // position/time/waypoint still describe the actor, and the hero keeps its portal.
  static std::shared_ptr<HeroDeed> snapshot(Npc& actor, std::string_view kind, Npc* victim = nullptr,
                                          const Item* item = nullptr, Interactive* chest = nullptr,
                                          size_t count = 0, const Tempest::Vec3* ownershipPosition = nullptr);
  static std::vector<InventoryItem> inventory(Npc& npc);
  static int32_t aivar(Npc& npc, std::string_view name);
  static int32_t constant(Npc& npc, std::string_view name, int32_t fallback = -1);
  void witnessed(Npc& npc);
  void collectWitnesses(Npc& culprit, Npc* victim);

  // Assigned at capture, before any witness may hold the perception. The bridge
  // prefixes it with the save branch for both hero_deed and perception_held.
  uint64_t id = 0;
  std::string kind;
  Person culprit;
  std::string item;
  size_t count = 0;
  Person owner;
  int32_t ownerGuild = -1;
  bool itemDropped = false;
  std::string room;
  Person roomOwner;
  int32_t roomGuild = -1;
  std::string chest;
  Person chestOwner;
  int32_t chestGuild = -1;
  Person victim;
  bool victimIsMonster = false;
  bool victimIsSheep = false;
  bool victimInFight = false;
  bool victimEnemy = false;
  bool victimStartedFight = false;
  bool victimToughGuy = false;
  int32_t heroGuild = -1;
  Tempest::Vec3 position;
  std::string waypoint;
  int64_t day = 0;
  int64_t hour = 0;
  int64_t minute = 0;
  std::vector<Person> witnesses;
  std::vector<InventoryItem> inventoryBefore;
  std::vector<InventoryItem> inventoryAfter;
  int32_t pickedPocketBefore = 0;
  int32_t pickedPocketAfter = 0;
  bool caught = false;
  };
