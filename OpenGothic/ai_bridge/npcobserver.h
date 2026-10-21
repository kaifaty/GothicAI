#pragma once

#include <rapidjson/document.h>
#include <Tempest/Vec>

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "game/constants.h"
#include "game/gamescript.h"
#include "npcsteal.h"

class Npc;
class World;
struct PhysicsNoise;
struct HeroDeed;

// NPCs that follow their routine, as seen by the AI server's planner (protocol v5):
// - npc_roster: npc's around the player, twice a second;
// - npc_event:  what they notice about the player, and threats from others: monsters near or attacking,
//               armed npc's (from the engine's own perceptions, which still run); a monster killed
//               (`monster_killed`: the danger is over);
// - npc_act:    "soft" actions put into the npc's AI queue the way scripts do it (AI_OutputSVM,
//               AI_TurnToNpc, ...): when the queue runs empty, the engine resumes the routine itself;
// - npc_idle:   such an action is done.
// Npc's under NpcController or in a conversation are reported but never acted upon.
class NpcObserver final {
  public:
    using Emit = std::function<void(std::string&&)>;
    // "attached" / "dialog" for npc's somebody else drives, empty otherwise
    using Owner = std::function<std::string_view(std::string_view symbol)>;

    NpcObserver(Emit emit, Owner owner);

    void tick(World& world);
    void perception(World& world, Npc& self, Npc& other, Npc* victim, PercType perc);
    void physicsNoise(World& world, Npc& self, const PhysicsNoise& noise);
    void act(World& world, const rapidjson::Value& msg);
    // threats, hero deeds and room entry are held from free npc's scripts for the server;
    // it answers with a verdict, or the scripts get them after HoldMs
    bool intercept(World& world, Npc& self, Npc& other, Npc* victim, PercType perc,
                   const HeroDeed* deed = nullptr, std::string_view deedId = {});
    void verdict(World& world, const rapidjson::Value& msg);
    void setHoldThreats(bool on) { holdThreats = on; }
    void setHoldDeeds(bool on) { holdDeeds = on; }
    void setHoldBandits(World* world, bool on);
    // the server is gone: every held threat goes to the scripts now
    void releaseAll(World& world);
    // `routine`: an npc's daily routine, its routine variants and current state (v6)
    void routine(World& world, const rapidjson::Value& msg);
    void clear();
    void cancelDeeds(World& world);
    // A running soft act already owns movement/queues; a body lease must not overlap it.
    bool isActing(std::string_view symbol) const;

  private:
    struct Running final {
      std::string id;
      std::string symbol;
      size_t      instance = 0;
      uint64_t    start    = 0;
      ScriptFn    state;            // fight/flee: done when the npc leaves this script state
      bool        entered  = false;
      uint64_t    maxMs    = 0;
      // give (v15): the item changes hands once the walk and the turn are done
      size_t      item     = 0;
      size_t      count    = 0;
      size_t      giveTo   = size_t(-1);  // the receiver's instance
      // start_activity with `arrive` (v15): "arrived" once it does the activity within `radius` of `at`
      bool          arrival = false;
      Tempest::Vec3 at;
      float         radius  = 0;
      bool          investigate = false;
      NpcSteal::Active theft;
      bool          deed = false;
      size_t        fightTarget = size_t(-1);
      bool          nonlethal = false;
      };

    void emitRoster(World& world);
    void armedApproach(World& world, Npc& self, std::string_view symbol);
    // true when the player just came close: the caller emits player_near after the roster
    bool trackProximity(World& world, Npc& npc, std::string_view symbol, float dist);
    void checkRunning(World& world, uint64_t now);
    void emitEvent(std::string_view npc, std::string_view kind, std::string_view other,
                   double distance, std::string_view detail, std::string_view otherName,
                   std::string_view victim = {});
    bool throttled(std::string_view npc, std::string_view kind, uint64_t now, uint64_t periodMs);
    void reply(std::string_view id, std::string_view npc, std::string_view status, std::string_view detail,
               std::string_view action = {});

    Emit                                  emit;
    Owner                                 owner;
    uint64_t                              nextRoster = 0;
    std::unordered_map<std::string,bool>  close;      // npc -> the player is within reach
    std::unordered_map<std::string,uint64_t> lastEvent;  // "npc/kind" -> time
    std::unordered_map<const Npc*,Tempest::Vec3> previousPositions;
    std::vector<Running>                  running;

    struct Held final {
      std::string id;
      std::string key;     // actor pair, with separate deed/room keys for the hero
      std::string item;    // restore the theft's ITEM before a late script delivery
      std::string deed;    // hero_deed id: rules may consume it without taking the body
      bool        room   = false;  // entry rules may warn or grant time to leave without taking the body
      bool        bandit = false;  // sight of a hostile bandit or its prospective party target
      const Npc*  self   = nullptr;
      const Npc*  other  = nullptr;
      const Npc*  victim = nullptr;
      PercType    perc   = PERC_None;
      uint64_t    at     = 0;
      };
    enum class HoldTo : uint8_t { Scripts, Server, Parley };
    struct Verdict final {
      HoldTo   to     = HoldTo::Scripts;
      uint64_t until  = 0;
      bool     bandit = false;
      };
    void releaseHeld(World& world, Held& h);

    bool                                     holdThreats = false;
    bool                                     holdDeeds = false;
    bool                                     holdBandits = false;
    uint64_t                                 nextHold    = 1;
    std::vector<Held>                        held;
    std::unordered_map<std::string,Verdict>  verdicts;
  };
